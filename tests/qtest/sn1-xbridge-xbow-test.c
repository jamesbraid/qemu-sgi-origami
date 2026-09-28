/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * XBridge-mode crossbar at widget 0, with two port views to distinguish
 * source-port reporting from packet routing; no Origin 300 model.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"

#define XBOW "/machine/peripheral/xbow"

/* Port views: what a reader entering on port A or port E sees. */
#define PORT_A 0xa
#define PORT_E 0xe
#define BASE_A 0x10000000ULL
#define BASE_E 0x11000000ULL

/* Packet destinations for the links the crossbar can address. */
#define TARGET_A 0x20000000ULL
#define TARGET_E 0x21000000ULL
#define SCRATCH 0x8000

/* Registers sit in the low half of an eight-byte slot: offset plus four. */
#define LINK(n) (0x100 + ((n) - 8) * 0x40)

#define ID_REVISION 2
#define ID_PART 0xd000
#define IDENTIFICATION ((ID_REVISION << 28) | (ID_PART << 12) | 1)

static uint32_t reg_read_at(QTestState *qts, uint64_t base, unsigned off)
{
    return qtest_readl(qts, base + off + 4);
}

static void reg_write_at(QTestState *qts, uint64_t base, unsigned off,
                         uint32_t value)
{
    qtest_writel(qts, base + off + 4, value);
}

static uint32_t reg_read(QTestState *qts, unsigned off)
{
    return reg_read_at(qts, BASE_A, off);
}

static void reg_write(QTestState *qts, unsigned off, uint32_t value)
{
    reg_write_at(qts, BASE_A, off, value);
}

/* The IP45 population: the Bedrock on A and the two heads on E and F. */
static QTestState *xbow_start(const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors "
        "-device sgi-xtalk-test-target,id=targeta "
        "-device sgi-xtalk-test-target,id=targete "
        "-device sgi-xbridge-xbow,id=xbow,links-present=0xc4,"
        "target-a=/machine/peripheral/targeta/targeta.ram[0],"
        "target-e=/machine/peripheral/targete/targete.ram[0] %s", extra);

    qtest_sysbus_mmio_map(qts, XBOW, PORT_A - 8, BASE_A);
    qtest_sysbus_mmio_map(qts, XBOW, PORT_E - 8, BASE_E);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/targeta", 0, TARGET_A);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/targete", 0, TARGET_E);
    return qts;
}

static void test_identification(void)
{
    QTestState *qts = xbow_start("");

    g_assert_cmphex(reg_read(qts, 0), ==, IDENTIFICATION);
    g_assert_cmphex(reg_read_at(qts, BASE_E, 0), ==, IDENTIFICATION);
    /* Read-only: the write is an access error and changes nothing. */
    reg_write(qts, 0, 0);
    g_assert_cmphex(reg_read(qts, 0), ==, IDENTIFICATION);
    g_assert_cmphex(reg_read(qts, 8) & (1U << 5), ==, 1U << 5);
    qtest_quit(qts);
}

static void test_reset_values(void)
{
    QTestState *qts = xbow_start("");
    unsigned n;

    g_assert_cmphex(reg_read(qts, 0x10), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x18), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0x80);
    g_assert_cmphex(reg_read(qts, 0x28), ==, 0xfffff);
    g_assert_cmphex(reg_read(qts, 0x30), ==, 0x00080000);
    g_assert_cmphex(reg_read(qts, 0x38), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x40), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x48), ==, 0x03ff1810);
    g_assert_cmphex(reg_read(qts, 0x58), ==, 2);
    g_assert_cmphex(reg_read(qts, 0x60), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x68), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x70), ==, 2);
    g_assert_cmphex(reg_read(qts, 0x78), ==, 0xff);
    for (n = 8; n <= 0xf; n++) {
        g_assert_cmphex(reg_read(qts, 0x80 + (n - 8) * 8), ==, 0xff);
        g_assert_cmphex(reg_read(qts, LINK(n) + 0x08), ==, 0x00080000);
        g_assert_cmphex(reg_read(qts, LINK(n) + 0x18), ==, 0);
        g_assert_cmphex(reg_read(qts, LINK(n) + 0x20), ==, 0);
    }
    g_assert_cmphex(reg_read(qts, 0xc0), ==, 0);
    /* Status holds no state at reset beyond the reader's own port. */
    g_assert_cmphex(reg_read(qts, 8) & ~0x3c0U, ==, 0);
    qtest_quit(qts);
}

