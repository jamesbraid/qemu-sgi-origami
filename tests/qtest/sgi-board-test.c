/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-baseio's three presets, and sgi-pci-expansion: widget identity, PCI
 * population per device number, and the interrupt chain from a device to the
 * widget interrupt, parameterised over every case so the same contract runs
 * against all four (the sibling-parity rule).
 *
 * A preset is not itself user_creatable-reachable the way a real carrier
 * builds it (a bare Bridge or XBridge, then sgi-baseio on its bus) --
 * sgi-board-test-fixture builds exactly that pair for the "ip29", "io6" and
 * "io8" presets, the same way sgi-xio-board-fixture stands in for a
 * module's Crossbow in the XIO board base class's own qtest; see
 * tests/qtest/board-test-fixture.c. sgi-pci-expansion is also
 * `-device`-creatable now (see its own header comment for the spare fabric
 * proxy that makes it so), but this file still runs it through the same
 * fixture's "module"/"slot"/"fabric" path, matching the other three cases;
 * sgi-board-test.c's own attach-pending case, and sn-builder- test.c's
 * test_pci_expansion_attaches_via_spare_fabric and test_xio_
 * baseio_attaches_via_spare_fabric (which boot the real -M origin2000
 * machine, unlike this file's bare fixture), cover the real `-device` command
 * line instead.
 *
 * Every case's internal Bridge or XBridge chip shares one register layout
 * (hw/pci-host/sgi-bridge.c and sgi-xbridge.c): a 32-bit value per eight-byte
 * register slot, read and written at the slot's own offset plus four; type-0
 * PCI configuration space at 0x020000 plus 0x1000 per device; a device's own
 * data window, independent of how its BAR is currently mapped, at 0x200000
 * plus 0x200000 for device 0 or 1, or plus 0x100000 for device 2 and up from
 * 0x600000; and BRIDGE_DEVICE(n) at 0x200 plus eight times n, the window
 * control register a probe card's BAR is mapped through the same way
 * test_config_pio() in sn1-xbridge-test.c does it. This file reuses that
 * pattern rather than restating it.
 *
 * Interrupt observation differs by chip family because Bridge exposes no GPIO
 * at all. Register 0x100 reflects every slot's PCI INTA automatically
 * (OpenBSD's rule, confirmed by a real Origin 200's two SCSI
 * controllers). XBridge's head re-drives every raw PCI interrupt as
 * its own "pci-intx" GPIO output regardless of board wiring, observable with
 * qtest_irq_intercept_out_named() the way sn1-xbridge-test.c already does.
 *
 * Two more sections share this file rather than getting one of their own:
 * sgi-baseio also resolves its "chardev-a" and "nvram-drive" properties
 * against the command line's own chardev and drive backends a second time,
 * inside its own preset build() (hw/mips/sgi/baseio.c's header comment
 * explains why: the machine builder hands it an id, not a claimed backend);
 * an id that names no real backend must be refused at realize, naming the id,
 * rather than leaving the IOC3 silently built without it. And
 * sgi-xio-board, the base class sgi-pci-expansion derives from, has its own
 * property validation, re-parenting under its module, and the deferred attach
 * hook that lets a `-device` board reach a Crossbow that must realize only
 * once every board is in; the attach-pending case uses
 * sgi-xio-board-fixture, a test-only stand-in for a machine builder that
 * owns one Crossbow and drives the same machine-init-done attach path a real
 * builder would.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"
#include "qobject/qdict.h"

/*
 * sgi-pci-expansion re-parents from the fixture to its module container at
 * its own realize (hw/mips/sgi/xio-board.c, sgi_xio_board_reparent()), under
 * a name from its own qdev id. That id is unset here, so it uses its
 * slot number, matching
 * board_start()'s own command line for it. A preset built through the
 * fixture's own "board-type=sgi-baseio" path does not re-parent at all
 * (sgi-baseio is not an sgi-xio-board), so its chip and the sgi-baseio behind
 * it stay exactly where the fixture put them.
 */
#define BOARD_REPARENTED "/machine/module0/slot8"
#define FIXTURE          "/machine/peripheral/wrap"

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
 * Use 32-bit PCI memory space and PROBE_BAR at 1 MiB window granularity,
 * as in sn1-xbridge-test.c:test_config_pio().
 */
#define PROBE_WINDOW_CONTROL 0x3010
#define PROBE_REG_INTERRUPT  0x10

typedef struct FixedDevice {
    unsigned int device;
    uint8_t revision;
} FixedDevice;

typedef struct BoardCase {
    const char *subtest;
    /*
     * Use board-type=sgi-baseio,preset=<preset> for three BaseIO presets;
     * board-type=<type> with module/slot/fabric for PCI expansion.
     */
    const char *preset;
    const char *type;
    bool reparents;
    /* The chip a real carrier would build, as a QOM child of its path. */
    const char *chip;
    /*
     * Select the sysbus MMIO region carrying the fixed-device bus: 0 unless
     * the chip exposes multiple regions, as XBridge does.
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

static const FixedDevice io6_fixed[] = {
    { 0, 5 }, { 1, 5 }, { 2, 1 },
};

static const FixedDevice io8_fixed[] = {
    { 1, 6 }, { 4, 1 }, { 5, 0x10 },
};

static const BoardCase board_cases[] = {
    {
        .subtest = "ip29",
        .preset = "ip29",
        .chip = "chip",
        .bus = "pci", /* Bridge's own unnamed default (sgi-bridge.c). */
        /*
         * Bare Bridge defaults: revision 4, part 0xc002, manufacturer 0x036;
         * (4 << 28) | (0xc002 << 12) | (0x036 << 1) | 1.
         */
        .identification = 0x4c00206dU,
        .fixed = ip29_io_fixed,
        .fixed_count = G_N_ELEMENTS(ip29_io_fixed),
    },
    {
        .subtest = "io6",
        .preset = "io6",
        .chip = "chip",
        .bus = "pci",
        /* Same revision and Bridge defaults as the "ip29" preset. */
        .identification = 0x4c00206dU,
        .fixed = io6_fixed,
        .fixed_count = G_N_ELEMENTS(io6_fixed),
    },
    {
        .subtest = "io8",
        .preset = "io8",
        .chip = "chip",
        /*
         * XBridge region 0 covers crosstalk; regions 1-3 are crossbar, head E
         * and head F widget views, so head F is region 3.
         */
        .mmio_region = 3,
        /*
         * A head's bus is named from its own unset qdev id, falling back to
         * its QOM child name (sgi-xbridge.c, xbridge_realize()): the chip's
         * own head-f child, not a device id this test ever sets.
         */
        .bus = "head-f.0",
        /*
         * Head F's own identification: unconfigured by this test's own bare
         * chip, so XBridge's fixed defaults apply (part 0xd002, manufacturer
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
        .reparents = true,
        .chip = "bridge",
        .bus = "pci",
        /*
         * Bare Bridge defaults: revision 1, part 0xc002, manufacturer 0x036;
         * (1 << 28) | (0xc002 << 12) | (0x036 << 1) | 1. No fixed devices.
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
    g_autofree char *fixture_args = tc->reparents ?
        g_strdup_printf("board-type=%s,module=0,slot=8", tc->type) :
        g_strdup_printf("board-type=sgi-baseio,preset=%s", tc->preset);
    g_autofree char *cmdline = g_strdup_printf(
        "-M none -cpu R10000 -S "
        "-device sgi-board-test-fixture,id=wrap,%s "
        "-device sgi-bridge-dma-test,bus=%s,addr=%u",
        fixture_args, tc->bus, PROBE_DEVICE);
    QTestState *qts = qtest_init(cmdline);
    g_autofree char *chip_path = g_strdup_printf(
        "%s/%s", tc->reparents ? BOARD_REPARENTED : FIXTURE, tc->chip);

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
 * A sub-word config access takes the crosstalk PIO byte-lane correction meant
 * for a device's own data window (hw/pci-host/sgi-xbridge.c, xbridge_pio()'s
 * offset ^= width - size, which Bridge's own config dispatch shares) and
 * returns a byte from the wrong lane. A whole-word read is exempt (no
 * correction at width == size) and qtest_readl() itself assembles it back
 * into a normal host-order value, so every config field this test reads comes
 * from one, matching test_config_pio() in sn1-xbridge-test.c reading
 * identification the same way.
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
        g_autofree char *chip_path = g_strdup_printf(
            "%s/%s", tc->reparents ? BOARD_REPARENTED : FIXTURE, tc->chip);
        /*
         * Probe INTA is raw output device * 4 + 0; XBridge drives
         * head-f-pci-intx regardless of fixed-device wiring.
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

/*
 * BaseIO rejects an unknown chardev or drive backend ID at realize
 * and names the missing ID.
 */

static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output, message));
}

