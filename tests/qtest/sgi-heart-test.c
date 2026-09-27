/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic bank populations for the standalone IP30 HEART profile. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-migrate.h"

#define HEART_PATH "/machine/peripheral/heart"
#define HEART_REGS 0x0ff00000ULL
#define HEART_ONEWIRE (HEART_REGS + 0xb8)
#define HEART_ONEWIRE_LO (HEART_REGS + 0xbc)
#define HEART_ONEWIRE_COMMAND(pulse, sample) \
    (((uint64_t)(pulse) << 10) | ((uint64_t)(sample) << 2))
#define HEART_WIDGET 0x11000000ULL
#define HEART_PACKET (3ULL << 48)
#define HEART_WIDGET_ID 0x4c00106cU
#define HEART_XTALK_PATH "/machine/peripheral/heart-xtalk"
#define HEART_XTALK_REGS 0x0f000000ULL
#define HEART_SMALL 0x10000000ULL
#define HEART_LARGE (8ULL << 36)
#define HEART_XTALK_PACKET 0x3000000000ULL
#define HEART_MISSING_PACKET 0x3000001000ULL
#define HEART_XTALK_RAM 0x4000000000ULL
#define HEART_BRIDGE 0x5000000000ULL
#define HEART_BRIDGE_PATH "/machine/peripheral/bridge"
#define HEART_BRIDGE_CARD (HEART_BRIDGE + 0xa00000)
#define HEART_RAM 0x20000000ULL
#define UNIT (32ULL * 1024 * 1024)
#define PROBE_DESC 0x81bf0040U
#define MODULO_DESC 0x817f0040U
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

static void test_onewire_empty(void)
{
    QTestState *s = heart_start(64, "");
    uint64_t reset = HEART_ONEWIRE_COMMAND(500, 65);
    uint64_t read = HEART_ONEWIRE_COMMAND(6, 13);

    g_assert_cmphex(qtest_readq(s, HEART_ONEWIRE), ==, 0);
    qtest_writeq(s, HEART_ONEWIRE, reset);
    g_assert_cmphex(qtest_readq(s, HEART_ONEWIRE) & 3, ==, 0);
    qtest_clock_step(s, 564000);
    g_assert_cmphex(qtest_readl(s, HEART_ONEWIRE_LO) & 3, ==, 0);
    qtest_clock_step(s, 1000);
    g_assert_cmphex(qtest_readq(s, HEART_ONEWIRE), ==, reset | 3);

    qtest_writel(s, HEART_ONEWIRE_LO, read);
    g_assert_cmphex(qtest_readl(s, HEART_ONEWIRE_LO) & 3, ==, 0);
    qtest_clock_step(s, 19000);
    g_assert_cmphex(qtest_readl(s, HEART_ONEWIRE_LO), ==, read | 3);

    qtest_writel(s, HEART_ONEWIRE_LO, HEART_ONEWIRE_COMMAND(80, 30));
    qtest_clock_step(s, 110000);
    g_assert_cmphex(qtest_readl(s, HEART_ONEWIRE_LO) & 3, ==, 2);
    qtest_quit(s);
}

static void test_onewire_present(void)
{
    QTestState *s = heart_start(64, "-device ds2505,bus=onewire");

    qtest_writel(s, HEART_ONEWIRE_LO, HEART_ONEWIRE_COMMAND(500, 65));
    qtest_clock_step(s, 565000);
    g_assert_cmphex(qtest_readl(s, HEART_ONEWIRE_LO) & 3, ==, 2);
    qtest_quit(s);
}

static void test_onewire_replace_reset(void)
{
    QTestState *s = heart_start(64, "");
    uint64_t reset = HEART_ONEWIRE_COMMAND(500, 65);
    uint64_t read = HEART_ONEWIRE_COMMAND(6, 13);

    qtest_writeq(s, HEART_ONEWIRE, reset);
    qtest_clock_step(s, 100000);
    qtest_writel(s, HEART_ONEWIRE_LO, read);
    qtest_clock_step(s, 19000);
    g_assert_cmphex(qtest_readl(s, HEART_ONEWIRE_LO), ==, read | 3);
    qtest_clock_step(s, 500000);
    g_assert_cmphex(qtest_readl(s, HEART_ONEWIRE_LO), ==, read | 3);

    qtest_writeq(s, HEART_ONEWIRE, reset);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, HEART_ONEWIRE), ==, 0);
    qtest_clock_step(s, 565000);
    g_assert_cmphex(qtest_readq(s, HEART_ONEWIRE), ==, 0);
    qtest_quit(s);
}

