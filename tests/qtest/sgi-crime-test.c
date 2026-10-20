/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"
#include "memtx.h"
#include "sgi-migrate.h"

#define CRIME_PATH "/machine/peripheral/crime"
#define CRIME_BASE 0x14000000ULL
#define CRIME_ID (CRIME_BASE + 0x00)
#define CRIME_ID_LOW (CRIME_BASE + 0x04)
#define CRIME_CONTROL (CRIME_BASE + 0x08)
#define CRIME_STATUS (CRIME_BASE + 0x10)
#define CRIME_ENABLE (CRIME_BASE + 0x18)
#define CRIME_SOFTWARE (CRIME_BASE + 0x20)
#define CRIME_HARDWARE (CRIME_BASE + 0x28)
#define CRIME_TIMER (CRIME_BASE + 0x38)
#define CRIME_MEMORY_CONTROL (CRIME_BASE + 0x200)
#define CRIME_CPU_ERROR_STATUS (CRIME_BASE + 0x4048)
#define CRIME_MEMORY_ERROR_STATUS (CRIME_BASE + 0x250)
#define CRIME_MEMORY_ERROR_ADDRESS (CRIME_BASE + 0x258)
#define CRIME_RENDER_STATUS (CRIME_BASE + 0x01004000ULL)
#define CRIME_BANK(i) (CRIME_BASE + 0x208 + 8 * (i))
#define BIT64(n) (1ULL << (n))
#define CRIME_FITTED_RAM (64ULL * 1024 * 1024)
#define CRIME_LARGE_FIT (128ULL * 1024 * 1024)
#define CRIME_BANK_SIZE (128ULL * 1024 * 1024)
#define CRIME_HIGH_RAM 0x20000000ULL
#define CRIME_FULL_RAM 0x40000000ULL
#define CRIME_NO_ECC_RAM 0x80000000ULL
#define CRIME_CARRIER_BASE 0x100000000ULL
#define CRIME_CARRIER_PATH "/machine/unattached/system[0]"

static QTestState *crime_start(const char *extra)
{
    QTestState *s = qtest_initf(MEMTX_TEST_DEVICE "-M none -cpu R10000 "
        "-S -device sgi-crime,id=crime,interface-revision=1 %s", extra);

    qtest_sysbus_mmio_map(s, CRIME_PATH, 0, CRIME_BASE);
    return s;
}

static QTestState *crime_start_id_word_lane(void)
{
    QTestState *s = qtest_init(MEMTX_TEST_DEVICE "-M none -cpu R10000 "
        "-S -device sgi-crime,id=crime,interface-revision=1,"
        "prom-id-word=on");

    qtest_sysbus_mmio_map(s, CRIME_PATH, 0, CRIME_BASE);
    return s;
}

static QTestState *crime_start_with_ram_size(const char *size,
                                             const char *extra)
{
    QTestState *s = qtest_initf(MEMTX_TEST_DEVICE "-M none -cpu R10000 "
        "-S -object memory-backend-ram,id=fit,size=%s "
        "-device sgi-crime,id=crime,interface-revision=1,"
        "ram=/objects/fit/fit[0] %s", size, extra);

    qtest_sysbus_mmio_map(s, CRIME_PATH, 0, CRIME_BASE);
    return s;
}

static QTestState *crime_start_with_ram(const char *extra)
{
    return crime_start_with_ram_size("64M", extra);
}

static QTestState *crime_start_populated_options(bool approx,
                                                  const char *extra)
{
    QTestState *s = qtest_initf(MEMTX_TEST_DEVICE "-M none -cpu R10000 "
        "-S -object memory-backend-ram,id=fit,size=1G "
        "-device sgi-crime,id=crime,interface-revision=1,"
        "ram=/objects/fit/fit[0],eight-128m-banks=on%s %s",
        approx ? ",reset-low-ram-approx=on" : "", extra);

    qtest_sysbus_mmio_map(s, CRIME_PATH, 0, CRIME_BASE);
    return s;
}

static QTestState *crime_start_populated(void)
{
    return crime_start_populated_options(false, "");
}

static QTestState *crime_start_populated_approx(const char *extra)
{
    return crime_start_populated_options(true, extra);
}

static QTestState *crime_start_empty_renderer_status(void)
{
    QTestState *s = qtest_init(MEMTX_TEST_DEVICE "-M none -cpu R10000 "
        "-S -object memory-backend-ram,id=fit,size=1G "
        "-device sgi-crime,id=crime,interface-revision=1,"
        "ram=/objects/fit/fit[0],eight-128m-banks=on,"
        "reset-low-ram-approx=on,empty-renderer-status=on");

    qtest_sysbus_mmio_map(s, CRIME_PATH, 0, CRIME_BASE);
    return s;
}

static void test_empty_renderer_status(void)
{
    static const uint64_t other_offsets[] = {
        CRIME_BASE + 0x01000000ULL,
        CRIME_BASE + 0x01001000ULL,
        CRIME_BASE + 0x01002000ULL,
        CRIME_BASE + 0x01003000ULL,
        CRIME_RENDER_STATUS + 4,
        CRIME_BASE + 0x01004ffcULL,
    };
    QTestState *s = crime_start_populated_approx("");
    uint8_t byte;
    uint16_t half;
    uint32_t word;
    uint64_t wide;

    g_assert_cmpint(memtx_readl(s, CRIME_RENDER_STATUS, &word),
                    !=, MEMTX_OK);
    qtest_quit(s);

    s = crime_start_empty_renderer_status();
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmpint(memtx_readl(s, CRIME_RENDER_STATUS, &word),
                        ==, MEMTX_OK);
        g_assert_cmphex(word, ==, 0x1e000000);
    }
    g_assert_cmpint(memtx_readb(s, CRIME_RENDER_STATUS, &byte),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readw(s, CRIME_RENDER_STATUS, &half),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readq(s, CRIME_RENDER_STATUS, &wide),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeb(s, CRIME_RENDER_STATUS, 0),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_writew(s, CRIME_RENDER_STATUS, 0),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_writel(s, CRIME_RENDER_STATUS, 0),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, CRIME_RENDER_STATUS, 0),
                    !=, MEMTX_OK);
    for (unsigned i = 0; i < ARRAY_SIZE(other_offsets); i++) {
        g_assert_cmpint(memtx_readl(s, other_offsets[i], &word),
                        !=, MEMTX_OK);
        g_assert_cmpint(memtx_writel(s, other_offsets[i], 0),
                        !=, MEMTX_OK);
    }
    g_assert_cmpint(memtx_readl(s, CRIME_RENDER_STATUS, &word),
                    ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0x1e000000);
    qtest_system_reset(s);
    g_assert_cmpint(memtx_readl(s, CRIME_RENDER_STATUS, &word),
                    ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0x1e000000);
    qtest_quit(s);
}

