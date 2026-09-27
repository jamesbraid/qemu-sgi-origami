/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic bank populations for the standalone IP30 HEART profile. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-migrate.h"

#define HEART_PATH "/machine/peripheral/heart"
#define HEART_REGS 0x0ff00000ULL
#define HEART_WIDGET 0x11000000ULL
#define HEART_PACKET 0x12000000ULL
#define HEART_WIDGET_ID 0x4c00106cU
#define HEART_XTALK_PATH "/machine/peripheral/heart-xtalk"
#define HEART_XTALK_REGS 0x0f000000ULL
#define HEART_SMALL 0x10000000ULL
#define HEART_LARGE (8ULL << 36)
#define HEART_XTALK_PACKET 0x3000000000ULL
#define HEART_MISSING_PACKET 0x3000001000ULL
#define HEART_RAM 0x20000000ULL
#define UNIT (32ULL * 1024 * 1024)
#define PROBE_DESC 0x81bf0040U
#define PROBE_RAM (HEART_RAM + 0x40 * UNIT)

static uint32_t heart_descriptor(unsigned index)
{
    return index == 0 ? PROBE_DESC :
           0x80000000U | (index << 16) | (0x100 + index * 0x10);
}

static QTestState *heart_start(unsigned mib, const char *extra)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 -device "
        "sgi-heart,id=heart,bank0-size=%" PRIu64 ",widget-revision=4 -S %s",
        (uint64_t)mib * 1024 * 1024, extra);

    qtest_sysbus_mmio_map(s, HEART_PATH, 0, HEART_REGS);
    qtest_sysbus_mmio_map(s, HEART_PATH, 1, HEART_WIDGET);
    qtest_sysbus_mmio_map(s, HEART_PATH, 2, HEART_PACKET);
    return s;
}

static void test_descriptors(gconstpointer data)
{
    unsigned mib = GPOINTER_TO_UINT(data);
    QTestState *s = heart_start(mib, "");
    uint64_t span = (uint64_t)mib * 1024 * 1024;
    uint64_t value;
    unsigned detected_units = 0;

    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20), ==, 0);
    qtest_writel(s, HEART_REGS + 0x20, PROBE_DESC);
    g_assert_cmphex(qtest_readl(s, HEART_REGS + 0x20), ==, PROBE_DESC);
    qtest_writeq(s, PROBE_RAM + 0x100, 0x123456789abcdef0ULL);
    for (unsigned unit = 1; unit < 64; unit++) {
        if (qtest_readq(s, PROBE_RAM + 0x100 + unit * UNIT) ==
            0x123456789abcdef0ULL) {
            detected_units = unit;
            break;
        }
    }
    g_assert_cmpuint(detected_units, ==, mib / 32);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x100 + span), ==,
                    0x123456789abcdef0ULL);
    qtest_writeq(s, PROBE_RAM + 0x100 + 2 * span,
                 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x100), ==,
                    0xfedcba9876543210ULL);
    qtest_writeq(s, PROBE_RAM + 0x100 + 63 * UNIT, 0x1020304050607080ULL);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x100 +
                                (63 % detected_units) * UNIT), ==,
                    0x1020304050607080ULL);

    /* An absent descriptor claims probe space but cannot retain a pattern. */
    qtest_writel(s, HEART_REGS + 0x20, 0);
    qtest_writel(s, HEART_REGS + 0x24, PROBE_DESC);
    g_assert_cmpuint(qtest_readq_tx(s, 0x10000000000ULL, &value), ==,
                     MEMTX_DECODE_ERROR);
    g_assert_cmphex(value, ==, 0);
    g_assert_cmpuint(qtest_writeq_tx(s, 0x10000000000ULL, UINT64_MAX), ==,
                     MEMTX_DECODE_ERROR);
    g_assert_cmpuint(qtest_writeq_tx(s, PROBE_RAM + 0x100, UINT64_MAX), ==,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_readq_tx(s, PROBE_RAM + 0x100, &value), ==,
                     MEMTX_OK);
    g_assert_cmphex(value, ==, 0);
    qtest_writeq(s, HEART_REGS + 0x20, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20), ==, 0);
    qtest_quit(s);
}

