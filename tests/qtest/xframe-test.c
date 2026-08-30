/*
 * Neterion/S2io Xframe: the enumeration contract on the Origin 200 Bridge.
 *
 * SCAFFOLDING SLICE ONLY. This file asserts what the scaffolding actually
 * models: PCI identity (vendor/device, and subsystem vendor/id once the
 * SGI-board properties are set), that both BAR windows are present and
 * sized as the datasheet states, and that a reset leaves that identity
 * alone. It does NOT assert anything about the 64-bit PIF/TxP register
 * file behind those windows -- that register file is not modelled in this
 * slice (see include/hw/net/xframe.h and docs/library/
 * nic-on-bridge-conventions.md's "Access width" note: the Bridge splits an
 * 8-byte device access into two 4-byte cycles today, which cannot serve
 * this device's full-value-side-effect registers correctly, and that is
 * under review by the Bridge owner). A follow-on slice adds the register-
 * contract qtests (the swapper diagnostic, SW_RESET, RMAC_CFG_KEY, the
 * factory-MAC read-back) once that settles -- see docs/superpowers/plans/
 * 2026-08-29-xframe-ethernet-adapter.md, Task P3 and Task 1.
 *
 * This runs on the origin200-Bridge harness rather than a generic PCI
 * graph, following tests/qtest/sn0-qlogic-isp-test.c and the shared
 * convention (docs/library/nic-on-bridge-conventions.md, Section 3): a
 * device tested in isolation can enumerate correctly and still be wrong on
 * this machine, because config-space access reaches it through the
 * Bridge's own decode rather than a host-native PCI root. The card is an
 * add-in device, attached at a free slot with -device, the same pattern
 * tests/qtest/sn0-bridge-test.c uses for its own probe and DMA cards; the
 * base I/O board's built-in devices (two SCSI adapters and the IOC3) sit at
 * devices 0-2, so this file's slots are chosen clear of those.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Crosstalk space, and the bridge's own widget window inside it -- restated
 * from tests/qtest/sn0-bridge-test.c rather than shared with it, the same
 * way every sn0-*-test.c file restates these constants for itself.
 */
#define SPACE_XTALK             0x20000000000ULL
#define BRIDGE_WIDGET           8
#define BRIDGE_BASE             (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

#define MACHINE_WITH_BRIDGE     "-M origin200,io-bridge=on"

/*
 * PCI type 0 configuration space, and where a device's function 0 sits
 * inside it.
 */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000
#define CFG0(device)            (BRIDGE_BASE + BRIDGE_CFG0 + \
                                 (device) * CFG0_DEVICE_STRIDE)

/*
 * The base I/O board populates devices 0-2 (two SCSI adapters and the
 * IOC3) whenever the bridge exists, so this file's cards sit clear of
 * those -- and clear of each other, though nothing here needs two cards up
 * at once, since every test starts its own machine.
 */
#define XFRAME_SLOT             4
#define XFRAME_HERC_SLOT        5
#define XFRAME_SGI_SLOT         6

#define XFRAME_CARD             " -device xframe,addr=" stringify(XFRAME_SLOT)

/* The configuration registers this file reads or probes. */
#define PCI_COMMAND              0x04
#define PCI_COMMAND_IO           0x0001
#define PCI_COMMAND_MEMORY       0x0002
#define PCI_COMMAND_MASTER       0x0004
#define PCI_BAR0                 0x10
#define PCI_BAR0_HI              0x14
#define PCI_BAR2                 0x18
#define PCI_SUBSYSTEM_VENDOR_ID  0x2c

/*
 * The identity a plain (no SGI properties) card presents --
 * docs/datasheets/xframe.md's "PCI identity" table: vendor 0x17d5,
 * device 0x5831 (Xframe I / Xena, SGI Release 1).
 */
#define XFRAME_VENDOR            0x17d5
#define XFRAME_DEVICE_XENA       0x5831
#define XFRAME_DEVICE_HERC       0x5832
#define XFRAME_IDENTITY          (XFRAME_VENDOR | (XFRAME_DEVICE_XENA << 16))

/* The SGI board's own PCI subsystem vendor ID; its subsystem device ID is
 * OPEN in the datasheet and must not be invented, so this file only ever
 * asserts that half stays at its unbranded default (0).
 */
#define XFRAME_SGI_SUBSYSTEM_VENDOR  0x10a9

/*
 * BAR sizes, restated from docs/datasheets/xframe.md's "PCI identity" and
 * "Register map" sections rather than derived here, for the same reason
 * tests/qtest/sn0-bridge-test.c states its window offsets rather than
 * recomputing them: a test that recomputes the implementation's arithmetic
 * checks that the arithmetic ran, not that it is right.
 */
#define XFRAME_PIF_WINDOW_SIZE   0x4000    /* BAR0, spans >= 0x0000-0x33ff */
#define XFRAME_TXP_WINDOW_SIZE   0x100000  /* second BAR, 8 * 0x00020000 */