static void crime_assert_ram_view(QTestState *s, uint64_t address)
{
    g_autofree char *mtree = qtest_hmp(s, "info mtree -f");
    const char *view = strstr(mtree, "Root memory region: system");
    const char *end;

    g_assert_nonnull(view);
    view = strchr(view, '\n');
    g_assert_nonnull(view);
    view++;
    end = strstr(view, "\nFlatView #");
    if (!end) {
        end = view + strlen(view);
    }
    while (view < end) {
        const char *next = strchr(view, '\n');
        uint64_t first, last;
        char type[32];

        if (!next || next > end) {
            next = end;
        }
        if (sscanf(view, " %" SCNx64 "-%" SCNx64 " (prio %*d, %31[^)])",
                   &first, &last, type) == 3 &&
            address >= first && address <= last) {
            g_assert_cmpstr(type, ==, "ram");
            return;
        }
        view = next + (next < end);
    }
    g_assert_not_reached();
}

static QTestState *crime_start_with_carrier(void)
{
    QTestState *s = qtest_init(MEMTX_TEST_DEVICE "-M none -cpu R10000 "
        "-S -object memory-backend-ram,id=fit,size=1G "
        "-device sgi-crime,id=crime,interface-revision=1,"
        "ram=/objects/fit/fit[0],eight-128m-banks=on,"
        "decoded-carrier=" CRIME_CARRIER_PATH ",decoded-base=0x100000000");

    qtest_sysbus_mmio_map(s, CRIME_PATH, 0, CRIME_BASE);
    return s;
}

static QTestState *crime_start_with_carrier_approx(void)
{
    QTestState *s = qtest_init(MEMTX_TEST_DEVICE "-M none -cpu R10000 "
        "-S -object memory-backend-ram,id=fit,size=1G "
        "-device sgi-crime,id=crime,interface-revision=1,"
        "ram=/objects/fit/fit[0],eight-128m-banks=on,"
        "reset-low-ram-approx=on,decoded-carrier=" CRIME_CARRIER_PATH
        ",decoded-base=0x100000000");

    qtest_sysbus_mmio_map(s, CRIME_PATH, 0, CRIME_BASE);
    return s;
}

static void crime_assert_carrier_properties(QTestState *s)
{
    QDict *reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'decoded-carrier'}}", CRIME_PATH);

    g_assert_cmpstr(qdict_get_str(reply, "return"), ==, CRIME_CARRIER_PATH);
    qobject_unref(reply);
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'decoded-base'}}", CRIME_PATH);
    g_assert_cmphex(qdict_get_uint(reply, "return"), ==, CRIME_CARRIER_BASE);
    qobject_unref(reply);
}

static void crime_assert_carrier_properties_locked(QTestState *s)
{
    QDict *reply = qtest_qmp(s,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'decoded-base','value':8589934592}}", CRIME_PATH);

    g_assert_nonnull(qdict_get_qdict(reply, "error"));
    qobject_unref(reply);
    reply = qtest_qmp(s,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'decoded-carrier','value':null}}", CRIME_PATH);
    g_assert_nonnull(qdict_get_qdict(reply, "error"));
    qobject_unref(reply);
    crime_assert_carrier_properties(s);
}

static void crime_program_banks(QTestState *s)
{
    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(s, CRIME_BANK(bank), 0x100 | (bank << 2));
    }
}

static void mace(QTestState *s, unsigned source, bool level)
{
    qtest_set_irq_in(s, CRIME_PATH, "mace", source, level);
}

static void assert_cpu_ip(QTestState *s, unsigned expected)
{
    for (unsigned output = 0; output < 5; output++) {
        g_assert_cmpint(qtest_get_irq(s, output), ==,
                        !!(expected & (1U << output)));
    }
}

static void test_reset_and_access(void)
{
    QTestState *s = crime_start("");
    uint64_t value;
    uint32_t narrow;

    g_assert_cmphex(qtest_readq(s, CRIME_ID), ==, 0xb1);
    g_assert_cmphex(qtest_readq(s, CRIME_CONTROL), ==, 0x120);
    g_assert_cmphex(qtest_readq(s, CRIME_ENABLE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_SOFTWARE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_HARDWARE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_STATUS), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_CONTROL), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_CPU_ERROR_STATUS), ==, 0);
    for (unsigned bank = 0; bank < 8; bank++) {
        g_assert_cmphex(qtest_readq(s, CRIME_BANK(bank)), ==, bank);
        g_assert_cmpuint(memtx_writeq(s, CRIME_BANK(bank), 0x100 | bank),
                         !=, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, CRIME_BANK(bank)), ==, bank);
    }
    g_assert_cmpuint(memtx_readl(s, CRIME_ID, &narrow), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readq(s, CRIME_ID + 4, &value), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writel(s, CRIME_ENABLE, 1U << 4), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writel(s, CRIME_SOFTWARE, 1U << 28),
                     !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_ENABLE + 4, BIT64(4)),
                     !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_ENABLE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_SOFTWARE), ==, 0);
    g_assert_cmpuint(memtx_writeq(s, CRIME_CONTROL, BIT64(9)), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_CONTROL, BIT64(11)), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_CONTROL), ==, 0x120);
    g_assert_cmpuint(memtx_writeq(s, CRIME_MEMORY_CONTROL, 1), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_CONTROL), ==, 0);
    g_assert_cmpuint(memtx_writeq(s, CRIME_HARDWARE, 1), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readq(s, CRIME_BASE + 0x48, &value), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readq(s, CRIME_BASE + 0x100, &value), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_BASE + 0x100, 0), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_ENABLE), ==, 0);
    qtest_quit(s);
}

