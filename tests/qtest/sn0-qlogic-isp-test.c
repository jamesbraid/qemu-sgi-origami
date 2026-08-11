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
 * The Bridge corrects narrow device-window accesses now (task #44), so this
 * file issues the address a real guest issues rather than the device's own
 * natural offset: `offset ^ (4 - width)`, the same formula OpenBSD's
 * `xbridge(4)` and Linux's `mangle-port.h` both compute for this device, and
 * the same formula this Bridge model now applies to un-correct it before the
 * access reaches the adapter. That correction is not unconditional — it is
 * scoped to devices that need it (task #44's own design record), which is
 * why `sn0-ioc3-test.c` exists unmodified beside this file: the controller
 * needs none of this and its own tests still issue natural offsets.
 *
 * The reset signature is the register to prove it with: three mailbox words
 * whose two bytes are never equal, so a hidden byte swap cannot produce a
 * value that happens to look right the way a check on symmetric bits could.
 * The same three values anchor the x86 suite's own reset-signature test, so
 * agreement here is agreement about the same chip, not two different numbers
 * that happen to both look plausible.
 *
 * The programmed-access swap bit is asserted clear rather than left
 * unmentioned. Real Bridge hardware has exactly this one mechanism for a
 * live gate, and both guests leave it disabled, always — OpenBSD's
 * `xbridge(4)` corrects register addresses itself, by XOR, rather than
 * asking the Bridge to swap them, and Linux's own `bridge_probe()` never
 * touches this bit either. Asserting it here means a later change to either
 * the model or this test's understanding of the guest has to be a
 * deliberate edit, not a silent drift — and it is what makes the
 * correction below the right one to apply unconditionally with respect to
 * live register state, gated only by which device is in the slot.
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
#define PCI_BAR0                0x10
#define PCI_BAR1                0x14

/*
 * The bridge's own per-slot device register, generalised from the
 * device-0-only form above: device 2's window-aiming and correction-
 * exemption test needs its own slot's register, not device 0's.
 */
#define BRIDGE_DEVICE(n)        (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)

/*
 * Device 0's window: the first of the two large 2 MB windows the base I/O
 * board's SCSI adapters sit in, matching the layout tests/qtest/sn0-bridge-
 * test.c already exercises for the other seven slots.
 */
#define WINDOW_OFFSET_DEVICE0   0x200000
#define WINDOW_DEVICE0          (BRIDGE_BASE + WINDOW_OFFSET_DEVICE0)

/*
 * IOC3's slot and window, restated from tests/qtest/sn0-ioc3-test.c rather
 * than shared with it, the same way that file restates SN0_IOC3_DEVICE
 * rather than importing it — this is the one device this file needs to
 * reach to test the *other* half of task #44's scoping claim, not a second
 * copy of that file's own coverage.
 */
#define PCI_IOC3_DEVICE         2
#define WINDOW_IOC3             (BRIDGE_BASE + 0x600000)
#define IOC3_MMIO               0x10000000u

/* Serial port A's line-control and modem-control registers, a byte apart. */
#define SERIAL_A_BASE           0x020178
#define SERIAL_A_LCR            (SERIAL_A_BASE + 3)
#define SERIAL_A_MCR            (SERIAL_A_BASE + 4)

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

/*
 * The address a real guest issues for a halfword access to device-window
 * offset `natural`, on a device the Bridge corrects: only the low two bits
 * move, matching the Bridge ASIC specification's own byte-lane tables
 * (Appendix A, Tables 65/73/74) and the identical arithmetic
 * `sgi_bridge_config_address()` already applies to configuration space.
 * `xbridge_read_2`/`write_2` and Linux's `__swizzle_addr_w` both compute
 * exactly this for a 2-byte access.
 */
#define GUEST_HALFWORD(natural)  (((natural) & ~3u) | (((natural) & 3u) ^ 2u))

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
     *
     * This is no longer a live gate the Bridge model reads — task #44's
     * design keys the correction on which device occupies the slot, not on
     * this bit, because both guests leave it disabled unconditionally. What
     * this test still documents is why that scoping is right rather than a
     * coincidence: if either guest ever programmed this bit for this
     * device, the premise "the correction is unconditional with respect to
     * live register state" would need re-examining, and this is the
     * assertion that would catch a guest starting to do so.
     */
    g_assert_cmphex(qtest_readl(qts, REG_BRIDGE_DEVICE0) & DEV_SWAP_PROGRAMMED,
                    ==, 0);

    aim_device0_window_at_its_bar(qts);
    g_assert_cmphex(qtest_readl(qts, REG_BRIDGE_DEVICE0) & DEV_SWAP_PROGRAMMED,
                    ==, 0);

    qtest_quit(qts);
}

/*
 * qtest_readw/writew, not a raw-byte helper decoding by hand — the reverse
 * of what this file used before task #44's fix, and checked empirically
 * rather than assumed, because this exact reasoning has been wrong in both
 * directions this investigation already.
 *
 * Before the fix, the device window was a transparent alias with no
 * MemoryRegionOps and no declared endianness of its own — the only
 * declared-endianness boundary between a raw qtest access and the device
 * was the device's own (`DEVICE_LITTLE_ENDIAN`), and `qtest_memread`,
 * decoded by hand as little-endian, was the tool that reached it with
 * nothing else in between. After the fix, the window is a real
 * `memory_region_init_io` region with its own declared endianness
 * (`DEVICE_NATIVE_ENDIAN`, matching this machine's target so that a real
 * guest's own load or store crosses it with no swap of its own) — a second
 * genuine boundary that a raw, host-framed byte tool does not cross the
 * same way a target-framed guest access does. Measured directly: reading
 * `guest_mailbox1` through the new window, a raw-byte decode returns
 * `0x5349` (wrong) while `qtest_readw` returns `0x4953` (right) — the
 * mirror image of the false positive `qtest_readw` gave through the old
 * alias, for the mirror-image reason. `qtest_writew`/`qtest_readw` are
 * what a guest's own value-based load or store actually looks like now
 * that the window does its own endianness-aware dispatch, which is
 * precisely why they were the wrong tool before it did.
 */
static void test_reset_signature_through_the_fabric(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    uint64_t guest_mailbox0 = WINDOW_DEVICE0 + GUEST_HALFWORD(ISP_MAILBOX(0));
    uint64_t guest_mailbox1 = WINDOW_DEVICE0 + GUEST_HALFWORD(ISP_MAILBOX(1));
    uint64_t guest_mailbox2 = WINDOW_DEVICE0 + GUEST_HALFWORD(ISP_MAILBOX(2));
    uint64_t guest_mailbox3 = WINDOW_DEVICE0 + GUEST_HALFWORD(ISP_MAILBOX(3));

    aim_device0_window_at_its_bar(qts);

    /*
     * The same three values the x86 suite's reset-signature test checks,
     * reached this time through the real crosstalk-to-PCI decode rather than
     * libqos's generic x86 graph — and at the address a real guest computes
     * for them, `GUEST_HALFWORD`, not the device's own natural offset: the
     * whole point of task #44's fix is that the Bridge now expects the
     * corrected address and un-corrects it, the way OpenBSD and Linux both
     * already assume. Each halfword's two bytes differ, so a hidden byte
     * swap anywhere on the path — the window's correction, the bridge's
     * decode, the device's own MemoryRegionOps — returns a visibly wrong
     * number rather than a coincidentally correct one, and reading the
     * *uncorrected* natural offset instead would now be the wrong test:
     * that is exactly the address this fix means to make land on the wrong
     * register.
     */
    g_assert_cmphex(qtest_readw(qts, guest_mailbox1), ==, ISP_MAILBOX1_SIGNATURE);
    g_assert_cmphex(qtest_readw(qts, guest_mailbox2), ==, ISP_MAILBOX2_SIGNATURE);
    g_assert_cmphex(qtest_readw(qts, guest_mailbox3), ==, ISP_MAILBOX3_SIGNATURE);

    /*
     * The reset signature alone only proves the read side: those three
     * values were placed in the device's own state before any guest access,
     * so a model could get every read right here while still mishandling a
     * write through this window. Mailbox 0 holds no signature and resets
     * clear, which is what makes it safe to reuse for proving the write side
     * too — nothing above depends on its reset value being anything in
     * particular, and its two bytes still differ, so this still catches a
     * swap.
     *
     * A write-then-read at one address alone is not enough, and left as
     * that alone it would not have caught task #44's own real defect: write
     * and read through the *same* wrong transform can land on the same
     * wrong register and still round-trip. So the check that actually
     * matters is the one after it — that mailbox 1's signature, checked
     * above, is still exactly ISP_MAILBOX1_SIGNATURE and not the value just
     * written to mailbox 0. Mailboxes 0 and 1 are the pair this correction
     * swaps with each other (0x70 and 0x72, two bytes apart, both aligned),
     * so a transform that is missing, doubled, or reversed sends this
     * write to mailbox 1 instead of mailbox 0, and only re-checking
     * mailbox 1's own, independently-known value afterwards would notice.
     */
    qtest_writew(qts, guest_mailbox0, 0x1234);
    g_assert_cmphex(qtest_readw(qts, guest_mailbox0), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, guest_mailbox1), ==, ISP_MAILBOX1_SIGNATURE);

    qtest_quit(qts);
}

