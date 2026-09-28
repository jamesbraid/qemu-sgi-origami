/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Bedrock hub lock under multi-threaded TCG, exercised by both processors.
 * The test checks mutual exclusion and blocked-contender wakeup.
 */

/*
 * LB scratch 3's read-set-if-zero alias is the node's test-and-set lock. The
 * IP35 PROM guards its MD performance-counter-0 software lock word with it;
 * after 10,000,000 busy RTC ticks, a contender clears scratch 3 to break a
 * presumed dead lock. Hardware holders run continuously through a few
 * uncached accesses and release before that deadline. Under multi-threaded
 * TCG, each access takes the big lock. Two accesses per polling iteration can
 * starve a live holder until the contender breaks its lock; concurrent
 * read-modify-writes then lose an update and leave an unowned bit set.
 *
 * Both CPUs rendezvous, then repeat the PROM's poll-and-break sequence. Each
 * holds the lock through many uncached reads interleaved with register work,
 * increments shared LB scratch 0, and releases it. The final word must equal
 * both CPUs' total rounds, and blocked contenders must make few busy reads.
 * Parking until release enforces both; without it, the busy-read check fails
 * on any host and mutual exclusion can fail with an unfair big lock.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"

#define BEDROCK_LOCAL (SPACE_XTALK + UINT64_C(0x01000000))
#define LB_SCRATCH_0 UINT64_C(0x608000)
#define LB_SCRATCH_3 UINT64_C(0x608018)
#define LB_SCRATCH_3_TEST_AND_SET UINT64_C(0x608058)
#define PI_CPU_NUMBER UINT64_C(0x20)
#define PI_RT_COUNTER UINT64_C(0x30100)

/*
 * State words in low RAM, clear of the flash and of the 32 KiB per-processor
 * window the firmware filter overlays at 1 MiB. The guest reaches them
 * through Hub special space's uncached alias of its own node memory, and the
 * host reads the same alias: plain node memory sits behind the
 * memory/directory unit's bank translation, which answers nothing until
 * firmware configures it. Slot A's block, then slot C's block 0x100 above it.
 */
#define STATE_PHYS UINT64_C(0x180000)
#define STATE_HOST (SPACE_HUB + STATE_PHYS)
#define STATE_SLOT_STRIDE 0x100
#define STATE_ALIVE 0x00
#define STATE_DONE 0x08
#define STATE_ROUNDS 0x10
#define STATE_BREAKS 0x18
#define STATE_BUSY 0x20

#define ALIVE_MAGIC 0xa11e
#define DONE_MAGIC 0xd04e

#define ROUNDS 64
/* Uncached reads inside the section, and register-only work between. */
#define SECTION_READS 32
#define SECTION_SPIN 2000
/*
 * The contender's break deadline: two seconds of the 1.25 MHz counter,
 * matching the model's own hold bound with room to spare, so a contender the
 * model parks on a held lock is woken by the release long before it would
 * break the lock. A contender left polling instead (no fix) never reaches it
 * either, because the holder's section is far shorter. The deadline is a
 * backstop, and breaks stay zero on both paths.
 */
#define DEADLINE_TICKS 2500000
/*
 * A parked contender makes one busy read per hold, and a hold ends at the
 * holder's release, so busy reads stay near the round count. A polling
 * contender makes hundreds per round.
 */
#define BUSY_LIMIT (8 * 2 * ROUNDS)

#define POLL_TRIES 3000
#define POLL_INTERVAL_US 10000

#define R_ZERO 0
#define R_T0 8
#define R_T1 9
#define R_T2 10
#define R_T3 11
#define R_T4 12
#define R_T5 13
#define R_T6 14
#define R_T7 15
#define R_S0 16
#define R_S1 17
#define R_S2 18
#define R_S3 19
#define R_S4 20
#define R_S5 21
#define R_S6 22
#define R_S7 23
#define R_T8 24

#define OPC_BEQ 4
#define OPC_BNE 5
#define OPC_ANDI 12
#define OPC_ORI 13
#define OPC_LUI 15
#define OPC_DADDIU 25
#define OPC_LD 55
#define OPC_SD 63