static void test_prom_id_word(void)
{
    static const uint64_t other_words[] = {
        CRIME_ID, CRIME_CONTROL, CRIME_STATUS, CRIME_TIMER,
        CRIME_BASE + 0x40,
    };
    QTestState *s = crime_start("");
    g_autoptr(QDict) default_property = NULL;
    g_autoptr(QDict) selected_property = NULL;
    uint8_t byte;
    uint16_t half;
    uint32_t word;
    uint64_t wide;

    g_assert_cmpint(memtx_readl(s, CRIME_ID_LOW, &word), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_ID), ==, 0xb1);
    default_property = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'prom-id-word'}}", CRIME_PATH);
    g_assert_nonnull(qdict_get(default_property, "return"));
    g_assert_false(qdict_get_bool(default_property, "return"));
    qtest_system_reset(s);
    g_assert_cmpint(memtx_readl(s, CRIME_ID_LOW, &word), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_ID), ==, 0xb1);
    qtest_quit(s);

    s = crime_start_id_word_lane();
    selected_property = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'prom-id-word'}}", CRIME_PATH);
    g_assert_nonnull(qdict_get(selected_property, "return"));
    g_assert_true(qdict_get_bool(selected_property, "return"));
    g_assert_cmpint(memtx_readl(s, CRIME_ID_LOW, &word), ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0xb1);
    g_assert_cmphex(qtest_readq(s, CRIME_ID), ==, 0xb1);
    for (unsigned i = 0; i < ARRAY_SIZE(other_words); i++) {
        g_assert_cmpint(memtx_readl(s, other_words[i], &word),
                        !=, MEMTX_OK);
    }
    g_assert_cmpint(memtx_readb(s, CRIME_ID_LOW, &byte), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readw(s, CRIME_ID_LOW, &half), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readq(s, CRIME_ID_LOW, &wide), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readl(s, CRIME_ID_LOW + 1, &word), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeb(s, CRIME_ID_LOW, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writew(s, CRIME_ID_LOW, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writel(s, CRIME_ID_LOW, 0xa1), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, CRIME_ID_LOW, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, CRIME_ID, 0xa1), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readl(s, CRIME_ID_LOW, &word), ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0xb1);
    g_assert_cmphex(qtest_readq(s, CRIME_ID), ==, 0xb1);
    qtest_system_reset(s);
    g_assert_cmpint(memtx_readl(s, CRIME_ID_LOW, &word), ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0xb1);
    g_assert_cmphex(qtest_readq(s, CRIME_ID), ==, 0xb1);
    qtest_quit(s);
}

static void test_timer_counter(void)
{
    QTestState *s = crime_start("");

    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 0);
    g_assert_cmpuint(memtx_writeq(s, CRIME_TIMER, 0), ==, MEMTX_OK);
    g_usleep(20000);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 0);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 66666666);

    qtest_writeq(s, CRIME_TIMER, 0x12345678);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 0x12345678);
    qtest_clock_step(s, 15);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 0x12345678);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 0x12345679);

    qtest_writeq(s, CRIME_TIMER, 0xfffffffe);
    qtest_clock_step(s, 15);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 0xfffffffe);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 0xffffffff);
    qtest_clock_step(s, 15);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 0);

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 0);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 66666666);
    qtest_quit(s);
}

static void test_timer_invalid_access(void)
{
    QTestState *s = crime_start("");
    uint8_t byte;
    uint16_t half;
    uint32_t word;
    uint64_t wide;

    qtest_writeq(s, CRIME_TIMER, 0x100);
    g_assert_cmpuint(memtx_readb(s, CRIME_TIMER, &byte), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readw(s, CRIME_TIMER, &half), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readl(s, CRIME_TIMER, &word), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readq(s, CRIME_TIMER + 4, &wide), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readq(s, CRIME_TIMER + 0x10, &wide),
                     !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeb(s, CRIME_TIMER, 0), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writew(s, CRIME_TIMER, 0), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writel(s, CRIME_TIMER, 0), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_TIMER + 4, 0), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_TIMER + 0x10, 0),
                     !=, MEMTX_OK);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 66666922);
    g_assert_cmpuint(memtx_writeq(s, CRIME_TIMER, BIT64(32)), !=, MEMTX_OK);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 133333588);
    qtest_quit(s);
}

static void test_timer_no_irq(void)
{
    QTestState *s = crime_start("");

    qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
    qtest_writeq(s, CRIME_ENABLE, BIT64(5));
    qtest_writeq(s, CRIME_TIMER, 0xfffffffe);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readq(s, CRIME_TIMER), ==, 66666664);
    g_assert_cmphex(qtest_readq(s, CRIME_HARDWARE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_STATUS), ==, 0);
    assert_cpu_ip(s, 0);
    qtest_quit(s);
}

static void test_mace_level_routing(void)
{
    const struct {
        unsigned source;
        unsigned output;
    } cases[] = {
        { 0, 1 }, /* IP3 */
        { 1, 1 },
        { 2, 1 },
        { 3, 0 }, /* IP2 */
        { 4, 0 }, /* IP2 */
        { 5, 0 },
        { 8, 1 }, /* IP3 */
        { 9, 1 },
        { 10, 1 },
        { 11, 1 },
        { 12, 1 },
        { 13, 1 },
        { 14, 1 },
        { 15, 1 },
        { 6, 3 }, /* IP5 */
        { 7, 4 }, /* IP6 */
    };
    QTestState *s = crime_start("");

    qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        uint64_t bit = BIT64(cases[i].source);

        mace(s, cases[i].source, true);
        g_assert_cmphex(qtest_readq(s, CRIME_HARDWARE), ==, bit);
        g_assert_cmphex(qtest_readq(s, CRIME_STATUS), ==, bit);
        assert_cpu_ip(s, 0);
        qtest_writeq(s, CRIME_ENABLE, bit);
        assert_cpu_ip(s, 1U << cases[i].output);
        qtest_writeq(s, CRIME_ENABLE, 0);
        assert_cpu_ip(s, 0);
        g_assert_cmphex(qtest_readq(s, CRIME_HARDWARE), ==, bit);
        mace(s, cases[i].source, false);
        g_assert_cmphex(qtest_readq(s, CRIME_STATUS), ==, 0);
    }
    qtest_writeq(s, CRIME_ENABLE, BIT64(8) | BIT64(9));
    mace(s, 8, true);
    mace(s, 9, true);
    assert_cpu_ip(s, 1U << 1);
    mace(s, 8, false);
    assert_cpu_ip(s, 1U << 1);
    mace(s, 9, false);
    assert_cpu_ip(s, 0);
    qtest_quit(s);
}

static void test_software_interrupts(void)
{
    QTestState *s = crime_start("");

    qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
    qtest_writeq(s, CRIME_SOFTWARE, BIT64(28) | BIT64(29));
    g_assert_cmphex(qtest_readq(s, CRIME_STATUS), ==,
                    BIT64(28) | BIT64(29));
    assert_cpu_ip(s, 0);
    qtest_writeq(s, CRIME_ENABLE, BIT64(28) | BIT64(29));
    assert_cpu_ip(s, (1U << 2) | (1U << 3));
    g_assert_cmpuint(memtx_writeq(s, CRIME_ENABLE, BIT64(27)),
                     !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_ENABLE), ==,
                    BIT64(28) | BIT64(29));
    qtest_writeq(s, CRIME_SOFTWARE, BIT64(29));
    assert_cpu_ip(s, 1U << 3);
    g_assert_cmpuint(memtx_writeq(s, CRIME_SOFTWARE, BIT64(27)),
                     !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_SOFTWARE), ==, BIT64(29));
    qtest_writeq(s, CRIME_SOFTWARE, 0);
    assert_cpu_ip(s, 0);
    qtest_quit(s);
}

