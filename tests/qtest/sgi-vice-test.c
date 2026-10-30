/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/misc/sgi-dma-word-test.h"
#include "memtx.h"
#include "sgi-migrate.h"
#include <glib/gstdio.h>

#define BASE 0x17000000ULL
#define PATH "/machine/peripheral/vice"

static QTestState *start_options(unsigned hz, bool stable, const char *extra)
{
    QTestState *s = qtest_initf("-M none -m 32M -cpu R10000 "
        "-device sgi-vice,id=vice,part-id=225,device-clock-hz=%u,"
        "msp-clock-hz=66000000,stable-reference=%s " MEMTX_TEST_DEVICE " %s",
        hz, stable ? "on" : "off", extra);

    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    return s;
}

static QTestState *start(void)
{
    return start_options(60000000, true, "");
}

static void test_identity(void)
{
    QTestState *s = start();

    g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 0xe1);
    g_assert_cmphex(qtest_readb(s, BASE + 15), ==, 0xe1);
    g_assert_cmphex(qtest_readl(s, BASE + 8), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x180), ==, 0x10);
    g_assert_cmphex(qtest_readq(s, BASE + 0x188), ==, 0x10);
    qtest_quit(s);
}


static void reg(QTestState *s, unsigned offset, uint64_t value)
{
    g_assert_cmpint(memtx_store(s, BASE + offset, value, 8), ==, MEMTX_OK);
}

static uint64_t read_reg(QTestState *s, unsigned offset)
{
    return qtest_readq(s, BASE + offset);
}

static void descriptor(QTestState *s, unsigned channel, unsigned index,
                       uint16_t flags, uint32_t system, uint16_t width,
                       uint16_t stride, uint16_t lines, uint16_t local)
{
    uint16_t words[] = { flags, system >> 16, system, width, stride,
                         lines, local, 0 };

    for (unsigned i = 0; i < 8; i++) {
        reg(s, 0x1000 + channel * 0x100 + index * 0x40 + i * 8, words[i]);
    }
}

static void run(QTestState *s, unsigned channel, unsigned index, bool bypass)
{
    reg(s, 0x180 + channel * 0x40,
        3 | (0x10 << index) | (bypass ? 0x100 : 0));
}

static void finish(QTestState *s)
{
    qtest_clock_step(s, 20000);
}

static void check_bytes(QTestState *s, uint64_t address,
                        const uint8_t *expected, unsigned count)
{
    uint8_t bytes[512];

    g_assert_cmpuint(count, <=, sizeof(bytes));
    if (address >= BASE && address < BASE + 0x100000) {
        /* Bulk qtest IO can dispatch a misaligned eight-byte transaction. */
        for (unsigned i = 0; i < count; i++) {
            bytes[i] = qtest_readb(s, address + i);
        }
    } else {
        qtest_memread(s, address, bytes, count);
    }
    for (unsigned i = 0; i < count; i++) {
        if (bytes[i] != expected[i]) {
            g_test_message("address=0x%" PRIx64 " byte=%u got=%02x want=%02x "
                           "status=%" PRIx64 "/%" PRIx64,
                           address, i, bytes[i], expected[i],
                           read_reg(s, 0x188), read_reg(s, 0x1c8));
            break;
        }
    }
    g_assert_cmpmem(bytes, count, expected, count);
}

/* Missing masking, lane preservation or tail decoding corrupts these values. */
static void test_memories(void)
{
    QTestState *s = start();
    const unsigned writable[] = { 0x2000, 0x2ff8, 0x4000, 0x47f8,
                                  0x8000, 0x87f8, 0x8800, 0x8ff8,
                                  0x9000, 0x97f8 };
    const unsigned holes[] = { 0x3000, 0x3ff8, 0x4800, 0x4ff8,
                              0x6400, 0x6ff8, 0x9800, 0xdff8,
                              0x20000, 0xffff8 };
    uint64_t value;

    for (unsigned i = 0; i < G_N_ELEMENTS(writable); i++) {
        unsigned a = writable[i];
        reg(s, a, 0x0123456789abcdefULL);
        g_assert_cmphex(read_reg(s, a), ==, 0x0123456789abcdefULL);
        g_assert_cmphex(qtest_readb(s, BASE + a), ==, 0x01);
        g_assert_cmphex(qtest_readw(s, BASE + a + 2), ==, 0x4567);
    }
    reg(s, 0x5000, 0xffffffff81234567ULL);
    g_assert_cmphex(read_reg(s, 0x5000), ==, 0x003fffff00234567ULL);
    reg(s, 0x63f8, 0x000123450006789aULL);
    g_assert_cmphex(read_reg(s, 0x63f8), ==, 0x000123450006789aULL);
    for (unsigned i = 0; i < G_N_ELEMENTS(holes); i++) {
        reg(s, holes[i], UINT64_MAX);
        g_assert_cmphex(read_reg(s, holes[i]), ==, 0);
    }
    g_assert_cmpint(memtx_store(s, BASE + 0x2000, 0xabcd, 2), !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, BASE + 0x4000, 0xab, 1), !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, BASE + 0x5000, 0xabcd, 2), !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, BASE + 0x1007, 0x12, 1), !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, BASE + 0xf006, 0x1234, 2), !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, BASE + 0xf004, 0x12340007, 4), !=, MEMTX_OK);
    reg(s, 0xf000, 0x12340003);
    g_assert_cmphex(read_reg(s, 0xf000), ==, 0x12340003);
    g_assert_cmpint(memtx_load(s, BASE + 0x8001, &value, 4), !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, BASE + 0x8001, 0x1234, 8), !=, MEMTX_OK);
    reg(s, 0x100, 0x12345678);
    g_assert_cmpint(memtx_store(s, BASE + 0x107, 0xa9, 1), ==, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0x100), ==, 0x123456a9);
    /* Reserved bank bits are retained, not DMA permissions. */
    reg(s, 0x100, 0);
    g_assert_cmpint(memtx_store(s, BASE + 0x2004, 0x10293847, 4), ==, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0x2000), ==, 0x0123456710293847ULL);
    g_assert_cmpint(memtx_store(s, BASE + 0x4002, 0xbeef, 2), ==, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0x4000), ==, 0x0123beef89abcdefULL);
    g_assert_cmpint(memtx_store(s, BASE + 0x5000, 0xffabcdef, 4), ==, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0x5000), ==, 0x002bcdef00234567ULL);
    g_assert_cmpint(memtx_store(s, BASE + 0x1006, 0xa000, 2), ==, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0x1000), ==, 0xa000);
    g_assert_cmpint(memtx_store(s, BASE + 0x1004, 0x1234beef, 4), ==, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0x1000), ==, 0xbeef);
    g_assert_cmpint(memtx_store(s, BASE + 0xf004, 0x450003, 4), ==, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0xf000), ==, 0x450003);
    qtest_quit(s);
}

/*
 * Strobes must latch zero payloads, masks must neither create nor consume
 * events.
 */
static void test_interrupts(void)
{
    QTestState *s = start();

    qtest_irq_intercept_out_named(s, PATH, "sysbus-irq");
    reg(s, 0x80, 0);
    g_assert_cmphex(read_reg(s, 0x160), ==, 0x10);
    g_assert_false(qtest_get_irq(s, 0));
    reg(s, 0xe010, 0x14);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_cmpint(memtx_store(s, BASE + 0x1bc, 0x12345678, 4), ==, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0x160), ==, 0x14);
    reg(s, 0xe008, 0);
    g_assert_cmphex(read_reg(s, 0x160), ==, 0x14);
    reg(s, 0xe008, 0x10);
    g_assert_true(qtest_get_irq(s, 0));
    reg(s, 0xe008, 4);
    g_assert_false(qtest_get_irq(s, 0));
    reg(s, 0x1b8, 0);
    g_assert_true(qtest_get_irq(s, 0));
    reg(s, 0xe010, 0);
    g_assert_false(qtest_get_irq(s, 0));
    reg(s, 0x160, UINT64_MAX);
    g_assert_cmphex(read_reg(s, 0x160), ==, 4);
    qtest_quit(s);
}

/*
 * Byte patterns catch reversed direction, fixed values and hidden whole-copy
 * behavior.
 */
