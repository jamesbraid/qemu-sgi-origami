/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional textureless SI attachment with synthetic firmware and DMA data. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-launch.h"
#include "sgi-migrate.h"
#include "qobject/qdict.h"

#define XBOW 0x10000000ULL
#define SI_SHORT 0x1b000000ULL
#define SI_LONG 0xb000000000ULL
#define HEART 0x0ff00000ULL
#define HEART_WIDGET 0x18000000ULL
#define BRIDGE 0x1f000000ULL
#define SI_ID 0x0c003555u
#define SI_HQ_REVISION 0x2000000000000000ULL
#define CONTROL 0x24
#define COMMAND 0x120400
#define MAPPING 0x130400
#define CORE_LINKS 0x8100
#define SI_LINKS 0x8900
#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152," \
    "r12000-scache-block-words=32"

static char *flash_path;

static QTestState *start(bool octane2, const char *graphics, const char *extra)
{
    g_autofree char *quoted = g_shell_quote(flash_path);

    return qtest_initf("-M %s%s -m %uM -cpu %s -bios %s -S %s",
                      octane2 ? "octane2" : "octane", graphics,
                      octane2 ? 256 : 128,
                      octane2 ? OCTANE2_CPU : "R10000", quoted, extra);
}

static uint64_t property(QTestState *qts, const char *path, const char *name)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, name);

    return qdict_get_int(reply, "return");
}

static void set_property(QTestState *qts, const char *path, const char *name,
                         uint64_t value)
{
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':%s,'property':%s,"
        "'value':%" PRIu64 "}}", path, name, value);
}

static void assert_identity(QTestState *qts)
{
    uint32_t word;
    uint64_t value;

    g_assert_cmpuint(qtest_readl_tx(qts, SI_SHORT + 4, &word), ==, 0);
    g_assert_cmphex(word, ==, SI_ID);
    g_assert_cmpuint(qtest_readl_tx(qts, SI_LONG + 4, &word), ==, 0);
    g_assert_cmphex(word, ==, SI_ID);
    g_assert_cmpuint(qtest_readq_tx(qts, SI_SHORT, &value), ==, 0);
    g_assert_cmphex(value, ==, SI_HQ_REVISION);
    g_assert_cmpuint(qtest_readq_tx(qts, SI_LONG, &value), ==, 0);
    g_assert_cmphex(value, ==, SI_HQ_REVISION);
}

static void assert_core(QTestState *qts, bool octane2)
{
    uint32_t value;

    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 4), ==,
                    octane2 ? 0x6c00106d : 0x4c00106d);

    g_assert_cmpuint(qtest_readl_tx(qts, BRIDGE + 4, &value), ==, 0);
}

static void test_absent(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    const char *selections[] = { "", ",graphics-board=none" };
    uint32_t value;

    for (unsigned i = 0; i < G_N_ELEMENTS(selections); i++) {
        QTestState *qts = start(octane2, selections[i], "");

        g_assert_cmphex(property(qts, "/machine/crossbow", "links-present"),
                        ==, CORE_LINKS);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, CORE_LINKS);
        g_assert_cmpuint(qtest_readl_tx(qts, SI_SHORT + 4, &value), !=, 0);
        g_assert_cmpuint(qtest_readl_tx(qts, SI_LONG + 4, &value), !=, 0);
        assert_core(qts, octane2);
        qtest_quit(qts);
    }
}

static void test_population(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    QTestState *qts = start(octane2, ",graphics-board=si", "");

    g_assert_cmphex(property(qts, "/machine/crossbow", "links-present"),
                    ==, SI_LINKS);
    g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                    ==, SI_LINKS);
    assert_identity(qts);
    assert_core(qts, octane2);
    qtest_quit(qts);
}

