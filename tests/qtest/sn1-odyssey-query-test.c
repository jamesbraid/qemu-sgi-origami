/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "exec/memattrs.h"
#include "qemu/bswap.h"
#include "hw/misc/sgi-dma-word-test.h"
#include "hw/display/sgi/head-ops.h"

static void test_word_mapping(const void *opaque)
{
    unsigned window = GPOINTER_TO_UINT(opaque);
    uint64_t target = SGI_DMA_WORD_TEST_DEVICE_BASE +
                      window * SGI_DMA_WORD_TEST_STRIDE +
                      SGI_DMA_WORD_TEST_COUNTER;
    uint8_t before[0x2000], after[0x2000];
    const uint8_t value[] = { 0x91, 0xa2, 0xb3, 0xc4 };
    QTestState *qts = qtest_init("-M none -cpu R14000 "
                                "-device sgi-dma-word-test");
    unsigned i;

    for (i = 0; i < sizeof(before); i++) {
        before[i] = (i * 53 + 17) & 0xff;
    }
    qtest_memwrite(qts, SGI_DMA_WORD_TEST_BACKING, before, sizeof(before));
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL + SGI_DMA_WORD_TEST_ADDR_HIGH,
                 target >> 32);
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL + SGI_DMA_WORD_TEST_ADDR_LOW,
                 target);
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL + SGI_DMA_WORD_TEST_VALUE,
                 0x91a2b3c4);
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL + SGI_DMA_WORD_TEST_GO, 1);
    qtest_memread(qts, SGI_DMA_WORD_TEST_BACKING, after, sizeof(after));
    if (window == SGI_DMA_WORD_TEST_WRITABLE) {
        memcpy(before + SGI_DMA_WORD_TEST_COUNTER, value, sizeof(value));
        g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                    SGI_DMA_WORD_TEST_RESULT), ==, 0);
    } else {
        g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                    SGI_DMA_WORD_TEST_RESULT),
                         ==, SGI_FAIL_BUS);
    }
    g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_MMIO_READS), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_MMIO_WRITES), ==, 0);
    if (window == SGI_DMA_WORD_TEST_MMIO) {
        uint64_t mmio = SGI_DMA_WORD_TEST_MEMORY +
                        window * SGI_DMA_WORD_TEST_STRIDE +
                        SGI_DMA_WORD_TEST_COUNTER;
        uint8_t bytes[4];

        /* Demonstrate that normal transactions reach the rejected target. */
        qtest_memread(qts, mmio, bytes, sizeof(bytes));
        qtest_memwrite(qts, mmio, value, sizeof(value));
        g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                    SGI_DMA_WORD_TEST_MMIO_READS), ==, 1);
        g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                    SGI_DMA_WORD_TEST_MMIO_WRITES), ==, 1);
        qtest_memread(qts, SGI_DMA_WORD_TEST_BACKING, after, sizeof(after));
        g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    }
    qtest_quit(qts);
}

static void test_ordinary_store(const void *opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned window = (mode & 1) ? SGI_DMA_WORD_TEST_READONLY :
                                  SGI_DMA_WORD_TEST_WRITABLE;
    bool backing_readonly = mode & 2;
    bool halfword = mode & 4;
    uint64_t target = SGI_DMA_WORD_TEST_DEVICE_BASE +
                      window * SGI_DMA_WORD_TEST_STRIDE +
                      SGI_DMA_WORD_TEST_COUNTER;
    const uint8_t seed[] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t after[4];
    QTestState *qts = qtest_init("-M none -cpu R14000 "
                                "-device sgi-dma-word-test");

    qtest_memwrite(qts, SGI_DMA_WORD_TEST_MEMORY + SGI_DMA_WORD_TEST_COUNTER,
                   seed, sizeof(seed));
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL +
                 SGI_DMA_WORD_TEST_BACKING_READONLY, backing_readonly);
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL + SGI_DMA_WORD_TEST_ADDR_LOW,
                 target);
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL + SGI_DMA_WORD_TEST_VALUE,
                 0x91a2b3c4);
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL + SGI_DMA_WORD_TEST_GO,
                 halfword ? 3 : 2);
    qtest_memread(qts, SGI_DMA_WORD_TEST_MEMORY + SGI_DMA_WORD_TEST_COUNTER,
                  after, sizeof(after));
    if (backing_readonly) {
        g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                    SGI_DMA_WORD_TEST_RESULT), ==,
                         MEMTX_DECODE_ERROR);
        g_assert_cmpmem(after, sizeof(after), seed, sizeof(seed));
    } else {
        const uint8_t word[] = { 0x91, 0xa2, 0xb3, 0xc4 };
        const uint8_t half[] = { 0xb3, 0xc4, 0x33, 0x44 };
        const uint8_t *expected = halfword ? half : word;

        /* Typed stores resolve the backing, bypassing alias permissions. */
        g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                    SGI_DMA_WORD_TEST_RESULT), ==, MEMTX_OK);
        g_assert_cmpmem(after, sizeof(after), expected, sizeof(after));
    }
    qtest_quit(qts);
}

static void test_half_publication(const void *opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    static const unsigned windows[] = {
        SGI_DMA_WORD_TEST_WRITABLE, SGI_DMA_WORD_TEST_READONLY,
        SGI_DMA_WORD_TEST_HALF_SPLIT_READONLY, SGI_DMA_WORD_TEST_HALF_SHORT,
        SGI_DMA_WORD_TEST_HALF_SPLIT_BACKING, SGI_DMA_WORD_TEST_UNALIGNED_HOST,
        SGI_DMA_WORD_TEST_MMIO,
    };
    uint8_t before[0x2000], after[0x2000];
    uint64_t target = SGI_DMA_WORD_TEST_DEVICE_BASE +
                      SGI_DMA_WORD_TEST_COUNTER;
    uint64_t composed = SGI_DMA_WORD_TEST_COUNTER;
    unsigned expected = mode == 0 ? 0 : SGI_FAIL_BUS;
    QTestState *qts = qtest_init("-M none -cpu R14000 "
                                "-device sgi-dma-word-test");
    unsigned i;

    if (mode < ARRAY_SIZE(windows)) {
        target += windows[mode] * SGI_DMA_WORD_TEST_STRIDE;
    } else if (mode == 7) {
        target++;
        expected = SGI_FAIL_BOUNDS;
    } else if (mode == 8) {
        target = UINT64_MAX - 1;
        expected = SGI_FAIL_BOUNDS;
    } else if (mode == 9 || mode == 10) {
        composed = mode == 9 ? composed + 1 : UINT64_MAX - 1;
        expected = SGI_FAIL_BOUNDS;
        qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL +
                     SGI_DMA_WORD_TEST_COMPOSE_OVERRIDE, 1);
    } else {
        target = SGI_DMA_WORD_TEST_DEVICE_BASE - 2;
    }
    for (i = 0; i < sizeof(before); i++) {
        before[i] = (i * 53 + 17) & 0xff;
    }
    qtest_memwrite(qts, SGI_DMA_WORD_TEST_BACKING, before, sizeof(before));
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL +
                 SGI_DMA_WORD_TEST_COMPOSE_HIGH,
                 composed >> 32);
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL + SGI_DMA_WORD_TEST_COMPOSE_LOW,
                 composed);
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL + SGI_DMA_WORD_TEST_ADDR_HIGH,
                 target >> 32);
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL + SGI_DMA_WORD_TEST_ADDR_LOW,
                 target);
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL + SGI_DMA_WORD_TEST_VALUE,
                 0xa1b2);
    qtest_writel(qts, SGI_DMA_WORD_TEST_CONTROL + SGI_DMA_WORD_TEST_GO, 4);
    qtest_memread(qts, SGI_DMA_WORD_TEST_BACKING, after, sizeof(after));
    if (mode == 0) {
        before[SGI_DMA_WORD_TEST_COUNTER] = 0xa1;
        before[SGI_DMA_WORD_TEST_COUNTER + 1] = 0xb2;
    }
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_RESULT), ==, expected);
    g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_MMIO_READS), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_MMIO_WRITES), ==, 0);
    qtest_quit(qts);
}

