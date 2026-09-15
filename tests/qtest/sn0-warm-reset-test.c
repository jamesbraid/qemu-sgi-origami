/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Every processor must re-enter a generated ROM after a guest-requested reset.
 * Public Linux arch/mips/sgi-ip27/ip27-reset.c writes NI_PORT_RESET with
 * NPR_PORTRESET | NPR_LOCALRESET. Its asm/sn/sn0/hubni.h and hubpi.h define
 * the reset bits, node ID and processor-number registers used here.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sn0-user-image.h"

#define IMAGE_SIZE       0x1000
#define CODE_START       0x400
#define PARK             0x800
#define NODE_STRIDE      0x100000000ULL
#define MAILBOX          0x00200000
#define RESET_REQUEST    0x10
#define RESET_CYCLES     32
#define HUB_LOCAL        0x9200000001000000ULL
#define PI_CPU_NUM       0x20
#define NI_STATUS        0x600000
#define NI_PORT_RESET    0x600008
#define WARM_LOCAL_RESET 0x81

typedef struct GuestImage {
    uint8_t code[IMAGE_SIZE];
    unsigned int pos;
} GuestImage;

static void emit(GuestImage *img, uint32_t instruction)
{
    g_assert_cmpuint(img->pos + 4, <=, IMAGE_SIZE);
    stl_be_p(img->code + img->pos, instruction);
    img->pos += 4;
}

static void emit_i(GuestImage *img, unsigned int op, unsigned int rs,
                   unsigned int rt, uint16_t imm)
{
    emit(img, (op << 26) | (rs << 21) | (rt << 16) | imm);
}

static void emit_li64(GuestImage *img, unsigned int rt, uint64_t value)
{
    emit_i(img, 13, 0, rt, value >> 48);
    for (int shift = 32; shift >= 0; shift -= 16) {
        emit(img, (rt << 16) | (rt << 11) | (16 << 6) | 0x38);
        emit_i(img, 13, rt, rt, value >> shift);
    }
}

static void emit_branch(GuestImage *img, unsigned int op, unsigned int rs,
                        unsigned int rt, unsigned int target)
{
    int offset = ((int)target - (int)img->pos - 4) / 4;

    g_assert_cmpint(offset, >=, INT16_MIN);
    g_assert_cmpint(offset, <=, INT16_MAX);
    emit_i(img, op, rs, rt, offset);
    emit(img, 0);
}

static void write_guest_image(int fd)
{
    GuestImage img = { };
    unsigned int poll;
    ssize_t written;

    /* Leave the board's flash configuration block clear. */
    emit_branch(&img, 4, 0, 0, CODE_START);
    img.pos = CODE_START;

    /* Enable Status.KX for explicit, uncached node-qualified addresses. */
    emit(&img, 0x40086000);          /* mfc0 t0, Status */
    emit_i(&img, 13, 8, 8, 0x80);   /* ori t0, t0, KX */
    emit(&img, 0x40886000);          /* mtc0 t0, Status */
    for (unsigned int i = 0; i < 4; i++) {
        emit(&img, 0);
    }

    emit_li64(&img, 8, HUB_LOCAL);
    emit_i(&img, 55, 8, 9, PI_CPU_NUM); /* ld t1, PI_CPU_NUM(t0) */
    emit_li64(&img, 10, HUB_LOCAL + NI_STATUS);
    emit_i(&img, 55, 10, 11, 0);       /* ld t3, 0(t2) */
    emit(&img, 0x000b5a02);            /* srl t3, t3, 8 */
    emit_i(&img, 12, 11, 11, 0x1ff);  /* andi t3, t3, NASID mask */

    /* t4 names this node's RAM; t5 selects this slice's independent word. */
    emit_li64(&img, 12, 0x9600000000000000ULL + MAILBOX);
    emit(&img, 0x000b683c);            /* dsll32 t5, t3, 0 */
    emit(&img, 0x018d6025);            /* or t4, t4, t5 */
    emit(&img, 0x000968f8);            /* dsll t5, t1, 3 */
    emit(&img, 0x018d682d);            /* daddu t5, t4, t5 */
    emit_i(&img, 55, 13, 14, 0);       /* ld t6, 0(t5) */
    emit_i(&img, 25, 14, 14, 1);       /* daddiu t6, t6, 1 */
    emit_i(&img, 63, 13, 14, 0);       /* sd t6, 0(t5) */
    emit(&img, 0x0000000f);            /* sync */

    /* Only node zero, slice zero requests resets; other CPUs keep executing. */
    emit_branch(&img, 5, 11, 0, PARK);
    emit_branch(&img, 5, 9, 0, PARK);
    poll = img.pos;
    emit_i(&img, 55, 12, 15, RESET_REQUEST); /* ld t7, request(t4) */
    emit_branch(&img, 5, 15, 14, poll);
    emit(&img, 0x0000000f);                 /* sync */
    emit_i(&img, 9, 0, 15, WARM_LOCAL_RESET);
    emit_i(&img, 63, 10, 15, NI_PORT_RESET - NI_STATUS);
    emit_branch(&img, 4, 0, 0, PARK);

    img.pos = PARK;
    emit_branch(&img, 4, 0, 0, PARK);
    written = write(fd, img.code, sizeof(img.code));
    g_assert_cmpint(written, ==, sizeof(img.code));
}