static void test_ioc3_slot_is_exempt_at_the_same_stride(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /*
     * The other half of task #44's scoping claim, in the one file that
     * argues for the correction existing at all: the same Bridge mechanism
     * that now corrects device 0's window must leave device 2 — IOC3 — at
     * natural offsets, because IOC3 needs none of it. sn0-ioc3-test.c
     * already covers this device's own contract in full and is left
     * unmodified by this change; this test exists so the exemption's
     * evidence sits beside the correction it is the other half of, rather
     * than being implicit in "the other file still passes."
     *
     * Line control and modem control are adjacent one-byte registers in
     * IOC3's serial block (docs/datasheets/ioc3.md), a byte apart — the
     * exact granularity a wrongly-applied `offset ^ 3` correction would
     * scatter across the wrong side of a four-byte word. Writing distinct
     * values to both and reading them back at their own natural offsets is
     * the same shape sn0-ioc3-test.c's own
     * test_registers_are_a_byte_apart uses, repeated here deliberately so
     * this file states its own half of the contract rather than relying on
     * a reader to know the other suite proves it.
     */
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_BAR0, IOC3_MMIO);
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(PCI_IOC3_DEVICE),
                DEV_MEMORY_SPACE | (IOC3_MMIO >> 20));

    qtest_writeb(qts, WINDOW_IOC3 + SERIAL_A_LCR, 0x03);
    qtest_writeb(qts, WINDOW_IOC3 + SERIAL_A_MCR, 0x05);

    g_assert_cmphex(qtest_readb(qts, WINDOW_IOC3 + SERIAL_A_LCR), ==, 0x03);
    g_assert_cmphex(qtest_readb(qts, WINDOW_IOC3 + SERIAL_A_MCR), ==, 0x05);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/qlogic-isp/swap-bit-is-unset", NULL,
                        test_swap_bit_is_unset);
    qtest_add_data_func("sn0/qlogic-isp/reset-signature-through-the-fabric",
                        NULL, test_reset_signature_through_the_fabric);
    qtest_add_data_func("sn0/qlogic-isp/ioc3-slot-is-exempt-at-the-same-stride",
                        NULL, test_ioc3_slot_is_exempt_at_the_same_stride);

    return g_test_run();
}
