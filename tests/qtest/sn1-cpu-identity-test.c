/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 CPU identity, checked by code running on each processor. Each
 * publishes its physical slice to its own real-time compare register.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"

#define BEDROCK_LOCAL (SPACE_IO + UINT64_C(0x01000000))
#define BEDROCK_PI1 UINT64_C(0x200000)
#define PI_CPU_ENABLE_A UINT64_C(0x50)
#define PI_RT_COMPARE_A UINT64_C(0x108)
#define PI_RT_COMPARE_STRIDE UINT64_C(0x8)

/* What each slot writes, so a swap or a shared write is visible. */
#define SLOT_A_MARK 0xa
#define SLOT_C_MARK 0xc

/*
 * The four-processor marks. Bits 1:0 are the slice the publishing processor
 * read out of the CPU number register, so a processor that misreads its own
 * slot writes into another slot's register and leaves its own at zero.
 */
#define SLOT_MARK_BASE 0xa0

/* Both slots of both processor interfaces. */
#define SGI_SLICES 4

#define POLL_TRIES 200
#define POLL_INTERVAL_US 10000

/*
 * The CPU number register reports the reader's physical slice in bits 1:0.
 * qtest uses CPU 0's address space without a current CPU, taking the
 * no-issuing-processor path; it cannot test another CPU's identity. Both CPUs
 * enter at the reset vector and run identical code, so each must read its own
 * identity. t0 points uncached to the local processor interface; t2 is the
 * slot and t3 the published value.
 *
 * The window a reference is meant for does not live in the physical address
 * on this platform: an uncached reference carries it in the attribute bits
 * above the address, and the memory system lifts that into a window. So the
 * pointer is the crosstalk attribute plus the register offset, not a
 * forty-two bit physical address, and no TLB entry is needed. Sixty-four bit
 * kernel addressing has to be enabled first, which is why the code begins in
 * the status register.
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
    stl_be_p(image + 0x424, 0x314a0002); /* andi t2, t2, 2 -- the PI bit */
    stl_be_p(image + 0x428, 0x1540000d); /* bne t2, zero, 0x460 -- slice C */
    stl_be_p(image + 0x42c, 0x00000000); /* nop */

    /* Slice A publishes its own mark without enabling C first. */
    stl_be_p(image + 0x430, 0x340b000a); /* ori t3, zero, SLOT_A_MARK */
    stl_be_p(image + 0x434, 0xfd0b0108); /* sd t3, 0x108(t0) -- compare A */
    stl_be_p(image + 0x438, 0x00000000); /* nop */
    stl_be_p(image + 0x43c, 0x00000000); /* nop */
    stl_be_p(image + 0x440, 0x1000ffff); /* beq zero, zero, self */
    stl_be_p(image + 0x444, 0x00000000); /* nop */

    /*
     * Slice C keeps publishing so the host can observe continued execution.
     */
    stl_be_p(image + 0x460, 0x340b000c); /* ori t3, zero, SLOT_C_MARK */
    stl_be_p(image + 0x464, 0xfd0b0108); /* sd t3, 0x108(t0) -- PI1 compare A */
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
 * With a shared PI bit both processors take the slice-A path and the PI1
 * compare register remains untouched, making this check time out.
 */