static void assert_backend_id_rejected(const char *preset,
                                       const char *property,
                                       const char *unresolved_id)
{
    g_autofree char *device = g_strdup_printf(
        "sgi-board-test-fixture,board-type=sgi-baseio,preset=%s,%s=%s",
        preset, property, unresolved_id);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "none", "-display", "none",
        "-device", device, NULL
    };

    assert_qemu_rejects(args, unresolved_id);
}

static void test_ip29_unresolved_chardev(const void *data)
{
    assert_backend_id_rejected("ip29", "chardev-a", "no-such-chardev");
}

static void test_ip29_unresolved_drive(const void *data)
{
    assert_backend_id_rejected("ip29", "nvram-drive", "no-such-drive");
}

static void test_io6_unresolved_chardev(const void *data)
{
    assert_backend_id_rejected("io6", "chardev-a", "no-such-chardev");
}

static void test_io6_unresolved_drive(const void *data)
{
    assert_backend_id_rejected("io6", "nvram-drive", "no-such-drive");
}

static void test_io8_unresolved_chardev(const void *data)
{
    assert_backend_id_rejected("io8", "chardev-a", "no-such-chardev");
}

static void test_io8_unresolved_drive(const void *data)
{
    assert_backend_id_rejected("io8", "nvram-drive", "no-such-drive");
}