static void test_reset_pending_irq(void)
{
    QTestState *s = crime_start("");

    qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
    qtest_writeq(s, CRIME_ENABLE, BIT64(4) | BIT64(28));
    qtest_writeq(s, CRIME_SOFTWARE, BIT64(28));
    mace(s, 4, true);
    assert_cpu_ip(s, (1U << 0) | (1U << 2));
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, CRIME_ENABLE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_SOFTWARE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_HARDWARE), ==, BIT64(4));
    assert_cpu_ip(s, 0);
    mace(s, 4, false);
    g_assert_cmphex(qtest_readq(s, CRIME_HARDWARE), ==, 0);
    qtest_quit(s);
}

static void test_pending_irq_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("crime-irq-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = crime_start("");
    to = crime_start("-incoming defer");
    qtest_irq_intercept_out_named(to, CRIME_PATH, "cpu-ip");
    qtest_writeq(from, CRIME_ENABLE, BIT64(4) | BIT64(29));
    qtest_writeq(from, CRIME_SOFTWARE, BIT64(29));
    mace(from, 4, true);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readq(to, CRIME_HARDWARE), ==, BIT64(4));
    g_assert_cmphex(qtest_readq(to, CRIME_SOFTWARE), ==, BIT64(29));
    assert_cpu_ip(to, (1U << 0) | (1U << 3));
    mace(to, 4, false);
    assert_cpu_ip(to, 1U << 3);
    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(directory);
}

static void test_timer_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("crime-timer-XXXXXX",
                                                  &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = crime_start("");
    to = crime_start("-incoming defer");
    qtest_irq_intercept_out_named(to, CRIME_PATH, "cpu-ip");
    qtest_writeq(from, CRIME_ENABLE, BIT64(4) | BIT64(29));
    qtest_writeq(from, CRIME_SOFTWARE, BIT64(29));
    mace(from, 4, true);
    qtest_writeq(from, CRIME_TIMER, 0x100);
    qtest_clock_step(from, 16);
    g_assert_cmphex(qtest_readq(from, CRIME_TIMER), ==, 0x101);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readq(to, CRIME_TIMER), ==, 0x101);
    qtest_clock_step(to, 15);
    g_assert_cmphex(qtest_readq(to, CRIME_TIMER), ==, 0x102);
    g_assert_cmphex(qtest_readq(to, CRIME_HARDWARE), ==, BIT64(4));
    g_assert_cmphex(qtest_readq(to, CRIME_SOFTWARE), ==, BIT64(29));
    assert_cpu_ip(to, (1U << 0) | (1U << 3));
    qtest_writeq(to, CRIME_TIMER, 0xfffffffe);
    qtest_clock_step(to, 31);
    g_assert_cmphex(qtest_readq(to, CRIME_TIMER), ==, 0);
    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(directory);
}

static void test_ram_aliases_and_holes(void)
{
    QTestState *s = crime_start_with_ram("");
    const uint64_t bases[] = { 0, CRIME_FULL_RAM, CRIME_NO_ECC_RAM };
    const uint64_t pattern[] = {
        0x8e1a2b3c4d5e6f70ULL,
        0x1534a5c6d7e8f901ULL,
        0xfedcba9876543210ULL,
    };
    uint8_t byte;

    /* No-ECC is a data alias here; ECC checking is not modeled. */
    for (unsigned source = 0; source < ARRAY_SIZE(bases); source++) {
        qtest_writeq(s, bases[source] + 0x100, pattern[source]);
        for (unsigned view = 0; view < ARRAY_SIZE(bases); view++) {
            g_assert_cmphex(qtest_readq(s, bases[view] + 0x100), ==,
                            pattern[source]);
        }
    }
    for (unsigned source = 0; source < ARRAY_SIZE(bases); source++) {
        qtest_writeb(s, bases[source] + CRIME_FITTED_RAM - 1,
                     0xa0 + source);
        for (unsigned view = 0; view < ARRAY_SIZE(bases); view++) {
            g_assert_cmphex(qtest_readb(s, bases[view] +
                                            CRIME_FITTED_RAM - 1), ==,
                            0xa0 + source);
        }
    }
    for (unsigned view = 0; view < ARRAY_SIZE(bases); view++) {
        g_assert_cmpuint(memtx_readb(s, bases[view] + CRIME_FITTED_RAM,
                                      &byte), !=, MEMTX_OK);
        g_assert_cmpuint(memtx_writeb(s, bases[view] + CRIME_FITTED_RAM,
                                       0xff), !=, MEMTX_OK);
    }
    g_assert_cmpuint(memtx_readb(s, 0x20000000, &byte), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readb(s, CRIME_FITTED_RAM - 1), ==, 0xa2);
    for (unsigned bank = 0; bank < 8; bank++) {
        g_assert_cmphex(qtest_readq(s, CRIME_BANK(bank)), ==, bank);
        g_assert_cmpuint(memtx_writeq(s, CRIME_BANK(bank), 0x100 | bank),
                         !=, MEMTX_OK);
    }
    qtest_quit(s);
}

static void test_ram_alias_mapping_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("crime-ram-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    uint8_t byte;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = crime_start_with_ram("");
    to = crime_start_with_ram("-incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    /* -M none does not register the backing RAM for content migration. */
    qtest_writeq(to, CRIME_FULL_RAM + 0x200, 0x9264af731d0be5c8ULL);
    g_assert_cmphex(qtest_readq(to, 0x200), ==,
                    0x9264af731d0be5c8ULL);
    g_assert_cmphex(qtest_readq(to, CRIME_NO_ECC_RAM + 0x200), ==,
                    0x9264af731d0be5c8ULL);
    g_assert_cmpuint(memtx_readb(to, CRIME_FULL_RAM + CRIME_FITTED_RAM,
                                  &byte), !=, MEMTX_OK);
    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(directory);
}

static void test_ram_128m_aliases_and_holes(void)
{
    QTestState *s = crime_start_with_ram_size("128M", "");
    const uint64_t bases[] = { 0, CRIME_FULL_RAM, CRIME_NO_ECC_RAM };
    uint8_t byte;

    for (unsigned source = 0; source < ARRAY_SIZE(bases); source++) {
        qtest_writeq(s, bases[source] + CRIME_LARGE_FIT - 8,
                     0x0123456789abcdefULL + source);
        for (unsigned view = 0; view < ARRAY_SIZE(bases); view++) {
            g_assert_cmphex(qtest_readq(s, bases[view] +
                                        CRIME_LARGE_FIT - 8), ==,
                            0x0123456789abcdefULL + source);
        }
    }
    for (unsigned source = 0; source < ARRAY_SIZE(bases); source++) {
        qtest_writeb(s, bases[source] + CRIME_LARGE_FIT - 1,
                     0xa0 + source);
        for (unsigned view = 0; view < ARRAY_SIZE(bases); view++) {
            g_assert_cmphex(qtest_readb(s, bases[view] +
                                            CRIME_LARGE_FIT - 1), ==,
                            0xa0 + source);
        }
    }
    for (unsigned view = 0; view < ARRAY_SIZE(bases); view++) {
        g_assert_cmpuint(memtx_readb(s, bases[view] + CRIME_LARGE_FIT,
                                      &byte), !=, MEMTX_OK);
        g_assert_cmpuint(memtx_writeb(s, bases[view] + CRIME_LARGE_FIT,
                                       0xff), !=, MEMTX_OK);
    }
    g_assert_cmpuint(memtx_readb(s, 0x20000000, &byte), !=, MEMTX_OK);
    for (unsigned bank = 0; bank < 8; bank++) {
        g_assert_cmphex(qtest_readq(s, CRIME_BANK(bank)), ==, bank);
        g_assert_cmpuint(memtx_writeq(s, CRIME_BANK(bank), 0x100),
                         !=, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, CRIME_BANK(bank)), ==, bank);
    }
    g_assert_cmphex(qtest_readb(s, CRIME_FULL_RAM + CRIME_LARGE_FIT - 1),
                    ==, 0xa2);
    g_assert_cmpuint(memtx_readb(s, CRIME_FULL_RAM + CRIME_LARGE_FIT,
                                  &byte), !=, MEMTX_OK);
    qtest_quit(s);
}

