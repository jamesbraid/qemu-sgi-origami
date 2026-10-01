/*
 * Unclaimed Bridge PCI configuration cycles set PCI_MASTER_TOUT and latch
 * the PCI address and device number (Bridge specification rev 4.0, section
 * 6.4.1, table 57). Empty-slot reads by IP27 PROM, IRIX, Linux and OpenBSD
 * also fail at the CPU inside a bus-error guard; an executing CPU tests it.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sn0-user-image.h"
#include "sgi-test.h"

#define BRIDGE_WIDGET       8
#define BRIDGE_BASE         (SPACE_IO + ((uint64_t)BRIDGE_WIDGET << 24))

/* Registers are 32 bits in the low half of an eight-byte slot. */
#define REG_DATA            4
#define REG(offset)         (BRIDGE_BASE + (offset) + REG_DATA)

#define BRIDGE_INT_DEST_UP  0x030
#define BRIDGE_INT_DEST_LO  0x038
/*
 * The destination firmware programs (PROM 6.156 at 0x1fc44700): the Hub's
 * widget in bits 19:16, and the Hub's interrupt-modify register in the XIO
 * I/O view, bit 47 set with the node at bit 38.
 */
#define HUB_XIO_WIDGET          1
#define HUB_XIO_INT_MODIFY      0x01800090u
#define HUB_XIO_DEST_UPPER(node) \
    ((HUB_XIO_WIDGET << 16) | 0x8000u | ((node) << (38 - 32)))
#define BRIDGE_PCI_ERR_UP   0x0d0
#define BRIDGE_PCI_ERR_LO   0x0d8
#define BRIDGE_INT_STATUS   0x100
#define BRIDGE_INT_ENABLE   0x108
#define BRIDGE_INT_RESET    0x110
#define BRIDGE_HOST_ERR     0x128

#define INT_MASTER_TIMEOUT  (1u << 11)
#define INT_MULTI_ERR       (1u << 31)
#define INT_RESET_PCI_GRP   (1u << 0)
#define INT_RESET_MULTI     (1u << 6)

/* PCI error upper address: device number at 18:16, PCI address 47:32 below. */
#define PCI_ERR_DEVICE(n)   ((uint32_t)(n) << 16)

/* Type 0 configuration: 4 KB per device, 256 bytes per function. */
#define CFG0(dev, fn)       (BRIDGE_BASE + 0x20000 + (dev) * 0x1000 + \
                             (fn) * 0x100)

/*
 * The configuration cycle's address: the device's ID select line (table
 * 61: device n drives address line 24 + n), the function and the dword.
 */
#define IDSEL(dev)          (1u << (24 + (dev)))

/* The Origin 200's base bus: two SCSI adapters and the IOC3 in 0-2. */
#define PRESENT_SLOT        0
#define EMPTY_SLOT          7
#define OTHER_EMPTY_SLOT    5
#define NO_CARD             0xffffffffu

#define PI_INT_MODIFY       0x90
#define ERR_HOST_BIT        5u

#define MACHINE             "-M origin200"

static void test_empty_slot_latches_master_timeout(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), INT_MASTER_TIMEOUT);

    /* A claimed cycle is not an error. */
    g_assert_cmphex(qtest_readl(qts, CFG0(PRESENT_SLOT, 0)), !=, NO_CARD);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) &
                    INT_MASTER_TIMEOUT, ==, 0);

    /* The data half still floats high; the status is what changes. */
    g_assert_cmphex(qtest_readl(qts, CFG0(EMPTY_SLOT, 0) + 0x8), ==, NO_CARD);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) &
                    INT_MASTER_TIMEOUT, ==, INT_MASTER_TIMEOUT);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_PCI_ERR_LO)), ==,
                    IDSEL(EMPTY_SLOT) | 0x8);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_PCI_ERR_UP)), ==,
                    PCI_ERR_DEVICE(EMPTY_SLOT));

    /* The PCI group's strobe clears it. */
    qtest_writel(qts, REG(BRIDGE_INT_RESET), INT_RESET_PCI_GRP);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) &
                    INT_MASTER_TIMEOUT, ==, 0);

    qtest_quit(qts);
}

