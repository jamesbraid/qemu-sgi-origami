/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Optional MGRAS attachment with synthetic firmware and DMA data. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-launch.h"
#include "qobject/qdict.h"

#define XBOW 0x10000000ULL
#define SI_SHORT 0x1b000000ULL
#define SI_LONG 0xb000000000ULL
#define HEART 0x0ff00000ULL
#define HEART_WIDGET 0x18000000ULL
#define BRIDGE 0x1f000000ULL
#define SI_ID 0x0c003555u
#define SI_HQ_REVISION 0x2c00355500000000ULL
#define CONTROL 0x24
#define COMMAND 0x120400
#define MAPPING 0x130400
#define NIC 0x011098
#define CORE_LINKS 0x8100
#define SI_LINKS 0x8900
#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152," \
    "r12000-scache-block-words=32"

static char *flash_path;

static uint64_t short_base(unsigned widget);
static uint64_t long_base(unsigned widget);

static QTestState *start(bool octane2, const char *graphics, const char *extra)
{
    g_autofree char *quoted = g_shell_quote(flash_path);

    return qtest_initf(MEMTX_TEST_DEVICE
                       "-M %s%s -m %uM -cpu %s -bios %s -S %s",
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

    g_assert_cmpuint(memtx_readl(qts, SI_SHORT + 4, &word), ==, 0);
    g_assert_cmphex(word, ==, SI_ID);
    g_assert_cmpuint(memtx_readl(qts, SI_LONG + 4, &word), ==, 0);
    g_assert_cmphex(word, ==, SI_ID);
    g_assert_cmpuint(memtx_readq(qts, SI_SHORT, &value), ==, 0);
    g_assert_cmphex(value, ==, SI_HQ_REVISION);
    g_assert_cmpuint(memtx_readq(qts, SI_LONG, &value), ==, 0);
    g_assert_cmphex(value, ==, SI_HQ_REVISION);
}

static void assert_identity_at(QTestState *qts, unsigned widget)
{
    uint32_t word;
    uint64_t value;
    uint64_t short_address = short_base(widget);
    uint64_t long_address = long_base(widget);

    g_assert_cmpuint(memtx_readl(qts, short_address + 4, &word), ==, 0);
    g_assert_cmphex(word, ==, SI_ID);
    g_assert_cmpuint(memtx_readl(qts, long_address + 4, &word), ==, 0);
    g_assert_cmphex(word, ==, SI_ID);
    g_assert_cmpuint(memtx_readq(qts, short_address, &value), ==, 0);
    g_assert_cmphex(value, ==, SI_HQ_REVISION);
    g_assert_cmpuint(memtx_readq(qts, long_address, &value), ==, 0);
    g_assert_cmphex(value, ==, SI_HQ_REVISION);
}

static void assert_core(QTestState *qts, bool octane2)
{
    uint32_t value;

    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 4), ==,
                    octane2 ? 0x6c00106d : 0x4c00106d);

    g_assert_cmpuint(memtx_readl(qts, BRIDGE + 4, &value), ==, 0);
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
        g_assert_cmpuint(memtx_readl(qts, SI_SHORT + 4, &value), !=, 0);
        g_assert_cmpuint(memtx_readl(qts, SI_LONG + 4, &value), !=, 0);
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
        g_assert_cmpuint(memtx_readl(qts, holes[i], &value), !=, 0);
        g_assert_cmpuint(memtx_writel(qts, holes[i], 0xffffffff), !=, 0);
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
    g_assert_cmpuint(memtx_readl(qts, SI_SHORT + CONTROL, &value), !=, 0);
    g_assert_cmpuint(memtx_readl(qts, SI_LONG + CONTROL, &value), !=, 0);
    g_assert_cmpuint(memtx_writel(qts, SI_SHORT + CONTROL, 0), !=, 0);
    g_assert_cmpuint(memtx_writel(qts, SI_LONG + CONTROL, 0), !=, 0);
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
    g_assert_cmpuint(memtx_writel(qts, link_register(widget, 0x30), 0),
                     ==, 0);
}