static void test_shared_controls(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    QTestState *qts = start(octane2, ",graphics-board=si", "");
    uint8_t bytes[4];
    const uint8_t expected[] = { 0x39, 0xa5, 0x62, 0xd7 };
    uint32_t heart = qtest_readl(qts, HEART_WIDGET + CONTROL);
    uint32_t bridge = qtest_readl(qts, BRIDGE + CONTROL);

    qtest_writel(qts, SI_SHORT + CONTROL, 0x39a562d7);
    g_assert_cmphex(qtest_readl(qts, SI_LONG + CONTROL), ==, 0x39a562d7);
    qtest_memread(qts, SI_LONG + CONTROL, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), expected, sizeof(expected));
    qtest_writel(qts, SI_LONG + CONTROL, 0xe1625b38);
    g_assert_cmphex(qtest_readl(qts, SI_SHORT + CONTROL), ==, 0xe1625b38);
    qtest_writeb(qts, SI_SHORT + 0x073c08, 0xa7);
    g_assert_cmphex(qtest_readb(qts, SI_LONG + 0x073c08), ==, 0xa7);
    qtest_writew(qts, SI_LONG + 0x071810, 0x3b91);
    g_assert_cmphex(qtest_readw(qts, SI_SHORT + 0x071810), ==, 0x3b91);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + CONTROL), ==, heart);
    g_assert_cmphex(qtest_readl(qts, BRIDGE + CONTROL), ==, bridge);
    qtest_quit(qts);
}

static void test_widths_and_holes(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    QTestState *qts = start(octane2, ",graphics-board=si", "");
    const uint64_t holes[] = {
        SI_SHORT + 0x200000, SI_LONG + 0x200000,
        SI_LONG + 0x1000000 + CONTROL, SI_LONG + (1ULL << 47) + CONTROL,
        SI_SHORT + 0x1000000 + CONTROL, SI_LONG + (1ULL << 36) + CONTROL,
    };
    uint32_t value;

    qtest_writel(qts, SI_SHORT + CONTROL, 0x39a562d7);
    qtest_writeb(qts, SI_LONG + CONTROL, 0xff);
    qtest_writew(qts, SI_SHORT + CONTROL, 0xffff);
    g_assert_cmphex(qtest_readl(qts, SI_LONG + CONTROL), ==, 0x39a562d7);
    qtest_writel(qts, SI_SHORT + 0x040000, 0x1a2b3c4d);
    qtest_writel(qts, SI_SHORT + 0x040004, 0x5e6f7081);
    qtest_writeq(qts, SI_LONG + 0x040000, 0xa1b2c3d4e5f60718ULL);
    g_assert_cmphex(qtest_readl(qts, SI_LONG + 0x040000), ==, 0x1a2b3c4d);
    g_assert_cmphex(qtest_readl(qts, SI_LONG + 0x040004), ==, 0x5e6f7081);
    for (unsigned i = 0; i < G_N_ELEMENTS(holes); i++) {
        g_assert_cmpuint(qtest_readl_tx(qts, holes[i], &value), !=, 0);
        g_assert_cmpuint(qtest_writel_tx(qts, holes[i], 0xffffffff), !=, 0);
        g_assert_cmphex(qtest_readl(qts, SI_SHORT + CONTROL), ==, 0x39a562d7);
    }
    assert_core(qts, octane2);
    qtest_quit(qts);
}

static void test_liveness(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    QTestState *qts = start(octane2, ",graphics-board=si", "");
    uint32_t value;

    qtest_writel(qts, SI_SHORT + CONTROL, 0x5d3a81c6);
    set_property(qts, "/machine/crossbow", "links-alive", CORE_LINKS);
    g_assert_cmpuint(qtest_readl_tx(qts, SI_SHORT + CONTROL, &value), !=, 0);
    g_assert_cmpuint(qtest_readl_tx(qts, SI_LONG + CONTROL, &value), !=, 0);
    g_assert_cmpuint(qtest_writel_tx(qts, SI_SHORT + CONTROL, 0), !=, 0);
    g_assert_cmpuint(qtest_writel_tx(qts, SI_LONG + CONTROL, 0), !=, 0);
    assert_core(qts, octane2);
    set_property(qts, "/machine/crossbow", "links-alive", SI_LINKS);
    g_assert_cmphex(qtest_readl(qts, SI_SHORT + CONTROL), ==, 0x5d3a81c6);
    g_assert_cmphex(qtest_readl(qts, SI_LONG + CONTROL), ==, 0x5d3a81c6);
    qtest_quit(qts);
}