static void test_descriptors(gconstpointer data)
{
    unsigned mib = GPOINTER_TO_UINT(data);
    QTestState *s = heart_start(mib, "");
    uint64_t span = (uint64_t)mib * 1024 * 1024;
    uint64_t value;
    unsigned detected_units = 0;

    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20), ==, 0);
    qtest_writel(s, HEART_REGS + 0x20, MODULO_DESC);
    g_assert_cmphex(qtest_readl(s, HEART_REGS + 0x20), ==, MODULO_DESC);
    qtest_writeq(s, PROBE_RAM + 0x100, 0x123456789abcdef0ULL);
    for (unsigned unit = 1; unit < 64; unit++) {
        if (qtest_readq(s, PROBE_RAM + 0x100 + unit * UNIT) ==
            0x123456789abcdef0ULL) {
            detected_units = unit;
            break;
        }
    }
    g_assert_cmpuint(detected_units, ==, mib / 32);
    if (mib == 64) {
        qtest_writeq(s, PROBE_RAM + 0x100 + UNIT,
                     0x0f1e2d3c4b5a6978ULL);
        g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x100), ==,
                        0x123456789abcdef0ULL);
    }
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

static uint64_t projected_offset(unsigned mib, uint64_t offset)
{
    switch (mib) {
    case 64:
        return (offset & (UNIT - 1)) | ((offset & (2 * UNIT)) >> 1);
    case 128:
        return (offset & (UNIT - 1)) | ((offset & (2 * UNIT)) >> 1) |
               ((offset & (1ULL << 30)) >> 4);
    case 256:
        return (offset & (4 * UNIT - 1)) |
               ((offset & (8 * UNIT)) >> 1);
    default:
        g_assert_not_reached();
    }
}

