/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Firmware-free tests of the selected MACE/AD1843 functional contract. */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-migrate.h"
#include "memtx.h"

#define AUDIO 0x1f300000ULL
#define FIXTURE 0x1f3f0000ULL
#define PATH "/machine/peripheral/audio"

typedef struct AudioFixture {
    QTestState *qts;
    char *dir;
    char *ram;
    int fd;
} AudioFixture;

static const uint16_t reset_words[32] = {
    0xc001, 0, 0, 0x8888, 0x8888, 0x8888, 0x8888, 0x8888,
    0x8868, 0x8888, 0x8888, 0, 0, 0x8080, 0x8080, 0,
    0x00ff, 0xbb80, 0, 0x00ff, 0xbb80, 0, 0x00ff, 0xbb80,
    0, 0, 0, 0x00c0, 0xc400, 0, 0, 0,
};

static const uint16_t masks[32] = {
    0xc00f, 0x030f, 0xffff, 0x9f9f, 0x9f9f, 0x9f9f, 0x9f9f, 0x9f9f,
    0x9ffb, 0xbfbf, 0xbfbf, 0xbfbf, 0xbfbf, 0xbfbf, 0xbfbf, 0x0f0f,
    0xfcff, 0xffff, 0x01ff, 0xfcff, 0xffff, 0x01ff, 0xfcff, 0xffff,
    0x01ff, 0xc3f3, 0xcfff, 0x93d3, 0xffff, 0, 0, 0,
};

static AudioFixture fixture_start_config(const char *extra, bool o2)
{
    AudioFixture t = { 0 };

    t.dir = g_dir_make_tmp("mace-audio-XXXXXX", NULL);
    g_assert_nonnull(t.dir);
    t.ram = g_build_filename(t.dir, "ram", NULL);
    t.fd = g_open(t.ram, O_CREAT | O_EXCL | O_RDWR, 0600);
    g_assert_cmpint(t.fd, >=, 0);
    g_assert_cmpint(ftruncate(t.fd, o2 ? 1ULL << 30 : 64 * 1024), ==, 0);
    if (o2) {
        t.qts = qtest_initf("-M o2-test,bank-profile=8x128m-synthetic,"
            "crime-profile=1.5,memory-backend=ram -m 1G -cpu R10000 "
            "-nodefaults -object memory-backend-file,id=ram,size=1G,"
            "mem-path=%s,share=on %s", t.ram, extra);
        return t;
    }
    t.qts = qtest_initf("-M none -cpu R10000 -nodefaults "
        "-object memory-backend-file,id=ram,size=64K,mem-path=%s,share=on "
        "-device sgi-mace-audio-test,id=audio,ram=/objects/ram/ram[0] "
        MEMTX_TEST_DEVICE " %s", t.ram, extra);
    qtest_sysbus_mmio_map(t.qts, PATH, 0, AUDIO);
    qtest_sysbus_mmio_map(t.qts, PATH, 1, FIXTURE);
    qtest_irq_intercept_out(t.qts, PATH);
    qtest_writeq(t.qts, FIXTURE + 0x188, 0x40000000);
    return t;
}

static AudioFixture fixture_start(const char *extra)
{
    return fixture_start_config(extra, false);
}

static void fixture_stop(AudioFixture *t)
{
    qtest_quit(t->qts);
    close(t->fd);
    g_assert_cmpint(unlink(t->ram), ==, 0);
    g_assert_cmpint(rmdir(t->dir), ==, 0);
    g_free(t->ram);
    g_free(t->dir);
}

static uint16_t raw(AudioFixture *t, unsigned reg)
{
    return qtest_readq(t->qts, FIXTURE + 0x200 + reg * 8);
}

static uint64_t observe(AudioFixture *t, unsigned offset)
{
    return qtest_readq(t->qts, FIXTURE + offset);
}

static void step(AudioFixture *t, int64_t ns)
{
    qtest_clock_step(t->qts, ns);
}

static void next_serial(AudioFixture *t)
{
    uint64_t now = observe(t, 0x180);
    uint64_t next = observe(t, 0x178);

    g_assert_cmpuint(next, !=, INT64_MAX);
    g_assert_cmpuint(next, >=, now);
    step(t, next - now);
}

static void before_stage(AudioFixture *t, unsigned stage, unsigned group)
{
    for (unsigned i = 0; i < 24; i++) {
        if (observe(t, 0x160) == group * 8 + stage) {
            return;
        }
        next_serial(t);
    }
    g_assert_not_reached();
}

static void codec_set(AudioFixture *t, unsigned reg, uint16_t value)
{
    qtest_writeq(t->qts, AUDIO + 8, (reg << 17) | value);
    step(t, 80000);
}

static uint16_t codec_get(AudioFixture *t, unsigned reg)
{
    qtest_writeq(t->qts, AUDIO + 8, (reg << 17) | 0x10000);
    step(t, 80000);
    return qtest_readq(t->qts, AUDIO + 0x18);
}

static void codec_init(AudioFixture *t)
{
    step(t, 100);
    qtest_writeq(t->qts, AUDIO, 0);
    step(t, 600000);
    g_assert_cmphex(raw(t, 0), ==, 0x4001);
    g_assert_cmphex(qtest_readq(t->qts, AUDIO), ==, 2);
}

static void codec_power(AudioFixture *t)
{
    codec_init(t);
    codec_set(t, 28, 0x4400);
    step(t, 474000000);
    g_assert_cmphex(raw(t, 0), ==, 1);
}

static void memory_pair(AudioFixture *t, unsigned offset,
                         uint32_t left, uint32_t right)
{
    uint8_t data[8];

    stl_be_p(data, left);
    stl_be_p(data + 4, right);
    g_assert_cmpint(pwrite(t->fd, data, sizeof(data), offset), ==,
                    sizeof(data));
}

static void controller_registers(void)
{
    AudioFixture t = fixture_start("");
    uint64_t value;

    for (unsigned alias = 0; alias < 4; alias++) {
        uint64_t addr = AUDIO + alias * 0x1780;

        g_assert_cmphex(qtest_readq(t.qts, addr), ==, 1);
        g_assert_cmphex(qtest_readq(t.qts, addr + 8), ==, 0x10000);
        g_assert_cmphex(qtest_readl(t.qts, addr), ==, 0);
        g_assert_cmphex(qtest_readl(t.qts, addr + 4), ==, 1);
        for (unsigned reg = 2; reg < 16; reg++) {
            g_assert_cmphex(qtest_readq(t.qts, addr + 8 * reg), ==,
                            reg >= 4 && !(reg & 3) ? 0x400 : 0);
        }
    }
    qtest_writeq(t.qts, FIXTURE + 0x148, 3);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO), ==, 0x1800001);
    qtest_writel(t.qts, AUDIO, 0);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO), ==, 0x1800001);
    qtest_writel(t.qts, AUDIO + 4, 0x1000001);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO), ==, 0x1000001);
    qtest_writeq(t.qts, AUDIO + 0x18, 0xffff);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x18), ==, 0);
    qtest_writeq(t.qts, AUDIO + 0x10, UINT64_MAX);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x10), ==, 0xffff);
    for (unsigned size = 1; size <= 2; size++) {
        memtx_store(t.qts, AUDIO + 4, 0, size);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO), ==, 0x1000001);
    }
    memtx_store(t.qts, AUDIO + 5, 0, 4);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO), ==, 0x1000001);
    memtx_load(t.qts, AUDIO + 3, &value, 8);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x18), ==, 0);
    fixture_stop(&t);
}

static void codec_registers(void)
{
    AudioFixture t = fixture_start("");

    for (unsigned reg = 0; reg < 32; reg++) {
        g_assert_cmphex(raw(&t, reg), ==, reset_words[reg]);
    }
    qtest_writeq(t.qts, AUDIO + 8, (2 << 17) | 0xffff);
    step(&t, 1000000);
    g_assert_cmphex(raw(&t, 2), ==, 0);
    codec_init(&t);
    codec_set(&t, 2, 0xffff);
    g_assert_cmphex(raw(&t, 2), ==, 0);
    codec_set(&t, 28, 0x7c00);
    step(&t, 474000000);
    for (unsigned reg = 2; reg < 32; reg++) {
        if (reg == 27 || reg == 28) {
            continue;
        }
        codec_set(&t, reg, 0xffff);
        g_assert_cmphex(raw(&t, reg) & ~masks[reg], ==, 0);
        if (reg == 2 || (reg >= 11 && reg <= 26 &&
                         reg != 18 && reg != 21 && reg != 24)) {
            g_assert_cmphex(raw(&t, reg), ==, masks[reg]);
        }
    }
    for (unsigned reg = 29; reg < 32; reg++) {
        g_assert_cmphex(codec_get(&t, reg), ==, 0);
    }
    qtest_writeq(t.qts, AUDIO + 8, (64 << 17) | 0x1234);
    step(&t, 80000);
    g_assert_cmphex(raw(&t, 0), ==, 1);
    codec_set(&t, 25, 0);
    codec_set(&t, 26, 0);
    codec_set(&t, 27, 0);
    for (unsigned reg = 3; reg <= 10; reg++) {
        codec_set(&t, reg, 0);
        g_assert_cmphex(raw(&t, reg) & (reg == 8 ? 0xff00 : 0xffff), ==,
                        reset_words[reg] & (reg == 8 ? 0xff00 : 0xffff));
    }
    fixture_stop(&t);
}

static void power_timing(void)
{
    AudioFixture t = fixture_start("");
    uint64_t time;

    step(&t, 100);
    qtest_writeq(t.qts, AUDIO, 0);
    for (unsigned i = 0; i < 32; i++) {
        g_assert_cmphex(qtest_readq(t.qts, AUDIO), ==, 0);
    }
    step(&t, 599999);
    g_assert_cmphex(raw(&t, 0), ==, 0xc001);
    step(&t, 1);
    g_assert_cmphex(raw(&t, 0), ==, 0x4001);
    before_stage(&t, 5, 0);
    qtest_writeq(t.qts, AUDIO + 8, 0x10000);
    /* Latch a power write and observe its commit deadline exactly. */
    before_stage(&t, 1, 1);
    qtest_writeq(t.qts, AUDIO + 8, (28 << 17) | 0x0400);
    next_serial(&t);
    before_stage(&t, 5, 1);
    time = observe(&t, 0x178);
    next_serial(&t);
    g_assert_cmphex(raw(&t, 28) & 0xc000, ==, 0x4000);
    codec_set(&t, 28, 0x8400);
    g_assert_cmphex(raw(&t, 28) & 0x8000, ==, 0);
    /* Stop retrying the rejected write before startup completes. */
    qtest_writeq(t.qts, AUDIO + 8, 0x10000);
    step(&t, time + 474000000 - observe(&t, 0x180) - 1);
    g_assert_cmphex(raw(&t, 0) & 0x4000, ==, 0x4000);
    step(&t, 1);
    g_assert_cmphex(raw(&t, 0), ==, 1);
    codec_set(&t, 28, 0x8400);
    g_assert_cmphex(raw(&t, 0), ==, 0x4001);
    step(&t, 5000000);
    codec_set(&t, 28, 0x0400);
    step(&t, 470000000);
    g_assert_cmphex(raw(&t, 0), ==, 1);
    qtest_writeq(t.qts, AUDIO, 1);
    g_assert_cmphex(raw(&t, 0), ==, 0xc001);
    step(&t, 100);
    qtest_writeq(t.qts, AUDIO, 0);
    step(&t, 600000);
    codec_set(&t, 28, 0x0400);
    step(&t, 100000000);
    qtest_writeq(t.qts, AUDIO, 1);
    step(&t, 500000000);
    g_assert_cmphex(raw(&t, 0), ==, 0xc001);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO), ==, 1);
    fixture_stop(&t);
}