static QTestState *query_test_init(void)
{
    QTestState *qts = qtest_init("-M none -cpu R14000 "
                                "-device sgi-dma-word-test,native=on");

    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x4004, 0);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x400c,
                 SGI_DMA_WORD_TEST_MEMORY);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x4034, 0);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x403c,
                 SGI_DMA_WORD_TEST_MEMORY + 0x800);
    return qts;
}

static void query_words(QTestState *qts, const uint32_t *words, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x100000, words[i]);
    }
}

static void query_seed(QTestState *qts, uint8_t *bytes)
{
    unsigned i;

    for (i = 0; i < 0x2000; i++) {
        bytes[i] = (i * 53 + 17) & 0xff;
    }
    qtest_memwrite(qts, SGI_DMA_WORD_TEST_BACKING, bytes, 0x2000);
}

static void query_assert_memory(QTestState *qts, const uint8_t *expected)
{
    uint8_t after[0x2000];

    qtest_memread(qts, SGI_DMA_WORD_TEST_BACKING, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), expected, sizeof(after));
}

static void query_expect_defaults(uint8_t *bytes, uint32_t count,
                                  uint32_t slot, uint16_t sequence)
{
    const uint32_t offsets[] = {
        0x900, 0x904, 0x914, 0x918, 0x91c, 0x920,
        0x928, 0x92c, 0x930, 0x934, 0x950, 0x954, 0x958, 0x978,
    };
    const uint32_t values[] = {
        0xc580c406, 1, 0xc580cc0e, 0x3f800000, 0x3f800000, 0x3f800000,
        0xc180cc19, 0, 0, 0x3f800000, 0xc380c821, 0, 0, 0x2f00c02e,
    };
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(offsets); i++) {
        stl_be_p(bytes + offsets[i], values[i]);
    }
    stl_be_p(bytes + 0x97c, count);
    stw_be_p(bytes + 0x800 + 2 * slot, sequence);
}

static void query_submit(QTestState *qts, uint32_t count, uint32_t slot,
                          uint32_t sequence)
{
    const uint32_t words[] = { 0x4c0, count, 0x10a5a, slot, sequence };

    query_words(qts, words, ARRAY_SIZE(words));
}

static void test_query_defaults(void)
{
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();

    query_seed(qts, expected);
    query_submit(qts, 0x12345678, 3, 0x9876);
    query_expect_defaults(expected, 0x12345678, 3, 0x9876);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void query_fragment(QTestState *qts, const uint32_t *words,
                            unsigned count, bool wide, unsigned start)
{
    unsigned i = 0;

    while (i < count) {
        uint64_t port = SGI_DMA_WORD_TEST_NATIVE +
                        (((i + start) & 1) ? 0x110000 : 0x100000);

        if (wide && i + 1 < count) {
            qtest_writeq(qts, port, ((uint64_t)words[i] << 32) | words[i + 1]);
            i += 2;
        } else {
            qtest_writel(qts, port, words[i++]);
        }
    }
}

static void test_query_fragments(const void *opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned prefix = (mode % 4) + 1;
    const uint32_t words[] = { 0x4c0, 0x12345678, 0x10a5a, 3, 0x9876 };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();

    query_seed(qts, expected);
    query_fragment(qts, words, prefix, mode >= 4, 0);
    query_assert_memory(qts, expected);
    query_fragment(qts, words + prefix, ARRAY_SIZE(words) - prefix,
                    mode >= 4, prefix);
    query_expect_defaults(expected, 0x12345678, 3, 0x9876);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void test_query_setters(void)
{
    const uint32_t setters[] = {
        0xce80c406, 0xfffffff7,
        0xc580cc0e, 0x3e800000, 0x3f000000, 0x3f400000,
        0xc180cc19, 0xbf800000, 0x80000000, 0x3f800000,
        0xc380c821, 0x3f800000, 0x40000000, 0x3000c02e,
    };
    const uint32_t offsets[] = {
        0x900, 0x904, 0x914, 0x918, 0x91c, 0x920,
        0x928, 0x92c, 0x930, 0x934, 0x950, 0x954, 0x958, 0x978,
    };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();
    unsigned i;

    query_seed(qts, expected);
    query_words(qts, setters, ARRAY_SIZE(setters));
    query_assert_memory(qts, expected);
    query_submit(qts, UINT32_MAX, 3, 0x1ffff);
    for (i = 0; i < ARRAY_SIZE(offsets); i++) {
        stl_be_p(expected + offsets[i], setters[i]);
    }
    stl_be_p(expected + 0x97c, UINT32_MAX);
    stw_be_p(expected + 0x806, 0xffff);
    query_assert_memory(qts, expected);
    query_submit(qts, 0, 3, 0x10000);
    stl_be_p(expected + 0x97c, 0);
    stw_be_p(expected + 0x806, 0);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void query_route_irq(QTestState *qts)
{
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x1044, 0);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x104c,
                 SGI_DMA_WORD_TEST_IRQ_SINK + 0x10);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x1054, 0x5a000000);
}

static void query_assert_no_irq(QTestState *qts)
{
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_IRQ_WRITES), ==, 0);
}