static void test_source_port(void)
{
    QTestState *qts = xbow_start("");

    g_assert_cmphex((reg_read_at(qts, BASE_A, 8) >> 6) & 0xf, ==, PORT_A);
    g_assert_cmphex((reg_read_at(qts, BASE_E, 8) >> 6) & 0xf, ==, PORT_E);
    /* The field is not state: the clearing alias reports it too. */
    g_assert_cmphex((reg_read_at(qts, BASE_E, 0x50) >> 6) & 0xf, ==, PORT_E);
    qtest_quit(qts);
}

static void test_control_and_destination(void)
{
    QTestState *qts = xbow_start("");

    reg_write(qts, 0x20, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0xfe);
    reg_write(qts, 0x20, 0);
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0);
    /* Vector, target and address are writable; bit 19 is fixed at one. */
    reg_write(qts, 0x30, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x30), ==, 0xff0fffff);
    reg_write(qts, 0x30, 0);
    g_assert_cmphex(reg_read(qts, 0x30), ==, 0x00080000);
    reg_write(qts, 0x38, 0x12345678);
    g_assert_cmphex(reg_read(qts, 0x38), ==, 0x12345678);
    reg_write(qts, 0x28, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x28), ==, 0xfffff);
    reg_write(qts, 0x58, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x58), ==, 0x3f);
    /* Only the three-bit link select of a performance counter is writable. */
    reg_write(qts, 0x60, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x60), ==, 0x00700000);
    /* Pulse and offset store; the data bits belong to the one-wire path. */
    reg_write(qts, 0x70, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x70) & 0xffffc, ==, 0xffffc);
    qtest_quit(qts);
}

static void test_lock(void)
{
    QTestState *qts = xbow_start("");

    reg_write(qts, 0xc0, 0x5a);
    g_assert_cmphex(reg_read(qts, 0xc0), ==, 0x5a);
    /* A second holder is refused until the first clears. */
    reg_write_at(qts, BASE_E, 0xc0, 0x3c);
    g_assert_cmphex(reg_read(qts, 0xc0), ==, 0x5a);
    g_assert_cmphex(reg_read(qts, 0xc8), ==, 0x5a);
    reg_write(qts, 0xc8, 0);
    g_assert_cmphex(reg_read(qts, 0xc0), ==, 0);
    reg_write_at(qts, BASE_E, 0xc0, 0x3c);
    g_assert_cmphex(reg_read(qts, 0xc0), ==, 0x3c);
    qtest_quit(qts);
}

static void test_fences(void)
{
    QTestState *qts = xbow_start("");
    unsigned off;

    /*
     * Every fence bit is read/write, the link's own bit included: that a link
     * should admit itself is advice to software, not a fixed bit.
     */
    for (off = 0x78; off <= 0xb8; off += 8) {
        reg_write(qts, off, 0);
        g_assert_cmphex(reg_read(qts, off), ==, 0);
        reg_write(qts, off, off);
        g_assert_cmphex(reg_read(qts, off), ==, off);
        reg_write(qts, off, UINT32_MAX);
        g_assert_cmphex(reg_read(qts, off), ==, 0xff);
    }
    qtest_quit(qts);
}