static void response_pipeline(void)
{
    AudioFixture t = fixture_start("");
    uint16_t before;

    codec_power(&t);
    codec_set(&t, 2, 0x1234);
    codec_get(&t, 17);
    before = qtest_readq(t.qts, AUDIO + 0x18);
    before_stage(&t, 1, 0);
    qtest_writeq(t.qts, AUDIO + 8, (2 << 17) | 0xabcd);
    next_serial(&t);
    qtest_writeq(t.qts, AUDIO + 8, (17 << 17) | 0x10000);
    g_assert_cmphex(raw(&t, 2), ==, 0x1234);
    for (unsigned i = 0; i < 64; i++) {
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x18), ==, before);
    }
    before_stage(&t, 5, 0);
    next_serial(&t);
    g_assert_cmphex(raw(&t, 2), ==, 0xabcd);
    before_stage(&t, 2, 1);
    next_serial(&t);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x18), ==, 0x1234);
    qtest_writeq(t.qts, AUDIO + 0x10, 0x1000);
    g_assert_cmphex(observe(&t, 0x190) & 1, ==, 1);
    qtest_writeq(t.qts, FIXTURE + 0x198, 1);
    g_assert_true(qtest_get_irq(t.qts, 0));
    qtest_writeq(t.qts, AUDIO + 0x10, 0);
    g_assert_false(qtest_get_irq(t.qts, 0));
    codec_set(&t, 26, 0x40);
    g_assert_cmphex(raw(&t, 26), ==, 0x40);
    codec_set(&t, 26, 0xe0);
    g_assert_cmpuint(observe(&t, 0x158), ==, 16384000);
    fixture_stop(&t);
}

static void thresholds(void)
{
    AudioFixture t = fixture_start("");

    for (unsigned channel = 0; channel < 3; channel++) {
        unsigned control = 0x20 + 0x20 * channel;

        for (unsigned mode = 0; mode < 8; mode++) {
            for (unsigned blocks = 0; blocks <= 127; blocks++) {
                bool expected = mode >= 1 && mode <= 3 ?
                    (channel ? blocks < 32 * mode : blocks >= 32 * mode) :
                    mode == 4 ? blocks == 0 : mode == 5 ? blocks != 0 :
                    mode == 6 ? blocks == 127 : mode == 7 ? blocks != 127 :
                    false;

                qtest_writeq(t.qts, AUDIO + control, mode << 5);
                qtest_writeq(t.qts, AUDIO + control + 8, 0);
                qtest_writeq(t.qts, AUDIO + control + 16, blocks * 32);
                g_assert_cmphex(qtest_readq(t.qts, AUDIO + control + 24), ==,
                                blocks * 32);
                g_assert_cmpint(!!(observe(&t, 0x190) &
                                   (1 << (2 + channel * 2))), ==, expected);
            }
        }
        qtest_writeq(t.qts, AUDIO + control, 0x400);
        qtest_writeq(t.qts, AUDIO + control + 8, 0xfe0);
        qtest_writeq(t.qts, AUDIO + control + 16, 0xfe0);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + control + 8), ==, 0);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + control + 16), ==, 0);
        g_assert_cmphex(observe(&t, 0x190) & (3 << (2 + channel * 2)), ==, 0);
    }
    qtest_writeq(t.qts, AUDIO + 0x20, 0);
    qtest_writeq(t.qts, FIXTURE + 0x188, 0x80000000);
    g_assert_cmphex(observe(&t, 0x188), ==, 0x40000000);
    qtest_writeq(t.qts, FIXTURE + 0x188, 0x40000001);
    g_assert_cmphex(observe(&t, 0x188), ==, 0x40000001);
    fixture_stop(&t);
}

static void rates_and_domains(void)
{
    AudioFixture t = fixture_start("");

    codec_power(&t);
    codec_set(&t, 26, 0x10);
    codec_set(&t, 28, 0x7c00);
    codec_set(&t, 17, 12000);
    codec_set(&t, 20, 24000);
    codec_set(&t, 23, 32000);
    codec_set(&t, 15, 0x0919);
    g_assert_cmphex(raw(&t, 15), ==, 0x0909);
    codec_set(&t, 27, 0x303);
    g_assert_cmphex(raw(&t, 27), ==, 0xc0);
    codec_set(&t, 15, 0x0905);
    codec_set(&t, 27, 0x303);
    g_assert_cmphex(raw(&t, 27), ==, 0x303);
    codec_set(&t, 27, 0x301);
    g_assert_cmphex(raw(&t, 27), ==, 0x303);
    codec_set(&t, 27, 0x300);
    g_assert_cmphex(raw(&t, 27), ==, 0x300);
    codec_set(&t, 25, 0x8000);
    g_assert_cmphex(raw(&t, 25), ==, 0);
    codec_set(&t, 27, 0);
    codec_set(&t, 25, 0x8000);
    codec_set(&t, 27, 0x200);
    g_assert_cmphex(raw(&t, 27), ==, 0);
    codec_set(&t, 25, 0);
    codec_set(&t, 26, 0);
    codec_set(&t, 15, 0x0101);
    codec_set(&t, 17, 49000);
    codec_set(&t, 27, 0x100);
    g_assert_cmphex(raw(&t, 27), ==, 0x100);
    codec_set(&t, 17, 49001);
    g_assert_cmpuint(raw(&t, 17), ==, 49000);
    codec_set(&t, 27, 0x8100);
    codec_set(&t, 17, 54000);
    g_assert_cmpuint(raw(&t, 17), ==, 54000);
    codec_set(&t, 17, 54001);
    g_assert_cmpuint(raw(&t, 17), ==, 54000);
    codec_set(&t, 27, 0);
    codec_set(&t, 16, 0x8000);
    codec_set(&t, 27, 0x100);
    g_assert_cmphex(raw(&t, 27), ==, 0);
    codec_set(&t, 16, 0);
    codec_set(&t, 17, 24000);
    for (unsigned converter = 0; converter < 4; converter++) {
        unsigned bit = converter < 2 ? converter : converter + 6;
        unsigned selector_shift = converter < 2 ? converter * 2 :
                                  8 + (converter - 2) * 2;

        codec_set(&t, 27, 0);
        codec_set(&t, 15, 1 << selector_shift);
        codec_set(&t, 25, 1 << bit);
        codec_set(&t, 27, 1 << bit);
        g_assert_cmphex(raw(&t, 27), ==, 1 << bit);
        codec_set(&t, 17, 24001);
        g_assert_cmpuint(raw(&t, 17), ==, 24000);
        codec_set(&t, 27, 0);
        codec_set(&t, 17, 24001);
        codec_set(&t, 27, 1 << bit);
        g_assert_cmphex(raw(&t, 27), ==, 0);
        codec_set(&t, 17, 24000);
        codec_set(&t, 16, 0x0800);
        codec_set(&t, 27, 1 << bit);
        g_assert_cmphex(raw(&t, 27), ==, 0);
        codec_set(&t, 16, 0);
        codec_set(&t, 25, 0);
    }
    fixture_stop(&t);
}

static void serial_start_and_status(void)
{
    AudioFixture t = fixture_start("");
    uint64_t start_frame;
    uint16_t snapshot;

    codec_power(&t);
    codec_set(&t, 26, 0x100);
    before_stage(&t, 1, 0);
    qtest_writeq(t.qts, AUDIO + 8, (27 << 17) | 0x100);
    next_serial(&t);
    before_stage(&t, 5, 0);
    next_serial(&t);
    start_frame = observe(&t, 0x150);
    g_assert_cmpuint((observe(&t, 0x170) >> 16) & 255, ==, 6);
    while (observe(&t, 0x150) < start_frame + 5) {
        next_serial(&t);
    }
    g_assert_cmpuint((observe(&t, 0x170) >> 16) & 255, ==, 1);
    g_assert_cmphex(raw(&t, 1) & 0x100, ==, 0);
    while (observe(&t, 0x150) < start_frame + 6) {
        next_serial(&t);
    }
    g_assert_cmpuint((observe(&t, 0x170) >> 16) & 255, ==, 0);
    codec_set(&t, 27, 0);
    codec_set(&t, 26, 0x40);
    codec_set(&t, 27, 3);
    qtest_writeq(t.qts, FIXTURE, 35000);
    qtest_writeq(t.qts, FIXTURE + 8, 30000);
    step(&t, 1000000);
    g_assert_cmphex(raw(&t, 1) & 15, ==, 6);
    before_stage(&t, 1, 0);
    snapshot = raw(&t, 1);
    qtest_writeq(t.qts, AUDIO + 8, (1 << 17) | 0x10000);
    next_serial(&t);
    g_assert_cmphex(raw(&t, 1), ==, 0);
    qtest_writeq(t.qts, AUDIO + 8, (17 << 17) | 0x10000);
    before_stage(&t, 2, 0);
    next_serial(&t);
    before_stage(&t, 2, 0);
    next_serial(&t);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x18), ==, snapshot);
    g_assert_cmphex(raw(&t, 1) & 15, ==, 6);
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        before_stage(&t, 1, 0);
        qtest_writeq(t.qts, AUDIO + 8, (1 << 17) |
                      (attempt ? 0xffff : 0));
        next_serial(&t);
        qtest_writeq(t.qts, AUDIO + 8, (17 << 17) | 0x10000);
        before_stage(&t, 5, 0);
        snapshot = raw(&t, 1);
        next_serial(&t);
        g_assert_cmphex(raw(&t, 1), ==, 0);
        before_stage(&t, 2, 0);
        next_serial(&t);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x18), ==, snapshot);
        step(&t, 50000);
        g_assert_cmphex(raw(&t, 1) & 15, ==, 6);
    }
    codec_set(&t, 27, 0);
    g_assert_cmphex(raw(&t, 1), ==, 0);
    fixture_stop(&t);
}

