/*
 * The Hub's and the Bridge's programmed-I/O forwarding remembers which
 * region a page of its target space resolved to (hw/misc/sgi-pio-forward.c).
 * These tests remap the target between two accesses to the same address
 * and expect the second to follow the new mapping: a Bridge cycle after the
 * device's base address register moves, and a processor store after the
 * Hub's big window is re-aimed.
 *
 * A PsiTech RAD4's BAR2 is the target: plain readable words, so where a
 * store landed can be read back.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sn0-user-image.h"
#include "sgi-test.h"
#include "sgi-wait.h"

#define BRIDGE_WIDGET       8
#define BRIDGE_BASE         (SPACE_IO + ((uint64_t)BRIDGE_WIDGET << 24))
#define REG_DATA            4
#define REG(off)            (BRIDGE_BASE + (off) + REG_DATA)
#define BRIDGE_DEVICE(n)    REG(0x200 + 8 * (n))
#define BRIDGE_INT_STATUS   0x100
#define BRIDGE_INT_ENABLE   0x108
#define INT_MASTER_TIMEOUT  (1u << 11)
#define CFG0(dev)           (BRIDGE_BASE + 0x20000 + (dev) * 0x1000)

#define PCI_COMMAND_OFF     0x04
#define PCI_BAR0_OFF        0x10
#define PCI_BAR2_OFF        0x18

/*
 * Slot 3 is the Origin 200's first free slot, with the first 1 MiB device
 * window at widget offset 0x700000 (tests/qtest/psitech-rad4-test.c).
 */
#define RAD4_SLOT           3
#define WINDOW_OFFSET       0x700000u
#define WINDOW_SIZE         0x100000u
#define RAD4_BAR0_PCI       0x20000000u
#define RAD4_BAR2_PCI       0x30000000u
#define RAD4_BAR2_MOVED     0x31000000u     /* BAR2 is 16 MiB */

#define RAD4                "-device psitech-rad4,addr=3 "
/* An executing processor, as tests/qtest/sn0-pi-gfx-test.c runs one. */
#define RUNNING \
    "-M origin200,x-uncatalogued-graphics=on -m 64M -smp 2 -cpu R10000 " \
    "-accel tcg,thread=multi " \
    "-no-reboot -no-shutdown -S " RAD4

/* Big windows and their table, as tests/qtest/sn0-widget-decode-test.c. */
#define BIG_WINDOW_SIZE     0x20000000ULL
#define BIG_WINDOW(n)       (0x20000000ULL + (n) * BIG_WINDOW_SIZE)
#define IO_BWIN_ENTRY(n)    (0x400160 + (n) * 8)
#define IO_BWIN_WIDGET(w)   ((uint64_t)(w) << 8)
#define TEST_WINDOW         1
/*
 * An entry selecting I/O space: the model maps that for window 0 alone, so
 * window 1 then reaches nothing. An absent widget would not do: the Origin
 * 200's Bridge answers at every widget number but the Hub's.
 */
#define IO_BWIN_SPACE       (1u << 12)

/* A processor's uncached I/O-space address. */
#define UNCACHED_IO         UINT64_C(0x9200000000000000)

#define MAILBOX             0x50000u
#define ENTRY               0x800

/* Point the slot's window at the 1 MiB of PCI space holding @pci. */
static void window_to(QTestState *qts, uint32_t pci)
{
    qtest_writel(qts, BRIDGE_DEVICE(RAD4_SLOT),
                 DEV_MEMORY_SPACE | ((pci & ~(WINDOW_SIZE - 1)) >> 20));
}

static uint64_t window(uint32_t pci)
{
    return BRIDGE_BASE + WINDOW_OFFSET + (pci & (WINDOW_SIZE - 1));
}

static void configure_rad4(QTestState *qts, uint32_t bar2)
{
    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_BAR0_OFF, RAD4_BAR0_PCI);
    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_BAR2_OFF, bar2);
    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_COMMAND_OFF, PCI_COMMAND_MEMORY);
}

/*
 * The same window address before and after BAR2 moves away: the first
 * write lands in the framebuffer, the second reaches no device and ends in
 * the Bridge's master time-out, and the framebuffer, read at BAR2's new
 * address, keeps the first value.
 */