static void test_query_unsupported(const void *opaque)
{
    /* Bytes above 255 and texture forms other than two floats. */
    static const uint32_t tags[] = { 0xc580cc08, 0xce80cc08, 0xc380c800 };
    uint32_t tag = tags[GPOINTER_TO_UINT(opaque)];
    unsigned operands = (tag >> 10) & 0xf;
    uint32_t setter[17] = { tag };
    const uint32_t sync[] = { 0x10a5a, 3, 0x4321 };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();
    unsigned i;

    operands = operands == 15 ? 16 : operands;
    for (i = 1; i <= operands; i++) {
        setter[i] = 0x3f800000;
    }
    query_route_irq(qts);
    query_seed(qts, expected);
    query_words(qts, setter, operands + 1);
    query_submit(qts, 0x12345678, 0x8003, 0x9876);
    query_assert_memory(qts, expected);
    query_assert_no_irq(qts);
    query_words(qts, sync, ARRAY_SIZE(sync));
    stw_be_p(expected + 0x806, 0x4321);
    query_assert_memory(qts, expected);
    if (tag == 0xc380c800) {
        const uint32_t restore[] = { 0xc380c821, 0, 0 };

        query_words(qts, restore, ARRAY_SIZE(restore));
    } else {
        const uint32_t restore[] = {
            0xc580cc0e, 0x3f800000, 0x3f800000, 0x3f800000,
        };

        query_words(qts, restore, ARRAY_SIZE(restore));
    }
    query_submit(qts, 0xabcdef01, 3, 0x5432);
    query_expect_defaults(expected, 0xabcdef01, 3, 0x5432);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void test_query_cancel_address(const void *opaque)
{
    static const uint32_t offsets[] = { 0x4004, 0x400c, 0x4034, 0x403c };
    static const uint32_t values[] = {
        0, SGI_DMA_WORD_TEST_MEMORY, 0, SGI_DMA_WORD_TEST_MEMORY + 0x800,
    };
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned prefix = mode % 4 + 1;
    unsigned event = mode / 4;
    const uint32_t query[] = { 0x4c0, 0x12345678, 0x10a5a, 3, 0x9876 };
    const uint32_t sync[] = { 0x10a5a, 3, 0x4321 };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();

    query_seed(qts, expected);
    query_words(qts, query, prefix);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + offsets[event], values[event]);
    query_assert_memory(qts, expected);
    query_words(qts, sync, ARRAY_SIZE(sync));
    stw_be_p(expected + 0x806, 0x4321);
    query_assert_memory(qts, expected);
    query_submit(qts, 0xabcdef01, 3, 0x5432);
    query_expect_defaults(expected, 0xabcdef01, 3, 0x5432);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void test_query_partial_setter(void)
{
    const uint32_t index[] = { 0xc580c406 };
    const uint32_t normal[] = { 0xc180cc19, 0x3f800000, 0, 0 };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();

    query_seed(qts, expected);
    query_words(qts, index, ARRAY_SIZE(index));
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x400c,
                 SGI_DMA_WORD_TEST_MEMORY);
    query_words(qts, normal, ARRAY_SIZE(normal));
    query_submit(qts, 0x12345678, 3, 0x9876);
    query_expect_defaults(expected, 0x12345678, 3, 0x9876);
    stl_be_p(expected + 0x92c, 0x3f800000);
    stl_be_p(expected + 0x934, 0);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void test_query_reset(const void *opaque)
{
    unsigned prefix = GPOINTER_TO_UINT(opaque) + 1;
    const uint32_t query[] = { 0x4c0, 0x12345678, 0x10a5a, 3, 0x9876 };
    const uint32_t sync[] = { 0x10a5a, 3, 0x4321 };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();

    query_seed(qts, expected);
    query_words(qts, query, prefix);
    qtest_system_reset(qts);
    query_assert_memory(qts, expected);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x400c,
                 SGI_DMA_WORD_TEST_MEMORY);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x403c,
                 SGI_DMA_WORD_TEST_MEMORY + 0x800);
    query_words(qts, sync, ARRAY_SIZE(sync));
    stw_be_p(expected + 0x806, 0x4321);
    query_assert_memory(qts, expected);
    query_submit(qts, 0xabcdef01, 3, 0x5432);
    query_expect_defaults(expected, 0xabcdef01, 3, 0x5432);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void test_query_rejected_context(const void *opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned prefix = mode % 4 + 1;
    uint64_t event = ((uint64_t)(mode < 4 ? 6 : 7) << 40) |
                     (mode < 4 ? 1 : 0);
    const uint32_t query[] = { 0x4c0, 0x12345678, 0x10a5a, 3, 0x9876 };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();

    query_seed(qts, expected);
    query_words(qts, query, prefix);
    qtest_writeq(qts, SGI_DMA_WORD_TEST_NATIVE + 0x118000, event);
    query_assert_memory(qts, expected);
    query_words(qts, query + prefix, ARRAY_SIZE(query) - prefix);
    query_expect_defaults(expected, 0x12345678, 3, 0x9876);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void test_query_completed_event(void)
{
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();

    query_seed(qts, expected);
    query_submit(qts, 0x12345678, 3, 0x9876);
    query_expect_defaults(expected, 0x12345678, 3, 0x9876);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x4004, 0);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x400c,
                 SGI_DMA_WORD_TEST_MEMORY);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x4034, 0);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x403c,
                 SGI_DMA_WORD_TEST_MEMORY + 0x800);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void test_query_irq_ordering(void)
{
    uint8_t expected[0x2000], observed[0x2000];
    QTestState *qts = query_test_init();

    query_route_irq(qts);
    query_seed(qts, expected);
    query_submit(qts, 0x12345678, 0x8003, 0x9876);
    query_expect_defaults(expected, 0x12345678, 3, 0x9876);
    query_assert_memory(qts, expected);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_IRQ_WRITES), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_IRQ_ADDR), ==, 0x10);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_IRQ_SIZE), ==, 8);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_IRQ_VALUE), ==, 0x15a);
    qtest_memread(qts, SGI_DMA_WORD_TEST_CONTROL +
                  SGI_DMA_WORD_TEST_IRQ_SNAPSHOT,
                  observed, sizeof(observed));
    g_assert_cmpmem(observed, sizeof(observed), expected, sizeof(expected));
    query_submit(qts, 0xabcdef01, 3, 0x5432);
    query_expect_defaults(expected, 0xabcdef01, 3, 0x5432);
    query_assert_memory(qts, expected);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_IRQ_WRITES), ==, 1);
    qtest_quit(qts);
}