static uint64_t mailbox(unsigned int cpu)
{
    return (cpu / 2) * NODE_STRIDE + MAILBOX + (cpu % 2) * 8;
}

static void wait_for_generation(QTestState *qts, unsigned int generation)
{
    unsigned int seen = 0;

    /*
     * Completion is a guest observation, independent of host speed. Meson's
     * timeout bounds a hung run; a timeout alone does not prove a lost CPU.
     */
    while (seen != 0xf) {
        for (unsigned int cpu = 0; cpu < 4; cpu++) {
            uint64_t value = qtest_readq(qts, mailbox(cpu));

            g_assert_cmpuint(value, <=, generation);
            if (value == generation && !(seen & (1u << cpu))) {
                seen |= 1u << cpu;
                g_test_message("generation %u: CPU %u arrived",
                               generation, cpu);
            }
        }
        if (seen != 0xf) {
            g_usleep(1000);
        }
    }
}

static void test_warm_reset_rendezvous(void)
{
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    fd = g_file_open_tmp("sn0-warm-reset-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    write_guest_image(fd);
    close(fd);
    qts = qtest_initf(
        "-M origin200 -m 128M -cpu R10000 "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1 "
        "-bios %s -accel tcg,thread=multi -S -no-shutdown", image);
    unlink(image);
    /* No PROM ran to number node one's Hub; mailbox(2) and mailbox(3)
     * need it claimed, and a full machine reset (every generation below)
     * clears it back to zero along with everything else. */
    sn0_program_nasid(qts, 1);
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        qtest_writeq(qts, mailbox(cpu), 0);
    }
    qtest_writeq(qts, MAILBOX + RESET_REQUEST, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_generation(qts, 1);
    for (unsigned int generation = 1;
         generation <= RESET_CYCLES; generation++) {
        qtest_writeq(qts, MAILBOX + RESET_REQUEST, generation);
        qtest_qmp_eventwait(qts, "RESET");
        sn0_program_nasid(qts, 1);
        wait_for_generation(qts, generation + 1);
    }
    qtest_quit(qts);
}

#define HUB_BY_NODE       0x20001800000ULL
#define PI_INT_PEND_MOD   0x90
#define PI_INT_PEND0      0x98
#define NI_SCRATCH_REG0   0x600100
#define NI_SCRATCH_REG1   0x600108
#define PUBLISH_BIT       7
#define ACK_BIT           8
#define DONE_BIT          9
#define READBACK_FAIL_BIT 10
#define STALE_FAIL_BIT    11
#define COMPLETE_BITS    ((1u << PUBLISH_BIT) | (1u << ACK_BIT) | \
                          (1u << DONE_BIT))
#define OWNED_BITS       (COMPLETE_BITS | (1u << READBACK_FAIL_BIT) | \
                          (1u << STALE_FAIL_BIT))
