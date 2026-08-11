/*
 * SGI QLogic ISP1040 SCSI adapter: the register window's access contract on
 * real crosstalk-to-PCI hardware.
 *
 * Every existing qtest for this device (tests/qtest/qlogic-isp-test.c) runs
 * through libqos's generic x86 PCI graph, which never leaves little-endian,
 * host-native byte order. That suite proved the device's own logic; it never
 * proved that a big-endian guest reading this device's registers through the
 * SGI Bridge gets the same bytes back, because nothing on that path — the
 * Bridge's per-device byte-swap bit, the crosstalk-to-PCI decode, the window
 * itself — exists in an x86 qtest. This is the first test to run the device
 * through that path, on the machine's real topology: the base I/O board's
 * two adapters, which origin200 fits at PCI devices 0 and 1 whenever the
 * bridge exists, with no drive attached and no -device needed to reach them.
 *
 * The reset signature is the register to prove it with: three mailbox words
 * whose two bytes are never equal, so a hidden byte swap cannot produce a
 * value that happens to look right the way a check on symmetric bits could.
 * The same three values anchor the x86 suite's own reset-signature test, so
 * agreement here is agreement about the same chip, not two different numbers
 * that happen to both look plausible.
 *
 * The programmed-access swap bit is asserted clear rather than left
 * unmentioned. OpenBSD's own Bridge driver never sets it for this device —
 * xbridge_probe_device_hook() corrects register addresses itself, by XOR,
 * rather than asking the Bridge to swap them — and the Bridge model does not
 * implement byte swapping on any path yet. Both facts point the same way
 * today. Asserting it here means a later change to either the model or this
 * test's understanding of the guest has to be a deliberate edit, not a
 * silent drift.
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
 * The bridge's own device(0) register: 32 bits in the low half of an
 * eight-byte slot, which this file touches exactly once, to aim the window
 * and to check the one bit this file is about.
 */
#define REG_DATA                4
#define BRIDGE_DEVICE0          0x200
#define REG_BRIDGE_DEVICE0      (BRIDGE_BASE + BRIDGE_DEVICE0 + REG_DATA)

#define DEV_MEMORY_SPACE        (1u << 12)
#define DEV_SWAP_PROGRAMMED     (1u << 13)

/*
 * PCI type 0 configuration space, and where device 0 sits inside it: the
 * first of the base I/O board's two SCSI adapters. The machine fits both
 * whenever the bridge exists, so reaching this one needs no -device.
 */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000
#define CFG0(device)            (BRIDGE_BASE + BRIDGE_CFG0 + \
                                 (device) * CFG0_DEVICE_STRIDE)
#define SCSI_DEVICE0            0

#define PCI_COMMAND             0x04
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_BAR1                0x14

/*
 * Device 0's window: the first of the two large 2 MB windows the base I/O
 * board's SCSI adapters sit in, matching the layout tests/qtest/sn0-bridge-
 * test.c already exercises for the other seven slots.
 */
#define WINDOW_OFFSET_DEVICE0   0x200000
#define WINDOW_DEVICE0          (BRIDGE_BASE + WINDOW_OFFSET_DEVICE0)

/*
 * Where this test points the adapter's BAR in PCI memory space. The value
 * only has to be twenty-bit aligned, which the window base field requires.
 */
#define ADAPTER_MMIO             0x10000000u

#define MACHINE_WITH_BRIDGE      "-M origin200,io-bridge=on"

/*
 * Register offsets within the adapter's own window, exactly as the x86 qtest
 * suite addresses them (tests/qtest/qlogic-isp-test.c) — this is PCI space
 * reached through the window, not the bridge's own eight-byte-slot layout,
 * so no plus-four applies here.
 */
#define ISP_REG_MAILBOX_BASE    0x70
#define ISP_MAILBOX(n)          (ISP_REG_MAILBOX_BASE + (n) * 2)

#define ISP_MAILBOX1_SIGNATURE  0x4953
#define ISP_MAILBOX2_SIGNATURE  0x5020
#define ISP_MAILBOX3_SIGNATURE  0x2020

static void aim_device0_window_at_its_bar(QTestState *qts)
{
    /*
     * Memory space, not I/O: the bridge's device register resets with its
     * memory-or-I/O bit already set, and OpenBSD's own bring-up leaves it
     * there for this device, so this is the aperture a real guest reaches
     * the adapter through.
     */
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_BAR1, ADAPTER_MMIO);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, REG_BRIDGE_DEVICE0,
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
}

