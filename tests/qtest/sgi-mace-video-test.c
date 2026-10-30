/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Register-contract and timed linear-DMA tests for MACE revision 2 video. */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-migrate.h"

#define MACE 0x1f000000ULL
#define TIMER (MACE + 0x340038)

#define FIXTURE 0x1f3f0000ULL
#define VIDEO0 (MACE + 0x100000)
#define VIDEO1 (MACE + 0x180000)
#define VIDEO2 (MACE + 0x200000)
#define VIDEO_PATH "/machine/peripheral/video"

typedef struct VideoFixture {
    QTestState *qts;
    char *dir;
    char *ram;
    int fd;
} VideoFixture;

static VideoFixture fixture_start(const char *extra)
{
    VideoFixture t = { 0 };

    t.dir = g_dir_make_tmp("mace-video-XXXXXX", NULL);
    g_assert_nonnull(t.dir);
    t.ram = g_build_filename(t.dir, "ram", NULL);
    t.fd = g_open(t.ram, O_CREAT | O_EXCL | O_RDWR, 0600);
    g_assert_cmpint(t.fd, >=, 0);
    g_assert_cmpint(ftruncate(t.fd, 4 << 20), ==, 0);
    t.qts = qtest_initf("-M none -cpu R10000 -nodefaults "
        "-object memory-backend-file,id=ram,size=4M,mem-path=%s,share=on "
        "-device sgi-mace-video-test,id=video,ram=/objects/ram/ram[0],synthetic-revision=10 "
        MEMTX_TEST_DEVICE " %s", t.ram, extra);
    for (unsigned i = 0; i < 3; i++) {
        qtest_sysbus_mmio_map(t.qts, VIDEO_PATH, i, VIDEO0 + i * 0x80000);
    }
    qtest_sysbus_mmio_map(t.qts, VIDEO_PATH, 3, TIMER);
    qtest_sysbus_mmio_map(t.qts, VIDEO_PATH, 4, FIXTURE);
    qtest_irq_intercept_out(t.qts, VIDEO_PATH);
    return t;
}

static void fixture_stop(VideoFixture *t)
{
    qtest_quit(t->qts);
    close(t->fd);
    g_assert_cmpint(unlink(t->ram), ==, 0);
    g_assert_cmpint(rmdir(t->dir), ==, 0);
    g_free(t->ram);
    g_free(t->dir);
}

static void event(VideoFixture *t, int64_t ns, unsigned port, unsigned type,
                   unsigned field, unsigned x, unsigned y, uint32_t data)
{
    qtest_writeq(t->qts, FIXTURE, ns);
    qtest_writeq(t->qts, FIXTURE + 8, type | port << 4 | field << 8);
    qtest_writeq(t->qts, FIXTURE + 0x10, x);
    qtest_writeq(t->qts, FIXTURE + 0x18, y);
    qtest_writeq(t->qts, FIXTURE + 0x20, data);
    qtest_writeq(t->qts, FIXTURE + 0x28, 1);
}

static void timer_layout(void)
{
    VideoFixture t = fixture_start("");
    QTestState *s = t.qts;
    uint8_t bytes[8];

    for (unsigned i = 0; i < 3; i++) {
        uint64_t addr = TIMER + i * 8;
        uint64_t pair = 0x10203040a1b2c3d4ULL + i;

        qtest_writeq(s, addr, pair);
        qtest_memread(s, addr, bytes, sizeof(bytes));
        g_assert_cmphex(ldq_be_p(bytes), ==, pair);
        g_assert_cmphex(qtest_readl(s, addr), ==, 0x10203040);
        g_assert_cmphex(qtest_readl(s, addr + 4), ==, 0xa1b2c3d4U + i);
        qtest_writel(s, addr, 0x55667788);
        qtest_writel(s, addr + 4, 0x99aabbcc);
        g_assert_cmphex(qtest_readq(s, addr), ==, 0x5566778899aabbccULL);
        qtest_writeb(s, addr, 0xff);
        qtest_writew(s, addr + 2, 0xffff);
        g_assert_cmphex(qtest_readq(s, addr), ==, 0x5566778899aabbccULL);
    }
    g_assert_cmpint(memtx_store(s, TIMER + 20, UINT64_MAX, 8), !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, TIMER + 4, UINT64_MAX, 8), !=, MEMTX_OK);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readq(s, TIMER + i * 8), ==,
                       0x5566778899aabbccULL);

    }
    qtest_qmp_assert_success(s, "{ 'execute': 'system_reset' }");
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readq(s, TIMER + i * 8), ==, 0);
    }
    fixture_stop(&t);
}

static void registers(void)
{
    static const uint64_t masks[2][12] = {
        { 0x3ff, 0, 0x1fffc, 0xffffffc7, 0xfff8, 0xff8,
          0x1ffffffffff, 0x7fffffffff, 0xff,
          0x1ffffffffff, 0x7fffffffff, 0xff },
        { 0x19f, 0, 0x3ffffde, 0xffffffc5, 0xfff8, 0x3ffff8,
          0x3ffffffffff, 0xffffffffff, 0x3ffffffffff,
          0xffffffffff, 0x7ff, 0 },
    };
    VideoFixture t = fixture_start("");
    QTestState *s = t.qts;
    uint64_t value;

    for (unsigned ch = 0; ch < 3; ch++) {
        uint64_t base = MACE + 0x100000 + ch * 0x80000;

        for (unsigned r = 0; r < 12; r++) {
            if (r == 1 || (ch == 2 && r == 11)) {
                continue;
            }
            qtest_writeq(s, base + r * 8, masks[ch == 2][r]);
            g_assert_cmphex(qtest_readq(s, base + r * 8), ==,
                           masks[ch == 2][r]);
        }
        for (unsigned r = 0; r < 8; r++) {
            uint64_t data = 0x1020304050607080ULL + ch * 0x100 + r;

            qtest_writeq(s, base + 0x80 + r * 8, data);
            g_assert_cmphex(qtest_readq(s, base + 0x80 + r * 8), ==, data);
        }
        g_assert_cmphex(qtest_readq(s, base + 8), ==, 0);
        qtest_writeq(s, base + 8, UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, base + 8), ==, 0);
        for (unsigned bytes = 1; bytes <= 4; bytes *= 2) {
            g_assert_cmpint(memtx_store(s, base + 0x20, UINT64_MAX, bytes),
                            !=, MEMTX_OK);
            g_assert_cmphex(qtest_readq(s, base + 0x20), ==, 0xfff8);
        }
        g_assert_cmpint(memtx_load(s, base + 0x100, &value, 8), !=, MEMTX_OK);
        g_assert_cmpint(memtx_load(s, base + 0x60, &value, 8), !=, MEMTX_OK);
    }
    qtest_qmp_assert_success(s, "{ 'execute': 'system_reset' }");
    for (unsigned ch = 0; ch < 3; ch++) {
        uint64_t base = MACE + 0x100000 + ch * 0x80000;

        g_assert_cmphex(qtest_readq(s, base), ==, 0);
        g_assert_cmphex(qtest_readq(s, base + 8), ==, 0);
        g_assert_cmphex(qtest_readq(s, base + 0x80), ==, 0);
    }
    fixture_stop(&t);
}

static void signal_counters(void)
{
    VideoFixture t = fixture_start("");
    QTestState *s = t.qts;

    qtest_writeq(s, VIDEO2 + 0x58, 5);
    qtest_writeq(s, VIDEO0 + 0x10, 2);
    qtest_writeq(s, VIDEO1 + 0x10, 2 | 1 << 2);
    qtest_writeq(s, VIDEO0, 2);
    qtest_writeq(s, VIDEO1, 2);
    event(&t, 9600, 0, 1, 0, 0, 0, 0);
    event(&t, 9600, 0, 4, 0, 0, 0, 0);
    event(&t, 19200, 0, 3, 0, 0, 0, 0);
    event(&t, 19200, 0, 4, 1, 0, 0, 0);
    event(&t, 28800, 0, 3, 1, 0, 0, 0);
    event(&t, 28800, 0, 4, 1, 0, 0, 0);
    event(&t, 38400, 0, 3, 1, 0, 0, 0);
    qtest_clock_step(s, 9600);
    g_assert_cmphex(qtest_readq(s, TIMER), ==, 10ULL << 32);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_false(qtest_get_irq(s, 1));
    g_assert_cmphex(qtest_readq(s, VIDEO0 + 8), ==, 0x402);
    g_assert_cmphex(qtest_readq(s, VIDEO0 + 8), ==, 0x400);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_clock_step(s, 9600);
    g_assert_cmphex(qtest_readq(s, TIMER), ==, (20ULL << 32) | 2);
    qtest_clock_step(s, 9600);
    g_assert_cmphex(qtest_readq(s, TIMER), ==, (30ULL << 32) | 3);
    qtest_clock_step(s, 9600);
    g_assert_cmphex(qtest_readq(s, TIMER), ==, (30ULL << 32) | 5);
    g_assert_cmphex(qtest_readq(s, TIMER + 8), ==, 0);
    qtest_writeq(s, VIDEO0 + 0x10, 0);
    event(&t, 48000, 0, 4, 0, 0, 0, 0);
    qtest_clock_step(s, 9600);
    g_assert_cmphex(qtest_readq(s, TIMER), ==, (30ULL << 32) | 5);
    fixture_stop(&t);
}