#define CP0_STATUS 12
#define STATUS_KX 0x80

typedef struct Image {
    uint8_t *bytes;
    unsigned int pos;
} Image;

static void emit(Image *p, uint32_t word)
{
    g_assert_cmpuint(p->pos + 4, <=, PROM_SIZE);
    stl_be_p(p->bytes + p->pos, word);
    p->pos += 4;
}

static void emit_i(Image *p, unsigned int op, unsigned int rs,
                   unsigned int rt, uint16_t imm)
{
    emit(p, (op << 26) | (rs << 21) | (rt << 16) | imm);
}

static void emit_nop(Image *p)
{
    emit(p, 0);
}

/* Load a 32-bit constant into @rt. */
static void emit_li32(Image *p, unsigned int rt, uint32_t value)
{
    emit_i(p, OPC_LUI, R_ZERO, rt, value >> 16);
    emit_i(p, OPC_ORI, rt, rt, value);
}

/* Load a 64-bit constant: high halfword, then three shift-and-or steps. */
static void emit_li64(Image *p, unsigned int rt, uint64_t value)
{
    emit_i(p, OPC_ORI, R_ZERO, rt, value >> 48);
    for (unsigned int shift = 32; ; shift -= 16) {
        emit(p, (rt << 16) | (rt << 11) | (16 << 6) | 0x38); /* dsll 16 */
        emit_i(p, OPC_ORI, rt, rt, value >> shift);
        if (shift == 0) {
            break;
        }
    }
}

static void emit_daddu(Image *p, unsigned int rd, unsigned int rs,
                       unsigned int rt)
{
    emit(p, (rs << 21) | (rt << 16) | (rd << 11) | 0x2d);
}

static void emit_sltu(Image *p, unsigned int rd, unsigned int rs,
                      unsigned int rt)
{
    emit(p, (rs << 21) | (rt << 16) | (rd << 11) | 0x2b);
}

/* Branch with a delay-slot nop; the target is patched in later. */
static unsigned int emit_branch(Image *p, unsigned int op, unsigned int rs,
                                unsigned int rt)
{
    unsigned int at = p->pos;

    emit_i(p, op, rs, rt, 0);
    emit_nop(p);
    return at;
}

static void patch_branch(Image *p, unsigned int at, unsigned int dest)
{
    int delta = ((int)dest - (int)at - 4) / 4;

    g_assert_cmpint(delta, >=, INT16_MIN);
    g_assert_cmpint(delta, <=, INT16_MAX);
    stl_be_p(p->bytes + at, ldl_be_p(p->bytes + at) | (uint16_t)delta);
}

/* Add one to the 64-bit state word at @offset off s1. Clobbers t0. */
static void emit_count(Image *p, unsigned int offset)
{
    emit_i(p, OPC_LD, R_S1, R_T0, offset);
    emit_i(p, OPC_DADDIU, R_T0, R_T0, 1);
    emit_i(p, OPC_SD, R_S1, R_T0, offset);
}

/*
 * Guest code. s0 is the local hub window, s1 this processor's state block, s2
 * the test-and-set twin, s3 plain scratch 3, s4 the shared word in scratch 0,
 * s5 the real-time counter, s6 the rounds left and s7 the current claim's
 * deadline.
 */