static void test_transfer(void)
{
    QTestState *s = start();
    uint8_t pattern[64], expected[64];

    for (unsigned i = 0; i < sizeof(pattern); i++) {
        pattern[i] = i * 37 + 11;
    }
    qtest_memwrite(s, 0x12300, pattern, 64);
    reg(s, 0xe000, 2);
    descriptor(s, 0, 0, 0xa000, 0x12300, 32, 0, 1, 0x87f0);
    run(s, 0, 0, true);
    g_assert_cmphex(read_reg(s, 0x188) & 4, ==, 4);
    g_assert_cmphex(read_reg(s, 0x1a8), ==, 0);
    finish(s);
    check_bytes(s, BASE + 0x87f0, pattern, 32);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x219);
    g_assert_cmphex(read_reg(s, 0x160), ==, 1);
    descriptor(s, 1, 3, 0x8000, 0x24000, 16, 24, 2, 0x87f0);
    run(s, 1, 3, true);
    finish(s);
    check_bytes(s, 0x24000, pattern, 16);
    check_bytes(s, 0x24018, pattern + 16, 16);
    g_assert_cmphex(read_reg(s, 0x1c8), ==, 0x281);
    g_assert_cmphex(read_reg(s, 0x160), ==, 0x81);
    reg(s, 0x190, 0xa35c);
    descriptor(s, 0, 1, 0xb000, 0xdeadbeef, 16, 0, 2, 0x9000);
    run(s, 0, 1, false);
    finish(s);
    for (unsigned i = 0; i < 32; i++) {
        expected[i] = i & 1 ? 0x5c : 0xa3;
    }
    check_bytes(s, BASE + 0x9000, expected, 32);
    qtest_quit(s);
}

/*
 * Literal noncontiguous mappings catch page-index, tiling and permission
 * mistakes.
 */
static void test_translation(void)
{
    QTestState *s = start();
    uint8_t a[16], b[16];

    memset(a, 0xa7, sizeof(a));
    memset(b, 0x39, sizeof(b));
    qtest_memwrite(s, 0x2fff8, a, 8);
    qtest_memwrite(s, 0x50000, b, 8);
    reg(s, 0xf000, 0x20003);
    reg(s, 0xf008, 0x50003);
    descriptor(s, 0, 0, 0xa000, 0x80fff8, 16, 0, 1, 0x8000);
    run(s, 0, 0, true); /* Host permission absent: mapping still applies. */
    finish(s);
    check_bytes(s, BASE + 0x8000, a, 8);
    check_bytes(s, BASE + 0x8008, b, 8);
    qtest_memwrite(s, 0x201f8, a, 8);
    qtest_memwrite(s, 0x50000, b, 8);
    descriptor(s, 1, 0, 0xa000, 0x108001f8, 16, 0, 1, 0x8800);
    run(s, 1, 0, false);
    finish(s);
    check_bytes(s, BASE + 0x8800, a, 8);
    check_bytes(s, BASE + 0x8808, b, 8);
    reg(s, 0xf008, 0x50001);
    descriptor(s, 0, 0, 0x8000, 0x810000, 8, 0, 1, 0x8000);
    run(s, 0, 0, false);
    finish(s);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x812);
    g_assert_cmphex(read_reg(s, 0x1a8), ==, 8);
    g_assert_cmphex(read_reg(s, 0x160) & 2, ==, 2);
    qtest_quit(s);
}

/* Four descriptors must chain, wrap and skip without inventing a completion. */
static void test_chain(void)
{
    QTestState *s = start();

    reg(s, 0x190, 0x1234);
    descriptor(s, 0, 2, 0x4000, 0, 0, 0, 0, 0);
    descriptor(s, 0, 3, 0x3000, 0, 8, 0, 1, 0x8000);
    descriptor(s, 0, 0, 0xb000, 0, 8, 0, 1, 0x8008);
    run(s, 0, 2, false);
    finish(s);
    g_assert_cmphex(read_reg(s, 0x8000), ==, 0x1234123412341234ULL);
    g_assert_cmphex(read_reg(s, 0x8008), ==, 0x1234123412341234ULL);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x219);
    for (unsigned d = 0; d < 4; d++) {
        descriptor(s, 1, d, 0x4000, 0, 0, 0, 0, 0);
    }
    run(s, 1, 0, false);
    finish(s);
    g_assert_cmphex(read_reg(s, 0x1c8) & 7, ==, 4);
    qtest_quit(s);
}

/* Polling makes no progress; STOP drains only staging; reset discards it. */
static void test_boundaries(void)
{
    QTestState *s = start();
    uint8_t pattern[32];

    memset(pattern, 0x96, sizeof(pattern));
    qtest_memwrite(s, 0x12300, pattern, sizeof(pattern));
    reg(s, 0xe000, 2);
    descriptor(s, 0, 0, 0xa000, 0x12300, 32, 0, 1, 0x8000);
    run(s, 0, 0, true);
    for (unsigned i = 0; i < 20; i++) {
        g_assert_cmphex(read_reg(s, 0x8000), ==, 0);
    }
    qtest_clock_step(s, 17); /* First source fetch. */
    g_assert_cmphex(read_reg(s, 0x198), ==, 0x12300);
    g_assert_cmphex(read_reg(s, 0x1a8), ==, 32);
    g_assert_cmphex(read_reg(s, 0x8000), ==, 0);
    reg(s, 0x180, 0x117); /* STOP plus pending GO, unchanged fields. */
    finish(s);
    g_assert_cmphex(read_reg(s, 0x8000), ==, 0x9696969696969696ULL);
    g_assert_cmphex(read_reg(s, 0x8008), ==, 0);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x118);
    g_assert_cmphex(read_reg(s, 0x1a8), ==, 24);
    reg(s, 0x180, 8);
    qtest_clock_step(s, 267);
    reg(s, 0x180, 0);
    g_assert_cmphex(read_reg(s, 0x180), ==, 0x10);
    descriptor(s, 0, 0, 0xa000, 0x12300, 32, 0, 1, 0x8800);
    run(s, 0, 0, true);
    qtest_clock_step(s, 17);
    reg(s, 0x180, 8);
    finish(s);
    reg(s, 0x180, 0);
    g_assert_cmphex(read_reg(s, 0x8800), ==, 0);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x10);
    qtest_quit(s);
}

/* A short pulse must remain held, including fractional 60 MHz periods. */
static void test_reset_duration(void)
{
    QTestState *s = start();

    reg(s, 0x190, 0x5678);
    reg(s, 0x180, 8);
    qtest_clock_step(s, 250);
    g_assert_cmpint(memtx_store(s, BASE + 0x180, 0, 8), !=, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0x180), ==, 0x18);
    reg(s, 0x180, 8); /* Continuous assertion must retain its start. */
    qtest_clock_step(s, 16);
    g_assert_cmpint(memtx_store(s, BASE + 0x180, 0, 8), !=, MEMTX_OK);
    qtest_clock_step(s, 1);
    reg(s, 0x180, 0);
    g_assert_cmphex(read_reg(s, 0x180), ==, 0x10);
    g_assert_cmphex(read_reg(s, 0x190), ==, 0x5678);
    qtest_set_irq_in(s, PATH, "external-reset", 0, 1);
    qtest_clock_step(s, 266);
    qtest_set_irq_in(s, PATH, "external-reset", 0, 0);
    g_assert_cmpint(memtx_store(s, BASE + 0x180, 0x11, 8), !=, MEMTX_OK);
    qtest_clock_step(s, 1);
    qtest_set_irq_in(s, PATH, "external-reset", 0, 0);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x10);
    g_assert_cmphex(read_reg(s, 0x190), ==, 0);
    qtest_quit(s);
}

/*
 * Held requests must fail before register/data publication or fake
 * completion.
 */
static void test_unsupported(void)
{
    QTestState *s = start();
    const unsigned ports[] = { 0x10, 0x28, 0x30, 0x38, 0x40, 0x48,
        0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x110, 0x118, 0x120,
        0x128, 0x130, 0x138, 0x140, 0x148, 0x150, 0x158, 0x168,
        0x170, 0x178, 0x1b0, 0x1f0, 0x1f8, 0x7000 };
    uint64_t value;

    for (unsigned i = 0; i < G_N_ELEMENTS(ports); i++) {
        g_assert_cmpint(memtx_store(s, BASE + ports[i], 1, 8), !=, MEMTX_OK);
        g_assert_cmpint(memtx_load(s, BASE + ports[i], &value, 8),
                       !=, MEMTX_OK);
    }
    g_assert_cmpint(memtx_store(s, BASE + 8, 0xe2, 8), !=, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 8), ==, 0xe1);
    g_assert_cmpint(memtx_load(s, BASE + 0x80, &value, 8), !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, BASE + 0x87, 0, 1), !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, BASE + 0x180, 1, 8), !=, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x10);
    descriptor(s, 0, 0, 0xa000, 0x800000, 0, 0, 1, 0x8000);
    run(s, 0, 0, false);
    finish(s);
    g_assert_cmphex(read_reg(s, 0x188) & 3, ==, 0);
    g_assert_cmphex(read_reg(s, 0x8000), ==, 0);
    qtest_quit(s);
}