static void test_ram_unsupported_fit(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*requires a 64 or 128 MiB RAM region*");
        return;
    }
    s = qtest_init(MEMTX_TEST_DEVICE "-M none -cpu R10000 -S "
                   "-object memory-backend-ram,id=fit,size=32M "
                   "-device sgi-crime,id=crime,interface-revision=1,"
                   "ram=/objects/fit/fit[0]");
    qtest_quit(s);
}

static void test_ram_readonly_fit(void)
{
    GError *error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *path = NULL;
    QTestState *s;
    int fd;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*requires writable RAM*");
        return;
    }
    directory = g_dir_make_tmp("crime-rom-XXXXXX", &error);
    g_assert_no_error(error);
    path = g_build_filename(directory, "backing", NULL);
    fd = g_open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, CRIME_FITTED_RAM), ==, 0);
    close(fd);
    s = qtest_initf(MEMTX_TEST_DEVICE "-M none -cpu R10000 -S "
                    "-object memory-backend-file,id=rom,size=64M,"
                    "mem-path=%s,readonly=on,rom=on "
                    "-device sgi-crime,id=crime,interface-revision=1,"
                    "ram=/objects/rom/rom[0]", path);
    qtest_quit(s);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_populated_reset_controls(void)
{
    QTestState *s = crime_start_populated();
    uint64_t value;
    uint32_t narrow;

    for (unsigned bank = 0; bank < 8; bank++) {
        g_assert_cmphex(qtest_readq(s, CRIME_BANK(bank)), ==, bank);
    }
    g_assert_cmpuint(memtx_readl(s, CRIME_BANK(0), &narrow), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readq(s, CRIME_BANK(0) + 4, &value),
                     !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_BANK(0), 0x120), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_BANK(0)), ==, 0);
    qtest_quit(s);
}

static void test_populated_banks_and_views(void)
{
    QTestState *s = crime_start_populated();

    crime_program_banks(s);
    for (unsigned bank = 0; bank < 8; bank++) {
        uint64_t offset = bank * CRIME_BANK_SIZE;
        uint8_t first = 0x10 + bank;
        uint8_t last = 0xe0 + bank;

        g_assert_cmphex(qtest_readq(s, CRIME_BANK(bank)), ==,
                        0x100 | (bank << 2));
        qtest_writeb(s, CRIME_FULL_RAM + offset, first);
        qtest_writeb(s, CRIME_FULL_RAM + offset + CRIME_BANK_SIZE - 1,
                     last);
        g_assert_cmphex(qtest_readb(s, CRIME_NO_ECC_RAM + offset), ==,
                        first);
        g_assert_cmphex(qtest_readb(s, CRIME_NO_ECC_RAM + offset +
                                       CRIME_BANK_SIZE - 1), ==, last);
        if (bank < 2) {
            g_assert_cmphex(qtest_readb(s, offset), ==, first);
            g_assert_cmphex(qtest_readb(s, offset + CRIME_BANK_SIZE - 1),
                            ==, last);
        }
        if (bank >= 4) {
            uint64_t high = CRIME_HIGH_RAM +
                            (bank - 4) * CRIME_BANK_SIZE;

            g_assert_cmphex(qtest_readb(s, high), ==, first);
            g_assert_cmphex(qtest_readb(s, high + CRIME_BANK_SIZE - 1),
                            ==, last);
        }
    }
    qtest_quit(s);
}

static void test_populated_priority_and_remap(void)
{
    QTestState *s = crime_start_populated();
    uint64_t value;

    qtest_writeq(s, CRIME_BANK(0), 0x100);
    qtest_writeq(s, CRIME_BANK(1), 0x104);
    qtest_writeb(s, CRIME_FULL_RAM, 0x31);
    qtest_writeb(s, CRIME_FULL_RAM + CRIME_BANK_SIZE, 0x72);
    qtest_writeq(s, CRIME_BANK(1), 0x100);
    g_assert_cmphex(qtest_readb(s, CRIME_FULL_RAM), ==, 0x31);
    qtest_writeq(s, CRIME_BANK(0), 0x104);
    g_assert_cmphex(qtest_readb(s, CRIME_FULL_RAM), ==, 0x72);
    g_assert_cmphex(qtest_readb(s, CRIME_FULL_RAM + CRIME_BANK_SIZE),
                    ==, 0x31);
    qtest_writeq(s, CRIME_BANK(0), 0x107);
    g_assert_cmphex(qtest_readq(s, CRIME_BANK(0)), ==, 0x107);
    g_assert_cmphex(qtest_readb(s, CRIME_FULL_RAM + CRIME_BANK_SIZE),
                    ==, 0x31);

    /* Bank zero's unsupported 16 Mbit match outranks bank one's data. */
    qtest_writeq(s, CRIME_BANK(0), 0);
    g_assert_cmpuint(memtx_readq(s, CRIME_FULL_RAM, &value), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_FULL_RAM,
                                  0x123456789abcdef0ULL), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_ERROR_STATUS) &
                    (BIT64(25) | BIT64(26)), ==, 0);
    qtest_writeq(s, CRIME_BANK(0), 0x107);
    g_assert_cmphex(qtest_readb(s, CRIME_FULL_RAM), ==, 0x72);
    qtest_quit(s);
}