static void test_projection(gconstpointer data)
{
    unsigned mib = GPOINTER_TO_UINT(data);
    QTestState *s = heart_start(mib, "");
    uint64_t capacity = (uint64_t)mib << 20;
    uint64_t discontinuity = mib == 256 ? 4 * UNIT : UNIT;
    uint64_t alias = mib == 256 ? 4 * UNIT : UNIT;
    uint64_t offsets[] = { 0, 8, 0x100, UNIT, 2 * UNIT,
                           4 * UNIT, 8 * UNIT, 32 * UNIT };
    uint64_t first = 0x1021324354657687ULL;
    uint64_t second = 0x98a9bacbdcedfe0fULL;
    uint32_t final_desc = 0x80000000U | ((capacity / UNIT - 1) << 16);

    qtest_writel(s, HEART_REGS + 0x20, PROBE_DESC);
    for (unsigned i = 0; i < G_N_ELEMENTS(offsets); i++) {
        uint64_t physical = HEART_RAM + projected_offset(mib, offsets[i]);
        uint64_t probe = PROBE_RAM + offsets[i];

        qtest_writeq(s, probe, first);
        qtest_writel(s, HEART_REGS + 0x20, final_desc);
        g_assert_cmphex(qtest_readq(s, physical), ==, first);
        qtest_writeq(s, physical, second);
        qtest_writel(s, HEART_REGS + 0x20, PROBE_DESC);
        g_assert_cmphex(qtest_readq(s, probe), ==, second);
    }
    qtest_writeq(s, PROBE_RAM, first);
    qtest_writeq(s, PROBE_RAM + alias, second);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM), ==, second);
    qtest_writeq(s, PROBE_RAM, first);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + alias), ==, first);

    qtest_writeq(s, PROBE_RAM + discontinuity - 4, first);
    g_assert_cmphex(qtest_readl(s, PROBE_RAM + discontinuity - 4), ==,
                    first >> 32);
    g_assert_cmphex(qtest_readl(s, PROBE_RAM), ==, (uint32_t)first);

    /* Only the base field changes; the alias choice must follow it. */
    qtest_writel(s, HEART_REGS + 0x20, (PROBE_DESC & ~0x1ffU) | 0x60);
    qtest_writeq(s, PROBE_RAM + 0x20 * UNIT + 0x100, first);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x20 * UNIT + alias + 0x100),
                    ==, first);
    qtest_writeq(s, PROBE_RAM + 0x20 * UNIT + alias + 0x100, second);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x20 * UNIT + 0x100), ==,
                    second);

    /* Density 5 and a short density-6 descriptor use ordinary modulo. */
    qtest_writel(s, HEART_REGS + 0x20, MODULO_DESC);
    qtest_writeq(s, PROBE_RAM + 0x100, first);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x100 + capacity), ==,
                    first);
    qtest_writel(s, HEART_REGS + 0x20,
                 0x81800040U | ((2 * capacity / UNIT - 1) << 16));
    qtest_writeq(s, PROBE_RAM + 0x100, second);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x100 + capacity), ==,
                    second);
    qtest_writel(s, HEART_REGS + 0x20, PROBE_DESC);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x100), ==, second);

    /* The older path checks adjacent 32 MiB units after density changes. */
    qtest_writel(s, HEART_REGS + 0x20, MODULO_DESC);
    qtest_writeq(s, PROBE_RAM + 0x80, first);
    qtest_writeq(s, PROBE_RAM + UNIT + 0x80, second);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x80), ==, first);

    /* The newer path checks half its provisional descriptor size. */
    {
        uint64_t provisional = mib == 128 ? 4 * UNIT : 2 * capacity;
        uint64_t half = provisional / 2;
        uint32_t descriptor = 0x81400040U |
                              ((provisional / UNIT - 1) << 16);

        qtest_writel(s, HEART_REGS + 0x20, descriptor);
        if (mib == 128) {
            qtest_writeq(s, PROBE_RAM + half + 0x80, second);
        }
        qtest_writeq(s, PROBE_RAM + 0x80, first);
        g_assert_cmphex(qtest_readq(s, PROBE_RAM + half + 0x80), ==,
                        mib == 128 ? second : first);
    }

    qtest_writel(s, HEART_REGS + 0x20, final_desc);
    for (unsigned unit = 0; unit < capacity / UNIT; unit++) {
        qtest_writeq(s, HEART_RAM + (uint64_t)unit * UNIT,
                     first + unit);
    }
    for (unsigned unit = 0; unit < capacity / UNIT; unit++) {
        g_assert_cmphex(qtest_readq(s, HEART_RAM + (uint64_t)unit * UNIT),
                        ==, first + unit);
    }
    qtest_writeq(s, HEART_RAM + capacity - 8, second);
    g_assert_cmphex(qtest_readq(s, HEART_RAM + capacity - 8), ==, second);
    qtest_quit(s);
}

static void test_two_64m_banks(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -device "
        "sgi-heart,id=heart,bank0-size=67108864,bank1-size=67108864 -S");
    uint64_t first = HEART_RAM + 0x100;
    uint64_t second = HEART_RAM + 2 * UNIT + 0x100;

    qtest_sysbus_mmio_map(s, HEART_PATH, 0, HEART_REGS);
    qtest_writel(s, HEART_REGS + 0x20, 0x80010000U);
    qtest_writel(s, HEART_REGS + 0x24, 0x80010002U);
    qtest_writeq(s, first, 0x123456789abcdef0ULL);
    qtest_writeq(s, second, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(s, first), ==, 0x123456789abcdef0ULL);
    g_assert_cmphex(qtest_readq(s, second), ==, 0xfedcba9876543210ULL);
    qtest_quit(s);
}

static void test_invalid_bank_capacity(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*capacity must be 0, 64M, 128M or 256M*");
        return;
    }
    s = qtest_init("-M none -cpu R10000 -device "
                   "sgi-heart,id=heart,bank0-size=100663296 -S");
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