/* The first fault must preserve the committed prefix and unfinished count. */
static void test_late_fault(void)
{
    QTestState *s = start();
    const uint8_t prefix[] = { 0x2d, 0xa1, 0x17, 0x89, 0x36, 0xc2, 0x44, 0xff };
    const uint8_t zeros[8] = { 0 };

    qtest_memwrite(s, 0x2fff8, prefix, 8);
    reg(s, 0xf000, 0x20003);
    descriptor(s, 0, 0, 0xa000, 0x80fff8, 16, 0, 1, 0x8000);
    run(s, 0, 0, false);
    finish(s);
    check_bytes(s, BASE + 0x8000, prefix, 8);
    check_bytes(s, BASE + 0x8008, zeros, 8);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x71a);
    g_assert_cmphex(read_reg(s, 0x1a8), ==, 8);
    g_assert_cmphex(read_reg(s, 0x198), ==, 0x2fff8);
    g_assert_cmphex(read_reg(s, 0x1a0), ==, 0x8000);
    g_assert_cmphex(read_reg(s, 0x160), ==, 2);
    g_assert_cmpint(memtx_store(s, BASE + 0x180, 0x11, 8), !=, MEMTX_OK);
    qtest_quit(s);
}

/*
 * Concurrent fetches must observe ordered traffic, not two snapshots/memmove.
 */
static void test_concurrent(void)
{
    QTestState *s = start();
    const uint8_t first[] = { 0x19, 0x28, 0x37, 0x46, 0x55, 0x64, 0x73, 0x82 };
    const uint8_t old[] = { 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89 };

    qtest_memwrite(s, 0x12300, first, 8);
    qtest_memwrite(s, BASE + 0x8000, old, 8);
    reg(s, 0xe000, 2);
    descriptor(s, 0, 0, 0xa000, 0x12300, 8, 0, 1, 0x8000);
    descriptor(s, 1, 0, 0x8000, 0x12400, 8, 0, 1, 0x8000);
    run(s, 0, 0, true);
    run(s, 1, 0, true);
    qtest_clock_step(s, 17);
    g_assert_cmphex(read_reg(s, 0x198), ==, 0x12300);
    g_assert_cmphex(read_reg(s, 0x1d8), ==, 0);
    qtest_clock_step(s, 17);
    g_assert_cmphex(read_reg(s, 0x1e0), ==, 0x8000);
    check_bytes(s, BASE + 0x8000, old, 8);
    finish(s);
    check_bytes(s, BASE + 0x8000, first, 8);
    check_bytes(s, 0x12400, old, 8);
    g_assert_cmphex(read_reg(s, 0x160), ==, 0x81);
    qtest_quit(s);
}

/* Every linear index and tiled column/group selects independent backing. */
static void test_translation_indices(void)
{
    QTestState *s = start();

    reg(s, 0xe000, 2);
    for (unsigned i = 0; i < 64; i++) {
        reg(s, 0xf000 + i * 8, ((i + 1) << 16) | 3);
        qtest_writeq(s, ((i + 1) << 16) + 0x2400,
                     0xa718293a4b5c6d00ULL | i);
    }
    for (unsigned i = 0; i < 64; i++) {
        descriptor(s, 0, 0, 0xa000, 0x802400 + (i << 16),
                   8, 0, 1, 0x8000);
        run(s, 0, 0, false);
        finish(s);
        g_assert_cmphex(read_reg(s, 0x8000), ==, 0xa718293a4b5c6d00ULL | i);
        /*
         * Bits 18:12 = 0x12 give a fixed, independently selected offset
         * 0x2400.
         */
        descriptor(s, 1, 0, 0xa000,
                   0x10812000 + ((i / 8) << 19) + ((i % 8) << 9),
                   8, 0, 1, 0x8800);
        run(s, 1, 0, false);
        finish(s);
        g_assert_cmphex(read_reg(s, 0x8800), ==, 0xa718293a4b5c6d00ULL | i);
    }
    qtest_quit(s);
}

/* Clock-dependent admission must not reduce to a fixed nanosecond delay. */
static void test_other_clocks(void)
{
    const unsigned hz[] = { 50000000, 125000000, 70000000 };
    const unsigned hold[] = { 320, 128, 229 };

    for (unsigned i = 0; i < G_N_ELEMENTS(hz); i++) {
        QTestState *s = start_options(hz[i], true, "");

        reg(s, 0x180, 8);
        qtest_clock_step(s, hold[i] - 1);
        g_assert_cmpint(memtx_store(s, BASE + 0x180, 0, 8), !=, MEMTX_OK);
        qtest_clock_step(s, 1);
        reg(s, 0x180, 0);
        g_assert_cmphex(read_reg(s, 0x180), ==, 0x10);
        qtest_quit(s);
    }
    QTestState *s = start_options(60000000, false, "");
    reg(s, 0x180, 8);
    qtest_clock_step(s, 1000000000);
    g_assert_cmpint(memtx_store(s, BASE + 0x180, 0, 8), !=, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0x180), ==, 0x18);
    qtest_quit(s);
}

static void assert_refused(QTestState *s)
{
    g_autoptr(QDict) response = qtest_qmp_assert_success_ref(s,
        "{'execute':'query-status'}");

    g_assert_cmpstr(qdict_get_str(response, "status"), ==, "internal-error");
    g_assert_cmphex(read_reg(s, 0x188) & 3, ==, 0);
    g_assert_cmphex(read_reg(s, 0x160), ==, 0);
}

static void test_held_descriptors(void)
{
    static const struct {
        uint16_t flags, width, stride, lines, local;
        uint32_t system;
    } cases[] = {
        { 0xa001, 8, 0, 1, 0x8000, 0x800000 },
        { 0xa002, 8, 0, 1, 0x8000, 0x800000 },
        { 0xa004, 8, 0, 1, 0x8000, 0x800000 },
        { 0xa008, 8, 0, 1, 0x8000, 0x800000 },
        { 0xa080, 8, 0, 1, 0x8000, 0x800000 },
        { 0xa100, 8, 0, 1, 0x8000, 0x800000 },
        { 0xa200, 8, 0, 1, 0x8000, 0x800000 },
        { 0xa400, 8, 0, 1, 0x8000, 0x800000 },
        { 0xa800, 8, 0, 1, 0x8000, 0x800000 },
        { 0xc000, 8, 0, 1, 0x8000, 0x800000 },
        { 0x9000, 8, 0, 1, 0x8000, 0x800000 },
        { 0xb000, 7, 0, 1, 0x8000, 0x800000 },
        { 0xa000, 0, 0, 1, 0x8000, 0x800000 },
        { 0xa000, 8, 0, 0, 0x8000, 0x800000 },
        { 0xa000, 32768, 0, 2, 0x8000, 0x800000 },
        { 0xa000, 8, 0x8000, 2, 0x8000, 0x800000 },
        { 0xa000, 8, 0, 1, 0x8001, 0x800000 },
        { 0xa000, 8, 0, 1, 0x7000, 0x800000 },
        { 0xa000, 8, 0, 1, 0x1000, 0x800000 },
        { 0xa000, 8, 0, 1, 0xf000, 0x800000 },
        { 0xa000, 8, 0, 1, 0x9800, 0x800000 },
        { 0xa000, 4, 0, 1, 0x2000, 0x800000 },
        { 0xa000, 8, 0, 1, 0x2000, 0x800004 },
        { 0xa000, 3, 0, 1, 0x4000, 0x800000 },
        { 0xa000, 6, 0, 1, 0x5000, 0x800000 },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        QTestState *s = start();

        descriptor(s, 0, 0, cases[i].flags, cases[i].system,
                   cases[i].width, cases[i].stride, cases[i].lines,
                   cases[i].local);
        run(s, 0, 0, false);
        finish(s);
        assert_refused(s);
        g_assert_cmphex(read_reg(s, 0x8000), ==, 0);
        qtest_quit(s);
    }
}

/*
 * Save before fetch, with a staged beat, after commit and while reset is
 * held.
 */
