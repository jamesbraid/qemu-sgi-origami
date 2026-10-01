/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Synthetic bank populations for the standalone IP30 HEART profile. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-migrate.h"

#define HEART_PATH "/machine/peripheral/heart"
#define HEART_REGS 0x0ff00000ULL
#define HEART_STATUS (HEART_REGS + 0x80)
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
    QTestState *s = qtest_initf(MEMTX_TEST_DEVICE "-M none -cpu R10000 -device "
        "sgi-heart,id=heart,bank0-size=%" PRIu64 ",widget-revision=4 -S %s",
        (uint64_t)mib * 1024 * 1024, extra);

    qtest_sysbus_mmio_map(s, HEART_PATH, 0, HEART_REGS);
    qtest_sysbus_mmio_map(s, HEART_PATH, 1, HEART_WIDGET);
    qtest_sysbus_mmio_map(s, HEART_PATH, 2, HEART_PACKET);
    return s;
}

#define HEART_ECC_ENABLE ((1ULL << 15) | 1)
#define HEART_ECC_GENERATE (1ULL << 19)
#define HEART_ECC_IRQ (1ULL << 63)
#define HEART_ECC_DOUBLE (1ULL << 20)
#define HEART_ECC_UNCORRECTABLE ((1ULL << 15) | 2)
#define HEART_ECC_FORCE (1ULL << 21)

static void test_ecc_single_bit(void)
{
    QTestState *s = heart_start(64, "");
    const uint64_t word = HEART_RAM + 0x128;
    const uint64_t pattern = 0xa5762dcf89013be4ULL;

    qtest_irq_intercept_out_named(s, HEART_PATH, "processor");
    qtest_writel(s, HEART_REGS + 0x20, 0x80010000);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
    qtest_writeq(s, word, pattern);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==,
                    word | (0x13ULL << 34));
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0xa0), ==, pattern ^ 1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, HEART_ECC_IRQ);
    qtest_writeq(s, HEART_REGS + 0x10000, HEART_ECC_IRQ);
    g_assert_false(qtest_get_irq(s, 4));
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | (1ULL << 13));
    g_assert_true(qtest_get_irq(s, 4));
    qtest_writeq(s, HEART_REGS + 0x10040, 1);
    g_assert_true(qtest_get_irq(s, 4));
    qtest_writeq(s, HEART_REGS + 0x10028, HEART_ECC_IRQ);
    g_assert_false(qtest_get_irq(s, 4));
    g_assert_cmphex(qtest_readq(s, word), ==, pattern);
    g_assert_true(qtest_get_irq(s, 4));
    qtest_quit(s);
}

static void heart_ecc_clear(QTestState *s)
{
    qtest_writeq(s, HEART_REGS + 0x10040, 1);
    qtest_writeq(s, HEART_REGS + 0x10028, HEART_ECC_IRQ);
}

static void test_ecc_words_aliases(void)
{
    QTestState *s = heart_start(64, "-global sgi-heart.bank1-size=67108864");
    const uint64_t words[] = { HEART_RAM, HEART_RAM + 0x128,
        HEART_RAM + UNIT + 0x40, HEART_RAM + 8 * UNIT + 0x880,
        HEART_RAM + 9 * UNIT + 0x10 };
    const uint64_t patterns[] = { 0, UINT64_MAX, 0x968734120bcefa55ULL,
                                 0x1234567800000000ULL, 0x2a9c61e400081f30ULL };

    /* Each physical 128 MiB window aliases a fitted 64 MiB bank. */
    qtest_writel(s, HEART_REGS + 0x20, 0x80030000);
    qtest_writel(s, HEART_REGS + 0x24, 0x80030008);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        qtest_writeq(s, words[i], patterns[i]);
    }
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        heart_ecc_clear(s);
        g_assert_cmphex(qtest_readq(s, words[i] + 2 * UNIT), ==, patterns[i]);
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==,
                        (words[i] + 2 * UNIT) | (0x13ULL << 34));
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0xa0), ==, patterns[i] ^ 1);
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 1);
    }
    /* Writing another word cannot repair any of the faults. */
    qtest_writeq(s, HEART_RAM + 0x1000, 0x1b5698a21);
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        qtest_writeq(s, words[i] + 2 * UNIT, patterns[i] ^ 0x729);
        heart_ecc_clear(s);
        g_assert_cmphex(qtest_readq(s, words[i]), ==, patterns[i] ^ 0x729);
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    }
    g_assert_cmpuint(memtx_writel(s, HEART_REGS + 0x20, 0x80010000), ==,
                     MEMTX_OK);
    qtest_quit(s);
}

static void test_ecc_controls(void)
{
    QTestState *s = heart_start(64, "");
    const uint64_t word = HEART_RAM + 0x188;
    const uint64_t pattern = 0xde37249580fab16cULL;
    const uint64_t saved_address = word | (0x13ULL << 34);

    qtest_writel(s, HEART_REGS + 0x20, 0x80010000);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    qtest_writeq(s, word, pattern);
    qtest_writeq(s, HEART_REGS, 0);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern ^ 1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==, 0);
    qtest_writeq(s, HEART_REGS, 1ULL << 15);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern);
    qtest_writeq(s, HEART_REGS + 0x10028, HEART_ECC_IRQ);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0);
    qtest_writeq(s, HEART_REGS, 0);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern ^ 1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==, saved_address);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0xa0), ==, pattern ^ 1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 1);
    qtest_writeq(s, HEART_REGS, 1ULL << 15);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0);
    qtest_writeq(s, word, pattern ^ 0x876);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
    heart_ecc_clear(s);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern ^ 0x876);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==, saved_address);
    qtest_quit(s);
}

