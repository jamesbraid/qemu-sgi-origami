/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the Broadcom NetXtreme BCM570x adapter behind the Origin 200
 * Bridge.
 *
 * The device is a generic PCI NIC that the SGI IRIS Release 2 Gigabit board
 * instantiates with a particular PCI four-tuple. IRIX's tg driver registers on
 * the raw Broadcom primary vendor/device and then, at attach, insists the card
 * also carry SGI's subsystem identity; a card with the Broadcom IDs but the
 * wrong or absent SGI subsystem half passes the primary filter and is rejected
 * only at attach, a failure a boot cannot show cheaply. So this test asserts the
 * whole four-tuple, plus the two indirect config-space windows the driver uses
 * before it ever touches the BAR (docs/datasheets/bcm570x.md, "three access
 * paths" and the qtest checklist).
 *
 * The card is placed at a free slot behind the origin200 Bridge, the same way
 * tests/qtest/sn0-bridge-test.c and tests/qtest/sn0-qlogic-isp-test.c reach an
 * add-in card: config space through the Bridge's type-0 window, and the device
 * BAR through a per-slot memory window the test aims by hand. Byte order across
 * the Bridge is settled by the transmit round-trip qtest in a later task, not
 * here; every access in this file is a 32-bit dword, for which the Bridge's
 * byte-lane address correction is the identity, so nothing here depends on the
 * unsettled register endianness.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"

/* Crosstalk space and the Bridge's own widget window inside it. */
#define SPACE_XTALK             0x20000000000ULL
#define BRIDGE_WIDGET           8
#define BRIDGE_BASE             (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

#define MACHINE_WITH_BRIDGE     "-M origin200,io-bridge=on"

/*
 * PCI type-0 configuration space through the Bridge, one 4 KB stride per
 * device and 0x100 per function, exactly as sn0-bridge-test.c addresses it.
 */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000
#define CFG0_FUNCTION_STRIDE    0x100
#define CFG0(device, function)  (BRIDGE_BASE + BRIDGE_CFG0 + \
                                 (device) * CFG0_DEVICE_STRIDE + \
                                 (function) * CFG0_FUNCTION_STRIDE)

/*
 * The Bridge's own per-slot device register, used to aim a slot's memory
 * window: REG() reaches the 32-bit datum in the low half of the eight-byte
 * slot, and DEV_MEMORY_SPACE plus the window base (bits 31:20 of the card's
 * PCI address) point the window at the card.
 */
#define REG_DATA                4
#define REG(offset)             (BRIDGE_BASE + (offset) + REG_DATA)
#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))
#define DEV_MEMORY_SPACE        (1u << 12)

/*
 * Where each slot's memory window sits in the widget's address space. The
 * first two slots get 2 MB windows and the rest 1 MB, laid end to end from
 * 0x200000 -- written out rather than recomputed from the model's own rule.
 */
static const uint64_t window_offset[8] = {
    0x200000, 0x400000, 0x600000, 0x700000,
    0x800000, 0x900000, 0xa00000, 0xb00000,
};
#define WINDOW(slot)            (BRIDGE_BASE + window_offset[slot])

/* Where the card's BAR0 is placed in PCI memory space; 16 MB per slot. */
#define CARD_MMIO(slot)         (0x10000000u + (slot) * 0x1000000u)

/*
 * Base I/O board devices sit at 0, 1 (SCSI) and 2 (IOC3); 3..7 are free. The
 * card goes at slot 4, matching sn0-bridge-test.c's own probe slot.
 */
#define BCM_SLOT                4

/* The single-port SGI IRIS Release 2 board's four-tuple (bcm5701, copper). */
#define BCM_VENDOR_ID           0x14e4
#define BCM_DEVICE_ID           0x1645
#define BCM_SUBSYS_VENDOR_ID    0x10a9
#define BCM_SUBSYS_ID           0x8010

/* The indirect-access config registers (docs/datasheets/bcm570x.md). */
#define BCM_PCI_REG_BASEADDR    0x78
#define BCM_PCI_MEMWIN_BASEADDR 0x7c
#define BCM_PCI_REG_DATA        0x80
#define BCM_PCI_MEMWIN_DATA     0x84

/* BAR0 layout: register file low, on-chip SRAM window at 0x8000. */
#define BCM_SRAM_WINDOW         0x8000u

/*
 * The Bridge corrects a narrow (1- or 2-byte) device access by moving only the
 * low two address bits, `off ^ (4 - size)`, the same formula OpenBSD's
 * xbridge(4) and Linux's swizzle both compute. A 16-bit access at natural
 * offset `n` therefore lands at `(n & ~3) | ((n & 3) ^ 2)`. A 4-byte access is
 * unaffected (`^ 0`), which is why the dword reads below need no correction.
 */
#define GUEST_HALFWORD(n)       (((n) & ~3u) | (((n) & 3u) ^ 2u))

static QTestState *bcm570x_start(void)
{
    /*
     * An explicit netdev, referenced by id, so the card's frontend has a
     * backend without competing for the machine's default network device the
     * onboard IOC3 already claims.
     */
    return qtest_initf(MACHINE_WITH_BRIDGE
                       " -netdev user,id=n0"
                       " -device bcm570x,addr=%02x.0,netdev=n0", BCM_SLOT);
}

/* Aim the card's slot window at its BAR0, so the BAR is reachable. */
static void bcm570x_aim_window(QTestState *qts)
{
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + PCI_BASE_ADDRESS_0, CARD_MMIO(BCM_SLOT));
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, REG(BRIDGE_DEVICE(BCM_SLOT)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(BCM_SLOT) >> 20));
}