static void migration_signal(void)
{
    VideoFixture from = fixture_start("");
    VideoFixture to = fixture_start("-incoming defer");
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate", from.dir);

    qtest_writeq(from.qts, VIDEO2 + 0x58, 1);
    qtest_writeq(from.qts, VIDEO0 + 0x10, 2);
    qtest_writeq(from.qts, VIDEO0, 2);
    event(&from, 9600, 0, 1, 0, 0, 0, 0);
    event(&from, 9600, 0, 4, 0, 0, 0, 0);
    event(&from, 19200, 0, 3, 0, 0, 0, 0);
    event(&from, 19200, 0, 4, 1, 0, 0, 0);
    qtest_clock_step(from.qts, 9600);
    event(&to, 38400, 0, 4, 0, 0, 0, 0);
    qtest_clock_set(to.qts, 9600);
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    g_assert_cmphex(qtest_readq(to.qts, TIMER), ==, 10ULL << 32);
    g_assert_true(qtest_get_irq(to.qts, 0));
    qtest_clock_step(to.qts, 9600);
    qtest_clock_step(from.qts, 9600);
    g_assert_cmphex(qtest_readq(to.qts, TIMER), ==, (20ULL << 32) | 2);
    g_assert_cmphex(qtest_readq(to.qts, TIMER), ==,
                   qtest_readq(from.qts, TIMER));
    qtest_clock_step(to.qts, 19200);
    qtest_clock_step(from.qts, 19200);
    g_assert_cmphex(qtest_readq(to.qts, TIMER), ==,
                   qtest_readq(from.qts, TIMER));
    fixture_stop(&to);
    if (g_file_test(uri + 5, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(unlink(uri + 5), ==, 0);
    }
    fixture_stop(&from);
}

static void loss_and_counter_wrap(void)
{
    VideoFixture t = fixture_start("");
    QTestState *s = t.qts;
    uint64_t start = (uint64_t)UINT32_MAX * 960;

    qtest_writeq(s, VIDEO2 + 0x58, 1);
    qtest_writeq(s, VIDEO0 + 0x10, 2);
    qtest_writeq(s, TIMER, 0x55667788ffffffffULL);
    qtest_clock_set(s, start - 1);
    event(&t, start, 0, 1, 0, 0, 0, 0);
    event(&t, start, 0, 4, 0, 0, 0, 0);
    event(&t, start + 960, 0, 3, 0, 0, 0, 0);
    event(&t, start + 960, 0, 4, 1, 0, 0, 0);
    event(&t, start + 1920, 0, 3, 1, 0, 0, 0);
    event(&t, start + 1920, 0, 0, 1, 0, 0, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, TIMER), ==, UINT64_MAX);
    qtest_clock_step(s, 960);
    g_assert_cmphex(qtest_readq(s, TIMER), ==, 0);
    qtest_clock_step(s, 960);
    g_assert_cmphex(qtest_readq(s, TIMER), ==, 0);
    g_assert_cmphex(qtest_readq(s, VIDEO0 + 8), ==, 10);
    g_assert_cmphex(qtest_readq(s, VIDEO0 + 8), ==, 0);
    fixture_stop(&t);
}

static void loss_irq_without_field(void)
{
    VideoFixture t = fixture_start("");
    QTestState *s = t.qts;

    qtest_writeq(s, VIDEO2 + 0x58, 1);
    qtest_writeq(s, VIDEO0 + 0x10, 2);
    event(&t, 9600, 0, 1, 0, 0, 0, 0);
    event(&t, 19200, 0, 0, 0, 0, 0, 0);
    qtest_clock_step(s, 9600);
    g_assert_cmphex(qtest_readq(s, VIDEO0 + 8), ==, 0x400);
    qtest_clock_step(s, 9600);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writeq(s, VIDEO0, 8);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, VIDEO0, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writeq(s, VIDEO0, 8);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, VIDEO0 + 8), ==, 8);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, VIDEO0 + 8), ==, 0);
    g_assert_cmphex(qtest_readq(s, TIMER), ==, 0);
    fixture_stop(&t);
}

static void synthetic_revision(void)
{
    VideoFixture t = fixture_start("");
    QTestState *s = t.qts;

    g_assert_cmphex(qtest_readq(s, VIDEO2 + 0x58), ==, 10ULL << 32);
    qtest_writeq(s, VIDEO2 + 0x58, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, VIDEO2 + 0x58), ==,
                   (10ULL << 32) | 0x7fffff);
    qtest_qmp_assert_success(s, "{ 'execute': 'system_reset' }");
    g_assert_cmphex(qtest_readq(s, VIDEO2 + 0x58), ==, 10ULL << 32);
    fixture_stop(&t);
}

static uint32_t pattern(unsigned seed, unsigned x, unsigned y)
{
    unsigned p = x & ~1U;

    return ((0x40 + ((seed + p + y) % 31)) << 24) |
           ((0x20 + ((seed * 3 + p * 5 + y * 7) % 173)) << 16) |
           ((0xa0 + ((seed + p * 3 + y * 2) % 47)) << 8) |
           (0x30 + ((seed * 7 + p * 11 + y * 13) % 163));
}

static void table(VideoFixture *t, unsigned offset, uint16_t page)
{
    uint8_t data[64] = { 0 };

    stw_be_p(data, page);
    g_assert_cmpint(pwrite(t->fd, data, sizeof(data), offset), ==,
                    sizeof(data));
}

static void capture_setup(VideoFixture *t, unsigned ch, unsigned source,
                           unsigned width, unsigned lines, unsigned offset,
                           bool interleaved, unsigned selector)
{
    uint64_t base = VIDEO0 + ch * 0x80000;
    uint32_t cfg = (2 << 10) | (source << 2) | (interleaved ? 1 << 16 : 0);
    uint64_t h = (uint64_t)(width / 2 - 1) << 10;
    uint64_t v = 1 | (uint64_t)lines << 20;

    qtest_writeq(t->qts, VIDEO2 + 0x58, 5);
    qtest_writeq(t->qts, base + 0x10, cfg);
    qtest_writeq(t->qts, base + 0x20, offset);
    qtest_writeq(t->qts, base + 0x28, width);
    qtest_writeq(t->qts, base + 0x30, h);
    qtest_writeq(t->qts, base + 0x38, v);
    qtest_writeq(t->qts, base + 0x48, h);
    qtest_writeq(t->qts, base + 0x50, v);
    qtest_writeq(t->qts, base + 0x10, cfg | 2);
    qtest_writeq(t->qts, base + 0x10, cfg | 3);
    qtest_writeq(t->qts, base + 0x18, (0x40000000 + ch * 64) | 4 | selector);
    qtest_writeq(t->qts, base, 7);
}

static int64_t field(VideoFixture *t, int64_t sof, unsigned source,
                      unsigned id, unsigned pixels, unsigned lines,
                      unsigned seed)
{
    qtest_writeq(t->qts, FIXTURE, sof);
    qtest_writeq(t->qts, FIXTURE + 8, source << 4 | id << 8);
    qtest_writeq(t->qts, FIXTURE + 0x10, pixels);
    qtest_writeq(t->qts, FIXTURE + 0x18, lines);
    qtest_writeq(t->qts, FIXTURE + 0x70, seed);
    qtest_writeq(t->qts, FIXTURE + 0x68, 1);
    return sof + (int64_t)lines * pixels * 100;
}

static void capture_small(void)
{
    VideoFixture t = fixture_start("");
    uint8_t guard[16], actual[16], expected[8];
    int64_t end;

    memset(guard, 0x59, sizeof(guard));
    g_assert_cmpint(pwrite(t.fd, guard, sizeof(guard), 0x10000), ==,
                    sizeof(guard));
    table(&t, 0, 0x4001);
    capture_setup(&t, 0, 0, 8, 1, 0, false, 0);
    end = field(&t, 1000, 0, 0, 14, 3, 17);
    qtest_clock_step(t.qts, 1000);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x18), ==, 0x40000000);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0x403);
    qtest_clock_step(t.qts, 1999);
    g_assert_cmpint(pread(t.fd, actual, sizeof(actual), 0x10000), ==,
                    sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), guard, sizeof(guard));
    qtest_clock_step(t.qts, 1);
    stl_be_p(expected, pattern(17, 2, 1));
    stl_be_p(expected + 4, pattern(17, 4, 1));
    g_assert_cmpint(pread(t.fd, actual, sizeof(actual), 0x10000), ==,
                    sizeof(actual));
    g_assert_cmpmem(actual, 8, expected, sizeof(expected));
    g_assert_cmpmem(actual + 8, 8, guard + 8, 8);
    g_assert_true(qtest_get_irq(t.qts, 0));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0x404);
    g_assert_false(qtest_get_irq(t.qts, 0));
    g_assert_cmpint(memtx_store(t.qts, VIDEO0 + 0x10, (2 << 10) | 2, 8),
                    ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0), ==, 7);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x80), ==,
                   0x4001000000000000ULL);
    g_assert_cmphex(qtest_readq(t.qts, TIMER), ==, 1ULL << 32);
    qtest_clock_step(t.qts, end - 3000);
    g_assert_cmphex(qtest_readq(t.qts, TIMER), ==, (1ULL << 32) | 2);
    fixture_stop(&t);
}

