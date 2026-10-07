/* SPDX-License-Identifier: BSD-3-Clause */
/* R10000 data-cache visibility against an external physical-RAM observer. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sn0-user-image.h"

#define ENTRY       0x800
#define FAULT_ENTRY 0x400
#define DATA_A      0x00200000u
#define DATA_B      (DATA_A + 0x100)
#define RESULT      0x00201000u
#define RESULT_GUEST SN0_KSEG1(RESULT)
#define CACHED_A    (UINT64_C(0x9800000000000000) | DATA_A)
#define PHASE       0x00
#define COMMAND     0x04
#define FAULT       0x08
#define CAUSE       0x0c
#define EPC         0x10
#define READ_1      0x18
#define READ_2      0x1c
#define READ_3      0x20
#define READ_4      0x24
#define READ_5      0x28
#define FAULT_MAGIC 0xdead

#define OLD_A       0x11223344u
#define NEW_A       0x55667788u
#define OLD_B       0x01020304u
#define NEW_B       0xa1b2c3d4u

static void emit_fault(SN0Image *p)
{
    unsigned int jump = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);

    sn0_patch_branch(p, jump, ENTRY);
    for (unsigned int vec = SN0_VEC_TLB_REFILL;
         vec <= SN0_VEC_GENERAL; vec += 0x80) {
        p->pos = vec;
        jump = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
        sn0_patch_branch(p, jump, FAULT_ENTRY);
    }
    p->pos = FAULT_ENTRY;
    sn0_emit_li(p, SN0_R_K0, RESULT_GUEST);
    sn0_emit_mfc0(p, SN0_R_K1, SN0_CP0_CAUSE);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_K1, CAUSE);
    sn0_emit_mfc0(p, SN0_R_K1, SN0_CP0_EPC);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_K0, SN0_R_K1, EPC);
    sn0_emit_li(p, SN0_R_K1, FAULT_MAGIC);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_K1, FAULT);
    sn0_emit_park(p);
    p->pos = ENTRY;
}

static void emit_read(SN0Image *p, unsigned int offset, unsigned int slot)
{
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_S0, SN0_R_T0, offset);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S1, SN0_R_T0, slot);
}

static void emit_checkpoint(SN0Image *p, unsigned int phase)
{
    unsigned int loop;
    unsigned int again;

    sn0_emit_li(p, SN0_R_T1, phase);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S1, SN0_R_T1, PHASE);
    loop = p->pos;
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_S1, SN0_R_T2, COMMAND);
    again = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T1, SN0_R_T2);
    sn0_patch_branch(p, again, loop);
}

static void emit_cache(SN0Image *p, unsigned int operation,
                       unsigned int offset)
{
    sn0_emit(p, mips_cache(operation, SN0_R_S0, offset));
}

static void build_image(SN0Image *p, unsigned int which)
{
    emit_fault(p);
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li64(p, SN0_R_S0, CACHED_A);
    sn0_emit_li(p, SN0_R_S1, RESULT_GUEST);

    if (which == 1 || which == 3) {
        emit_read(p, 0, READ_1);
        sn0_emit_li(p, SN0_R_T0, NEW_A);
        sn0_emit_i(p, SN0_OPC_SW, SN0_R_S0, SN0_R_T0, 0);
        emit_read(p, 0, READ_2);
        emit_checkpoint(p, 1);
        if (which == 1) {
            emit_cache(p, 0x15, 0);
            emit_read(p, 0, READ_3);
            emit_checkpoint(p, 2);
            emit_cache(p, 0x17, 0);
            emit_read(p, 0, READ_4);
        } else {
            emit_read(p, 0x100, READ_3);
            emit_read(p, 0, READ_4);
            emit_checkpoint(p, 2);
            emit_cache(p, 0x17, 0);
            emit_read(p, 0, READ_5);
        }
    } else {
        emit_read(p, 0x100, READ_1);
        emit_checkpoint(p, 1);
        emit_read(p, 0x100, READ_2);
        emit_checkpoint(p, 2);
        emit_cache(p, 0x13, 0x100);
        emit_read(p, 0x100, READ_3);
    }
    sn0_emit_li(p, SN0_R_T1, 3);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S1, SN0_R_T1, PHASE);
    sn0_emit_park(p);
}

static uint32_t backing_word(QTestState *qts, uint32_t address)
{
    uint8_t bytes[4];

    qtest_memread(qts, address, bytes, sizeof(bytes));
    return ldl_be_p(bytes);
}

static void set_backing_word(QTestState *qts, uint32_t address,
                             uint32_t value)
{
    uint8_t bytes[4];

    stl_be_p(bytes, value);
    qtest_memwrite(qts, address, bytes, sizeof(bytes));
}

static void wait_phase(QTestState *qts, unsigned int expected)
{
    SgiWait wait;

    sgi_wait_start(&wait, "R10000 cache phase %u", expected);
    while (qtest_readl(qts, RESULT + PHASE) != expected) {
        if (qtest_readl(qts, RESULT + FAULT)) {
            g_error("R10000 exception: Cause 0x%08x EPC 0x%016" PRIx64,
                    qtest_readl(qts, RESULT + CAUSE),
                    qtest_readq(qts, RESULT + EPC));
        }
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readl(qts, RESULT + FAULT), ==, 0);
}

static void run_case(unsigned int which)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    build_image(image, which);
    path = sn0_image_write(image, "o2-r10k-cache");
    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000,cache-data-visibility=on "
                      "-bios %s -accel tcg,thread=single -S "
                      "-no-reboot -no-shutdown", path);
    unlink(path);
    set_backing_word(qts, DATA_A, OLD_A);
    set_backing_word(qts, DATA_B, OLD_B);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    wait_phase(qts, 1);
    if (which == 2) {
        g_assert_cmphex(qtest_readl(qts, RESULT + READ_1), ==, OLD_B);
        g_assert_cmphex(backing_word(qts, DATA_B), ==, OLD_B);
        set_backing_word(qts, DATA_B, NEW_B);
        g_assert_cmphex(backing_word(qts, DATA_B), ==, NEW_B);
    } else {
        g_assert_cmphex(qtest_readl(qts, RESULT + READ_1), ==, OLD_A);
        g_assert_cmphex(qtest_readl(qts, RESULT + READ_2), ==, NEW_A);
        g_assert_cmphex(backing_word(qts, DATA_A), ==, OLD_A);
        if (which == 3) {
            set_backing_word(qts, DATA_B, NEW_B);
        }
    }
    qtest_writel(qts, RESULT + COMMAND, 1);

    wait_phase(qts, 2);
    if (which == 1) {
        g_assert_cmphex(qtest_readl(qts, RESULT + READ_3), ==, NEW_A);
        g_assert_cmphex(backing_word(qts, DATA_A), ==, OLD_A);
    } else if (which == 2) {
        g_assert_cmphex(qtest_readl(qts, RESULT + READ_2), ==, OLD_B);
        g_assert_cmphex(backing_word(qts, DATA_B), ==, NEW_B);
    } else {
        g_assert_cmphex(qtest_readl(qts, RESULT + READ_3), ==, NEW_B);
        g_assert_cmphex(qtest_readl(qts, RESULT + READ_4), ==, NEW_A);
        g_assert_cmphex(backing_word(qts, DATA_A), ==, OLD_A);
        g_assert_cmphex(backing_word(qts, DATA_B), ==, NEW_B);
    }
    qtest_writel(qts, RESULT + COMMAND, 2);

    wait_phase(qts, 3);
    if (which == 2) {
        g_assert_cmphex(qtest_readl(qts, RESULT + READ_3), ==, NEW_B);
        g_assert_cmphex(backing_word(qts, DATA_B), ==, NEW_B);
    } else {
        g_assert_cmphex(backing_word(qts, DATA_A), ==, NEW_A);
        g_assert_cmphex(qtest_readl(qts, RESULT + READ_2), ==, NEW_A);
        if (which == 1) {
            g_assert_cmphex(qtest_readl(qts, RESULT + READ_4), ==, NEW_A);
        } else {
            g_assert_cmphex(qtest_readl(qts, RESULT + READ_5), ==, NEW_A);
            g_assert_cmphex(backing_word(qts, DATA_B), ==, NEW_B);
        }
    }
    qtest_quit(qts);
}

static void test_dirty_data(void) { run_case(1); }
static void test_external_write(void) { run_case(2); }
static void test_two_lines(void) { run_case(3); }

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/o2/r10k-cache/dirty-data", test_dirty_data);
    qtest_add_func("/o2/r10k-cache/external-write", test_external_write);
    qtest_add_func("/o2/r10k-cache/two-lines", test_two_lines);
    return g_test_run();
}