static void test_migration(void)
{
    for (unsigned phase = 0; phase < 8; phase++) {
        g_autofree char *directory = g_strdup("/tmp/vice-migration-XXXXXX");
        g_autofree char *socket = NULL;
        g_autofree char *uri = NULL;
        QTestState *from, *to;

        g_assert_nonnull(g_mkdtemp(directory));
        socket = g_build_filename(directory, "socket", NULL);
        uri = g_strdup_printf("unix:%s", socket);
        from = start();
        to = start_options(60000000, true, "-incoming defer");
        reg(from, 0x190, 0x3ab7);
        if (phase == 5 || phase == 6) {
            reg(from, 0xe000, 2);
            reg(from, 0x8000, 0x0123456789abcdefULL);
            reg(from, 0x8008, 0x2468ace013579bdfULL);
            reg(from, 0x8010, 0x1122334455667788ULL);
            descriptor(from, 0, 0, 0x8000, 0x10000, 24, 0, 1, 0x8000);
            run(from, 0, 0, true);
            qtest_clock_step(from, phase == 5 ? 17 : 34);
        } else {
            descriptor(from, 0, 0, 0xb000, 0, 24, 0, 1, 0x8000);
            run(from, 0, 0, false);
        }
        if (phase == 7) {
            finish(from);
            reg(from, 0xe010, 1);
        } else if (phase < 3) {
            if (phase) {
                qtest_clock_step(from, phase == 1 ? 17 : 34);
            }
        } else if (phase < 5) {
            reg(from, 0x180, 8);
            qtest_clock_step(from, 125);
            if (phase == 4) {
                qtest_set_irq_in(from, PATH, "external-reset", 0, 1);
                qtest_clock_step(from, 125);
            }
        }
        qtest_irq_intercept_out_named(to, PATH, "sysbus-irq");
        qtest_qmp_assert_success(from, "{'execute':'stop'}");
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        g_assert_cmphex(read_reg(to, 0x188), ==, read_reg(from, 0x188));
        g_assert_cmphex(read_reg(to, 0x1a8), ==, read_reg(from, 0x1a8));
        g_assert_cmphex(read_reg(to, 0x8000), ==, read_reg(from, 0x8000));
        g_assert_cmphex(read_reg(to, 0x108), ==, read_reg(from, 0x108));
        g_assert_cmphex(read_reg(to, 0x160), ==, read_reg(from, 0x160));
        qtest_clock_timers(to, true);
        if (phase < 3) {
            finish(to);
            g_assert_cmphex(read_reg(to, 0x8000), ==, 0x3ab73ab73ab73ab7ULL);
            g_assert_cmphex(read_reg(to, 0x8010), ==, 0x3ab73ab73ab73ab7ULL);
            g_assert_cmphex(read_reg(to, 0x188), ==, 0x219);
            reg(to, 0xe008, 1);
            finish(to);
            g_assert_cmphex(read_reg(to, 0x160), ==, 0);
        } else if (phase == 3) {
            qtest_clock_step(to, 141);
            g_assert_cmpint(memtx_store(to, BASE + 0x180, 0, 8), !=, MEMTX_OK);
            qtest_clock_step(to, 1);
            reg(to, 0x180, 0);
            g_assert_cmphex(read_reg(to, 0x180), ==, 0x10);
            g_assert_cmphex(read_reg(to, 0x8000), ==, 0);
        } else if (phase == 4) {
            qtest_clock_step(to, 141);
            qtest_set_irq_in(to, PATH, "external-reset", 0, 0);
            g_assert_cmpint(memtx_store(to, BASE + 0x180, 0x11, 8),
                           !=, MEMTX_OK);
            qtest_clock_step(to, 1);
            qtest_set_irq_in(to, PATH, "external-reset", 0, 0);
            g_assert_cmphex(read_reg(to, 0x8000), ==, 0);
        }
        if (phase == 5 || phase == 6) {
            reg(to, 0x8000, 0xffffffffffffffffULL);
            if (phase == 6) {
                qtest_writeq(to, 0x10000, 0xdeadbeef09876543ULL);
            }
            finish(to);
            g_assert_cmphex(qtest_readq(to, 0x10000), ==,
                phase == 5 ? 0x0123456789abcdefULL : 0xdeadbeef09876543ULL);
            g_assert_cmphex(qtest_readq(to, 0x10008), ==,
                            0x2468ace013579bdfULL);
            g_assert_cmphex(qtest_readq(to, 0x10010), ==,
                            0x1122334455667788ULL);
            g_assert_cmphex(read_reg(to, 0x188), ==, 0x211);
        }
        if (phase == 7) {
            g_assert_true(qtest_get_irq(to, 0));
            g_assert_cmpuint(qtest_get_irq_raise_count(to, 0), ==, 1);
            reg(to, 0xe008, 1);
            finish(to);
            g_assert_false(qtest_get_irq(to, 0));
            g_assert_cmpuint(qtest_get_irq_raise_count(to, 0), ==, 1);
        }
        qtest_quit(from);
        qtest_quit(to);
        g_unlink(socket);
        g_rmdir(directory);
    }
}


static void test_local_dma(void)
{
    QTestState *s = start();
    const unsigned local[] = { 0x2000, 0x4000, 0x5000 };
    const uint8_t bytes[] = { 0x91, 0xab, 0x37, 0xcd, 0x28, 0x76, 0xf0, 0x42,
                              0x19, 0x8e, 0x4c, 0x62, 0xde, 0x35, 0xa9, 0x07 };
    uint8_t masked[16];

    memcpy(masked, bytes, sizeof(masked));
    for (unsigned i = 0; i < 16; i += 4) {
        masked[i] = 0;
        masked[i + 1] &= 0x3f;
    }
    reg(s, 0xe000, 2);
    qtest_memwrite(s, 0x10000, bytes, sizeof(bytes));
    for (unsigned i = 0; i < G_N_ELEMENTS(local); i++) {
        descriptor(s, 0, 0, 0xa000, 0x10000, 16, 0, 1, local[i]);
        run(s, 0, 0, true);
        finish(s);
        check_bytes(s, BASE + local[i], i == 2 ? masked : bytes, 16);
        descriptor(s, 1, 0, 0x8000, 0x20000 + i * 16, 16, 0, 1, local[i]);
        run(s, 1, 0, true);
        finish(s);
        check_bytes(s, 0x20000 + i * 16, i == 2 ? masked : bytes, 16);
    }
    qtest_quit(s);
}

static void test_permission_changes(void)
{
    const uint8_t data[] = { 0xc7, 0xa3, 0x59, 0x21, 0x64, 0x18, 0xdb, 0xfe };
    const uint8_t zeros[8] = { 0 };

    for (unsigned write = 0; write < 2; write++) {
        QTestState *s = start();

        reg(s, 0xe000, 2);
        qtest_memwrite(s, write ? BASE + 0x8000 : 0x10000, data, 8);
        descriptor(s, 0, 0, write ? 0x8000 : 0xa000, 0x10000,
                   16, 0, 1, 0x8000);
        run(s, 0, 0, true);
        qtest_clock_step(s, 17);
        reg(s, 0xe000, 0);
        finish(s);
        g_assert_cmphex(read_reg(s, 0x188) & 0xf03, ==, 0x702);
        g_assert_cmphex(read_reg(s, 0x1a8), ==, write ? 16 : 8);
        check_bytes(s, write ? 0x10000 : BASE + 0x8000,
                    write ? zeros : data, 8);
        qtest_quit(s);
    }
}

static void test_completion_enable(void)
{
    QTestState *s = start();

    reg(s, 0x190, 0x1234);
    descriptor(s, 0, 0, 0xb000, 0, 8, 0, 1, 0x8000);
    run(s, 0, 0, false);
    qtest_clock_step(s, 17);
    reg(s, 0x180, 0x10); /* GO zero preserves work, IE zero masks this event. */
    finish(s);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x219);
    g_assert_cmphex(read_reg(s, 0x160), ==, 0);
    run(s, 0, 0, false);
    finish(s);
    g_assert_cmphex(read_reg(s, 0x160), ==, 1);
    qtest_quit(s);
}

/* Padding stores must not become control commands or publish reserved bits. */
static void test_padding(void)
{
    QTestState *s = start();

    reg(s, 0xe000, 0xdeadbeef00000002ULL);
    g_assert_cmphex(read_reg(s, 0xe000), ==, 2);
    g_assert_cmpint(memtx_store(s, BASE + 0xe000, 0xcafebabe, 4), ==, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0xe000), ==, 2);
    reg(s, 0x180, 0xa5a5a5a500000010ULL);
    g_assert_cmphex(read_reg(s, 0x180), ==, 0x10);
    qtest_quit(s);
}

/* Recovering from a short release must leave a later distinct GO usable. */
static void test_reset_restart(void)
{
    QTestState *s = start();

    descriptor(s, 0, 0, 0xb000, 0, 8, 0, 1, 0x8000);
    reg(s, 0x190, 0x1234);
    reg(s, 0x180, 8);
    qtest_clock_step(s, 266);
    g_assert_cmpint(memtx_store(s, BASE + 0x180, 0, 8), !=, MEMTX_OK);
    qtest_clock_step(s, 1);
    reg(s, 0x180, 0);
    qtest_clock_timers(s, true);
    run(s, 0, 0, false);
    finish(s);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x219);
    g_assert_cmphex(read_reg(s, 0x8000), ==, 0x1234123412341234ULL);
    qtest_quit(s);
}