static void test_empty_slot_write_latches_master_timeout(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), INT_MASTER_TIMEOUT);

    /* A write is a cycle too, and nobody answers it either. */
    qtest_writel(qts, CFG0(EMPTY_SLOT, 1) + 0x10, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) &
                    INT_MASTER_TIMEOUT, ==, INT_MASTER_TIMEOUT);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_PCI_ERR_LO)), ==,
                    IDSEL(EMPTY_SLOT) | (1 << 8) | 0x10);

    qtest_quit(qts);
}

/*
 * Chapter 6: a status bit is set only when its enable bit is. Every guest
 * probes with the time-out masked, so a model that latched regardless
 * would report an error no real Bridge records.
 */
static void test_master_timeout_needs_its_enable(void)
{
    QTestState *qts = qtest_init(MACHINE);

    g_assert_cmphex(qtest_readl(qts, CFG0(EMPTY_SLOT, 0)), ==, NO_CARD);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) &
                    INT_MASTER_TIMEOUT, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_PCI_ERR_LO)), ==, 0);

    qtest_quit(qts);
}

/*
 * A second error in the group before software clears the first sets the
 * multiple-error bit and leaves the first error's address in place.
 */
static void test_second_timeout_keeps_first_address(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), INT_MASTER_TIMEOUT);

    qtest_readl(qts, CFG0(EMPTY_SLOT, 0));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) & INT_MULTI_ERR,
                    ==, 0);

    qtest_readl(qts, CFG0(OTHER_EMPTY_SLOT, 0));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) &
                    (INT_MULTI_ERR | INT_MASTER_TIMEOUT), ==,
                    INT_MULTI_ERR | INT_MASTER_TIMEOUT);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_PCI_ERR_LO)), ==,
                    IDSEL(EMPTY_SLOT));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_PCI_ERR_UP)), ==,
                    PCI_ERR_DEVICE(EMPTY_SLOT));

    /* Each has its own strobe; clearing the group rearms the capture. */
    qtest_writel(qts, REG(BRIDGE_INT_RESET), INT_RESET_MULTI);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) &
                    (INT_MULTI_ERR | INT_MASTER_TIMEOUT), ==,
                    INT_MASTER_TIMEOUT);
    qtest_writel(qts, REG(BRIDGE_INT_RESET), INT_RESET_PCI_GRP);
    qtest_readl(qts, CFG0(OTHER_EMPTY_SLOT, 0));
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_PCI_ERR_LO)), ==,
                    IDSEL(OTHER_EMPTY_SLOT));

    qtest_quit(qts);
}

static void test_master_timeout_raises_host_interrupt(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writel(qts, REG(BRIDGE_INT_DEST_LO),
                 HUB_XIO_INT_MODIFY);
    qtest_writel(qts, REG(BRIDGE_INT_DEST_UP), HUB_XIO_DEST_UPPER(0));
    qtest_writel(qts, REG(BRIDGE_HOST_ERR), ERR_HOST_BIT);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), INT_MASTER_TIMEOUT);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);
    qtest_readl(qts, CFG0(EMPTY_SLOT, 0));
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==,
                    1ULL << ERR_HOST_BIT);

    qtest_quit(qts);
}

/*
 * Where the processor fragment records each load's outcome: the Cause
 * register if the load faulted, zero if it completed.
 */
#define CAUSE_EMPTY         0x30000
#define CAUSE_PRESENT       0x30008
#define CAUSE_WRITE         0x30010
#define GUEST_MARKER_ADDR   0x30018
#define GUEST_MARKER        0x5a17c0deu
#define CAUSE_EXCCODE_MASK  0x7c
#define CAUSE_EXCCODE_DBE   (7 << 2)

#define IMG_MAIN            0x400
#define UNCACHED(phys)      (UINT64_C(0x9600000000000000) | (phys))
#define SMALL_WINDOW(off)   (UINT64_C(0x9200000000000000) | \
                             ((uint64_t)BRIDGE_WIDGET << 24) | (off))

/* s0 names the slot the next access's outcome goes to, preset to zero. */
static void emit_outcome_slot(SN0Image *p, uint32_t slot)
{
    sn0_emit_li64(p, SN0_R_S0, UNCACHED(slot));
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_ZERO, 0);
}