static uint64_t link_register(unsigned widget, unsigned offset)
{
    return XBOW + 0x100 + (widget - 8) * 0x40 + offset + 4;
}

static void assert_link_alive(QTestState *qts, unsigned widget, bool alive)
{
    g_assert_cmphex(qtest_readl(qts, link_register(widget, 0x10)) &
                    (1u << 31), ==, alive ? 1u << 31 : 0);
    g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive") &
                    (1u << widget), ==, alive ? 1u << widget : 0);
}

static void reset_link(QTestState *qts, unsigned widget)
{
    g_assert_cmpuint(qtest_writel_tx(qts, link_register(widget, 0x30), 0),
                     ==, 0);
}

static void assert_si_unreachable(QTestState *qts)
{
    uint32_t value;

    g_assert_cmpuint(qtest_readl_tx(qts, SI_SHORT + 4, &value), !=, 0);
    g_assert_cmpuint(qtest_readl_tx(qts, SI_LONG + 4, &value), !=, 0);
}

static void assert_link_masks(QTestState *qts, unsigned links)
{
    const char *names[] = {
        "links-present", "startup-links-alive", "link-reset-restore-alive",
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(names); i++) {
        g_assert_cmphex(property(qts, "/machine/crossbow", names[i]),
                        ==, links);
    }
}

static void test_link_reset_recovery(gconstpointer data)
{
    QTestState *qts = start(GPOINTER_TO_INT(data), ",graphics-board=si", "");

    assert_identity(qts);
    reset_link(qts, 11);
    assert_link_alive(qts, 11, false);
    qtest_clock_step(qts, 19);
    assert_link_alive(qts, 11, false);
    qtest_clock_step(qts, 1);
    assert_link_alive(qts, 11, true);
    assert_identity(qts);
    assert_link_masks(qts, SI_LINKS);

    /* Recovery preserves routes while the reported alive bit is clear. */
    reset_link(qts, 11);
    assert_link_alive(qts, 11, false);
    assert_identity(qts);
    qtest_clock_step(qts, 19);
    assert_link_alive(qts, 11, false);
    assert_identity(qts);
    qtest_clock_step(qts, 1);
    assert_link_alive(qts, 11, true);
    assert_identity(qts);

    set_property(qts, "/machine/crossbow", "links-alive", CORE_LINKS);
    reset_link(qts, 11);
    assert_link_alive(qts, 11, false);
    assert_si_unreachable(qts);
    qtest_clock_step(qts, 19);
    assert_link_alive(qts, 11, false);
    assert_si_unreachable(qts);
    qtest_clock_step(qts, 1);
    assert_link_alive(qts, 11, false);
    assert_si_unreachable(qts);
    g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                    ==, CORE_LINKS);
    qtest_system_reset(qts);
    g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                    ==, SI_LINKS);
    assert_identity(qts);
    qtest_quit(qts);
}

static void test_link_reset_absent(gconstpointer data)
{
    const char *selections[] = { "", ",graphics-board=none" };

    for (unsigned i = 0; i < G_N_ELEMENTS(selections); i++) {
        QTestState *qts = start(GPOINTER_TO_INT(data), selections[i], "");

        assert_link_masks(qts, CORE_LINKS);
        reset_link(qts, 11);
        assert_link_alive(qts, 11, false);
        assert_si_unreachable(qts);
        qtest_clock_step(qts, 19);
        assert_link_alive(qts, 11, false);
        assert_si_unreachable(qts);
        qtest_clock_step(qts, 1);
        assert_link_alive(qts, 11, false);
        assert_si_unreachable(qts);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-present"),
                        ==, CORE_LINKS);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, CORE_LINKS);
        qtest_system_reset(qts);
        assert_si_unreachable(qts);
        qtest_quit(qts);
    }
}

static void assert_bridge(QTestState *qts, uint32_t expected)
{
    uint32_t value;

    g_assert_cmpuint(qtest_readl_tx(qts, BRIDGE + 4, &value), ==, 0);
    g_assert_cmphex(value, ==, expected);
}