static void test_link_registers(void)
{
    QTestState *qts = xbow_start("");
    const unsigned present = 0xc4;
    unsigned n;

    /* The per-port sets decode 0x40 apart, link 8 first. */
    reg_write(qts, 0x108, 0x00040000);
    reg_write(qts, 0x148, 0x00080000);
    reg_write(qts, 0x2c8, 0x000c0000);
    g_assert_cmphex(reg_read(qts, 0x108), ==, 0x00040000);
    g_assert_cmphex(reg_read(qts, 0x148), ==, 0x00080000);
    g_assert_cmphex(reg_read(qts, 0x2c8), ==, 0x000c0000);
    /* Writable fields read back; the force-bad one-shot always reads zero. */
    reg_write(qts, 0x108, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x108), ==, 0xbf7f01ff);
    for (n = 8; n <= 0xf; n++) {
        uint32_t status = reg_read(qts, LINK(n) + 0x10);
        uint32_t aux = reg_read(qts, LINK(n) + 0x38);
        bool attached = present & (1U << (n - 8));

        g_assert_cmpuint(!!(aux & (1U << 5)), ==, attached);
        g_assert_cmpuint(!!(status & (1U << 31)), ==, attached);
        /* An empty port fails initialization and says so. */
        g_assert_cmpuint(!!(status & (1U << 5)), ==, !attached);
        g_assert_cmpuint(!!(aux & (1U << 6)), ==, !attached);
        g_assert_cmphex(aux & (1U << 4), ==, 0);
        g_assert_cmphex(reg_read(qts, LINK(n)), ==, 0);
    }
    qtest_quit(qts);
}

static void test_eight_bit_mode(void)
{
    QTestState *qts = xbow_start("-global sgi-xbridge-xbow.links-8bit=0x40");

    g_assert_cmphex(reg_read(qts, LINK(0xe) + 0x38) & (1U << 4), ==, 1U << 4);
    g_assert_cmphex(reg_read(qts, LINK(0xa) + 0x38) & (1U << 4), ==, 0);
    qtest_quit(qts);
}

static void test_access_errors(void)
{
    QTestState *qts = xbow_start("");
    unsigned off;

    /* Each of these is an error the status register reports, not a fault. */
    g_assert_cmphex(reg_read(qts, 8) & (1U << 5), ==, 0);
    qtest_readq(qts, BASE_A + 0x20);
    g_assert_cmphex(reg_read(qts, 8) & (1U << 5), ==, 1U << 5);
    g_assert_cmphex(reg_read(qts, 0x50) & (1U << 5), ==, 1U << 5);
    g_assert_cmphex(reg_read(qts, 8) & (1U << 5), ==, 0);
    /* A 64-bit write is refused whole, not applied by halves. */
    qtest_writeq(qts, BASE_A + 0x20, UINT64_MAX);
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0x80);
    g_assert_cmphex(reg_read(qts, 0x50) & (1U << 5), ==, 1U << 5);
    for (off = 0xd0; off <= 0xf8; off += 8) {
        reg_read(qts, off);
        g_assert_cmphex(reg_read(qts, 0x50) & (1U << 5), ==, 1U << 5);
    }
    reg_read(qts, 0x300);
    g_assert_cmphex(reg_read(qts, 0x50) & (1U << 5), ==, 1U << 5);
    reg_write(qts, 0x50, 0);
    g_assert_cmphex(reg_read(qts, 0x50) & (1U << 5), ==, 1U << 5);
    reg_write(qts, LINK(0xa) + 0x28, 0);
    g_assert_cmphex(reg_read(qts, 0x50) & (1U << 5), ==, 1U << 5);
    reg_write(qts, LINK(0xa) + 0x10, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x50) & (1U << 5), ==, 1U << 5);
    g_assert_cmphex(reg_read(qts, 8) & (1U << 5), ==, 0);
    /* A read of a write-only link reset register is one as well. */
    reg_read(qts, LINK(9) + 0x30);
    g_assert_cmphex(reg_read(qts, 0x50) & (1U << 5), ==, 1U << 5);
    g_assert_cmphex(reg_read(qts, 8) & (1U << 5), ==, 0);
    /* The clearing alias returns the value it clears, once. */
    qtest_writeb(qts, BASE_A + 0x24, 0);
    g_assert_cmphex(reg_read(qts, 0x50) & (1U << 5), ==, 1U << 5);
    g_assert_cmphex(reg_read(qts, 0x50) & (1U << 5), ==, 0);
    qtest_quit(qts);
}