static uint32_t bar_readl(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, WINDOW(BCM_SLOT) + off);
}

/*
 * The four-tuple IRIX tg requires. The Broadcom primary vendor/device are
 * standard, and the SGI subsystem identity sits in the standard PCI subsystem
 * fields: subsystem vendor at config 0x2C, subsystem id at 0x2E.
 *
 * Note on offsets: docs/datasheets/bcm570x.md's config table and qtest
 * checklist place the subsystem *device* at 0x2C and the subsystem *vendor* at
 * 0x2E, taking OpenBSD bge's header macro names (BGE_PCI_SUBSYS 0x2C /
 * BGE_PCI_SUBVID 0x2E) at face value. Those macro names are backwards: bge's
 * own driver (if_bge.c) reads the subsystem through the standard
 * PCI_SUBSYS_ID_REG (0x2C) dword and takes PCI_VENDOR() -- the low half at
 * 0x2C -- as the subsystem vendor and PCI_PRODUCT() -- the high half at 0x2E --
 * as the device. That is plain standard-PCI layout, which is what real silicon
 * and IRIX both use, so the card presents SGI vendor 0x10a9 at 0x2C and the
 * board id 0x8010 at 0x2E. The datasheet table/checklist need the same
 * correction.
 */
static void test_pci_four_tuple(const void *data)
{
    QTestState *qts = bcm570x_start();

    /*
     * Dword reads first: a 4-byte config read crosses the Bridge with no
     * byte-lane correction, so vendor sits in the low half and device in the
     * high half, exactly as sn0-bridge-test.c reads a probe card's identity.
     */
    g_assert_cmphex(qtest_readl(qts, CFG0(BCM_SLOT, 0) + PCI_VENDOR_ID), ==,
                    BCM_VENDOR_ID | ((uint32_t)BCM_DEVICE_ID << 16));
    g_assert_cmphex(qtest_readl(qts, CFG0(BCM_SLOT, 0) + PCI_SUBSYSTEM_VENDOR_ID),
                    ==, BCM_SUBSYS_VENDOR_ID | ((uint32_t)BCM_SUBSYS_ID << 16));

    /*
     * And each field on its own as a corrected halfword, so a hidden swap in
     * one half cannot hide behind the other. The vendor at natural offset 0 is
     * read at guest offset 2, and so on.
     */
    g_assert_cmphex(qtest_readw(qts, CFG0(BCM_SLOT, 0) + GUEST_HALFWORD(0x00)),
                    ==, BCM_VENDOR_ID);
    g_assert_cmphex(qtest_readw(qts, CFG0(BCM_SLOT, 0) + GUEST_HALFWORD(0x02)),
                    ==, BCM_DEVICE_ID);
    g_assert_cmphex(qtest_readw(qts, CFG0(BCM_SLOT, 0) + GUEST_HALFWORD(0x2c)),
                    ==, BCM_SUBSYS_VENDOR_ID);
    g_assert_cmphex(qtest_readw(qts, CFG0(BCM_SLOT, 0) + GUEST_HALFWORD(0x2e)),
                    ==, BCM_SUBSYS_ID);

    qtest_quit(qts);
}

/*
 * The indirect register window round-trips. Writing a register offset to config
 * 0x78 and a value to config 0x80 must reach the same register file the BAR
 * exposes at 0x0000-0x7FFF: read the register back through BAR0 at that offset
 * and it must match, proving 0x78/0x80 alias the file.
 */
static void test_indirect_register_window(const void *data)
{
    QTestState *qts = bcm570x_start();
    const uint32_t reg_off = 0x0410;    /* in-range, dword-aligned */
    const uint32_t value = 0x12345678;

    bcm570x_aim_window(qts);

    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_REG_BASEADDR, reg_off);
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_REG_DATA, value);

    /* Through the BAR at the same offset: the aliasing proof. */
    g_assert_cmphex(bar_readl(qts, reg_off), ==, value);

    /* And back through the indirect window itself. */
    g_assert_cmphex(qtest_readl(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_REG_DATA), ==,
                    value);

    qtest_quit(qts);
}

/*
 * The indirect memory window round-trips. Writing an on-chip SRAM offset to
 * config 0x7C and a value to config 0x84 reaches one word of SRAM; the same
 * base, masked to a 32 KB boundary, positions the BAR0 window at 0x8000. Read
 * the word back both through config 0x7C/0x84 and through the BAR0 window, and
 * the two views must agree.
 */
static void test_indirect_memory_window(const void *data)
{
    QTestState *qts = bcm570x_start();
    const uint32_t sram_off = 0x0c14;   /* the MAC-address mailbox, aligned */
    const uint32_t value = 0xdeadbeef;

    bcm570x_aim_window(qts);

    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MEMWIN_BASEADDR, sram_off);
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MEMWIN_DATA, value);

    /* Back through the indirect window. */
    g_assert_cmphex(qtest_readl(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MEMWIN_DATA),
                    ==, value);

    /* And through the BAR0 SRAM window at 0x8000 + (offset & 0x7FFF). */
    g_assert_cmphex(bar_readl(qts, BCM_SRAM_WINDOW + (sram_off & 0x7fffu)), ==,
                    value);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/bcm570x/pci-four-tuple", NULL, test_pci_four_tuple);
    qtest_add_data_func("/bcm570x/indirect-register-window", NULL,
                        test_indirect_register_window);
    qtest_add_data_func("/bcm570x/indirect-memory-window", NULL,
                        test_indirect_memory_window);

    return g_test_run();
}