static void capture_interleaved(void)
{
    VideoFixture t = fixture_start("");
    uint8_t data[64] = { 0 }, actual[8], expected[8];

    stw_be_p(data, 0x4001);
    stw_be_p(data + 2, 0x4004);
    g_assert_cmpint(pwrite(t.fd, data, 64, 0), ==, 64);
    capture_setup(&t, 0, 0, 16, 2, 65528, true, 3);
    field(&t, 1000, 0, 1, 18, 4, 23);
    qtest_clock_step(t.qts, 1000);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x80), ==,
                   0x4001400400000000ULL);
    qtest_clock_step(t.qts, 4600);
    for (unsigned row = 0; row < 2; row++) {
        for (unsigned unit = 0; unit < 2; unit++) {
            unsigned addr = row ? 0x40018 + unit * 8 :
                            unit ? 0x40000 : 0x1fff8;

            stl_be_p(expected, pattern(23, 2 + unit * 4, 1 + row));
            stl_be_p(expected + 4, pattern(23, 4 + unit * 4, 1 + row));
            g_assert_cmpint(pread(t.fd, actual, 8, addr), ==, 8);
            g_assert_cmpmem(actual, 8, expected, 8);
        }
    }
    g_assert_cmpint(pread(t.fd, actual, 8, 0x40010), ==, 8);
    g_assert_cmphex(ldq_be_p(actual), ==, 0);
    g_assert_cmpint(pread(t.fd, actual, 8, 0x20000), ==, 8);
    g_assert_cmphex(ldq_be_p(actual), ==, 0);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0x406);
    fixture_stop(&t);
}

static void queued_isolation(void)
{
    VideoFixture t = fixture_start("");
    uint8_t actual[8], expected[8];

    table(&t, 0, 0x4001);
    table(&t, 64, 0x4002);
    capture_setup(&t, 0, 0, 8, 2, 0, false, 0);
    field(&t, 1000, 0, 0, 14, 4, 31);
    field(&t, 10000, 0, 1, 14, 4, 47);
    qtest_clock_step(t.qts, 1000);
    qtest_writeq(t.qts, VIDEO0 + 0x18, 0x40000040);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8) & 1, ==, 1);
    qtest_writeq(t.qts, VIDEO0 + 0x18, 0x40000044);
    qtest_writeq(t.qts, VIDEO0 + 0x20, 24);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writeq(t.qts, VIDEO0 + 0x80 + i * 8, 0x123456789abcdef0ULL + i);
        g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x80 + i * 8), ==,
                       0x123456789abcdef0ULL + i);
    }
    g_assert_cmpint(memtx_store(t.qts, VIDEO0 + 0x10, (2 << 10) | 2, 8),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(t.qts, VIDEO0 + 0x38, 1 | 1 << 20, 8),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(t.qts, VIDEO0 + 0x50, 1 | 1 << 20, 8),
                    ==, MEMTX_OK);
    qtest_clock_step(t.qts, 3400);
    for (unsigned row = 0; row < 2; row++) {
        stl_be_p(expected, pattern(31, 2, 1 + row));
        stl_be_p(expected + 4, pattern(31, 4, 1 + row));
        g_assert_cmpint(pread(t.fd, actual, 8, 0x10000 + row * 8), ==, 8);
        g_assert_cmpmem(actual, 8, expected, 8);
    }
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x18), ==, 0x40000044);
    qtest_clock_step(t.qts, 7600);
    stl_be_p(expected, pattern(47, 2, 1));
    stl_be_p(expected + 4, pattern(47, 4, 1));
    g_assert_cmpint(pread(t.fd, actual, 8, 0x20018), ==, 8);
    g_assert_cmpmem(actual, 8, expected, 8);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x80), ==,
                   0x4002000000000000ULL);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x18), ==, 0x40000040);
    fixture_stop(&t);
}

static void capture_cancel(gconstpointer opaque)
{
    unsigned action = GPOINTER_TO_UINT(opaque);
    VideoFixture t = fixture_start("");
    uint8_t actual[16], expected[8];

    table(&t, 0, 0x4001);
    capture_setup(&t, 0, 0, 16, 1, 0, false, 0);
    field(&t, 1000, 0, 0, 18, 3, 53);
    if (action == 1) {
        event(&t, 3800, 0, 0, 0, 0, 0, 0);
    }
    qtest_clock_step(t.qts, action == 0 ? 3799 : 3800);
    if (action == 0) {
        qtest_writeq(t.qts, VIDEO0, 6);
    } else if (action == 2) {
        qtest_writeq(t.qts, VIDEO0, 6);
    }
    qtest_clock_step(t.qts, 1000);
    stl_be_p(expected, pattern(53, 2, 1));
    stl_be_p(expected + 4, pattern(53, 4, 1));
    g_assert_cmpint(pread(t.fd, actual, 16, 0x10000), ==, 16);
    g_assert_cmpmem(actual, 8, expected, 8);
    if (action == 2) {
        g_assert_cmphex(ldq_be_p(actual + 8), !=, 0);
    } else {
        g_assert_cmphex(ldq_be_p(actual + 8), ==, 0);
    }
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8) & 0x1fc, ==,
                   action == 1 ? 8 : action == 2 ? 4 : 0);
    fixture_stop(&t);
}

static uint64_t pixel_time(unsigned pixel)
{
    return ((uint64_t)pixel * 2000 + 26) / 27;
}

static uint32_t output_setup(VideoFixture *t, unsigned id)
{
    QTestState *s = t->qts;
    uint32_t cfg = (2 << 14) | (2 << 1) | (2 << 3);
    uint64_t h = (5 << 10) | (1 << 20);

    qtest_writeq(s, VIDEO2 + 0x10, cfg);
    qtest_writeq(s, VIDEO2 + 0x20, 0);
    qtest_writeq(s, VIDEO2 + 0x28, (1 << 12) | 8);
    qtest_writeq(s, VIDEO2 + 0x30, h);
    qtest_writeq(s, VIDEO2 + 0x38, (2 << 10) | (3 << 20));
    qtest_writeq(s, VIDEO2 + 0x40, h);
    qtest_writeq(s, VIDEO2 + 0x48, (3 << 10) | (4 << 20));
    qtest_writeq(s, VIDEO2 + 0x18, 0x40000004 | id);
    qtest_writeq(s, VIDEO2, 7);
    qtest_writeq(s, VIDEO2 + 0x10, cfg | 32);
    qtest_writeq(s, VIDEO2 + 0x10, cfg | 33);
    g_assert_cmphex(qtest_readq(s, VIDEO2 + 0x10), ==, cfg | 33);
    return cfg;
}

static void output_timed(void)
{
    static const uint8_t pixels[8] = { 0x4e, 0x25, 0xb2, 0x77,
                                      0x68, 0x39, 0xc4, 0x93 };
    VideoFixture t = fixture_start("");
    uint8_t changed[8] = { 0 };
    uint32_t cfg;
    unsigned count;

    table(&t, 0, 0x4001);
    g_assert_cmpint(pwrite(t.fd, pixels, 8, 0x10000), ==, 8);
    cfg = output_setup(&t, 1);
    qtest_clock_step(t.qts, pixel_time(48));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 0x18), ==, 0x40000001);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8), ==, 3);
    qtest_clock_step(t.qts, pixel_time(64) - pixel_time(48));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8), ==, 1);
    g_assert_cmpint(pwrite(t.fd, changed, 8, 0x10000), ==, 8);
    qtest_clock_step(t.qts, pixel_time(67) - pixel_time(64));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8), ==, 4);
    count = qtest_readq(t.qts, FIXTURE + 0x58);
    g_assert_cmpuint(count, >=, 8);
    for (unsigned i = 0; i < 8; i++) {
        unsigned pixel = i / 2;
        uint64_t obs = FIXTURE + 0x1000 + (count - 8 + i) * 32;

        g_assert_cmpuint(qtest_readq(t.qts, obs), ==, pixel_time(64 + pixel));
        g_assert_cmphex(qtest_readq(t.qts, obs + 8), ==,
                       ((uint64_t)(4 + pixel) << 32) | 1);
        g_assert_cmphex(qtest_readq(t.qts, obs + 16), ==,
                       ldl_be_p(pixels + (pixel / 2) * 4));
        g_assert_cmphex(qtest_readq(t.qts, obs + 24), ==, 0x102 + (i & 1));
    }
    g_assert_cmphex(qtest_readq(t.qts, TIMER + 16), ==, (3ULL << 32) | 2);
    qtest_writeq(t.qts, VIDEO2, 6);
    qtest_clock_step(t.qts, pixel_time(108) - pixel_time(67));
    g_assert_cmphex(qtest_readq(t.qts, TIMER + 16), ==, (8ULL << 32) | 3);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg | 32);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg);
    fixture_stop(&t);
}