static void build_probe_image(SN0Image *p)
{
    unsigned int at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);

    sn0_patch_branch(p, at, IMG_MAIN);
    p->pos = SN0_VEC_TLB_REFILL;
    sn0_emit_park(p);
    p->pos = SN0_VEC_XTLB_REFILL;
    sn0_emit_park(p);

    /* Record Cause where s0 points and step over the faulting access. */
    p->pos = SN0_VEC_GENERAL;
    sn0_emit_mfc0(p, SN0_R_K0, SN0_CP0_CAUSE);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_K0, 0);
    sn0_emit_mfc0(p, SN0_R_K1, SN0_CP0_EPC);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_K1, SN0_R_K1, 4);
    sn0_emit_mtc0(p, SN0_R_K1, SN0_CP0_EPC);
    sn0_emit_eret(p);

    p->pos = IMG_MAIN;
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);

    emit_outcome_slot(p, CAUSE_EMPTY);
    sn0_emit_li64(p, SN0_R_T0, SMALL_WINDOW(0x20000 + EMPTY_SLOT * 0x1000));
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_T0, SN0_R_T2, 0);
    sn0_emit_nop(p);

    emit_outcome_slot(p, CAUSE_PRESENT);
    sn0_emit_li64(p, SN0_R_T0, SMALL_WINDOW(0x20000 + PRESENT_SLOT * 0x1000));
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_T0, SN0_R_T2, 0);
    sn0_emit_nop(p);

    /* A store has no response to carry an error: it posts. */
    emit_outcome_slot(p, CAUSE_WRITE);
    sn0_emit_li64(p, SN0_R_T0,
                  SMALL_WINDOW(0x20000 + EMPTY_SLOT * 0x1000 + 0x10));
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_T0, SN0_R_T2, 0);
    sn0_emit_nop(p);

    sn0_emit_li64(p, SN0_R_T0, UNCACHED(GUEST_MARKER_ADDR));
    sn0_emit_li(p, SN0_R_T1, GUEST_MARKER);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_T0, SN0_R_T1, 0);
    sn0_emit_park(p);
}

static void test_empty_slot_read_bus_errors(void)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;
    uint32_t marker = 0;

    build_probe_image(image);
    path = sn0_image_write(image, "sn0-bridge-master-timeout-guest");

    g_assert_true(g_setenv("QTEST_QEMU_ARGS", "-accel tcg", true));
    qts = qtest_initf(MACHINE " -smp 1 -bios %s", path);
    g_unsetenv("QTEST_QEMU_ARGS");
    unlink(path);

    /* The fragment zeroes each outcome itself; the marker says it ran. */
    g_test_timer_start();
    while (marker != GUEST_MARKER && g_test_timer_elapsed() < 10) {
        marker = qtest_readl(qts, GUEST_MARKER_ADDR);
        g_usleep(1000);
    }
    g_assert_cmphex(marker, ==, GUEST_MARKER);

    g_assert_cmphex(qtest_readq(qts, CAUSE_EMPTY) & CAUSE_EXCCODE_MASK, ==,
                    CAUSE_EXCCODE_DBE);
    g_assert_cmphex(qtest_readq(qts, CAUSE_PRESENT), ==, 0);
    g_assert_cmphex(qtest_readq(qts, CAUSE_WRITE), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sn0/bridge/master-timeout/empty-slot-read-latches",
                   test_empty_slot_latches_master_timeout);
    qtest_add_func("sn0/bridge/master-timeout/empty-slot-write-latches",
                   test_empty_slot_write_latches_master_timeout);
    qtest_add_func("sn0/bridge/master-timeout/needs-its-enable",
                   test_master_timeout_needs_its_enable);
    qtest_add_func("sn0/bridge/master-timeout/second-keeps-first-address",
                   test_second_timeout_keeps_first_address);
    qtest_add_func("sn0/bridge/master-timeout/raises-host-interrupt",
                   test_master_timeout_raises_host_interrupt);
    qtest_add_func("sn0/bridge/master-timeout/empty-slot-read-bus-errors",
                   test_empty_slot_read_bus_errors);

    return g_test_run();
}