static void test_query_dma_failure(const void *opaque)
{
    static const unsigned windows[] = {
        SGI_DMA_WORD_TEST_READONLY, SGI_DMA_WORD_TEST_SPLIT_READONLY,
        SGI_DMA_WORD_TEST_SHORT, SGI_DMA_WORD_TEST_MMIO,
        SGI_DMA_WORD_TEST_SPLIT_BACKING, SGI_DMA_WORD_TEST_UNALIGNED_HOST,
        SGI_DMA_WORD_TEST_PAYLOAD_READONLY,
    };
    unsigned mode = GPOINTER_TO_UINT(opaque);
    unsigned window = windows[mode];
    const uint32_t sync[] = { 0x10a5a, 3, 0x4321 };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();

    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x400c,
                 SGI_DMA_WORD_TEST_MEMORY + window * SGI_DMA_WORD_TEST_STRIDE);
    query_route_irq(qts);
    query_seed(qts, expected);
    query_submit(qts, 0x12345678, 0x8003, 0x9876);
    if (mode >= 1 && mode <= 4) {
        uint8_t count[4], token[2];

        memcpy(count, expected + 0x97c, sizeof(count));
        memcpy(token, expected + 0x806, sizeof(token));
        query_expect_defaults(expected, 0x12345678, 3, 0x9876);
        memcpy(expected + 0x97c, count, sizeof(count));
        memcpy(expected + 0x806, token, sizeof(token));
    } else if (window == SGI_DMA_WORD_TEST_PAYLOAD_READONLY) {
        stl_be_p(expected + 0x900, 0xc580c406);
        stl_be_p(expected + 0x904, 1);
        stl_be_p(expected + 0x914, 0xc580cc0e);
        stl_be_p(expected + 0x918, 0x3f800000);
        stl_be_p(expected + 0x91c, 0x3f800000);
        stl_be_p(expected + 0x920, 0x3f800000);
    }
    query_assert_memory(qts, expected);
    query_assert_no_irq(qts);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_MMIO_READS), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_MMIO_WRITES), ==, 0);
    query_words(qts, sync, ARRAY_SIZE(sync));
    stw_be_p(expected + 0x806, 0x4321);
    query_assert_memory(qts, expected);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x400c,
                 SGI_DMA_WORD_TEST_MEMORY);
    query_submit(qts, 0xabcdef01, 3, 0x5432);
    query_expect_defaults(expected, 0xabcdef01, 3, 0x5432);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void test_query_sync_failure(const void *opaque)
{
    static const unsigned windows[] = {
        SGI_DMA_WORD_TEST_READONLY, SGI_DMA_WORD_TEST_HALF_SPLIT_READONLY,
        SGI_DMA_WORD_TEST_HALF_SHORT, SGI_DMA_WORD_TEST_HALF_SPLIT_BACKING,
        SGI_DMA_WORD_TEST_UNALIGNED_HOST, SGI_DMA_WORD_TEST_MMIO,
    };
    unsigned window = windows[GPOINTER_TO_UINT(opaque)];
    const uint32_t sync[] = { 0x10a5a, 3, 0x4321 };
    uint8_t expected[0x2000], token[2];
    QTestState *qts = query_test_init();

    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x403c,
                 SGI_DMA_WORD_TEST_MEMORY + window * SGI_DMA_WORD_TEST_STRIDE +
                 SGI_DMA_WORD_TEST_COUNTER);
    query_route_irq(qts);
    query_seed(qts, expected);
    memcpy(token, expected + 0x800, sizeof(token));
    query_submit(qts, 0x12345678, 0x8000, 0x9876);
    query_expect_defaults(expected, 0x12345678, 0, 0x9876);
    memcpy(expected + 0x800, token, sizeof(token));
    query_assert_memory(qts, expected);
    query_assert_no_irq(qts);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_MMIO_READS), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_MMIO_WRITES), ==, 0);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x403c,
                 SGI_DMA_WORD_TEST_MEMORY + 0x800);
    query_words(qts, sync, ARRAY_SIZE(sync));
    stw_be_p(expected + 0x806, 0x4321);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void test_query_accepted_context(const void *opaque)
{
    unsigned prefix = GPOINTER_TO_UINT(opaque) + 1;
    const uint32_t query[] = { 0x4c0, 0x12345678, 0x10a5a, 3, 0x9876 };
    const uint32_t sync[] = { 0x10a5a, 3, 0x4321 };
    const uint64_t event = (UINT64_C(7) << 40) | 1;
    uint8_t seed[0x2000], expected[0x2000];
    QTestState *baseline = query_test_init();
    QTestState *qts;

    /* Compare only cancellation; snapshot contents remain opaque here. */
    query_seed(baseline, seed);
    qtest_writeq(baseline, SGI_DMA_WORD_TEST_NATIVE + 0x118000, event);
    qtest_memread(baseline, SGI_DMA_WORD_TEST_BACKING, expected,
                  sizeof(expected));
    g_assert_cmpuint(ldl_be_p(expected + 0x97c), !=, 0x12345678);
    g_assert_cmpuint(lduw_be_p(expected + 0x806), !=, 0x9876);
    qtest_quit(baseline);

    qts = query_test_init();
    query_seed(qts, seed);
    query_words(qts, query, prefix);
    qtest_writeq(qts, SGI_DMA_WORD_TEST_NATIVE + 0x118000, event);
    query_assert_memory(qts, expected);
    query_words(qts, sync, ARRAY_SIZE(sync));
    stw_be_p(expected + 0x806, 0x4321);
    query_assert_memory(qts, expected);
    query_submit(qts, 0xabcdef01, 3, 0x5432);
    query_expect_defaults(expected, 0xabcdef01, 3, 0x5432);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void query_assert_pattern(QTestState *qts, const uint32_t *expected)
{
    unsigned i;

    for (i = 0; i < 32; i++) {
        g_assert_cmphex(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                   SGI_DMA_WORD_TEST_PATTERN + 4 * i), ==,
                        expected[i]);
    }
}