static void dma_formats(void)
{
    AudioFixture t = fixture_start("");

    codec_power(&t);
    for (unsigned format = 0; format < 4; format++) {
        uint16_t left = format == 0 ? 0xe100 : format == 1 ? 12345 :
                        format == 2 ? 0x9b00 : 0xb600;
        uint16_t right = format == 0 ? 0x2600 : format == 1 ? -23456 :
                         format == 2 ? 0x1b00 : 0x3600;
        uint8_t data[32];

        for (unsigned i = 0; i < 127 * 4; i++) {
            memory_pair(&t, 0x1000 + i * 8,
                          ((uint32_t)(int32_t)(int16_t)left << 8) | 0x12,
                          ((uint32_t)(int32_t)(int16_t)right << 8) | 0x34);
        }
        codec_set(&t, 25, 0xa0);
        codec_set(&t, 26, 0x50 | (format << 8) | format | (format << 2));
        qtest_writeq(t.qts, AUDIO + 0x40, 0);
        qtest_writeq(t.qts, AUDIO + 0x50, 0xfe0);
        qtest_writeq(t.qts, AUDIO + 0x40, 0x200);
        qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
        codec_set(&t, 27, 0x103);
        step(&t, 2000000);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x48), !=, 0);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), !=, 0);
        g_assert_cmpint(pread(t.fd, data, sizeof(data), 0x100), ==,
                        sizeof(data));
        for (unsigned i = 0; i < 4; i++) {
            uint32_t got_left = ldl_be_p(data + i * 8);
            uint32_t got_right = ldl_be_p(data + i * 8 + 4);

            g_assert_cmphex(got_left & 255, ==, 255);
            g_assert_cmphex(got_right & 255, ==, 255);
            g_assert_cmphex((got_left >> 8) & 0xffff, ==, left);
            g_assert_cmphex((got_right >> 8) & 0xffff, ==, right);
        }
        qtest_writeq(t.qts, AUDIO + 0x40, 0);
        unsigned frozen = qtest_readq(t.qts, AUDIO + 0x48);
        unsigned msc = observe(&t, 0x408);
        step(&t, 500000);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x48), ==, frozen);
        g_assert_cmpuint((uint32_t)observe(&t, 0x408), >, msc);
        codec_set(&t, 27, 0);
        qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
        qtest_writeq(t.qts, AUDIO + 0x40, 0x400);
    }
    fixture_stop(&t);
}

static unsigned g711_table_character(unsigned format, unsigned interval)
{
    return format == 2 ? 255 - interval : (128 + interval) ^ 0x55;
}

static void g711_capture_check(AudioFixture *t, int32_t left, int32_t right,
                                unsigned left_code, unsigned right_code)
{
    uint8_t data[32];
    unsigned codes[2] = { left_code, right_code };

    qtest_writeq(t->qts, AUDIO + 0x20, 0x400);
    qtest_writeq(t->qts, FIXTURE, left);
    qtest_writeq(t->qts, FIXTURE + 8, right);
    step(t, 100000);
    qtest_writeq(t->qts, AUDIO + 0x20, 0x200);
    step(t, 125000);
    g_assert_cmpuint(qtest_readq(t->qts, AUDIO + 0x30), >=, 32);
    g_assert_cmpint(pread(t->fd, data, sizeof(data), 0), ==, sizeof(data));
    for (unsigned pair = 0; pair < 4; pair++) {
        for (unsigned side = 0; side < 2; side++) {
            uint32_t expected = (codes[side] & 128 ? 0xff000000U : 0) |
                                (codes[side] << 16) | 255;
            uint32_t actual = ldl_be_p(data + pair * 8 + side * 4);

            if (actual != expected) {
                g_test_message("input %d/%d, character %02x/%02x, pair %u",
                               left, right, left_code, right_code, pair);
            }
            g_assert_cmphex(actual, ==, expected);
        }
    }
}

static void g711_encoding(void)
{
    /* G.711 tables 1a/1b/2a/2b: normalized upper decisions and widths. */
    static const unsigned a_ranges[][2] = {
        { 64, 2 }, { 128, 4 }, { 256, 8 }, { 512, 16 },
        { 1024, 32 }, { 2048, 64 }, { 4096, 128 },
    };
    static const unsigned mu_ranges[][2] = {
        { 1, 1 }, { 31, 2 }, { 95, 4 }, { 223, 8 }, { 479, 16 },
        { 991, 32 }, { 2015, 64 }, { 4063, 128 }, { 8159, 256 },
    };
    AudioFixture t = fixture_start("");

    codec_power(&t);
    for (unsigned format = 2; format < 4; format++) {
        const unsigned (*ranges)[2] = format == 2 ? mu_ranges : a_ranges;
        unsigned groups = format == 2 ? ARRAY_SIZE(mu_ranges) :
                                       ARRAY_SIZE(a_ranges);
        unsigned scale = format == 2 ? 4 : 8;
        unsigned lower = 0, intervals = 0;
        int32_t decisions[129], levels[128];
        unsigned positive = format == 2 ? 0xcf : 0x95;
        int32_t interior = format == 2 ? 896 : 2056;
        uint8_t playback[0xfe0];

        g_test_message("G.711 format %u", format);
        codec_set(&t, 27, 0);
        codec_set(&t, 25, 0);
        codec_set(&t, 26, 0x50 | format | (format << 2));
        codec_set(&t, 27, 0x83);
        step(&t, 500000);
        g711_capture_check(&t, interior, -interior, positive, positive ^ 128);
        interior = format == 2 ? 893 : 2050;
        g711_capture_check(&t, interior, -interior, positive, positive ^ 128);
        for (unsigned group = 0; group < groups; group++) {
            unsigned width = ranges[group][1];

            for (; lower < ranges[group][0]; lower += width) {
                g_assert_cmpuint(intervals, <, 128);
                decisions[intervals] = lower * scale;
                levels[intervals] = (2 * lower + width) * scale / 2;
                intervals++;
            }
        }
        g_assert_cmpuint(intervals, ==, 128);
        decisions[128] = lower * scale;
        if (format == 2) {
            levels[0] = 0;
        }
        for (unsigned interval = 0; interval < intervals; interval++) {
            int32_t first = decisions[interval] + 1;
            int32_t last = decisions[interval + 1] - 1;
            unsigned code = g711_table_character(format, interval);

            g711_capture_check(&t, first, -first, code, code ^ 128);
            g711_capture_check(&t, last, -last, code, code ^ 128);
            if (interval) {
                int32_t boundary = decisions[interval];
                int32_t below = boundary - levels[interval - 1];
                int32_t above = levels[interval] - boundary;
                unsigned previous = g711_table_character(format, interval - 1);
                unsigned selected = below < above ? previous :
                    above < below ? code : MIN(previous, code);

                g711_capture_check(&t, boundary, -boundary, selected,
                                     selected ^ 128);
            }
        }
        positive = g711_table_character(format, 0);
        g711_capture_check(&t, 0, 0, positive, positive);
        positive = g711_table_character(format, 127);
        g711_capture_check(&t, decisions[128], -decisions[128],
                             positive, positive ^ 128);
        g711_capture_check(&t, INT16_MAX, INT16_MIN, positive, positive ^ 128);
        g711_capture_check(&t, 40000, -40000, positive, positive ^ 128);

        /* Check strict interiors through real signed16 DAC serial DMA too. */
        codec_set(&t, 27, 0);
        qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
        qtest_writeq(t.qts, AUDIO + 0x40, 0x400);
        interior = format == 2 ? 896 : 2056;
        positive = format == 2 ? 0xcf : 0x95;
        for (unsigned offset = 0; offset < sizeof(playback); offset += 8) {
            stl_be_p(playback + offset, ((uint32_t)interior << 8) | 0x12);
            stl_be_p(playback + offset + 4, ((uint32_t)-interior << 8) | 0x34);
        }
        g_assert_cmpint(pwrite(t.fd, playback, sizeof(playback), 0x1000), ==,
                        sizeof(playback));
        codec_set(&t, 25, 0xa0);
        codec_set(&t, 26, 0x150 | format | (format << 2));
        qtest_writeq(t.qts, AUDIO + 0x40, 0);
        qtest_writeq(t.qts, AUDIO + 0x50, 0xfe0);
        qtest_writeq(t.qts, AUDIO + 0x40, 0x200);
        codec_set(&t, 27, 0x103);
        step(&t, 2000000);
        g711_capture_check(&t, interior, -interior, positive, positive ^ 128);
        codec_set(&t, 27, 0);
        qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
        qtest_writeq(t.qts, AUDIO + 0x40, 0x400);
    }
    fixture_stop(&t);
}

static void capture_nodes(void)
{
    AudioFixture t = fixture_start("");
    uint8_t data[32];

    codec_power(&t);
    codec_set(&t, 26, 0x55);
    codec_set(&t, 27, 0x93);
    for (unsigned source = 0; source < 6; source++) {
        unsigned node = source == 0 ? 0 : source == 1 ? 4 :
                        source == 5 ? 5 : source - 1;

        qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
        qtest_writeq(t.qts, FIXTURE + node * 16, 1000 + source * 100);
        qtest_writeq(t.qts, FIXTURE + node * 16 + 8, -2000 - source * 100);
        codec_set(&t, 2, (source << 13) | (source << 5));
        step(&t, 200000);
        qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
        step(&t, 100000);
        g_assert_cmpint(pread(t.fd, data, sizeof(data), 0), ==, sizeof(data));
        g_assert_cmphex((ldl_be_p(data) >> 8) & 0xffff, ==,
                        1000 + source * 100);
        g_assert_cmphex((ldl_be_p(data + 4) >> 8) & 0xffff, ==,
                        (uint16_t)(-2000 - source * 100));
    }
    codec_set(&t, 2, 0);
    qtest_writeq(t.qts, FIXTURE, 10000);
    qtest_writeq(t.qts, FIXTURE + 8, 11000);
    qtest_writeq(t.qts, FIXTURE + 0x90, 2000);
    qtest_writeq(t.qts, FIXTURE + 0x98, 3000);
    codec_set(&t, 28, 0x4403);
    step(&t, 100000);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
    step(&t, 100000);
    g_assert_cmpint(pread(t.fd, data, sizeof(data), 0), ==, sizeof(data));
    g_assert_cmphex((ldl_be_p(data) >> 8) & 0xffff, ==, 8000);
    g_assert_cmphex((ldl_be_p(data + 4) >> 8) & 0xffff, ==, 8000);
    fixture_stop(&t);
}

static int32_t node(AudioFixture *t, unsigned offset)
{
    return (int32_t)observe(t, offset);
}