static void test_widget_id(void)
{
    QTestState *s = heart_start(128, "");
    uint32_t id;
    uint16_t half;
    uint8_t byte;
    uint64_t wide;

    qtest_writel(s, HEART_REGS + 0x20, PROBE_DESC);
    qtest_writeq(s, PROBE_RAM, 0x0123456789abcdefULL);
    g_assert_cmpuint(qtest_readb_tx(s, PROBE_RAM, &byte), ==, MEMTX_OK);
    g_assert_cmphex(byte, ==, 0x01);
    g_assert_cmpuint(qtest_readw_tx(s, PROBE_RAM, &half), ==, MEMTX_OK);
    g_assert_cmphex(half, ==, 0x0123);
    g_assert_cmpuint(qtest_readl_tx(s, HEART_WIDGET + 4, &id), ==, MEMTX_OK);
    g_assert_cmphex(id, ==, HEART_WIDGET_ID);
    g_assert_cmpuint(qtest_readq_tx(s, HEART_WIDGET, &wide), ==, MEMTX_OK);
    g_assert_cmphex(wide, ==, (uint64_t)id);
    g_assert_cmpuint(qtest_readb_tx(s, HEART_WIDGET, &byte), ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_readw_tx(s, HEART_WIDGET, &half), ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_readl_tx(s, HEART_WIDGET, &id), ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_readq_tx(s, HEART_WIDGET + 8, &wide),
                     ==, MEMTX_ERROR);
    qtest_quit(s);
}

static void test_widget_control(void)
{
    QTestState *s = heart_start(128, "");
    uint32_t word;
    uint16_t half;
    uint8_t byte;
    uint64_t wide;

    g_assert_cmpuint(qtest_readl_tx(s, HEART_WIDGET + 0x24, &word), ==,
                     MEMTX_OK);
    g_assert_cmphex(word, ==, 0);
    g_assert_cmpuint(qtest_writel_tx(s, HEART_WIDGET + 0x24, 0xa5c3f00f),
                     ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x24), ==, 0xa5c3f00f);
    g_assert_cmpuint(qtest_writel_tx(s, HEART_WIDGET + 0x24, 0x5a3c0ff0),
                     ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x24), ==, 0x5a3c0ff0);
    word = qtest_readl(s, HEART_WIDGET + 0x24);
    word = (word & ~0x00ff0000U) | 0x00420000U;
    g_assert_cmpuint(qtest_writel_tx(s, HEART_WIDGET + 0x24, word), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x24), ==, word);

    g_assert_cmpuint(qtest_readb_tx(s, HEART_WIDGET + 0x24, &byte), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_writeb_tx(s, HEART_WIDGET + 0x24, 0xff), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_readw_tx(s, HEART_WIDGET + 0x24, &half), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_writew_tx(s, HEART_WIDGET + 0x24, 0xffff), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_readq_tx(s, HEART_WIDGET + 0x20, &wide), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_WIDGET + 0x20, UINT64_MAX),
                     ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_readl_tx(s, HEART_WIDGET + 0x20, &word), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_writel_tx(s, HEART_WIDGET + 0x20, UINT32_MAX),
                     ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_readl_tx(s, HEART_WIDGET + 0x28, &word), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_writel_tx(s, HEART_WIDGET + 0x28, UINT32_MAX),
                     ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x24), ==, 0x5a420ff0);

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x24), ==, 0);
    qtest_quit(s);
}