static void test_each_processor_knows_its_slot(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    QTestState *s;
    uint64_t compare_a = 0;
    uint64_t compare_c = 0;
    uint64_t compare_c_after = 0;

    /* Guest code has to run, so the accelerator has to be a real one. */
    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    prom = create_identity_prom();
    quoted = g_shell_quote(prom);
    /*
     * The accelerator named here wins over the -accel qtest libqtest appends,
     * and both processors need their own thread to make progress
     * independently.
     */
    s = qtest_initf("-M origin300 -cpu R14000 -smp 2 -bios %s "
                    "-accel tcg,thread=multi -no-shutdown -S", quoted);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");

    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        compare_a = qtest_readq(s, BEDROCK_LOCAL + PI_RT_COMPARE_A);
        compare_c = qtest_readq(s, BEDROCK_LOCAL + BEDROCK_PI1 +
                                  PI_RT_COMPARE_A);
        if (compare_a && compare_c) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }

    /* Each processor reached its own register without a CPU_ENABLE write. */
    g_assert_cmphex(compare_a, ==, SLOT_A_MARK);
    g_assert_cmphex(compare_c, >=, SLOT_C_MARK);

    qtest_writeq(s, BEDROCK_LOCAL + BEDROCK_PI1 + PI_CPU_ENABLE_A, 1);
    g_assert_cmphex(qtest_readq(s, BEDROCK_LOCAL + BEDROCK_PI1 +
                                PI_CPU_ENABLE_A), ==, 1);
    g_usleep(10 * POLL_INTERVAL_US);
    compare_c = qtest_readq(s, BEDROCK_LOCAL + BEDROCK_PI1 +
                              PI_RT_COMPARE_A);
    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        compare_c_after = qtest_readq(s, BEDROCK_LOCAL + BEDROCK_PI1 +
                                        PI_RT_COMPARE_A);
        if (compare_c_after != compare_c) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(compare_c_after, !=, compare_c);

    qtest_writeq(s, BEDROCK_LOCAL + BEDROCK_PI1 + PI_CPU_ENABLE_A, 0);
    g_assert_cmphex(qtest_readq(s, BEDROCK_LOCAL + BEDROCK_PI1 +
                                PI_CPU_ENABLE_A), ==, 0);
    g_usleep(10 * POLL_INTERVAL_US);
    compare_c = qtest_readq(s, BEDROCK_LOCAL + BEDROCK_PI1 +
                              PI_RT_COMPARE_A);
    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        compare_c_after = qtest_readq(s, BEDROCK_LOCAL + BEDROCK_PI1 +
                                        PI_RT_COMPARE_A);
        if (compare_c_after != compare_c) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(compare_c_after, !=, compare_c);

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * Guest code for the four-processor population. Every processor runs the same
 * instructions and has to find its own slice: it reads the CPU number
 * register through the crosstalk local form, which resolves to the
 * requester's own processor interface, and publishes a slice-stamped mark
 * into the real-time compare register that interface keeps for its own slot.
 * Four distinct registers end up holding four distinct marks only if every
 * processor read its own slot correctly.
 */
static char *create_four_slot_identity_prom(void)
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
    stl_be_p(image + 0x424, 0x314a0003); /* andi t2, t2, 3 -- the slice */
    stl_be_p(image + 0x428, 0x354b00a0); /* ori t3, t2, SLOT_MARK_BASE */
    stl_be_p(image + 0x42c, 0x314c0001); /* andi t4, t2, 1 -- slot in PI */
    stl_be_p(image + 0x430, 0x000c60f8); /* dsll t4, t4, 3 -- compare stride */
    stl_be_p(image + 0x434, 0x010c602d); /* daddu t4, t0, t4 */
    stl_be_p(image + 0x438, 0xfd8b0108); /* sd t3, 0x108(t4) -- own compare */
    stl_be_p(image + 0x43c, 0x00000000); /* nop */
    stl_be_p(image + 0x440, 0x1000ffff); /* beq zero, zero, self */
    stl_be_p(image + 0x444, 0x00000000); /* nop */

    fd = g_file_open_tmp("sn1-four-slot-identity-prom-XXXXXX.bin", &path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/*
 * The four-processor population fills both slots of both interfaces. A
 * processor that reads a slot the machine did not put it in publishes into a
 * register another processor owns and leaves its own at zero, which is what a
 * second processor reporting slice A looks like from outside: two boot
 * masters in the stock PROM.
 */
static void test_four_processors_know_their_slots(void)
{
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    QTestState *s;
    uint64_t published[SGI_SLICES] = { 0 };
    uint64_t address[SGI_SLICES] = {
        BEDROCK_LOCAL + PI_RT_COMPARE_A,
        BEDROCK_LOCAL + PI_RT_COMPARE_A + PI_RT_COMPARE_STRIDE,
        BEDROCK_LOCAL + BEDROCK_PI1 + PI_RT_COMPARE_A,
        BEDROCK_LOCAL + BEDROCK_PI1 + PI_RT_COMPARE_A + PI_RT_COMPARE_STRIDE,
    };

    /* Guest code has to run, so the accelerator has to be a real one. */
    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    prom = create_four_slot_identity_prom();
    quoted = g_shell_quote(prom);
    s = qtest_initf("-M origin300 -cpu R14000 -smp 4 -bios %s "
                    "-accel tcg,thread=multi -no-shutdown -S", quoted);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");

    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        unsigned int seen = 0;

        for (unsigned int slice = 0; slice < SGI_SLICES; slice++) {
            published[slice] = qtest_readq(s, address[slice]);
            seen += published[slice] != 0;
        }
        if (seen == SGI_SLICES) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }

    for (unsigned int slice = 0; slice < SGI_SLICES; slice++) {
        g_assert_cmphex(published[slice], ==, SLOT_MARK_BASE | slice);
    }

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/cpu-identity/each-processor-knows-its-slot",
                   test_each_processor_knows_its_slot);
    qtest_add_func("sn1/cpu-identity/four-processors-know-their-slots",
                   test_four_processors_know_their_slots);
    return g_test_run();
}