static void test_core_link_reset_recovery(gconstpointer data)
{
    const char *selections[] = {
        "", ",graphics-board=none", ",graphics-board=si",
    };
    const unsigned widgets[] = { 8, 15 };

    for (unsigned selection = 0; selection < G_N_ELEMENTS(selections);
         selection++) {
        bool graphics = selection == 2;
        uint32_t links = graphics ? SI_LINKS : CORE_LINKS;
        QTestState *qts = start(GPOINTER_TO_INT(data),
                               selections[selection], "");
        uint32_t bridge = qtest_readl(qts, BRIDGE + 4);

        assert_link_masks(qts, links);
        for (unsigned i = 0; i < G_N_ELEMENTS(widgets); i++) {
            reset_link(qts, widgets[i]);
            assert_link_alive(qts, widgets[i], false);
            assert_bridge(qts, bridge);
            if (graphics) {
                assert_identity(qts);
            } else {
                assert_si_unreachable(qts);
            }
            qtest_clock_step(qts, 19);
            assert_link_alive(qts, widgets[i], false);
            assert_bridge(qts, bridge);
            qtest_clock_step(qts, 1);
            assert_link_alive(qts, widgets[i], true);
            assert_bridge(qts, bridge);
            if (graphics) {
                assert_identity(qts);
            } else {
                assert_si_unreachable(qts);
            }
            g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                            ==, links);
        }
        set_property(qts, "/machine/crossbow", "links-alive", links & ~0x8000);
        reset_link(qts, 15);
        assert_link_alive(qts, 15, false);
        uint32_t value;

        g_assert_cmpuint(qtest_readl_tx(qts, BRIDGE + 4, &value), !=, 0);
        qtest_clock_step(qts, 19);
        assert_link_alive(qts, 15, false);
        g_assert_cmpuint(qtest_readl_tx(qts, BRIDGE + 4, &value), !=, 0);
        qtest_clock_step(qts, 1);
        assert_link_alive(qts, 15, false);
        g_assert_cmpuint(qtest_readl_tx(qts, BRIDGE + 4, &value), !=, 0);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, links & ~0x8000);
        qtest_system_reset(qts);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, links);
        assert_bridge(qts, bridge);

        /* A dead CPU ingress cannot deliver its own reset command. */
        set_property(qts, "/machine/crossbow", "links-alive", links & ~0x100);
        g_assert_cmpuint(qtest_writel_tx(qts, link_register(8, 0x30), 0),
                         !=, 0);
        qtest_clock_step(qts, 20);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, links & ~0x100);
        g_assert_cmpuint(qtest_readl_tx(qts, BRIDGE + 4, &value), !=, 0);
        qtest_system_reset(qts);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, links);
        assert_bridge(qts, bridge);
        if (graphics) {
            assert_identity(qts);
        } else {
            assert_si_unreachable(qts);
        }
        qtest_quit(qts);
    }
}

static void enter_context(QTestState *qts)
{
    qtest_writel(qts, SI_SHORT + 0x011000, 0);
    qtest_writel(qts, SI_SHORT + 0x130500, 0x4fc);
    qtest_writel(qts, SI_SHORT + 0x011028, 0x80000);
    for (unsigned i = 0; i < 63; i++) {
        qtest_writel(qts, SI_SHORT + COMMAND, i);
    }
    qtest_writel(qts, SI_SHORT + 0x011028, 0x10000000);
}

static void assert_reset(QTestState *qts)
{
    assert_identity(qts);
    g_assert_cmphex(qtest_readl(qts, SI_SHORT + CONTROL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SI_LONG + CONTROL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SI_LONG + 0x040000), ==, 0);
    g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                    ==, SI_LINKS);
}

static void test_reset(gconstpointer data)
{
    QTestState *qts = start(GPOINTER_TO_INT(data), ",graphics-board=si", "");

    qtest_writel(qts, SI_SHORT + CONTROL, 0xc1d28354);
    qtest_writel(qts, SI_LONG + 0x040000, 0x75e68192);
    enter_context(qts);
    set_property(qts, "/machine/crossbow", "links-alive", CORE_LINKS);
    qtest_system_reset(qts);
    assert_reset(qts);
    {
        g_autoptr(QDict) reply = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{'path':'/machine/module0/mgras',"
            "'property':'x-test-first-ack'}}");

        g_assert_false(qdict_get_bool(reply, "return"));
    }
    qtest_quit(qts);
}