static void test_widget_packets(void)
{
    QTestState *s = heart_start(128, "");
    uint64_t level0 = 1ULL << 3;
    uint64_t level1 = 1ULL << 16;
    uint64_t level2 = 1ULL << 42;

    qtest_irq_intercept_out_named(s, HEART_PATH, "processor");
    g_assert_cmpuint(qtest_writel_tx(s, HEART_PACKET + 0x80, 0x103), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 0x140), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 0x303), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x88, 0x103), ==,
                     MEMTX_ERROR);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0);

    qtest_writeq(s, HEART_REGS + 0x10000,
                 level0 | level1 | level2);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 0x103), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, level0);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_false(qtest_get_irq(s, 1));
    g_assert_false(qtest_get_irq(s, 2));
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 0x110), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==,
                    level0 | level1);
    g_assert_true(qtest_get_irq(s, 1));
    g_assert_false(qtest_get_irq(s, 2));
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 0x12a), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10038), ==,
                    level0 | level1 | level2);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 1));
    g_assert_true(qtest_get_irq(s, 2));
    qtest_writeq(s, HEART_REGS + 0x10000, level0 | level2);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==,
                    level0 | level1 | level2);
    g_assert_false(qtest_get_irq(s, 1));
    qtest_writeq(s, HEART_REGS + 0x10000,
                 level0 | level1 | level2);
    g_assert_true(qtest_get_irq(s, 1));
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 16), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==,
                    level0 | level2);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_false(qtest_get_irq(s, 1));
    g_assert_true(qtest_get_irq(s, 2));
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 3), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, level2);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_false(qtest_get_irq(s, 1));
    g_assert_true(qtest_get_irq(s, 2));
    qtest_writeq(s, HEART_REGS + 0x10000, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, level2);
    g_assert_false(qtest_get_irq(s, 2));
    qtest_writeq(s, HEART_REGS + 0x10000, level2);
    g_assert_true(qtest_get_irq(s, 2));
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_PACKET + 0x80, 42), ==,
                     MEMTX_OK);
    g_assert_false(qtest_get_irq(s, 2));
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
    uint32_t id;

    qtest_irq_intercept_out_named(s, HEART_PATH, "processor");
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
    qtest_writeq(s, HEART_REGS + 0x10000, 1ULL << 16);
    qtest_writeq(s, HEART_REGS + 0x10020, 0xf | (1ULL << 16));
    g_assert_true(qtest_get_irq(s, 1));
    qtest_clock_step(s, 800);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20000), ==, 10);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmpuint(qtest_readl_tx(s, HEART_WIDGET + 4, &id), ==, MEMTX_OK);
    g_assert_cmphex(id, ==, HEART_WIDGET_ID);
    g_assert_cmpuint(qtest_readq_tx(s, HEART_WIDGET, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, (uint64_t)id);
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
    g_assert_false(qtest_get_irq(s, 1));
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x80), ==, 0x10);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x50000), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20000), ==, 0);
    g_assert_cmpuint(qtest_readq_tx(s, PROBE_RAM, &value), ==,
                     MEMTX_DECODE_ERROR);
    qtest_writel(s, HEART_REGS + 0x20, PROBE_DESC);
    g_assert_cmpuint(qtest_readq_tx(s, PROBE_RAM, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0xfeedface);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + UNIT), ==, 0xfeedface);
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
    qtest_irq_intercept_out_named(from, HEART_PATH, "processor");
    qtest_irq_intercept_out_named(to, HEART_PATH, "processor");
    qtest_writel(from, HEART_WIDGET + 0x24, 0x5a420ff0);
    qtest_writeq(from, HEART_REGS, 0x800);
    qtest_writeq(from, HEART_REGS + 0x08, 0x21);
    qtest_writeq(from, HEART_REGS + 0x10, 0x84000);
    qtest_writeq(from, HEART_REGS + 0x18, 0x13579bdf);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writel(from, HEART_REGS + 0x20 + 4 * i,
                     heart_descriptor(i));
    }
    qtest_writeq(from, PROBE_RAM, 0x123456789abcdef0ULL);
    qtest_writeq(from, PROBE_RAM + 2 * UNIT, 0x87654321fedcba98ULL);
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeq(from, HEART_REGS + 0x10000 + 8 * i,
                     (1ULL << i) | (i == 0 ? 1ULL << 16 : 0));
    }
    qtest_writeq(from, HEART_REGS + 0x10020, 0xf | (1ULL << 16));
    g_assert_true(qtest_get_irq(from, 1));
    qtest_clock_step(from, 800);
    qtest_writel(from, HEART_ONEWIRE_LO, HEART_ONEWIRE_COMMAND(500, 65));
    qtest_clock_step(from, 100000);
    g_assert_cmphex(qtest_readl(from, HEART_ONEWIRE_LO) & 3, ==, 0);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readl(to, HEART_ONEWIRE_LO) & 3, ==, 0);
    qtest_clock_step(to, 464999);
    g_assert_cmphex(qtest_readl(to, HEART_ONEWIRE_LO) & 3, ==, 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readl(to, HEART_ONEWIRE_LO) & 3, ==, 3);
    g_assert_cmphex(qtest_readl(to, HEART_WIDGET + 0x24), ==, 0x5a420ff0);
    g_assert_cmphex(qtest_readq(to, HEART_REGS), ==, 0x800);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x08), ==, 0x21);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10), ==, 0x84000);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x18), ==, 0x13579bdf);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readl(to, HEART_REGS + 0x20 + 4 * i), ==,
                        heart_descriptor(i));
    }
    g_assert_cmphex(qtest_readq(to, PROBE_RAM), ==, 0x123456789abcdef0ULL);
    g_assert_cmphex(qtest_readq(to, PROBE_RAM + 2 * UNIT), ==,
                    0x87654321fedcba98ULL);
    g_assert_cmphex(qtest_readq(to, PROBE_RAM + UNIT), ==,
                    0x123456789abcdef0ULL);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10000 + 8 * i), ==,
                        (1ULL << i) | (i == 0 ? 1ULL << 16 : 0));
    }
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10030), ==,
                    0xf | (1ULL << 16));
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10038), ==,
                    0x1 | (1ULL << 16));
    g_assert_true(qtest_get_irq(to, 1));
    qtest_writeq(to, HEART_REGS + 0x10000, 1);
    g_assert_false(qtest_get_irq(to, 1));
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10030) & (1ULL << 16),
                    ==, 1ULL << 16);
    qtest_writeq(to, HEART_REGS + 0x10000, 1 | (1ULL << 16));
    g_assert_true(qtest_get_irq(to, 1));
    qtest_writeq(to, HEART_REGS + 0x10028, 1ULL << 16);
    g_assert_false(qtest_get_irq(to, 1));
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10030), ==, 0xf);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x20000), ==, 7072);
    qtest_clock_step(to, 80);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x20000), ==, 7073);
    qtest_writel(to, HEART_ONEWIRE_LO, HEART_ONEWIRE_COMMAND(500, 65));
    qtest_qmp_assert_success(to, "{'execute':'system_reset'}");
    qtest_clock_step(to, 565000);
    g_assert_cmphex(qtest_readq(to, HEART_ONEWIRE), ==, 0);
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
    qtest_sysbus_mmio_map(s, HEART_XTALK_PATH, 5, HEART_XTALK_RAM);
    qtest_irq_intercept_out_named(s, HEART_XTALK_PATH, "processor");
    return s;
}