static void test_link_status_clear(void)
{
    QTestState *qts = xbow_start("");
    uint32_t status = reg_read(qts, LINK(0xa) + 0x10);

    /* Alive is link state, so the clearing alias returns and keeps it. */
    g_assert_cmphex(status & (1U << 31), ==, 1U << 31);
    g_assert_cmphex(reg_read(qts, LINK(0xa) + 0x28), ==, status);
    g_assert_cmphex(reg_read(qts, LINK(0xa) + 0x10), ==, status);
    /* An absent port's failure is state too. */
    status = reg_read(qts, LINK(8) + 0x10);
    g_assert_cmphex(status & (1U << 5), ==, 1U << 5);
    g_assert_cmphex(reg_read(qts, LINK(8) + 0x28), ==, status);
    g_assert_cmphex(reg_read(qts, LINK(8) + 0x10), ==, status);
    qtest_quit(qts);
}

static void test_error_command_word(void)
{
    QTestState *qts = xbow_start("");

    reg_write(qts, 0x40, 0x12345678);
    g_assert_cmphex(reg_read(qts, 0x40), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x10), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x18), ==, 0);
    qtest_quit(qts);
}

static void test_interrupt_packets(void)
{
    QTestState *qts = xbow_start("");

    reg_write(qts, 0x30, 0x2a0e0000);
    reg_write(qts, 0x38, SCRATCH);
    qtest_writeq(qts, TARGET_E + SCRATCH, 0x123456789abcdef0ULL);
    /* Disabled: the condition is recorded and nothing is sent. */
    reg_read(qts, 0xd0);
    g_assert_cmphex(reg_read(qts, 8) & (1U << 5), ==, 1U << 5);
    g_assert_cmphex(reg_read(qts, 8) & (1U << 23), ==, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET_E + SCRATCH), ==,
                    0x123456789abcdef0ULL);
    reg_read(qts, 0x50);
    /*
     * Enabled: one set packet carrying the vector, then no second one. With
     * no link conditions modeled, only the widget-0 alias can acknowledge, so
     * "clearing one leaves it outstanding" has no observable here.
     */
    reg_write(qts, 0x20, 1U << 5);
    reg_read(qts, 0xd0);
    g_assert_cmphex(reg_read(qts, 8) & (1U << 23), ==, 1U << 23);
    g_assert_cmphex(qtest_readq(qts, TARGET_E + SCRATCH), ==, 0x12a);
    qtest_writeq(qts, TARGET_E + SCRATCH, 0);
    reg_read(qts, 0xd8);
    g_assert_cmphex(qtest_readq(qts, TARGET_E + SCRATCH), ==, 0);
    /* Acknowledging the last pending condition sends the clear packet. */
    reg_read(qts, 0x50);
    g_assert_cmphex(reg_read(qts, 8) & (1U << 23), ==, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET_E + SCRATCH), ==, 0x2a);
    /* A destination the crossbar cannot reach is refused, not faulted. */
    reg_write(qts, 0x30, 0x2a0d0000);
    reg_read(qts, 0xd0);
    g_assert_cmphex(reg_read(qts, 8) & (1U << 23), ==, 1U << 23);
    reg_read(qts, 0x50);
    /* A write-only link reset read raises the same condition, and sends. */
    reg_write(qts, 0x30, 0x2a0e0000);
    qtest_writeq(qts, TARGET_E + SCRATCH, 0);
    reg_read(qts, LINK(9) + 0x30);
    g_assert_cmphex(qtest_readq(qts, TARGET_E + SCRATCH), ==, 0x12a);
    reg_read(qts, 0x50);
    qtest_quit(qts);
}