static void test_migration(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("octane-si-migration-XXXXXX",
                                               &error);
    g_autofree char *socket = g_build_filename(directory, "mig.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start(octane2, ",graphics-board=si", "");
    QTestState *to = start(octane2, ",graphics-board=si", "-incoming defer");

    g_assert_no_error(error);
    qtest_writel(from, SI_SHORT + CONTROL, 0xc1d28354);
    qtest_writel(from, SI_LONG + 0x040000, 0x75e68192);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    assert_identity(to);
    g_assert_cmphex(qtest_readl(to, SI_LONG + CONTROL), ==, 0xc1d28354);
    g_assert_cmphex(qtest_readl(to, SI_SHORT + 0x040000), ==, 0x75e68192);
    qtest_system_reset(to);
    assert_reset(to);
    qtest_quit(to);
    qtest_quit(from);
    if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

/* Existing HQ4 fixture grammar, local mapped SDRAM only. */
#define RSS(reg, value) \
    (((uint64_t)(0x60001 | ((reg) << 6)) << 34) | (uint32_t)(value))
#define RSS_EXEC(reg, value) \
    (((uint64_t)(0x70001 | ((reg) << 6)) << 34) | (uint32_t)(value))

static void descriptor(QTestState *qts, uint8_t reg, uint32_t value)
{
    qtest_writeq(qts, SI_LONG + COMMAND,
                 ((uint64_t)(0x00080004 | (reg << 8)) << 32) | value);
}

static uint32_t pixel(QTestState *qts, unsigned x, unsigned y)
{
    set_property(qts, "/machine/module0/mgras", "x-test-pixel-x", x);
    set_property(qts, "/machine/module0/mgras", "x-test-pixel-y", y);
    return property(qts, "/machine/module0/mgras", "x-test-pixel");
}

static void test_local_dma(gconstpointer data)
{
    QTestState *qts = start(GPOINTER_TO_INT(data), ",graphics-board=si", "");
    const uint8_t source[] = { 0, 0x11, 0x22, 0x33, 0, 0x44, 0x55, 0x66 };
    uint8_t returned[sizeof(source)];

    /* Split the two pixels across two ordinary HEART SDRAM pages. */
    g_assert_cmpuint(qtest_writel_tx(qts, 0x20008018, 0), ==, 0);
    qtest_writeq(qts, 0x20008018, 0x20010);
    qtest_writeq(qts, 0x20008020, 0x20011);
    qtest_memwrite(qts, 0x20010ffc, source, sizeof(source));
    enter_context(qts);
    qtest_writel(qts, SI_SHORT + MAPPING, 0x00482008);
    qtest_writeq(qts, SI_LONG + MAPPING, 0x20008018);
    qtest_writeq(qts, SI_SHORT + MAPPING, 0x0048300400000002ULL);
    qtest_writeq(qts, SI_LONG + MAPPING, 0x00484b0400080000ULL);
    qtest_writeq(qts, SI_SHORT + COMMAND, RSS(0x162, 0));
    qtest_writeq(qts, SI_LONG + COMMAND, RSS(0x163, 0x00ffffff));
    qtest_writeq(qts, SI_SHORT + COMMAND, RSS(0x161, 3u << 26));
    qtest_writeq(qts, SI_LONG + COMMAND, RSS(0x46, (50u << 16) | 50));
    qtest_writeq(qts, SI_SHORT + COMMAND, RSS_EXEC(0x47, (51u << 16) | 50));
    descriptor(qts, 4, 8);
    descriptor(qts, 5, 4);
    descriptor(qts, 6, 4088);
    descriptor(qts, 7, 1);
    descriptor(qts, 8, 8);
    descriptor(qts, 11, 0xb1);
    g_assert_cmphex(pixel(qts, 50, 50), ==, 0x00332211);
    g_assert_cmphex(pixel(qts, 51, 50), ==, 0x00665544);
    g_assert_cmphex(pixel(qts, 49, 50), ==, 0);
    g_assert_cmphex(pixel(qts, 52, 50), ==, 0);
    qtest_memset(qts, 0x20010ffc, 0xa5, sizeof(source));
    qtest_writeq(qts, SI_LONG + COMMAND, RSS(0x46, (50u << 16) | 973));
    qtest_writeq(qts, SI_SHORT + COMMAND, RSS_EXEC(0x47, (51u << 16) | 973));
    descriptor(qts, 11, 0xb9);
    qtest_memread(qts, 0x20010ffc, returned, sizeof(returned));
    g_assert_cmpmem(returned, sizeof(returned), source, sizeof(source));
    g_assert_cmphex(property(qts, "/machine/module0/mgras", "x-test-error"),
                    ==, 0);
    qtest_quit(qts);
}

/* Synthetic programmed vectors exercise transport, not IP30 allocation. */
static void test_interrupt_transport(gconstpointer data)
{
    QTestState *qts = start(GPOINTER_TO_INT(data), ",graphics-board=si", "");
    const uint64_t other = 1ULL << 5;
    const uint64_t pending = 1ULL << 18;

    qtest_irq_intercept_out_named(qts, "/machine/heart", "processor");
    qtest_writel(qts, SI_SHORT + 0x011060, 8u << 24);
    qtest_writel(qts, SI_LONG + 0x011068, 0x80);
    qtest_writel(qts, SI_SHORT + 0x011070, 18u << 8);
    qtest_writel(qts, SI_LONG + 0x011040, 0x80000);
    qtest_writel(qts, SI_SHORT + 0x011000, 0);
    qtest_writel(qts, SI_LONG + 0x130500, 0x4fc);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030) & pending, ==, pending);
    g_assert_false(qtest_get_irq(qts, 1));
    qtest_writeq(qts, HEART + 0x10000, pending);
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_writeq(qts, HEART + 0x10000, 0);
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030) & pending, ==, pending);
    qtest_writeq(qts, HEART + 0x10000, pending);
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_writeq(qts, HEART + 0x10020, other);
    qtest_writel(qts, SI_SHORT + 0x011028, 0x80000);
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030) & (pending | other), ==,
                    other);
    qtest_writel(qts, SI_LONG + 0x130500, 0x4fc);
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_writel(qts, SI_SHORT + 0x011028, 0x80000);
    qtest_writeq(qts, HEART + 0x10028, other);
    qtest_writel(qts, SI_LONG + 0x011070, 7u << 8);
    qtest_writeq(qts, HEART + 0x10000, 1u << 7);
    qtest_writel(qts, SI_SHORT + 0x130500, 0x4fc);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    qtest_writel(qts, SI_LONG + 0x011028, 0x80000);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_rejected_selection(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", octane2 ? "octane2,graphics-board=v12" :
        "octane,graphics-board=v12", "-display", "none", "-qmp", "stdio", NULL,
    };

    assert_launch_rejected(args, "graphics must be none or si");
}