static void assert_mgras_unreachable(QTestState *qts)
{
    uint32_t value;

    g_assert_cmpuint(memtx_readl(qts, SI_SHORT + 4, &value), !=, 0);
    g_assert_cmpuint(memtx_readl(qts, SI_LONG + 4, &value), !=, 0);
}

static void assert_mgras_unreachable_at(QTestState *qts, unsigned widget)
{
    uint32_t value;

    g_assert_cmpuint(memtx_readl(qts, short_base(widget) + 4, &value), !=, 0);
    g_assert_cmpuint(memtx_readl(qts, long_base(widget) + 4, &value), !=, 0);
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
    bool octane2 = GPOINTER_TO_INT(data);

    for (unsigned widget = 9; widget <= 12; widget++) {
        g_autofree char *selection = g_strdup_printf(
            ",graphics-board=si,graphics-widget=%u", widget);
        unsigned links = CORE_LINKS | (1u << widget);
        QTestState *qts = start(octane2, selection, "");

        assert_identity_at(qts, widget);
        reset_link(qts, widget);
        assert_link_alive(qts, widget, false);
        qtest_clock_step(qts, 19);
        assert_link_alive(qts, widget, false);
        qtest_clock_step(qts, 1);
        assert_link_alive(qts, widget, true);
        assert_identity_at(qts, widget);
        assert_link_masks(qts, links);

        /* Routing stays usable while the reported alive bit is clear. */
        reset_link(qts, widget);
        assert_link_alive(qts, widget, false);
        assert_identity_at(qts, widget);
        qtest_clock_step(qts, 19);
        assert_link_alive(qts, widget, false);
        assert_identity_at(qts, widget);
        qtest_clock_step(qts, 1);
        assert_link_alive(qts, widget, true);
        assert_identity_at(qts, widget);

        set_property(qts, "/machine/crossbow", "links-alive", CORE_LINKS);
        reset_link(qts, widget);
        assert_link_alive(qts, widget, false);
        assert_mgras_unreachable_at(qts, widget);
        qtest_clock_step(qts, 19);
        assert_link_alive(qts, widget, false);
        assert_mgras_unreachable_at(qts, widget);
        qtest_clock_step(qts, 1);
        assert_link_alive(qts, widget, false);
        assert_mgras_unreachable_at(qts, widget);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, CORE_LINKS);
        qtest_system_reset(qts);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, links);
        assert_identity_at(qts, widget);
        qtest_quit(qts);
    }
}

static void test_link_reset_absent(gconstpointer data)
{
    const char *selections[] = { "", ",graphics-board=none" };

    for (unsigned i = 0; i < G_N_ELEMENTS(selections); i++) {
        QTestState *qts = start(GPOINTER_TO_INT(data), selections[i], "");

        assert_link_masks(qts, CORE_LINKS);
        reset_link(qts, 11);
        assert_link_alive(qts, 11, false);
        assert_mgras_unreachable(qts);
        qtest_clock_step(qts, 19);
        assert_link_alive(qts, 11, false);
        assert_mgras_unreachable(qts);
        qtest_clock_step(qts, 1);
        assert_link_alive(qts, 11, false);
        assert_mgras_unreachable(qts);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-present"),
                        ==, CORE_LINKS);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, CORE_LINKS);
        qtest_system_reset(qts);
        assert_mgras_unreachable(qts);
        qtest_quit(qts);
    }
}