static void test_query_streaming_cancel(const void *opaque)
{
    static const uint32_t offsets[] = { 0x4004, 0x400c, 0x4034, 0x403c };
    static const uint32_t values[] = {
        0, SGI_DMA_WORD_TEST_MEMORY, 0, SGI_DMA_WORD_TEST_MEMORY + 0x800,
    };
    unsigned mode = GPOINTER_TO_UINT(opaque);
    uint32_t low[17] = { 0x00013c4e }, high[17] = { 0x00013c4f };
    uint32_t replacement[17] = { 0x00013c4e };
    uint32_t pattern[32];
    const uint32_t index[] = { 0xc580c406, 0xfffffff9 };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();
    unsigned i;

    query_seed(qts, expected);
    for (i = 0; i < 16; i++) {
        pattern[i] = low[i + 1] = 0xa1000000 + i;
        pattern[16 + i] = high[i + 1] = 0xb2000000 + i;
        replacement[i + 1] = 0xc3000000 + i;
    }
    query_words(qts, low, ARRAY_SIZE(low));
    query_words(qts, high, ARRAY_SIZE(high));
    query_assert_pattern(qts, pattern);
    query_words(qts, replacement, 3);
    pattern[0] = replacement[1];
    pattern[1] = replacement[2];
    query_assert_pattern(qts, pattern);
    if (mode < 4) {
        qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + offsets[mode],
                     values[mode]);
    } else if (mode == 7) {
        qtest_system_reset(qts);
        memset(pattern, 0, sizeof(pattern));
        qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x400c,
                     SGI_DMA_WORD_TEST_MEMORY);
        qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x403c,
                     SGI_DMA_WORD_TEST_MEMORY + 0x800);
    } else {
        uint64_t event = ((uint64_t)(mode == 5 ? 6 : 7) << 40) |
                         (mode == 6 ? 0 : 1);

        qtest_writeq(qts, SGI_DMA_WORD_TEST_NATIVE + 0x118000, event);
    }
    query_assert_pattern(qts, pattern);
    if (mode == 5 || mode == 6) {
        query_words(qts, replacement + 3, 14);
        for (i = 2; i < 16; i++) {
            pattern[i] = replacement[i + 1];
        }
        query_assert_pattern(qts, pattern);
    }
    for (i = 0; i < 16; i++) {
        pattern[16 + i] = high[i + 1] = 0xd4000000 + i;
    }
    query_words(qts, high, ARRAY_SIZE(high));
    query_assert_pattern(qts, pattern);
    query_words(qts, index, ARRAY_SIZE(index));
    /* Snapshot writes are independent of the new query response. */
    qtest_memread(qts, SGI_DMA_WORD_TEST_BACKING, expected, sizeof(expected));
    query_submit(qts, 0x12345678, 3, 0x9876);
    query_expect_defaults(expected, 0x12345678, 3, 0x9876);
    stl_be_p(expected + 0x904, 0xfffffff9);
    query_assert_memory(qts, expected);
    query_assert_pattern(qts, pattern);
    qtest_quit(qts);
}

static void test_query_unpaired(void)
{
    const uint32_t prefix[] = { 0x4c0, 0x12345678 };
    const uint32_t index[] = { 0xc580c406, 0xfffffffb };
    const uint32_t sync[] = { 0x10a5a, 3, 0x4321 };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();

    query_seed(qts, expected);
    query_words(qts, prefix, ARRAY_SIZE(prefix));
    query_words(qts, index, ARRAY_SIZE(index));
    query_assert_memory(qts, expected);
    query_words(qts, sync, ARRAY_SIZE(sync));
    stw_be_p(expected + 0x806, 0x4321);
    query_assert_memory(qts, expected);
    query_submit(qts, 0xabcdef01, 3, 0x5432);
    query_expect_defaults(expected, 0xabcdef01, 3, 0x5432);
    stl_be_p(expected + 0x904, 0xfffffffb);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void test_query_zero_shadow_context(void)
{
    const uint32_t tag[] = { 0xc580c406 };
    const uint32_t operand[] = { 9 };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();

    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x400c, 0);
    query_seed(qts, expected);
    query_words(qts, tag, ARRAY_SIZE(tag));
    qtest_writeq(qts, SGI_DMA_WORD_TEST_NATIVE + 0x118000,
                 (UINT64_C(7) << 40) | 1);
    query_words(qts, operand, ARRAY_SIZE(operand));
    query_assert_memory(qts, expected);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x400c,
                 SGI_DMA_WORD_TEST_MEMORY);
    query_submit(qts, 0x12345678, 3, 0x9876);
    query_expect_defaults(expected, 0x12345678, 3, 0x9876);
    stl_be_p(expected + 0x904, 9);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

static void test_query_context_snapshot_failure(const void *opaque)
{
    unsigned prefix = GPOINTER_TO_UINT(opaque) + 1;
    const uint32_t query[] = { 0x4c0, 0x12345678, 0x10a5a, 3, 0x9876 };
    const uint32_t sync[] = { 0x10a5a, 3, 0x4321 };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();

    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x400c, 0x10000000);
    query_seed(qts, expected);
    query_words(qts, query, prefix);
    qtest_writeq(qts, SGI_DMA_WORD_TEST_NATIVE + 0x118000,
                 (UINT64_C(7) << 40) | 1);
    query_assert_memory(qts, expected);
    query_words(qts, sync, ARRAY_SIZE(sync));
    stw_be_p(expected + 0x806, 0x4321);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

/*
 * Formats 7, 8, 13, 21 and 22 to 24: each record answers with the complete
 * command word its last setter sent and exactly that word's raw operands
 * (docs/reviews/odyssey-query-formats-7-13-evidence-20260930.md). The decode
 * helpers apply the GL library's published consumer rules.
 */
#define QUERY_S8 0x3b808081u
#define QUERY_S32 0x2f800005u

static uint32_t query_f32_bits(float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static float query_bits_f32(uint32_t bits)
{
    float f;

    memcpy(&f, &bits, sizeof(f));
    return f;
}

/*
 * Format 13: ((float)v * 2 + 1) * S32, with the multiply-add fused. In
 * double, (float)v * 2 + 1 is exact, so one rounding to single matches it.
 */
static uint32_t query_decode_13(uint32_t v)
{
    float fused = (double)(float)(int32_t)v * 2.0 + 1.0;

    return query_f32_bits(fused * query_bits_f32(QUERY_S32));
}

/* Format 8: the 256-entry table, (float)i * S8. */
static uint32_t query_decode_8(uint32_t v)
{
    g_assert_cmpuint(v, <=, 255);
    return query_f32_bits((float)v * query_bits_f32(QUERY_S8));
}

static void query_expect_words(uint8_t *bytes, unsigned offset,
                               const uint32_t *words, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        stl_be_p(bytes + offset + 4 * i, words[i]);
    }
}

/* Answer one query and compare the whole allocation against the defaults. */
static void query_check_record(QTestState *qts, uint8_t *expected,
                               uint32_t count, unsigned offset,
                               const uint32_t *record, unsigned words)
{
    query_submit(qts, count, 3, count & 0xffff);
    query_expect_defaults(expected, count, 3, count & 0xffff);
    query_expect_words(expected, offset, record, words);
    query_assert_memory(qts, expected);
}