static void analog_routing(void)
{
    AudioFixture t = fixture_start("");

    codec_power(&t);
    codec_set(&t, 27, 0x90);
    codec_set(&t, 8, 0x8808);
    for (unsigned input = 1; input <= 4; input++) {
        qtest_writeq(t.qts, FIXTURE + input * 16, 2000);
        qtest_writeq(t.qts, FIXTURE + input * 16 + 8, -1000);
        codec_set(&t, 3 + input, 0x0808);
        g_assert_cmpint(node(&t, 0x100), ==, 2000);
        g_assert_cmpint(node(&t, 0x108), ==, -1000);
        codec_set(&t, 8, 0x8888);
        g_assert_cmpint(node(&t, 0x100), ==, 0);
        codec_set(&t, 8, 0x8808);
        codec_set(&t, 3 + input, 0x8888);
        qtest_writeq(t.qts, FIXTURE + input * 16, 0);
        qtest_writeq(t.qts, FIXTURE + input * 16 + 8, 0);
    }
    qtest_writeq(t.qts, FIXTURE + 0x50, 3000);
    qtest_writeq(t.qts, FIXTURE + 0x58, 1000);
    codec_set(&t, 8, 0x0888);
    g_assert_cmpint(node(&t, 0x100), ==, 3000);
    g_assert_cmpint(node(&t, 0x130), ==, 2000);
    codec_set(&t, 8, 0x08c8);
    g_assert_cmpint(node(&t, 0x100), ==, 3000);
    g_assert_cmpint(node(&t, 0x130), ==, 0);
    codec_set(&t, 8, 0x08a8);
    codec_set(&t, 27, 0xd0);
    g_assert_cmpint(node(&t, 0x120), ==, 0);
    codec_set(&t, 8, 0x0888);
    g_assert_cmpint(node(&t, 0x120), ==, 1500);
    codec_set(&t, 8, 0x0898);
    g_assert_cmpint(node(&t, 0x120), ==, 3000);
    codec_set(&t, 27, 0x90);
    g_assert_cmpint(node(&t, 0x120), ==, 0);
    g_assert_cmpint(node(&t, 0x130), ==, 2000);
    codec_set(&t, 8, 0x8800);
    qtest_writeq(t.qts, FIXTURE + 0x60, 5000);
    qtest_writeq(t.qts, FIXTURE + 0x68, -2000);
    step(&t, 80000);
    g_assert_cmpint(node(&t, 0x100), ==, 5000);
    g_assert_cmpint(node(&t, 0x108), ==, -2000);
    codec_set(&t, 8, 0x8880);
    g_assert_cmpint(node(&t, 0x100), ==, 5000);
    codec_set(&t, 8, 0x8888);
    g_assert_cmpint(node(&t, 0x100), ==, 0);
    codec_set(&t, 27, 0);
    qtest_writeq(t.qts, FIXTURE + 0x138, 1);
    step(&t, 80000);
    g_assert_cmpint(node(&t, 0x130), ==, 2000);
    qtest_writeq(t.qts, FIXTURE + 0x138, 0);
    step(&t, 80000);
    g_assert_cmpint(node(&t, 0x130), ==, 0);
    fixture_stop(&t);
}

static void dac_graph_and_gains(void)
{
    AudioFixture t = fixture_start("");

    codec_power(&t);
    for (unsigned i = 0; i < 127 * 4; i++) {
        memory_pair(&t, 0x1000 + i * 8, 0x001000ff, 0xfff000ff);
        memory_pair(&t, 0x2000 + i * 8, 0x001800ff, 0xfff400ff);
    }
    codec_set(&t, 26, 0x555);
    codec_set(&t, 8, 0x8803);
    codec_set(&t, 27, 0x1393);
    codec_set(&t, 3, 0x0808);
    qtest_writeq(t.qts, AUDIO + 0x40, 0);
    qtest_writeq(t.qts, AUDIO + 0x50, 0xfe0);
    qtest_writeq(t.qts, AUDIO + 0x40, 0x200);
    qtest_writeq(t.qts, AUDIO + 0x60, 0);
    qtest_writeq(t.qts, AUDIO + 0x70, 0xfe0);
    qtest_writeq(t.qts, AUDIO + 0x60, 0x200);
    step(&t, 1000000);
    g_assert_cmpint(node(&t, 0x100), ==, 6144);
    g_assert_cmpint(node(&t, 0x108), ==, -3072);
    g_assert_cmpint(node(&t, 0x110), ==, 0);
    codec_set(&t, 10, 0x0808);
    g_assert_cmpint(node(&t, 0x100), ==, 6144);
    g_assert_cmpint(node(&t, 0x110), ==, 6144);
    codec_set(&t, 10, 0x8888);
    g_assert_cmpint(node(&t, 0x100), ==, 6144);
    g_assert_cmpint(node(&t, 0x110), ==, 0);
    codec_set(&t, 9, 0x0808);
    g_assert_cmpint(node(&t, 0x100), ==, 10240);
    g_assert_cmpint(node(&t, 0x108), ==, -7168);
    codec_set(&t, 11, 0x8080);
    step(&t, 100000);
    g_assert_cmpint(node(&t, 0x100), ==, 6144);
    codec_set(&t, 11, 0);
    codec_set(&t, 8, 0x8800);
    codec_set(&t, 9, 0x0c0c);
    g_assert_cmphex(observe(&t, 0x310) & 0xffff, ==, 0x0808);
    g_assert_cmpuint(observe(&t, 0x348) & 0xffff, >, 0);
    step(&t, 384 * 1000000000ULL / 48000);
    g_assert_cmphex(observe(&t, 0x310) & 0xffff, ==, 0x0c0c);
    codec_set(&t, 8, 0x8801);
    codec_set(&t, 9, 0x0808);
    g_assert_cmphex(observe(&t, 0x310) & 0xffff, ==, 0x0808);
    codec_set(&t, 27, 0x1093);
    codec_set(&t, 25, 0xa0);
    codec_set(&t, 27, 0x1193);
    g_assert_cmphex(raw(&t, 27), ==, 0x1193);
    codec_set(&t, 27, 0);
    codec_set(&t, 25, 0x50);
    g_assert_cmphex(raw(&t, 25), ==, 0x50);
    codec_set(&t, 27, 3);
    g_assert_cmphex(raw(&t, 27), ==, 0);
    fixture_stop(&t);
}

static void digital_mix_constraints(void)
{
    AudioFixture t = fixture_start("");

    codec_power(&t);
    codec_set(&t, 13, 0);
    codec_set(&t, 27, 0x103);
    g_assert_cmphex(raw(&t, 27), ==, 0xc0);
    codec_set(&t, 26, 0x40);
    codec_set(&t, 27, 0x103);
    g_assert_cmphex(raw(&t, 27), ==, 0x103);
    codec_set(&t, 27, 0);
    codec_set(&t, 26, 0);
    codec_set(&t, 28, 0x4c00);
    codec_set(&t, 17, 24000);
    codec_set(&t, 15, 0x105);
    codec_set(&t, 27, 0x103);
    g_assert_cmphex(raw(&t, 27), ==, 0x103);
    codec_set(&t, 27, 0);
    codec_set(&t, 28, 0x5c00);
    codec_set(&t, 20, 24000);
    codec_set(&t, 15, 0x205);
    codec_set(&t, 27, 0x103);
    g_assert_cmphex(raw(&t, 27), ==, 0);
    fixture_stop(&t);
}

static void dma_faults_and_wrap(void)
{
    AudioFixture t = fixture_start("");
    uint8_t data[32];
    uint32_t pointer;

    codec_power(&t);
    codec_set(&t, 26, 0x55);
    codec_set(&t, 27, 3);
    qtest_writeq(t.qts, FIXTURE, 1234);
    qtest_writeq(t.qts, FIXTURE + 8, -2345);
    qtest_writeq(t.qts, AUDIO + 0x20, 0);
    qtest_writeq(t.qts, AUDIO + 0x28, 0x20);
    qtest_writeq(t.qts, AUDIO + 0x30, 0);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
    step(&t, 1000000);
    g_assert_cmphex(observe(&t, 0x190) & 8, ==, 8);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), ==, 0);
    qtest_writeq(t.qts, AUDIO + 0x28, 0x40);
    step(&t, 100000);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), ==, 0);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
    g_assert_cmphex(observe(&t, 0x190) & 8, ==, 0);
    qtest_writeq(t.qts, AUDIO + 0x20, 0);
    qtest_writeq(t.qts, AUDIO + 0x28, 0xfc0);
    qtest_writeq(t.qts, AUDIO + 0x30, 0xfe0);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
    step(&t, 100000);
    g_assert_cmpint(pread(t.fd, data, sizeof(data), 0xfe0), ==, sizeof(data));
    g_assert_cmphex((ldl_be_p(data) >> 8) & 0xffff, ==, 1234);
    g_assert_cmphex((ldl_be_p(data + 4) >> 8) & 0xffff, ==,
                    (uint16_t)-2345);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), ==, 0);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
    qtest_writeq(t.qts, FIXTURE + 0x188, 0x80000000);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
    step(&t, 200000);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), !=, 0);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
    qtest_writeq(t.qts, FIXTURE + 0x188, 0);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
    step(&t, 200000);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), ==, 0);
    g_assert_cmphex(observe(&t, 0x168) & 255, ==, 2);
    g_assert_cmphex(observe(&t, 0x190) & 8, ==, 0);
    for (unsigned channel = 1; channel < 3; channel++) {
        unsigned control = 0x20 + channel * 0x20;

        qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
        qtest_writeq(t.qts, FIXTURE + 0x188, 0x44000000);
        codec_set(&t, 27, channel == 1 ? 0x103 : 0x203);
        qtest_writeq(t.qts, AUDIO + control, 0);
        qtest_writeq(t.qts, AUDIO + control + 16, 32);
        qtest_writeq(t.qts, AUDIO + control, 0x200);
        step(&t, 1000000);
        pointer = qtest_readq(t.qts, AUDIO + control + 8);
        g_assert_cmphex(pointer, ==, 0);
        g_assert_cmphex(observe(&t, 0x190) & (1 << (3 + channel * 2)), ==,
                        1 << (3 + channel * 2));
        qtest_writeq(t.qts, AUDIO + control, 0x400);
    }
    fixture_stop(&t);
}