static void test_xtalk_dma(void)
{
    QTestState *s = heart_xtalk_start("");
    uint64_t boundary = HEART_RAM + 128 * 1024 * 1024ULL;
    uint64_t packet = HEART_XTALK_RAM + 128 * 1024 * 1024ULL;
    uint64_t value;

    qtest_writel(s, HEART_XTALK_REGS + 0x20, 0x80030000);
    qtest_writel(s, HEART_XTALK_REGS + 0x24, 0x80030004);
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_XTALK_RAM - 8, UINT64_MAX),
                     ==, MEMTX_DECODE_ERROR);
    g_assert_cmpuint(qtest_writeq_tx(s, packet - 4,
                                     0x1021324354657687ULL), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, boundary - 4), ==,
                    0x1021324354657687ULL);
    qtest_writeq(s, boundary - 4, 0x8877665544332211ULL);
    g_assert_cmpuint(qtest_readq_tx(s, packet - 4, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x8877665544332211ULL);

    /* One eight-byte packet must leave its valid first four bytes alone. */
    qtest_writel(s, HEART_XTALK_REGS + 0x24, 0);
    qtest_writeq(s, boundary - 8, 0xaabbccddeeff0011ULL);
    g_assert_cmpuint(qtest_writeq_tx(s, packet - 4,
                                     0xdeadbeef01234567ULL),
                     ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readq(s, boundary - 8), ==,
                    0xaabbccddeeff0011ULL);
    g_assert_cmpuint(qtest_readq_tx(s, packet - 4, &value),
                     ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_writeq_tx(s, packet - 8,
                                     0x123456789abcdef0ULL), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, boundary - 8), ==,
                    0x123456789abcdef0ULL);

    qtest_writel(s, HEART_XTALK_REGS + 0x24, 0x80030004);
    g_assert_cmpuint(qtest_writeq_tx(s, packet,
                                     0x8877665544332211ULL), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, boundary), ==,
                    0x8877665544332211ULL);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmpuint(qtest_readq_tx(s, packet, &value), ==, MEMTX_ERROR);
    qtest_writel(s, HEART_XTALK_REGS + 0x24, 0x80030004);
    g_assert_cmpuint(qtest_readq_tx(s, packet, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x8877665544332211ULL);
    qtest_quit(s);
}