static void test_bridge_follows_a_moved_bar(void)
{
    QTestState *qts = qtest_init("-M origin200,x-uncatalogued-graphics=on "
                                 RAD4);

    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), INT_MASTER_TIMEOUT);
    configure_rad4(qts, RAD4_BAR2_PCI);
    window_to(qts, RAD4_BAR2_PCI);

    qtest_writel(qts, window(RAD4_BAR2_PCI), 0x00112233u);
    g_assert_cmphex(qtest_readl(qts, window(RAD4_BAR2_PCI)), ==,
                    0x00112233u);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) &
                    INT_MASTER_TIMEOUT, ==, 0);

    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_BAR2_OFF, RAD4_BAR2_MOVED);
    qtest_writel(qts, window(RAD4_BAR2_PCI), 0x00445566u);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) &
                    INT_MASTER_TIMEOUT, ==, INT_MASTER_TIMEOUT);

    window_to(qts, RAD4_BAR2_MOVED);
    g_assert_cmphex(qtest_readl(qts, window(RAD4_BAR2_MOVED)), ==,
                    0x00112233u);
    qtest_writel(qts, window(RAD4_BAR2_MOVED), 0x00778899u);
    g_assert_cmphex(qtest_readl(qts, window(RAD4_BAR2_MOVED)), ==,
                    0x00778899u);

    qtest_quit(qts);
}

/*
 * Slice 0 stores the word in its mailbox, once, at the address in its
 * mailbox, then clears the count. Both slices start this image at reset,
 * so slice 1 parks: two processors serving the one mailbox could each
 * make the store, and one that read a request before the test re-aimed
 * the window could make it after.
 */
static void build_store_server(SN0Image *p)
{
    unsigned int at, loop, slice1;

    at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    sn0_patch_branch(p, at, ENTRY);
    for (unsigned int vec = SN0_VEC_TLB_REFILL; vec <= SN0_VEC_GENERAL;
         vec += 0x80) {
        p->pos = vec;
        sn0_emit_park(p);
    }
    p->pos = ENTRY;
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_cpu_number(p, SN0_R_T2);
    slice1 = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T2, SN0_R_ZERO);
    sn0_emit_li(p, SN0_R_S0, SN0_KSEG1(MAILBOX));

    loop = p->pos;
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T1, 0);        /* count */
    at = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_T1, SN0_R_ZERO);
    sn0_patch_branch(p, at, loop);
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T2, 8);        /* address */
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T8, 16);       /* word */
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_T2, SN0_R_T8, 0);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_ZERO, 0);      /* done */
    at = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, at, loop);

    sn0_patch_branch(p, slice1, p->pos);
    sn0_emit_park(p);
}

/* Slice 0 stores @word at processor address @addr. */
static void guest_store(QTestState *qts, uint64_t addr, uint32_t word)
{
    SgiWait w;

    qtest_writeq(qts, MAILBOX + 16, word);
    qtest_writeq(qts, MAILBOX + 8, addr);
    qtest_writeq(qts, MAILBOX, 1);
    sgi_wait_start(&w, "guest store");
    while (qtest_readq(qts, MAILBOX)) {
        sgi_wait_pause(&w, 1000);
    }
}

static void aim_big_window(QTestState *qts, uint64_t entry)
{
    qtest_writeq(qts, HUB_LOCAL + IO_BWIN_ENTRY(TEST_WINDOW), entry);
}

/*
 * The same processor address before and after the Hub's big window is
 * re-aimed: through the Bridge the store lands in the framebuffer, with the
 * window turned off it reaches nothing, and aimed back it lands again.
 */
static void test_hub_follows_a_reaimed_big_window(void)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    uint64_t store = UNCACHED_IO + BIG_WINDOW(TEST_WINDOW) + WINDOW_OFFSET +
                     (RAD4_BAR2_PCI & (WINDOW_SIZE - 1));
    QTestState *qts;

    build_store_server(image);
    path = sn0_image_write(image, "sn0-pio-forward");
    qts = qtest_initf(RUNNING "-bios %s", path);
    unlink(path);

    configure_rad4(qts, RAD4_BAR2_PCI);
    window_to(qts, RAD4_BAR2_PCI);
    aim_big_window(qts, IO_BWIN_WIDGET(BRIDGE_WIDGET));
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    guest_store(qts, store, 0x00112233u);
    g_assert_cmphex(qtest_readl(qts, window(RAD4_BAR2_PCI)), ==,
                    0x00112233u);

    aim_big_window(qts, IO_BWIN_SPACE | IO_BWIN_WIDGET(BRIDGE_WIDGET));
    guest_store(qts, store, 0x00445566u);
    g_assert_cmphex(qtest_readl(qts, window(RAD4_BAR2_PCI)), ==,
                    0x00112233u);

    aim_big_window(qts, IO_BWIN_WIDGET(BRIDGE_WIDGET));
    guest_store(qts, store, 0x00778899u);
    g_assert_cmphex(qtest_readl(qts, window(RAD4_BAR2_PCI)), ==,
                    0x00778899u);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0-pio-forward/bridge-follows-a-moved-bar",
                   test_bridge_follows_a_moved_bar);
    qtest_add_func("/sn0-pio-forward/hub-follows-a-reaimed-big-window",
                   test_hub_follows_a_reaimed_big_window);
    return g_test_run();
}