static void test_wide_descriptor_clear(void)
{
    QTestState *s = heart_start(128, "");
    uint64_t value;
    uint64_t address[4];

    for (unsigned i = 0; i < 4; i++) {
        uint32_t descriptor = heart_descriptor(i);

        address[i] = HEART_RAM + (uint64_t)(descriptor & 0x1ff) * UNIT;
        qtest_writel(s, HEART_REGS + 0x20 + 4 * i, descriptor);
        g_assert_cmphex(qtest_readl(s, HEART_REGS + 0x20 + 4 * i), ==,
                        descriptor);
        g_assert_cmpuint(qtest_readq_tx(s, address[i], &value), ==,
                         MEMTX_OK);
    }
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20), ==,
                    ((uint64_t)heart_descriptor(0) << 32) |
                    heart_descriptor(1));
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x28), ==,
                    ((uint64_t)heart_descriptor(2) << 32) |
                    heart_descriptor(3));

    qtest_writeq(s, HEART_REGS + 0x20, 0);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(s, HEART_REGS + 0x20 + 4 * i), ==,
                        i < 2 ? 0 : heart_descriptor(i));
        g_assert_cmpuint(qtest_readq_tx(s, address[i], &value), ==,
                         i < 2 ? MEMTX_DECODE_ERROR : MEMTX_OK);
    }

    qtest_writeq(s, HEART_REGS + 0x28, 0);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(s, HEART_REGS + 0x20 + 4 * i), ==, 0);
        g_assert_cmpuint(qtest_readq_tx(s, address[i], &value), ==,
                         MEMTX_DECODE_ERROR);
    }
    qtest_quit(s);
}

static void test_widget_packets(void)
{
    QTestState *s = heart_start(128, "");
    uint32_t id;
    uint64_t ignored;
    uint64_t level0 = 1ULL << 3;
    uint64_t level2 = 1ULL << 42;

    qtest_irq_intercept_out_named(s, HEART_PATH, "processor");
    g_assert_cmpuint(qtest_readl_tx(s, HEART_WIDGET + 4, &id), ==, MEMTX_OK);
    g_assert_cmphex(id, ==, HEART_WIDGET_ID);
    g_assert_cmpuint(qtest_readl_tx(s, HEART_WIDGET, &id), ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_readq_tx(s, HEART_WIDGET, &ignored),
                     ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_writel_tx(s, HEART_PACKET + 0x80, 0x103), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 0x140), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 0x303), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x88, 0x103), ==,
                     MEMTX_ERROR);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0);

    qtest_writeq(s, HEART_REGS + 0x10000, level0 | level2);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 0x103), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, level0);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_false(qtest_get_irq(s, 1));
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 0x12a), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10038), ==,
                    level0 | level2);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 1));
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 3), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, level2);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 1));
    qtest_writeq(s, HEART_REGS + 0x10000, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, level2);
    g_assert_false(qtest_get_irq(s, 1));
    qtest_writeq(s, HEART_REGS + 0x10000, level2);
    g_assert_true(qtest_get_irq(s, 1));
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 42), ==,
                     MEMTX_OK);
    g_assert_false(qtest_get_irq(s, 1));
    qtest_quit(s);
}

static void test_interrupts(void)
{
    QTestState *s = heart_start(128, "");

    qtest_writeq(s, HEART_REGS + 0x10008, 0x2);
    qtest_writeq(s, HEART_REGS + 0x10020, 0x3);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0x3);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10038), ==, 0);
    qtest_writeq(s, HEART_REGS + 0x10000, 0x1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10038), ==, 0x1);
    qtest_writeq(s, HEART_REGS + 0x10000, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0x3);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10038), ==, 0);
    qtest_writeq(s, HEART_REGS + 0x10028, 0x1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0x2);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10008), ==, 0x2);
    qtest_quit(s);
}