static void test_query_index_formats(void)
{
    static const uint32_t setters[][2] = {
        { 0xce80c407, 0x3f800000 },
        { 0xc580c407, 0x40a00000 },
        { 0xc580c406, 0x00000005 },
        { 0xce80c406, 0xfffffff7 },
    };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();
    unsigned i;

    query_seed(qts, expected);
    for (i = 0; i < ARRAY_SIZE(setters); i++) {
        query_words(qts, setters[i], 2);
        /* +0x908 to +0x913 keep their sentinels. */
        query_check_record(qts, expected, 0x1000 + i, 0x900, setters[i], 2);
    }
    qtest_quit(qts);
}

/* Format 14: three raw singles, from either the c580 or the ce80 word. */
static void test_query_float_colour(void)
{
    static const uint32_t setters[][4] = {
        { 0xce80cc0e, 0x3e800000, 0x7fc12345, 0x80000000 },
        { 0xc580cc0e, 0x3f000000, 0xbf800000, 0x00000001 },
    };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();
    unsigned i;

    query_seed(qts, expected);
    for (i = 0; i < ARRAY_SIZE(setters); i++) {
        query_words(qts, setters[i], 4);
        /* Exactly three operands: +0x924 keeps its sentinel. */
        query_check_record(qts, expected, 0x1800 + i, 0x914, setters[i], 4);
    }
    qtest_quit(qts);
}

static void test_query_int_colour(void)
{
    static const uint32_t setters[][4] = {
        { 0xc580cc0d, 0x7fffffff, 0x3fffffff, 0x00000000 },
        { 0xce80cc0d, 0x80000000, 0x3f800000, 0x7ffffb00 },
    };
    static const uint32_t decoded[][3] = {
        { 0x3f800005, 0x3f000005, 0x2f800005 },
        { 0xbf800005, 0x3efe000a, 0x3f800000 },
    };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();
    unsigned i, j;

    query_seed(qts, expected);
    for (i = 0; i < ARRAY_SIZE(setters); i++) {
        query_words(qts, setters[i], 4);
        /* Exactly three operands: +0x924 keeps its sentinel. */
        query_check_record(qts, expected, 0x2000 + i, 0x914, setters[i], 4);
        for (j = 0; j < 3; j++) {
            uint32_t v = ldl_be_p(expected + 0x918 + 4 * j);

            g_assert_cmphex(query_decode_13(v), ==, decoded[i][j]);
        }
    }
    qtest_quit(qts);
}

static void test_query_byte_colour(void)
{
    static const uint32_t bytes[] = { 0xc580cc08, 255, 128, 0 };
    static const uint32_t zeros[] = { 0xce80cc08, 0, 0, 0 };
    static const uint32_t wide[] = { 0xc580cc08, 256, 0, 0 };
    static const uint32_t restore[] = { 0xc580cc08, 1, 2, 3 };
    static const uint32_t decoded[] = { 0x3f800000, 0x3f008081, 0 };
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *args = NULL;
    g_autofree char *log = NULL;
    uint8_t expected[0x2000];
    QTestState *qts;
    unsigned i;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-query-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    args = g_strdup_printf("-M none -cpu R14000 "
                           "-device sgi-dma-word-test,native=on "
                           "-d guest_errors -D %s", quoted);
    qts = qtest_init(args);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x4004, 0);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x400c,
                 SGI_DMA_WORD_TEST_MEMORY);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x4034, 0);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x403c,
                 SGI_DMA_WORD_TEST_MEMORY + 0x800);
    query_route_irq(qts);
    query_seed(qts, expected);

    query_words(qts, bytes, ARRAY_SIZE(bytes));
    query_check_record(qts, expected, 0x3000, 0x914, bytes, 4);
    for (i = 0; i < 3; i++) {
        g_assert_cmphex(query_decode_8(ldl_be_p(expected + 0x918 + 4 * i)),
                        ==, decoded[i]);
    }
    query_words(qts, zeros, ARRAY_SIZE(zeros));
    query_check_record(qts, expected, 0x3001, 0x914, zeros, 4);

    /* Above 255: no payload, counter, paired sync or interrupt. */
    query_words(qts, wide, ARRAY_SIZE(wide));
    query_submit(qts, 0x3002, 0x8003, 0x3002);
    query_assert_memory(qts, expected);
    query_assert_no_irq(qts);

    /* A later in-range setter restores support. */
    query_words(qts, restore, ARRAY_SIZE(restore));
    query_submit(qts, 0x3003, 0x8003, 0x3003);
    query_expect_defaults(expected, 0x3003, 3, 0x3003);
    query_expect_words(expected, 0x914, restore, ARRAY_SIZE(restore));
    query_assert_memory(qts, expected);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_IRQ_WRITES), ==, 1);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "unsupported current-query record 1 "
                                 "tag 0xc580cc08"));
    g_assert_nonnull(strstr(log, "unsupported current-query record 1 "
                                 "in snapshot"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

static void test_query_four_float_colour(void)
{
    static const uint32_t setters[][5] = {
        { 0xc580d015, 0x3e800000, 0x7fc12345, 0x80000000, 0x3e000000 },
        { 0xce80d015, 0x3f800000, 0, 0x3f000000, 0xbf800000 },
    };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();
    unsigned i;

    query_seed(qts, expected);
    for (i = 0; i < ARRAY_SIZE(setters); i++) {
        query_words(qts, setters[i], 5);
        /* All four operand slots; +0x928 stays the normal's command. */
        query_check_record(qts, expected, 0x4000 + i, 0x914, setters[i], 5);
        g_assert_cmphex((uint32_t)ldl_be_p(expected + 0x928), ==, 0xc180cc19);
    }
    qtest_quit(qts);
}

static void test_query_normal_formats(void)
{
    static const uint32_t setters[][4] = {
        { 0xc180cc18, 0x7fffffff, 0x80000000, 0x00000000 },
        { 0xc180cc17, 0x00007fff, 0xffff8000, 0x00000001 },
        { 0xc180cc16, 0x0000007f, 0xffffff80, 0x00000000 },
        { 0xc180cc19, 0x3f800000, 0x7fc12345, 0x80000000 },
    };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();
    unsigned i;

    query_seed(qts, expected);
    for (i = 0; i < ARRAY_SIZE(setters); i++) {
        query_words(qts, setters[i], 4);
        /* Three operands: +0x938 keeps its sentinel. */
        query_check_record(qts, expected, 0x5000 + i, 0x928, setters[i], 4);
    }
    qtest_quit(qts);
}

/*
 * The context save writes the retained index and colour words with their
 * raw operands, the count from bits 13:10, at +0x240/+0x800 and
 * +0x250/+0x840 and in ring slot 0 of attributes 1 and 2. Replaying them
 * through the kernel port the way the kernel does restores records that
 * decode to the values the first client set.
 */
static void query_replay(QTestState *qts, uint32_t word_offset,
                         uint32_t operand_offset)
{
    uint8_t page[0x2000];
    uint32_t word;
    unsigned i, count;

    qtest_memread(qts, SGI_DMA_WORD_TEST_BACKING, page, sizeof(page));
    word = ldl_be_p(page + word_offset);
    count = (word >> 10) & 0xf;
    g_assert_cmpuint(count, >, 0);
    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x110000, word);
    for (i = 0; i < count; i++) {
        qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x110000,
                     ldl_be_p(page + operand_offset + 4 * i));
    }
}