/*
 * What a slot with nothing in it answers -- the standard PCI "no device"
 * value, which must not be zero or a probe of an empty slot could be
 * mistaken for a real one.
 */
#define NO_CARD                  0xffffffffu

/*
 * Size a 32-bit memory BAR by the standard PCI probe: write all ones,
 * read back what stuck, restore the original value, then decode. The low
 * four bits of a memory BAR are type/prefetch flags rather than address
 * bits, so they come out of the decode; a device's actual size is the
 * one's-complement of what remains, plus one.
 */
static uint64_t xframe_bar_size32(QTestState *qts, uint64_t cfg_addr,
                                  uint32_t bar_offset)
{
    uint32_t orig = qtest_readl(qts, cfg_addr + bar_offset);
    uint32_t probe;

    qtest_writel(qts, cfg_addr + bar_offset, 0xffffffffu);
    probe = qtest_readl(qts, cfg_addr + bar_offset);
    qtest_writel(qts, cfg_addr + bar_offset, orig);

    return (uint64_t)(~(probe & ~0xfu)) + 1;
}

/*
 * The same probe for a 64-bit memory BAR (BAR0's own kind): the flag bits
 * live only in the low dword, so only that half is masked before the two
 * dwords are combined and decoded as one 64-bit field.
 */
static uint64_t xframe_bar_size64(QTestState *qts, uint64_t cfg_addr,
                                  uint32_t bar_offset)
{
    uint32_t orig_lo = qtest_readl(qts, cfg_addr + bar_offset);
    uint32_t orig_hi = qtest_readl(qts, cfg_addr + bar_offset + 4);
    uint32_t probe_lo, probe_hi;
    uint64_t probe;

    qtest_writel(qts, cfg_addr + bar_offset, 0xffffffffu);
    qtest_writel(qts, cfg_addr + bar_offset + 4, 0xffffffffu);
    probe_lo = qtest_readl(qts, cfg_addr + bar_offset);
    probe_hi = qtest_readl(qts, cfg_addr + bar_offset + 4);
    qtest_writel(qts, cfg_addr + bar_offset, orig_lo);
    qtest_writel(qts, cfg_addr + bar_offset + 4, orig_hi);

    probe = ((uint64_t)probe_hi << 32) | (uint64_t)(probe_lo & ~0xfu);
    return ~probe + 1;
}

/*
 * The generic device, no SGI-board properties set. This is the payoff of
 * naming the model after the silicon rather than the board
 * (docs/datasheets/xframe.md's "Device identity as configurable
 * properties"): a card with nothing but its silicon identity has to
 * enumerate cleanly on its own.
 */
static void test_identity(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE XFRAME_CARD);

    g_assert_cmphex(qtest_readl(qts, CFG0(XFRAME_SLOT)), ==, XFRAME_IDENTITY);

    qtest_quit(qts);
}

/*
 * The device-id property selects the other ASIC generation (Xframe II /
 * Herc) rather than a separate device type -- docs/datasheets/xframe.md's
 * "Device identity as configurable properties".
 */
static void test_device_id_selects_herc(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -device xframe,addr="
                                 stringify(XFRAME_HERC_SLOT)
                                 ",device-id=0x5832");
    uint32_t identity = qtest_readl(qts, CFG0(XFRAME_HERC_SLOT));

    g_assert_cmphex(identity, ==,
                    XFRAME_VENDOR | (XFRAME_DEVICE_HERC << 16));

    qtest_quit(qts);
}

/*
 * The generic device carries no subsystem branding of its own. This is the
 * regression the realize() path has to get right: the PCI core fills an
 * unset subsystem pair with QEMU's own generic default rather than leaving
 * it zero, so the device model must positively overwrite that default
 * rather than merely not setting anything -- see hw/net/xframe.c's
 * xframe_realize() comment.
 */
static void test_subsystem_is_unbranded_by_default(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE XFRAME_CARD);

    g_assert_cmphex(qtest_readl(qts, CFG0(XFRAME_SLOT) +
                                PCI_SUBSYSTEM_VENDOR_ID), ==, 0);

    qtest_quit(qts);
}

/*
 * The SGI instantiation: subsystem-vendor-id 0x10a9
 * (docs/datasheets/xframe.md's "Device identity as configurable
 * properties"). Its subsystem device ID is OPEN there and must not be
 * invented, so this asserts only the half the datasheet actually settles --
 * the other half of the same dword must still read the unbranded default,
 * proving nothing was invented to fill it.
 */
static void test_subsystem_vendor_when_sgi_board(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -device xframe,addr="
                                 stringify(XFRAME_SGI_SLOT)
                                 ",subsystem-vendor-id=0x10a9");
    uint32_t subsystem = qtest_readl(qts, CFG0(XFRAME_SGI_SLOT) +
                                     PCI_SUBSYSTEM_VENDOR_ID);

    g_assert_cmphex(subsystem, ==, XFRAME_SGI_SUBSYSTEM_VENDOR);

    qtest_quit(qts);
}