static void build_program(Image *p)
{
    unsigned int wait_peer, round, poll, got, retry, section, spin;
    unsigned int branch;

    stl_be_p(p->bytes + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(p->bytes + offset, 0x1000ffff); /* beq zero, zero, self */
    }
    p->pos = 0x400;

    /* 64-bit kernel addressing, without which the pointers below fault. */
    emit(p, 0x40000000 | (R_T0 << 16) | (CP0_STATUS << 11)); /* mfc0 */
    emit_i(p, OPC_ORI, R_T0, R_T0, STATUS_KX);
    emit(p, 0x40800000 | (R_T0 << 16) | (CP0_STATUS << 11)); /* mtc0 */
    for (unsigned int i = 0; i < 4; i++) {
        emit_nop(p);
    }

    /* Uncached, crosstalk attribute: the local hub register window. */
    emit_li64(p, R_S0, UINT64_C(0x9200000001000000));
    emit_li64(p, R_S2,
              UINT64_C(0x9200000001000000) | LB_SCRATCH_3_TEST_AND_SET);
    emit_li64(p, R_S3, UINT64_C(0x9200000001000000) | LB_SCRATCH_3);
    emit_li64(p, R_S4, UINT64_C(0x9200000001000000) | LB_SCRATCH_0);
    emit_li64(p, R_S5, UINT64_C(0x9200000001000000) | PI_RT_COUNTER);

    /* This processor's state block: slot A at the base, slot C above. */
    emit_li64(p, R_S1, UINT64_C(0x9000000000000000) | STATE_PHYS);
    emit_i(p, OPC_LD, R_S0, R_T2, PI_CPU_NUMBER);
    emit_i(p, OPC_ANDI, R_T2, R_T2, 2);              /* the PI bit */
    emit(p, (R_T2 << 16) | (R_T2 << 11) | (7 << 6) | 0x38); /* dsll 7 */
    emit_daddu(p, R_S1, R_S1, R_T2);

    /*
     * Rendezvous, so both contend from the first round: publish alive, then
     * wait on the peer's block, which is the other one of the two.
     */
    emit_i(p, OPC_ORI, R_ZERO, R_T0, ALIVE_MAGIC);
    emit_i(p, OPC_SD, R_S1, R_T0, STATE_ALIVE);
    emit_li64(p, R_T1, UINT64_C(0x9000000000000000) |
              (STATE_PHYS + STATE_SLOT_STRIDE));
    emit(p, (R_T1 << 21) | (R_T2 << 16) | (R_T1 << 11) | 0x2f); /* dsubu */
    wait_peer = p->pos;
    emit_i(p, OPC_LD, R_T1, R_T0, STATE_ALIVE);
    branch = emit_branch(p, OPC_BEQ, R_T0, R_ZERO);
    patch_branch(p, branch, wait_peer);

    emit_i(p, OPC_ORI, R_ZERO, R_S6, ROUNDS);

    /* Each round: a claim with the PROM's deadline and break. */
    round = p->pos;
    emit_i(p, OPC_LD, R_S5, R_T0, 0);
    emit_li32(p, R_T1, DEADLINE_TICKS);
    emit_daddu(p, R_S7, R_T0, R_T1);
    poll = p->pos;
    emit_i(p, OPC_LD, R_S2, R_T0, 0);                /* test-and-set */
    got = emit_branch(p, OPC_BEQ, R_T0, R_ZERO);
    emit_count(p, STATE_BUSY);
    emit_i(p, OPC_LD, R_S5, R_T1, 0);                /* now */
    emit_sltu(p, R_T3, R_S7, R_T1);                  /* deadline < now */
    retry = emit_branch(p, OPC_BEQ, R_T3, R_ZERO);
    patch_branch(p, retry, poll);
    emit_i(p, OPC_SD, R_S3, R_ZERO, 0);              /* break the lock */
    emit_count(p, STATE_BREAKS);
    branch = emit_branch(p, OPC_BEQ, R_ZERO, R_ZERO);
    patch_branch(p, branch, poll);

    /*
     * The critical section: read the shared word, then many more uncached
     * reads each followed by register-only work, then write it back plus one
     * and release.
     */
    patch_branch(p, got, p->pos);
    emit_i(p, OPC_LD, R_S4, R_T5, 0);
    emit_i(p, OPC_ORI, R_ZERO, R_T6, SECTION_READS);
    section = p->pos;
    emit_i(p, OPC_LD, R_S4, R_T7, 0);
    emit_i(p, OPC_ORI, R_ZERO, R_T8, SECTION_SPIN);
    spin = p->pos;
    emit_i(p, OPC_DADDIU, R_T8, R_T8, (uint16_t)-1);
    branch = emit_branch(p, OPC_BNE, R_T8, R_ZERO);
    patch_branch(p, branch, spin);
    emit_i(p, OPC_DADDIU, R_T6, R_T6, (uint16_t)-1);
    branch = emit_branch(p, OPC_BNE, R_T6, R_ZERO);
    patch_branch(p, branch, section);
    emit_i(p, OPC_DADDIU, R_T5, R_T5, 1);
    emit_i(p, OPC_SD, R_S4, R_T5, 0);
    emit_i(p, OPC_SD, R_S3, R_ZERO, 0);              /* release */
    emit_count(p, STATE_ROUNDS);
    emit_i(p, OPC_DADDIU, R_S6, R_S6, (uint16_t)-1);
    branch = emit_branch(p, OPC_BNE, R_S6, R_ZERO);
    patch_branch(p, branch, round);

    emit_i(p, OPC_ORI, R_ZERO, R_T0, DONE_MAGIC);
    emit_i(p, OPC_SD, R_S1, R_T0, STATE_DONE);
    emit(p, 0x1000ffff); /* beq zero, zero, self */
    emit_nop(p);
}