static void assert_bridge(QTestState *qts, uint32_t expected)
{
    uint32_t value;

    g_assert_cmpuint(memtx_readl(qts, BRIDGE + 4, &value), ==, 0);
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
                assert_mgras_unreachable(qts);
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
                assert_mgras_unreachable(qts);
            }
            g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                            ==, links);
        }
        set_property(qts, "/machine/crossbow", "links-alive", links & ~0x8000);
        reset_link(qts, 15);
        assert_link_alive(qts, 15, false);
        uint32_t value;

        g_assert_cmpuint(memtx_readl(qts, BRIDGE + 4, &value), !=, 0);
        qtest_clock_step(qts, 19);
        assert_link_alive(qts, 15, false);
        g_assert_cmpuint(memtx_readl(qts, BRIDGE + 4, &value), !=, 0);
        qtest_clock_step(qts, 1);
        assert_link_alive(qts, 15, false);
        g_assert_cmpuint(memtx_readl(qts, BRIDGE + 4, &value), !=, 0);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, links & ~0x8000);
        qtest_system_reset(qts);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, links);
        assert_bridge(qts, bridge);

        /* A dead CPU ingress cannot deliver its own reset command. */
        set_property(qts, "/machine/crossbow", "links-alive", links & ~0x100);
        g_assert_cmpuint(memtx_writel(qts, link_register(8, 0x30), 0),
                         !=, 0);
        qtest_clock_step(qts, 20);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, links & ~0x100);
        g_assert_cmpuint(memtx_readl(qts, BRIDGE + 4, &value), !=, 0);
        qtest_system_reset(qts);
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, links);
        assert_bridge(qts, bridge);
        if (graphics) {
            assert_identity(qts);
        } else {
            assert_mgras_unreachable(qts);
        }
        qtest_quit(qts);
    }
}

static void enter_context(QTestState *qts, uint64_t sh)
{
    qtest_writel(qts, sh + 0x011000, 0);
    qtest_writel(qts, sh + 0x130500, 0x4fc);
    qtest_writel(qts, sh + 0x011028, 0x80000);
    for (unsigned i = 0; i < 63; i++) {
        qtest_writel(qts, sh + COMMAND, i);
    }
    qtest_writel(qts, sh + 0x011028, 0x10000000);
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
    enter_context(qts, SI_SHORT);
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

/*
 * HQ4 DMA through HEART SDRAM. On Octane the page list holds 32-bit
 * page-frame numbers from a list address that is only 4-byte aligned:
 * IRIX 6.5.30's IP30 X server lists that way, and the Linux IMPACTSR
 * driver's table is an array of 32-bit words too.
 */
#define RSS(reg, value) \
    (((uint64_t)(0x60001 | ((reg) << 6)) << 34) | (uint32_t)(value))
#define RSS_EXEC(reg, value) \
    (((uint64_t)(0x70001 | ((reg) << 6)) << 34) | (uint32_t)(value))

static void descriptor(QTestState *qts, uint64_t lo, uint8_t reg,
                       uint32_t value)
{
    qtest_writeq(qts, lo + COMMAND,
                 ((uint64_t)(0x00080004 | (reg << 8)) << 32) | value);
}

static uint32_t pixel(QTestState *qts, unsigned x, unsigned y)
{
    set_property(qts, "/machine/module0/mgras", "x-test-pixel-x", x);
    set_property(qts, "/machine/module0/mgras", "x-test-pixel-y", y);
    return property(qts, "/machine/module0/mgras", "x-test-pixel");
}

static void local_dma(QTestState *qts, uint64_t sh, uint64_t lo)
{
    const uint8_t source[] = { 0, 0x11, 0x22, 0x33, 0, 0x44, 0x55, 0x66 };
    uint8_t returned[sizeof(source)];

    /*
     * Split the two pixels across two ordinary HEART SDRAM pages. The list
     * starts 12 bytes into its page, as the IRIX ones do, so rounding its
     * address down to 8 bytes or reading 64-bit entries loses the second
     * page.
     */
    g_assert_cmpuint(memtx_writel(qts, 0x20008008, 0), ==, 0);
    qtest_writel(qts, 0x2000800c, 0x20010);
    qtest_writel(qts, 0x20008010, 0x20011);
    qtest_writel(qts, 0x20008014, 0);
    qtest_memwrite(qts, 0x20010ffc, source, sizeof(source));
    enter_context(qts, sh);
    qtest_writel(qts, sh + MAPPING, 0x00482008);
    qtest_writeq(qts, lo + MAPPING, 0x2000800c);
    qtest_writeq(qts, sh + MAPPING, 0x0048300400000002ULL);
    qtest_writeq(qts, lo + MAPPING, 0x00484b0400080000ULL);
    qtest_writeq(qts, sh + COMMAND, RSS(0x162, 0));
    qtest_writeq(qts, lo + COMMAND, RSS(0x163, 0x00ffffff));
    qtest_writeq(qts, sh + COMMAND, RSS(0x161, 3u << 26));
    qtest_writeq(qts, lo + COMMAND, RSS(0x46, (50u << 16) | 50));
    qtest_writeq(qts, sh + COMMAND, RSS_EXEC(0x47, (51u << 16) | 50));
    descriptor(qts, lo, 4, 8);
    descriptor(qts, lo, 5, 4);
    descriptor(qts, lo, 6, 4088);
    descriptor(qts, lo, 7, 1);
    descriptor(qts, lo, 8, 8);
    descriptor(qts, lo, 11, 0xb1);
    g_assert_cmphex(pixel(qts, 50, 50), ==, 0x00332211);
    g_assert_cmphex(pixel(qts, 51, 50), ==, 0x00665544);
    g_assert_cmphex(pixel(qts, 49, 50), ==, 0);
    g_assert_cmphex(pixel(qts, 52, 50), ==, 0);
    qtest_memset(qts, 0x20010ffc, 0xa5, sizeof(source));
    qtest_writeq(qts, lo + COMMAND, RSS(0x46, (50u << 16) | 973));
    qtest_writeq(qts, sh + COMMAND, RSS_EXEC(0x47, (51u << 16) | 973));
    descriptor(qts, lo, 11, 0xb9);
    qtest_memread(qts, 0x20010ffc, returned, sizeof(returned));
    g_assert_cmpmem(returned, sizeof(returned), source, sizeof(source));
    g_assert_cmphex(property(qts, "/machine/module0/mgras", "x-test-error"),
                    ==, 0);
}

static void test_local_dma(gconstpointer data)
{
    QTestState *qts = start(GPOINTER_TO_INT(data), ",graphics-board=si", "");

    local_dma(qts, SI_SHORT, SI_LONG);
    qtest_quit(qts);
}

/* Synthetic programmed vectors exercise transport, not IP30 allocation. */
static void interrupt_transport(QTestState *qts, uint64_t sh, uint64_t lo)
{
    const uint64_t other = 1ULL << 5;
    const uint64_t pending = 1ULL << 18;

    qtest_irq_intercept_out_named(qts, "/machine/heart", "processor");
    qtest_writel(qts, sh + 0x011060, 8u << 24);
    qtest_writel(qts, lo + 0x011068, 0x80);
    qtest_writel(qts, sh + 0x011070, 18u << 8);
    qtest_writel(qts, lo + 0x011040, 0x80000);
    qtest_writel(qts, sh + 0x011000, 0);
    qtest_writel(qts, lo + 0x130500, 0x4fc);
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
    qtest_writel(qts, sh + 0x011028, 0x80000);
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030) & (pending | other), ==,
                    other);
    qtest_writel(qts, lo + 0x130500, 0x4fc);
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_writel(qts, sh + 0x011028, 0x80000);
    qtest_writeq(qts, HEART + 0x10028, other);
    qtest_writel(qts, lo + 0x011070, 7u << 8);
    qtest_writeq(qts, HEART + 0x10000, 1u << 7);
    qtest_writel(qts, sh + 0x130500, 0x4fc);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    qtest_writel(qts, lo + 0x011028, 0x80000);
    g_assert_false(qtest_get_irq(qts, 0));
}