/*
 * BAR0: a 64-bit memory BAR (config 0x10/0x14), sized to cover the PIF
 * register map's documented span. The type/space flags are read straight
 * off the unprobed BAR rather than through the sizing helper, because
 * those bits are fixed by what the device registered and a sizing probe
 * masks them away on purpose.
 */
static void test_bar0_is_a_64bit_pif_window(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE XFRAME_CARD);
    uint32_t bar0_low = qtest_readl(qts, CFG0(XFRAME_SLOT) + PCI_BAR0);

    /* Memory space (bit 0 clear), 64-bit type (bits 2:1 = 0b10), not
     * prefetchable (bit 3 clear): 0b0100.
     */
    g_assert_cmphex(bar0_low & 0xf, ==, 0x4);

    g_assert_cmphex(xframe_bar_size64(qts, CFG0(XFRAME_SLOT), PCI_BAR0), ==,
                    XFRAME_PIF_WINDOW_SIZE);

    qtest_quit(qts);
}

/*
 * The second BAR (config 0x18 -- BAR index 2, since index 1 is BAR0's own
 * upper half): a 32-bit memory window for the TxP transmit doorbell,
 * sized to the eight-FIFO span docs/datasheets/xframe.md's register map
 * states.
 */
static void test_bar2_is_the_txp_window(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE XFRAME_CARD);
    uint32_t bar2 = qtest_readl(qts, CFG0(XFRAME_SLOT) + PCI_BAR2);

    /* Memory space, 32-bit type, not prefetchable: every flag bit clear. */
    g_assert_cmphex(bar2 & 0xf, ==, 0x0);

    g_assert_cmphex(xframe_bar_size32(qts, CFG0(XFRAME_SLOT), PCI_BAR2), ==,
                    XFRAME_TXP_WINDOW_SIZE);

    qtest_quit(qts);
}

/*
 * Reset state, asserted the way the scaffolding can support it: through
 * 32-bit configuration-space reads only, never a register behind either
 * BAR (see this file's own header). An add-in card at a free slot is
 * outside the machine's built-in resource assignment
 * (sn0_assign_node_pci_resources touches only the base I/O board's own
 * devices), so nothing reprograms this card's command register or BARs on
 * reset -- power-on defaults before, and again after.
 */
static void test_reset_leaves_identity_and_bars_alone(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE XFRAME_CARD);
    uint64_t bar0_size_before, bar0_size_after;

    /* Cold: no firmware or guest has enabled the card yet. */
    g_assert_cmphex(qtest_readl(qts, CFG0(XFRAME_SLOT) + PCI_COMMAND) &
                    0xffff, ==, 0);

    bar0_size_before = xframe_bar_size64(qts, CFG0(XFRAME_SLOT), PCI_BAR0);

    qtest_system_reset(qts);

    g_assert_cmphex(qtest_readl(qts, CFG0(XFRAME_SLOT)), ==, XFRAME_IDENTITY);
    bar0_size_after = xframe_bar_size64(qts, CFG0(XFRAME_SLOT), PCI_BAR0);
    g_assert_cmphex(bar0_size_after, ==, bar0_size_before);
    g_assert_cmphex(qtest_readl(qts, CFG0(XFRAME_SLOT) + PCI_COMMAND) &
                    0xffff, ==, 0);

    qtest_quit(qts);
}

/* An empty slot answers "no card" -- the control every card-present
 * assertion above depends on meaning something.
 */
static void test_an_empty_slot_answers_nothing(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE XFRAME_CARD);

    g_assert_cmphex(qtest_readl(qts, CFG0(XFRAME_HERC_SLOT)), ==, NO_CARD);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("xframe/identity", NULL, test_identity);
    qtest_add_data_func("xframe/device-id-selects-herc", NULL,
                        test_device_id_selects_herc);
    qtest_add_data_func("xframe/subsystem-is-unbranded-by-default", NULL,
                        test_subsystem_is_unbranded_by_default);
    qtest_add_data_func("xframe/subsystem-vendor-when-sgi-board", NULL,
                        test_subsystem_vendor_when_sgi_board);
    qtest_add_data_func("xframe/bar0-is-a-64bit-pif-window", NULL,
                        test_bar0_is_a_64bit_pif_window);
    qtest_add_data_func("xframe/bar2-is-the-txp-window", NULL,
                        test_bar2_is_the_txp_window);
    qtest_add_data_func("xframe/reset-leaves-identity-and-bars-alone", NULL,
                        test_reset_leaves_identity_and_bars_alone);
    qtest_add_data_func("xframe/an-empty-slot-answers-nothing", NULL,
                        test_an_empty_slot_answers_nothing);

    return g_test_run();
}