static void test_query_save_round_trip(const void *opaque)
{
    static const struct {
        unsigned record;
        uint32_t words[5];
        unsigned count;
    } cases[] = {
        { 1, { 0xc580cc0e, 0x3e800000, 0x3f000000, 0x3f400000 }, 4 },
        { 1, { 0xc580cc08, 255, 128, 0 }, 4 },
        { 1, { 0xc580d015, 0x3e800000, 0x3f000000, 0x3f400000,
               0x3e000000 }, 5 },
        { 0, { 0xc580c407, 0x40a00000 }, 2 },
        { 1, { 0xc580cc0d, 0x7fffffff, 0x3fffffff, 0 }, 4 },
        { 0, { 0xce80c407, 0x3f800000 }, 2 },
    };
    static const uint32_t other[] = {
        0xc580cc0e, 0, 0x3f800000, 0, 0xc580c406, 9,
    };
    unsigned c = GPOINTER_TO_UINT(opaque);
    unsigned record = cases[c].record;
    unsigned count = cases[c].count;
    const uint32_t *words = cases[c].words;
    unsigned word_offset = record ? 0x250 : 0x240;
    unsigned operand_offset = record ? 0x840 : 0x800;
    unsigned ring_word = 0x100 + 16 * (record + 1);
    unsigned ring_operands = 0x300 + 64 * (record + 1);
    uint8_t page[0x2000], expected[0x2000];
    QTestState *qts = query_test_init();
    unsigned i;

    query_seed(qts, expected);
    query_words(qts, words, count);
    qtest_writeq(qts, SGI_DMA_WORD_TEST_NATIVE + 0x118000,
                 (UINT64_C(7) << 40) | (1 << 16) | 1);
    qtest_memread(qts, SGI_DMA_WORD_TEST_BACKING, page, sizeof(page));
    g_assert_cmphex((uint32_t)ldl_be_p(page + word_offset), ==, words[0]);
    g_assert_cmphex((uint32_t)ldl_be_p(page + ring_word), ==, words[0]);
    for (i = 1; i < count; i++) {
        uint32_t saved = ldl_be_p(page + operand_offset + 4 * (i - 1));
        uint32_t ring = ldl_be_p(page + ring_operands + 4 * (i - 1));

        g_assert_cmphex(saved, ==, words[i]);
        g_assert_cmphex(ring, ==, words[i]);
    }

    /* Another client's colour and index, then the kernel's replay. */
    query_words(qts, other, ARRAY_SIZE(other));
    query_replay(qts, 0x240, 0x800);
    query_replay(qts, 0x250, 0x840);
    qtest_memread(qts, SGI_DMA_WORD_TEST_BACKING, expected, sizeof(expected));
    query_submit(qts, 0x6000, 3, 0x6000);
    query_expect_defaults(expected, 0x6000, 3, 0x6000);
    query_expect_words(expected, record ? 0x914 : 0x900, words, count);
    query_assert_memory(qts, expected);

    /* The decoded value is the one set, not a reinterpretation. */
    if (words[0] == 0xc580cc08) {
        g_assert_cmphex(query_decode_8(ldl_be_p(expected + 0x918)), ==,
                        0x3f800000);
        g_assert_cmphex(query_decode_8(ldl_be_p(expected + 0x91c)), ==,
                        0x3f008081);
        g_assert_cmphex(query_decode_8(ldl_be_p(expected + 0x920)), ==, 0);
    } else if (words[0] == 0xc580cc0d) {
        g_assert_cmphex(query_decode_13(ldl_be_p(expected + 0x918)), ==,
                        0x3f800005);
    }
    qtest_quit(qts);
}

/* Replay ring attribute a, slot 0, from the page through the kernel port. */
static void query_replay_ring(QTestState *qts, unsigned a)
{
    uint32_t word = qtest_readl(qts, SGI_DMA_WORD_TEST_BACKING + 0x100 +
                                16 * a);
    unsigned i, count = (word >> 10) & 0xf;

    qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x110000, word);
    for (i = 0; i < count; i++) {
        qtest_writel(qts, SGI_DMA_WORD_TEST_NATIVE + 0x110000,
                     qtest_readl(qts, SGI_DMA_WORD_TEST_BACKING + 0x300 +
                                 64 * a + 4 * i));
    }
}

/*
 * The edge flag and the normal survive a switch: the save writes ring 7
 * slot 0 as the edge record's word and ring 3 slot 0 as the normal record's
 * word and raw operands, and the kernel's replay of both after another
 * client's settings brings the query answers back. A never-set edge flag
 * saves true, and a client's false flag survives another client's true one.
 */
static void test_query_edge_and_normal_survive(void)
{
    static const uint32_t mine[] = {
        0x2f00c02e, 0x3000c02e,
        0xc180cc18, 0x7fffffff, 0x80000000, 0x12345678,
    };
    static const uint32_t other[] = {
        0x2f00c02e, 0xc180cc19, 0x3f800000, 0, 0,
    };
    uint8_t expected[0x2000];
    QTestState *qts = query_test_init();
    unsigned i;

    query_seed(qts, expected);
    qtest_writeq(qts, SGI_DMA_WORD_TEST_NATIVE + 0x118000,
                 (UINT64_C(7) << 40) | (UINT64_C(0x7f) << 16) | 1);
    g_assert_cmphex(qtest_readl(qts, SGI_DMA_WORD_TEST_BACKING + 0x170), ==,
                    0x2f00c02e);

    query_words(qts, mine, ARRAY_SIZE(mine));
    qtest_writeq(qts, SGI_DMA_WORD_TEST_NATIVE + 0x118000,
                 (UINT64_C(7) << 40) | (1 << 16) | 1);
    g_assert_cmphex(qtest_readl(qts, SGI_DMA_WORD_TEST_BACKING + 0x170), ==,
                    0x3000c02e);
    g_assert_cmphex(qtest_readl(qts, SGI_DMA_WORD_TEST_BACKING + 0x130), ==,
                    0xc180cc18);
    for (i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readl(qts, SGI_DMA_WORD_TEST_BACKING + 0x3c0 +
                                    4 * i), ==, mine[3 + i]);
    }

    query_words(qts, other, ARRAY_SIZE(other));
    query_replay_ring(qts, 3);
    query_replay_ring(qts, 7);
    qtest_memread(qts, SGI_DMA_WORD_TEST_BACKING, expected, sizeof(expected));
    query_submit(qts, 0x6100, 3, 0x6100);
    query_expect_defaults(expected, 0x6100, 3, 0x6100);
    query_expect_words(expected, 0x928, mine + 2, 4);
    stl_be_p(expected + 0x978, 0x3000c02e);
    query_assert_memory(qts, expected);
    qtest_quit(qts);
}

