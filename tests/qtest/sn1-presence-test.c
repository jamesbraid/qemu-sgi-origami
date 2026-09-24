/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Bedrock presence-publication join under multi-threaded TCG, taken
 * by the processors themselves.
 *
 * Right after the reset-edge rendezvous the stock IP35 PROM's lowest
 * processor on each processor interface publishes the interface's fitted
 * processors as bits 4 to 7 of II scratch 0, under the hub lock, and every
 * processor's first barrier then reads those bits to decide whom to wait
 * for: a processor whose bit is still clear is treated as absent. On
 * hardware the publisher's two stores land long before any peer's first
 * such read. Under multi-threaded TCG the publisher's host thread can be
 * left unscheduled across that window, so its peers read the bits clear,
 * pass their first barrier without it, and stay a barrier ahead of it for
 * the rest of the boot -- until the copy barrier, where they switch to the
 * PROM's RAM copy while it is still being made (docs/known-issues.md, "The
 * presence-publication join").
 *
 * Four processors run the same program, with the roles the PROM gives them:
 * every one makes its first LB scratch 1 write; the publishers (slots A and
 * C) make their second, take the hub lock, publish their interface's two
 * bits and release, C only after half a second of the real-time counter
 * has passed; the others (B and D) make their second write at once. Every
 * processor then records its first read of II scratch 0. The model must
 * show every processor seeing all four bits on that read: B and D because
 * their second scratch 1 write was held until both interfaces had
 * published, A because its release parked it until C had. Without the
 * join, B, D and A all read before C's publication and record 0x30 at
 * most.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"

#define BEDROCK_LOCAL (SPACE_XTALK + UINT64_C(0x01000000))
#define II_SCRATCH_0 UINT64_C(0x400150)
#define LB_SCRATCH_1 UINT64_C(0x608008)
#define LB_SCRATCH_3 UINT64_C(0x608018)
#define LB_SCRATCH_3_TEST_AND_SET UINT64_C(0x608058)
#define PI_CPU_NUMBER UINT64_C(0x20)
#define PI_RT_COUNTER UINT64_C(0x30100)

/*
 * State words in low RAM, clear of the flash and of the 32 KiB
 * per-processor window the firmware filter overlays at 1 MiB, reached by
 * the guest through Hub special space's uncached alias of its own node
 * memory and by the host through the same alias (the hub-lock test says
 * why). One block per processor, by the PI's own processor number.
 */
#define STATE_PHYS UINT64_C(0x180000)
#define STATE_HOST (SPACE_HUB + STATE_PHYS)
#define STATE_SLOT_STRIDE 0x100
#define STATE_SEEN 0x00
#define STATE_DONE 0x08

#define DONE_MAGIC 0xd04e

/* Every fitted slot's bit, once both interfaces have published. */
#define PRESENCE_ALL 0xf0

/*
 * How long C holds back its publication: half a second of the 1.25 MHz
 * real-time counter, which follows the host clock under multi-threaded
 * TCG, so it is the same under any host load. Far longer than the others
 * need to reach their first read, and a quarter of the model's hold
 * bound, so a parked processor is woken by the publication rather than
 * timing out.
 */
#define PUBLISH_DELAY_TICKS 625000

#define POLL_TRIES 3000
#define POLL_INTERVAL_US 10000

#define R_ZERO 0
#define R_T0 8
#define R_T1 9
#define R_T2 10
#define R_T3 11
#define R_T4 12
#define R_T5 13
#define R_S0 16
#define R_S1 17
#define R_S2 18
#define R_S3 19
#define R_S4 20
#define R_S5 21
#define R_S6 22

#define OPC_BEQ 4
#define OPC_BNE 5
#define OPC_ANDI 12
#define OPC_ORI 13
#define OPC_LUI 15
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

static void emit_or(Image *p, unsigned int rd, unsigned int rs,
                    unsigned int rt)
{
    emit(p, (rs << 21) | (rt << 16) | (rd << 11) | 0x25);
}