static void test_ecc_unsupported(void)
{
    QTestState *s = heart_start(64, "");
    const uint64_t word = HEART_RAM + 0x800;
    const uint64_t pattern = 0x5e972b80c416a3dfULL;
    const uint64_t invalid[] = { HEART_ECC_GENERATE,
        HEART_ECC_GENERATE | (1ULL << 15), HEART_ECC_GENERATE | 1,
        HEART_ECC_ENABLE | HEART_ECC_GENERATE | (1ULL << 20),
        HEART_ECC_ENABLE | HEART_ECC_GENERATE | (1ULL << 21),
        HEART_ECC_ENABLE | HEART_ECC_GENERATE | (1ULL << 25) };
    uint64_t value;
    uint32_t part;

    qtest_writel(s, HEART_REGS + 0x20, 0x80010000);
    qtest_writeq(s, word - 8, 0x8231dca94180be76ULL);
    qtest_writeq(s, word, pattern);
    qtest_writeq(s, word + 8, 0xbc2697184ed305faULL);
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        qtest_writeq(s, HEART_REGS, invalid[i]);
        g_assert_cmpuint(memtx_writeq(s, word, UINT64_MAX), ==, MEMTX_ERROR);
        qtest_writeq(s, HEART_REGS, 0);
        g_assert_cmphex(qtest_readq(s, word), ==, pattern);
    }
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
    g_assert_cmpuint(memtx_writel(s, word, 0x170349a8), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, word - 4, UINT64_MAX), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, HEART_PACKET + word, UINT64_MAX), ==,
                     MEMTX_ERROR);
    qtest_writeq(s, word, pattern);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern);
    for (unsigned control = 20; control <= 25; control++) {
        if (control != 20 && control != 21 && control != 25) {
            continue;
        }
        qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | (1ULL << control));
        g_assert_cmpuint(memtx_readq(s, word, &value), ==, MEMTX_ERROR);
        if (control != 21) {
            g_assert_cmpuint(memtx_writeq(s, word, 0), ==, MEMTX_ERROR);
        }
        /* Unsupported ECC controls leave ordinary unpoisoned RAM usable. */
        if (control == 20) {
            g_assert_cmpuint(memtx_writeq(s, word + 0x40, 0x6bf09dc48),
                             ==, MEMTX_ERROR);
            g_assert_cmphex(qtest_readq(s, word + 0x40), ==, 0);
        } else {
            qtest_writeq(s, word + 0x40, 0x6bf09dc48);
            g_assert_cmphex(qtest_readq(s, word + 0x40), ==, 0x6bf09dc48);
        }
    }
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
    g_assert_cmpuint(memtx_readl(s, word, &part), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, word + 4, 0), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, word - 4, 0), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, word + 4, 0), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_readq(s, word - 4, &value), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_readq(s, HEART_PACKET + word, &value), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, HEART_PACKET + word - 4, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, HEART_REGS + 0x20, 0), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, HEART_REGS + 0x20, 0), ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readl(s, HEART_REGS + 0x20), ==, 0x80010000);
    g_assert_cmphex(qtest_readq(s, word - 8), ==, 0x8231dca94180be76ULL);
    g_assert_cmphex(qtest_readq(s, word + 8), ==, 0xbc2697184ed305faULL);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==,
                    word | (0x13ULL << 34));
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0xa0), ==, pattern ^ 1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==,
                        HEART_ECC_IRQ);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern);
    qtest_quit(s);
}


static void test_ecc_double_bit(void)
{
    QTestState *s = heart_start(64, "");
    const uint64_t words[] = { HEART_RAM, HEART_RAM + 0x128,
                              HEART_RAM + UNIT + 0x38 };
    const uint64_t patterns[] = { 0, UINT64_MAX, 0xabcdef0123456789ULL };
    uint64_t value;

    qtest_irq_intercept_out_named(s, HEART_PATH, "processor");
    qtest_writel(s, HEART_REGS + 0x20, 0x80030000);
    qtest_writeq(s, HEART_REGS, HEART_ECC_UNCORRECTABLE | HEART_ECC_DOUBLE);
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        g_assert_cmpuint(memtx_writeq(s, words[i], patterns[i]), ==,
                         MEMTX_OK);
    }
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        uint64_t alias = words[i] + 2 * UNIT;

        qtest_writeq(s, HEART_REGS, 0);
        g_assert_cmphex(qtest_readq(s, alias), ==, patterns[i] ^ 3);
        qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
        g_assert_cmpuint(memtx_readq(s, alias, &value), ==, MEMTX_ERROR);
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
        qtest_writeq(s, HEART_REGS, HEART_ECC_UNCORRECTABLE);
        g_assert_cmpuint(memtx_readq(s, alias, &value), ==, MEMTX_ERROR);
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 2);
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==,
                        HEART_ECC_IRQ);
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==,
                        alias | (0x30ULL << 34));
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0xa0), ==, patterns[i] ^ 3);
        qtest_writeq(s, HEART_REGS + 0x10000, HEART_ECC_IRQ);
        g_assert_false(qtest_get_irq(s, 4));
        qtest_writeq(s, HEART_REGS, HEART_ECC_UNCORRECTABLE | (1ULL << 13));
        g_assert_true(qtest_get_irq(s, 4));
        qtest_writeq(s, HEART_REGS + 0x10028, HEART_ECC_IRQ);
        g_assert_false(qtest_get_irq(s, 4));
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 2);
        qtest_writeq(s, HEART_REGS + 0x10040, 2);
        g_assert_cmpuint(memtx_readq(s, words[i], &value), ==, MEMTX_ERROR);
        g_assert_true(qtest_get_irq(s, 4));
        qtest_writeq(s, HEART_REGS, i & 1 ? HEART_ECC_FORCE : 0);
        g_assert_cmpuint(memtx_writeq(s, alias, patterns[i] ^ 0x123), ==,
                         MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 2);
        qtest_writeq(s, HEART_REGS + 0x10040, 2);
        qtest_writeq(s, HEART_REGS + 0x10028, HEART_ECC_IRQ);
        qtest_writeq(s, HEART_REGS, HEART_ECC_UNCORRECTABLE);
        g_assert_cmphex(qtest_readq(s, words[i]), ==, patterns[i] ^ 0x123);
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    }
    qtest_quit(s);
}

static void test_ecc_double_unsupported(void)
{
    QTestState *s = heart_start(64, "");
    const uint64_t word = HEART_RAM + 0x1000;
    const uint64_t pattern = 0x9876543210abcdefULL;
    const uint64_t invalid[] = {
        HEART_ECC_DOUBLE, HEART_ECC_DOUBLE | 2,
        HEART_ECC_DOUBLE | (1ULL << 15),
        HEART_ECC_UNCORRECTABLE | HEART_ECC_DOUBLE | HEART_ECC_GENERATE,
        HEART_ECC_UNCORRECTABLE | HEART_ECC_DOUBLE | HEART_ECC_FORCE,
        HEART_ECC_UNCORRECTABLE | HEART_ECC_DOUBLE | (1ULL << 25)
    };
    uint64_t value;
    uint32_t part;

    qtest_writel(s, HEART_REGS + 0x20, 0x80010000);
    qtest_writeq(s, word, pattern);
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        qtest_writeq(s, HEART_REGS, invalid[i]);
        g_assert_cmpuint(memtx_writeq(s, word, 0), ==, MEMTX_ERROR);
        qtest_writeq(s, HEART_REGS, 0);
        g_assert_cmphex(qtest_readq(s, word), ==, pattern);
    }
    qtest_writeq(s, HEART_REGS, HEART_ECC_UNCORRECTABLE | HEART_ECC_DOUBLE);
    g_assert_cmpuint(memtx_writel(s, word, 0), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, word - 4, 0), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, HEART_PACKET + word, 0), ==,
                     MEMTX_ERROR);
    qtest_writeq(s, word, pattern);
    qtest_writeq(s, HEART_REGS, HEART_ECC_UNCORRECTABLE);
    g_assert_cmpuint(memtx_readl(s, word, &part), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_readq(s, word - 4, &value), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, word + 4, 0), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_readq(s, HEART_PACKET + word, &value), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, HEART_REGS + 0x20, 0), ==, MEMTX_ERROR);
    qtest_writeq(s, HEART_REGS, HEART_ECC_FORCE);
    g_assert_cmpuint(memtx_readq(s, word, &value), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, word, 0), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, word + 0x100, 0), ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0);
    qtest_writeq(s, HEART_REGS, 0);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern ^ 3);
    qtest_writeq(s, HEART_REGS, HEART_ECC_FORCE);
    qtest_writeq(s, word, pattern);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
    qtest_writeq(s, word, pattern);
    qtest_writeq(s, HEART_REGS, HEART_ECC_FORCE);
    qtest_writeq(s, word, pattern ^ 0x321);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern ^ 0x321);
    qtest_quit(s);
}