static void test_interrupt_transport(gconstpointer data)
{
    QTestState *qts = start(GPOINTER_TO_INT(data), ",graphics-board=si", "");

    interrupt_transport(qts, SI_SHORT, SI_LONG);
    qtest_quit(qts);
}

static void test_rejected_selection(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", octane2 ? "octane2,graphics-board=v12" :
        "octane,graphics-board=v12", "-display", "none", "-qmp", "stdio", NULL,
    };

    assert_launch_rejected(args, "graphics must be none, vpro or an MGRAS board");
}

static void test_rejected_widget(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    const unsigned widgets[] = { 8, 13, 15 };

    for (unsigned i = 0; i < G_N_ELEMENTS(widgets); i++) {
        g_autofree char *machine = g_strdup_printf(
            "%s,graphics-board=si,graphics-widget=%u",
            octane2 ? "octane2" : "octane", widgets[i]);
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", machine, "-display", "none",
            "-qmp", "stdio", NULL,
        };

        assert_launch_rejected(args, "graphics widget must be 9 to 12");
    }
}

static void test_rejected_widget_without_board(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    const char *selections[] = {
        ",graphics-widget=11",
        ",graphics-board=none,graphics-widget=9",
        ",graphics-board=vpro,graphics-widget=11",
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(selections); i++) {
        g_autofree char *machine = g_strdup_printf("%s%s",
            octane2 ? "octane2" : "octane", selections[i]);
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", machine, "-display", "none",
            "-qmp", "stdio", NULL,
        };

        assert_launch_rejected(args,
                               "graphics-widget requires an MGRAS graphics-board");
    }
}