#define CONSUMER          0x600
#define READBACK_FAIL     0x700
#define STALE_FAIL        0x740

static void emit_pending(GuestImage *img, unsigned int bit)
{
    emit_i(img, 13, 0, 12, 0x100 | bit); /* ori t4, zero, set|bit */
    emit_i(img, 63, 8, 12, PI_INT_PEND_MOD);
    emit_i(img, 55, 8, 12, PI_INT_PEND0);
    emit(img, 0x0000000f);               /* sync 0 */
}

static void emit_wait_pending(GuestImage *img, unsigned int bit)
{
    unsigned int poll = img->pos;

    emit_i(img, 55, 8, 12, PI_INT_PEND0);
    emit_i(img, 12, 12, 12, 1u << bit);
    emit_branch(img, 4, 12, 0, poll);
    emit(img, 0x0000000f);               /* sync 0 */
}

static void write_scratch_publish_image(int fd)
{
    GuestImage img = { };
    ssize_t written;

    emit_branch(&img, 4, 0, 0, CODE_START);
    img.pos = CODE_START;
    emit(&img, 0x40086000);          /* mfc0 t0, Status */
    emit_i(&img, 13, 8, 8, 0x80);   /* ori t0, t0, KX */
    emit(&img, 0x40886000);          /* mtc0 t0, Status */
    for (unsigned int i = 0; i < 4; i++) {
        emit(&img, 0);
    }
    emit_li64(&img, 8, HUB_LOCAL);
    emit_i(&img, 55, 8, 9, PI_CPU_NUM);
    emit_li64(&img, 10, HUB_LOCAL + NI_SCRATCH_REG0);
    emit_i(&img, 55, 10, 16, 0);     /* ld s0, expected token */
    emit_branch(&img, 5, 9, 0, CONSUMER);

    /* Complete the uncached publication before telling the other slice. */
    emit_i(&img, 63, 10, 16, 8);    /* sd s0, SCRATCH_REG1 */
    emit_i(&img, 55, 10, 11, 8);    /* ld t3, SCRATCH_REG1 */
    emit(&img, 0x0000000f);         /* sync 0 */
    emit_branch(&img, 5, 11, 16, READBACK_FAIL);
    emit_pending(&img, PUBLISH_BIT);
    emit_wait_pending(&img, ACK_BIT);
    emit_pending(&img, DONE_BIT);
    emit_branch(&img, 4, 0, 0, PARK);
    g_assert_cmpuint(img.pos, <=, CONSUMER);

    img.pos = CONSUMER;
    emit_wait_pending(&img, PUBLISH_BIT);
    emit_i(&img, 55, 10, 11, 8);    /* ld t3, SCRATCH_REG1 */
    emit_branch(&img, 5, 11, 16, STALE_FAIL);
    emit_pending(&img, ACK_BIT);
    emit_branch(&img, 4, 0, 0, PARK);
    g_assert_cmpuint(img.pos, <=, READBACK_FAIL);

    img.pos = READBACK_FAIL;
    emit_pending(&img, READBACK_FAIL_BIT);
    emit_branch(&img, 4, 0, 0, PARK);
    img.pos = STALE_FAIL;
    emit_i(&img, 63, 10, 11, 0);    /* Preserve the peer's observed value. */
    emit_i(&img, 55, 10, 12, 0);
    emit(&img, 0x0000000f);
    emit_pending(&img, STALE_FAIL_BIT);
    emit_branch(&img, 4, 0, 0, PARK);
    img.pos = PARK;
    emit_branch(&img, 4, 0, 0, PARK);
    written = write(fd, img.code, sizeof(img.code));
    g_assert_cmpint(written, ==, sizeof(img.code));
}