static void test_ecc_mixed_classes(void)
{
    QTestState *s = heart_start(64, "");
    const uint64_t single = HEART_RAM + 0x88, double_word = HEART_RAM + 0x90;
    uint64_t value;

    qtest_writel(s, HEART_REGS + 0x20, 0x80010000);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
    qtest_writeq(s, single, 0xfedcba9876543210ULL);
    qtest_writeq(s, HEART_REGS, HEART_ECC_UNCORRECTABLE | HEART_ECC_DOUBLE);
    qtest_writeq(s, double_word, 0x12876490abcdef35ULL);
    qtest_writeq(s, HEART_REGS + 0x10020, 1ULL << 61);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
    g_assert_cmphex(qtest_readq(s, single), ==, 0xfedcba9876543210ULL);
    g_assert_cmpuint(memtx_readq(s, double_word, &value), ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==,
                    single | (0x13ULL << 34));
    qtest_writeq(s, HEART_REGS, HEART_ECC_UNCORRECTABLE);
    g_assert_cmpuint(memtx_readq(s, double_word, &value), ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 3);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==,
                    HEART_ECC_IRQ | (1ULL << 61));
    qtest_writeq(s, HEART_REGS + 0x10040, 2);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 1);
    /* Re-injection replaces the addressed class, retaining the other word. */
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
    qtest_writeq(s, double_word, 0x1234567890abcdefULL);
    qtest_writeq(s, HEART_REGS, 0);
    g_assert_cmphex(qtest_readq(s, double_word), ==, 0x1234567890abcdeeULL);
    qtest_writeq(s, HEART_REGS, HEART_ECC_UNCORRECTABLE | HEART_ECC_DOUBLE);
    qtest_writeq(s, single, 0xabcd1234987650feULL);
    qtest_writeq(s, HEART_REGS, 0);
    g_assert_cmphex(qtest_readq(s, single), ==, 0xabcd1234987650fdULL);
    qtest_writeq(s, HEART_RAM + 0x188, 0x2817);
    g_assert_cmphex(qtest_readq(s, single), ==, 0xabcd1234987650fdULL);
    qtest_quit(s);
}

static unsigned heart_ecc_region_count(QTestState *s)
{
    QDict *response = qtest_qmp(s, "{'execute':'qom-list',"
        "'arguments':{'path':%s}}", HEART_PATH);
    QList *properties = qdict_get_qlist(response, "return");
    const QListEntry *entry;
    unsigned count = 0;

    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        count += g_str_has_prefix(qdict_get_str(property, "name"), "heart.ecc");
    }
    qobject_unref(response);
    return count;
}

static void test_ecc_region_lifetime(void)
{
    QTestState *s = heart_start(64, "");
    unsigned initial = heart_ecc_region_count(s);
    unsigned retained;

    qtest_writel(s, HEART_REGS + 0x20, 0x80010000);
    for (unsigned batch = 0; batch < 2; batch++) {
        for (unsigned i = 0; i < 256; i++) {
            uint64_t word = HEART_RAM + 0x1000 + (batch * 256 + i) * 32;
            uint64_t pattern = 0x83a6271cb09d5ef4ULL ^ word;

            qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
            qtest_writeq(s, word, pattern);
            qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
            heart_ecc_clear(s);
            g_assert_cmphex(qtest_readq(s, word), ==, pattern);
            g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==,
                            word | (0x13ULL << 34));
            qtest_writeq(s, word, pattern ^ 0x721);
            heart_ecc_clear(s);
            g_assert_cmphex(qtest_readq(s, word), ==, pattern ^ 0x721);
            g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
        }
        /* Allocation must follow live coverage, not repaired addresses. */
        g_assert_cmpuint(heart_ecc_region_count(s), <=, initial + 8);
    }
    retained = heart_ecc_region_count(s);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmpuint(heart_ecc_region_count(s), <=, retained);
    qtest_quit(s);
}

static void test_ecc_reset(void)
{
    QTestState *s = heart_start(64, "");
    const uint64_t word = HEART_RAM + 0x68;
    const uint64_t pattern = 0xd986ed734fa58021ULL;

    qtest_irq_intercept_out_named(s, HEART_PATH, "processor");
    qtest_writel(s, HEART_REGS + 0x20, 0x80010000);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
    qtest_writeq(s, word, pattern);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | (1ULL << 13));
    qtest_writeq(s, HEART_REGS + 0x10000, HEART_ECC_IRQ);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern);
    g_assert_true(qtest_get_irq(s, 4));
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, HEART_REGS), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0xa0), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    g_assert_false(qtest_get_irq(s, 4));
    qtest_writel(s, HEART_REGS + 0x20, 0x80010000);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
    g_assert_cmphex(qtest_readq(s, word), ==, pattern);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    qtest_quit(s);
}

static void test_ecc_double_reset(void)
{
    QTestState *s = heart_start(64, "");
    uint64_t value;

    qtest_writel(s, HEART_REGS + 0x20, 0x80010000);
    qtest_writeq(s, HEART_REGS, HEART_ECC_UNCORRECTABLE | HEART_ECC_DOUBLE);
    qtest_writeq(s, HEART_RAM + 0x20, 0x3210aabbccdd8765ULL);
    qtest_writeq(s, HEART_REGS, HEART_ECC_UNCORRECTABLE);
    g_assert_cmpuint(memtx_readq(s, HEART_RAM + 0x20, &value), ==,
                     MEMTX_ERROR);
    qtest_system_reset(s);
    qtest_writel(s, HEART_REGS + 0x20, 0x80010000);
    qtest_writeq(s, HEART_REGS, HEART_ECC_UNCORRECTABLE);
    g_assert_cmphex(qtest_readq(s, HEART_RAM + 0x20), ==,
                    0x3210aabbccdd8765ULL);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0xa0), ==, 0);
    qtest_quit(s);
}