static void test_ram_views(void)
{
    if (!qtest_has_device("sgi-dma-word-test")) {
        g_test_skip("Existing alias/MMIO fixture is absent from this build");
        return;
    }
    const uint32_t system[] = { 0x40002900, 0x4000497c, 0x4000897c,
                                0x01fffffc };
    const uint8_t bytes[] = { 0x21, 0x43, 0x65, 0x87, 0xa9, 0xcb, 0xed, 0x0f };

    for (unsigned i = 0; i < G_N_ELEMENTS(system); i++) {
        QTestState *s = start_options(60000000, true,
                                      "-device sgi-dma-word-test,id=views");

        qtest_memwrite(s, BASE + 0x8000, bytes, 8);
        reg(s, 0xe000, 2);
        descriptor(s, 0, 0, 0x8000, system[i], 8, 0, 1, 0x8000);
        run(s, 0, 0, true);
        finish(s);
        assert_refused(s);
        g_assert_cmphex(read_reg(s, 0x1a8), ==,
                        i == 1 ? 6 : i == 3 ? 4 : 8);
        if (i == 1) {
            g_assert_cmphex(qtest_readw(s, 0x5000097c), ==, 0x2143);
            g_assert_cmphex(qtest_readw(s, 0x5000097e), ==, 0);
        }
        if (i == 2) {
            g_assert_cmphex(qtest_readl(s, 0x30000014), ==, 0);
            g_assert_cmphex(qtest_readl(s, 0x30000018), ==, 0);
        }
        if (i == 3) {
            g_assert_cmphex(qtest_readl(s, 0x01fffffc), ==, 0x21436587);
        }
        qtest_quit(s);
    }
}

static void test_fill_progress(void)
{
    QTestState *s = start();

    reg(s, 0x190, 0x6543);
    descriptor(s, 0, 0, 0xb000, 0, 16, 0, 1, 0x8000);
    run(s, 0, 0, false);
    qtest_clock_step(s, 17);
    g_assert_cmphex(read_reg(s, 0x1a0), ==, 0);
    g_assert_cmphex(read_reg(s, 0x1a8), ==, 16);
    qtest_clock_step(s, 17);
    g_assert_cmphex(read_reg(s, 0x1a0), ==, 0x8000);
    g_assert_cmphex(read_reg(s, 0x1a8), ==, 8);
    qtest_quit(s);
}


static void test_descriptor_edits(void)
{
    QTestState *s = start();

    reg(s, 0x190, 0xa1b2);
    descriptor(s, 0, 0, 0xb000, 0, 16, 0, 1, 0x8000);
    run(s, 0, 0, false);
    /* A descriptor is latched at fetch, not at GO. */
    reg(s, 0x1030, 0x8800);
    qtest_clock_step(s, 17);
    reg(s, 0x1040, 0x4000); /* A different descriptor remains writable. */
    g_assert_cmpint(memtx_store(s, BASE + 0x1030, 0x9000, 8), !=, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0x1030), ==, 0x8800);
    g_assert_cmphex(read_reg(s, 0x1040), ==, 0x4000);
    g_assert_cmphex(read_reg(s, 0x8800), ==, 0);
    qtest_quit(s);
}

static void test_stride(void)
{
    QTestState *s = start();
    uint8_t bytes[24];

    for (unsigned i = 0; i < sizeof(bytes); i++) {
        bytes[i] = i * 19 + 3;
    }
    reg(s, 0xe000, 2);
    qtest_memwrite(s, 0x12301, bytes, sizeof(bytes));
    descriptor(s, 0, 0, 0xa000, 0x12301, 8, 4, 3, 0x8000);
    run(s, 0, 0, true);
    finish(s);
    check_bytes(s, BASE + 0x8000, bytes, 8);
    check_bytes(s, BASE + 0x8008, bytes + 4, 8);
    check_bytes(s, BASE + 0x8010, bytes + 8, 8);
    g_assert_cmphex(read_reg(s, 0x198), ==, 0x12308);
    descriptor(s, 1, 0, 0xa000, 0x12301, 8, 0xffff, 1, 0x8800);
    run(s, 1, 0, true);
    finish(s);
    check_bytes(s, BASE + 0x8800, bytes, 8);
    qtest_quit(s);
}

static void test_padding_pending(void)
{
    QTestState *s = start();

    reg(s, 0x190, 0xabcd);
    descriptor(s, 0, 0, 0xb000, 0, 16, 0, 1, 0x8000);
    run(s, 0, 0, false);
    qtest_clock_step(s, 17);
    reg(s, 0x180, 0x17);
    finish(s);
    g_assert_cmpint(memtx_store(s, BASE + 0x180, 0xa5a5a5a5, 4),
                   ==, MEMTX_OK);
    g_assert_cmphex(read_reg(s, 0x180), ==, 0x17);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x118);
    qtest_quit(s);
}


static void test_address_bounds(void)
{
    const uint32_t invalid[] = { 0x007ffff8, 0x00c00000, 0x107ffff8,
                                 0x10c00000, 0xfffffff8 };

    for (unsigned i = 0; i < G_N_ELEMENTS(invalid); i++) {
        QTestState *s = start();
        unsigned channel = i & 1;
        unsigned offset = channel * 0x40;

        descriptor(s, channel, 0, 0xa000, invalid[i], 8, 0, 1, 0x8000);
        run(s, channel, 0, false);
        finish(s);
        g_assert_cmphex(read_reg(s, 0x188 + offset) & 0xf03, ==, 0x702);
        g_assert_cmphex(read_reg(s, 0x1a8 + offset), ==, 8);
        g_assert_cmphex(read_reg(s, 0x160), ==, channel ? 0x100 : 2);
        qtest_quit(s);
    }
    for (unsigned tiled = 0; tiled < 2; tiled++) {
        QTestState *s = start();
        const uint8_t bytes[] = { 0x91, 0x78, 0x65, 0x43,
                                  0x21, 0x0f, 0xed, 0xcb };

        reg(s, 0xf1f8, 0x20003);
        qtest_memwrite(s, 0x2fff8, bytes, 8);
        descriptor(s, 0, 0, 0xa000, tiled ? 0x10bffff8 : 0x00bffff8,
                   16, 0, 1, 0x8000);
        run(s, 0, 0, false);
        finish(s);
        check_bytes(s, BASE + 0x8000, bytes, 8);
        g_assert_cmphex(read_reg(s, 0x188), ==, 0x71a);
        g_assert_cmphex(read_reg(s, 0x1a8), ==, 8);
        qtest_quit(s);
    }
}

static void test_read_only(void)
{
    QTestState *s = start();
    const unsigned registers[] = { 8, 0x108, 0x188, 0x198, 0x1a0, 0x1a8,
                                   0x1c8, 0x1d8, 0x1e0, 0x1e8 };

    for (unsigned i = 0; i < G_N_ELEMENTS(registers); i++) {
        uint64_t before = read_reg(s, registers[i]);

        g_assert_cmpint(memtx_store(s, BASE + registers[i], 0xa5a5, 8),
                       !=, MEMTX_OK);
        g_assert_cmphex(read_reg(s, registers[i]), ==, before);
    }
    qtest_quit(s);
}

static void test_counter(void)
{
    QTestState *s = start();

    g_assert_cmphex(read_reg(s, 0x108), ==, 0);
    qtest_clock_step(s, 15);
    g_assert_cmphex(read_reg(s, 0x108), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(read_reg(s, 0x108), ==, 1);
    qtest_clock_step(s, 84);
    g_assert_cmphex(read_reg(s, 0x108), ==, 6);
    qtest_set_irq_in(s, PATH, "external-reset", 0, 1);
    g_assert_cmphex(read_reg(s, 0x108), ==, 0);
    qtest_clock_step(s, 100);
    g_assert_cmphex(read_reg(s, 0x108), ==, 6);
    qtest_quit(s);
}


/* Resetting the peer must not execute an unsupported descriptor again. */
static void test_peer_reset(void)
{
    QTestState *s = start();

    descriptor(s, 0, 0, 0xa000, 0x800000, 0, 0, 1, 0x8000);
    descriptor(s, 1, 0, 0xb000, 0, 8, 0, 1, 0x8800);
    run(s, 0, 0, false);
    finish(s);
    assert_refused(s);
    reg(s, 0x1c0, 8);
    qtest_clock_step(s, 267);
    reg(s, 0x1c0, 0);
    reg(s, 0x1d0, 0xabcd);
    run(s, 1, 0, false);
    qtest_clock_timers(s, true);
    finish(s);
    g_assert_cmphex(read_reg(s, 0x1c8), ==, 0x219);
    g_assert_cmphex(read_reg(s, 0x8800), ==, 0xabcdabcdabcdabcdULL);
    g_assert_cmphex(read_reg(s, 0x188) & 3, ==, 0);
    g_assert_cmphex(read_reg(s, 0x8000), ==, 0);
    qtest_quit(s);
}


static void test_skip_progress(void)
{
    QTestState *s = start();

    for (unsigned i = 0; i < 4; i++) {
        descriptor(s, 0, i, 0x4000, 0, 0, 0, 0, 0);
    }
    run(s, 0, 2, false);
    qtest_clock_step(s, 17);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x384);
    qtest_clock_step(s, 17);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x314);
    g_assert_cmphex(read_reg(s, 0x160), ==, 0);
    qtest_quit(s);
}