/*
 * Every mapped record in one response, observed at interrupt delivery: the
 * counter and paired sync appear only with all records already written.
 */
static void test_query_mixed_records(void)
{
    static const uint32_t setters[] = {
        0xce80c407, 0x3f800000,
        0xc580cc0d, 0x7fffffff, 0x80000000, 0x12345678,
        0xc180cc19, 0xbf800000, 0x3f000000, 0x80000000,
        0xc380c821, 0x3e800000, 0x40400000,
        0x3000c02e,
    };
    static const unsigned offsets[] = {
        0x900, 0x904, 0x914, 0x918, 0x91c, 0x920,
        0x928, 0x92c, 0x930, 0x934, 0x950, 0x954, 0x958, 0x978,
    };
    uint8_t expected[0x2000], observed[0x2000];
    QTestState *qts = query_test_init();
    unsigned i;

    query_route_irq(qts);
    query_seed(qts, expected);
    query_words(qts, setters, ARRAY_SIZE(setters));
    query_assert_memory(qts, expected);
    query_submit(qts, 0x7000, 0x8003, 0x7000);
    for (i = 0; i < ARRAY_SIZE(offsets); i++) {
        stl_be_p(expected + offsets[i], setters[i]);
    }
    stl_be_p(expected + 0x97c, 0x7000);
    stw_be_p(expected + 0x806, 0x7000);
    query_assert_memory(qts, expected);
    g_assert_cmpuint(qtest_readl(qts, SGI_DMA_WORD_TEST_CONTROL +
                                SGI_DMA_WORD_TEST_IRQ_WRITES), ==, 1);
    qtest_memread(qts, SGI_DMA_WORD_TEST_CONTROL +
                  SGI_DMA_WORD_TEST_IRQ_SNAPSHOT,
                  observed, sizeof(observed));
    g_assert_cmpmem(observed, sizeof(observed), expected, sizeof(expected));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    static const char *names[] = {
        "writable", "readonly", "split-readonly", "short", "mmio",
        "split-backing", "unaligned-host",
    };
    unsigned i;

    g_test_init(&argc, &argv, NULL);
    for (i = 0; i < ARRAY_SIZE(names); i++) {
        g_autofree char *name = g_strdup_printf("/sgi-dma-word/real-memory/%s",
                                               names[i]);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_word_mapping);
    }
    for (i = 0; i < 8; i++) {
        g_autofree char *name = g_strdup_printf("/sgi-dma-word/ordinary/%u", i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_ordinary_store);
    }
    for (i = 0; i < 12; i++) {
        g_autofree char *name = g_strdup_printf("/sgi-dma-half/real-memory/%u",
                                               i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_half_publication);
    }
    for (i = 0; i < 7; i++) {
        g_autofree char *name = g_strdup_printf("/sgi-query/dma-failure/%u", i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_query_dma_failure);
    }
    for (i = 0; i < 6; i++) {
        g_autofree char *name = g_strdup_printf(
            "/sgi-query/sync-failure/%u", i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_query_sync_failure);
    }
    for (i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf(
            "/sgi-query/accepted-context/%u", i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i),
                            test_query_accepted_context);
    }
    for (i = 0; i < 8; i++) {
        g_autofree char *name = g_strdup_printf(
            "/sgi-query/streaming-cancel/%u", i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i),
                            test_query_streaming_cancel);
    }
    qtest_add_func("/sgi-query/unpaired", test_query_unpaired);
    qtest_add_func("/sgi-query/zero-shadow-context",
                   test_query_zero_shadow_context);
    for (i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf(
            "/sgi-query/context-snapshot-failure/%u", i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i),
                            test_query_context_snapshot_failure);
    }
    qtest_add_func("/sgi-query/irq-ordering", test_query_irq_ordering);
    qtest_add_func("/sgi-query/defaults", test_query_defaults);
    qtest_add_func("/sgi-query/setters", test_query_setters);
    qtest_add_func("/sgi-query/partial-setter", test_query_partial_setter);
    qtest_add_func("/sgi-query/completed-event", test_query_completed_event);
    for (i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("/sgi-query/reset/%u", i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_query_reset);
    }
    for (i = 0; i < 8; i++) {
        g_autofree char *name = g_strdup_printf(
            "/sgi-query/rejected-context/%u", i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i),
                            test_query_rejected_context);
    }
    for (i = 0; i < 8; i++) {
        g_autofree char *fragment = g_strdup_printf(
            "/sgi-query/fragments/%u", i);

        qtest_add_data_func(fragment, GUINT_TO_POINTER(i),
                            test_query_fragments);
    }
    for (i = 0; i < 3; i++) {
        g_autofree char *unsupported = g_strdup_printf(
            "/sgi-query/unsupported/%u", i);

        qtest_add_data_func(unsupported, GUINT_TO_POINTER(i),
                            test_query_unsupported);
    }
    for (i = 0; i < 6; i++) {
        g_autofree char *name = g_strdup_printf(
            "/sgi-query/save-round-trip/%u", i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i),
                            test_query_save_round_trip);
    }
    qtest_add_func("/sgi-query/index-formats", test_query_index_formats);
    qtest_add_func("/sgi-query/float-colour", test_query_float_colour);
    qtest_add_func("/sgi-query/int-colour", test_query_int_colour);
    qtest_add_func("/sgi-query/byte-colour", test_query_byte_colour);
    qtest_add_func("/sgi-query/four-float-colour",
                   test_query_four_float_colour);
    qtest_add_func("/sgi-query/normal-formats", test_query_normal_formats);
    qtest_add_func("/sgi-query/mixed-records", test_query_mixed_records);
    qtest_add_func("/sgi-query/edge-and-normal-survive",
                   test_query_edge_and_normal_survive);
    for (i = 0; i < 16; i++) {
        g_autofree char *name = g_strdup_printf(
            "/sgi-query/address-cancel/%u", i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i),
                            test_query_cancel_address);
    }
    return g_test_run();
}