static void test_ecc_migration(gconstpointer data)
{
    bool double_bit = GPOINTER_TO_UINT(data) & 4;
    uint64_t enables = double_bit ? HEART_ECC_UNCORRECTABLE : HEART_ECC_ENABLE;
    uint64_t generation = double_bit ? HEART_ECC_DOUBLE : HEART_ECC_GENERATE;
    uint64_t value;
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("heart-ecc-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    const uint64_t word = HEART_RAM + 0x200;
    const uint64_t other = HEART_RAM + UNIT + 0x888;
    const uint64_t pattern = 0x391c5d78e0af246bULL;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = heart_start(64, "");
    to = heart_start(64, "-incoming defer");
    qtest_irq_intercept_out_named(to, HEART_PATH, "processor");
    if (GPOINTER_TO_UINT(data) & 3) {
        qtest_writel(to, HEART_REGS + 0x20, 0x80010000);
        qtest_writeq(to, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
        for (unsigned i = 0; i < 32; i++) {
            qtest_writeq(to, HEART_RAM + 0x1000 + i * 8, UINT64_MAX - i);
        }
        qtest_writeq(to, HEART_REGS, HEART_ECC_ENABLE);
        if (GPOINTER_TO_UINT(data) == 2) {
            qtest_system_reset(to);
        }
    }
    qtest_writel(from, HEART_REGS + 0x20, 0x80030000);
    qtest_writeq(from, HEART_REGS, enables | generation);
    qtest_writeq(from, word, pattern);
    qtest_writeq(from, other, pattern ^ 0xab879);
    if (double_bit) {
        qtest_writeq(from, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
        qtest_writeq(from, word + 8, pattern ^ 0x983);
    }
    qtest_writeq(from, HEART_REGS, enables | (1ULL << 13));
    qtest_writeq(from, HEART_REGS + 0x10000, HEART_ECC_IRQ);
    if (double_bit) {
        g_assert_cmpuint(memtx_readq(from, word, &value), ==, MEMTX_ERROR);
    } else {
        g_assert_cmphex(qtest_readq(from, word), ==, pattern);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, HEART_REGS), ==,
                    enables | (1ULL << 13));
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10040), ==,
                    double_bit ? 2 : 1);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x98), ==,
                    word | ((double_bit ? 0x30ULL : 0x13ULL) << 34));
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0xa0), ==,
                    pattern ^ (double_bit ? 3 : 1));
    g_assert_true(qtest_get_irq(to, 4));
    qtest_writeq(to, HEART_REGS + 0x10040, 3);
    qtest_writeq(to, HEART_REGS + 0x10028, HEART_ECC_IRQ);
    g_assert_false(qtest_get_irq(to, 4));
    g_assert_cmphex(qtest_readq(to, HEART_RAM + 0x1000), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10040), ==, 0);
    if (double_bit) {
        g_assert_cmpuint(memtx_readq(to, other + 2 * UNIT, &value), ==,
                         MEMTX_ERROR);
    } else {
        g_assert_cmphex(qtest_readq(to, other + 2 * UNIT), ==,
                        pattern ^ 0xab879);
    }
    g_assert_true(qtest_get_irq(to, 4));
    qtest_writeq(to, other + 2 * UNIT, 0x0192837465abcedfULL);
    qtest_writeq(to, HEART_REGS + 0x10040, 3);
    qtest_writeq(to, HEART_REGS + 0x10028, HEART_ECC_IRQ);
    g_assert_cmphex(qtest_readq(to, other), ==, 0x0192837465abcedfULL);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10040), ==, 0);
    if (double_bit) {
        g_assert_cmpuint(memtx_readq(to, word + 2 * UNIT, &value), ==,
                         MEMTX_ERROR);
        qtest_writeq(to, HEART_REGS, 0);
        g_assert_cmphex(qtest_readq(to, word), ==, pattern ^ 3);
    } else {
        g_assert_cmphex(qtest_readq(to, word + 2 * UNIT), ==, pattern);
    }
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10040), ==,
                    double_bit ? 2 : 1);
    if (double_bit) {
        qtest_writeq(to, HEART_REGS, HEART_ECC_ENABLE);
        g_assert_cmphex(qtest_readq(to, word + 8), ==, pattern ^ 0x983);
        g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10040), ==, 3);
    }
    qtest_quit(to);
    qtest_quit(from);
    if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_ecc_projection(gconstpointer data)
{
    unsigned mib = GPOINTER_TO_UINT(data);
    QTestState *s = heart_start(mib, "");
    uint64_t alias = mib == 256 ? 4 * UNIT : UNIT;
    const uint64_t pattern = 0x326019ac87de54bfULL;

    qtest_writel(s, HEART_REGS + 0x20, PROBE_DESC);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
    qtest_writeq(s, PROBE_RAM + alias + 0x100, pattern);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x100), ==, pattern);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x98), ==,
                    (PROBE_RAM + 0x100) | (0x13ULL << 34));
    qtest_writeq(s, PROBE_RAM + 0x100, pattern ^ 0x98);
    heart_ecc_clear(s);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + alias + 0x100), ==,
                    pattern ^ 0x98);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10040), ==, 0);
    qtest_quit(s);
}

