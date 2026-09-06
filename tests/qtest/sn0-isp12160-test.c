/*
 * SGI QLogic ISP12160 dual-channel SCSI adapter: the part's register
 * contract through the real crosstalk-to-PCI path.
 *
 * The adapter is plugged into bridge slot 3, beside the board's own two
 * ISP1040s and its IOC3, so every access here crosses the same byte-lane
 * correction and crosstalk decode a guest driver would.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Crosstalk space, and the bridge's own widget window inside it. */
#define SPACE_XTALK     0x20000000000ULL
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

/*
 * The bridge's registers are 32 bits in the low half of an eight-byte slot,
 * so every register address carries this offset.
 */
#define REG_DATA                4

#define DEV_MEMORY_SPACE        (1u << 12)

/* PCI type 0 configuration space, one 4 KB block per device. */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000

#define PCI_VENDOR_ID           0x00
#define PCI_COMMAND             0x04
#define PCI_COMMAND_IO          0x0001
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_COMMAND_MASTER      0x0004
#define PCI_CLASS_REVISION      0x08
#define PCI_BAR0                0x10
#define PCI_BAR1                0x14
#define PCI_INTERRUPT_LINE      0x3c

/* Class 0x01 mass storage, subclass 0x00 SCSI, programming interface 0. */
#define PCI_CLASS_SCSI          0x010000

/*
 * The board fits its two ISP1040s at devices 0 and 1 and the IOC3 at 2, so
 * the plugged adapter takes slot 3 and the 1 MB window that goes with it.
 */
#define MACHINE  "-M origin200 -m 64 -device qlogic-isp12160,bus=pci,addr=3"
#define SLOT     3

#define WINDOW            (BRIDGE_BASE + 0x700000)
#define CFG               (BRIDGE_BASE + BRIDGE_CFG0 + \
                           SLOT * CFG0_DEVICE_STRIDE)
#define REG_BRIDGE_DEVICE (BRIDGE_BASE + 0x200 + 8 * SLOT + REG_DATA)

/* A window carries PCI address bits 31:20, so both BARs land 20-bit aligned. */
#define ADAPTER_MMIO      0x10700000u
#define ADAPTER_IO        0x00700000u

/* The guest's halfword address: offset ^ (4 - width), low two bits only. */
#define GUEST_HALFWORD(natural)  (((natural) & ~3u) | (((natural) & 3u) ^ 2u))

/* Offsets in the adapter's own window: PCI space, so no plus-four applies. */
#define ISP_REG_ID_LOW          0x00
#define ISP_REG_ID_HIGH         0x02
#define ISP_REG_MAILBOX_BASE    0x70
#define ISP_MAILBOX(n)          (ISP_REG_MAILBOX_BASE + (n) * 2)

#define ISP_VENDOR_ID           0x1077
#define ISP12160_DEVICE_ID      0x1216
#define ISP12160_REVISION       0x06

/* The two identity registers as one doubleword. */
#define ISP_ID_PAIR       (((uint32_t)ISP12160_DEVICE_ID << 16) | ISP_VENDOR_ID)

/* 256 bytes of registers: what a BAR written with all ones must report. */
#define ISP_REG_WINDOW_MASK     0xffffff00u

/* Every access to the adapter goes through the window's byte-lane swizzle. */
static void isp_writew(QTestState *qts, unsigned natural, uint16_t val)
{
    qtest_writew(qts, WINDOW + GUEST_HALFWORD(natural), val);
}

static uint16_t isp_readw(QTestState *qts, unsigned natural)
{
    return qtest_readw(qts, WINDOW + GUEST_HALFWORD(natural));
}

/*
 * The command half alone is written back: the status half beside it is
 * read-only or write-one-to-clear, and a read-modify-write of the whole
 * doubleword would hand its latches back to it.
 */
static void enable_command_bits(QTestState *qts, uint32_t bits)
{
    uint32_t cmd = qtest_readl(qts, CFG + PCI_COMMAND) & 0xffffu;

    qtest_writel(qts, CFG + PCI_COMMAND, cmd | bits);
}