static void capture_fault(gconstpointer opaque)
{
    unsigned fault = GPOINTER_TO_UINT(opaque);
    VideoFixture t = fixture_start(fault == 3 ?
        "-global sgi-mace-video-test.mapped-bytes=65540" : "");
    uint8_t data[64] = { 0 }, actual[16], expected[8];
    unsigned offset = fault == 2 ? 65528 : 0;

    stw_be_p(data, fault == 0 ? 0 : 0x4001);
    stw_be_p(data + 2, 0xf001);
    g_assert_cmpint(pwrite(t.fd, data, 64, 0), ==, 64);
    capture_setup(&t, 0, 0, 16, 1, offset, false, 0);
    if (fault == 1) {
        qtest_writeq(t.qts, VIDEO0 + 0x80, 0x123456789abcdef0ULL);
        qtest_writeq(t.qts, VIDEO0 + 0x18, 0x00000044);
    }
    field(&t, 1000, 0, 0, 18, 3, 61);
    qtest_clock_step(t.qts, 3800);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==,
                   0x402 | (fault == 0 ? 16 : 256));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x18) & 4, ==, 0);
    if (fault == 1) {
        g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x80), ==,
                       0x123456789abcdef0ULL);
    }
    g_assert_cmpint(pread(t.fd, actual, 16, 0x10000 + offset), ==, 16);
    if (fault == 2) {
        stl_be_p(expected, pattern(61, 2, 1));
        stl_be_p(expected + 4, pattern(61, 4, 1));
        g_assert_cmpmem(actual, 8, expected, 8);
    } else {
        g_assert_cmphex(ldq_be_p(actual), ==, 0);
    }
    qtest_writeq(t.qts, VIDEO0 + 0x18, 0x40000004);
    field(&t, 10000, 0, 1, 18, 3, 67);
    qtest_clock_step(t.qts, 10000 - 3800);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x18) & 4, ==, 4);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8) & 5, ==, 0);
    fixture_stop(&t);
}

static void capture_restart(void)
{
    VideoFixture t = fixture_start("");
    uint8_t actual[8], expected[8];

    table(&t, 0, 0);
    capture_setup(&t, 0, 0, 8, 1, 0, false, 1);
    field(&t, 1000, 0, 0, 14, 3, 71);
    qtest_clock_step(t.qts, 3000);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0x412);
    table(&t, 64, 0x8002);
    qtest_writeq(t.qts, VIDEO0 + 0x18, 0x80000044);
    field(&t, 10000, 0, 1, 14, 3, 73);
    qtest_clock_step(t.qts, 7000);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x18), ==, 0x80000044);
    qtest_writeq(t.qts, VIDEO0, 6);
    qtest_writeq(t.qts, VIDEO0, 7);
    field(&t, 20000, 0, 0, 14, 3, 79);
    qtest_clock_step(t.qts, 12000);
    stl_be_p(expected, pattern(79, 2, 1));
    stl_be_p(expected + 4, pattern(79, 4, 1));
    g_assert_cmpint(pread(t.fd, actual, 8, 0x20000), ==, 8);
    g_assert_cmpmem(actual, 8, expected, 8);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0x406);
    fixture_stop(&t);
}

static void span_boundary(gconstpointer opaque)
{
    bool interleaved = GPOINTER_TO_UINT(opaque);
    VideoFixture t = fixture_start("");
    unsigned lines = interleaved ? 512 : 1023;

    table(&t, 0, 0x4001);
    capture_setup(&t, 0, 0, 2048, lines, 2048, interleaved, 0);
    event(&t, 1000, 0, 1, 0, 0, 0, 0);
    event(&t, 1000, 0, 4, 0, 1034, lines + 2, 0);
    qtest_clock_step(t.qts, 1000);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x18), ==, 0x40000000);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0x403);
    qtest_qmp_assert_success(t.qts, "{ 'execute': 'system_reset' }");
    capture_setup(&t, 0, 0, 2048, lines, 2056, interleaved, 0);
    qtest_writeq(t.qts, VIDEO0 + 0x80, 0x1122334455667788ULL);
    qtest_writeq(t.qts, VIDEO0 + 0x18, 0x00000044);
    event(&t, 2000, 0, 4, 0, 1034, lines + 2, 0);
    qtest_clock_step(t.qts, 1000);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x18), ==, 0x44);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x80), ==,
                   0x1122334455667788ULL);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0x402);
    fixture_stop(&t);
}

static void capture_padding(void)
{
    VideoFixture t = fixture_start("");
    uint8_t actual[16], expected[8];

    table(&t, 0, 0x4001);
    capture_setup(&t, 0, 0, 8, 2, 0, false, 0);
    qtest_writeq(t.qts, VIDEO0 + 0x10, (2 << 10) | 2);
    qtest_writeq(t.qts, VIDEO0 + 0x38, 1 | (2 << 10) | (2 << 20));
    qtest_writeq(t.qts, VIDEO0 + 0x10, (2 << 10) | 3);
    field(&t, 1000, 0, 0, 14, 4, 83);
    qtest_clock_step(t.qts, 4400);
    g_assert_cmpint(pread(t.fd, actual, 16, 0x10000), ==, 16);
    g_assert_cmphex(ldq_be_p(actual), ==, 0x8010801080108010ULL);
    stl_be_p(expected, pattern(83, 2, 2));
    stl_be_p(expected + 4, pattern(83, 4, 2));
    g_assert_cmpmem(actual + 8, 8, expected, 8);
    fixture_stop(&t);
}

static void input_pair(void)
{
    VideoFixture t = fixture_start("");
    uint8_t actual[8], expected[8];

    table(&t, 0, 0x4001);
    table(&t, 64, 0x4002);
    capture_setup(&t, 0, 0, 8, 1, 0, false, 2);
    capture_setup(&t, 1, 1, 8, 1, 0, false, 3);
    field(&t, 1000, 0, 1, 14, 3, 89);
    field(&t, 1000, 1, 1, 14, 3, 97);
    qtest_clock_step(t.qts, 3000);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x18) & 4, ==, 4);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO1 + 0x18) & 4, ==, 0);
    stl_be_p(expected, pattern(97, 2, 1));
    stl_be_p(expected + 4, pattern(97, 4, 1));
    g_assert_cmpint(pread(t.fd, actual, 8, 0x20000), ==, 8);
    g_assert_cmpmem(actual, 8, expected, 8);
    field(&t, 10000, 0, 0, 14, 3, 101);
    qtest_clock_step(t.qts, 9000);
    stl_be_p(expected, pattern(101, 2, 1));
    stl_be_p(expected + 4, pattern(101, 4, 1));
    g_assert_cmpint(pread(t.fd, actual, 8, 0x10000), ==, 8);
    g_assert_cmpmem(actual, 8, expected, 8);
    fixture_stop(&t);
}

static void global_reset_final(void)
{
    VideoFixture t = fixture_start("");
    uint8_t actual[16];

    table(&t, 0, 0x4001);
    capture_setup(&t, 0, 0, 16, 1, 0, false, 0);
    field(&t, 1000, 0, 0, 18, 3, 103);
    qtest_clock_step(t.qts, 3799);
    qtest_clock_timers(t.qts, false);
    qtest_clock_step(t.qts, 1);
    qtest_qmp_assert_success(t.qts, "{ 'execute': 'system_reset' }");
    qtest_clock_timers(t.qts, true);
    qtest_clock_step(t.qts, 1000);
    g_assert_cmpint(pread(t.fd, actual, 16, 0x10000), ==, 16);
    g_assert_cmphex(ldq_be_p(actual), !=, 0);
    g_assert_cmphex(ldq_be_p(actual + 8), ==, 0);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x80), ==, 0);
    g_assert_cmphex(qtest_readq(t.qts, TIMER), ==, 0);
    g_assert_false(qtest_get_irq(t.qts, 0));
    fixture_stop(&t);
}

