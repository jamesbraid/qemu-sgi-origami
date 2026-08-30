/*
 * Alteon Tigon 2 (the SGI "eg" gigabit board) behind the Origin 200 Bridge.
 *
 * The Tigon is an add-in PCI card, so unlike the base I/O board's soldered
 * adapters it is instantiated with -device at a free slot and reached through
 * that slot's own Bridge decode -- the same way tests/qtest/sn0-bridge-test.c
 * hosts its probe card. Everything here runs on the machine's real
 * crosstalk-to-PCI topology so the register contract, the firmware-mediated
 * ring engine, and the descriptor byte order are all proven on the fabric the
 * card actually sits on rather than a generic PCI bus that has no Bridge.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Crosstalk space and the Bridge's widget window inside it. */
#define SPACE_XTALK     0x20000000000ULL
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

#define MACHINE_WITH_BRIDGE  "-M origin200,io-bridge=on -device tigon,addr=3"

/*
 * The card's slot. Devices 0 and 1 are the base I/O board's SCSI adapters and
 * device 2 is IOC3; 3 is the first free slot, and its widget window is the
 * first of the six 1 MB windows (sn0-bridge-test.c's own window table).
 */
#define TIGON_SLOT      3
#define WINDOW_TIGON    (BRIDGE_BASE + 0x700000)

/* PCI type 0 configuration space through the Bridge. */
#define BRIDGE_CFG0     0x020000
#define CFG0_STRIDE     0x1000
#define CFG0(dev)       (BRIDGE_BASE + BRIDGE_CFG0 + (dev) * CFG0_STRIDE)

#define PCI_VENDOR_DEVICE   0x00
#define PCI_COMMAND         0x04
#define PCI_REV_CLASS       0x08
#define PCI_BAR0            0x10
#define PCI_INTR_LINE       0x3c

#define PCI_COMMAND_MEMORY  0x0002

/* The Bridge's own per-slot device register, low half of its 8-byte slot. */
#define REG_DATA            4
#define BRIDGE_DEVICE(n)    (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)
#define DEV_MEMORY_SPACE    (1u << 12)

/* Where the card's BAR is parked in PCI memory space (1 MB aligned). */
#define ADAPTER_MMIO        0x10000000u

/* The card's identity, from docs/datasheets/tigon.md. */
#define TIGON_VENDOR_ID     0x10a9
#define TIGON_DEVICE_ID     0x0009
#define TIGON_CLASS_CODE    0x020000
#define TIGON_CHIP_REV      0x6         /* Tigon 2 rev nibble */

/* Register offsets within the card's own window (if_tireg.h). */
#define TI_MISC_HOST_CTL    0x040
#define TI_MHC_RESET        0x00000008
#define TI_MHC_CHIP_REV_MASK 0xf0000000

/*
 * A 4-byte access needs no byte-lane correction -- sgi_bridge_window_correct()
 * moves only the low two bits and leaves a full-word access at its natural
 * offset -- so a 32-bit register is reached at WINDOW + offset directly.
 */
static void aim_window_at_bar(QTestState *qts)
{
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_BAR0, ADAPTER_MMIO);
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(TIGON_SLOT),
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
}

static void test_config_identity(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /* Vendor and device, the SGI Tigon pair, in one 32-bit config read. */
    g_assert_cmphex(qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_VENDOR_DEVICE), ==,
                    TIGON_VENDOR_ID | (TIGON_DEVICE_ID << 16));

    /* Class code (bytes 0x09-0x0B): Ethernet network controller. */
    g_assert_cmphex(qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_REV_CLASS) >> 8, ==,
                    TIGON_CLASS_CODE);

    /* One interrupt pin, INTA. */
    g_assert_cmphex((qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_INTR_LINE) >> 8) &
                    0xff, ==, 0x01);

    qtest_quit(qts);
}

static void test_one_memory_bar(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /* BAR0 is a 32-bit, non-prefetchable memory BAR: low nibble all clear. */
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_BAR0, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_BAR0), ==,
                    0xffffc000u);

    /* There is no second BAR. */
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_BAR0 + 4, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_BAR0 + 4), ==, 0);

    qtest_quit(qts);
}

static void test_misc_host_ctl_chip_rev(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    aim_window_at_bar(qts);

    /* The chip-revision nibble is the driver's Tigon 1 vs 2 runtime test. */
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_MISC_HOST_CTL) &
                    TI_MHC_CHIP_REV_MASK, ==, (uint32_t)TIGON_CHIP_REV << 28);

    /* The RESET bit is accepted and leaves the read-only rev nibble intact. */
    qtest_writel(qts, WINDOW_TIGON + TI_MISC_HOST_CTL, TI_MHC_RESET);
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_MISC_HOST_CTL) &
                    TI_MHC_CHIP_REV_MASK, ==, (uint32_t)TIGON_CHIP_REV << 28);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sn0/tigon/config-identity", NULL,
                        test_config_identity);
    qtest_add_data_func("/sn0/tigon/one-memory-bar", NULL,
                        test_one_memory_bar);
    qtest_add_data_func("/sn0/tigon/misc-host-ctl-chip-rev", NULL,
                        test_misc_host_ctl_chip_rev);

    return g_test_run();
}