static void timestamps(void)
{
    AudioFixture t = fixture_start("");
    uint64_t old[3], now[3];

    codec_power(&t);
    codec_set(&t, 26, 0x555);
    codec_set(&t, 27, 0x303);
    step(&t, 1000000);
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeq(t.qts, AUDIO + 0x20 + i * 0x20, 0);
        qtest_writeq(t.qts, FIXTURE + 0x400 + i * 8, 0x12345678fffffffeULL);
        old[i] = observe(&t, 0x400 + i * 8);
        g_assert_cmphex(old[i], ==, 0x12345678fffffffeULL);
    }
    step(&t, 300000);
    for (unsigned i = 0; i < 3; i++) {
        now[i] = observe(&t, 0x400 + i * 8);
        g_assert_cmpuint((uint32_t)now[i], <, 20);
        g_assert_cmpuint(now[i] >> 32, >, 0);
        g_assert_cmpuint(now[i] >> 32, <=, observe(&t, 0x180) / 960);
        g_assert_cmphex(qtest_readl(t.qts, FIXTURE + 0x400 + i * 8), ==,
                        now[i] >> 32);
        g_assert_cmphex(qtest_readl(t.qts, FIXTURE + 0x404 + i * 8), ==,
                        (uint32_t)now[i]);
        for (unsigned poll = 0; poll < 100; poll++) {
            g_assert_cmphex(observe(&t, 0x400 + i * 8), ==, now[i]);
        }
        qtest_writel(t.qts, FIXTURE + 0x400 + i * 8, 0xabcdef12);
        g_assert_cmphex(observe(&t, 0x400 + i * 8), ==,
                        0xabcdef1200000000ULL | (uint32_t)now[i]);
        qtest_writel(t.qts, FIXTURE + 0x404 + i * 8, 0x456789ab);
        g_assert_cmphex(observe(&t, 0x400 + i * 8), ==,
                        0xabcdef12456789abULL);
        qtest_writeq(t.qts, AUDIO + 0x20 + i * 0x20, 0x400);
    }
    step(&t, 500000);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(observe(&t, 0x400 + i * 8), ==, 0);
    }
    fixture_stop(&t);
}

static void phase_latches(void)
{
    AudioFixture t = fixture_start("");
    uint16_t old;

    codec_power(&t);
    codec_set(&t, 28, 0x7c00);
    for (unsigned generator = 0; generator < 3; generator++) {
        unsigned reg = 18 + 3 * generator;

        before_stage(&t, 1, 0);
        qtest_writeq(t.qts, AUDIO + 8, (reg << 17) | 255);
        next_serial(&t);
        before_stage(&t, 5, 0);
        next_serial(&t);
        g_assert_cmpuint(observe(&t, 0x318 + generator * 8), ==, 255);
        before_stage(&t, 1, 1);
        qtest_writeq(t.qts, AUDIO + 8, (reg << 17) | 0x10000);
        next_serial(&t);
        g_assert_cmpuint(observe(&t, 0x330 + generator * 8), ==, 0);
        before_stage(&t, 2, 0);
        next_serial(&t);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x18), ==, 0);
        before_stage(&t, 1, 1);
        qtest_writeq(t.qts, AUDIO + 8, (reg << 17) | 0x1ff);
        next_serial(&t);
        before_stage(&t, 5, 1);
        old = observe(&t, 0x318 + generator * 8);
        next_serial(&t);
        g_assert_cmpuint(observe(&t, 0x318 + generator * 8), ==, 0x1ff);
        qtest_writeq(t.qts, AUDIO + 8, 0x10000);
        before_stage(&t, 2, 0);
        next_serial(&t);
        /* Commit samples live work, rather than the previous slot-0 latch. */
        g_assert_cmpuint(qtest_readq(t.qts, AUDIO + 0x18), <=, old);
        step(&t, 100000);
        g_assert_cmpuint(observe(&t, 0x318 + generator * 8), ==, 0x100);
    }
    fixture_stop(&t);
}

static void dma_raw_and_mono(void)
{
    AudioFixture t = fixture_start("");

    codec_power(&t);
    for (unsigned dac = 0; dac < 2; dac++) {
        uint8_t data[32];
        unsigned control = 0x40 + dac * 0x20;

        for (unsigned i = 0; i < 127 * 4; i++) {
            memory_pair(&t, (dac + 1) * 0x1000 + i * 8,
                          0x01800001, 0xfe000002);
        }
        codec_set(&t, 25, dac ? 0xf0 : 0xa0);
        codec_set(&t, 26, 0x55 | (1 << (8 + dac * 2)));
        qtest_writeq(t.qts, AUDIO + control, 0);
        qtest_writeq(t.qts, AUDIO + control + 16, 0xfe0);
        qtest_writeq(t.qts, AUDIO + control, 0x200);
        qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
        codec_set(&t, 27, (0x100 << dac) | 3);
        step(&t, 1000000);
        g_assert_cmpint(pread(t.fd, data, sizeof(data), 0x100), ==,
                        sizeof(data));
        for (unsigned i = 0; i < 8; i++) {
            g_assert_cmphex((uint32_t)ldl_be_p(data + i * 4), ==, 0xffffffff);
        }
        codec_set(&t, 27, 0);
        qtest_writeq(t.qts, AUDIO + control, 0x400);
        qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
        for (unsigned i = 0; i < 127 * 4; i++) {
            memory_pair(&t, (dac + 1) * 0x1000 + i * 8,
                          0x001234ab, 0xffcdef12);
        }
        codec_set(&t, 26, 0x55 | (1 << (8 + dac * 2)) |
                           (1 << (14 + dac)));
        qtest_writeq(t.qts, AUDIO + control, 0);
        qtest_writeq(t.qts, AUDIO + control + 16, 0xfe0);
        qtest_writeq(t.qts, AUDIO + control, 0x200);
        qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
        codec_set(&t, 27, (0x100 << dac) | 3);
        step(&t, 1000000);
        g_assert_cmpint(pread(t.fd, data, sizeof(data), 0x100), ==,
                        sizeof(data));
        for (unsigned i = 0; i < 8; i++) {
            g_assert_cmphex((uint32_t)ldl_be_p(data + i * 4), ==, 0x001234ff);
        }
        codec_set(&t, 27, 0);
        qtest_writeq(t.qts, AUDIO + control, 0x400);
        qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
    }
    fixture_stop(&t);
}

static void fifo_requests(void)
{
    AudioFixture t = fixture_start("");

    codec_power(&t);
    codec_set(&t, 26, 0x100);
    qtest_writeq(t.qts, AUDIO + 0x40, 0);
    qtest_writeq(t.qts, AUDIO + 0x50, 0xfe0);
    qtest_writeq(t.qts, AUDIO + 0x40, 0x200);
    before_stage(&t, 1, 0);
    qtest_writeq(t.qts, AUDIO + 8, (27 << 17) | 0x100);
    next_serial(&t);
    before_stage(&t, 5, 0);
    next_serial(&t);
    qtest_writeq(t.qts, AUDIO + 8, 0x10000);
    before_stage(&t, 2, 1);
    g_assert_cmpuint((uint32_t)observe(&t, 0x408), ==, 0);
    next_serial(&t);
    g_assert_cmpuint((uint32_t)observe(&t, 0x408), ==, 1);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x48), ==, 0);
    g_assert_cmpuint((observe(&t, 0x300) >> 16) & 255, ==, 0);
    step(&t, 150000);
    g_assert_cmpuint((observe(&t, 0x300) >> 16) & 255, ==, 4);
    g_assert_cmpuint(observe(&t, 0x308) & 1, ==, 0);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x48), ==, 32);
    g_assert_cmpuint((uint32_t)observe(&t, 0x408), ==, 4);
    step(&t, 250000);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x48), >, 32);
    fixture_stop(&t);
}

static void partial_capture(void)
{
    AudioFixture t = fixture_start("");

    codec_power(&t);
    codec_set(&t, 26, 0x45);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
    codec_set(&t, 27, 1);
    step(&t, 500000);
    g_assert_cmphex(observe(&t, 0x168) & 255, ==, 3);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), ==, 0);
    g_assert_cmphex(observe(&t, 0x190) & 8, ==, 0);
    fixture_stop(&t);
}

static void dma_full_ring(void)
{
    AudioFixture t = fixture_start("");
    uint8_t data[32];

    codec_power(&t);
    codec_set(&t, 26, 0x155);
    codec_set(&t, 27, 0x103);
    qtest_writeq(t.qts, FIXTURE, 1111);
    qtest_writeq(t.qts, FIXTURE + 8, -2222);
    step(&t, 500000);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x2c0);
    for (unsigned event = 0; event < 10000; event++) {
        if (qtest_readq(t.qts, AUDIO + 0x30) == 0xfe0) {
            break;
        }
        next_serial(&t);
    }
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), ==, 0xfe0);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x38), ==, 0xfe0);
    g_assert_cmphex(observe(&t, 0x190) & 12, ==, 4);
    g_assert_cmpint(pread(t.fd, data, sizeof(data), 0xfc0), ==, sizeof(data));
    g_assert_cmphex((uint32_t)ldl_be_p(data), ==, (1111 << 8) | 255);
    g_assert_cmphex((uint32_t)ldl_be_p(data + 4), ==,
                    ((uint32_t)-2222 << 8) | 255);
    qtest_writeq(t.qts, AUDIO + 0x28, 0xfe0);
    g_assert_cmphex(observe(&t, 0x190) & 4, ==, 0);
    step(&t, 100000);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), ==, 0);
    g_assert_cmphex(observe(&t, 0x168) & 255, ==, 0);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
    for (unsigned pair = 0; pair < 127 * 4; pair++) {
        memory_pair(&t, 0x1000 + pair * 8,
                      (pair << 8) | 0x12, ((uint32_t)-pair << 8) | 0x34);
    }
    qtest_writeq(t.qts, AUDIO + 0x40, 0);
    qtest_writeq(t.qts, AUDIO + 0x50, 0xfe0);
    qtest_writeq(t.qts, AUDIO + 0x40, 0x280);
    step(&t, 12000000);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x48), ==, 0xfe0);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x58), ==, 0);
    g_assert_cmphex(observe(&t, 0x190) & 0x30, ==, 0x10);
    fixture_stop(&t);
}