static void migrate_partial(gconstpointer opaque)
{
    bool output = GPOINTER_TO_UINT(opaque);
    static const uint8_t pixels[8] = { 0x44, 0x27, 0xb1, 0x72,
                                      0x61, 0x3a, 0xc7, 0x95 };
    VideoFixture from = fixture_start("");
    VideoFixture to = fixture_start("-incoming defer");
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate", from.dir);
    uint8_t actual[16], other[16], zero[8] = { 0 };
    uint64_t now = output ? pixel_time(65) : 3600;
    uint64_t final = output ? pixel_time(67) : 3800;

    table(&from, 0, 0x4001);
    if (output) {
        g_assert_cmpint(pwrite(from.fd, pixels, 8, 0x10000), ==, 8);
        output_setup(&from, 1);
    } else {
        capture_setup(&from, 0, 0, 16, 1, 0, false, 0);
        field(&from, 1000, 0, 0, 18, 3, 107);
    }
    qtest_clock_step(from.qts, now);
    if (output) {
        g_assert_cmpint(pwrite(from.fd, zero, 8, 0x10000), ==, 8);
    }
    qtest_writeq(from.qts, (output ? VIDEO2 : VIDEO0) + 0x80,
                  0x123456789abcdef0ULL);
    qtest_clock_set(to.qts, now);
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    qtest_clock_step(to.qts, final - now);
    qtest_clock_step(from.qts, final - now);
    g_assert_cmphex(qtest_readq(to.qts, (output ? VIDEO2 : VIDEO0) + 8) & 5,
                    ==, 4);
    g_assert_cmphex(qtest_readq(to.qts, (output ? VIDEO2 : VIDEO0) + 0x80),
                    ==, 0x123456789abcdef0ULL);
    if (output) {
        unsigned count = qtest_readq(to.qts, FIXTURE + 0x58);

        g_assert_cmpuint(count, ==, qtest_readq(from.qts, FIXTURE + 0x58));
        for (unsigned i = count - 8; i < count; i++) {
            uint64_t obs = FIXTURE + 0x1000 + i * 32;

            g_assert_cmphex(qtest_readq(to.qts, obs + 16), ==,
                ldl_be_p(pixels + ((i - (count - 8)) / 4) * 4));
            g_assert_cmphex(qtest_readq(to.qts, obs), ==,
                           qtest_readq(from.qts, obs));
        }
    } else {
        g_assert_cmpint(pread(to.fd, actual, 16, 0x10000), ==, 16);
        /* Source timers stop after migration; use independent fixture bytes. */
        for (unsigned i = 0; i < 4; i++) {
            stl_be_p(other + i * 4, pattern(107, 2 + i * 2, 1));
        }
        g_assert_cmpmem(actual, 16, other, 16);
        g_assert_cmphex(ldq_be_p(actual + 8), !=, 0);
    }
    fixture_stop(&to);
    if (g_file_test(uri + 5, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(unlink(uri + 5), ==, 0);
    }
    fixture_stop(&from);
}

static void output_loopback(void)
{
    static const uint8_t pixels[8] = { 0x4d, 0x24, 0xb3, 0x73,
                                      0x6c, 0x31, 0xc9, 0x97 };
    VideoFixture t = fixture_start("");
    uint8_t actual[8];
    uint32_t cfg;

    table(&t, 0, 0x4001);
    table(&t, 64, 0x4002);
    g_assert_cmpint(pwrite(t.fd, pixels, 8, 0x10000), ==, 8);
    capture_setup(&t, 1, 2, 8, 1, 0, false, 3);
    cfg = output_setup(&t, 1);
    qtest_clock_step(t.qts, pixel_time(67));
    g_assert_cmpint(pread(t.fd, actual, 8, 0x20000), ==, 8);
    g_assert_cmpmem(actual, 8, pixels, 8);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO1 + 8) & 5, ==, 4);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8) & 5, ==, 4);
    g_assert_cmpint(memtx_store(t.qts, VIDEO2 + 0x10, cfg | 32, 8),
                    ==, MEMTX_OK);
    fixture_stop(&t);
}

static void output_reference(gconstpointer opaque)
{
    unsigned reference = GPOINTER_TO_UINT(opaque);
    unsigned source = reference == 1 ? 4 : reference == 2 ? 0 : 1;
    VideoFixture t = fixture_start("");
    uint32_t cfg = (2 << 14) | (2 << 1) | (reference << 6);
    uint64_t h = (5 << 10) | (1 << 20);
    unsigned delay = reference == 1 ? 550 : 800;

    table(&t, 0, 0x4001);
    qtest_writeq(t.qts, VIDEO2 + 0x58, 5);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg);
    qtest_writeq(t.qts, VIDEO2 + 0x28, (1 << 12) | 8);
    qtest_writeq(t.qts, VIDEO2 + 0x30, h);
    qtest_writeq(t.qts, VIDEO2 + 0x40, h);
    qtest_writeq(t.qts, VIDEO2 + 0x38, (2 << 10) | (3 << 20));
    qtest_writeq(t.qts, VIDEO2 + 0x48, (2 << 10) | (3 << 20));
    qtest_writeq(t.qts, VIDEO2 + 0x50, 1);
    qtest_writeq(t.qts, VIDEO2 + 0x18, 0x40000004);
    qtest_writeq(t.qts, VIDEO2, 0x10f);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg | 32);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg | 33);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 0x10), ==, cfg | 33);
    field(&t, 1000, source, 0, 12, 4, 109);
    event(&t, 2900 + delay, source, 0, 0, 0, 0, 0);
    qtest_clock_step(t.qts, 1000 + delay);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8), ==, 3);
    g_assert_cmphex(qtest_readq(t.qts, TIMER + 16), ==,
                   (uint64_t)((1000 + delay) / 960) << 32);
    if (reference == 2) {
        g_assert_cmpint(memtx_store(t.qts, VIDEO2 + 0x58, 4, 8),
                        !=, MEMTX_OK);
        g_assert_cmpint(memtx_store(t.qts, VIDEO2 + 0x50, 2, 8),
                        !=, MEMTX_OK);
    }
    qtest_clock_step(t.qts, 1900);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8), ==, 8);
    g_assert_false(qtest_get_irq(t.qts, 2));
    g_assert_cmphex(qtest_readq(t.qts, TIMER + 16) & UINT32_MAX, ==, 0);
    fixture_stop(&t);
}

static void descriptor_word_boundary(void)
{
    VideoFixture t = fixture_start("");
    uint8_t data[64] = { 0 }, actual[8], expected[8];
    unsigned width = 2048, rows = 50;
    uint64_t last = 65528 + (rows - 1) * (width * 2ULL) + width - 8;
    unsigned first_pixel = width / 2 - 4 + 2;
    uint64_t address;

    for (unsigned i = 0; i < 32; i++) {
        stw_be_p(data + i * 2, 0x4001 + (i * 7) % 32);
    }
    g_assert_cmpint(pwrite(t.fd, data, 64, 0), ==, 64);
    capture_setup(&t, 0, 0, width, rows, 65528, true, 0);
    field(&t, 1000, 0, 0, width / 2 + 10, rows + 2, 113);
    qtest_clock_step(t.qts, 1000 + ((uint64_t)rows *
                     (width / 2 + 10) + first_pixel + 4) * 100);
    address = ((1 + ((last >> 16) * 7) % 32) << 16) | (last & 0xffff);
    stl_be_p(expected, pattern(113, first_pixel, rows));
    stl_be_p(expected + 4, pattern(113, first_pixel + 2, rows));
    g_assert_cmpuint(last >> 16, >=, 4);
    g_assert_cmpint(pread(t.fd, actual, 8, address), ==, 8);
    g_assert_cmpmem(actual, 8, expected, 8);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8) & 5, ==, 4);
    fixture_stop(&t);
}

static void held_input_modes(void)
{
    VideoFixture t = fixture_start("");
    static const uint32_t extra[] = {
        1 << 4, 1 << 5, 1 << 6, 1 << 13, 1 << 14, 2 << 14, 3 << 14,
    };

    table(&t, 0, 0x4001);
    capture_setup(&t, 0, 0, 8, 1, 0, false, 0);
    qtest_writeq(t.qts, VIDEO0 + 0x10, (2 << 10) | 2);
    qtest_writeq(t.qts, VIDEO0 + 0x10, 2 << 10);
    for (unsigned i = 0; i < 7; i++) {
        unsigned fmt = i < 2 ? i : i + 1;
        uint32_t cfg = fmt << 10;

        qtest_writeq(t.qts, VIDEO0 + 0x10, cfg);
        qtest_writeq(t.qts, VIDEO0 + 0x10, cfg | 2);
        g_assert_cmpint(memtx_store(t.qts, VIDEO0 + 0x10, cfg | 3, 8),
                        !=, MEMTX_OK);
        qtest_writeq(t.qts, VIDEO0 + 0x10, cfg);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(extra); i++) {
        uint32_t cfg = (2 << 10) | extra[i];

        qtest_writeq(t.qts, VIDEO0 + 0x10, cfg);
        qtest_writeq(t.qts, VIDEO0 + 0x10, cfg | 2);
        g_assert_cmpint(memtx_store(t.qts, VIDEO0 + 0x10, cfg | 3, 8),
                        !=, MEMTX_OK);
        qtest_writeq(t.qts, VIDEO0 + 0x10, cfg);
    }
    qtest_writeq(t.qts, VIDEO0 + 0x10, (2 << 10) | 2);
    qtest_writeq(t.qts, VIDEO0 + 0x30, (3 << 10) | (1ULL << 38));
    g_assert_cmpint(memtx_store(t.qts, VIDEO0 + 0x10, (2 << 10) | 3, 8),
                    !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x18) & 4, ==, 4);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0);
    fixture_stop(&t);
}

static void revision_refusal(gconstpointer opaque)
{
    unsigned variant = GPOINTER_TO_UINT(opaque);
    const char *args[] = {
        "-M none -nodefaults -object memory-backend-ram,id=ram,size=4M "
        "-device sgi-mace-video-test,ram=/objects/ram/ram[0]",
        "-M none -nodefaults -object memory-backend-ram,id=ram,size=4M "
        "-device sgi-mace-video-test,ram=/objects/ram/ram[0],synthetic-revision=16",
        "-M none -nodefaults -device sgi-mace,synthetic-revision=10",
    };
    QTestState *s = qtest_init_ext("QTEST_QEMU_BINARY", args[variant], NULL,
                                  false);

    qtest_set_expected_status(s, 1);
    qtest_wait_qemu(s);
    qtest_quit(s);
}