/* rd = rt << rs. */
static void emit_dsllv(Image *p, unsigned int rd, unsigned int rt,
                       unsigned int rs)
{
    emit(p, (rs << 21) | (rt << 16) | (rd << 11) | 0x14);
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

/*
 * Guest code. s0 is the local hub window, s1 this processor's state
 * block, s2 LB scratch 1, s3 II scratch 0, s4 the hub lock's test-and-set
 * twin, s5 plain LB scratch 3, s6 the real-time counter, and t2 this
 * processor's number from the PI: bit 0 its slot within the interface, bit
 * 1 the interface.
 */
static void build_program(Image *p)
{
    unsigned int branch, to_peer, to_read, wait, lock;

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
    emit_li64(p, R_S2, UINT64_C(0x9200000001000000) | LB_SCRATCH_1);
    emit_li64(p, R_S3, UINT64_C(0x9200000001000000) | II_SCRATCH_0);
    emit_li64(p, R_S4,
              UINT64_C(0x9200000001000000) | LB_SCRATCH_3_TEST_AND_SET);
    emit_li64(p, R_S5, UINT64_C(0x9200000001000000) | LB_SCRATCH_3);
    emit_li64(p, R_S6, UINT64_C(0x9200000001000000) | PI_RT_COUNTER);

    /* This processor's state block, by its processor number. */
    emit_li64(p, R_S1, UINT64_C(0x9000000000000000) | STATE_PHYS);
    emit_i(p, OPC_LD, R_S0, R_T2, PI_CPU_NUMBER);
    emit_i(p, OPC_ANDI, R_T2, R_T2, 3);
    emit(p, (R_T2 << 16) | (R_T0 << 11) | (8 << 6) | 0x38); /* dsll t0,t2,8 */
    emit_daddu(p, R_S1, R_S1, R_T0);

    /* Every processor's first LB scratch 1 write: the reset-edge join. */
    emit_i(p, OPC_SD, R_S2, R_ZERO, 0);

    /* Its slot within the interface: the second one only reads. */
    emit_i(p, OPC_ANDI, R_T2, R_T3, 1);             /* t3 = t2 & 1 */
    to_peer = emit_branch(p, OPC_BNE, R_T3, R_ZERO);

    /*
     * A publisher. C alone (interface bit set) first waits out the delay
     * on the real-time counter, the way a late host thread would.
     */
    emit_i(p, OPC_ANDI, R_T2, R_T4, 2);             /* t4 = t2 & 2 */
    branch = emit_branch(p, OPC_BEQ, R_T4, R_ZERO);
    emit_i(p, OPC_LD, R_S6, R_T0, 0);
    emit_li32(p, R_T1, PUBLISH_DELAY_TICKS);
    emit_daddu(p, R_T5, R_T0, R_T1);                 /* deadline */
    wait = p->pos;
    emit_i(p, OPC_LD, R_S6, R_T0, 0);
    emit_sltu(p, R_T1, R_T0, R_T5);                  /* now < deadline */
    emit_i(p, OPC_BNE, R_T1, R_ZERO, 0);
    patch_branch(p, p->pos - 4, wait);
    emit_nop(p);
    patch_branch(p, branch, p->pos);

    /* The second scratch 1 write, then publish under the hub lock. */
    emit_i(p, OPC_SD, R_S2, R_ZERO, 0);
    lock = p->pos;
    emit_i(p, OPC_LD, R_S4, R_T0, 0);                /* test-and-set */
    emit_i(p, OPC_BNE, R_T0, R_ZERO, 0);
    patch_branch(p, p->pos - 4, lock);
    emit_nop(p);
    emit_i(p, OPC_LD, R_S3, R_T0, 0);
    emit_i(p, OPC_ORI, R_ZERO, R_T1, 0x30);          /* both slots' bits */
    emit_dsllv(p, R_T1, R_T1, R_T2);                 /* ... of my interface */
    emit_or(p, R_T0, R_T0, R_T1);
    emit_i(p, OPC_SD, R_S3, R_T0, 0);
    emit_i(p, OPC_SD, R_S5, R_ZERO, 0);              /* release */
    to_read = emit_branch(p, OPC_BEQ, R_ZERO, R_ZERO);

    /* The interface's other processor: its second write, then the read. */
    patch_branch(p, to_peer, p->pos);
    emit_i(p, OPC_SD, R_S2, R_ZERO, 0);

    /* The first presence read, recorded as is. */
    patch_branch(p, to_read, p->pos);
    emit_i(p, OPC_LD, R_S3, R_T0, 0);
    emit_i(p, OPC_SD, R_S1, R_T0, STATE_SEEN);
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
    fd = g_file_open_tmp("sn1-presence-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_exact(fd, bytes, PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static uint64_t state_word(QTestState *s, unsigned int number,
                           unsigned int offset)
{
    return qtest_readq(s, STATE_HOST + number * STATE_SLOT_STRIDE + offset);
}

static void test_first_presence_read_sees_every_interface(void)
{
    static const char names[4] = { 'A', 'B', 'C', 'D' };
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    QTestState *s;
    uint64_t done[4] = { 0, 0, 0, 0 };
    uint64_t seen[4];

    /* Guest code has to run, so the accelerator has to be a real one. */
    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    prom = write_image();
    quoted = g_shell_quote(prom);
    /*
     * The accelerator named here wins over the -accel qtest libqtest
     * appends: the race needs each processor on its own host thread.
     */
    s = qtest_initf("-M origin300 -cpu R14000 -smp 4 -bios %s "
                    "-accel tcg,thread=multi -no-shutdown -S", quoted);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");

    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        bool all = true;

        for (unsigned int n = 0; n < 4; n++) {
            done[n] = state_word(s, n, STATE_DONE);
            all = all && done[n] == DONE_MAGIC;
        }
        if (all) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    for (unsigned int n = 0; n < 4; n++) {
        seen[n] = state_word(s, n, STATE_SEEN);
        g_test_message("processor %c: first presence read 0x%" PRIx64,
                       names[n], seen[n]);
    }
    for (unsigned int n = 0; n < 4; n++) {
        g_assert_cmphex(done[n], ==, DONE_MAGIC);
    }
    /* Both interfaces published, and the hub lock came back free. */
    g_assert_cmphex(qtest_readq(s, BEDROCK_LOCAL + II_SCRATCH_0), ==,
                    PRESENCE_ALL);
    g_assert_cmphex(qtest_readq(s, BEDROCK_LOCAL + LB_SCRATCH_3), ==, 0);
    /* And nobody read the bits before both had published. */
    for (unsigned int n = 0; n < 4; n++) {
        g_assert_cmphex(seen[n], ==, PRESENCE_ALL);
    }

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/presence/first-read-sees-every-interface",
                   test_first_presence_read_sees_every_interface);
    return g_test_run();
}