int main(int argc, char **argv)
{
    const struct {
        const char *name;
        GTestDataFunc test;
    } tests[] = {
        { "absent", test_absent },
        { "population", test_population },
        { "shared-controls", test_shared_controls },
        { "widths-and-holes", test_widths_and_holes },
        { "liveness", test_liveness },
        { "link-reset-recovery", test_link_reset_recovery },
        { "link-reset-absent", test_link_reset_absent },
        { "core-link-reset-recovery", test_core_link_reset_recovery },
        { "reset", test_reset },
        { "migration", test_migration },
        { "local-dma", test_local_dma },
        { "interrupt-transport", test_interrupt_transport },
        { "rejected-selection", test_rejected_selection },
    };
    const uint8_t erased[] = { 0xff, 0xff, 0xff, 0xff };
    int fd, result;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("octane-si-prom-XXXXXX", &flash_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, erased, sizeof(erased)), ==, sizeof(erased));
    close(fd);
    for (unsigned profile = 0; profile < 2; profile++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(tests); i++) {
            g_autofree char *path = g_strdup_printf("/%s/si/%s",
                profile ? "octane2" : "octane", tests[i].name);

            qtest_add_data_func(path, GINT_TO_POINTER(profile), tests[i].test);
        }
    }
    result = g_test_run();
    g_assert_cmpint(unlink(flash_path), ==, 0);
    g_free(flash_path);
    return result;
}
