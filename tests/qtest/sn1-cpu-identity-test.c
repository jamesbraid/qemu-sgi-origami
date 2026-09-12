/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 processor identity, executed by the processors themselves.
 *
 * The processor-interface CPU number register reports the reading
 * processor's slot in bit 0. That cannot be checked from a qtest: every
 * qtest access is issued through the first processor's address space with
 * no current processor set, so the register always takes its
 * no-issuing-processor path and an assertion about the second processor
 * would pass against any implementation. The check has to be made by guest
 * code running on the processor whose identity is in question.
 *
 * Both processors enter at the reset vector and run the same instructions,
 * so telling themselves apart is the whole test. Each publishes into the
 * real-time compare register its slot owns, which is what the stock
 * firmware does and what failed when both believed they were slot A.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define PROM_SIZE 0x1686a8

#define SPACE_XTALK UINT64_C(0x20000000000)
#define BEDROCK_LOCAL (SPACE_XTALK + UINT64_C(0x01000000))
#define PI_CPU_ENABLE_B UINT64_C(0x58)
#define PI_RT_COMPARE_A UINT64_C(0x108)
#define PI_RT_COMPARE_B UINT64_C(0x110)

/* What each slot writes, so a swap or a shared write is visible. */
#define SLOT_A_MARK 0xa
#define SLOT_B_MARK 0xb

#define POLL_TRIES 200
#define POLL_INTERVAL_US 10000

/*
 * Guest code. Register t0 holds an uncached pointer to the local processor
 * interface, t2 the slot, t3 the value published.
 *
 * The window a reference is meant for does not live in the physical address
 * on this platform: an uncached reference carries it in the attribute bits
 * above the address, and the memory system lifts that into a window. So the
 * pointer is the crosstalk attribute plus the register offset, not a
 * forty-two bit physical address, and no TLB entry is needed. Sixty-four
 * bit kernel addressing has to be enabled first, which is why the code
 * begins in the status register.
 */
static char *create_identity_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    stl_be_p(image + 0x004, 0x00000000); /* nop */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* beq zero, zero, self */
    }

    /* 64-bit kernel addressing, without which the pointer below faults. */
    stl_be_p(image + 0x400, 0x400c6000); /* mfc0 t4, Status */
    stl_be_p(image + 0x404, 0x358c0080); /* ori t4, t4, Status.KX */
    stl_be_p(image + 0x408, 0x408c6000); /* mtc0 t4, Status */
    stl_be_p(image + 0x40c, 0x00000000); /* nop */

    /* Uncached, crosstalk attribute, register offset. */
    stl_be_p(image + 0x410, 0x3c089200); /* lui t0, 0x9200 */
    stl_be_p(image + 0x414, 0x0008403c); /* dsll32 t0, t0, 0 */
    stl_be_p(image + 0x418, 0x3c090100); /* lui t1, 0x0100 */
    stl_be_p(image + 0x41c, 0x01094025); /* or t0, t0, t1 */

    stl_be_p(image + 0x420, 0xdd0a0020); /* ld t2, 0x20(t0) -- CPU number */
    stl_be_p(image + 0x424, 0x314a0001); /* andi t2, t2, 1 -- the slot bit */
    stl_be_p(image + 0x428, 0x1540000d); /* bne t2, zero, 0x460 -- slot B */
    stl_be_p(image + 0x42c, 0x00000000); /* nop */

    /* Slot A publishes its own mark without enabling slot B first. */
    stl_be_p(image + 0x430, 0x340b000a); /* ori t3, zero, SLOT_A_MARK */
    stl_be_p(image + 0x434, 0xfd0b0108); /* sd t3, 0x108(t0) -- compare A */
    stl_be_p(image + 0x438, 0x00000000); /* nop */
    stl_be_p(image + 0x43c, 0x00000000); /* nop */
    stl_be_p(image + 0x440, 0x1000ffff); /* beq zero, zero, self */
    stl_be_p(image + 0x444, 0x00000000); /* nop */

    /* Slot B keeps publishing so the host can observe continued execution. */
    stl_be_p(image + 0x460, 0x340b000b); /* ori t3, zero, SLOT_B_MARK */
    stl_be_p(image + 0x464, 0xfd0b0110); /* sd t3, 0x110(t0) -- compare B */
    stl_be_p(image + 0x468, 0x256b0001); /* addiu t3, t3, 1 */
    stl_be_p(image + 0x46c, 0x00000000); /* nop */
    stl_be_p(image + 0x470, 0x1000fffc); /* beq zero, zero, 0x464 */
    stl_be_p(image + 0x474, 0x00000000); /* nop */

    fd = g_file_open_tmp("sn1-cpu-identity-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/*
 * With a shared slot bit both processors take the slot-A path, compare B
 * is never written, and this times out rather than reporting the wrong
 * value -- which is the failure the stock firmware showed.
 */
static void test_each_processor_knows_its_slot(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    QTestState *s;
    uint64_t compare_a = 0;
    uint64_t compare_b = 0;
    uint64_t compare_b_after = 0;

    /* Guest code has to run, so the accelerator has to be a real one. */
    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    prom = create_identity_prom();
    quoted = g_shell_quote(prom);
    /*
     * The accelerator named here wins over the -accel qtest libqtest
     * appends, and both processors need their own thread to make progress
     * independently.
     */
    s = qtest_initf("-M origin300 -cpu R14000 -smp 2 -bios %s "
                    "-accel tcg,thread=multi -no-shutdown -S", quoted);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");

    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        compare_a = qtest_readq(s, BEDROCK_LOCAL + PI_RT_COMPARE_A);
        compare_b = qtest_readq(s, BEDROCK_LOCAL + PI_RT_COMPARE_B);
        if (compare_a && compare_b) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }

    /* Each processor reached its own register without a CPU_ENABLE write. */
    g_assert_cmphex(compare_a, ==, SLOT_A_MARK);
    g_assert_cmphex(compare_b, >=, SLOT_B_MARK);

    qtest_writeq(s, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    g_usleep(10 * POLL_INTERVAL_US);
    compare_b = qtest_readq(s, BEDROCK_LOCAL + PI_RT_COMPARE_B);
    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        compare_b_after = qtest_readq(s, BEDROCK_LOCAL + PI_RT_COMPARE_B);
        if (compare_b_after != compare_b) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(compare_b_after, !=, compare_b);

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/cpu-identity/each-processor-knows-its-slot",
                   test_each_processor_knows_its_slot);
    return g_test_run();
}