#define SPLIT_MEMORY (SGI_DMA_WORD_TEST_MEMORY + \
    SGI_DMA_WORD_TEST_SPLIT_EIGHT * SGI_DMA_WORD_TEST_STRIDE)

static const uint8_t typed_pattern[16] = {
    0x91, 0xab, 0x37, 0xcd, 0x28, 0x76, 0xf0, 0x42,
    0x19, 0x8e, 0x4c, 0x62, 0xde, 0x35, 0xa9, 0x07
};

static QTestState *start_split_options(const char *extra)
{
    g_autofree char *options = NULL;
    QTestState *s;

    if (!qtest_has_device("sgi-dma-word-test")) {
        g_test_skip("Existing alias/MMIO fixture is absent from this build");
        return NULL;
    }
    options = g_strdup_printf("-device sgi-dma-word-test,id=views %s", extra);
    s = start_options(60000000, true, options);
    reg(s, 0xe000, 2);
    return s;
}

static QTestState *start_split(void)
{
    return start_split_options("");
}

static void split_source(QTestState *s)
{
    qtest_memwrite(s, SGI_DMA_WORD_TEST_BACKING + SGI_DMA_WORD_TEST_SPLIT_FIRST,
                   typed_pattern, 2);
    qtest_memwrite(s, SGI_DMA_WORD_TEST_BACKING +
                   SGI_DMA_WORD_TEST_SPLIT_SECOND, typed_pattern + 2, 6);
    qtest_memwrite(s, SGI_DMA_WORD_TEST_BACKING + 8, typed_pattern + 8, 8);
}

static void test_typed_assembly(void)
{
    QTestState *s = start_split();
    const uint8_t zeros[16] = { 0 };

    if (!s) {
        return;
    }
    split_source(s);
    descriptor(s, 0, 0, 0xa000, SPLIT_MEMORY, 16, 0, 1, 0x2000);
    run(s, 0, 0, true);
    qtest_clock_step(s, 17);
    check_bytes(s, BASE + 0x2000, zeros, 16);
    g_assert_cmphex(read_reg(s, 0x1a8), ==, 16);
    qtest_clock_step(s, 17);
    check_bytes(s, BASE + 0x2000, zeros, 16);
    g_assert_cmphex(read_reg(s, 0x1a8), ==, 16);
    qtest_clock_step(s, 17);
    check_bytes(s, BASE + 0x2000, typed_pattern, 8);
    check_bytes(s, BASE + 0x2008, zeros, 8);
    g_assert_cmphex(read_reg(s, 0x1a8), ==, 8);
    finish(s);
    check_bytes(s, BASE + 0x2000, typed_pattern, 16);
    g_assert_cmphex(read_reg(s, 0x188), ==, 0x219);
    qtest_quit(s);
}

static void test_typed_revocation(void)
{
    const uint8_t zeros[16] = { 0 };

    for (unsigned mode = 0; mode < 4; mode++) {
        QTestState *s = start_split();
        uint32_t system = mode < 2 ? SPLIT_MEMORY : 0x00806000;
        bool write = mode & 1;

        if (!s) {
            return;
        }
        reg(s, 0xf000, 0x40010003);
        if (write) {
            reg(s, 0x2000, 0x91ab37cd2876f042ULL);
            reg(s, 0x2008, 0x198e4c62de35a907ULL);
        } else {
            split_source(s);
        }
        descriptor(s, 0, 0, write ? 0x8000 : 0xa000,
                   system, 16, 0, 1, 0x2000);
        run(s, 0, 0, mode < 2);
        qtest_clock_step(s, 17);
        if (write) {
            /* First two destination bytes committed. */
            qtest_clock_step(s, 17);
        }
        if (mode < 2) {
            reg(s, 0xe000, 0);
        } else {
            reg(s, 0xf000, 0);
        }
        finish(s);
        g_assert_cmphex(read_reg(s, 0x188) & 0xf03, ==, 0x702);
        g_assert_cmphex(read_reg(s, 0x1a8), ==, write ? 14 : 16);
        if (write) {
            check_bytes(s, SGI_DMA_WORD_TEST_BACKING +
                        SGI_DMA_WORD_TEST_SPLIT_FIRST, typed_pattern, 2);
            check_bytes(s, SGI_DMA_WORD_TEST_BACKING +
                        SGI_DMA_WORD_TEST_SPLIT_SECOND, zeros, 6);
        } else {
            check_bytes(s, BASE + 0x2000, zeros, 16);
        }
        qtest_quit(s);
    }
}


static void test_typed_stop_reset(void)
{
    const uint8_t zeros[16] = { 0 };

    for (unsigned reset = 0; reset < 2; reset++) {
        QTestState *s = start_split();

        if (!s) {
            return;
        }
        split_source(s);
        descriptor(s, 0, 0, 0xa000, SPLIT_MEMORY, 16, 0, 1, 0x2000);
        run(s, 0, 0, true);
        qtest_clock_step(s, 17);
        reg(s, 0x180, reset ? 8 : 0x117);
        finish(s);
        if (reset) {
            reg(s, 0x180, 0);
            check_bytes(s, BASE + 0x2000, zeros, 16);
            g_assert_cmphex(read_reg(s, 0x188), ==, 0x10);
        } else {
            check_bytes(s, BASE + 0x2000, typed_pattern, 8);
            check_bytes(s, BASE + 0x2008, zeros, 8);
            g_assert_cmphex(read_reg(s, 0x188), ==, 0x118);
            g_assert_cmphex(read_reg(s, 0x1a8), ==, 8);
            g_assert_cmphex(read_reg(s, 0x160), ==, 0);
        }
        qtest_quit(s);
    }
}