static void test_ecc_guard_overlap(void)
{
    QTestState *s = heart_start(64, "");
    const uint64_t word = HEART_RAM + UNIT - 8;
    uint64_t value;

    qtest_writel(s, HEART_REGS + 0x20, 0x80010000);
    qtest_writeq(s, word - 8, 0x85bf014adc732968ULL);
    qtest_writeq(s, word + 16, 0x315c402a87bd916eULL);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE | HEART_ECC_GENERATE);
    qtest_writeq(s, word, 0x69f78da014e352bcULL);
    qtest_writeq(s, word + 8, 0x451bea907cd326f8ULL);
    qtest_writeq(s, HEART_REGS, HEART_ECC_ENABLE);
    g_assert_cmpuint(memtx_writeq(s, word - 1, UINT64_MAX), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, word + 1, UINT64_MAX), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, word + 15, UINT64_MAX), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readq(s, word + 1, &value), ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readq(s, word - 8), ==, 0x85bf014adc732968ULL);
    g_assert_cmphex(qtest_readq(s, word + 16), ==, 0x315c402a87bd916eULL);
    g_assert_cmphex(qtest_readq(s, word), ==, 0x69f78da014e352bcULL);
    g_assert_cmphex(qtest_readq(s, word + 8), ==, 0x451bea907cd326f8ULL);
    qtest_quit(s);
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
    QTestState *s = heart_start(64,
        "-device ds2505,bus=onewire,family=0x0b");
    uint8_t family = 0;

    qtest_writel(s, HEART_ONEWIRE_LO, HEART_ONEWIRE_COMMAND(500, 65));
    qtest_clock_step(s, 565000);
    g_assert_cmphex(qtest_readl(s, HEART_ONEWIRE_LO) & 3, ==, 2);

    /* Read ROM is 0x33, least significant bit first. */
    for (unsigned bit = 0; bit < 8; bit++) {
        unsigned pulse = (0x33 & (1u << bit)) ? 6 : 80;
        unsigned sample = pulse == 6 ? 110 : 30;

        qtest_writel(s, HEART_ONEWIRE_LO,
                     HEART_ONEWIRE_COMMAND(pulse, sample));
        g_assert_cmphex(qtest_readl(s, HEART_ONEWIRE_LO) & 3, ==, 0);
        qtest_clock_step(s, (pulse + sample) * 1000);
        g_assert_cmphex(qtest_readl(s, HEART_ONEWIRE_LO) & 3, ==,
                        pulse == 6 ? 3 : 2);
    }
    for (unsigned bit = 0; bit < 8; bit++) {
        qtest_writel(s, HEART_ONEWIRE_LO, HEART_ONEWIRE_COMMAND(6, 13));
        g_assert_cmphex(qtest_readl(s, HEART_ONEWIRE_LO) & 3, ==, 0);
        qtest_clock_step(s, 19000);
        family |= (qtest_readl(s, HEART_ONEWIRE_LO) & 1) << bit;
        g_assert_cmphex(qtest_readl(s, HEART_ONEWIRE_LO) & 2, ==, 2);
    }
    g_assert_cmphex(family, ==, 0x0b);
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
    g_assert_cmpuint(memtx_readq(s, 0x10000000000ULL, &value), ==,
                     MEMTX_DECODE_ERROR);
    g_assert_cmphex(value, ==, 0);
    g_assert_cmpuint(memtx_writeq(s, 0x10000000000ULL, UINT64_MAX), ==,
                     MEMTX_DECODE_ERROR);
    g_assert_cmpuint(memtx_writeq(s, PROBE_RAM + 0x100, UINT64_MAX), ==,
                     MEMTX_OK);
    g_assert_cmpuint(memtx_readq(s, PROBE_RAM + 0x100, &value), ==,
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
    QTestState *s = qtest_init(MEMTX_TEST_DEVICE "-M none -cpu R10000 -device "
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
    s = qtest_init(MEMTX_TEST_DEVICE "-M none -cpu R10000 -device "
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
        g_assert_cmpuint(memtx_readq(s, address[i], &value), ==,
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
        g_assert_cmpuint(memtx_readq(s, address[i], &value), ==,
                         i < 2 ? MEMTX_DECODE_ERROR : MEMTX_OK);
    }

    qtest_writeq(s, HEART_REGS + 0x28, 0);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(s, HEART_REGS + 0x20 + 4 * i), ==, 0);
        g_assert_cmpuint(memtx_readq(s, address[i], &value), ==,
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
    g_assert_cmpuint(memtx_readb(s, PROBE_RAM, &byte), ==, MEMTX_OK);
    g_assert_cmphex(byte, ==, 0x01);
    g_assert_cmpuint(memtx_readw(s, PROBE_RAM, &half), ==, MEMTX_OK);
    g_assert_cmphex(half, ==, 0x0123);
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 4, &id), ==, MEMTX_OK);
    g_assert_cmphex(id, ==, HEART_WIDGET_ID);
    g_assert_cmpuint(memtx_readq(s, HEART_WIDGET, &wide), ==, MEMTX_OK);
    g_assert_cmphex(wide, ==, (uint64_t)id);
    g_assert_cmpuint(memtx_readb(s, HEART_WIDGET, &byte), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_readw(s, HEART_WIDGET, &half), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET, &id), ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_readq(s, HEART_WIDGET + 8, &wide),
                     ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 4), ==, HEART_WIDGET_ID);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 4, &id), ==, MEMTX_OK);
    g_assert_cmphex(id, ==, HEART_WIDGET_ID);
    g_assert_cmpuint(memtx_readq(s, HEART_WIDGET, &wide), ==, MEMTX_OK);
    g_assert_cmphex(wide, ==, HEART_WIDGET_ID);
    qtest_quit(s);
}

static void test_widget_control(void)
{
    QTestState *s = heart_start(128, "");
    uint32_t word;
    uint16_t half;
    uint8_t byte;
    uint64_t wide;

    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 0x24, &word), ==,
                     MEMTX_OK);
    g_assert_cmphex(word, ==, 0);
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x24, 0xa5c3f00f),
                     ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x24), ==, 0xa5c3f00f);
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x24, 0x5a3c0ff0),
                     ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x24), ==, 0x5a3c0ff0);
    word = qtest_readl(s, HEART_WIDGET + 0x24);
    word = (word & ~0x00ff0000U) | 0x00420000U;
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x24, word), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x24), ==, word);

    g_assert_cmpuint(memtx_readb(s, HEART_WIDGET + 0x24, &byte), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeb(s, HEART_WIDGET + 0x24, 0xff), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readw(s, HEART_WIDGET + 0x24, &half), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writew(s, HEART_WIDGET + 0x24, 0xffff), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readq(s, HEART_WIDGET + 0x20, &wide), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, HEART_WIDGET + 0x20, UINT64_MAX),
                     ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 0x20, &word), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x20, UINT32_MAX),
                     ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 0x28, &word), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x28, UINT32_MAX),
                     ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x24), ==, 0x5a420ff0);

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x24), ==, 0);
    qtest_quit(s);
}