static void fifo_input(void)
{
    VideoFixture t = fixture_start("");
    uint8_t actual[16];

    table(&t, 0, 0x4001);
    capture_setup(&t, 0, 0, 16, 1, 0, false, 0);
    field(&t, 1000, 0, 0, 18, 3, 127);
    qtest_clock_step(t.qts, 1000);
    qtest_readq(t.qts, VIDEO0 + 8);
    qtest_clock_step(t.qts, 2500);
    qtest_writeq(t.qts, FIXTURE + 0x40, 0);
    qtest_clock_step(t.qts, 1000);
    g_assert_cmpint(pread(t.fd, actual, 16, 0x10000), ==, 16);
    g_assert_cmphex(ldq_be_p(actual), !=, 0);
    g_assert_cmphex(ldq_be_p(actual + 8), ==, 0);
    g_assert_false(qtest_get_irq(t.qts, 0));
    qtest_writeq(t.qts, VIDEO0, 0x87);
    g_assert_true(qtest_get_irq(t.qts, 0));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0x480);
    g_assert_false(qtest_get_irq(t.qts, 0));
    fixture_stop(&t);
}

static void output_fault(gconstpointer opaque)
{
    unsigned fault = GPOINTER_TO_UINT(opaque);
    VideoFixture t = fixture_start("");
    uint8_t pixels[8] = { 0x50, 0x40, 0xb0, 0x90, 0x60, 0x30, 0xa0, 0x80 };

    table(&t, 0, fault == 0 ? 0 : 0x4001);
    table(&t, 64, 0x4002);
    g_assert_cmpint(pwrite(t.fd, pixels, 8, 0x10000), ==, 8);
    g_assert_cmpint(pwrite(t.fd, pixels, 8, 0x20000), ==, 8);
    output_setup(&t, 1);
    qtest_writeq(t.qts, VIDEO2, 0x19f);
    if (fault == 1) {
        qtest_writeq(t.qts, VIDEO2 + 0x18, 0x00000045);
        qtest_writeq(t.qts, VIDEO2 + 0x80, 0x1122334455667788ULL);
    }
    qtest_clock_step(t.qts, pixel_time(64));
    if (fault == 2) {
        qtest_writeq(t.qts, FIXTURE + 0x40, 2);
    }
    g_assert_true(qtest_get_irq(t.qts, 2));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8), ==,
                   2 | (fault == 0 ? 16 : fault == 1 ? 256 : 128));
    g_assert_false(qtest_get_irq(t.qts, 2));
    if (fault == 1) {
        g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 0x80), ==,
                       0x1122334455667788ULL);
    }
    qtest_writeq(t.qts, VIDEO2 + 0x18, 0x40000045);
    qtest_clock_step(t.qts, pixel_time(108) - pixel_time(64));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 0x18), ==, 0x40000045);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8) & 5, ==, 0);
    qtest_writeq(t.qts, VIDEO2, 0x19e);
    qtest_writeq(t.qts, VIDEO2, 0x19f);
    qtest_clock_step(t.qts, pixel_time(175) - pixel_time(108));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 0x18), ==, 0x40000041);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8) & 5, ==, 4);
    fixture_stop(&t);
}

static void held_output_modes(void)
{
    VideoFixture t = fixture_start("");
    uint32_t base = output_setup(&t, 0);
    static const uint32_t extra[] = { 1 << 17, 1 << 18, 2 << 18,
                                     3 << 18, 1 << 21, 1 << 8, 1 << 9,
                                     1 << 10, 1 << 22 };

    qtest_writeq(t.qts, VIDEO2 + 0x10, base | 32);
    qtest_writeq(t.qts, VIDEO2 + 0x10, base);
    for (unsigned i = 0; i < 7 + ARRAY_SIZE(extra); i++) {
        uint32_t cfg = i < 7 ?
            (base & ~(7 << 14)) | ((i < 2 ? i : i + 1) << 14) :
            base | extra[i - 7];

        qtest_writeq(t.qts, VIDEO2 + 0x10, cfg);
        if (memtx_store(t.qts, VIDEO2 + 0x10, cfg | 32, 8) == MEMTX_OK) {
            g_assert_cmpint(memtx_store(t.qts, VIDEO2 + 0x10, cfg | 33, 8),
                            !=, MEMTX_OK);
        }
        qtest_writeq(t.qts, VIDEO2 + 0x10, cfg);
        g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8) & 5, ==, 0);
    }
    qtest_writeq(t.qts, VIDEO2 + 0x10, base);
    qtest_writeq(t.qts, VIDEO2 + 0x30, (5 << 10) | (1 << 20) | (1ULL << 40));
    g_assert_cmpint(memtx_store(t.qts, VIDEO2 + 0x10, base | 32, 8),
                    !=, MEMTX_OK);
    qtest_writeq(t.qts, VIDEO2 + 0x30, (5 << 10) | (1 << 20) | 0x3ff);
    g_assert_cmpint(memtx_store(t.qts, VIDEO2 + 0x10, base | 32, 8),
                    !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 0x18) & 4, ==, 4);
    fixture_stop(&t);
}

static void output_interleaved(void)
{
    VideoFixture t = fixture_start("");
    uint8_t descriptor[64] = { 0 }, unit[8];
    uint32_t cfg = (2 << 14) | (1 << 20) | (2 << 1);
    uint64_t h = (9 << 10) | (1 << 20);
    unsigned count, matched = 0;

    stw_be_p(descriptor, 0x4001);
    stw_be_p(descriptor + 2, 0x8004);
    g_assert_cmpint(pwrite(t.fd, descriptor, 64, 0), ==, 64);
    for (unsigned row = 0; row < 2; row++) {
        for (unsigned u = 0; u < 2; u++) {
            unsigned addr = row ? 0x40018 + u * 8 : u ? 0x40000 : 0x1fff8;

            stl_be_p(unit, pattern(131, 2 + u * 4, row + 1));
            stl_be_p(unit + 4, pattern(131, 4 + u * 4, row + 1));
            g_assert_cmpint(pwrite(t.fd, unit, 8, addr), ==, 8);
        }
    }
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg);
    qtest_writeq(t.qts, VIDEO2 + 0x20, 65528);
    qtest_writeq(t.qts, VIDEO2 + 0x28, (2 << 12) | 16);
    qtest_writeq(t.qts, VIDEO2 + 0x30, h);
    qtest_writeq(t.qts, VIDEO2 + 0x40, h);
    qtest_writeq(t.qts, VIDEO2 + 0x38, (3 << 10) | (4 << 20));
    qtest_writeq(t.qts, VIDEO2 + 0x48, (4 << 10) | (5 << 20));
    qtest_writeq(t.qts, VIDEO2 + 0x18, 0x40000005);
    qtest_writeq(t.qts, VIDEO2, 7);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg | 32);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg | 33);
    qtest_clock_step(t.qts, pixel_time(151));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8) & 5, ==, 4);
    count = qtest_readq(t.qts, FIXTURE + 0x58);
    for (unsigned i = 0; i < count; i++) {
        uint64_t obs = FIXTURE + 0x1000 + i * 32;
        uint64_t position = qtest_readq(t.qts, obs + 8);
        unsigned x = position >> 32, y = position;

        if (qtest_readq(t.qts, obs + 24) != 0x102 || y < 1 || y > 2 ||
            x < 4 || x >= 12) {
            continue;
        }
        g_assert_cmphex(qtest_readq(t.qts, obs + 16), ==,
                       pattern(131, x - 2, y));
        matched++;
    }
    g_assert_cmpuint(matched, ==, 16);
    fixture_stop(&t);
}

static void output_zero_geometry(gconstpointer opaque)
{
    VideoFixture t = fixture_start("");
    bool zero_lines = GPOINTER_TO_UINT(opaque);

    table(&t, 0, 0x4001);
    output_setup(&t, 1);
    qtest_writeq(t.qts, VIDEO2 + 0x28, zero_lines ? 8 : 1 << 12);
    qtest_writeq(t.qts, VIDEO2 + 0x80, 0x1122334455667788ULL);
    qtest_clock_step(t.qts, pixel_time(48));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 0x18), ==, 0x40000005);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 0x80), ==,
                   0x1122334455667788ULL);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8), ==, 2);
    qtest_writeq(t.qts, VIDEO2 + 0x28, (1 << 12) | 8);
    qtest_clock_step(t.qts, pixel_time(108) - pixel_time(48));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 0x18), ==, 0x40000005);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8), ==, 2);
    qtest_clock_step(t.qts, pixel_time(175) - pixel_time(108));
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 0x18), ==, 0x40000001);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 8) & 5, ==, 4);
    fixture_stop(&t);
}