static void test_typed_migration(void)
{
    g_autofree char *directory = g_strdup("/tmp/vice-migration-XXXXXX");
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = start_split();
    QTestState *to;
    const uint8_t zeros[16] = { 0 };

    if (!from) {
        return;
    }
    g_assert_nonnull(g_mkdtemp(directory));
    socket = g_build_filename(directory, "socket", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    to = start_split_options("-incoming defer");
    split_source(from);
    /* This fixture's static RAM is nomigrate; preload the identical input. */
    split_source(to);
    descriptor(from, 0, 0, 0xa000, SPLIT_MEMORY, 16, 0, 1, 0x2000);
    run(from, 0, 0, true);
    qtest_clock_step(from, 17);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    check_bytes(to, BASE + 0x2000, zeros, 16);
    qtest_writew(to, SGI_DMA_WORD_TEST_BACKING + SGI_DMA_WORD_TEST_SPLIT_FIRST,
                 0xffff);
    qtest_clock_timers(to, true);
    finish(to);
    check_bytes(to, BASE + 0x2000, typed_pattern, 16);
    g_assert_cmphex(read_reg(to, 0x188), ==, 0x219);
    g_assert_cmphex(read_reg(to, 0x160), ==, 1);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_rmdir(directory);
}


static void test_typed_staged_grant(void)
{
    const uint8_t zeros[8] = { 0 };

    for (unsigned mapped = 0; mapped < 2; mapped++) {
        QTestState *s = start_split();

        if (!s) {
            return;
        }
        split_source(s);
        reg(s, 0xf000, 0x40010003);
        descriptor(s, 0, 0, 0xa000, mapped ? 0x00806000 : SPLIT_MEMORY,
                   16, 0, 1, 0x2000);
        run(s, 0, 0, !mapped);
        qtest_clock_step(s, 34); /* Both source subrequests accepted. */
        if (mapped) {
            reg(s, 0xf000, 0);
        } else {
            reg(s, 0xe000, 0);
        }
        finish(s);
        check_bytes(s, BASE + 0x2000, typed_pattern, 8);
        check_bytes(s, BASE + 0x2008, zeros, 8);
        g_assert_cmphex(read_reg(s, 0x1a8), ==, 8);
        g_assert_cmphex(read_reg(s, 0x188), ==, 0x71a);
        qtest_quit(s);
    }
}

static void test_typed_remap(void)
{
    const uint8_t replacement[14] = {
        0x31, 0x52, 0x73, 0x94, 0xb5, 0xd6,
        0x18, 0x39, 0x5a, 0x7b, 0x9c, 0xbd, 0xde, 0xff
    };
    const uint8_t zeros[6] = { 0 };

    for (unsigned mode = 0; mode < 3; mode++) {
        QTestState *s = start_split();
        bool write = mode == 2;

        if (!s) {
            return;
        }
        reg(s, 0xf000, 0x40010003);
        if (write) {
            reg(s, 0x2000, 0x91ab37cd2876f042ULL);
            reg(s, 0x2008, 0x198e4c62de35a907ULL);
        } else {
            split_source(s);
            qtest_memwrite(s, 0x36002, replacement, sizeof(replacement));
            check_bytes(s, 0x36002, replacement, sizeof(replacement));
        }
        descriptor(s, 1, 0, write ? 0x8000 : 0xa000,
                   0x00806000, 16, 0, 1, 0x2000);
        run(s, 1, 0, false);
        qtest_clock_step(s, mode ? 34 : 17);
        reg(s, 0xf000, 0x00030003);
        g_assert_cmphex(read_reg(s, 0xf000), ==, 0x30003);
        if (write) {
            reg(s, 0x2000, UINT64_MAX); /* Already staged bytes stay intact. */
        }
        finish(s);
        if (write) {
            check_bytes(s, SGI_DMA_WORD_TEST_BACKING +
                        SGI_DMA_WORD_TEST_SPLIT_FIRST, typed_pattern, 2);
            check_bytes(s, SGI_DMA_WORD_TEST_BACKING +
                        SGI_DMA_WORD_TEST_SPLIT_SECOND, zeros, 6);
            check_bytes(s, 0x36002, typed_pattern + 2, 14);
        } else {
            unsigned accepted = mode ? 8 : 2;

            check_bytes(s, BASE + 0x2000, typed_pattern, accepted);
            check_bytes(s, BASE + 0x2000 + accepted,
                        replacement + accepted - 2, 16 - accepted);
        }
        g_assert_cmphex(read_reg(s, 0x1c8), ==, write ? 0x211 : 0x219);
        qtest_quit(s);
    }
}

static void test_typed_small_units(void)
{
    const uint8_t masked[16] = {
        0, 0x2b, 0x37, 0xcd, 0, 0x36, 0xf0, 0x42,
        0, 0x0e, 0x4c, 0x62, 0, 0x35, 0xa9, 0x07
    };
    const uint8_t zeros[16] = { 0 };

    for (unsigned table = 0; table < 2; table++) {
        QTestState *s = start_split();
        unsigned local = table ? 0x5000 : 0x4000;
        const uint8_t *expected = table ? masked : typed_pattern;

        if (!s) {
            return;
        }
        split_source(s);
        descriptor(s, 1, 0, 0xa000, SPLIT_MEMORY, 16, 0, 1, local);
        run(s, 1, 0, true);
        qtest_clock_step(s, 34);
        if (table) {
            check_bytes(s, BASE + local, zeros, 16);
            g_assert_cmphex(read_reg(s, 0x1e8), ==, 16);
        } else {
            check_bytes(s, BASE + local, expected, 2);
            check_bytes(s, BASE + local + 2, zeros, 14);
            g_assert_cmphex(read_reg(s, 0x1e8), ==, 14);
        }
        finish(s);
        check_bytes(s, BASE + local, expected, 16);
        qtest_memset(s, SGI_DMA_WORD_TEST_BACKING, 0, 0x2000);
        descriptor(s, 0, 0, 0x8000, SPLIT_MEMORY, 16, 0, 1, local);
        run(s, 0, 0, true);
        finish(s);
        check_bytes(s, SGI_DMA_WORD_TEST_BACKING +
                    SGI_DMA_WORD_TEST_SPLIT_FIRST, expected, 2);
        check_bytes(s, SGI_DMA_WORD_TEST_BACKING +
                    SGI_DMA_WORD_TEST_SPLIT_SECOND, expected + 2, 6);
        check_bytes(s, SGI_DMA_WORD_TEST_BACKING + 8, expected + 8, 8);
        g_assert_cmphex(read_reg(s, 0x188), ==, 0x211);
        qtest_quit(s);
    }
}


static void test_typed_write_migration(void)
{
    g_autofree char *directory = g_strdup("/tmp/vice-migration-XXXXXX");
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = start_split();
    QTestState *to;
    const uint8_t guard[2] = { 0xee, 0xff };

    if (!from) {
        return;
    }
    g_assert_nonnull(g_mkdtemp(directory));
    socket = g_build_filename(directory, "socket", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    to = start_split_options("-incoming defer");
    reg(from, 0x2000, 0x91ab37cd2876f042ULL);
    reg(from, 0x2008, 0x198e4c62de35a907ULL);
    descriptor(from, 0, 0, 0x8000, SPLIT_MEMORY, 16, 0, 1, 0x2000);
    run(from, 0, 0, true);
    qtest_clock_step(from, 34);
    g_assert_cmphex(read_reg(from, 0x1a8), ==, 14);
    check_bytes(from, SGI_DMA_WORD_TEST_BACKING +
                SGI_DMA_WORD_TEST_SPLIT_FIRST, typed_pattern, 2);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(read_reg(to, 0x1a8), ==, 14);
    /* External test traffic replaces a prefix that must never be replayed. */
    qtest_memwrite(to, SGI_DMA_WORD_TEST_BACKING +
                   SGI_DMA_WORD_TEST_SPLIT_FIRST, guard, sizeof(guard));
    reg(to, 0x2000, UINT64_MAX);
    qtest_clock_timers(to, true);
    finish(to);
    check_bytes(to, SGI_DMA_WORD_TEST_BACKING +
                SGI_DMA_WORD_TEST_SPLIT_FIRST, guard, sizeof(guard));
    check_bytes(to, SGI_DMA_WORD_TEST_BACKING +
                SGI_DMA_WORD_TEST_SPLIT_SECOND, typed_pattern + 2, 6);
    check_bytes(to, SGI_DMA_WORD_TEST_BACKING + 8, typed_pattern + 8, 8);
    g_assert_cmphex(read_reg(to, 0x188), ==, 0x211);
    g_assert_cmphex(read_reg(to, 0x160), ==, 1);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_rmdir(directory);
}


/* System byte placement is independent of BSP/table local publication units. */
static void test_odd_system(void)
{
    const uint8_t masked[8] = { 0, 0x2b, 0x37, 0xcd,
                                0, 0x36, 0xf0, 0x42 };

    for (unsigned table = 0; table < 2; table++) {
        for (unsigned write = 0; write < 2; write++) {
            for (unsigned odd = 0; odd < 2; odd++) {
                QTestState *s = start();
                unsigned unit = table ? 4 : 2;
                unsigned local = table ? 0x5080 : 0x4080;
                unsigned channel = table;
                unsigned offset = channel * 0x40;
                unsigned system = 0x10000 + odd;
                uint8_t expected[48];
                const uint8_t *data = table ? masked : typed_pattern;
                uint64_t before, after, source;

                memset(expected, 0xcc, sizeof(expected));
                qtest_memwrite(s, 0xfff8, expected, sizeof(expected));
                reg(s, 0xe000, 2);
                reg(s, local - 8, UINT64_C(0x1122334455667788));
                reg(s, local + 8, UINT64_C(0x89abcdef01234567));
                if (write) {
                    reg(s, local, UINT64_C(0x91ab37cd2876f042));
                } else {
                    for (unsigned row = 0; row < 2; row++) {
                        qtest_memwrite(s, system + row * (unit + 1),
                                       typed_pattern + row * unit, unit);
                    }
                }
                before = read_reg(s, local - 8);
                after = read_reg(s, local + 8);
                source = read_reg(s, local);
                descriptor(s, channel, 0, write ? 0x8000 : 0xa000,
                           system, unit, unit + 1, 2, local);
                run(s, channel, 0, true);
                finish(s);
                g_assert_cmphex(read_reg(s, 0x188 + offset), ==,
                                write ? 0x211 : 0x219);
                g_assert_cmphex(read_reg(s, 0x1a8 + offset), ==, 0);
                g_assert_cmphex(read_reg(s, 0x160), ==, channel ? 0x80 : 1);
                check_bytes(s, BASE + local, data, 2 * unit);
                g_assert_cmphex(read_reg(s, local - 8), ==, before);
                g_assert_cmphex(read_reg(s, local + 8), ==, after);
                if (write) {
                    g_assert_cmphex(read_reg(s, local), ==, source);
                } else if (!table) {
                    const uint8_t zeros[4] = { 0 };

                    check_bytes(s, BASE + local + 4, zeros, sizeof(zeros));
                }
                for (unsigned row = 0; row < 2; row++) {
                    memcpy(expected + 8 + odd + row * (unit + 1),
                           (write ? data : typed_pattern) + row * unit, unit);
                }
                check_bytes(s, 0xfff8, expected, sizeof(expected));
                qtest_quit(s);
            }
        }
    }
}

static void test_odd_split_read(void)
{
    const uint8_t zeros[8] = { 0 };
    const uint8_t bsp[2] = { 0xab, 0x37 };
    const uint8_t table_entry[4] = { 0, 0x37, 0xcd, 0x28 };

    for (unsigned table = 0; table < 2; table++) {
        for (unsigned revoke = 0; revoke < 3; revoke++) {
            QTestState *s = start_split();
            unsigned unit = table ? 4 : 2;
            unsigned local = table ? 0x5000 : 0x4000;
            unsigned channel = table ? 0 : 1;
            unsigned offset = channel * 0x40;

            if (!s) {
                return;
            }
            split_source(s);
            descriptor(s, channel, 0, 0xa000, SPLIT_MEMORY + 1,
                       unit, 0, 1, local);
            run(s, channel, 0, true);
            qtest_clock_step(s, 17); /* One source byte, not a local unit. */
            check_bytes(s, BASE + local, zeros, sizeof(zeros));
            g_assert_cmphex(read_reg(s, 0x1a8 + offset), ==, unit);
            if (revoke == 1) {
                reg(s, 0xe000, 0);
            } else {
                qtest_clock_step(s, 17);
                check_bytes(s, BASE + local, zeros, sizeof(zeros));
                g_assert_cmphex(read_reg(s, 0x1a8 + offset), ==, unit);
                if (revoke == 2) {
                    reg(s, 0xe000, 0);
                }
            }
            finish(s);
            if (revoke == 1) {
                check_bytes(s, BASE + local, zeros, sizeof(zeros));
                g_assert_cmphex(read_reg(s, 0x188 + offset), ==, 0x71a);
                g_assert_cmphex(read_reg(s, 0x1a8 + offset), ==, unit);
            } else {
                check_bytes(s, BASE + local, table ? table_entry : bsp, unit);
                check_bytes(s, BASE + local + unit, zeros, 8 - unit);
                g_assert_cmphex(read_reg(s, 0x188 + offset), ==, 0x219);
                g_assert_cmphex(read_reg(s, 0x1a8 + offset), ==, 0);
            }
            qtest_quit(s);
        }
    }
}

static void test_odd_split_write(void)
{
    const uint8_t guards[6] = { 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc };

    for (unsigned table = 0; table < 2; table++) {
        QTestState *s = start_split();
        unsigned unit = table ? 4 : 2;
        unsigned local = table ? 0x5000 : 0x4000;
        unsigned channel = table;
        unsigned offset = channel * 0x40;
        uint8_t accepted[2] = { 0xcc, table ? 0 : 0x91 };
        uint64_t source;

        if (!s) {
            return;
        }
        qtest_memwrite(s, SGI_DMA_WORD_TEST_BACKING +
                       SGI_DMA_WORD_TEST_SPLIT_FIRST, guards, 2);
        qtest_memwrite(s, SGI_DMA_WORD_TEST_BACKING +
                       SGI_DMA_WORD_TEST_SPLIT_SECOND, guards, 6);
        reg(s, local, UINT64_C(0x91ab37cd2876f042));
        source = read_reg(s, local);
        descriptor(s, channel, 0, 0x8000, SPLIT_MEMORY + 1,
                   unit, 0, 1, local);
        run(s, channel, 0, true);
        qtest_clock_step(s, 34); /* One accepted system destination byte. */
        g_assert_cmphex(read_reg(s, 0x1a8 + offset), ==, unit - 1);
        check_bytes(s, SGI_DMA_WORD_TEST_BACKING +
                    SGI_DMA_WORD_TEST_SPLIT_FIRST, accepted, 2);
        check_bytes(s, SGI_DMA_WORD_TEST_BACKING +
                    SGI_DMA_WORD_TEST_SPLIT_SECOND, guards, 6);
        reg(s, 0xe000, 0);
        finish(s);
        g_assert_cmphex(read_reg(s, 0x188 + offset), ==, 0x712);
        g_assert_cmphex(read_reg(s, 0x1a8 + offset), ==, unit - 1);
        g_assert_cmphex(read_reg(s, local), ==, source);
        check_bytes(s, SGI_DMA_WORD_TEST_BACKING +
                    SGI_DMA_WORD_TEST_SPLIT_FIRST, accepted, 2);
        check_bytes(s, SGI_DMA_WORD_TEST_BACKING +
                    SGI_DMA_WORD_TEST_SPLIT_SECOND, guards, 6);
        qtest_quit(s);
    }
}

static void test_msp_system_alignment(void)
{
    for (unsigned odd = 0; odd < 2; odd++) {
        QTestState *s = start();

        reg(s, 0xe000, 2);
        descriptor(s, 0, 0, 0xa000, odd ? 0x10001 : 0x10000,
                   8, odd ? 0 : 9, odd ? 1 : 2, 0x2000);
        run(s, 0, 0, true);
        finish(s);
        assert_refused(s);
        g_assert_cmphex(read_reg(s, 0x2000), ==, 0);
        qtest_quit(s);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi-vice/identity", test_identity);
    qtest_add_func("/sgi-vice/memories", test_memories);
    qtest_add_func("/sgi-vice/interrupts", test_interrupts);
    qtest_add_func("/sgi-vice/transfer", test_transfer);
    qtest_add_func("/sgi-vice/translation", test_translation);
    qtest_add_func("/sgi-vice/chain", test_chain);
    qtest_add_func("/sgi-vice/boundaries", test_boundaries);
    qtest_add_func("/sgi-vice/reset-duration", test_reset_duration);
    qtest_add_func("/sgi-vice/unsupported", test_unsupported);
    qtest_add_func("/sgi-vice/late-fault", test_late_fault);
    qtest_add_func("/sgi-vice/concurrent", test_concurrent);
    qtest_add_func("/sgi-vice/translation-indices", test_translation_indices);
    qtest_add_func("/sgi-vice/other-clocks", test_other_clocks);
    qtest_add_func("/sgi-vice/held-descriptors", test_held_descriptors);
    qtest_add_func("/sgi-vice/migration", test_migration);
    qtest_add_func("/sgi-vice/local-dma", test_local_dma);
    qtest_add_func("/sgi-vice/permission-changes", test_permission_changes);
    qtest_add_func("/sgi-vice/completion-enable", test_completion_enable);
    qtest_add_func("/sgi-vice/padding", test_padding);
    qtest_add_func("/sgi-vice/reset-restart", test_reset_restart);
    qtest_add_func("/sgi-vice/ram-views", test_ram_views);
    qtest_add_func("/sgi-vice/fill-progress", test_fill_progress);
    qtest_add_func("/sgi-vice/descriptor-edits", test_descriptor_edits);
    qtest_add_func("/sgi-vice/stride", test_stride);
    qtest_add_func("/sgi-vice/padding-pending", test_padding_pending);
    qtest_add_func("/sgi-vice/address-bounds", test_address_bounds);
    qtest_add_func("/sgi-vice/read-only", test_read_only);
    qtest_add_func("/sgi-vice/counter", test_counter);
    qtest_add_func("/sgi-vice/peer-reset", test_peer_reset);
    qtest_add_func("/sgi-vice/skip-progress", test_skip_progress);
    qtest_add_func("/sgi-vice/typed-assembly", test_typed_assembly);
    qtest_add_func("/sgi-vice/typed-revocation", test_typed_revocation);
    qtest_add_func("/sgi-vice/typed-stop-reset", test_typed_stop_reset);
    qtest_add_func("/sgi-vice/typed-migration", test_typed_migration);
    qtest_add_func("/sgi-vice/typed-staged-grant", test_typed_staged_grant);
    qtest_add_func("/sgi-vice/typed-remap", test_typed_remap);
    qtest_add_func("/sgi-vice/typed-small-units", test_typed_small_units);
    qtest_add_func("/sgi-vice/typed-write-migration",
                   test_typed_write_migration);
    qtest_add_func("/sgi-vice/odd-system", test_odd_system);
    qtest_add_func("/sgi-vice/odd-split-read", test_odd_split_read);
    qtest_add_func("/sgi-vice/odd-split-write", test_odd_split_write);
    qtest_add_func("/sgi-vice/msp-system-alignment", test_msp_system_alignment);
    return g_test_run();
}