static void test_widget_timeout(void)
{
    QTestState *s = heart_start(128, "");
    uint64_t wide;
    uint32_t word;
    uint16_t half;
    uint8_t byte;

    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 0x2c, &word), ==,
                     MEMTX_OK);
    g_assert_cmphex(word, ==, 0);
    g_assert_cmpuint(memtx_readq(s, HEART_WIDGET + 0x28, &wide), ==,
                     MEMTX_OK);
    g_assert_cmphex(wide, ==, 0);
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 0x44, &word), ==,
                     MEMTX_OK);
    g_assert_cmphex(word, ==, 0);
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x2c, 0x80), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x2c), ==, 0x80);
    g_assert_cmphex(qtest_readq(s, HEART_WIDGET + 0x28), ==, 0x80);
    qtest_writel(s, HEART_WIDGET + 0x2c, 0x12345678);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x2c), ==, 0x12345678);
    g_assert_cmphex(qtest_readq(s, HEART_WIDGET + 0x28), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(s, HEART_REGS + 0x2c), ==, 0);
    g_assert_cmpuint(memtx_writeq(s, HEART_WIDGET + 0x28, 0), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_WIDGET + 0x28), ==, 0);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x2c), ==, 0);
    g_assert_cmpuint(memtx_writeq(s, HEART_WIDGET + 0x28,
                                    0x1234567887654321ULL), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_WIDGET + 0x28), ==, 0x87654321);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x2c), ==, 0x87654321);
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 0x28, &word), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x30, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readb(s, HEART_WIDGET + 0x2c, &byte), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeb(s, HEART_WIDGET + 0x2c, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readw(s, HEART_WIDGET + 0x2c, &half), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writew(s, HEART_WIDGET + 0x2c, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readq(s, HEART_WIDGET + 0x40, &wide), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readb(s, HEART_WIDGET + 0x44, &byte), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readw(s, HEART_WIDGET + 0x44, &half), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeb(s, HEART_WIDGET + 0x44, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writew(s, HEART_WIDGET + 0x44, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 0x48, &word), ==,
                     MEMTX_ERROR);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x2c), ==, 0x87654321);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x44), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x2c), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_WIDGET + 0x28), ==, 0);
    g_assert_cmphex(qtest_readl(s, HEART_WIDGET + 0x44), ==, 0);
    qtest_quit(s);
}

static void test_widget_errors(void)
{
    QTestState *s = heart_start(128, "");
    uint64_t wide;
    uint32_t word;
    uint16_t half;
    uint8_t byte;

    g_assert_cmpuint(memtx_readq(s, HEART_WIDGET + 0x60, &wide), ==,
                     MEMTX_OK);
    g_assert_cmphex(wide, ==, 0);
    g_assert_cmpuint(memtx_readq(s, HEART_WIDGET + 0x68, &wide), ==,
                     MEMTX_OK);
    g_assert_cmphex(wide, ==, 0);
    g_assert_cmpuint(memtx_writeq(s, HEART_WIDGET + 0x60, 0), ==,
                     MEMTX_OK);
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x44, 0), ==,
                     MEMTX_OK);
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 0x44, &word), ==,
                     MEMTX_OK);
    g_assert_cmphex(word, ==, 0);
    g_assert_cmpuint(memtx_writeq(s, HEART_WIDGET + 0x40, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x40, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x48, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x44, 1), ==,
                     MEMTX_ERROR);

    qtest_writeq(s, HEART_WIDGET + 0x68, 0xdeadbeef3ff62c07ULL);
    g_assert_cmphex(qtest_readq(s, HEART_WIDGET + 0x68), ==,
                    0x3ff62c07ULL);
    g_assert_cmpuint(memtx_readb(s, HEART_WIDGET + 0x68, &byte), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeb(s, HEART_WIDGET + 0x68, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readw(s, HEART_WIDGET + 0x68, &half), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writew(s, HEART_WIDGET + 0x68, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 0x68, &word), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x6c, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readq(s, HEART_WIDGET + 0x70, &wide), ==,
                     MEMTX_OK);
    g_assert_cmphex(wide, ==, 0);
    g_assert_cmpuint(memtx_writeq(s, HEART_WIDGET + 0x70, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 0x60, &word), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writel(s, HEART_WIDGET + 0x64, 0), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, HEART_WIDGET + 0x60,
                                    0xdeadbeef3ff62c07ULL), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_WIDGET + 0x60), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_WIDGET + 0x68), ==,
                    0x3ff62c07ULL);

    qtest_writeq(s, HEART_WIDGET + 0x68, 0x3ff7fc0fULL);
    g_assert_cmphex(qtest_readq(s, HEART_WIDGET + 0x68), ==,
                    0x3ff7fc0fULL);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x68), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, HEART_WIDGET + 0x60), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_WIDGET + 0x68), ==, 0);
    qtest_quit(s);
}

static void test_widget_diagnostics(void)
{
    static const uint64_t word_offsets[] = { 0x14, 0x1c };
    static const uint64_t wide_offsets[] = { 0x70, 0x78, 0x90 };
    QTestState *s = heart_start(128, "");
    uint64_t wide;
    uint32_t word;
    uint16_t half;
    uint8_t byte;

    qtest_writel(s, HEART_WIDGET + 0x2c, 0x12345678);
    qtest_writeq(s, HEART_WIDGET + 0x68, 0x3ff62c07);
    for (unsigned i = 0; i < G_N_ELEMENTS(word_offsets); i++) {
        uint64_t addr = HEART_WIDGET + word_offsets[i];

        g_assert_cmpuint(memtx_readl(s, addr, &word), ==, MEMTX_OK);
        g_assert_cmphex(word, ==, 0);
        g_assert_cmpuint(memtx_readb(s, addr, &byte), ==, MEMTX_ERROR);
        g_assert_cmpuint(memtx_readw(s, addr, &half), ==, MEMTX_ERROR);
        g_assert_cmpuint(memtx_readq(s, addr, &wide), ==, MEMTX_ERROR);
        g_assert_cmpuint(memtx_writel(s, addr, 0), ==, MEMTX_ERROR);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(wide_offsets); i++) {
        uint64_t addr = HEART_WIDGET + wide_offsets[i];

        g_assert_cmpuint(memtx_readq(s, addr, &wide), ==, MEMTX_OK);
        g_assert_cmphex(wide, ==, 0);
        g_assert_cmpuint(memtx_readb(s, addr, &byte), ==, MEMTX_ERROR);
        g_assert_cmpuint(memtx_readw(s, addr, &half), ==, MEMTX_ERROR);
        g_assert_cmpuint(memtx_readl(s, addr, &word), ==, MEMTX_ERROR);
        g_assert_cmpuint(memtx_readl(s, addr + 4, &word), ==,
                         MEMTX_ERROR);
        g_assert_cmpuint(memtx_writeq(s, addr, 0), ==, MEMTX_ERROR);
    }
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 0x18, &word), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readq(s, HEART_WIDGET + 0x88, &wide), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_readq(s, HEART_WIDGET + 0x98, &wide), ==,
                     MEMTX_ERROR);

    qtest_system_reset(s);
    for (unsigned i = 0; i < G_N_ELEMENTS(word_offsets); i++) {
        g_assert_cmphex(qtest_readl(s, HEART_WIDGET + word_offsets[i]), ==, 0);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(wide_offsets); i++) {
        g_assert_cmphex(qtest_readq(s, HEART_WIDGET + wide_offsets[i]), ==, 0);
    }
    qtest_quit(s);
}