static void test_populated_all_controls_at_zero(void)
{
    QTestState *s = crime_start_populated();
    const uint64_t last = 0x91a2b3c4d5e6f708ULL;

    qtest_writeq(s, CRIME_BANK(0), 0x104);
    qtest_writeq(s, CRIME_FULL_RAM + 2 * CRIME_BANK_SIZE - 8, last);
    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(s, CRIME_BANK(bank), 0x100);
    }
    g_assert_cmphex(qtest_readq(s, CRIME_FULL_RAM + CRIME_BANK_SIZE - 8),
                    ==, last);
    g_assert_cmphex(qtest_readq(s, CRIME_BANK_SIZE - 8), ==, last);
    g_assert_cmphex(qtest_readq(s, CRIME_NO_ECC_RAM + CRIME_BANK_SIZE - 8),
                    ==, last);
    qtest_quit(s);
}

static void test_populated_unmatched_write(void)
{
    QTestState *s = crime_start_populated();

    qtest_writeq(s, CRIME_BANK(0), 0x100);
    qtest_writeq(s, CRIME_FULL_RAM + 0x100, 0x3654aabbccdd9021ULL);
    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(s, CRIME_BANK(bank), 0x104);
    }
    memtx_writeq(s, CRIME_FULL_RAM + 0x01000000, 0x123456789abcdef0ULL);
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_ERROR_STATUS) & BIT64(26),
                    ==, BIT64(26));
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_ERROR_ADDRESS), ==,
                    0x01000000);
    g_assert_cmphex(qtest_readq(s, CRIME_FULL_RAM + CRIME_BANK_SIZE + 0x100),
                    ==, 0x3654aabbccdd9021ULL);
    qtest_quit(s);
}

static void test_populated_reset_geometry_and_migration(void)
{
    QTestState *s = crime_start_populated();
    QDict *response;
    uint64_t value;

    qtest_writeq(s, CRIME_BANK(0), 0x100);
    qtest_writeq(s, CRIME_FULL_RAM, 0x9fe1a62430bb7c5dULL);
    g_assert_cmphex(qtest_readq(s, CRIME_FULL_RAM), ==,
                    0x9fe1a62430bb7c5dULL);
    qtest_system_reset(s);
    for (unsigned bank = 0; bank < 8; bank++) {
        g_assert_cmphex(qtest_readq(s, CRIME_BANK(bank)), ==, bank);
    }
    g_assert_cmpuint(memtx_readq(s, CRIME_FULL_RAM, &value), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_ERROR_STATUS) & BIT64(25),
                    ==, 0);
    response = qtest_qmp(s, "{'execute':'migrate',"
                        "'arguments':{'uri':'unix:/no-crime-migration'}}");
    g_assert_nonnull(qdict_get_qdict(response, "error"));
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(response, "error"),
                                          "desc"), "non-migratable device"));
    qobject_unref(response);
    qtest_quit(s);
}

static void test_populated_reset_low_ram_approx(void)
{
    QTestState *s = crime_start_populated_approx("");
    const uint64_t views[] = { 0, CRIME_FULL_RAM, CRIME_NO_ECC_RAM };
    uint8_t byte;

    qtest_writeq(s, CRIME_BANK(1), 0x104);
    qtest_writeb(s, CRIME_FULL_RAM + CRIME_BANK_SIZE, 0x65);
    qtest_system_reset(s);
    for (unsigned bank = 0; bank < 8; bank++) {
        g_assert_cmphex(qtest_readq(s, CRIME_BANK(bank)), ==, bank);
    }
    for (unsigned view = 0; view < ARRAY_SIZE(views); view++) {
        crime_assert_ram_view(s, views[view]);
        crime_assert_ram_view(s, views[view] + 0x01ffffff);
        qtest_writeb(s, views[view], 0x41 + view);
        qtest_writeb(s, views[view] + 0x01ffffff, 0xa1 + view);
        for (unsigned alias = 0; alias < ARRAY_SIZE(views); alias++) {
            g_assert_cmphex(qtest_readb(s, views[alias]), ==, 0x41 + view);
            g_assert_cmphex(qtest_readb(s, views[alias] + 0x01ffffff),
                            ==, 0xa1 + view);
        }
    }
    g_assert_cmpuint(memtx_readb(s, CRIME_FULL_RAM + 0x02000000, &byte),
                     !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeb(s, CRIME_FULL_RAM + 0x02000000, 0xff),
                     !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_ERROR_STATUS) &
                    (BIT64(25) | BIT64(26)), ==, 0);
    g_assert_cmpuint(memtx_readb(s, CRIME_HIGH_RAM, &byte), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_ERROR_STATUS) & BIT64(25),
                    ==, BIT64(25));
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_ERROR_ADDRESS), ==,
                    0x20000000);
    qtest_writeq(s, CRIME_BANK(1), 0x104);
    g_assert_cmphex(qtest_readb(s, CRIME_FULL_RAM + CRIME_BANK_SIZE),
                    ==, 0x65);
    qtest_quit(s);
}

static void test_populated_approx_same_value_and_reset(void)
{
    QTestState *s = crime_start_populated_approx("");
    uint8_t byte;

    g_assert_cmpuint(memtx_writeb(s, CRIME_BANK(0), 0), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_BANK(0) + 4, 0), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_BANK(0), 0x120), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_BANK(0), BIT64(40)),
                     !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_BANK(0)), ==, 0);
    qtest_writeb(s, CRIME_FULL_RAM + 0x100, 0x39);
    g_assert_cmphex(qtest_readb(s, 0x100), ==, 0x39);

    qtest_writeq(s, CRIME_BANK(0), 0);
    g_assert_cmpuint(memtx_readb(s, 0x100, &byte), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_ERROR_STATUS) & BIT64(25),
                    ==, 0);
    qtest_writeq(s, CRIME_BANK(0), 0x100);
    g_assert_cmphex(qtest_readb(s, 0x100), ==, 0x39);

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, CRIME_BANK(0)), ==, 0);
    crime_assert_ram_view(s, 0x100);
    g_assert_cmphex(qtest_readb(s, 0x100), ==, 0x39);
    qtest_quit(s);
}

static void test_populated_approx_bank_one_first(void)
{
    QTestState *s = crime_start_populated_approx("");
    uint8_t byte;

    qtest_writeb(s, CRIME_FULL_RAM + 0x100, 0x57);
    qtest_writeq(s, CRIME_BANK(1), 0x104);
    g_assert_cmphex(qtest_readq(s, CRIME_BANK(0)), ==, 0);
    g_assert_cmpuint(memtx_readb(s, CRIME_FULL_RAM + 0x100, &byte),
                     !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_ERROR_STATUS) & BIT64(25),
                    ==, 0);
    qtest_writeb(s, CRIME_FULL_RAM + CRIME_BANK_SIZE + 0x100, 0x83);
    g_assert_cmphex(qtest_readb(s, CRIME_BANK_SIZE + 0x100), ==, 0x83);
    qtest_writeq(s, CRIME_BANK(0), 0x100);
    g_assert_cmphex(qtest_readb(s, CRIME_FULL_RAM + 0x100), ==, 0x57);
    g_assert_cmphex(qtest_readb(s, CRIME_FULL_RAM + CRIME_BANK_SIZE + 0x100),
                    ==, 0x83);
    qtest_quit(s);
}