static void test_reset(void)
{
    QTestState *s = heart_start(128, "");
    uint64_t value;

    qtest_writeq(s, HEART_REGS, 0x200810ULL);
    qtest_writeq(s, HEART_REGS + 0x08, 0x21);
    qtest_writeq(s, HEART_REGS + 0x10, 0x84000);
    qtest_writeq(s, HEART_REGS + 0x18, 0x13579bdf);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writel(s, HEART_REGS + 0x20 + 4 * i, heart_descriptor(i));
        g_assert_cmphex(qtest_readl(s, HEART_REGS + 0x20 + 4 * i), ==,
                        heart_descriptor(i));
    }
    qtest_writeq(s, PROBE_RAM, 0xfeedface);
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeq(s, HEART_REGS + 0x10000 + 8 * i, 1ULL << i);
    }
    qtest_writeq(s, HEART_REGS + 0x10020, 0xf);
    qtest_clock_step(s, 800);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20000), ==, 10);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, HEART_REGS), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x08), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x18), ==, 0);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readl(s, HEART_REGS + 0x20 + 4 * i), ==, 0);
    }
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10000 + 8 * i), ==, 0);
    }
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x80), ==, 0x10);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x50000), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20000), ==, 0);
    g_assert_cmpuint(qtest_readq_tx(s, PROBE_RAM, &value), ==,
                     MEMTX_DECODE_ERROR);
    qtest_writel(s, HEART_REGS + 0x20, PROBE_DESC);
    g_assert_cmpuint(qtest_readq_tx(s, PROBE_RAM, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0xfeedface);
    qtest_quit(s);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("heart-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = heart_start(128, "");
    to = heart_start(128, "-incoming defer");
    qtest_writeq(from, HEART_REGS, 0x800);
    qtest_writeq(from, HEART_REGS + 0x08, 0x21);
    qtest_writeq(from, HEART_REGS + 0x10, 0x84000);
    qtest_writeq(from, HEART_REGS + 0x18, 0x13579bdf);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writel(from, HEART_REGS + 0x20 + 4 * i,
                     heart_descriptor(i));
    }
    qtest_writeq(from, PROBE_RAM, 0x123456789abcdef0ULL);
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeq(from, HEART_REGS + 0x10000 + 8 * i, 1ULL << i);
    }
    qtest_writeq(from, HEART_REGS + 0x10020, 0xf);
    qtest_clock_step(from, 800);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, HEART_REGS), ==, 0x800);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x08), ==, 0x21);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10), ==, 0x84000);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x18), ==, 0x13579bdf);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readl(to, HEART_REGS + 0x20 + 4 * i), ==,
                        heart_descriptor(i));
    }
    g_assert_cmphex(qtest_readq(to, PROBE_RAM), ==, 0x123456789abcdef0ULL);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10000 + 8 * i), ==,
                        1ULL << i);
    }
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10030), ==, 0xf);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10038), ==, 0x1);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x20000), ==, 10);
    qtest_clock_step(to, 80);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x20000), ==, 11);
    qtest_quit(to);
    qtest_quit(from);
    if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}


static QTestState *heart_xtalk_start(const char *extra)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 -S "
                                 "-device sgi-heart-xtalk-test,id=heart-xtalk %s",
                                 extra);

    qtest_sysbus_mmio_map(s, HEART_XTALK_PATH, 0, HEART_XTALK_REGS);
    qtest_sysbus_mmio_map(s, HEART_XTALK_PATH, 1, HEART_SMALL);
    qtest_sysbus_mmio_map(s, HEART_XTALK_PATH, 2, HEART_LARGE);
    qtest_sysbus_mmio_map(s, HEART_XTALK_PATH, 3, HEART_XTALK_PACKET);
    qtest_sysbus_mmio_map(s, HEART_XTALK_PATH, 4, HEART_MISSING_PACKET);
    qtest_irq_intercept_out_named(s, HEART_XTALK_PATH, "processor");
    return s;
}