/*
 * sgi-xio-board: property validation, re-parenting under its module, and
 * the deferred attach hook that lets a `-device` board reach a Crossbow that
 * must realize only once every board is in.
 *
 * -M none's main system bus refuses to hot-plug a sysbus device at all (no
 * dynamic-sysbus allowance is wired up without a machine, and this packet
 * adds none), so every device here is a cold-plugged `-device` on the qtest
 * command line, never a QMP device_add sent afterward. A board whose
 * properties fail realize takes the whole process down as a fatal
 * command-line error, so the invalid-property cases run as trapped
 * subprocesses and check the process's own failure, the way
 * test_parent_l1_invalid_region() in sn1-bedrock-test.c checks a different
 * device's realize-time refusal.
 *
 * The attach-pending case uses sgi-xio-board-fixture, a test-only stand-in
 * for a machine builder: it owns one Crossbow, created unrealized, and (with
 * auto-attach left at its default) calls sgi_xio_board_attach_pending()
 * from the real machine-init-done notifier the way a builder would, then
 * realizes the Crossbow. A second fixture with auto-attach=false does neither
 * on its own; its "realize-now" and "retry" properties let the qtest drive
 * the same two steps by hand, which is the only way to put a Crossbow into
 * "already realized, nothing attached" without a hot-plug -M none refuses.
 */

/*
 * Matches SGI_XIO_BOARD_FIXTURE_SPAN in tests/qtest/xio-test-board.c:
 * how much address space the fixture's Crossbow gives each widget.
 */
#define WIDGET_SPAN (32ULL * 1024 * 1024)

/*
 * fixture0 keeps slot 8 alive beside probe slot 9; a link never
 * routes to itself.
 */
#define BOARD_WIDGET 8
#define PROBE_LINK   1 /* widget 9, the second live port */

#define CPU_BASE    0x40000000ULL
#define TARGET_BASE 0x80000000ULL

#define FIXTURE0 "/machine/peripheral/fixture0"
#define FIXTURE1 "/machine/peripheral/fixture1"

static char *qom_get_str(QTestState *qts, const char *path, const char *prop)
{
    QDict *r = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                         "{'path': %s, 'property': %s}}", path, prop);
    char *value = g_strdup(qdict_get_str(r, "return"));

    qobject_unref(r);
    return value;
}

static void test_bad_module(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*a crosstalk board placed by widget number requires a module "
            "index*");
        return;
    }
    /* No module= at all: the property keeps its unset sentinel. */
    s = qtest_init("-M none -device sgi-xio-test-board,id=b,slot=8 -S");
    qtest_quit(s);
}

static void test_bad_slot_low(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*a crosstalk board slot must name a Crossbow widget*");
        return;
    }
    /* Below the Crossbow's first widget. */
    s = qtest_init(
        "-M none -device sgi-xio-test-board,id=b,module=0,slot=7 -S");
    qtest_quit(s);
}

static void test_bad_slot_high(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*a crosstalk board slot must name a Crossbow widget*");
        return;
    }
    /* At and beyond the last one. */
    s = qtest_init(
        "-M none -device sgi-xio-test-board,id=b,module=0,slot=16 -S");
    qtest_quit(s);
}

static void test_slot_taken_twice(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*a crosstalk board already occupies module 0 slot 8*");
        return;
    }
    s = qtest_init(
        "-M none -device sgi-xio-test-board,id=a,module=0,slot=8 "
        "-device sgi-xio-test-board,id=b,module=0,slot=8 -S");
    qtest_quit(s);
}

static void test_slot_reused_in_another_module(void)
{
    /*
     * The same slot is free in another module; test the successful launch
     * directly.
     */
    QTestState *qts = qtest_init(
        "-M none -device sgi-xio-test-board,id=a,module=0,slot=8 "
        "-device sgi-xio-test-board,id=b,module=1,slot=8 -S");

    g_assert_true(qtest_qom_get_bool(qts, "/machine/module0/a", "realized"));
    g_assert_true(qtest_qom_get_bool(qts, "/machine/module1/b", "realized"));
    qtest_quit(qts);
}