static void test_populated_approx_carrier(void)
{
    QTestState *s = crime_start_with_carrier_approx();
    uint8_t byte;

    crime_assert_ram_view(s, CRIME_CARRIER_BASE + 0x100);
    crime_assert_ram_view(s, CRIME_CARRIER_BASE + CRIME_FULL_RAM + 0x100);
    crime_assert_ram_view(s, CRIME_CARRIER_BASE + CRIME_NO_ECC_RAM + 0x100);
    qtest_writeb(s, CRIME_CARRIER_BASE + CRIME_FULL_RAM + 0x100, 0x5d);
    g_assert_cmphex(qtest_readb(s, 0x100), ==, 0x5d);
    g_assert_cmphex(qtest_readb(s, CRIME_CARRIER_BASE + 0x100), ==, 0x5d);
    g_assert_cmphex(qtest_readb(s, CRIME_CARRIER_BASE + CRIME_NO_ECC_RAM +
                                   0x100), ==, 0x5d);
    g_assert_cmpuint(memtx_readb(s, CRIME_CARRIER_BASE + CRIME_HIGH_RAM,
                                  &byte), !=, MEMTX_OK);

    qtest_writeq(s, CRIME_BANK(0), 0);
    g_assert_cmpuint(memtx_readb(s, CRIME_CARRIER_BASE + 0x100, &byte),
                     !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readb(s, CRIME_CARRIER_BASE + CRIME_FULL_RAM +
                                  0x100, &byte), !=, MEMTX_OK);
    qtest_quit(s);
}

static void test_populated_approx_transition_log(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("crime-approx-XXXXXX",
                                                &error);
    g_autofree char *path = NULL;
    g_autofree char *options = NULL;
    g_autofree char *contents = NULL;
    const char *transition;
    QTestState *s;

    g_assert_no_error(error);
    path = g_build_filename(directory, "qemu.log", NULL);
    options = g_strdup_printf("-d unimp -D %s", path);
    s = crime_start_populated_approx(options);
    g_assert_cmpuint(memtx_writeq(s, CRIME_BANK(0), 0x120), !=, MEMTX_OK);
    qtest_writeq(s, CRIME_BANK(1), 0x104);
    qtest_writeq(s, CRIME_BANK(0), 0x100);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents,
        "reset low-RAM approximation active for decoder offsets "
        "[0, 0x2000000)"));
    transition = strstr(contents,
        "reset low-RAM approximation ended by bank 1 control write "
        "at register offset 0x210 value 0x104");
    g_assert_nonnull(transition);
    g_assert_null(strstr(transition + 1,
                         "reset low-RAM approximation ended"));
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_populated_approx_requires_profile(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*requires eight 128 MiB banks*");
        return;
    }
    s = qtest_init(MEMTX_TEST_DEVICE "-M none -cpu R10000 -S "
                   "-object memory-backend-ram,id=fit,size=64M "
                   "-device sgi-crime,id=crime,interface-revision=1,"
                   "ram=/objects/fit/fit[0],reset-low-ram-approx=on");
    qtest_quit(s);
}

static void test_populated_carrier_lifecycle(void)
{
    QTestState *s = crime_start_with_carrier();
    const uint64_t low = 0x91234abcde567f08ULL;
    const uint64_t high = 0x61de2345ab7890fcULL;
    uint64_t value;

    crime_assert_carrier_properties_locked(s);
    qtest_writeq(s, CRIME_BANK(0), 0x100);
    qtest_writeq(s, CRIME_BANK(4), 0x110);
    qtest_writeq(s, CRIME_FULL_RAM + 0x100, low);
    qtest_writeq(s, CRIME_FULL_RAM + 4 * CRIME_BANK_SIZE + 0x100, high);
    g_assert_cmphex(qtest_readq(s, CRIME_CARRIER_BASE + 0x100), ==, low);
    g_assert_cmphex(qtest_readq(s, CRIME_CARRIER_BASE +
                                   CRIME_FULL_RAM + 0x100), ==, low);
    g_assert_cmphex(qtest_readq(s, CRIME_CARRIER_BASE +
                                   CRIME_NO_ECC_RAM + 0x100), ==, low);
    g_assert_cmphex(qtest_readq(s, CRIME_CARRIER_BASE +
                                   CRIME_HIGH_RAM + 0x100), ==, high);

    qtest_writeq(s, CRIME_BANK(0), 0x104);
    g_assert_cmphex(qtest_readq(s, CRIME_CARRIER_BASE + CRIME_FULL_RAM +
                                   CRIME_BANK_SIZE + 0x100), ==, low);
    /* The carrier must retain the decoder's unmatched-address handling. */
    g_assert_cmpuint(memtx_writeq(s, CRIME_CARRIER_BASE + CRIME_FULL_RAM +
                                     0x01000000, 0xbad0bad1bad2bad3ULL),
                     !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_ERROR_STATUS) & BIT64(26),
                    ==, BIT64(26));
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_ERROR_ADDRESS), ==,
                    0x01000000);

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'realized','value':false}}", CRIME_PATH);
    g_assert_cmpuint(memtx_readq(s, CRIME_CARRIER_BASE + CRIME_FULL_RAM +
                                    CRIME_BANK_SIZE + 0x100, &value),
                     !=, MEMTX_OK);
    crime_assert_carrier_properties_locked(s);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'realized','value':true}}", CRIME_PATH);
    g_assert_cmphex(qtest_readq(s, CRIME_CARRIER_BASE + CRIME_FULL_RAM +
                                   CRIME_BANK_SIZE + 0x100), ==, low);
    qtest_quit(s);
}

static bool crime_migration_reason(QTestState *s, const char *needle)
{
    g_autoptr(QDict) info =
        qtest_qmp_assert_success_ref(s, "{'execute':'query-migrate'}");
    QList *reasons = qdict_get_qlist(info, "blocked-reasons");
    QListEntry *entry;

    if (!reasons) {
        return false;
    }
    QLIST_FOREACH_ENTRY(reasons, entry) {
        QString *reason = qobject_to(QString, qlist_entry_obj(entry));

        if (strstr(qstring_get_str(reason), needle)) {
            return true;
        }
    }
    return false;
}

static void crime_assert_outgoing_unmigratable(QTestState *s)
{
    g_autoptr(QDict) reply = qtest_qmp(s,
        "{'execute':'migrate',"
        "'arguments':{'uri':'unix:/no-crime-migration'}}");
    QDict *error = qdict_get_qdict(reply, "error");
    const char *description;

    g_assert_nonnull(error);
    description = qdict_get_str(error, "desc");
    g_assert_nonnull(strstr(description, "non-migratable device"));
    g_assert_nonnull(strstr(description, "sgi-crime"));
}