static uint64_t short_base(unsigned widget)
{
    return XBOW + ((uint64_t)widget << 24);
}

static uint64_t long_base(unsigned widget)
{
    return (uint64_t)widget << 36;
}

/* One MicroLAN bit slot on the board's serial-number chip. */
static bool nic_slot(QTestState *qts, uint64_t base, unsigned pulse,
                     unsigned sample)
{
    uint32_t result;

    qtest_writel(qts, base + NIC, (pulse << 10) | (sample << 2));
    result = qtest_readl(qts, base + NIC);
    g_assert_cmphex(result & 2, ==, 2);
    return result & 1;
}

static void nic_write_byte(QTestState *qts, uint64_t base, uint8_t value)
{
    for (unsigned bit = 0; bit < 8; bit++) {
        nic_slot(qts, base, value & (1u << bit) ? 6 : 80,
                 value & (1u << bit) ? 13 : 30);
    }
}

static uint8_t nic_read_byte(QTestState *qts, uint64_t base)
{
    uint8_t value = 0;

    for (unsigned bit = 0; bit < 8; bit++) {
        value |= nic_slot(qts, base, 6, 13) << bit;
    }
    return value;
}

/* The board name the record carries, as the PROM's "Name:" text reads it. */
static void assert_record_name(QTestState *qts, uint64_t base,
                               const char *name, const char *barcode)
{
    uint8_t record[62];
    char text[14];

    g_assert_false(nic_slot(qts, base, 500, 65));
    nic_write_byte(qts, base, 0xcc);
    nic_write_byte(qts, base, 0xf0);
    nic_write_byte(qts, base, 0);
    nic_write_byte(qts, base, 0);
    for (unsigned i = 0; i < sizeof(record); i++) {
        record[i] = nic_read_byte(qts, base);
    }
    g_assert_cmpmem(record + 1, 10, barcode, 10);
    memset(text, ' ', sizeof(text));
    memcpy(text, name, strlen(name));
    g_assert_cmpmem(record + 48, sizeof(text), text, sizeof(text));
}

typedef struct OctaneVariant {
    const char *board;
    unsigned widget;
    bool implemented;
    uint8_t version1;
    const char *record;
} OctaneVariant;

/* Each variant at a port published inventories or the stock IDE use. */
static const OctaneVariant variants[] = {
    { "si", 11, true, 0x45, "GM10" },
    { "si-t", 10, false, 0x45, "GM10" },
    { "ssi", 9, true, 0x46, "GM20" },
    { "mxi", 12, false, 0x66, "GM20" },
    { "esi", 10, true, 0x51, "MOT10" },
    { "esi-t", 11, false, 0x51, "MOT10" },
    { "essi", 12, true, 0x52, "MOT20" },
    { "emxi", 12, false, 0x72, "MOT20" },
};

/*
 * Each variant answers its tuple and identity at its selected port only,
 * adds only that port's presence bit, and names its board in the record.
 */