static void test_reparent(void)
{
    QTestState *qts = qtest_init(
        "-M none -device sgi-xio-test-board,id=solo,module=3,slot=10 -S");

    g_assert_true(
        qtest_qom_get_bool(qts, "/machine/module3/solo", "realized"));
    qtest_quit(qts);
}

/*
 * A board built before the machine-init-done notifier fires attaches through
 * sgi_xio_board_attach_pending() and is reachable through the Crossbow once
 * it realizes; a board whose Crossbow the qtest realizes by hand first
 * (standing in for one that already realized before this board could reach
 * it) is refused when the hook is then run against it.
 */
static void test_attach_pending(void)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S "
        "-device sgi-xio-board-fixture,id=fixture0,module=0 "
        "-device sgi-xio-test-board,id=board0,module=0,slot=%d "
        "-device sgi-xio-board-fixture,id=fixture1,module=1,"
        "auto-attach=false "
        "-device sgi-xio-test-board,id=board1,module=1,slot=8",
        BOARD_WIDGET);
    g_autofree char *error = NULL;

    /*
     * fixture0's machine-init-done notifier runs before the first QMP reply.
     */
    g_assert_true(qtest_qom_get_bool(qts, FIXTURE0, "attached"));
    error = qom_get_str(qts, FIXTURE0, "error");
    g_assert_cmpstr(error, ==, "");

    /*
     * Write through probe widget 9's view of widget 8; read the same RAM
     * through the target mapping.
     */
    qtest_sysbus_mmio_map(qts, FIXTURE0 "/crossbow", PROBE_LINK, CPU_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/module0/board0/widget-module0-slot8",
                          0, TARGET_BASE);
    qtest_writeq(qts, CPU_BASE + BOARD_WIDGET * WIDGET_SPAN,
                0x1122334455667788ULL);
    g_assert_cmphex(qtest_readq(qts, TARGET_BASE), ==,
                    0x1122334455667788ULL);

    /*
     * board1 realized and re-parented without attaching; fixture1 has
     * auto-attach=false and its Crossbow remains unrealized.
     */
    g_assert_true(
        qtest_qom_get_bool(qts, "/machine/module1/board1", "realized"));
    g_assert_false(qtest_qom_get_bool(qts, FIXTURE1, "attached"));

    /*
     * Put fixture1's Crossbow where a real one would be if it had already
     * realized before board1 could reach it, then run the same hook board1
     * was always going to need.
     */
    qtest_qom_set_bool(qts, FIXTURE1, "realize-now", true);
    qtest_qom_set_bool(qts, FIXTURE1, "retry", true);
    g_free(error);
    error = qom_get_str(qts, FIXTURE1, "error");
    g_assert_true(*error != '\0');

    /* fixture0's earlier success is untouched by fixture1's failure. */
    g_assert_true(qtest_qom_get_bool(qts, FIXTURE0, "attached"));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    size_t i;

    g_test_init(&argc, &argv, NULL);

    for (i = 0; i < G_N_ELEMENTS(board_cases); i++) {
        const BoardCase *tc = &board_cases[i];

        /*
         * g_test_add_data_func() owns the path for the process lifetime.
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

    qtest_add_data_func("/sgi-board/ip29/unresolved-chardev-refused", NULL,
                        test_ip29_unresolved_chardev);
    qtest_add_data_func("/sgi-board/ip29/unresolved-drive-refused", NULL,
                        test_ip29_unresolved_drive);
    qtest_add_data_func("/sgi-board/io6/unresolved-chardev-refused", NULL,
                        test_io6_unresolved_chardev);
    qtest_add_data_func("/sgi-board/io6/unresolved-drive-refused", NULL,
                        test_io6_unresolved_drive);
    qtest_add_data_func("/sgi-board/io8/unresolved-chardev-refused", NULL,
                        test_io8_unresolved_chardev);
    qtest_add_data_func("/sgi-board/io8/unresolved-drive-refused", NULL,
                        test_io8_unresolved_drive);

    qtest_add_func("/sgi-xio-board/bad-module", test_bad_module);
    qtest_add_func("/sgi-xio-board/bad-slot-low", test_bad_slot_low);
    qtest_add_func("/sgi-xio-board/bad-slot-high", test_bad_slot_high);
    qtest_add_func("/sgi-xio-board/slot-taken-twice",
                  test_slot_taken_twice);
    qtest_add_func("/sgi-xio-board/slot-reused-in-another-module",
                  test_slot_reused_in_another_module);
    qtest_add_func("/sgi-xio-board/reparent", test_reparent);
    qtest_add_func("/sgi-xio-board/attach-pending", test_attach_pending);

    return g_test_run();
}