static void truncated_input(gconstpointer opaque)
{
    bool short_field = GPOINTER_TO_UINT(opaque);
    VideoFixture t = fixture_start("");
    uint8_t actual[16];

    table(&t, 0, 0x4001);
    capture_setup(&t, 0, 0, 8, 2, 0, false, 0);
    event(&t, 1000, 0, 1, 0, 0, 0, 0);
    event(&t, 1000, 0, 4, 0, 14, 4, 0);
    if (short_field) {
        for (unsigned x = 2; x < 6; x++) {
            event(&t, 2000 + x * 100, 0, 2, 0, x, 1,
                  pattern(137, x, 1));
        }
        event(&t, 3000, 0, 3, 0, 14, 2, 0);
    } else {
        event(&t, 2000, 0, 5, 0, 4, 1, 0);
    }
    qtest_clock_step(t.qts, 3000);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==,
                   0x402 | (short_field ? 64 : 32));
    g_assert_cmpint(pread(t.fd, actual, 16, 0x10000), ==, 16);
    g_assert_cmphex(ldq_be_p(actual + 8), ==, 0);
    if (short_field) {
        g_assert_cmphex(ldq_be_p(actual), !=, 0);
    } else {
        g_assert_cmphex(ldq_be_p(actual), ==, 0);
    }
    fixture_stop(&t);
}

static void coincident_events(gconstpointer opaque)
{
    unsigned action = GPOINTER_TO_UINT(opaque);
    VideoFixture t = fixture_start("");
    uint8_t actual[8], expected[8];

    table(&t, 0, 0x4001);
    table(&t, 64, 0x4002);
    capture_setup(&t, 0, 0, 8, 1, 0, false, 0);
    event(&t, 1000, 0, 1, 0, 0, 0, 0);
    event(&t, 1000, 0, 4, 0, 14, 3, 0);
    event(&t, 2300, 0, 4, 1, 14, 3, 0);
    event(&t, 2300, 0, 3, 0, 14, 3, 0);
    for (unsigned x = 2; x < 6; x++) {
        event(&t, 1800 + x * 100, 0, 2, 0, x, 1, pattern(139, x, 1));
        event(&t, 2200 + x * 100, 0, 2, 1, x, 1, pattern(149, x, 1));
    }
    if (action == 1) {
        event(&t, 2300, 0, 0, 0, 0, 0, 0);
    }
    qtest_clock_step(t.qts, 1000);
    qtest_readq(t.qts, VIDEO0 + 8);
    qtest_writeq(t.qts, VIDEO0 + 0x18, 0x40000044);
    if (action == 2) {
        qtest_clock_step(t.qts, 1299);
        qtest_clock_timers(t.qts, false);
        qtest_clock_step(t.qts, 1);
        qtest_qmp_assert_success(t.qts, "{ 'execute': 'system_reset' }");
        qtest_clock_timers(t.qts, true);
    } else {
        qtest_clock_step(t.qts, 1300);
    }
    g_assert_cmpint(pread(t.fd, actual, 8, 0x10000), ==, 8);
    if (action) {
        g_assert_cmphex(ldq_be_p(actual), ==, 0);
        g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==,
                       action == 1 ? 8 : 0);
        g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x18) & 4, ==,
                       action == 1 ? 4 : 0);
    } else {
        stl_be_p(expected, pattern(139, 2, 1));
        stl_be_p(expected + 4, pattern(139, 4, 1));
        g_assert_cmpmem(actual, 8, expected, 8);
        g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0x407);
        g_assert_cmphex(qtest_readq(t.qts, TIMER), ==, (2ULL << 32) | 2);
        g_assert_cmpint(memtx_store(t.qts, VIDEO0 + 0x10, (2 << 10) | 2, 8),
                        !=, MEMTX_OK);
        qtest_clock_step(t.qts, 400);
        stl_be_p(expected, pattern(149, 2, 1));
        stl_be_p(expected + 4, pattern(149, 4, 1));
        g_assert_cmpint(pread(t.fd, actual, 8, 0x20000), ==, 8);
        g_assert_cmpmem(actual, 8, expected, 8);
        g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0x404);
    }
    fixture_stop(&t);
}

static void asymmetric_capture(void)
{
    VideoFixture t = fixture_start("");
    uint8_t actual[16];

    table(&t, 0, 0x4001);
    capture_setup(&t, 0, 0, 8, 1, 40, false, 0);
    qtest_writeq(t.qts, VIDEO0 + 0x10, (2 << 10) | 2);
    qtest_writeq(t.qts, VIDEO0 + 0x30, 4 | (7 << 10));
    qtest_writeq(t.qts, VIDEO0 + 0x38, 2 | (2 << 20));
    qtest_writeq(t.qts, VIDEO0 + 0x10, (2 << 10) | 3);
    field(&t, 1000, 0, 0, 18, 4, 157);
    qtest_clock_step(t.qts, 5600);
    g_assert_cmpint(pread(t.fd, actual, 16, 0x10028), ==, 16);
    g_assert_cmphex(ldq_be_p(actual), ==, 0x4ac9c6624c26cc78ULL);
    g_assert_cmphex(ldq_be_p(actual + 8), ==, 0);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8) & 5, ==, 4);
    fixture_stop(&t);
}

static void output_padding(void)
{
    static const uint8_t image[8] = { 0x42, 0x35, 0xa7, 0x81,
                                     0x67, 0x29, 0xc2, 0x96 };
    VideoFixture t = fixture_start("");
    uint32_t cfg = (2 << 14) | (2 << 1);
    uint64_t h = (7 << 10) | (2 << 20);
    uint64_t v = (3 << 10) | (4 << 20) | (1ULL << 30);
    unsigned count, black = 0, pixels = 0;

    table(&t, 0, 0x4001);
    g_assert_cmpint(pwrite(t.fd, image, 8, 0x10000), ==, 8);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg);
    qtest_writeq(t.qts, VIDEO2 + 0x28, (1 << 12) | 8);
    qtest_writeq(t.qts, VIDEO2 + 0x30, h);
    qtest_writeq(t.qts, VIDEO2 + 0x40, h);
    qtest_writeq(t.qts, VIDEO2 + 0x38, v);
    qtest_writeq(t.qts, VIDEO2 + 0x48, v);
    qtest_writeq(t.qts, VIDEO2 + 0x18, 0x40000005);
    qtest_writeq(t.qts, VIDEO2, 7);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg | 32);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg | 33);
    qtest_clock_step(t.qts, pixel_time(127));
    count = qtest_readq(t.qts, FIXTURE + 0x58);
    for (unsigned i = 0; i < count; i++) {
        uint64_t obs = FIXTURE + 0x1000 + i * 32;
        uint64_t position = qtest_readq(t.qts, obs + 8);
        unsigned x = position >> 32, y = position;

        if (qtest_readq(t.qts, obs + 24) != 0x102 || y > 2) {
            continue;
        }
        if (y == 2 && x >= 6 && x < 10) {
            g_assert_cmphex(qtest_readq(t.qts, obs + 16), ==,
                           ldl_be_p(image + ((x - 6) / 2) * 4));
            pixels++;
        } else {
            g_assert_cmphex(qtest_readq(t.qts, obs + 16), ==, 0x80108010);
            black++;
        }
    }
    g_assert_cmpuint(pixels, ==, 4);
    g_assert_cmpuint(black, ==, 20);
    fixture_stop(&t);
}

static void absent_signal(void)
{
    VideoFixture t = fixture_start("");
    uint32_t cfg = (2 << 14) | (2 << 1) | (2 << 6);
    uint64_t h = (5 << 10) | (1 << 20);

    table(&t, 0, 0x4001);
    capture_setup(&t, 0, 0, 8, 1, 0, false, 0);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg);
    qtest_writeq(t.qts, VIDEO2 + 0x28, (1 << 12) | 8);
    qtest_writeq(t.qts, VIDEO2 + 0x30, h);
    qtest_writeq(t.qts, VIDEO2 + 0x40, h);
    qtest_writeq(t.qts, VIDEO2 + 0x38, (2 << 10) | (3 << 20));
    qtest_writeq(t.qts, VIDEO2 + 0x48, (2 << 10) | (3 << 20));
    qtest_writeq(t.qts, VIDEO2 + 0x50, 1);
    qtest_writeq(t.qts, VIDEO2 + 0x18, 0x40000004);
    qtest_writeq(t.qts, VIDEO2, 7);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg | 32);
    qtest_writeq(t.qts, VIDEO2 + 0x10, cfg | 33);
    qtest_clock_step(t.qts, 1000000);
    for (unsigned i = 0; i < 3; i++) {
        uint64_t base = VIDEO0 + i * 0x80000;

        g_assert_cmphex(qtest_readq(t.qts, TIMER + i * 8), ==, 0);
        g_assert_cmphex(qtest_readq(t.qts, base + 8), ==, 0);
    }
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0x18) & 4, ==, 4);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO2 + 0x18) & 4, ==, 4);
    g_assert_cmpuint(qtest_readq(t.qts, FIXTURE + 0x58), ==, 0);
    fixture_stop(&t);
}