static void wait_for_scratch_publish(QTestState *qts, unsigned int phase)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    uint64_t pending[2] = { };

    for (;;) {
        bool done = true;

        for (unsigned int node = 0; node < 2; node++) {
            uint64_t base = HUB_BY_NODE + node * NODE_STRIDE;
            uint64_t expected = 0x13579bdf24680000ULL + phase * 0x100 + node;

            pending[node] = qtest_readq(qts, base + PI_INT_PEND0);
            g_assert_false(pending[node] & (1u << READBACK_FAIL_BIT));
            if (pending[node] & (1u << STALE_FAIL_BIT)) {
                g_error("phase %u node %u: notification preceded stale data; "
                        "expected 0x%" PRIx64 ", observed 0x%" PRIx64,
                        phase, node,
                        expected,
                        qtest_readq(qts, base + NI_SCRATCH_REG0));
            }
            if (pending[node] & (1u << DONE_BIT)) {
                g_assert_cmphex(pending[node] & COMPLETE_BITS, ==,
                                COMPLETE_BITS);
                g_assert_cmphex(qtest_readq(qts, base + NI_SCRATCH_REG1), ==,
                                expected);
            } else {
                done = false;
            }
        }
        if (done) {
            return;
        }
        if (g_get_monotonic_time() >= deadline) {
            g_error("phase %u: host deadline, not a visibility verdict; "
                    "pending node 0=0x%" PRIx64 " node 1=0x%" PRIx64,
                    phase, pending[0], pending[1]);
        }
        g_usleep(1000);
    }
}

/*
 * R10000 User's Manual v2.0 (1996), p. 56: uncached readback plus SYNC
 * completes publication. Linux db2ddb87143519e20a95aa36c60b36107b736a58,
 * arch/mips/include/asm/sn/intr.h, supplies PI pending set/clear semantics.
 * No firmware timing, CPU start order, or reset register value is assumed.
 */
static void test_scratch_publish(const void *opaque)
{
    const char *thread = opaque;
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    fd = g_file_open_tmp("sn0-scratch-publish-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    write_scratch_publish_image(fd);
    close(fd);
    qts = qtest_initf(
        "-M origin200 -m 128M -cpu R10000 "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1 "
        "-bios %s -accel tcg,thread=%s -S -no-shutdown", image, thread);
    unlink(image);

    /* No PROM ran to number node one's Hub; every base below for node 1
     * needs it claimed, and phase 1's full machine reset clears it again. */
    sn0_program_nasid(qts, 1);
    for (unsigned int phase = 0; phase < 2; phase++) {
        QDict *status;

        if (phase) {
            qtest_qmp_assert_success(qts, "{'execute':'stop'}");
            qtest_writeq(qts, HUB_BY_NODE + NI_PORT_RESET, WARM_LOCAL_RESET);
            qtest_qmp_eventwait(qts, "RESET");
            sn0_program_nasid(qts, 1);
        }
        /* Pausing at the register boundary lets us initialize both phases. */
        status = qtest_qmp(qts, "{'execute':'query-status'}");
        g_assert_false(qdict_get_bool(qdict_get_qdict(status, "return"),
                                      "running"));
        qobject_unref(status);
        for (unsigned int node = 0; node < 2; node++) {
            uint64_t base = HUB_BY_NODE + node * NODE_STRIDE;

            for (unsigned int bit = PUBLISH_BIT; bit <= STALE_FAIL_BIT; bit++) {
                qtest_writeq(qts, base + PI_INT_PEND_MOD, bit);
            }
            g_assert_cmphex(qtest_readq(qts, base + PI_INT_PEND0) &
                            OWNED_BITS, ==, 0);
            qtest_writeq(qts, base + NI_SCRATCH_REG0,
                         0x13579bdf24680000ULL + phase * 0x100 + node);
            qtest_writeq(qts, base + NI_SCRATCH_REG1, 0);
        }
        qtest_qmp_assert_success(qts, "{'execute':'cont'}");
        wait_for_scratch_publish(qts, phase);
        g_test_message("phase %u: both nodes published and acknowledged",
                       phase);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/warm-reset/four-cpu-rendezvous",
                   test_warm_reset_rendezvous);
    qtest_add_data_func("sn0/warm-reset/scratch-publish-single", "single",
                       test_scratch_publish);
    qtest_add_data_func("sn0/warm-reset/scratch-publish-multi", "multi",
                       test_scratch_publish);
    return g_test_run();
}