static void capture_staged_resume(void)
{
    AudioFixture t = fixture_start("");
    uint8_t guard[96], data[sizeof(guard)];

    codec_power(&t);
    codec_set(&t, 26, 0x55);
    codec_set(&t, 17, 8000);
    codec_set(&t, 28, 0x4c00);
    codec_set(&t, 15, 5);
    qtest_writeq(t.qts, FIXTURE + 0x198, 4);
    for (unsigned paused = 0; paused < 2; paused++) {
        uint64_t stamp;

        memset(guard, 0x59, sizeof(guard));
        g_assert_cmpint(pwrite(t.fd, guard, sizeof(guard), 0xfc0), ==,
                        sizeof(guard));
        qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
        qtest_writeq(t.qts, AUDIO + 0x20, 0);
        qtest_writeq(t.qts, AUDIO + 0x30, 0xfe0);
        qtest_writeq(t.qts, AUDIO + 0x20, 0x220);
        qtest_writeq(t.qts, FIXTURE, 1111);
        qtest_writeq(t.qts, FIXTURE + 8, -2222);
        codec_set(&t, 27, 0x83);
        for (unsigned pair = 0; pair < 4; pair++) {
            qtest_writeq(t.qts, FIXTURE, 1111 + 17 * pair);
            qtest_writeq(t.qts, FIXTURE + 8, -2222 - 29 * (int)pair);
            for (unsigned event = 0; event < 2000; event++) {
                if ((uint32_t)observe(&t, 0x400) == pair + 1) {
                    break;
                }
                next_serial(&t);
            }
            g_assert_cmpuint((uint32_t)observe(&t, 0x400), ==, pair + 1);
        }
        /* Disable before the next 8 kHz pair can overflow the held stage. */
        codec_set(&t, 27, 0x80);
        step(&t, 250000);
        stamp = observe(&t, 0x400);
        g_assert_cmpuint((uint32_t)stamp, ==, 4);
        g_assert_cmphex(observe(&t, 0x168) & 255, ==, 0);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), ==, 0xfe0);
        if (paused) {
            qtest_writeq(t.qts, AUDIO + 0x20, 0x20);
        }
        /* Leave 31 blocks: publication must restore the 32-block threshold. */
        qtest_writeq(t.qts, AUDIO + 0x28, 0xc00);
        step(&t, 50000);
        if (paused) {
            g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), ==, 0xfe0);
            g_assert_false(qtest_get_irq(t.qts, 0));
            g_assert_cmpint(pread(t.fd, data, sizeof(data), 0xfc0), ==,
                            sizeof(data));
            g_assert_cmpmem(guard, sizeof(guard), data, sizeof(data));
            qtest_writeq(t.qts, AUDIO + 0x20, 0x220);
            step(&t, 50000);
        }
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), ==, 0);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x38), ==, 0x400);
        g_assert_cmphex(observe(&t, 0x400), ==, stamp);
        g_assert_cmphex(observe(&t, 0x168) & 255, ==, 0);
        g_assert_true(qtest_get_irq(t.qts, 0));
        g_assert_cmpint(pread(t.fd, data, sizeof(data), 0xfc0), ==,
                        sizeof(data));
        g_assert_cmpmem(guard, 32, data, 32);
        g_assert_cmpmem(guard + 64, 32, data + 64, 32);
        for (unsigned pair = 0; pair < 4; pair++) {
            int32_t right = -2222 - 29 * (int)pair;

            g_assert_cmphex((uint32_t)ldl_be_p(data + 32 + pair * 8), ==,
                            ((1111 + 17 * pair) << 8) | 255);
            g_assert_cmphex((uint32_t)ldl_be_p(data + 36 + pair * 8), ==,
                            ((uint32_t)right << 8) | 255);
        }
        qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
    }
    fixture_stop(&t);
}

static void digital_attenuation(void)
{
    AudioFixture t = fixture_start("");
    uint8_t data[32];

    codec_power(&t);
    for (unsigned pair = 0; pair < 127 * 4; pair++) {
        memory_pair(&t, 0x1000 + pair * 8, 0x007fff12, 0x007fff34);
    }
    codec_set(&t, 25, 0xa0);
    codec_set(&t, 26, 0x155);
    codec_set(&t, 11, 0x3f3f);
    qtest_writeq(t.qts, AUDIO + 0x40, 0);
    qtest_writeq(t.qts, AUDIO + 0x50, 0xfe0);
    qtest_writeq(t.qts, AUDIO + 0x40, 0x200);
    codec_set(&t, 27, 0x103);
    step(&t, 500000);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
    step(&t, 100000);
    g_assert_cmpint(pread(t.fd, data, sizeof(data), 0), ==, sizeof(data));
    g_assert_cmphex((uint32_t)ldl_be_p(data), ==, 0x000001ff);
    g_assert_cmphex((uint32_t)ldl_be_p(data + 4), ==, 0x000001ff);
    codec_set(&t, 11, 0xbfbf);
    step(&t, 100000);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x400);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
    step(&t, 100000);
    g_assert_cmpint(pread(t.fd, data, sizeof(data), 0), ==, sizeof(data));
    g_assert_cmphex((uint32_t)ldl_be_p(data), ==, 255);
    g_assert_cmphex((uint32_t)ldl_be_p(data + 4), ==, 255);
    fixture_stop(&t);
}

static uint64_t codec_once(AudioFixture *t, unsigned reg, uint16_t value)
{
    uint64_t commit;

    before_stage(t, 1, 0);
    qtest_writeq(t->qts, AUDIO + 8, (reg << 17) | value);
    next_serial(t);
    qtest_writeq(t->qts, AUDIO + 8, 0x10000);
    before_stage(t, 5, 0);
    commit = observe(t, 0x178);
    next_serial(t);
    return commit;
}

static void step_to(AudioFixture *t, uint64_t when)
{
    uint64_t now = observe(t, 0x180);

    g_assert_cmpuint(when, >=, now);
    step(t, when - now);
}

static void actual_phase(void)
{
    const unsigned rates[] = { 8000, 48000 };

    AudioFixture t = fixture_start("");

    codec_power(&t);
    for (unsigned r = 0; r < ARRAY_SIZE(rates); r++) {

        codec_set(&t, 27, 0);
        codec_set(&t, 28, 0x4400);
        codec_set(&t, 17, rates[r]);
        codec_set(&t, 28, 0x4c00);
        codec_set(&t, 15, 5);
        codec_set(&t, 26, 0x55);
        codec_set(&t, 27, 3);
        step(&t, 500000);
        for (unsigned direction = 0; direction < 2; direction++) {
            uint64_t original, work, actual, count;
            double displacement = 1000000000.0 / (rates[r] * 3000.0);
            double expected, baseline;

            codec_once(&t, 18, (direction << 8) | 1);
            original = observe(&t, 0x500);
            work = observe(&t, 0x518);
            baseline = original;
            if (original <= work) {
                baseline += 1000000000.0 / rates[r];
            }
            step_to(&t, work);
            actual = observe(&t, 0x500);
            expected = baseline + (direction ? displacement : -displacement);
            g_assert_cmpfloat(fabs((double)actual - expected), <=, 1.0);
            count = observe(&t, 0x600);
            step_to(&t, actual);
            g_assert_cmpuint(observe(&t, 0x600), ==, count + 1);
            g_assert_cmpuint(observe(&t, 0x608), ==, actual);
        }
        /* The fixed source keeps its INIT anchor while generator 1 moves. */
        codec_set(&t, 27, 0);
        codec_set(&t, 15, 0);
        codec_set(&t, 27, 3);
        step(&t, 500000);
        codec_once(&t, 18, 255);
        for (unsigned i = 0; i < 4; i++) {
            uint64_t fixed_edge = observe(&t, 0x5c0);

            step_to(&t, fixed_edge);
            g_assert_cmpuint(observe(&t, 0x608), ==, fixed_edge);
        }
    }
    fixture_stop(&t);
}

static void bit_conversion_frequencies(void)
{
    static const unsigned bit[] = {
        2400, 4800, 7200, 9600, 12000, 14400,
        16800, 19200, 21600, 24000, 26400, 28800,
    };
    static const unsigned conversion[] = {
        50400, 58800, 63000, 67200, 78400, 84000,
        89600, 57600, 72000, 76800, 96000,
    };
    AudioFixture t = fixture_start("");

    codec_power(&t);
    codec_set(&t, 28, 0x7c00);
    for (unsigned g = 0; g < 3; g++) {
        codec_set(&t, 17 + 3 * g, 9600);
        for (unsigned high = 0; high < 16; high++) {
            for (unsigned low = 0; low < 16; low++) {
                bool defined = (high < ARRAY_SIZE(conversion) &&
                                low < ARRAY_SIZE(bit)) ||
                               (high == 15 && low == 15);
                uint64_t numerator, denominator;

                codec_set(&t, 16 + g * 3, (high << 4) | low);
                numerator = observe(&t, 0x700 + g * 0x20);
                denominator = observe(&t, 0x708 + g * 0x20);
                if (!defined) {
                    g_assert_cmpuint(denominator, ==, 0);
                    g_assert_cmpuint(numerator, ==, UINT64_MAX);
                } else if (high == 15) {
                    g_assert_cmpuint(numerator / denominator, ==,
                                     128 * 9600);
                } else {
                    g_assert_cmpuint(numerator * conversion[high], ==,
                                     (uint64_t)bit[low] * 9600 * 7 *
                                     denominator);
                }
                g_assert_cmpuint(observe(&t, 0x500 + g * 0x40 + 16), ==,
                                 9600 * 7);
            }
        }
        codec_set(&t, 16 + g * 3, 0);
        g_assert_cmpuint(observe(&t, 0x700 + g * 0x20) /
                         observe(&t, 0x708 + g * 0x20), ==, 3200);
        codec_set(&t, 16 + g * 3, 0x1000);
        g_assert_cmpuint(observe(&t, 0x700 + g * 0x20) /
                         observe(&t, 0x708 + g * 0x20), ==, 3400);
        codec_set(&t, 17 + g * 3, 48000);
        codec_set(&t, 16 + g * 3, 0x10ff);
        g_assert_cmpuint(observe(&t, 0x700 + g * 0x20) /
                         observe(&t, 0x708 + g * 0x20), ==, 6144200);
        codec_set(&t, 16 + g * 3, 0x14ff);
        g_assert_cmpuint(observe(&t, 0x710 + g * 0x20) /
                         observe(&t, 0x718 + g * 0x20), ==, 6144000);
        codec_set(&t, 16 + g * 3, 0x8000);
        g_assert_cmpuint(observe(&t, 0x708 + g * 0x20), ==, 0);
    }
    fixture_stop(&t);
}

static void phase_crossings(void)
{
    AudioFixture t = fixture_start("");
    bool advanced = false, retarded = false;

    codec_power(&t);
    codec_set(&t, 17, 48000);
    codec_set(&t, 28, 0x4c00);
    codec_set(&t, 15, 5);
    codec_set(&t, 26, 0x55);
    codec_set(&t, 27, 3);
    step(&t, 500000);
    qtest_writeq(t.qts, AUDIO + 8, (18 << 17) | 255);
    step(&t, 40000);
    for (unsigned tick = 0; tick < 12000 && !advanced; tick++) {
        uint64_t work = observe(&t, 0x518);
        uint64_t next = observe(&t, 0x500);
        uint64_t count = observe(&t, 0x600);

        g_assert_cmpuint(work, !=, INT64_MAX);
        step_to(&t, work);
        if (next > work && observe(&t, 0x608) == work) {
            double expected = work + 1000000000.0 / 48000 -
                              1000000000.0 / (48000 * 3000.0) + next - work;

            g_assert_cmpuint(observe(&t, 0x600), ==, count + 1);
            g_assert_cmpfloat(fabs((double)observe(&t, 0x500) - expected),
                              <=, 1.0);
            advanced = true;
        }
    }
    g_assert_true(advanced);
    qtest_writeq(t.qts, AUDIO + 8, (18 << 17) | 0x1ff);
    step(&t, 40000);
    for (unsigned tick = 0; tick < 12000 && !retarded; tick++) {
        uint64_t work = observe(&t, 0x518);
        uint64_t count = observe(&t, 0x600);

        step_to(&t, work);
        if ((int64_t)observe(&t, 0x508) < 0) {
            uint64_t after = observe(&t, 0x600);

            g_assert_cmpuint(after, >=, count);
            g_assert_cmpuint(after, <=, count + 1);
            g_assert_cmpuint(observe(&t, 0x500), >, work + 20833);
            retarded = true;
        }
    }
    g_assert_true(retarded);
    fixture_stop(&t);
}