static char *crime_invalid_incoming_error(QTestState *s)
{
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("crime-invalid-incoming-XXXXXX", &error);
    g_autofree char *stream = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *uri = NULL;
    const char invalid[] = "BAD!BAD!";
    int64_t deadline = g_get_monotonic_time() +
                       30 * G_TIME_SPAN_SECOND;
    char *description = NULL;

    g_assert_no_error(error);
    stream = g_build_filename(directory, "invalid.stream", NULL);
    g_assert_true(g_file_set_contents(stream, invalid, sizeof(invalid) - 1,
                                      &error));
    g_assert_no_error(error);
    quoted = g_shell_quote(stream);
    uri = g_strdup_printf("exec:cat %s", quoted);
    qtest_qmp_assert_success(s,
        "{'execute':'migrate-incoming',"
        "'arguments':{'uri':%s,'exit-on-error':false}}", uri);
    while (g_get_monotonic_time() < deadline) {
        g_autoptr(QDict) info =
            qtest_qmp_assert_success_ref(s, "{'execute':'query-migrate'}");

        if (g_str_equal(qdict_get_str(info, "status"), "failed") &&
            qdict_haskey(info, "error-desc")) {
            description = g_strdup(qdict_get_str(info, "error-desc"));
            break;
        }
        g_usleep(1000);
    }
    g_assert_nonnull(description);
    g_test_message("incoming failure: %s", description);
    g_assert_cmpint(g_unlink(stream), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    return description;
}

static void test_populated_migration_marker_lifecycle(void)
{
    QTestState *s = crime_start_populated_approx("");

    g_assert_true(crime_migration_reason(s, "non-migratable device"));
    crime_assert_outgoing_unmigratable(s);
    /* A valid same-value write ends the reset approximation. */
    qtest_writeq(s, CRIME_BANK(0), 0);
    crime_assert_outgoing_unmigratable(s);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'realized','value':false}}", CRIME_PATH);
    g_assert_false(crime_migration_reason(s, "non-migratable device"));
    g_assert_false(crime_migration_reason(s, "CRIME eight 128 MiB banks"));
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'realized','value':true}}", CRIME_PATH);
    g_assert_true(crime_migration_reason(s, "non-migratable device"));
    crime_assert_outgoing_unmigratable(s);
    qtest_quit(s);
}

static void test_populated_invalid_incoming(gconstpointer data)
{
    bool approx = GPOINTER_TO_INT(data);
    QTestState *s = crime_start_populated_options(approx, "-incoming defer");
    g_autofree char *description = crime_invalid_incoming_error(s);

    g_assert_nonnull(strstr(description, "non-migratable device"));
    g_assert_nonnull(strstr(description, "sgi-crime"));
    g_assert_null(strstr(description, "magic"));
    qtest_quit(s);
}

static void test_flat_invalid_incoming(void)
{
    QTestState *s = crime_start_with_ram("-incoming defer");
    g_autofree char *description = crime_invalid_incoming_error(s);

    g_assert_nonnull(strstr(description, "Not a migration stream, magic"));
    g_assert_null(strstr(description, "non-migratable device"));
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/crime/reset-access", test_reset_and_access);
    qtest_add_func("/sgi/crime/prom-id-word",
                   test_prom_id_word);
    qtest_add_func("/sgi/crime/timer-counter", test_timer_counter);
    qtest_add_func("/sgi/crime/timer-invalid-access",
                   test_timer_invalid_access);
    qtest_add_func("/sgi/crime/timer-no-irq", test_timer_no_irq);
    qtest_add_func("/sgi/crime/empty-renderer-status",
                   test_empty_renderer_status);
    qtest_add_func("/sgi/crime/mace-level-routing", test_mace_level_routing);
    qtest_add_func("/sgi/crime/software-interrupts", test_software_interrupts);
    qtest_add_func("/sgi/crime/reset-pending-irq", test_reset_pending_irq);
    qtest_add_func("/sgi/crime/migrate-pending-irq",
                   test_pending_irq_migration);
    qtest_add_func("/sgi/crime/migrate-timer", test_timer_migration);
    qtest_add_func("/sgi/crime/ram-aliases-holes",
                   test_ram_aliases_and_holes);
    qtest_add_func("/sgi/crime/ram-128m-aliases-holes",
                   test_ram_128m_aliases_and_holes);
    qtest_add_func("/sgi/crime/ram-alias-migration",
                   test_ram_alias_mapping_migration);
    qtest_add_func("/sgi/crime/ram-unsupported-fit",
                   test_ram_unsupported_fit);
    qtest_add_func("/sgi/crime/ram-readonly-fit",
                   test_ram_readonly_fit);
    qtest_add_func("/sgi/crime/populated-reset-controls",
                   test_populated_reset_controls);
    qtest_add_func("/sgi/crime/populated-banks-views",
                   test_populated_banks_and_views);
    qtest_add_func("/sgi/crime/populated-priority-remap",
                   test_populated_priority_and_remap);
    qtest_add_func("/sgi/crime/populated-all-zero",
                   test_populated_all_controls_at_zero);
    qtest_add_func("/sgi/crime/populated-unmatched-write",
                   test_populated_unmatched_write);
    qtest_add_func("/sgi/crime/populated-reset-geometry-migration",
                   test_populated_reset_geometry_and_migration);
    qtest_add_func("/sgi/crime/populated-reset-low-ram-approx",
                   test_populated_reset_low_ram_approx);
    qtest_add_func("/sgi/crime/populated-approx-same-value-reset",
                   test_populated_approx_same_value_and_reset);
    qtest_add_func("/sgi/crime/populated-approx-bank-one-first",
                   test_populated_approx_bank_one_first);
    qtest_add_func("/sgi/crime/populated-approx-carrier",
                   test_populated_approx_carrier);
    qtest_add_func("/sgi/crime/populated-approx-transition-log",
                   test_populated_approx_transition_log);
    qtest_add_func("/sgi/crime/populated-approx-requires-profile",
                   test_populated_approx_requires_profile);
    qtest_add_func("/sgi/crime/populated-carrier-lifecycle",
                   test_populated_carrier_lifecycle);
    qtest_add_func("/sgi/crime/populated-migration-marker-lifecycle",
                   test_populated_migration_marker_lifecycle);
    qtest_add_data_func("/sgi/crime/populated-incoming-no-approx",
                        GINT_TO_POINTER(0), test_populated_invalid_incoming);
    qtest_add_data_func("/sgi/crime/populated-incoming-approx",
                        GINT_TO_POINTER(1), test_populated_invalid_incoming);
    qtest_add_func("/sgi/crime/flat-incoming-checks-magic",
                   test_flat_invalid_incoming);
    return g_test_run();
}