static void write_exact(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        g_assert_cmpint(n, >, 0);
        sent += n;
    }
}

static char *write_image(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *bytes = g_malloc0(PROM_SIZE);
    Image image = { .bytes = bytes };
    char *path = NULL;
    int fd;

    build_program(&image);
    fd = g_file_open_tmp("sn1-hub-lock-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_exact(fd, bytes, PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static uint64_t state_word(QTestState *s, unsigned int slot,
                           unsigned int offset)
{
    return qtest_readq(s, STATE_HOST + slot * STATE_SLOT_STRIDE + offset);
}

static void test_contender_waits_for_a_running_holder(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    QTestState *s;
    uint64_t done[2] = { 0, 0 };
    uint64_t rounds[2], breaks[2], busy[2];

    /* Guest code has to run, so the accelerator has to be a real one. */
    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    prom = write_image();
    quoted = g_shell_quote(prom);
    /*
     * The accelerator named here wins over the -accel qtest libqtest appends:
     * the race needs each processor on its own host thread.
     */
    s = qtest_initf("-M origin300 -cpu R14000 -smp 2 -bios %s "
                    "-accel tcg,thread=multi -no-shutdown -S", quoted);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");

    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        done[0] = state_word(s, 0, STATE_DONE);
        done[1] = state_word(s, 1, STATE_DONE);
        if (done[0] == DONE_MAGIC && done[1] == DONE_MAGIC) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    for (unsigned int slot = 0; slot < 2; slot++) {
        rounds[slot] = state_word(s, slot, STATE_ROUNDS);
        breaks[slot] = state_word(s, slot, STATE_BREAKS);
        busy[slot] = state_word(s, slot, STATE_BUSY);
        g_test_message("slot %c: rounds %" PRIu64 " busy reads %" PRIu64
                       " breaks %" PRIu64, slot ? 'C' : 'A', rounds[slot],
                       busy[slot], breaks[slot]);
    }
    g_assert_cmphex(done[0], ==, DONE_MAGIC);
    g_assert_cmphex(done[1], ==, DONE_MAGIC);
    g_assert_cmpuint(rounds[0], ==, ROUNDS);
    g_assert_cmpuint(rounds[1], ==, ROUNDS);

    /* No live holder had its lock broken: every update landed. */
    g_assert_cmpuint(qtest_readq(s, BEDROCK_LOCAL + LB_SCRATCH_0), ==,
                     2 * ROUNDS);
    /* And a contender was parked, not left polling the busy lock. */
    g_assert_cmpuint(busy[0] + busy[1], <=, BUSY_LIMIT);
    g_assert_cmphex(qtest_readq(s, BEDROCK_LOCAL + LB_SCRATCH_3), ==, 0);

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/hub-lock/contender-waits-for-a-running-holder",
                   test_contender_waits_for_a_running_holder);
    return g_test_run();
}