static void heart_bridge_fill(QTestState *s, uint32_t address, uint32_t seed)
{
    qtest_writel(s, HEART_BRIDGE_CARD + 0x00, address);
    qtest_writel(s, HEART_BRIDGE_CARD + 0x04, 8);
    qtest_writel(s, HEART_BRIDGE_CARD + 0x08, seed);
    qtest_writel(s, HEART_BRIDGE_CARD + 0x0c, 1);
}

static void test_bridge_dma(void)
{
    QTestState *s = heart_xtalk_start(
        "-device sgi-bridge,id=bridge,board-part=123-4567-890,"
        "fabric=/machine/peripheral/heart-xtalk/xbow/"
        "sgi-xbow.widget15.ingress.packet[0],fabric-widget-shift=48 "
        "-device sgi-bridge-dma-test,bus=pci,addr=6");
    uint8_t seen[8], expected[8];
    uint64_t where = HEART_RAM + 0x1000;

    qtest_sysbus_mmio_map(s, HEART_BRIDGE_PATH, 0, HEART_BRIDGE);
    qtest_writel(s, HEART_XTALK_REGS + 0x20, 0x80030000);
    qtest_writel(s, HEART_BRIDGE + 0x020000 + 6 * 0x1000 + 0x10,
                 0x10000000);
    qtest_writel(s, HEART_BRIDGE + 0x020000 + 6 * 0x1000 + 0x04, 6);
    qtest_writel(s, HEART_BRIDGE + 0x200 + 6 * 8 + 4,
                 (1u << 19) | (1u << 12) | (0x10000000 >> 20));

    for (unsigned i = 0; i < 8; i++) {
        expected[i] = 0x40 + i;
    }
    qtest_writel(s, HEART_BRIDGE + 0x80 + 4, 8u << 20);
    heart_bridge_fill(s, 0xa0001000, 0x40);
    qtest_memread(s, where, seen, sizeof(seen));
    g_assert_cmpmem(seen, sizeof(seen), expected, sizeof(expected));

    qtest_writeq(s, where, 0x1021324354657687ULL);
    qtest_writel(s, HEART_BRIDGE_CARD + 0x14, 0xa0001000);
    qtest_writel(s, HEART_BRIDGE_CARD + 0x00, 0xa0002000);
    qtest_writel(s, HEART_BRIDGE_CARD + 0x0c, 2);
    g_assert_cmphex(qtest_readq(s, HEART_RAM + 0x2000), ==,
                    0x1021324354657687ULL);

    memset(seen, 0, sizeof(seen));
    qtest_memwrite(s, where, seen, sizeof(seen));
    qtest_writel(s, HEART_BRIDGE + 0x80 + 4, (8u << 20) | (1u << 17));
    heart_bridge_fill(s, 0x80001000, 0x50);
    for (unsigned i = 0; i < 8; i++) {
        expected[i] = 0x50 + i;
    }
    qtest_memread(s, where, seen, sizeof(seen));
    g_assert_cmpmem(seen, sizeof(seen), expected, sizeof(expected));

    memset(seen, 0, sizeof(seen));
    qtest_memwrite(s, where, seen, sizeof(seen));
    qtest_writel(s, HEART_BRIDGE + 0x80 + 4, 8u << 20);
    heart_bridge_fill(s, 0x80001000, 0x60);
    qtest_memread(s, where, seen, sizeof(seen));
    g_assert_cmpmem(seen, sizeof(seen), (uint8_t[8]) { 0 }, 8);

    qtest_writel(s, HEART_BRIDGE + 0x80 + 4, 9u << 20);
    heart_bridge_fill(s, 0xa0001000, 0x70);
    qtest_memread(s, where, seen, sizeof(seen));
    g_assert_cmpmem(seen, sizeof(seen), (uint8_t[8]) { 0 }, 8);
    qtest_quit(s);
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
    g_assert_true(qtest_get_irq(s, 2));
    g_assert_cmpuint(qtest_writeq_tx(s, HEART_XTALK_PACKET + 0x80, 3),
                     ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_XTALK_REGS + 0x10030), ==,
                    1ULL << 42);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 2));
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
    g_assert_false(qtest_get_irq(s, 2));
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
    qtest_writel(from, HEART_XTALK_REGS + 0x20, 0x80030000);
    qtest_writel(from, HEART_XTALK_REGS + 0x24, 0x80030004);
    g_assert_cmpuint(qtest_writeq_tx(from, HEART_XTALK_RAM + 4 * UNIT - 4,
                                     0x1021324354657687ULL), ==, MEMTX_OK);
    g_assert_true(qtest_get_irq(from, 0));
    g_assert_true(qtest_get_irq(from, 2));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, HEART_XTALK_REGS + 0x10030), ==, bits);
    g_assert_cmphex(qtest_readl(to, HEART_XTALK_REGS + 0x20), ==, 0x80030000);
    g_assert_cmphex(qtest_readl(to, HEART_XTALK_REGS + 0x24), ==, 0x80030004);
    g_assert_cmphex(qtest_readq(to, HEART_RAM + 4 * UNIT - 4), ==,
                    0x1021324354657687ULL);
    g_assert_cmphex(qtest_readq(to, HEART_XTALK_RAM + 4 * UNIT - 4), ==,
                    0x1021324354657687ULL);
    g_assert_true(qtest_get_irq(to, 0));
    g_assert_true(qtest_get_irq(to, 2));
    qtest_writeq(to, HEART_XTALK_PACKET + 0x80, 3);
    g_assert_false(qtest_get_irq(to, 0));
    g_assert_true(qtest_get_irq(to, 2));
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
    g_test_add_data_func("/heart/descriptor-64m", GUINT_TO_POINTER(64),
                         test_descriptors);
    g_test_add_func("/heart/two-64m-banks", test_two_64m_banks);
    g_test_add_func("/heart/invalid-bank-capacity",
                    test_invalid_bank_capacity);
    g_test_add_data_func("/heart/descriptor-128m", GUINT_TO_POINTER(128),
                         test_descriptors);
    g_test_add_data_func("/heart/descriptor-256m", GUINT_TO_POINTER(256),
                         test_descriptors);
    g_test_add_data_func("/heart/projection-64m", GUINT_TO_POINTER(64),
                         test_projection);
    g_test_add_data_func("/heart/projection-128m", GUINT_TO_POINTER(128),
                         test_projection);
    g_test_add_data_func("/heart/projection-256m", GUINT_TO_POINTER(256),
                         test_projection);
    g_test_add_func("/heart/wide-descriptor-clear",
                    test_wide_descriptor_clear);
    g_test_add_func("/heart/widget-id", test_widget_id);
    g_test_add_func("/heart/widget-control", test_widget_control);
    g_test_add_func("/heart/widget-packets", test_widget_packets);
    g_test_add_func("/heart/interrupts", test_interrupts);
    g_test_add_func("/heart/reset", test_reset);
    g_test_add_func("/heart/migration", test_migration);
    g_test_add_func("/heart/onewire-empty", test_onewire_empty);
    g_test_add_func("/heart/onewire-present", test_onewire_present);
    g_test_add_func("/heart/onewire-replace-reset", test_onewire_replace_reset);
    g_test_add_func("/heart/xtalk-routing", test_xtalk_routing);
    g_test_add_func("/heart/xtalk-dma", test_xtalk_dma);
    g_test_add_func("/heart/bridge-dma", test_bridge_dma);
    g_test_add_func("/heart/xtalk-migration", test_xtalk_migration);
    return g_test_run();
}