static void test_swap_bit_is_unset(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /*
     * Checked both before and after the window is aimed, because the two
     * facts this bit rests on are independent: the reset value is a fact
     * about the model with the window untouched, and the guest's own
     * programming sequence never sets this bit while doing everything else
     * aiming the window requires. Both must hold for "unset" to be more than
     * an accident of which one this test happened to check.
     */
    g_assert_cmphex(qtest_readl(qts, REG_BRIDGE_DEVICE0) & DEV_SWAP_PROGRAMMED,
                    ==, 0);

    aim_device0_window_at_its_bar(qts);
    g_assert_cmphex(qtest_readl(qts, REG_BRIDGE_DEVICE0) & DEV_SWAP_PROGRAMMED,
                    ==, 0);

    qtest_quit(qts);
}

/*
 * A little-endian halfword, read the way this device actually presents it on
 * the bus rather than the way qtest_readw would.
 *
 * qtest_readw is the wrong tool here, and it took a false positive to find
 * that out: qtest's protocol corrects a value for target-versus-host
 * endianness (tswap16, in system/qtest.c) on top of whatever the memory core
 * already did for the region's own declared endianness (adjust_endianness,
 * in system/memory.c, which compares against the *host build's* endianness,
 * not the target's). The two corrections only cancel out when the device's
 * declared endianness matches the target's, which is true for this fork's
 * other big-endian-declared devices and false for this one on purpose — it
 * is a little-endian PCI part on a big-endian target. The result is a wrong
 * value with nothing to say so: not a fault, not a log line, just the
 * halfword's two bytes in the other order. qtest_memread never applies
 * either correction — it hands back exactly the bytes address_space_read
 * produced — so decoding those bytes as little-endian here, by hand, is what
 * actually exercises what a guest's own load sees.
 */
static uint16_t read_le16(QTestState *qts, uint64_t addr)
{
    uint8_t bytes[2];

    qtest_memread(qts, addr, bytes, sizeof(bytes));
    return bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void write_le16(QTestState *qts, uint64_t addr, uint16_t value)
{
    uint8_t bytes[2] = { value & 0xff, value >> 8 };

    qtest_memwrite(qts, addr, bytes, sizeof(bytes));
}

static void test_reset_signature_through_the_fabric(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    aim_device0_window_at_its_bar(qts);

    /*
     * The same three values the x86 suite's reset-signature test checks,
     * reached this time through the real crosstalk-to-PCI decode rather than
     * libqos's generic x86 graph. Each halfword's two bytes differ, so a
     * hidden byte swap anywhere on the path — the window's mapping, the
     * bridge's decode, the device's own MemoryRegionOps — returns a visibly
     * wrong number rather than a coincidentally correct one.
     */
    g_assert_cmphex(read_le16(qts, WINDOW_DEVICE0 + ISP_MAILBOX(1)), ==,
                    ISP_MAILBOX1_SIGNATURE);
    g_assert_cmphex(read_le16(qts, WINDOW_DEVICE0 + ISP_MAILBOX(2)), ==,
                    ISP_MAILBOX2_SIGNATURE);
    g_assert_cmphex(read_le16(qts, WINDOW_DEVICE0 + ISP_MAILBOX(3)), ==,
                    ISP_MAILBOX3_SIGNATURE);

    /*
     * The reset signature alone only proves the read side: those three
     * values were placed in the device's own state before any guest access,
     * so a model could get every read right here while still mishandling a
     * write through this window. Mailbox 0 holds no signature and resets
     * clear, which is what makes it safe to reuse for proving the write side
     * too — nothing above depends on its reset value being anything in
     * particular, and its two bytes still differ, so this still catches a
     * swap.
     */
    write_le16(qts, WINDOW_DEVICE0 + ISP_MAILBOX(0), 0x1234);
    g_assert_cmphex(read_le16(qts, WINDOW_DEVICE0 + ISP_MAILBOX(0)), ==,
                    0x1234);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/qlogic-isp/swap-bit-is-unset", NULL,
                        test_swap_bit_is_unset);
    qtest_add_data_func("sn0/qlogic-isp/reset-signature-through-the-fabric",
                        NULL, test_reset_signature_through_the_fabric);

    return g_test_run();
}