static void test_xtalk_routing(void)
{
    QTestState *s = heart_xtalk_start("");
    uint32_t id;
    uint64_t value;
    const uint64_t bits = (1ULL << 3) | (1ULL << 42);

    g_assert_cmpuint(qtest_readl_tx(s, HEART_SMALL + (8 << 24) + 4, &id),
                     ==, MEMTX_OK);
    g_assert_cmphex(id, ==, HEART_WIDGET_ID);
    g_assert_cmpuint(qtest_readl_tx(s, HEART_LARGE + 4, &id),
                     ==, MEMTX_OK);
    g_assert_cmphex(id, ==, HEART_WIDGET_ID);
    g_assert_cmpuint(qtest_readl_tx(s, HEART_SMALL + (8 << 24), &id),
                     ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_MISSING_PACKET + 0x80, 0x103),
                     ==, MEMTX_DECODE_ERROR);
    qtest_writeq(s, HEART_XTALK_REGS + 0x10000, bits);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_XTALK_PACKET + 0x80, 0x103),
                     ==, MEMTX_OK);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_XTALK_PACKET + 0x80, 0x12a),
                     ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_XTALK_REGS + 0x10030), ==, bits);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 1));
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_XTALK_PACKET + 0x80, 3),
                     ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_XTALK_REGS + 0x10030), ==,
                    1ULL << 42);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 1));
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':"
        "'/machine/peripheral/heart-xtalk/xbow',"
        "'property':'links-alive','value':32768}}");
    g_assert_cmpuint(qtest_readl_tx(s, HEART_SMALL + (8 << 24) + 4, &id),
                     ==, MEMTX_DECODE_ERROR);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_XTALK_PACKET + 0x80, 0x103),
                     ==, MEMTX_DECODE_ERROR);
    g_assert_cmphex(qtest_readq(s, HEART_XTALK_REGS + 0x10030), ==,
                    1ULL << 42);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmpuint(qtest_readl_tx(s, HEART_SMALL + (8 << 24) + 4, &id),
                     ==, MEMTX_OK);
    g_assert_cmphex(id, ==, HEART_WIDGET_ID);
    g_assert_cmphex(qtest_readq(s, HEART_XTALK_REGS + 0x10030), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_false(qtest_get_irq(s, 1));
    g_assert_cmpuint(qtest_readq_tx(s, HEART_MISSING_PACKET + 0x80, &value),
                     ==, MEMTX_DECODE_ERROR);
    qtest_quit(s);
}

static void test_xtalk_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("heart-xtalk-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    const uint64_t bits = (1ULL << 3) | (1ULL << 42);

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = heart_xtalk_start("");
    to = heart_xtalk_start("-incoming defer");
    qtest_writeq(from, HEART_XTALK_REGS + 0x10000, bits);
    qtest_writeq(from, HEART_XTALK_PACKET + 0x80, 0x103);
    qtest_writeq(from, HEART_XTALK_PACKET + 0x80, 0x12a);
    g_assert_true(qtest_get_irq(from, 0));
    g_assert_true(qtest_get_irq(from, 1));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, HEART_XTALK_REGS + 0x10030), ==, bits);
    g_assert_true(qtest_get_irq(to, 0));
    g_assert_true(qtest_get_irq(to, 1));
    qtest_writeq(to, HEART_XTALK_PACKET + 0x80, 3);
    g_assert_false(qtest_get_irq(to, 0));
    g_assert_true(qtest_get_irq(to, 1));
    qtest_quit(to);
    qtest_quit(from);
    if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_data_func("/heart/descriptor-128m", GUINT_TO_POINTER(128),
                         test_descriptors);
    g_test_add_data_func("/heart/descriptor-256m", GUINT_TO_POINTER(256),
                         test_descriptors);
    g_test_add_func("/heart/wide-descriptor-clear",
                    test_wide_descriptor_clear);
    g_test_add_func("/heart/widget-packets", test_widget_packets);
    g_test_add_func("/heart/interrupts", test_interrupts);
    g_test_add_func("/heart/reset", test_reset);
    g_test_add_func("/heart/migration", test_migration);
    g_test_add_func("/heart/xtalk-routing", test_xtalk_routing);
    g_test_add_func("/heart/xtalk-migration", test_xtalk_migration);
    return g_test_run();
}