static void dac_mix_branches(void)
{
    AudioFixture t = fixture_start("");

    codec_power(&t);
    for (unsigned pair = 0; pair < 127 * 4; pair++) {
        memory_pair(&t, 0x1000 + pair * 8, 0x00100012, 0xfff00034);
        memory_pair(&t, 0x2000 + pair * 8, 0x00180056, 0xfff40078);
    }
    codec_set(&t, 25, 0x4000);
    codec_set(&t, 26, 0x550);
    codec_set(&t, 8, 0x8803);
    codec_set(&t, 27, 0x390);
    codec_set(&t, 10, 0x0808);
    for (unsigned d = 0; d < 2; d++) {
        qtest_writeq(t.qts, AUDIO + 0x40 + d * 0x20, 0);
        qtest_writeq(t.qts, AUDIO + 0x50 + d * 0x20, 0xfe0);
        qtest_writeq(t.qts, AUDIO + 0x40 + d * 0x20, 0x200);
    }
    step(&t, 1000000);
    g_assert_cmpint(node(&t, 0x100), ==, 0);
    g_assert_cmpint(node(&t, 0x110), ==, 10240);
    g_assert_cmpint(node(&t, 0x118), ==, -7168);
    qtest_writeq(t.qts, FIXTURE + 0x10, 2000);
    qtest_writeq(t.qts, FIXTURE + 0x18, -1000);
    codec_set(&t, 4, 0x0808);
    g_assert_cmpint(node(&t, 0x100), ==, 2000);
    g_assert_cmpint(node(&t, 0x108), ==, -1000);
    g_assert_cmpint(node(&t, 0x110), ==, 10240);
    codec_set(&t, 27, 0x290);
    step(&t, 100000);
    g_assert_cmpint(node(&t, 0x110), ==, 6144);
    g_assert_cmpint(node(&t, 0x118), ==, -3072);
    fixture_stop(&t);
}

static void underrun_mix(void)
{
    AudioFixture t = fixture_start("");

    codec_power(&t);
    codec_set(&t, 28, 0x4c00);
    codec_set(&t, 17, 48000);
    codec_set(&t, 15, 0x105);
    codec_set(&t, 26, 0x155);
    codec_set(&t, 13, 0);
    codec_set(&t, 8, 0x8801);
    codec_set(&t, 27, 0x183);
    codec_set(&t, 9, 0x0808);
    qtest_writeq(t.qts, FIXTURE, 20000);
    qtest_writeq(t.qts, FIXTURE + 8, -20000);
    qtest_writeq(t.qts, AUDIO + 8, (18 << 17) | 255);
    step(&t, 10000000);
    g_assert_cmphex(raw(&t, 1) & 0x100, ==, 0x100);
    g_assert_cmpint(node(&t, 0x100), ==, 20000);
    g_assert_cmpint(node(&t, 0x108), ==, -20000);
    codec_set(&t, 13, 0x3f3f);
    step(&t, 100000);
    g_assert_cmpint(node(&t, 0x100), ==, 0);
    g_assert_cmpint(node(&t, 0x108), ==, 0);
    fixture_stop(&t);
}

static void clock_continuity_and_reentry(void)
{
    AudioFixture t = fixture_start("");
    uint64_t before, commit, now, expected, edge;
    int64_t progress;

    codec_power(&t);
    codec_set(&t, 17, 48000);
    codec_set(&t, 28, 0x4c00);
    codec_set(&t, 15, 5);
    codec_set(&t, 26, 0x55);
    codec_set(&t, 27, 3);
    step(&t, 500000);
    before_stage(&t, 1, 0);
    qtest_writeq(t.qts, AUDIO + 8, (17 << 17) | 8000);
    next_serial(&t);
    qtest_writeq(t.qts, AUDIO + 8, 0x10000);
    before_stage(&t, 5, 0);
    now = observe(&t, 0x180);
    commit = observe(&t, 0x178);
    progress = observe(&t, 0x508);
    progress = (progress + (commit - now) * 48000 * 7 * 3) % 21000000000LL;
    expected = commit + (21000000000LL - progress + 8000 * 7 * 3 - 1) /
                         (8000 * 7 * 3);
    next_serial(&t);
    g_assert_cmpuint(observe(&t, 0x500), ==, expected);
    before = observe(&t, 0x600);
    step_to(&t, expected);
    g_assert_cmpuint(observe(&t, 0x600), ==, before + 1);
    g_assert_cmpuint(observe(&t, 0x608), ==, expected);
    codec_set(&t, 27, 0);
    codec_set(&t, 16, 0x80ff);
    codec_once(&t, 18, 0x1ab);
    step(&t, 1000000);
    g_assert_cmphex(observe(&t, 0x318), ==, 0x1ab);
    g_assert_cmpuint(observe(&t, 0x500), ==, INT64_MAX);
    commit = codec_once(&t, 16, 0xff);
    g_assert_cmpuint(observe(&t, 0x500), ==, commit + 125000);
    edge = observe(&t, 0x518);
    g_assert_cmpuint(edge, >, commit);
    step_to(&t, edge);
    g_assert_cmphex(observe(&t, 0x318), ==, 0x1aa);
    g_assert_cmpuint(observe(&t, 0x500), >, commit + 125000);
    codec_once(&t, 18, 1);
    edge = observe(&t, 0x518);
    step_to(&t, edge);
    g_assert_cmphex(observe(&t, 0x318), ==, 0);
    codec_once(&t, 18, 0x1ff);
    codec_once(&t, 18, 0x107);
    g_assert_cmphex(observe(&t, 0x318), ==, 0x107);
    step(&t, 10000);
    g_assert_cmphex(observe(&t, 0x318), ==, 0x100);
    fixture_stop(&t);
}

static void migration_active(gconstpointer opaque)
{
    bool backend = GPOINTER_TO_UINT(opaque);
    const char *args = backend ? "-audiodev none,id=host "
        "-global sgi-mace-audio-test.audiodev=host" : "";
    g_autofree char *incoming = g_strdup_printf("%s -incoming defer", args);
    AudioFixture from = fixture_start(args);
    AudioFixture to = fixture_start(incoming);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate", from.dir);
    uint8_t first[4096], second[4096];

    codec_power(&from);
    for (unsigned i = 0; i < 127 * 4; i++) {
        memory_pair(&from, 0x1000 + i * 8,
                      0x00123400 | (i & 255), 0xffcdef00 | (i & 255));
    }
    codec_set(&from, 28, 0x7c00);
    /* The pending response below crosses the two 32-slot ownership groups. */
    codec_set(&from, 26, 0x115);
    codec_set(&from, 25, 0xa0);
    qtest_writeq(from.qts, AUDIO + 0x40, 0);
    qtest_writeq(from.qts, AUDIO + 0x50, 0xfe0);
    qtest_writeq(from.qts, AUDIO + 0x40, 0x2e0);
    qtest_writeq(from.qts, AUDIO + 0x20, 0x2a0);
    codec_set(&from, 27, 0x183);
    step(&from, 350000);
    codec_set(&from, 9, 0x0808);
    before_stage(&from, 1, 1);
    qtest_writeq(from.qts, AUDIO + 8, (18 << 17) | 255);
    next_serial(&from);
    before_stage(&from, 5, 1);
    next_serial(&from);
    qtest_writeq(from.qts, AUDIO + 8, (18 << 17) | 0x10000);
    before_stage(&from, 2, 0);
    next_serial(&from);
    /* The qtest accelerator clock is not part of the migration stream. */
    qtest_clock_set(to.qts, observe(&from, 0x180));
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    g_assert_cmpuint(observe(&from, 0x180), ==, observe(&to, 0x180));
    if (backend) {
        g_assert_cmpuint(observe(&from, 0x350), ==, 3);
        g_assert_cmpuint(observe(&to, 0x350), ==, 3);
    }
    for (unsigned offset = 0; offset < 0x80; offset += 8) {
        g_assert_cmphex(qtest_readq(from.qts, AUDIO + offset), ==,
                        qtest_readq(to.qts, AUDIO + offset));
    }
    for (unsigned offset = 0x100; offset <= 0x130; offset += 8) {
        g_assert_cmphex(observe(&from, offset), ==, observe(&to, offset));
    }
    for (unsigned offset = 0x150; offset <= 0x178; offset += 8) {
        g_assert_cmphex(observe(&from, offset), ==, observe(&to, offset));
    }
    for (unsigned offset = 0x300; offset <= 0x348; offset += 8) {
        g_assert_cmphex(observe(&from, offset), ==, observe(&to, offset));
    }
    step(&from, 500000);
    step(&to, 500000);
    for (unsigned offset = 0; offset < 0x80; offset += 8) {
        g_assert_cmphex(qtest_readq(from.qts, AUDIO + offset), ==,
                        qtest_readq(to.qts, AUDIO + offset));
    }
    for (unsigned offset = 0x100; offset <= 0x130; offset += 8) {
        g_assert_cmphex(observe(&from, offset), ==, observe(&to, offset));
    }
    g_assert_cmpint(pread(from.fd, first, sizeof(first), 0), ==, sizeof(first));
    g_assert_cmpint(pread(to.fd, second, sizeof(second), 0), ==,
                    sizeof(second));
    g_assert_cmpmem(first, sizeof(first), second, sizeof(second));
    qtest_qmp_assert_success(to.qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(to.qts, AUDIO), ==, 1);
    g_assert_cmphex(raw(&to, 0), ==, 0xc001);
    fixture_stop(&to);
    if (g_file_test(uri + 5, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(unlink(uri + 5), ==, 0);
    }
    fixture_stop(&from);
}

static void migration_calibration(void)
{
    AudioFixture from = fixture_start("");
    AudioFixture to = fixture_start("-incoming defer");
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate", from.dir);
    uint64_t commit;

    codec_init(&from);
    commit = codec_once(&from, 28, 0x0400);
    step(&from, 100000000);
    /* The qtest accelerator clock is not part of the migration stream. */
    qtest_clock_set(to.qts, observe(&from, 0x180));
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    g_assert_cmpuint(observe(&from, 0x180), ==, observe(&to, 0x180));
    g_assert_cmphex(raw(&from, 0), ==, 0x4001);
    g_assert_cmphex(raw(&to, 0), ==, 0x4001);
    step_to(&from, commit + 474000000 - 1);
    step_to(&to, commit + 474000000 - 1);
    g_assert_cmphex(raw(&from, 0), ==, 0x4001);
    g_assert_cmphex(raw(&to, 0), ==, 0x4001);
    step(&from, 1);
    step(&to, 1);
    g_assert_cmphex(raw(&from, 0), ==, 1);
    g_assert_cmphex(raw(&to, 0), ==, 1);
    fixture_stop(&to);
    if (g_file_test(uri + 5, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(unlink(uri + 5), ==, 0);
    }
    fixture_stop(&from);
}

static void capture_partial_ram(void)
{
    AudioFixture t = fixture_start(
        "-global sgi-mace-audio-test.mapped-bytes=17");
    uint8_t guard[32], actual[sizeof(guard)];

    memset(guard, 0x59, sizeof(guard));
    g_assert_cmpint(pwrite(t.fd, guard, sizeof(guard), 0), ==, sizeof(guard));
    codec_power(&t);
    codec_set(&t, 26, 0x15);
    codec_set(&t, 27, 0x83);
    qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
    step(&t, 500000);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), ==, 0);
    g_assert_cmpuint(observe(&t, 0x168) & 255, ==, 2);
    g_assert_cmphex(observe(&t, 0x190) & 8, ==, 0);
    g_assert_cmpint(pread(t.fd, actual, sizeof(actual), 0), ==, sizeof(actual));
    g_assert_cmpmem(guard, sizeof(guard), actual, sizeof(actual));
    fixture_stop(&t);
}