static void aim_window_at_memory_bar(QTestState *qts)
{
    qtest_writel(qts, CFG + PCI_BAR1, ADAPTER_MMIO);
    enable_command_bits(qts, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, REG_BRIDGE_DEVICE,
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
}

static void aim_window_at_io_bar(QTestState *qts)
{
    qtest_writel(qts, CFG + PCI_BAR0, ADAPTER_IO);
    enable_command_bits(qts, PCI_COMMAND_IO | PCI_COMMAND_MASTER);
    qtest_writel(qts, REG_BRIDGE_DEVICE, ADAPTER_IO >> 20);
}

/*
 * The part's own identity, and the two apertures it decodes. The BARs are
 * sized rather than assumed: a window of the wrong length puts the register
 * file somewhere a driver's fixed offsets miss, and the space bit is what
 * tells the two apertures apart.
 */
static void test_identity(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    uint32_t bar;

    g_assert_cmphex(qtest_readl(qts, CFG + PCI_VENDOR_ID), ==, ISP_ID_PAIR);
    g_assert_cmphex(qtest_readl(qts, CFG + PCI_CLASS_REVISION) >> 8, ==,
                    PCI_CLASS_SCSI);
    g_assert_cmphex(qtest_readl(qts, CFG + PCI_CLASS_REVISION) & 0xff, ==,
                    ISP12160_REVISION);
    /* The adapter presents one interrupt line, on INTA. */
    g_assert_cmphex((qtest_readl(qts, CFG + PCI_INTERRUPT_LINE) >> 8) & 0xff,
                    ==, 1);

    qtest_writel(qts, CFG + PCI_BAR0, 0xffffffffu);
    bar = qtest_readl(qts, CFG + PCI_BAR0);
    g_assert_cmphex(bar & ~0x3u, ==, ISP_REG_WINDOW_MASK);
    g_assert_cmphex(bar & 0x1u, ==, 1);

    qtest_writel(qts, CFG + PCI_BAR1, 0xffffffffu);
    bar = qtest_readl(qts, CFG + PCI_BAR1);
    g_assert_cmphex(bar & ~0xfu, ==, ISP_REG_WINDOW_MASK);
    g_assert_cmphex(bar & 0x1u, ==, 0);

    qtest_quit(qts);
}

/*
 * The register file is cycled sixteen bits at a time whatever the guest
 * asks for. The two widths either side of that are handled differently and
 * both are pinned, because the declared access size is the only thing
 * standing between a register's side effects and being run twice.
 */
static void test_window_accepts_only_16_bit(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    unsigned b;

    aim_window_at_memory_bar(qts);

    g_assert_cmphex(isp_readw(qts, ISP_REG_ID_LOW), ==, ISP_VENDOR_ID);
    g_assert_cmphex(isp_readw(qts, ISP_REG_ID_HIGH), ==, ISP12160_DEVICE_ID);

    /*
     * A doubleword is split into two halfword cycles rather than reaching
     * the register file as one, so it answers with both identity registers.
     * The discriminating half is what a single wide cycle would answer:
     * the low register alone, which is what an undeclared implementation
     * width produces.
     */
    g_assert_cmphex(qtest_readl(qts, WINDOW + ISP_REG_ID_LOW), ==,
                    ISP_ID_PAIR);

    /*
     * A byte is narrower than the register file decodes, so it is refused
     * outright rather than split: every byte of the identity pair reads
     * zero instead of the byte that lives at that offset.
     */
    for (b = 0; b < 4; b++) {
        g_assert_cmphex(qtest_readb(qts, WINDOW + b), ==, 0);
    }

    qtest_quit(qts);
}

/*
 * One register file behind both apertures. Mailboxes 6 and 7 hold no reset
 * signature, so a value read back through one BAR was put there through the
 * other; both directions run because one can alias correctly alone.
 */
static void test_both_bars_alias_one_register_file(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);

    aim_window_at_io_bar(qts);
    isp_writew(qts, ISP_MAILBOX(7), 0x5a5a);

    aim_window_at_memory_bar(qts);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(7)), ==, 0x5a5a);

    isp_writew(qts, ISP_MAILBOX(6), 0xa5a5);

    aim_window_at_io_bar(qts);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(6)), ==, 0xa5a5);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/isp12160/identity", NULL, test_identity);
    qtest_add_data_func("sn0/isp12160/window-accepts-only-16-bit", NULL,
                        test_window_accepts_only_16_bit);
    qtest_add_data_func("sn0/isp12160/both-bars-alias-one-register-file",
                        NULL, test_both_bars_alias_one_register_file);

    return g_test_run();
}
