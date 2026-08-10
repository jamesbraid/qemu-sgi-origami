/*
 * SGI SN0 Hub ASIC — the registers a guest reads before it has a console.
 *
 * Two of these decide things a guest cannot recover from. It reads the
 * processor-number register to learn which of a node's two processors it is,
 * and the network-interface status register to learn whether nodes are numbered
 * in M mode or N mode, which sets the shift it forms every node-relative
 * address with afterwards. Both have to read zero on an unprogrammed Origin
 * 200, and a model that invented a different default would send the guest down
 * a path it announces in one word of output and gets wrong everywhere
 * underneath.
 *
 * The window is reached twice: once at an address that resolves to whichever
 * node the reading processor is on, which is how firmware reaches its own Hub
 * before node numbers are assigned, and once at an address naming a node. Both
 * have to decode, because a guest uses one to read its processor number and the
 * other to clear interrupt state in the same boot.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Crosstalk space, and the two halves of the crossbar's widget 1 window. */
#define SPACE_XTALK     0x20000000000ULL
#define HUB_LOCAL       (SPACE_XTALK + 0x01000000)
#define HUB_BY_NODE     (SPACE_XTALK + 0x01800000)

#define PI_REGION_PRESENT   0x000018
#define PI_PROCESSOR_NUMBER 0x000020
#define PI_CALIAS_SIZE      0x000028
#define PI_INT_PEND0        0x000098
#define PI_INT_PEND1        0x0000a0
#define PI_INT_MASK0_A      0x0000a8
#define PI_INT_MASK1_A      0x0000b0
#define PI_INT_MASK0_B      0x0000b8
#define PI_INT_MASK1_B      0x0000c0
#define NI_STATUS           0x600000

#define NI_MORE_NODES   (1ULL << 18)
#define NI_REGION_SIZE  (1ULL << 17)
#define NI_NODE_MASK    (0x1ffULL << 8)
#define NI_IDENTITY     0xffULL          /* revision and chip identity */

static void test_addressing_mode(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t status = qtest_readq(qts, HUB_LOCAL + NI_STATUS);

    /*
     * Zero in the more-nodes bit is M mode. One would make the guest print that
     * it is in N mode and compute every node-relative address with a different
     * shift.
     */
    g_assert_cmphex(status & NI_MORE_NODES, ==, 0);

    /*
     * This Hub is node zero. OpenBSD does not read this field, but Linux takes
     * its node number from it, so it has to be right for both.
     */
    g_assert_cmphex(status & NI_NODE_MASK, ==, 0);

    /* One region per node, which is what the field resets to. */
    g_assert_cmphex(status & NI_REGION_SIZE, ==, NI_REGION_SIZE);

    qtest_quit(qts);
}

static void test_processor_number(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The first processor of the node. Everything per-processor is selected
     * from this.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_PROCESSOR_NUMBER), ==, 0);

    qtest_quit(qts);
}

static void test_reset_values(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /* An unprogrammed Hub claims every memory region exists. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_REGION_PRESENT), ==,
                    ~0ULL);

    /* The cached alias resets to its largest size code. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CALIAS_SIZE), ==, 15);

    /* Nothing is pending before anything is wired up. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND1), ==, 0);

    qtest_quit(qts);
}

static void test_writes_the_guest_makes(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    const uint64_t masks[] = {
        PI_INT_MASK0_A, PI_INT_MASK1_A, PI_INT_MASK0_B, PI_INT_MASK1_B,
    };
    unsigned i;

    /*
     * The four interrupt masks: a guest writes zero to all of them during setup
     * and later writes real masks, so they have to remember what was written.
     * The IRIX register table calls them read-only, which the Hub datasheet
     * records as wrong; a read-only model would silently drop every interrupt a
     * guest tried to enable.
     */
    for (i = 0; i < ARRAY_SIZE(masks); i++) {
        qtest_writeq(qts, HUB_LOCAL + masks[i], 0);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + masks[i]), ==, 0);

        qtest_writeq(qts, HUB_LOCAL + masks[i], 0x0123456789abcdefULL);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + masks[i]), ==,
                        0x0123456789abcdefULL);
    }

    /* A guest writes all ones here, and zero to the alias size. */
    qtest_writeq(qts, HUB_LOCAL + PI_REGION_PRESENT, ~0ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_REGION_PRESENT), ==,
                    ~0ULL);

    qtest_writeq(qts, HUB_LOCAL + PI_CALIAS_SIZE, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_CALIAS_SIZE), ==, 0);

    qtest_quit(qts);
}

static void test_status_field_access(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t status;

    /*
     * Firmware assigns node numbers through this register, so the field is
     * writable.
     */
    qtest_writeq(qts, HUB_LOCAL + NI_STATUS, NI_NODE_MASK);
    status = qtest_readq(qts, HUB_LOCAL + NI_STATUS);
    g_assert_cmphex(status & NI_NODE_MASK, ==, NI_NODE_MASK);

    /* What the hardware reports about itself is not writable. */
    g_assert_cmphex(status & NI_IDENTITY, ==, 2 << 4);

    qtest_quit(qts);
}

static void test_both_windows(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * The same Hub either way on a single-node machine, and both halves have to
     * decode. A write through one has to be visible through the other, which is
     * what says they are one device rather than two.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + NI_STATUS), ==,
                    qtest_readq(qts, HUB_LOCAL + NI_STATUS));
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + PI_PROCESSOR_NUMBER), ==,
                    0);

    qtest_writeq(qts, HUB_BY_NODE + PI_INT_MASK0_A, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_MASK0_A), ==,
                    0xfedcba9876543210ULL);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/hub/addressing-mode", NULL, test_addressing_mode);
    qtest_add_data_func("sn0/hub/processor-number", NULL,
                        test_processor_number);
    qtest_add_data_func("sn0/hub/reset-values", NULL, test_reset_values);
    qtest_add_data_func("sn0/hub/guest-writes", NULL,
                        test_writes_the_guest_makes);
    qtest_add_data_func("sn0/hub/status-field-access", NULL,
                        test_status_field_access);
    qtest_add_data_func("sn0/hub/both-windows", NULL, test_both_windows);

    return g_test_run();
}