static void test_warm_reset(void)
{
    QTestState *qts = xbow_start("");

    qtest_irq_intercept_out_named(qts, XBOW, "warm-reset-out");
    reg_write(qts, 0x20, 0xfe);
    reg_write(qts, 0xc0, 0x11);
    /*
     * Port A is fenced out of widget 0: nothing resets, nothing forwards. The
     * reset behavior is specified; forwarding is unspecified. The model
     * blocks forwarding.
     */
    reg_write(qts, 0x78, 0xff & ~(1U << (PORT_A - 8)));
    qtest_set_irq_in(qts, XBOW, "warm-reset", PORT_A - 8, 1);
    qtest_set_irq_in(qts, XBOW, "warm-reset", PORT_A - 8, 0);
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0xfe);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, PORT_E - 8), ==, 0);
    /* Fenced in: the core resets and every other fenced port is reset. */
    reg_write(qts, 0x78, 0xff);
    reg_write(qts, 0x80 + (PORT_E - 8) * 8, 0xff & ~(1U << (PORT_A - 8)));
    qtest_set_irq_in(qts, XBOW, "warm-reset", PORT_A - 8, 1);
    qtest_set_irq_in(qts, XBOW, "warm-reset", PORT_A - 8, 0);
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0x80);
    g_assert_cmphex(reg_read(qts, 0xc0), ==, 0);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0xf - 8), ==, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, PORT_E - 8), ==, 0);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, PORT_A - 8), ==, 0);
    qtest_quit(qts);
}

static void test_migration_reset(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-xbridge-xbow-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from = xbow_start("");
    QTestState *to = xbow_start("-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    reg_write(from, 0x20, 0x24);
    reg_write(from, 0x30, 0x2a0e0000);
    reg_write(from, 0x38, SCRATCH);
    reg_write(from, 0xc0, 0x77);
    reg_write(from, 0x78, 0x0f);
    reg_write(from, 0x148, 0x01740000);
    reg_read(from, 0xd0);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    /* The source completes when its state is sent, not when it is loaded. */
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(reg_read(to, 0x20), ==, 0x24);
    g_assert_cmphex(reg_read(to, 0xc0), ==, 0x77);
    g_assert_cmphex(reg_read(to, 0x78), ==, 0x0f);
    g_assert_cmphex(reg_read(to, 0x148), ==, 0x01740000);
    g_assert_cmphex(reg_read(to, 8) & (1U << 5), ==, 1U << 5);
    g_assert_cmphex(reg_read(to, 8) & (1U << 23), ==, 1U << 23);
    /* The pending assertion survives: its acknowledgement still clears. */
    qtest_writeq(to, TARGET_E + SCRATCH, 0);
    reg_read(to, 0x50);
    g_assert_cmphex(qtest_readq(to, TARGET_E + SCRATCH), ==, 0x2a);
    qtest_system_reset(to);
    g_assert_cmphex(reg_read(to, 0x20), ==, 0x80);
    g_assert_cmphex(reg_read(to, 0x30), ==, 0x00080000);
    g_assert_cmphex(reg_read(to, 0xc0), ==, 0);
    g_assert_cmphex(reg_read(to, 0x78), ==, 0xff);
    g_assert_cmphex(reg_read(to, 0x148), ==, 0x00080000);
    g_assert_cmphex(reg_read(to, 8) & ~0x3c0U, ==, 0);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn1/xbridge-xbow/identification", test_identification);
    qtest_add_func("/sn1/xbridge-xbow/reset-values", test_reset_values);
    qtest_add_func("/sn1/xbridge-xbow/source-port", test_source_port);
    qtest_add_func("/sn1/xbridge-xbow/control-and-destination",
                   test_control_and_destination);
    qtest_add_func("/sn1/xbridge-xbow/lock", test_lock);
    qtest_add_func("/sn1/xbridge-xbow/fences", test_fences);
    qtest_add_func("/sn1/xbridge-xbow/link-registers", test_link_registers);
    qtest_add_func("/sn1/xbridge-xbow/eight-bit-mode", test_eight_bit_mode);
    qtest_add_func("/sn1/xbridge-xbow/access-errors", test_access_errors);
    qtest_add_func("/sn1/xbridge-xbow/link-status-clear",
                   test_link_status_clear);
    qtest_add_func("/sn1/xbridge-xbow/error-command-word",
                   test_error_command_word);
    qtest_add_func("/sn1/xbridge-xbow/interrupt-packets",
                   test_interrupt_packets);
    qtest_add_func("/sn1/xbridge-xbow/warm-reset", test_warm_reset);
    qtest_add_func("/sn1/xbridge-xbow/migration-reset", test_migration_reset);
    return g_test_run();
}