static void test_variants(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);

    for (unsigned i = 0; i < G_N_ELEMENTS(variants); i++) {
        const OctaneVariant *v = &variants[i];
        g_autofree char *selection = g_strdup_printf(
            ",graphics-board=%s,graphics-widget=%u", v->board, v->widget);
        QTestState *qts = start(octane2, selection, v->implemented ? "" :
                                "-global sgi-mgras.x-unimplemented-board=on");
        uint64_t sh = short_base(v->widget);
        uint32_t word;

        g_assert_cmphex(property(qts, "/machine/crossbow", "links-present"),
                        ==, CORE_LINKS | (1u << v->widget));
        g_assert_cmphex(property(qts, "/machine/crossbow", "links-alive"),
                        ==, CORE_LINKS | (1u << v->widget));
        g_assert_cmpuint(memtx_readl(qts, sh + 4, &word), ==, 0);
        g_assert_cmphex(word, ==, SI_ID);
        g_assert_cmpuint(memtx_readl(qts, long_base(v->widget) + 4, &word),
                         ==, 0);
        g_assert_cmphex(word, ==, SI_ID);
        for (unsigned w = 9; w <= 12; w++) {
            if (w != v->widget) {
                g_assert_cmpuint(memtx_readl(qts, short_base(w) + 4, &word),
                                 !=, 0);
                g_assert_cmpuint(memtx_readl(qts, long_base(w) + 4, &word),
                                 !=, 0);
            }
        }
        g_assert_cmphex(qtest_readl(qts, sh + 0x072488) >> 24, ==,
                        v->version1);
        assert_record_name(qts, sh, v->record,
                           "EMUG00    ");
        assert_core(qts, octane2);
        qtest_quit(qts);
    }
}

/* Exercise the XIO-routed raster and readback path for every board tuple. */
static void test_family_composition(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);

    for (unsigned i = 0; i < G_N_ELEMENTS(variants); i++) {
        const OctaneVariant *v = &variants[i];
        g_autofree char *selection = g_strdup_printf(
            ",graphics-board=%s,graphics-widget=%u", v->board, v->widget);
        QTestState *qts = start(octane2, selection, v->implemented ? "" :
                                "-global sgi-mgras.x-unimplemented-board=on");
        uint64_t sh = short_base(v->widget);
        uint64_t lo = long_base(v->widget);
        uint32_t heart = qtest_readl(qts, HEART_WIDGET + CONTROL);
        uint32_t bridge = qtest_readl(qts, BRIDGE + CONTROL);
        uint32_t control = 0x39a562d7;

        qtest_writel(qts, sh + CONTROL, control);
        local_dma(qts, sh, lo);

        /* The command path leaves the owning board's control word intact. */
        g_assert_cmphex(qtest_readl(qts, sh + CONTROL), ==, control);
        g_assert_cmphex(qtest_readl(qts, lo + CONTROL), ==, control);
        g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + CONTROL), ==, heart);
        g_assert_cmphex(qtest_readl(qts, BRIDGE + CONTROL), ==, bridge);
        assert_core(qts, octane2);
        qtest_quit(qts);
    }
}

/* DMA into HEART memory and interrupt packets to HEART from widget 12. */
static void test_widget_transport(gconstpointer data)
{
    QTestState *qts = start(GPOINTER_TO_INT(data),
                            ",graphics-board=essi,graphics-widget=12", "");

    local_dma(qts, short_base(12), long_base(12));
    qtest_quit(qts);
    qts = start(GPOINTER_TO_INT(data),
                ",graphics-board=essi,graphics-widget=12", "");
    interrupt_transport(qts, short_base(12), long_base(12));
    qtest_quit(qts);
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
        { "local-dma", test_local_dma },
        { "interrupt-transport", test_interrupt_transport },
        { "rejected-selection", test_rejected_selection },
        { "rejected-widget", test_rejected_widget },
        { "rejected-widget-without-board", test_rejected_widget_without_board },
        { "variants", test_variants },
        { "family-composition", test_family_composition },
        { "widget-transport", test_widget_transport },
    };
    const uint8_t erased[] = { 0xff, 0xff, 0xff, 0xff };
    int fd, result;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("octane-mgras-prom-XXXXXX", &flash_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, erased, sizeof(erased)), ==, sizeof(erased));
    close(fd);
    for (unsigned profile = 0; profile < 2; profile++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(tests); i++) {
            g_autofree char *path = g_strdup_printf("/%s/mgras/%s",
                profile ? "octane2" : "octane", tests[i].name);

            qtest_add_data_func(path, GINT_TO_POINTER(profile), tests[i].test);
        }
    }
    result = g_test_run();
    g_assert_cmpint(unlink(flash_path), ==, 0);
    g_free(flash_path);
    return result;
}