static void last_descriptor_byte(gconstpointer opaque)
{
    bool interleaved = GPOINTER_TO_UINT(opaque);
    unsigned rows = interleaved ? 512 : 1023;
    VideoFixture t = fixture_start("");
    uint8_t descriptor[64], actual[8], expected[8], guard[8];
    int64_t final = 1000 + ((int64_t)rows * 1034 + 1026) * 100;

    for (unsigned i = 0; i < 32; i++) {
        stw_be_p(descriptor + i * 2, 0x4001 + i);
    }
    memset(guard, 0x59, sizeof(guard));
    g_assert_cmpint(pwrite(t.fd, descriptor, 64, 0), ==, 64);
    g_assert_cmpint(pwrite(t.fd, guard, 8, 0x210000), ==, 8);
    capture_setup(&t, 0, 0, 2048, rows, 2048, interleaved, 0);
    field(&t, 1000, 0, 0, 1034, rows + 2, 163);
    qtest_clock_step(t.qts, final);
    stl_be_p(expected, pattern(163, 1022, rows));
    stl_be_p(expected + 4, pattern(163, 1024, rows));
    g_assert_cmpint(pread(t.fd, actual, 8, 0x20fff8), ==, 8);
    g_assert_cmpmem(actual, 8, expected, 8);
    g_assert_cmpint(pread(t.fd, actual, 8, 0x210000), ==, 8);
    g_assert_cmpmem(actual, 8, guard, 8);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8), ==, 0x406);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 0xb8), ==,
                   0x401d401e401f4020ULL);
    fixture_stop(&t);
}

static void stale_input_events(void)
{
    VideoFixture t = fixture_start("");
    uint8_t actual[8], expected[8];

    table(&t, 0, 0x4001);
    table(&t, 64, 0x4002);
    capture_setup(&t, 0, 0, 8, 1, 0, false, 0);
    qtest_writeq(t.qts, FIXTURE + 0x38, 1);
    event(&t, 1000, 0, 1, 0, 0, 0, 0);
    event(&t, 1000, 0, 4, 0, 14, 3, 0);
    event(&t, 3500, 0, 2, 0, 5, 1, pattern(167, 5, 1));
    event(&t, 4000, 0, 3, 0, 14, 3, 0);
    qtest_clock_step(t.qts, 1500);
    qtest_writeq(t.qts, VIDEO0, 6);
    qtest_writeq(t.qts, VIDEO0 + 0x18, 0x40000044);
    qtest_writeq(t.qts, VIDEO0, 7);
    qtest_writeq(t.qts, FIXTURE + 0x38, 2);
    event(&t, 3000, 0, 4, 0, 14, 3, 0);
    for (unsigned x = 2; x < 6; x++) {
        event(&t, x == 5 ? 3600 : 3000 + x * 100, 0, 2, 0, x, 1,
              pattern(173, x, 1));
    }
    event(&t, 4500, 0, 3, 0, 14, 3, 0);
    qtest_clock_step(t.qts, 2050);
    g_assert_cmpint(pread(t.fd, actual, 8, 0x20000), ==, 8);
    g_assert_cmphex(ldq_be_p(actual), ==, 0);
    g_assert_cmphex(qtest_readq(t.qts, VIDEO0 + 8) & 5, ==, 1);
    qtest_clock_step(t.qts, 50);
    stl_be_p(expected, pattern(173, 2, 1));
    stl_be_p(expected + 4, pattern(173, 4, 1));
    g_assert_cmpint(pread(t.fd, actual, 8, 0x20000), ==, 8);
    g_assert_cmpmem(actual, 8, expected, 8);
    qtest_clock_step(t.qts, 400);
    g_assert_cmphex(qtest_readq(t.qts, TIMER), ==, 3ULL << 32);
    qtest_clock_step(t.qts, 500);
    g_assert_cmphex(qtest_readq(t.qts, TIMER), ==, (3ULL << 32) | 2);
    fixture_stop(&t);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/mace/video/timer-layout", timer_layout);
    qtest_add_func("/sgi/mace/video/registers", registers);
    qtest_add_func("/sgi/mace/video/signal-counters", signal_counters);
    qtest_add_func("/sgi/mace/video/migration-signal", migration_signal);
    qtest_add_func("/sgi/mace/video/loss-counter-wrap", loss_and_counter_wrap);
    qtest_add_func("/sgi/mace/video/loss-irq-idle", loss_irq_without_field);
    qtest_add_func("/sgi/mace/video/synthetic-revision", synthetic_revision);
    qtest_add_func("/sgi/mace/video/capture-small", capture_small);
    qtest_add_func("/sgi/mace/video/capture-interleaved", capture_interleaved);
    qtest_add_func("/sgi/mace/video/queued-isolation", queued_isolation);
    qtest_add_data_func("/sgi/mace/video/disable-before-final",
                        NULL, capture_cancel);
    qtest_add_data_func("/sgi/mace/video/loss-at-final",
                        GUINT_TO_POINTER(1), capture_cancel);
    qtest_add_data_func("/sgi/mace/video/disable-at-final",
                        GUINT_TO_POINTER(2), capture_cancel);
    qtest_add_func("/sgi/mace/video/output-timed", output_timed);
    qtest_add_data_func("/sgi/mace/video/zero-page",
                        NULL, capture_fault);
    qtest_add_data_func("/sgi/mace/video/table-fault",
                        GUINT_TO_POINTER(1), capture_fault);
    qtest_add_data_func("/sgi/mace/video/pixel-fault-prefix",
                        GUINT_TO_POINTER(2), capture_fault);
    qtest_add_data_func("/sgi/mace/video/partial-unit-fault",
                        GUINT_TO_POINTER(3), capture_fault);
    qtest_add_func("/sgi/mace/video/restart-no-ecc", capture_restart);
    qtest_add_data_func("/sgi/mace/video/span-field",
                        NULL, span_boundary);
    qtest_add_data_func("/sgi/mace/video/span-interleaved",
                        GUINT_TO_POINTER(1), span_boundary);
    qtest_add_func("/sgi/mace/video/black-padding", capture_padding);
    qtest_add_func("/sgi/mace/video/two-inputs-parity", input_pair);
    qtest_add_func("/sgi/mace/video/reset-at-final", global_reset_final);
    qtest_add_data_func("/sgi/mace/video/migration-input-partial",
                        NULL, migrate_partial);
    qtest_add_data_func("/sgi/mace/video/migration-output-partial",
                        GUINT_TO_POINTER(1), migrate_partial);
    qtest_add_func("/sgi/mace/video/output-loopback", output_loopback);
    qtest_add_data_func("/sgi/mace/video/reference-external",
                        GUINT_TO_POINTER(1), output_reference);
    qtest_add_data_func("/sgi/mace/video/reference-primary",
                        GUINT_TO_POINTER(2), output_reference);
    qtest_add_data_func("/sgi/mace/video/reference-secondary",
                        GUINT_TO_POINTER(3), output_reference);
    qtest_add_func("/sgi/mace/video/descriptor-word-boundary",
                   descriptor_word_boundary);
    qtest_add_func("/sgi/mace/video/held-input-modes", held_input_modes);
    qtest_add_data_func("/sgi/mace/video/revision-missing",
                        NULL, revision_refusal);
    qtest_add_data_func("/sgi/mace/video/revision-range",
                        GUINT_TO_POINTER(1), revision_refusal);
    qtest_add_data_func("/sgi/mace/video/ordinary-synthetic-refused",
                        GUINT_TO_POINTER(2), revision_refusal);
    qtest_add_func("/sgi/mace/video/fifo-input", fifo_input);
    qtest_add_data_func("/sgi/mace/video/output-zero",
                        NULL, output_fault);
    qtest_add_data_func("/sgi/mace/video/output-table-fault",
                        GUINT_TO_POINTER(1), output_fault);
    qtest_add_data_func("/sgi/mace/video/output-fifo",
                        GUINT_TO_POINTER(2), output_fault);
    qtest_add_func("/sgi/mace/video/held-output-modes", held_output_modes);
    qtest_add_func("/sgi/mace/video/output-interleaved", output_interleaved);
    qtest_add_data_func("/sgi/mace/video/output-zero-width",
                        NULL, output_zero_geometry);
    qtest_add_data_func("/sgi/mace/video/output-zero-lines",
                        GUINT_TO_POINTER(1), output_zero_geometry);
    qtest_add_data_func("/sgi/mace/video/short-line",
                        NULL, truncated_input);
    qtest_add_data_func("/sgi/mace/video/short-field",
                        GUINT_TO_POINTER(1), truncated_input);
    qtest_add_data_func("/sgi/mace/video/coincident-field-events",
                        NULL, coincident_events);
    qtest_add_data_func("/sgi/mace/video/coincident-loss",
                        GUINT_TO_POINTER(1), coincident_events);
    qtest_add_data_func("/sgi/mace/video/coincident-reset",
                        GUINT_TO_POINTER(2), coincident_events);
    qtest_add_func("/sgi/mace/video/asymmetric-capture", asymmetric_capture);
    qtest_add_func("/sgi/mace/video/output-padding", output_padding);
    qtest_add_func("/sgi/mace/video/absent-signal", absent_signal);
    qtest_add_data_func("/sgi/mace/video/last-byte-field",
                        NULL, last_descriptor_byte);
    qtest_add_data_func("/sgi/mace/video/last-byte-interleaved",
                        GUINT_TO_POINTER(1), last_descriptor_byte);
    qtest_add_func("/sgi/mace/video/stale-input-events", stale_input_events);
    return g_test_run();
}