static void crime_dma_holes(void)
{
    AudioFixture t = fixture_start_config("", true);
    uint8_t guard[0x2000], actual[sizeof(guard)];

    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(t.qts, 0x14000208 + 8 * bank, 0x100);
    }
    step(&t, 100);
    qtest_writeq(t.qts, AUDIO, 0);
    step(&t, 700000);
    codec_set(&t, 28, 0x4400);
    step(&t, 474000000);
    codec_set(&t, 26, 0x115);
    codec_set(&t, 27, 0x183);
    memset(guard, 0x59, sizeof(guard));
    for (unsigned view = 0; view < 2; view++) {
        uint32_t base = view ? 0x80000000 : 0x40000000;

        for (unsigned i = 0; i < 3; i++) {
            qtest_writeq(t.qts, AUDIO + 0x20 + i * 0x20, 0x400);
        }
        qtest_writeq(t.qts, 0x1f310000, base);
        for (unsigned pair = 0; pair < 4; pair++) {
            memory_pair(&t, 0x1000 + pair * 8, 0x001234ff, 0xffedccff);
        }
        qtest_writeq(t.qts, AUDIO + 0x40, 0);
        qtest_writeq(t.qts, AUDIO + 0x50, 32);
        qtest_writeq(t.qts, AUDIO + 0x40, 0x200);
        step(&t, 300000);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x48), ==, 32);
        qtest_writeq(t.qts, AUDIO + 0x40, 0x400);
        qtest_writeq(t.qts, 0x1f310000, base + 0x08000000);
        qtest_writeq(t.qts, 0x14000250, 0);
        qtest_writeq(t.qts, AUDIO + 0x40, 0);
        qtest_writeq(t.qts, AUDIO + 0x50, 32);
        qtest_writeq(t.qts, AUDIO + 0x40, 0x200);
        step(&t, 300000);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x48), ==, 0);
        g_assert_cmphex(qtest_readq(t.qts, 0x1f310010) & 0x20, ==, 0x20);
        g_assert_cmphex(qtest_readq(t.qts, 0x14000250) & (1U << 25), ==,
                        1U << 25);
        g_assert_cmphex(qtest_readq(t.qts, 0x14000258), ==, 0x08001000);
        qtest_writeq(t.qts, AUDIO + 0x40, 0x400);
        qtest_writeq(t.qts, 0x14000250, 0);
        g_assert_cmpint(pwrite(t.fd, guard, sizeof(guard), 0), ==,
                        sizeof(guard));
        qtest_writeq(t.qts, AUDIO + 0x20, 0x200);
        step(&t, 300000);
        g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x30), ==, 0);
        g_assert_cmphex(qtest_readq(t.qts, 0x1f310010) & 8, ==, 0);
        g_assert_cmphex(qtest_readq(t.qts, 0x14000250) & (1U << 26), ==,
                        1U << 26);
        g_assert_cmphex(qtest_readq(t.qts, 0x14000258), ==, 0x08000000);
        g_assert_cmpint(pread(t.fd, actual, sizeof(actual), 0), ==,
                        sizeof(actual));
        g_assert_cmpmem(guard, sizeof(guard), actual, sizeof(actual));
    }
    fixture_stop(&t);
}

static void backend_progress(void)
{
    AudioFixture t = fixture_start("-audiodev none,id=host "
        "-global sgi-mace-audio-test.audiodev=host");

    g_assert_cmpuint(observe(&t, 0x350), ==, 3);
    codec_power(&t);
    codec_set(&t, 26, 0x155);
    codec_set(&t, 27, 0x100);
    qtest_writeq(t.qts, AUDIO + 0x40, 0x280);
    step(&t, 100000000);
    g_assert_cmpuint(qtest_readq(t.qts, FIXTURE + 0x408) & UINT32_MAX, >, 4000);
    g_assert_cmphex(qtest_readq(t.qts, AUDIO + 0x48), ==, 0);
    g_assert_cmpuint(observe(&t, 0x358), <=, 2048);
    fixture_stop(&t);
}

static void mace_connection(void)
{
    QTestState *qts = qtest_init("-M none -cpu R10000 -nodefaults "
                                "-device sgi-mace,id=mace");

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/mace", 0, 0x1f000000);
    qtest_irq_intercept_out_named(qts, "/machine/peripheral/mace", "crime");
    g_assert_cmphex(qtest_readq(qts, AUDIO), ==, 1);
    g_assert_cmphex(qtest_readq(qts, AUDIO + 0x20), ==, 0x400);
    qtest_writeq(qts, 0x1f310000, 0x40000000);
    qtest_writeq(qts, AUDIO + 0x20, 0);
    qtest_writeq(qts, 0x1f310000, 0x40008000);
    g_assert_cmphex(qtest_readq(qts, 0x1f310000), ==, 0x40000000);
    qtest_writeq(qts, 0x1f310000, 0x40000001);
    g_assert_cmphex(qtest_readq(qts, 0x1f310000), ==, 0x40000001);
    qtest_clock_step(qts, 100);
    qtest_writeq(qts, AUDIO, 0);
    qtest_clock_step(qts, 700000);
    g_assert_cmphex(qtest_readq(qts, AUDIO), ==, 2);
    qtest_writeq(qts, AUDIO + 0x10, 1);
    qtest_writeq(qts, 0x1f310018, 1);
    g_assert_true(qtest_get_irq(qts, 6));
    g_assert_cmphex(qtest_readq(qts, 0x1f310010) & 255, ==, 1);
    qtest_writeq(qts, 0x1f310010, 255);
    g_assert_true(qtest_get_irq(qts, 6));
    qtest_writeq(qts, AUDIO + 0x10, 0);
    g_assert_false(qtest_get_irq(qts, 6));
    qtest_set_irq_in(qts, "/machine/peripheral/mace", "volume", 0, 1);
    qtest_writeq(qts, 0x1f310018, 2);
    g_assert_true(qtest_get_irq(qts, 6));
    g_assert_cmphex(qtest_readq(qts, AUDIO) & (1U << 23), ==, 1U << 23);
    qtest_writel(qts, AUDIO, 0);
    g_assert_true(qtest_get_irq(qts, 6));
    qtest_writel(qts, AUDIO + 4, 0);
    g_assert_false(qtest_get_irq(qts, 6));
    for (unsigned i = 0; i < 3; i++) {
        uint64_t value = 0x1234567887654321ULL + i;
        uint64_t pair = 0x1f340020 + 8 * i;

        qtest_writeq(qts, pair, value);
        g_assert_cmphex(qtest_readq(qts, pair), ==, value);
        g_assert_cmphex(qtest_readl(qts, pair), ==, value >> 32);
        g_assert_cmphex(qtest_readl(qts, pair + 4), ==, (uint32_t)value);
    }
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(qts, AUDIO), ==, 1);
    g_assert_cmphex(qtest_readq(qts, 0x1f340020), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/mace-audio/mace-connection", mace_connection);
    qtest_add_func("/mace-audio/backend-progress", backend_progress);
    qtest_add_func("/mace-audio/crime-dma-holes", crime_dma_holes);
    qtest_add_func("/mace-audio/capture-partial-ram", capture_partial_ram);
    qtest_add_func("/mace-audio/controller-registers", controller_registers);
    qtest_add_func("/mace-audio/codec-registers", codec_registers);
    qtest_add_func("/mace-audio/power-timing", power_timing);
    qtest_add_func("/mace-audio/response-pipeline", response_pipeline);
    qtest_add_func("/mace-audio/thresholds", thresholds);
    qtest_add_func("/mace-audio/rates-and-domains", rates_and_domains);
    qtest_add_func("/mace-audio/serial-start-status", serial_start_and_status);
    qtest_add_func("/mace-audio/dma-formats", dma_formats);
    qtest_add_func("/mace-audio/g711-encoding", g711_encoding);
    qtest_add_func("/mace-audio/capture-nodes", capture_nodes);
    qtest_add_func("/mace-audio/analog-routing", analog_routing);
    qtest_add_func("/mace-audio/dac-graph-gains", dac_graph_and_gains);
    qtest_add_func("/mace-audio/digital-mix-constraints",
                    digital_mix_constraints);
    qtest_add_func("/mace-audio/dma-faults-wrap", dma_faults_and_wrap);
    qtest_add_func("/mace-audio/timestamps", timestamps);
    qtest_add_func("/mace-audio/phase-latches", phase_latches);
    qtest_add_func("/mace-audio/dma-raw-mono", dma_raw_and_mono);
    qtest_add_func("/mace-audio/fifo-requests", fifo_requests);
    qtest_add_func("/mace-audio/partial-capture", partial_capture);
    qtest_add_func("/mace-audio/dma-full-ring", dma_full_ring);
    qtest_add_func("/mace-audio/capture-staged-resume", capture_staged_resume);
    qtest_add_func("/mace-audio/digital-attenuation", digital_attenuation);
    qtest_add_func("/mace-audio/actual-phase", actual_phase);
    qtest_add_func("/mace-audio/bit-conv-frequencies",
                    bit_conversion_frequencies);
    qtest_add_func("/mace-audio/phase-crossings", phase_crossings);
    qtest_add_func("/mace-audio/dac-mix-branches", dac_mix_branches);
    qtest_add_func("/mace-audio/underrun-mix", underrun_mix);
    qtest_add_func("/mace-audio/clock-continuity-reentry",
                    clock_continuity_and_reentry);
    qtest_add_data_func("/mace-audio/migration-active", NULL, migration_active);
    qtest_add_data_func("/mace-audio/migration-backend",
                         GUINT_TO_POINTER(1), migration_active);
    qtest_add_func("/mace-audio/migration-calibration", migration_calibration);
    return g_test_run();
}
