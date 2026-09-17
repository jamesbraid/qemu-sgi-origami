/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The four crosstalk board classes: widget identity, PCI population per
 * device number, and the interrupt chain from a device to the board's
 * widget interrupt, parameterised over every class so the same contract
 * runs against all four (the sibling-parity rule).
 *
 * sgi-ip29-io, sgi-baseio and sgi-io8 are onboard-only (user_creatable
 * off): a real system never lets a user plug one in, so `-device` cannot
 * create them directly. sgi-pci-expansion is the plan's named `-device`
 * demonstrator but is not yet reachable that way either (see its own
 * header comment for why), so it is user_creatable off too for now.
 * sgi-board-test-fixture stands in for what a machine builder does with
 * one of them, the same way sgi-xtalk-board-fixture stands in for a
 * module's Crossbow in the base class's own qtest; see
 * hw/mips/sgi/board-test-fixture.c. All four go through it here, so one
 * code path covers all four.
 *
 * Every board's internal Bridge or XBridge chip shares one register
 * layout (hw/pci-host/sgi-bridge.c and sgi-xbridge.c: "Same programming
 * model as the Bridge with the deltas the XBridge datasheet lists"): a
 * 32-bit value per eight-byte register slot, read and written at the
 * slot's own offset plus four; type-0 PCI configuration space at
 * 0x020000 plus 0x1000 per device; a device's own data window,
 * independent of how its BAR is currently mapped, at 0x200000 plus
 * 0x200000 for device 0 or 1, or plus 0x100000 for device 2 and up from
 * 0x600000; and BRIDGE_DEVICE(n) at 0x200 plus eight times n, the window
 * control register a probe card's BAR is mapped through the same way
 * test_config_pio() in sn1-xbridge-test.c does it. This file reuses that
 * pattern rather than restating it.
 *
 * Interrupt observation differs by chip family because Bridge exposes no
 * GPIO at all -- only its own register 0x100, which reflects every
 * slot's PCI INTA automatically (OpenBSD's rule, confirmed by a real
 * Origin 200's two SCSI controllers) -- while XBridge's head re-drives
 * every raw PCI interrupt as its own "pci-intx" GPIO output regardless of
 * board wiring, observable with qtest_irq_intercept_out_named() the way
 * sn1-xbridge-test.c already does.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"
#include "qobject/qdict.h"

/*
 * The board re-parents from the fixture to its module container at its
 * own realize (hw/mips/sgi/xtalk-board.c, sgi_xtalk_board_reparent()),
 * under a name from its own qdev id -- unset here, so its slot number,
 * matching every board_start() command line below.
 */
#define BOARD "/machine/module0/slot8"

#define BASE 0x10000000ULL

#define REG_IDENTIFICATION 0x000
#define REG_INT_STATUS     0x100
#define REG_DEVICE(n)      (0x200 + 8 * (n))
#define REG_READ_OFFSET    4

#define CFG0_BASE           0x020000
#define CFG0_DEVICE_SIZE    0x001000
#define CFG0_DEVICE(n)      (CFG0_BASE + (n) * CFG0_DEVICE_SIZE)
#define CFG0_REVISION_ID    0x08

#define PCI_NUM_PINS 4

/* A device's own data window: two 2 MB windows, then six of 1 MB. */
static uint64_t window_offset(unsigned int device)
{
    return device < 2 ? 0x200000 + device * 0x200000
                      : 0x600000 + (device - 2) * 0x100000;
}

/* A free device number on every board's bus, fixed or empty alike. */
#define PROBE_DEVICE 3
#define PROBE_BAR    0x01000000ULL
/*
 * Matches test_config_pio()'s own value: 32-bit PCI memory space, base
 * field set from PROBE_BAR at the window's 1 MB granularity.
 */
#define PROBE_WINDOW_CONTROL 0x3010
#define PROBE_REG_INTERRUPT  0x10

typedef struct FixedDevice {
    unsigned int device;
    uint8_t revision;
} FixedDevice;

typedef struct BoardCase {
    const char *subtest;
    const char *type;
    /* The board's own internal chip, as a QOM child of BOARD. */
    const char *chip;
    /*
     * Which of the chip's sysbus MMIO regions carries the fixed devices'
     * bus (0 unless a board's chip exposes more than one, as XBridge
     * does).
     */
    unsigned int mmio_region;
    const char *bus;
    uint32_t identification;
    const FixedDevice *fixed;
    size_t fixed_count;
    bool xbridge;
} BoardCase;

static const FixedDevice ip29_io_fixed[] = {
    { 0, 5 }, { 1, 5 }, { 2, 1 },
};

static const FixedDevice baseio_fixed[] = {
    { 0, 5 }, { 1, 5 }, { 2, 1 },
};

static const FixedDevice io8_fixed[] = {
    { 1, 6 }, { 4, 1 }, { 5, 0x10 },
};

static const BoardCase board_cases[] = {
    {
        .subtest = "ip29-io",
        .type = "sgi-ip29-io",
        .chip = "bridge",
        .bus = "pci", /* Bridge's own unnamed default (sgi-bridge.c). */
        /*
         * revision 4, part 0xc002 and manufacturer 0x036 (Bridge's own
         * defaults, both unset by this board): (4 << 28) | (0xc002 << 12)
         * | (0x036 << 1) | 1.
         */
        .identification = 0x4c00206dU,
        .fixed = ip29_io_fixed,
        .fixed_count = G_N_ELEMENTS(ip29_io_fixed),
    },
    {
        .subtest = "baseio",
        .type = "sgi-baseio",
        .chip = "bridge",
        .bus = "pci",
        /* Same revision and Bridge defaults as sgi-ip29-io. */
        .identification = 0x4c00206dU,
        .fixed = baseio_fixed,
        .fixed_count = G_N_ELEMENTS(baseio_fixed),
    },
    {
        .subtest = "io8",
        .type = "sgi-io8",
        .chip = "chip",
        /*
         * Sysbus MMIO region 0 is the whole crosstalk space
         * (hw/pci-host/sgi-xbridge.c, chip_init()); 1, 2 and 3 are the
         * per-widget views in chip_view_widget[] order (crossbar, head E,
         * head F), so head F is region 3.
         */
        .mmio_region = 3,
        /*
         * A head's bus is named from its own unset qdev id, falling back
         * to its QOM child name (sgi-xbridge.c, xbridge_realize()): the
         * chip's own head-f child, not a device id this test ever sets.
         */
        .bus = "head-f.0",
        /*
         * Head F's own identification: unconfigured by this board, so
         * XBridge's fixed defaults apply (part 0xd002, manufacturer
         * 0x024, revision 2), the same value sn1-xbridge-test.c's
         * test_registers() observes on a bare sgi-xbridge-head.
         */
        .identification = 0x2d002049U,
        .fixed = io8_fixed,
        .fixed_count = G_N_ELEMENTS(io8_fixed),
        .xbridge = true,
    },
    {
        .subtest = "pci-expansion",
        .type = "sgi-pci-expansion",
        .chip = "bridge",
        .bus = "pci",
        /*
         * Bridge's own unmodified defaults: revision 1, part 0xc002,
         * manufacturer 0x036: (1 << 28) | (0xc002 << 12) | (0x036 << 1)
         * | 1. No fixed devices; this board is a bare Bridge.
         */
        .identification = 0x1c00206dU,
        .fixed = NULL,
        .fixed_count = 0,
    },
};

static uint32_t reg_read(QTestState *qts, unsigned int offset)
{
    return qtest_readl(qts, BASE + offset + REG_READ_OFFSET);
}

static void reg_write(QTestState *qts, unsigned int offset, uint32_t value)
{
    qtest_writel(qts, BASE + offset + REG_READ_OFFSET, value);
}

static QTestState *board_start(const BoardCase *tc)
{
    g_autofree char *cmdline = g_strdup_printf(
        "-M none -cpu R10000 -S "
        "-device sgi-board-test-fixture,id=wrap,board-type=%s,module=0,"
        "slot=8 "
        "-device sgi-bridge-dma-test,bus=%s,addr=%u",
        tc->type, tc->bus, PROBE_DEVICE);
    QTestState *qts = qtest_init(cmdline);
    g_autofree char *chip_path = g_strdup_printf("%s/%s", BOARD, tc->chip);

    qtest_sysbus_mmio_map(qts, chip_path, tc->mmio_region, BASE);
    return qts;
}

static void test_widget_identity(gconstpointer data)
{
    const BoardCase *tc = data;
    QTestState *qts = board_start(tc);

    g_assert_cmphex(reg_read(qts, REG_IDENTIFICATION), ==,
                    tc->identification);
    qtest_quit(qts);
}

/*
 * A sub-word config access takes the crosstalk PIO byte-lane correction
 * meant for a device's own data window (hw/pci-host/sgi-xbridge.c,
 * xbridge_pio()'s offset ^= width - size, which Bridge's own config
 * dispatch shares) and returns a byte from the wrong lane. A whole-word
 * read is exempt (no correction at width == size) and qtest_readl()
 * itself assembles it back into a normal host-order value, so every
 * config field this test reads comes from one, matching test_config_pio()
 * in sn1-xbridge-test.c reading identification the same way.
 */
static uint32_t cfg_read(QTestState *qts, unsigned int device,
                         unsigned int offset)
{
    return qtest_readl(qts, BASE + CFG0_DEVICE(device) + offset);
}

static void test_pci_population(gconstpointer data)
{
    const BoardCase *tc = data;
    QTestState *qts = board_start(tc);
    size_t i;

    for (i = 0; i < tc->fixed_count; i++) {
        unsigned int device = tc->fixed[i].device;
        uint16_t vendor = cfg_read(qts, device, 0) & 0xffff;
        uint8_t revision = cfg_read(qts, device, CFG0_REVISION_ID) & 0xff;

        g_assert_cmphex(vendor, !=, 0xffff);
        g_assert_cmphex(revision, ==, tc->fixed[i].revision);
    }
    /* The probe card itself answers where this test put it. */
    g_assert_cmphex(cfg_read(qts, PROBE_DEVICE, 0) & 0xffff, !=, 0xffff);
    qtest_quit(qts);
}

static void arm_probe(QTestState *qts)
{
    qtest_writel(qts, BASE + CFG0_DEVICE(PROBE_DEVICE) + PCI_BASE_ADDRESS_0,
                PROBE_BAR);
    qtest_writel(qts, BASE + CFG0_DEVICE(PROBE_DEVICE) + PCI_COMMAND,
                PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
    reg_write(qts, REG_DEVICE(PROBE_DEVICE), PROBE_WINDOW_CONTROL);
}

static void probe_set_interrupt(QTestState *qts, bool level)
{
    qtest_writel(qts, BASE + window_offset(PROBE_DEVICE) +
                PROBE_REG_INTERRUPT, level ? 1 : 0);
}

static void test_interrupt_chain(gconstpointer data)
{
    const BoardCase *tc = data;
    QTestState *qts = board_start(tc);

    arm_probe(qts);

    if (tc->xbridge) {
        g_autofree char *chip_path =
            g_strdup_printf("%s/%s", BOARD, tc->chip);
        /*
         * The probe card's own INTA, raw output index device * 4 + 0
         * (PCI_NUM_PINS per device, pin A first): re-driven as the
         * chip's own "head-f-pci-intx" regardless of whether this
         * board's own fixed devices wire anything to it.
         */
        unsigned int line = PROBE_DEVICE * PCI_NUM_PINS;

        qtest_irq_intercept_out_named(qts, chip_path, "head-f-pci-intx");
        g_assert_false(qtest_get_irq(qts, line));
        probe_set_interrupt(qts, true);
        g_assert_true(qtest_get_irq(qts, line));
        probe_set_interrupt(qts, false);
        g_assert_false(qtest_get_irq(qts, line));
    } else {
        g_assert_cmphex(reg_read(qts, REG_INT_STATUS) &
                        (1U << PROBE_DEVICE), ==, 0);
        probe_set_interrupt(qts, true);
        g_assert_cmphex(reg_read(qts, REG_INT_STATUS) &
                        (1U << PROBE_DEVICE), ==, 1U << PROBE_DEVICE);
        probe_set_interrupt(qts, false);
        g_assert_cmphex(reg_read(qts, REG_INT_STATUS) &
                        (1U << PROBE_DEVICE), ==, 0);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    size_t i;

    g_test_init(&argc, &argv, NULL);

    for (i = 0; i < G_N_ELEMENTS(board_cases); i++) {
        const BoardCase *tc = &board_cases[i];

        /*
         * Not freed: g_test_add_data_func() keeps the path for the life
         * of the suite, which is the life of this process.
         */
        qtest_add_data_func(
            g_strdup_printf("/sgi-board/%s/widget-identity", tc->subtest),
            tc, test_widget_identity);
        qtest_add_data_func(
            g_strdup_printf("/sgi-board/%s/pci-population", tc->subtest),
            tc, test_pci_population);
        qtest_add_data_func(
            g_strdup_printf("/sgi-board/%s/interrupt-chain", tc->subtest),
            tc, test_interrupt_chain);
    }

    return g_test_run();
}