static void test_widget_packets(void)
{
    QTestState *s = heart_start(128, "");
    uint64_t level0 = 1ULL << 3;
    uint64_t level1 = 1ULL << 16;
    uint64_t level2 = 1ULL << 42;

    qtest_irq_intercept_out_named(s, HEART_PATH, "processor");
    g_assert_cmpuint(memtx_writel(s, HEART_PACKET + 0x80, 0x103), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, HEART_PACKET + 0x80, 0x140), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, HEART_PACKET + 0x80, 0x303), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, HEART_PACKET + 0x88, 0x103), ==,
                     MEMTX_ERROR);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0);

    qtest_writeq(s, HEART_REGS + 0x10000,
                 level0 | level1 | level2);
    g_assert_cmpuint(memtx_writeq(s, HEART_PACKET + 0x80, 0x103), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, level0);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_false(qtest_get_irq(s, 1));
    g_assert_false(qtest_get_irq(s, 2));
    g_assert_cmpuint(memtx_writeq(s, HEART_PACKET + 0x80, 0x110), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==,
                    level0 | level1);
    g_assert_true(qtest_get_irq(s, 1));
    g_assert_false(qtest_get_irq(s, 2));
    g_assert_cmpuint(memtx_writeq(s, HEART_PACKET + 0x80, 0x12a), ==,
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
    g_assert_cmpuint(memtx_writeq(s, HEART_PACKET + 0x80, 16), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==,
                    level0 | level2);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_false(qtest_get_irq(s, 1));
    g_assert_true(qtest_get_irq(s, 2));
    g_assert_cmpuint(memtx_writeq(s, HEART_PACKET + 0x80, 3), ==,
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
    g_assert_cmpuint(memtx_writeq(s, HEART_PACKET + 0x80, 42), ==,
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

static void test_high_interrupt_levels(void)
{
    QTestState *s = heart_start(128, "");
    uint64_t timer = 1ULL << 50;
    uint64_t error = 1ULL << 51;

    qtest_irq_intercept_out_named(s, HEART_PATH, "processor");
    for (unsigned cpu = 1; cpu < 4; cpu++) {
        qtest_writeq(s, HEART_REGS + 0x10000 + cpu * 8, timer | error);
        g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10000 + cpu * 8), ==,
                        timer | error);
    }
    qtest_writeq(s, HEART_REGS + 0x10020, timer | error);
    g_assert_false(qtest_get_irq(s, 3));
    g_assert_false(qtest_get_irq(s, 4));
    qtest_writeq(s, HEART_REGS + 0x10000, timer | error);
    g_assert_true(qtest_get_irq(s, 3));
    g_assert_true(qtest_get_irq(s, 4));
    qtest_writeq(s, HEART_REGS + 0x10000, error);
    g_assert_false(qtest_get_irq(s, 3));
    g_assert_true(qtest_get_irq(s, 4));
    qtest_writeq(s, HEART_REGS + 0x10028, error);
    g_assert_false(qtest_get_irq(s, 4));
    qtest_writeq(s, HEART_REGS + 0x10000, timer);
    g_assert_true(qtest_get_irq(s, 3));
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_false(qtest_get_irq(s, 3));
    g_assert_false(qtest_get_irq(s, 4));
    qtest_quit(s);
}

static void test_processor_status(void)
{
    QTestState *s = heart_start(128, "");
    uint64_t status = qtest_readq(s, HEART_STATUS);

    g_assert_cmphex(status & 0xf, ==, 8);
    g_assert_cmphex(status & 0x10, ==, 0x10);
    g_assert_cmphex(status, ==, 0x18);
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
    g_assert_cmpuint(memtx_readl(s, HEART_WIDGET + 4, &id), ==, MEMTX_OK);
    g_assert_cmphex(id, ==, HEART_WIDGET_ID);
    g_assert_cmpuint(memtx_readq(s, HEART_WIDGET, &value), ==, MEMTX_OK);
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
    g_assert_cmphex(qtest_readq(s, HEART_STATUS), ==, 0x18);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x50000), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20000), ==, 0);
    g_assert_cmpuint(memtx_readq(s, PROBE_RAM, &value), ==,
                     MEMTX_DECODE_ERROR);
    qtest_writel(s, HEART_REGS + 0x20, PROBE_DESC);
    g_assert_cmpuint(memtx_readq(s, PROBE_RAM, &value), ==, MEMTX_OK);
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
    qtest_writel(from, HEART_WIDGET + 0x2c, 0x12345678);
    qtest_writeq(from, HEART_WIDGET + 0x68, 0x3ff62c07);
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
    g_assert_cmphex(qtest_readq(to, HEART_STATUS), ==, 0x18);
    g_assert_cmphex(qtest_readl(to, HEART_ONEWIRE_LO) & 3, ==, 0);
    qtest_clock_step(to, 464999);
    g_assert_cmphex(qtest_readl(to, HEART_ONEWIRE_LO) & 3, ==, 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readl(to, HEART_ONEWIRE_LO) & 3, ==, 3);
    g_assert_cmphex(qtest_readl(to, HEART_WIDGET + 0x24), ==, 0x5a420ff0);
    g_assert_cmphex(qtest_readl(to, HEART_WIDGET + 0x2c), ==, 0x12345678);
    g_assert_cmphex(qtest_readq(to, HEART_WIDGET + 0x28), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(to, HEART_WIDGET + 0x44), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART_WIDGET + 0x68), ==, 0x3ff62c07);
    g_assert_cmphex(qtest_readl(to, HEART_WIDGET + 0x14), ==, 0);
    g_assert_cmphex(qtest_readl(to, HEART_WIDGET + 0x1c), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART_WIDGET + 0x70), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART_WIDGET + 0x78), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART_WIDGET + 0x90), ==, 0);
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
    QTestState *s = qtest_initf(MEMTX_TEST_DEVICE "-M none -cpu R10000 -S "
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
    g_assert_cmpuint(memtx_writeq(s, HEART_XTALK_RAM - 8, UINT64_MAX),
                     ==, MEMTX_DECODE_ERROR);
    g_assert_cmpuint(memtx_writeq(s, packet - 4,
                                     0x1021324354657687ULL), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, boundary - 4), ==,
                    0x1021324354657687ULL);
    qtest_writeq(s, boundary - 4, 0x8877665544332211ULL);
    g_assert_cmpuint(memtx_readq(s, packet - 4, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x8877665544332211ULL);

    /* One eight-byte packet must leave its valid first four bytes alone. */
    qtest_writel(s, HEART_XTALK_REGS + 0x24, 0);
    qtest_writeq(s, boundary - 8, 0xaabbccddeeff0011ULL);
    g_assert_cmpuint(memtx_writeq(s, packet - 4,
                                     0xdeadbeef01234567ULL),
                     ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readq(s, boundary - 8), ==,
                    0xaabbccddeeff0011ULL);
    g_assert_cmpuint(memtx_readq(s, packet - 4, &value),
                     ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, packet - 8,
                                     0x123456789abcdef0ULL), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, boundary - 8), ==,
                    0x123456789abcdef0ULL);

    qtest_writel(s, HEART_XTALK_REGS + 0x24, 0x80030004);
    g_assert_cmpuint(memtx_writeq(s, packet,
                                     0x8877665544332211ULL), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, boundary), ==,
                    0x8877665544332211ULL);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmpuint(memtx_readq(s, packet, &value), ==, MEMTX_ERROR);
    qtest_writel(s, HEART_XTALK_REGS + 0x24, 0x80030004);
    g_assert_cmpuint(memtx_readq(s, packet, &value), ==, MEMTX_OK);
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
        "sgi-xbow.widget15.ingress.packet[0] "
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

    g_assert_cmpuint(memtx_readl(s, HEART_SMALL + (8 << 24) + 4, &id),
                     ==, MEMTX_OK);
    g_assert_cmphex(id, ==, HEART_WIDGET_ID);
    g_assert_cmpuint(memtx_readl(s, HEART_LARGE + 4, &id),
                     ==, MEMTX_OK);
    g_assert_cmphex(id, ==, HEART_WIDGET_ID);
    g_assert_cmpuint(memtx_readl(s, HEART_SMALL + (8 << 24), &id),
                     ==, MEMTX_ERROR);
    /* Crossbow drops requests to a legal widget without a reachable link. */
    g_assert_cmpuint(memtx_writeq(s, HEART_MISSING_PACKET + 0x80, 0x103),
                     ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readq(s, HEART_XTALK_REGS + 0x10030), ==, 0);
    qtest_writeq(s, HEART_XTALK_REGS + 0x10000, bits);
    g_assert_cmpuint(memtx_writeq(s, HEART_XTALK_PACKET + 0x80, 0x103),
                     ==, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, HEART_XTALK_PACKET + 0x80, 0x12a),
                     ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_XTALK_REGS + 0x10030), ==, bits);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 2));
    g_assert_cmpuint(memtx_writeq(s, HEART_XTALK_PACKET + 0x80, 3),
                     ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, HEART_XTALK_REGS + 0x10030), ==,
                    1ULL << 42);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 2));
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':"
        "'/machine/peripheral/heart-xtalk/xbow',"
        "'property':'links-alive','value':32768}}");
    g_assert_cmpuint(memtx_readl(s, HEART_SMALL + (8 << 24) + 4, &id),
                     ==, MEMTX_ERROR);
    g_assert_cmpuint(memtx_writeq(s, HEART_XTALK_PACKET + 0x80, 0x103),
                     ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readq(s, HEART_XTALK_REGS + 0x10030), ==,
                    1ULL << 42);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmpuint(memtx_readl(s, HEART_SMALL + (8 << 24) + 4, &id),
                     ==, MEMTX_OK);
    g_assert_cmphex(id, ==, HEART_WIDGET_ID);
    g_assert_cmphex(qtest_readq(s, HEART_XTALK_REGS + 0x10030), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_false(qtest_get_irq(s, 2));
    g_assert_cmpuint(memtx_readq(s, HEART_MISSING_PACKET + 0x80, &value),
                     ==, MEMTX_ERROR);
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
    g_assert_cmpuint(memtx_writeq(from, HEART_XTALK_RAM + 4 * UNIT - 4,
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
    qtest_add_func("/heart/ecc/single-bit", test_ecc_single_bit);
    qtest_add_func("/heart/ecc/double-bit", test_ecc_double_bit);
    qtest_add_func("/heart/ecc/mixed-classes", test_ecc_mixed_classes);
    qtest_add_func("/heart/ecc/double-unsupported",
                   test_ecc_double_unsupported);
    qtest_add_func("/heart/ecc/words-aliases", test_ecc_words_aliases);
    qtest_add_func("/heart/ecc/controls", test_ecc_controls);
    qtest_add_func("/heart/ecc/unsupported", test_ecc_unsupported);
    qtest_add_func("/heart/ecc/reset", test_ecc_reset);
    qtest_add_func("/heart/ecc/double-reset", test_ecc_double_reset);
    qtest_add_data_func("/heart/ecc/double-migration", GUINT_TO_POINTER(4),
                        test_ecc_migration);
    qtest_add_data_func("/heart/ecc/double-migration-replace",
                        GUINT_TO_POINTER(5), test_ecc_migration);
    qtest_add_func("/heart/ecc/region-lifetime", test_ecc_region_lifetime);
    qtest_add_data_func("/heart/ecc/migration", GUINT_TO_POINTER(0),
                        test_ecc_migration);
    qtest_add_data_func("/heart/ecc/migration-populated", GUINT_TO_POINTER(1),
                        test_ecc_migration);
    qtest_add_data_func("/heart/ecc/migration-populated-reset",
                        GUINT_TO_POINTER(2), test_ecc_migration);
    qtest_add_data_func("/heart/ecc/projection-64m", GUINT_TO_POINTER(64),
                        test_ecc_projection);
    qtest_add_data_func("/heart/ecc/projection-128m", GUINT_TO_POINTER(128),
                        test_ecc_projection);
    qtest_add_data_func("/heart/ecc/projection-256m", GUINT_TO_POINTER(256),
                        test_ecc_projection);
    qtest_add_func("/heart/ecc/guard-overlap", test_ecc_guard_overlap);
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
    g_test_add_func("/heart/widget-timeout", test_widget_timeout);
    g_test_add_func("/heart/widget-errors", test_widget_errors);
    g_test_add_func("/heart/widget-diagnostics", test_widget_diagnostics);
    g_test_add_func("/heart/widget-packets", test_widget_packets);
    g_test_add_func("/heart/interrupts", test_interrupts);
    g_test_add_func("/heart/high-interrupt-levels",
                    test_high_interrupt_levels);
    g_test_add_func("/heart/processor-status", test_processor_status);
    g_test_add_func("/heart/reset", test_reset);
    g_test_add_func("/heart/migration", test_migration);
    g_test_add_func("/heart/onewire-empty", test_onewire_empty);
    g_test_add_func("/heart/onewire-present", test_onewire_present);
    g_test_add_func("/heart/onewire-replace-reset", test_onewire_replace_reset);
    g_test_add_func("/heart/xtalk-routing", test_xtalk_routing);
    g_test_add_func("/heart/xtalk-dma", test_xtalk_dma);
    qtest_add_func("/heart/bridge-dma", test_bridge_dma);
    g_test_add_func("/heart/xtalk-migration", test_xtalk_migration);
    return g_test_run();
}
