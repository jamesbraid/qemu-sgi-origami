/* SPDX-License-Identifier: BSD-3-Clause */
/* Synthetic register, clock and framebuffer fixtures, not SGI captures. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include <glib/gstdio.h>

#define BASE 0x16000000u
#define PATH "/machine/peripheral/gbe"
#define VT(n) (0x10000 + (n) * 4)
#define WID(n) (0x48000 + (n) * 4)
#define CMAP(n) (0x50000 + (n) * 4)
#define GMAP(n) (0x60000 + (n) * 4)
#define LIST 0x40010000u
#define TILE 0x40020000u

static uint32_t rd(QTestState *q, unsigned a)
{
    return qtest_readl(q, BASE + a);
}

static void wr(QTestState *q, unsigned a, uint32_t v)
{
    qtest_writel(q, BASE + a, v);
}

static QTestState *start(const char *extra)
{
    QTestState *q = qtest_initf("-M none -cpu R10000 -m 64M -display none "
        "-device sgi-o2-gbe,id=gbe,functional-display=on,"
        "ram=/objects/ram/ram[0] %s", extra);
    qtest_sysbus_mmio_map(q, PATH, 0, BASE);
    return q;
}

/* 97 clocks x 67 lines at 10 MHz; visible 31 x 4, producer 31 x 4. */
static void mode(QTestState *q)
{
    wr(q, VT(1), 97 | 67 << 12);
    wr(q, VT(4), 4 | 5 << 12);
    wr(q, VT(15), 31 << 12);
    wr(q, VT(16), 4 << 12);
    wr(q, VT(13), 10 | 78 << 12);
    wr(q, VT(14), 4);
    wr(q, VT(17), 77 | (uint32_t)(-63 & 4095) << 12);
    wr(q, VT(18), 43 | (uint32_t)(-62 & 4095) << 12);
    wr(q, 0, 3u << 28);
    wr(q, 4, 3 | 3u << 14 | 1u << 20); /* VCO 80 MHz / 8. */
}

static void palette(QTestState *q)
{
    for (unsigned i = 0; i < 256; i++) {
        wr(q, GMAP(i), i << 24 | i << 16 | i << 8);
    }
    wr(q, WID(0), 3 | 5 << 2);
}

static void descriptor(QTestState *q, unsigned slot, uint16_t value)
{
    uint8_t b[2];
    stw_be_p(b, value);
    qtest_memwrite(q, LIST - 0x40000000 + slot * 2, b, 2);
}

static void pixel(QTestState *q, unsigned off, uint32_t value)
{
    uint8_t b[4];
    stl_be_p(b, value);
    qtest_memwrite(q, TILE - 0x40000000 + off, b, 4);
}

static uint32_t screen(QTestState *q, unsigned w, unsigned h,
                       unsigned x, unsigned y)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-gbe-screen-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "screen.ppm", NULL);
    g_autofree char *header = g_strdup_printf("P6\n%u %u\n255\n", w, h);
    g_autofree char *data = NULL;
    gsize size;
    const uint8_t *p;
    unsigned stride = QEMU_ALIGN_UP(w * 3, 4);
    qtest_qmp_assert_success(q,
        "{'execute':'screendump','arguments':{'filename':%s}}", path);
    g_assert_true(g_file_get_contents(path, &data, &size, NULL));
    g_assert_cmpuint(size, ==, strlen(header) + stride * h);
    g_assert_cmpmem(data, strlen(header), header, strlen(header));
    p = (uint8_t *)data + strlen(header) + y * stride + x * 3;
    uint32_t result = p[0] << 16 | p[1] << 8 | p[2];
    g_unlink(path);
    g_rmdir(dir);
    return result;
}

static void frame(QTestState *q)
{
    qtest_clock_step(q, 650000);
}

static void setup(QTestState *q)
{
    mode(q);
    palette(q);
    descriptor(q, 0, TILE >> 16);
    wr(q, 0x30000, 2 << 13 | 1 << 5);
    wr(q, 0x30004, 128 << 16);
    wr(q, 0x3000c, LIST | 1);
    wr(q, VT(0), 0);
    frame(q);
}

static void test_reset(void)
{
    QTestState *q = start("");
    g_assert_cmphex(rd(q, 0) & 0x3ffffc1, ==, 0x3ffffc1);
    g_assert_cmphex(rd(q, 0x14), ==, 0x666);
    g_assert_cmphex(rd(q, 8), ==, 3);
    g_assert_cmphex(rd(q, 0x10), ==, 3);
    g_assert_cmphex(rd(q, 0xc), ==, 8);
    g_assert_cmphex(rd(q, VT(0)), ==, 0x80000000);
    for (unsigned i = 1; i < 20; i++) {
        g_assert_cmphex(rd(q, VT(i)), ==, 0);
    }
    mode(q);
    wr(q, 0x3000c, LIST | 1);
    wr(q, 0x78000, 0x98765432);
    wr(q, 0x70004, 3);
    qtest_system_reset(q);
    g_assert_cmphex(rd(q, VT(0)), ==, 0x80000000);
    g_assert_cmphex(rd(q, 0x3000c), ==, 0);
    g_assert_cmphex(rd(q, 0x70004), ==, 0);
    qtest_quit(q);
}


static void reset_planes(QTestState *q)
{
    const uint8_t overlay[] = { 7, 0, 9 };

    setup(q);
    pixel(q, 0, 0x123456fe);
    pixel(q, 4, 0xabcdefa7);
    pixel(q, 65536, 0x987654ab);
    pixel(q, 65540, 0x334455ab);
    descriptor(q, 16, 0x4007);
    qtest_memwrite(q, 0x70000, overlay, sizeof(overlay));
    wr(q, CMAP(4359), 0x89abcd00);
    wr(q, CMAP(4361), 0x76543200);
    wr(q, 0x20000, 1 << 5);
    wr(q, 0x20008, (LIST + 32) | 1);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x89abcd);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0xabcdef);
}

static void test_dma_reset_registers(void)
{
    QTestState *q = start("");
    const unsigned regs[] = { 0x30000, 0x20000 };
    const uint32_t bits[] = { 1u << 15, 1u << 13 };

    mode(q);
    wr(q, VT(0), 0);
    qtest_clock_step(q, 155);
    for (unsigned i = 0; i < 2; i++) {
        uint32_t shape = 1 << 5 | (i ? 0 : 1 << 13);
        uint32_t counter = rd(q, VT(0));

        g_assert_cmphex(rd(q, regs[i]), ==, 0);
        wr(q, regs[i], shape | bits[i] | 0xffff0000u);
        g_assert_cmphex(rd(q, regs[i]), ==, shape | bits[i]);
        qtest_writeb(q, BASE + regs[i], 0);
        qtest_writew(q, BASE + regs[i], 0);
        qtest_writeq(q, BASE + regs[i], 0);
        qtest_writel(q, BASE + regs[i] + 1, 0);
        g_assert_cmphex(rd(q, regs[i]), ==, shape | bits[i]);
        g_assert_cmphex(rd(q, VT(0)), ==, counter);
        wr(q, regs[i], bits[i] | shape | 1);
        g_assert_cmphex(rd(q, regs[i]), ==, bits[i] | shape | 1);
        wr(q, regs[i], shape | 2);
        g_assert_cmphex(rd(q, regs[i]), ==, shape | 2);
        wr(q, regs[i], bits[i]);
    }
    qtest_clock_step(q, 45);
    g_assert_cmphex(rd(q, VT(0)), ==, 2);
    qtest_system_reset(q);
    g_assert_cmphex(rd(q, 0x30000), ==, 0);
    g_assert_cmphex(rd(q, 0x20000), ==, 0);
    g_assert_cmphex(rd(q, 0x30008), ==, 0);
    g_assert_cmphex(rd(q, 0x20004), ==, 0);
    qtest_quit(q);

    q = qtest_init("-M none -cpu R10000 -display none "
                   "-device sgi-o2-gbe,id=gbe");
    qtest_sysbus_mmio_map(q, PATH, 0, BASE);
    for (unsigned i = 0; i < 2; i++) {
        wr(q, regs[i], 0x1234);
        wr(q, regs[i], bits[i] | 1);
        g_assert_cmphex(rd(q, regs[i]), ==, 0x1234);
    }
    qtest_quit(q);
}

static void test_dma_reset_planes(void)
{
    for (unsigned plane = 0; plane < 2; plane++) {
        QTestState *q = start("");
        unsigned reg = plane ? 0x20000 : 0x30000;
        uint32_t shape = 1 << 5 | (plane ? 0 : 2 << 13);
        uint32_t bit = 1u << (plane ? 13 : 15);
        uint8_t saved[2], actual[2];

        reset_planes(q);
        wr(q, VT(0), 17 | 2 << 12);
        qtest_memread(q, 0x10000, saved, sizeof(saved));
        wr(q, reg, shape | bit);
        g_assert_cmphex(rd(q, reg), ==, shape | bit);
        g_assert_cmphex(rd(q, VT(0)), ==, 17 | 2 << 12);
        g_assert_cmphex(rd(q, 0x30008), ==, LIST | 1);
        g_assert_cmphex(rd(q, 0x3000c), ==, LIST | 1);
        g_assert_cmphex(rd(q, 0x20004), ==, (LIST + 32) | 1);
        g_assert_cmphex(rd(q, 0x20008), ==, (LIST + 32) | 1);
        g_assert_cmphex(rd(q, 0x30004), ==, 128 << 16);
        g_assert_cmphex(rd(q, 0x40000), ==, 0);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==,
                        plane ? 0x123456 : 0x89abcd);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==,
                        plane ? 0xabcdef : 0);
        qtest_memread(q, 0x10000, actual, sizeof(actual));
        g_assert_cmpmem(actual, sizeof(actual), saved, sizeof(saved));
        /* Zero normal input still passes through the WID/color pipeline. */
        if (!plane) {
            wr(q, WID(0), 3);
            wr(q, CMAP(0), 0x31415900);
            g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x314159);
            wr(q, WID(0), 3 | 5 << 2);
        }
        descriptor(q, plane ? 16 : 0, plane ? 0x4008 : 0x4003);
        pixel(q, 0, 0x55667700);
        const uint8_t replacement[] = { 9, 0, 7 };
        qtest_memwrite(q, 0x80000, replacement, sizeof(replacement));
        wr(q, reg, shape | bit); /* Repeated assertion keeps the hold. */
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==,
                        plane ? 0x556677 : 0x89abcd);
        wr(q, reg, shape);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==,
                        plane ? 0x556677 : 0x89abcd);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==,
                        plane ? 0xabcdef : 0);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==,
                        plane ? 0x765432 : 0x89abcd);
        if (!plane) {
            const uint8_t transparent = 0;
            qtest_memwrite(q, 0x70000, &transparent, 1);
            wr(q, WID(0), 3 | 5 << 2);
            g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x987654);
        }
        qtest_quit(q);
    }
}

static void test_dma_reset_recovery(void)
{
    for (unsigned plane = 0; plane < 2; plane++) {
        QTestState *q = start("");
        unsigned reg = plane ? 0x20000 : 0x30000;
        unsigned active = plane ? 0x20004 : 0x30008;
        unsigned shadow = active + 4;
        unsigned slot = plane ? 16 : 0;
        uint32_t bit = 1u << (plane ? 13 : 15);
        uint32_t shape = 1 << 5 | (plane ? 0 : 2 << 13);

        reset_planes(q);
        descriptor(q, slot, 0x4400); /* Outside the linked RAM. */
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                        plane ? 0x123456 : 0);
        wr(q, reg, shape | bit);
        descriptor(q, slot, plane ? 0x4007 : 0x4002);
        descriptor(q, 32, plane ? 0x4007 : 0x4002);
        wr(q, shadow, (LIST + 64) | 1);
        frame(q);
        g_assert_cmphex(rd(q, active), ==, (LIST + 64) | 1);
        wr(q, VT(0), 0x80000000u | 11 | 2 << 12);
        wr(q, reg, shape);
        frame(q);
        g_assert_cmphex(rd(q, VT(0)), ==, 0x80000000u | 11 | 2 << 12);
        g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                        plane ? 0x123456 : 0);
        wr(q, VT(0), 11 | 2 << 12);
        qtest_clock_step(q, 100);
        g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                        plane ? 0x123456 : 0);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                        plane ? 0x89abcd : 0xabcdef);
        wr(q, 4, 0); /* PLL stopped; a pulse cannot invent blanking. */
        wr(q, reg, shape | bit);
        wr(q, reg, shape);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                        plane ? 0x123456 : 0);
        wr(q, 4, 3 | 3u << 14 | 1u << 20);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                        plane ? 0x89abcd : 0xabcdef);
        qtest_quit(q);
    }
}

static void test_dma_reset_migration(void)
{
    for (unsigned plane = 0; plane < 2; plane++) {
        for (unsigned released = 0; released < 2; released++) {
            g_autofree char *dir = g_dir_make_tmp("gbe-reset-migrate-XXXXXX",
                                                 NULL);
            g_autofree char *sock = g_build_filename(dir, "migration.sock",
                                                    NULL);
            g_autofree char *uri = g_strdup_printf("unix:%s", sock);
            QTestState *from = start("");
            QTestState *to = start("-incoming defer");
            unsigned reg = plane ? 0x20000 : 0x30000;
            uint32_t bit = 1u << (plane ? 13 : 15);
            uint32_t shape = 1 << 5 | (plane ? 0 : 2 << 13);

            reset_planes(from);
            qtest_irq_intercept_out_named(from, PATH, "sysbus-irq");
            qtest_irq_intercept_out_named(to, PATH, "sysbus-irq");
            wr(from, VT(8), 7 | 6 << 12);
            wr(from, reg, shape | bit);
            descriptor(from, plane ? 16 : 0, plane ? 0x4007 : 0x4003);
            if (released) {
                wr(from, reg, shape);
            }
            uint32_t counter = rd(from, VT(0));
            qtest_qmp_assert_success(to,
                "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
            sgi_migrate_stopped(from, uri);
            sgi_wait_migration(to, "completed");
            g_assert_cmphex(rd(to, VT(0)), ==, counter);
            g_assert_cmphex(rd(to, reg), ==, shape | (released ? 0 : bit));
            g_assert_cmphex(screen(to, 31, 4, plane ? 0 : 1, 0), ==,
                            plane ? 0x123456 : 0);
            g_assert_cmpuint(qtest_get_irq_raise_count(to, 0), ==, 0);
            if (!released) {
                wr(from, reg, shape);
                wr(to, reg, shape);
            }
            frame(from);
            frame(to);
            g_assert_cmphex(rd(to, VT(0)), ==, rd(from, VT(0)));
            g_assert_cmphex(screen(to, 31, 4, plane ? 0 : 1, 0), ==,
                            screen(from, 31, 4, plane ? 0 : 1, 0));
            g_assert_cmphex(screen(to, 31, 4, plane ? 0 : 1, 0), ==,
                            plane ? 0x89abcd : 0x334455);
            g_assert_cmpuint(qtest_get_irq_raise_count(to, 0), ==, 1);
            qtest_quit(from);
            qtest_quit(to);
            g_unlink(sock);
            g_rmdir(dir);
        }
    }
}


static unsigned dma_faults(const char *path)
{
    g_autofree char *data = NULL;
    const char *p;
    unsigned count = 0;

    g_assert_true(g_file_get_contents(path, &data, NULL, NULL));
    p = data;
    while ((p = strstr(p, "sgi-o2-gbe: DMA outside linked RAM"))) {
        count++;
        p++;
    }
    return count;
}

static void test_dma_reset_active_controls(void)
{
    for (unsigned plane = 0; plane < 2; plane++) {
        QTestState *q = start("");
        unsigned reg = plane ? 0x20000 : 0x30000;
        unsigned active = plane ? 0x20004 : 0x30008;
        uint32_t shape = 1 << 5 | (plane ? 0 : 2 << 13);
        uint32_t bit = 1u << (plane ? 13 : 15);
        uint32_t original = (LIST + (plane ? 32 : 0)) | 1;

        reset_planes(q);
        uint32_t counter = rd(q, VT(0));
        qtest_writeb(q, BASE + reg, 0xff);
        qtest_writew(q, BASE + reg, UINT16_MAX);
        qtest_writeq(q, BASE + reg, UINT64_MAX);
        qtest_writel(q, BASE + reg + 1, UINT32_MAX);
        g_assert_cmphex(rd(q, reg), ==, shape);
        g_assert_cmphex(rd(q, VT(0)), ==, counter);
        wr(q, WID(0), 3 | 5 << 2);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x89abcd);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0xabcdef);
        wr(q, reg, shape | bit);
        wr(q, active, LIST + 64);
        g_assert_cmphex(rd(q, active), ==, LIST + 64);
        g_assert_cmphex(rd(q, active + 4), ==, original);
        wr(q, active, (LIST + 64) | 1);
        g_assert_cmphex(rd(q, active), ==, (LIST + 64) | 1);
        g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                        plane ? 0x123456 : 0);
        descriptor(q, 48, plane ? 0x4007 : 0x4003);
        wr(q, active + 4, (LIST + 96) | 1);
        g_assert_cmphex(rd(q, active), ==, (LIST + 64) | 1);
        frame(q);
        g_assert_cmphex(rd(q, active), ==, (LIST + 96) | 1);
        g_assert_cmphex(rd(q, active + 4), ==, (LIST + 96) | 1);
        wr(q, reg, shape);
        g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                        plane ? 0x123456 : 0);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                        plane ? 0x89abcd : 0x334455);
        qtest_quit(q);
    }
}

static void test_dma_reset_dma_sentinels(void)
{
    for (unsigned plane = 0; plane < 2; plane++) {
        for (unsigned released = 0; released < 2; released++) {
            g_autofree char *dir = g_dir_make_tmp("gbe-dma-sentinel-XXXXXX",
                                                 NULL);
            g_autofree char *log = g_build_filename(dir, "source.txt", NULL);
            g_autofree char *target = g_build_filename(dir, "target.txt", NULL);
            g_autofree char *args = g_strdup_printf("-d guest_errors -D %s",
                                                   log);
            g_autofree char *incoming = g_strdup_printf(
                "-incoming defer -d guest_errors -D %s", target);
            g_autofree char *sock = g_build_filename(dir, "migration.sock",
                                                    NULL);
            g_autofree char *uri = g_strdup_printf("unix:%s", sock);
            QTestState *from = start(args);
            QTestState *to = start(incoming);
            QTestState *both[] = { from, to };
            unsigned reg = plane ? 0x20000 : 0x30000;
            unsigned active = plane ? 0x20004 : 0x30008;
            unsigned slot = plane ? 16 : 0;
            uint32_t shape = 1 << 5 | (plane ? 0 : 2 << 13);
            uint32_t bit = 1u << (plane ? 13 : 15);
            uint32_t original = (LIST + slot * 2) | 1;

            reset_planes(from);
            wr(from, reg, shape | bit);
            /* A descriptor read here would emit an observable DMA failure. */
            wr(from, active, 0x44000001);
            wr(from, active + 4, 0x44000001);
            frame(from);
            g_assert_cmpuint(dma_faults(log), ==, 0);
            if (released) {
                wr(from, reg, shape);
            }
            qtest_qmp_assert_success(to,
                "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
            sgi_migrate_stopped(from, uri);
            sgi_wait_migration(to, "completed");
            g_assert_cmpuint(dma_faults(log), ==, 0);
            g_assert_cmpuint(dma_faults(target), ==, 0);
            for (unsigned i = 0; i < 2; i++) {
                QTestState *q = both[i];
                const char *path = i ? target : log;

                g_assert_cmphex(rd(q, reg), ==,
                                shape | (released ? 0 : bit));
                g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                                plane ? 0x123456 : 0);
                g_assert_cmpuint(dma_faults(path), ==, 0);
                if (!released) {
                    frame(q);
                    rd(q, VT(0));
                    g_assert_cmpuint(dma_faults(path), ==, 0);
                    wr(q, reg, shape);
                }
                g_assert_cmpuint(dma_faults(path), ==, 0);
                frame(q);
                rd(q, VT(0));
                g_assert_cmpuint(dma_faults(path), ==, 1);
                /* A valid list with a bad tile distinguishes pixel DMA. */
                wr(q, reg, shape | bit);
                wr(q, active, original);
                wr(q, active + 4, original);
                descriptor(q, slot, 0x4400);
                frame(q);
                rd(q, VT(0));
                g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                                plane ? 0x123456 : 0);
                g_assert_cmpuint(dma_faults(path), ==, 1);
                wr(q, reg, shape);
                g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                                plane ? 0x123456 : 0);
                g_assert_cmpuint(dma_faults(path), ==, 1);
                frame(q);
                rd(q, VT(0));
                g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                                plane ? 0x123456 : 0);
                g_assert_cmpuint(dma_faults(path), ==, 2);
                wr(q, reg, shape | bit);
                descriptor(q, slot, plane ? 0x4007 : 0x4002);
                wr(q, reg, shape);
                g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                                plane ? 0x123456 : 0);
                frame(q);
                rd(q, VT(0));
                g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                                plane ? 0x89abcd : 0xabcdef);
                g_assert_cmpuint(dma_faults(path), ==, 2);
            }
            qtest_quit(from);
            qtest_quit(to);
            g_unlink(log);
            g_unlink(target);
            g_unlink(sock);
            g_rmdir(dir);
        }
    }
}

static void test_dma_reset_did_cursor(void)
{
    QTestState *q = start("");
    uint8_t table[514] = { 0 };

    reset_planes(q);
    stl_be_p(table, 2047 | 1 << 19);
    stw_be_p(table + 512, 2047 | 1 << 11);
    qtest_memwrite(q, 0x60000, table, sizeof(table));
    wr(q, WID(0), 3);
    wr(q, WID(1), 3 | 1 << 5);
    wr(q, WID(2), 3 | 2 << 5);
    wr(q, CMAP(0), 0x10203000);
    wr(q, CMAP(256), 0x24681300);
    wr(q, CMAP(512), 0x13579b00);
    wr(q, 0x40004, 0x14006);
    frame(q);
    wr(q, 0x70000, 31 | 31 << 16);
    wr(q, 0x70008, 0x10293800);
    wr(q, 0x78000, 1u << 30);
    wr(q, 0x70004, 1);
    wr(q, 0x30000, 1u << 15 | 2 << 13 | 1 << 5);
    wr(q, 0x20000, 1u << 13 | 1 << 5);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x102938);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x246813);
    stw_be_p(table + 512, 2047 | 2 << 11);
    qtest_memwrite(q, 0x90000, table, sizeof(table));
    wr(q, 0x40004, 0x14009);
    frame(q);
    g_assert_cmphex(rd(q, 0x40000), ==, 0x14009);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x13579b);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x102938);
    wr(q, 0x70008, 0xa1b2c300);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0xa1b2c3);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x13579b);
    wr(q, 0x78000, 1u << 28);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x13579b);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0xa1b2c3);
    qtest_quit(q);
}

static void test_clock(void)
{
    QTestState *q = start("");
    mode(q);
    wr(q, VT(0), 0);
    qtest_clock_step(q, 1100);
    g_assert_cmphex(rd(q, VT(0)), ==, 11);
    for (unsigned i = 0; i < 20; i++) {
        g_assert_cmphex(rd(q, VT(0)), ==, 11);
    }
    wr(q, VT(0), 0x8000000b);
    qtest_clock_step(q, 12345);
    g_assert_cmphex(rd(q, VT(0)), ==, 0x8000000b);
    wr(q, VT(0), 11);
    qtest_clock_step(q, 8600);
    g_assert_cmphex(rd(q, VT(0)), ==, 1 << 12);
    wr(q, 4, 1u << 20); /* 20 MHz VCO: invalid. */
    g_assert_cmphex(rd(q, 4) & (3 << 22), ==, 3 << 22);
    qtest_clock_step(q, 10000);
    g_assert_cmphex(rd(q, VT(0)), ==, 1 << 12);
    qtest_quit(q);
}

static void test_rgb8_geometry(void)
{
    QTestState *q = start("");
    setup(q);
    pixel(q, 0, 0x123456fe);
    pixel(q, 29 * 4, 0x987654ab);
    pixel(q, 30 * 4, 0xabcdef01);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    g_assert_cmphex(screen(q, 31, 4, 29, 0), ==, 0x987654);
    g_assert_cmphex(screen(q, 31, 4, 30, 0), ==, 0xabcdef);
    /* PIXEN shifts producer without changing surface geometry. */
    wr(q, VT(13), 10 | 79 << 12);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x123456);
    /* Cursor remains available outside producer. */
    wr(q, 0x70000, 31 | 31 << 16);
    wr(q, 0x70008, 0x10203000);
    wr(q, 0x78000, 1u << 30);
    wr(q, 0x70004, 1);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x102030);
    qtest_quit(q);
}

static void test_formats(void)
{
    static const struct {
        uint32_t input, expected;
        unsigned mode;
    } cases[] = {
        { 0x000000a7, 0x112233, 0 },
        { 0x00000abc, 0x445566, 1 },
        { 0x000000a7, 0xb624ff, 2 },
        { 0x0000a5c0, 0xaa55cc, 3 },
        { 0x00007c1f, 0xff00ff, 4 },
        { 0x123456fe, 0x123456, 5 },
    };
    QTestState *q = start("");
    setup(q);
    wr(q, CMAP(0xa7), 0x11223300);
    wr(q, CMAP(0xabc), 0x44556600);
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        wr(q, WID(0), 3 | cases[i].mode << 2);
        pixel(q, 0, cases[i].input);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, cases[i].expected);
    }
    /* Independent component lookup and gamma channels. */
    wr(q, WID(0), 3 | 5 << 2 | 1 << 5);
    wr(q, CMAP(0x112), 0xa1000000);
    wr(q, CMAP(0x134), 0x00b20000);
    wr(q, CMAP(0x156), 0x0000c300);
    wr(q, GMAP(0xa1), 0x44000000);
    wr(q, GMAP(0xb2), 0x00550000);
    wr(q, GMAP(0xc3), 0x00006600);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x445566);
    g_assert_cmphex(rd(q, 0x58000), ==, 0);
    qtest_quit(q);
}

static void test_shadow_and_bounds(void)
{
    QTestState *q = start("");
    mode(q);
    wr(q, 0x3000c, LIST | 1);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(rd(q, 0x30008), ==, 0);
    wr(q, VT(0), 0);
    frame(q);
    g_assert_cmphex(rd(q, 0x30008), ==, LIST | 1);
    qtest_system_reset(q);
    setup(q);
    descriptor(q, 0, 0x4400); /* First byte beyond 64 MiB RAM. */
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
    qtest_quit(q);
}

static void test_i2c_idle(void)
{
    QTestState *q = start("");
    wr(q, 8, 0); /* Release both lines. */
    g_assert_cmphex(rd(q, 8), ==, 0);
    wr(q, 8, 1); /* START: SDA low with clock high. */
    g_assert_cmphex(rd(q, 8), ==, 1);
    g_assert_cmphex(rd(q, 0x10), ==, 3);
    qtest_system_reset(q);
    g_assert_cmphex(rd(q, 8), ==, 3);
    qtest_quit(q);
}


static void test_multitile(void)
{
    QTestState *q = start("");
    mode(q);
    palette(q);
    wr(q, VT(1), 257 | 200 << 12);
    wr(q, VT(4), 129 | 130 << 12);
    wr(q, VT(15), 136 << 12);
    wr(q, VT(16), 129 << 12);
    wr(q, VT(13), 115 | 238 << 12);
    wr(q, VT(14), 129);
    wr(q, 0x30000, 2 << 13 | 1 << 5 | 1);
    wr(q, 0x30004, 129 << 16);
    wr(q, 0x3000c, LIST | 1);
    for (unsigned i = 0; i < 4; i++) {
        descriptor(q, i, (TILE >> 16) + i);
    }
    pixel(q, 0, 0x11223300);
    pixel(q, 65536, 0x44556600);
    pixel(q, 2 * 65536, 0x77889900);
    pixel(q, 3 * 65536 + 7 * 4, 0xaabbcc00);
    wr(q, VT(0), 0);
    qtest_clock_step(q, 5200000);
    g_assert_cmphex(screen(q, 136, 129, 0, 0), ==, 0x112233);
    g_assert_cmphex(screen(q, 136, 129, 128, 0), ==, 0x445566);
    g_assert_cmphex(screen(q, 136, 129, 0, 128), ==, 0x778899);
    g_assert_cmphex(screen(q, 136, 129, 135, 128), ==, 0xaabbcc);
    qtest_quit(q);
}

static void i2c_lines(QTestState *q, unsigned reg, unsigned levels)
{
    wr(q, reg, ~levels & 3);
}

static void i2c_start(QTestState *q, unsigned reg)
{
    i2c_lines(q, reg, 3);
    i2c_lines(q, reg, 2);
}

static void i2c_stop(QTestState *q, unsigned reg)
{
    i2c_lines(q, reg, 0);
    i2c_lines(q, reg, 2);
    i2c_lines(q, reg, 3);
}

static bool i2c_send_byte(QTestState *q, unsigned reg, uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        unsigned data = byte >> i & 1;
        i2c_lines(q, reg, data);
        i2c_lines(q, reg, data | 2);
    }
    i2c_lines(q, reg, 1);
    i2c_lines(q, reg, 3);
    bool ack = rd(q, reg) & 1;
    i2c_lines(q, reg, 1);
    return ack;
}

static uint8_t i2c_recv_byte(QTestState *q, unsigned reg, bool ack)
{
    unsigned byte = 0;
    for (unsigned i = 0; i < 8; i++) {
        i2c_lines(q, reg, 1);
        i2c_lines(q, reg, 3);
        byte = byte << 1 | !(rd(q, reg) & 1);
    }
    i2c_lines(q, reg, ack ? 0 : 1);
    i2c_lines(q, reg, ack ? 2 : 3);
    i2c_lines(q, reg, 1);
    return byte;
}

static void test_i2c_slaves(void)
{
    QTestState *q = start("-device at24c-eeprom,bus=/gbe/i2c,"
                         "address=80,rom-size=256 "
                         "-device at24c-eeprom,bus=/gbe/i2cfp,address=80,rom-size=256");
    unsigned regs[] = { 8, 0x10 };
    for (unsigned i = 0; i < 2; i++) {
        unsigned reg = regs[i];
        i2c_start(q, reg);
        g_assert_true(i2c_send_byte(q, reg, 0xa0));
        g_assert_true(i2c_send_byte(q, reg, 7));
        g_assert_true(i2c_send_byte(q, reg, 0x31 + i * 0x42));
        i2c_stop(q, reg);
    }
    for (unsigned i = 0; i < 2; i++) {
        unsigned reg = regs[i];
        i2c_start(q, reg);
        g_assert_true(i2c_send_byte(q, reg, 0xa0));
        g_assert_true(i2c_send_byte(q, reg, 7));
        i2c_start(q, reg);
        g_assert_true(i2c_send_byte(q, reg, 0xa1));
        g_assert_cmphex(i2c_recv_byte(q, reg, false), ==, 0x31 + i * 0x42);
        i2c_stop(q, reg);
        i2c_start(q, reg);
        g_assert_false(i2c_send_byte(q, reg, 0xb0));
        i2c_stop(q, reg);
    }
    qtest_quit(q);
}

static void test_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-gbe-migration-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");
    setup(from);
    pixel(from, 0, 0x123456ab);
    descriptor(from, 16, (TILE >> 16) + 1);
    pixel(from, 65536, 0xabcdef00);
    wr(from, 0x3000c, (LIST + 32) | 1);
    wr(from, VT(8), 7 | 6 << 12);
    qtest_irq_intercept_out_named(from, PATH, "sysbus-irq");
    qtest_irq_intercept_out_named(to, PATH, "sysbus-irq");
    uint32_t counter = rd(from, VT(0));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(rd(to, VT(0)), ==, counter);
    g_assert_cmphex(rd(to, 0x30008), ==, LIST | 1);
    g_assert_cmphex(rd(to, 0x3000c), ==, (LIST + 32) | 1);
    g_assert_cmphex(screen(to, 31, 4, 0, 0), ==, 0x123456);
    g_assert_cmpuint(qtest_get_irq_raise_count(to, 0), ==, 0);
    frame(from);
    frame(to);
    g_assert_cmphex(rd(to, VT(0)), ==, rd(from, VT(0)));
    g_assert_cmphex(rd(to, 0x30008), ==, (LIST + 32) | 1);
    g_assert_cmphex(screen(to, 31, 4, 0, 0), ==,
                    screen(from, 31, 4, 0, 0));
    g_assert_cmphex(screen(to, 31, 4, 0, 0), ==, 0xabcdef);
    g_assert_cmpuint(qtest_get_irq_raise_count(to, 0), ==, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(to, 0), ==,
                     qtest_get_irq_raise_count(from, 0));
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_rmdir(dir);
}


static void test_did_overlay(void)
{
    QTestState *q = start("");
    uint8_t table[1024] = { 0 };
    uint8_t tile[65536];
    setup(q);
    for (unsigned i = 0; i < sizeof(tile); i += 4) {
        stl_be_p(tile + i, 1);
    }
    qtest_memwrite(q, TILE - 0x40000000, tile, sizeof(tile));
    /* Inclusive Y=0 uses block 1. Later rows use block 2. */
    stl_be_p(table, 1 << 19);
    stl_be_p(table + 4, 2047 | 2 << 19);
    stw_be_p(table + 512, 1 << 11);
    stw_be_p(table + 514, 2047 | 2 << 11);
    qtest_memwrite(q, 0x60000, table, sizeof(table));
    stw_be_p(table, 2047 | 3 << 11);
    qtest_memwrite(q, 0x60400, table, 2);
    for (unsigned i = 1; i <= 3; i++) {
        wr(q, WID(i), 3 | i << 5);
    }
    wr(q, CMAP(257), 0xff000000);
    wr(q, CMAP(513), 0x00ff0000);
    wr(q, CMAP(769), 0x0000ff00);
    wr(q, 0x40004, 0x14006);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0xff0000);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x00ff00);
    g_assert_cmphex(screen(q, 31, 4, 0, 1), ==, 0x0000ff);
    /* Changing the DID vertical alignment advances the selected run. */
    wr(q, VT(17), 77 | (uint32_t)(-62 & 4095) << 12);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x0000ff);
    /* An overlay index overrides normal pixels, but zero stays transparent. */
    descriptor(q, 16, 0x4007);
    uint8_t overlay[] = { 7, 0 };
    qtest_memwrite(q, 0x70000, overlay, sizeof(overlay));
    wr(q, CMAP(4359), 0x89abcd00);
    wr(q, 0x20000, 1 << 5);
    wr(q, 0x20008, (LIST + 32) | 1);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x89abcd);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x0000ff);
    wr(q, GMAP(0x89), 0x05000000);
    wr(q, GMAP(0xab), 0x00060000);
    wr(q, GMAP(0xcd), 0x00000700);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x050607);
    wr(q, GMAP(0x10), 0xff000000);
    wr(q, GMAP(0x20), 0x00ee0000);
    wr(q, GMAP(0x30), 0x0000dd00);
    wr(q, 0x70000, 31 | 31 << 16);
    wr(q, 0x70008, 0x10203000);
    wr(q, 0x78000, 1u << 30);
    wr(q, 0x70004, 1);
    /* Cursor wins over nonzero overlay and bypasses the nonidentity gamma. */
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x102030);
    qtest_quit(q);
}

static void assert_blocked(QTestState *q)
{
    g_autoptr(QDict) reply = qtest_qmp(q,
        "{'execute':'migrate','arguments':{'uri':'unix:/nonexistent'}}");
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(reply, "error"),
                                        "desc"), "GBE I2C transfer is active"));
}

static void test_i2c_reset_blocker(void)
{
    QTestState *q = start("-device at24c-eeprom,bus=/gbe/i2c,"
                         "address=80,rom-size=256");
    i2c_start(q, 8);
    assert_blocked(q);
    qtest_system_reset(q);
    i2c_start(q, 8);
    g_assert_true(i2c_send_byte(q, 8, 0xa0));
    assert_blocked(q);
    qtest_system_reset(q);
    i2c_start(q, 8);
    g_assert_true(i2c_send_byte(q, 8, 0xa1));
    i2c_lines(q, 8, 1);
    i2c_lines(q, 8, 3);
    assert_blocked(q);
    qtest_system_reset(q);
    i2c_start(q, 8);
    g_assert_true(i2c_send_byte(q, 8, 0xa0));
    i2c_stop(q, 8);
    /* An idle bus reaches URI validation instead of the transfer blocker. */
    g_autoptr(QDict) reply = qtest_qmp(q,
        "{'execute':'migrate','arguments':{'uri':'invalid:unsupported'}}");
    g_assert_null(strstr(qdict_get_str(qdict_get_qdict(reply, "error"), "desc"),
                         "GBE I2C transfer is active"));
    qtest_quit(q);
}


static void test_events(void)
{
    QTestState *q = start("");
    mode(q);
    qtest_irq_intercept_out_named(q, PATH, "sysbus-irq");
    wr(q, VT(8), 2 | 1 << 12);
    wr(q, VT(9), 67 | 2 << 12);
    wr(q, VT(0), 0);
    qtest_clock_step(q, 9700);
    g_assert_cmpuint(qtest_get_irq_raise_count(q, 0), ==, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(q, 1), ==, 0);
    g_assert_false(qtest_get_irq(q, 0));
    qtest_clock_step(q, 9700);
    g_assert_cmpuint(qtest_get_irq_raise_count(q, 1), ==, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(q, 2), ==, 1);
    wr(q, VT(8), 2 | 2 << 12);
    g_assert_cmpuint(qtest_get_irq_raise_count(q, 0), ==, 1);
    qtest_clock_step(q, 630500);
    g_assert_cmpuint(qtest_get_irq_raise_count(q, 3), ==, 1);
    qtest_clock_step(q, 19400);
    g_assert_cmpuint(qtest_get_irq_raise_count(q, 0), ==, 2);
    g_assert_cmpuint(qtest_get_irq_raise_count(q, 1), ==, 2);
    g_assert_cmpuint(qtest_get_irq_raise_count(q, 2), ==, 2);
    qtest_quit(q);
}

static void test_depth_and_halves(void)
{
    QTestState *q = start("");
    setup(q);
    pixel(q, 0, 0x12ab3c4d);
    wr(q, WID(0), 1 | 3 << 2);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x33cc44);
    wr(q, WID(0), 2 | 3 << 2);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x1122aa);
    wr(q, 0x30000, 1 << 13 | 1 << 5);
    wr(q, WID(0), 3 | 3 << 2);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x1122aa);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x33cc44);
    wr(q, 0x30000, 1 << 5);
    wr(q, WID(0), 3);
    wr(q, CMAP(0x12), 0x12345600);
    wr(q, CMAP(0xab), 0xabcdef00);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0xabcdef);
    /* RGB8 cannot fit a half-word: reject only normal pixels. */
    wr(q, WID(0), 1 | 5 << 2);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
    qtest_quit(q);
}


static void test_access_and_unsupported(void)
{
    QTestState *q = start("");
    wr(q, 0x30000, 0x1234);
    qtest_writeb(q, BASE + 0x30000, 0xff);
    qtest_writew(q, BASE + 0x30000, 0xffff);
    qtest_writel(q, BASE + 0x30001, 0xffffffff);
    qtest_writeq(q, BASE + 0x30000, UINT64_MAX);
    g_assert_cmphex(rd(q, 0x30000), ==, 0x1234);
    wr(q, 0x30000, 0x8000);
    g_assert_cmphex(rd(q, 0x30000), ==, 0x8000);
    wr(q, 0x20000, 0x1234);
    wr(q, 0x20000, 0x2000);
    g_assert_cmphex(rd(q, 0x20000), ==, 0x2000);
    wr(q, 4, 1 << 24);
    g_assert_cmphex(rd(q, 4) & 0x10ffff, ==, 0);
    wr(q, 0xc, 0);
    g_assert_cmphex(rd(q, 0xc), ==, 8);
    wr(q, VT(19), 123);
    g_assert_cmphex(rd(q, VT(19)), ==, 0);
    qtest_quit(q);
}

static void test_frame_lock_outputs(void)
{
    QTestState *q = start("");
    mode(q);
    qtest_irq_intercept_out_named(q, PATH, "timing");
    wr(q, VT(3), 20 | 10 << 12);
    wr(q, VT(7), 3 | 4 << 12);
    wr(q, VT(0), 0);
    qtest_clock_step(q, 1000);
    g_assert_true(qtest_get_irq(q, 1));
    qtest_clock_step(q, 1000);
    g_assert_false(qtest_get_irq(q, 1));
    qtest_set_irq_in(q, PATH, NULL, 11, 1);
    qtest_clock_step(q, 7700);
    g_assert_cmphex(rd(q, VT(0)), ==, 4 << 12);
    /* A held frame-lock input is consumed once, then counting continues. */
    qtest_clock_step(q, 9700);
    g_assert_cmphex(rd(q, VT(0)), ==, 5 << 12);
    qtest_quit(q);
}


static void test_did_overlapping_cache(void)
{
    QTestState *q = start("");
    uint8_t table[8], terminator[2];
    setup(q);
    pixel(q, 0, 1);
    pixel(q, 31 * 4, 1);
    wr(q, WID(0), 3);
    wr(q, WID(1), 3 | 1 << 5);
    wr(q, CMAP(1), 0x12345600);
    wr(q, CMAP(257), 0xabcdef00);
    /* Frame[0] selects a line table overlapping frame[1]'s first half. */
    stl_be_p(table, 0x00001000);
    stl_be_p(table + 4, 0x07ff1001);
    stw_be_p(terminator, 2047 | 1 << 11);
    qtest_memwrite(q, 0x60000, table, sizeof(table));
    qtest_memwrite(q, 0x60000 + 65476, terminator, sizeof(terminator));
    wr(q, 0x40004, 0x14006);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    g_assert_cmphex(screen(q, 31, 4, 0, 1), ==, 0xabcdef);
    qtest_quit(q);
}


static void test_clock_pin_phase(void)
{
    QTestState *q = start("");
    mode(q);
    wr(q, VT(0), 0);
    qtest_clock_step(q, 1);
    wr(q, 0, 3u << 28 | 64); /* Auxiliary output latch, same clock. */
    qtest_clock_step(q, 99);
    g_assert_cmphex(rd(q, VT(0)), ==, 1);
    qtest_clock_step(q, 1);
    wr(q, 4, 3 | 3u << 14 | 1u << 20); /* Same PLL encoding. */
    qtest_clock_step(q, 99);
    g_assert_cmphex(rd(q, VT(0)), ==, 2);
    qtest_clock_step(q, 1);
    /* Equivalent M/N encoding retains the same 10 MHz dot frequency. */
    wr(q, 4, 7 | 1 << 8 | 3u << 14 | 1u << 20);
    qtest_clock_step(q, 99);
    g_assert_cmphex(rd(q, VT(0)), ==, 3);
    qtest_quit(q);
}

static void test_descriptor_cache(void)
{
    QTestState *q = start("");
    setup(q);
    pixel(q, 0, 0x12345600);
    pixel(q, 65536, 0xabcdef00);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    descriptor(q, 0, (TILE >> 16) + 1);
    wr(q, WID(0), 3 | 5 << 2);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0xabcdef);
    descriptor(q, 16, TILE >> 16);
    wr(q, 0x30008, (LIST + 32) | 1);
    g_assert_cmphex(rd(q, 0x30008), ==, (LIST + 32) | 1);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0xabcdef);
    frame(q);
    g_assert_cmphex(rd(q, 0x30008), ==, LIST | 1);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0xabcdef);
    wr(q, 0x3000c, (LIST + 32) | 1);
    g_assert_cmphex(rd(q, 0x30008), ==, LIST | 1);
    frame(q);
    g_assert_cmphex(rd(q, 0x30008), ==, (LIST + 32) | 1);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    qtest_quit(q);
}

static void test_did_boundary_malformed(void)
{
    QTestState *q = start("");
    uint8_t entry[4];
    setup(q);
    pixel(q, 0, 1);
    wr(q, WID(0), 3);
    wr(q, WID(1), 3 | 1 << 5);
    wr(q, CMAP(1), 0x12345600);
    wr(q, CMAP(257), 0xabcdef00);
    stl_be_p(entry, 2047 | 255 << 11 | 127 << 19);
    qtest_memwrite(q, 0x60000, entry, 4);
    stw_be_p(entry, 2047 | 1 << 11);
    qtest_memwrite(q, 0x6fffe, entry, 2);
    wr(q, 0x40004, 0x14006);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0xabcdef);
    /* A missing terminator must not read beyond this 512-byte sub-block. */
    stw_be_p(entry, 10 | 1 << 11);
    qtest_memwrite(q, 0x6fffe, entry, 2);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    /* The next frame retries an invalid physical DID base without wrapping. */
    wr(q, 0x40004, 0x14400);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    qtest_quit(q);
}

static void test_wrapped_producer(void)
{
    QTestState *q = start("");
    setup(q);
    pixel(q, 0, 0x12345600);
    pixel(q, 4, 0xabcdef00);
    pixel(q, 30 * 4, 0x11223300);
    pixel(q, 31 * 4, 0x44556600);
    wr(q, VT(15), 90 | 24 << 12);
    wr(q, VT(16), 65 | 2 << 12);
    wr(q, VT(13), 3 | 71 << 12);
    wr(q, VT(14), 2 | 65 << 12);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    g_assert_cmphex(screen(q, 31, 4, 30, 0), ==, 0x112233);
    wr(q, VT(13), 2 | 71 << 12);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 29, 0), ==, 0);
    g_assert_cmphex(screen(q, 31, 4, 30, 0), ==, 0);
    wr(q, VT(13), 3 | 70 << 12); /* Source begins one pixel before crop. */
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0xabcdef);
    g_assert_cmphex(screen(q, 31, 4, 30, 0), ==, 0x445566);
    wr(q, VT(13), 71 | 71 << 12);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
    wr(q, VT(13), 98 | 71 << 12);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
    /* Zero plane input still passes through an indexed color lookup. */
    wr(q, WID(0), 3);
    wr(q, CMAP(0), 0x10203000);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x102030);
    wr(q, VT(15), 98 | 24 << 12);
    /* Empty geometry uses the existing scanout placeholder, not DMA width. */
    (void)screen(q, 640, 480, 0, 0);
    uint32_t before = rd(q, VT(0));
    qtest_clock_step(q, 100);
    g_assert_cmphex(rd(q, VT(0)), !=, before);
    wr(q, VT(15), 97 | 31 << 12); /* Endpoint equal to period aliases zero. */
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x102030);
    qtest_quit(q);
}

static void test_netbsd_vertical_phase(void)
{
    QTestState *q = start("");
    uint8_t table[1026] = { 0 }, tile[65536];
    setup(q);
    for (unsigned i = 0; i < sizeof(tile); i += 4) {
        stl_be_p(tile + i, 1);
    }
    qtest_memwrite(q, TILE - 0x40000000, tile, sizeof(tile));
    stl_be_p(table, 1 | 1 << 19);
    stl_be_p(table + 4, 2047 | 2 << 19);
    stw_be_p(table + 512, 2047 | 1 << 11);
    stw_be_p(table + 1024, 2047 | 2 << 11);
    qtest_memwrite(q, 0x60000, table, sizeof(table));
    wr(q, WID(1), 3 | 1 << 5);
    wr(q, WID(2), 3 | 2 << 5);
    wr(q, CMAP(257), 0x12345600);
    wr(q, CMAP(513), 0xabcdef00);
    wr(q, 0x40004, 0x14006);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    wr(q, VT(17), 77 | (uint32_t)(-61 & 4095) << 12);
    wr(q, VT(18), 43 | (uint32_t)(-60 & 4095) << 12);
    wr(q, 0x70000, 31 | 31 << 16);
    wr(q, 0x70008, 0x10203000);
    wr(q, 0x78000, 1u << 30);
    wr(q, 0x70004, 1);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0xabcdef);
    g_assert_cmphex(screen(q, 31, 4, 0, 2), ==, 0x102030);
    qtest_quit(q);
}

static void test_cursor_boundaries(void)
{
    QTestState *q = start("");
    setup(q);
    wr(q, VT(4), 32 | 33 << 12);
    wr(q, VT(15), 32 << 12);
    wr(q, VT(16), 32 << 12);
    wr(q, VT(13), 11 | 78 << 12);
    wr(q, VT(14), 32);
    wr(q, VT(18), 43 | (uint32_t)(-34 & 4095) << 12);
    wr(q, 0x70000, 31 | 31 << 16);
    wr(q, 0x70008, 0x10203000);
    wr(q, 0x7000c, 0x40506000);
    wr(q, 0x70010, 0x70809000);
    wr(q, 0x78000, 1u << 30 | 2u << 28);
    wr(q, 0x78004, 3u << 30);
    wr(q, 0x780fc, 3);
    wr(q, 0x70004, 1);
    frame(q);
    g_assert_cmphex(screen(q, 32, 32, 16, 0), ==, 0x708090);
    g_assert_cmphex(screen(q, 32, 32, 31, 31), ==, 0x708090);
    wr(q, 0x70000, 30 | 31 << 16);
    g_assert_cmphex(screen(q, 32, 32, 0, 0), ==, 0x405060);
    wr(q, 0x70000, 0xffff | 31 << 16); /* Entire glyph is left of picture. */
    g_assert_cmphex(screen(q, 32, 32, 0, 0), ==, 0);
    wr(q, 0x70000, 5 | 6 << 16);
    wr(q, 0x70004, 3);
    g_assert_cmphex(screen(q, 32, 32, 5, 0), ==, 0x102030);
    g_assert_cmphex(screen(q, 32, 32, 0, 6), ==, 0x102030);
    g_assert_cmphex(screen(q, 32, 32, 0, 0), ==, 0);
    qtest_quit(q);
}

static void test_fractional_and_changed_clock(void)
{
    QTestState *q = start("");
    mode(q);
    wr(q, 4, 12 | 2 << 8 | 3u << 14 | 1u << 20);
    wr(q, VT(0), 0);
    qtest_clock_step(q, 1199);
    g_assert_cmphex(rd(q, VT(0)), ==, 12);
    qtest_clock_step(q, 1);
    g_assert_cmphex(rd(q, VT(0)), ==, 13);
    qtest_clock_step(q, 1);
    wr(q, 4, 7 | 3u << 14 | 1u << 20); /* Change to 20 MHz. */
    qtest_clock_step(q, 49);
    g_assert_cmphex(rd(q, VT(0)), ==, 13);
    qtest_clock_step(q, 1);
    g_assert_cmphex(rd(q, VT(0)), ==, 14);
    qtest_quit(q);
}

#define CRIME_BASE 0x14000000u
#define MTE_BASE 0x15003000u
#define MTE_TLB 0x15001700u

static QTestState *start_board(const char *image, bool legacy)
{
    QTestState *q = qtest_initf(
        "-M o2-test,bank-profile=8x128m-synthetic,"
        "crime-profile=%s,reset-low-ram-approx=on,"
        "functional-memory-transfer=on,prom-private-window=on,"
        "prom-sb64-reset=on,gbe11-registers=on,prom-id-word=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=238 -cpu R10000 -m 1G -nodefaults",
        legacy ? "legacy-software" : "1.5", image);

    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(q, CRIME_BASE + 0x208 + bank * 8,
                     0x100 | bank << 2);
    }
    return q;
}

static void assert_cpu_route(QTestState *q, bool legacy, bool active)
{
    for (unsigned ip = 0; ip < 5; ip++) {
        g_assert_cmpint(qtest_get_irq(q, ip), ==,
                        active && ip == (legacy ? 0 : 1));
    }
}

static void test_board_interrupts(void)
{
    g_autofree char *dir = g_dir_make_tmp("gbe-board-events-XXXXXX", NULL);
    g_autofree char *image = g_build_filename(dir, "synthetic.bin", NULL);
    const uint64_t first = 1u << 16, second = 1u << 17;

    /* Padding a test-owned byte does not supply or execute SGI firmware. */
    g_assert_true(g_file_set_contents(image, "\0", 1, NULL));
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *q = start_board(image, legacy);
        qtest_irq_intercept_out_named(q, "/machine/crime", "cpu-ip");
        mode(q);
        wr(q, VT(8), 2 | 1 << 12);
        wr(q, VT(9), 4095 | 4095 << 12);
        wr(q, VT(0), 0);
        qtest_clock_step(q, 19400);
        g_assert_cmphex(qtest_readq(q, CRIME_BASE + 0x28) &
                        (first | second), ==, first | second);
        assert_cpu_route(q, legacy, false);
        qtest_writeq(q, CRIME_BASE + 0x18, first);
        assert_cpu_route(q, legacy, true);
        qtest_writeq(q, CRIME_BASE + 0x18, first | second);
        qtest_writeq(q, CRIME_BASE + 0x28, (uint32_t)~first);
        g_assert_cmphex(qtest_readq(q, CRIME_BASE + 0x28) &
                        (first | second), ==, second);
        assert_cpu_route(q, legacy, true);
        qtest_writeq(q, CRIME_BASE + 0x18, first);
        assert_cpu_route(q, legacy, false);
        qtest_writeq(q, CRIME_BASE + 0x18, second);
        assert_cpu_route(q, legacy, true);
        qtest_writeq(q, CRIME_BASE + 0x28, (uint32_t)~second);
        g_assert_cmphex(qtest_readq(q, CRIME_BASE + 0x28) &
                        (first | second), ==, 0);
        assert_cpu_route(q, legacy, false);
        /* Fresh events in the next frame relatch both causes. */
        frame(q);
        g_assert_cmphex(qtest_readq(q, CRIME_BASE + 0x28) &
                        (first | second), ==, first | second);
        assert_cpu_route(q, legacy, true);
        qtest_writeq(q, CRIME_BASE + 0x28, (uint32_t)~(first | second));
        g_assert_cmphex(qtest_readq(q, CRIME_BASE + 0x28) &
                        (first | second), ==, 0);
        assert_cpu_route(q, legacy, false);
        qtest_quit(q);
    }
    g_unlink(image);
    g_rmdir(dir);
}


static void test_board_dma_reset(void)
{
    g_autofree char *dir = g_dir_make_tmp("gbe-board-reset-XXXXXX", NULL);
    g_autofree char *image = g_build_filename(dir, "synthetic.bin", NULL);
    const uint64_t causes = 3u << 16;
    const uint32_t normal = 0x48020000u, overlay = 0x50030000u;
    const uint32_t replacement = 0x58040000u;
    uint8_t saved[8], actual[8];

    g_assert_true(g_file_set_contents(image, "\0", 1, NULL));
    QTestState *q = start_board(image, false);
    qtest_irq_intercept_out_named(q, "/machine/crime", "cpu-ip");
    mode(q);
    palette(q);
    qtest_writew(q, LIST, normal >> 16);
    qtest_writew(q, LIST + 32, overlay >> 16);
    qtest_writel(q, normal, 0x123456fe);
    qtest_writel(q, normal + 4, 0xabcdefa7);
    qtest_writel(q, replacement, 0x987654ab);
    qtest_writel(q, replacement + 4, 0x334455cd);
    qtest_writel(q, overlay, 0x07000900);
    wr(q, CMAP(4359), 0x89abcd00);
    wr(q, CMAP(4361), 0x76543200);
    wr(q, 0x30000, 2 << 13 | 1 << 5);
    wr(q, 0x30004, 128 << 16);
    wr(q, 0x3000c, LIST | 1);
    wr(q, 0x20000, 1 << 5);
    wr(q, 0x20008, (LIST + 32) | 1);
    wr(q, VT(8), 2 | 1 << 12);
    wr(q, VT(0), 0);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x89abcd);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0xabcdef);
    g_assert_cmphex(qtest_readq(q, CRIME_BASE + 0x28) & causes, ==, causes);
    qtest_writeq(q, CRIME_BASE + 0x18, causes);
    assert_cpu_route(q, false, true);
    qtest_memread(q, normal, saved, sizeof(saved));
    wr(q, 0x30000, 1u << 15 | 2 << 13 | 1 << 5);
    g_assert_cmphex(qtest_readq(q, CRIME_BASE + 0x28) & causes, ==, causes);
    assert_cpu_route(q, false, true);
    g_assert_cmphex(rd(q, 0x30008), ==, LIST | 1);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x89abcd);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0);
    qtest_memread(q, normal, actual, sizeof(actual));
    g_assert_cmpmem(saved, sizeof(saved), actual, sizeof(actual));
    qtest_writew(q, LIST, replacement >> 16);
    qtest_writel(q, normal + 4, 0x55667700);
    wr(q, 0x20000, 1u << 13 | 1 << 5);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
    wr(q, 0x30000, 2 << 13 | 1 << 5);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x987654);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x334455);
    wr(q, 0x20000, 1 << 5);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x987654);
    qtest_writeq(q, CRIME_BASE + 0x28, (uint32_t)~causes);
    g_assert_cmphex(qtest_readq(q, CRIME_BASE + 0x28) & causes, ==, 0);
    assert_cpu_route(q, false, false);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x89abcd);
    g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x334455);
    g_assert_cmphex(qtest_readq(q, CRIME_BASE + 0x28) & causes, ==, causes);
    assert_cpu_route(q, false, true);
    qtest_quit(q);
    g_unlink(image);
    g_rmdir(dir);
}

static void test_board_mte_scanout(void)
{
    static const struct {
        unsigned mode;
        uint32_t input[3], output[3];
    } formats[] = {
        { 2, { 0xa7, 0x1c, 0xe0 }, { 0xb624ff, 0x00ff00, 0xff0000 } },
        { 3, { 0xa5c0, 0x1230, 0x7890 },
             { 0xaa55cc, 0x112233, 0x778899 } },
        { 5, { 0x123456fe, 0x987654ab, 0xabcdef01 },
             { 0x123456, 0x987654, 0xabcdef } },
    };
    g_autofree char *dir = g_dir_make_tmp("gbe-board-mte-XXXXXX", NULL);
    g_autofree char *image = g_build_filename(dir, "synthetic.bin", NULL);
    const uint32_t source = 0x40030000u, destination = 0x48020000u;
    uint8_t input[124], copied[124];

    g_assert_true(g_file_set_contents(image, "\0", 1, NULL));
    QTestState *q = start_board(image, false);
    g_autoptr(QDict) link = qtest_qmp(q,
        "{'execute':'qom-get','arguments':{'path':'/machine/gbe',"
        "'property':'ram'}}");
    g_autoptr(QDict) decoded = qtest_qmp(q,
        "{'execute':'qom-get','arguments':{'path':'/machine/crime',"
        "'property':'sgi-crime.decoded-ram[0]'}}");
    g_assert_cmpstr(qdict_get_str(link, "return"), ==,
                    qdict_get_str(decoded, "return"));
    mode(q);
    palette(q);
    qtest_writew(q, LIST, destination >> 16);
    wr(q, 0x30004, 128 << 16);
    wr(q, 0x3000c, LIST | 1);
    /* Existing linear MTE A/B page translations point into distinct banks. */
    qtest_writeq(q, MTE_TLB,
                 (uint64_t)(0x80000000u | source >> 12) << 32);
    qtest_writeq(q, MTE_TLB + 128,
                 (uint64_t)(0x80000000u | destination >> 12) << 32);
    wr(q, VT(0), 0);
    for (unsigned depth = 0; depth < 3; depth++) {
        unsigned bytes = 1u << depth;
        unsigned positions[] = { 0, 29, 30 };
        memset(input, 0, sizeof(input));
        for (unsigned i = 0; i < 3; i++) {
            uint8_t *p = input + positions[i] * bytes;
            if (bytes == 1) {
                p[0] = formats[depth].input[i];
            } else if (bytes == 2) {
                stw_be_p(p, formats[depth].input[i]);
            } else {
                stl_be_p(p, formats[depth].input[i]);
            }
        }
        qtest_memwrite(q, source, input, 31 * bytes);
        wr(q, 0x30000, depth << 13 | 1 << 5);
        wr(q, WID(0), 3 | formats[depth].mode << 2);
        qtest_writel(q, MTE_BASE + 8, UINT32_MAX);
        qtest_writel(q, MTE_BASE + 0x20, 0);
        qtest_writel(q, MTE_BASE + 0x28, 31 * bytes - 1);
        qtest_writel(q, MTE_BASE + 0x30, 0);
        qtest_writel(q, MTE_BASE + 0x38, 31 * bytes - 1);
        qtest_writel(q, MTE_BASE + 0x800, 1u << 11 | 4u << 5 | 5u << 2);
        qtest_memread(q, destination, copied, 31 * bytes);
        g_assert_cmpmem(copied, 31 * bytes, input, 31 * bytes);
        frame(q);
        for (unsigned i = 0; i < 3; i++) {
            g_assert_cmphex(screen(q, 31, 4, positions[i], 0), ==,
                            formats[depth].output[i]);
        }
        qtest_writel(q, MTE_BASE + 0x18, 0);
        qtest_writel(q, MTE_BASE + 0x800, 5u << 2);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
        g_assert_cmphex(screen(q, 31, 4, 30, 0), ==, 0);
        qtest_memread(q, source, copied, 31 * bytes);
        g_assert_cmpmem(copied, 31 * bytes, input, 31 * bytes);
    }
    /* DMA follows live bank decoding rather than a saved RAM pointer. */
    qtest_writel(q, destination, 0x123456aa);
    qtest_writel(q, 0x50020000, 0xabcdef01);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    qtest_writeq(q, CRIME_BASE + 0x208 + 8, 0x100 | 2 << 2);
    qtest_writeq(q, CRIME_BASE + 0x208 + 16, 0x100 | 1 << 2);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0xabcdef);
    qtest_writeq(q, CRIME_BASE + 0x208 + 8, 0x100 | 1 << 2);
    qtest_writeq(q, CRIME_BASE + 0x208 + 16, 0x100 | 2 << 2);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    /* Reject invalid geometry, holes, no-ECC aliases and low aliases. */
    qtest_writeq(q, CRIME_BASE + 0x208 + 7 * 8, 0x1c);
    const uint16_t invalid_tiles[] = { 0x7802, 0x7a02, 0x8002, 0x0002 };
    for (unsigned i = 0; i < ARRAY_SIZE(invalid_tiles); i++) {
        qtest_writew(q, LIST, invalid_tiles[i]);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
    }
    qtest_writew(q, LIST, destination >> 16);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    qtest_quit(q);
    g_unlink(image);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi-gbe/reset", test_reset);
    qtest_add_func("/sgi-gbe/dma-reset/registers", test_dma_reset_registers);
    qtest_add_func("/sgi-gbe/dma-reset/planes", test_dma_reset_planes);
    qtest_add_func("/sgi-gbe/dma-reset/recovery", test_dma_reset_recovery);
    qtest_add_func("/sgi-gbe/dma-reset/migration", test_dma_reset_migration);
    qtest_add_func("/sgi-gbe/dma-reset/active-controls",
                   test_dma_reset_active_controls);
    qtest_add_func("/sgi-gbe/dma-reset/dma-sentinels",
                   test_dma_reset_dma_sentinels);
    qtest_add_func("/sgi-gbe/dma-reset/did-cursor", test_dma_reset_did_cursor);
    qtest_add_func("/sgi-gbe/clock", test_clock);
    qtest_add_func("/sgi-gbe/rgb8-geometry-cursor", test_rgb8_geometry);
    qtest_add_func("/sgi-gbe/formats", test_formats);
    qtest_add_func("/sgi-gbe/shadow-bounds", test_shadow_and_bounds);
    qtest_add_func("/sgi-gbe/i2c-idle", test_i2c_idle);
    qtest_add_func("/sgi-gbe/multitile", test_multitile);
    qtest_add_func("/sgi-gbe/i2c-slaves", test_i2c_slaves);
    qtest_add_func("/sgi-gbe/migration", test_migration);
    qtest_add_func("/sgi-gbe/did-overlay", test_did_overlay);
    qtest_add_func("/sgi-gbe/i2c-reset-blocker", test_i2c_reset_blocker);
    qtest_add_func("/sgi-gbe/events", test_events);
    qtest_add_func("/sgi-gbe/depth-halves", test_depth_and_halves);
    qtest_add_func("/sgi-gbe/access-unsupported", test_access_and_unsupported);
    qtest_add_func("/sgi-gbe/frame-lock-outputs", test_frame_lock_outputs);
    qtest_add_func("/sgi-gbe/did-overlapping-cache",
                   test_did_overlapping_cache);
    qtest_add_func("/sgi-gbe/clock-pin-phase", test_clock_pin_phase);
    qtest_add_func("/sgi-gbe/descriptor-cache", test_descriptor_cache);
    qtest_add_func("/sgi-gbe/did-boundary-malformed",
                   test_did_boundary_malformed);
    qtest_add_func("/sgi-gbe/wrapped-producer", test_wrapped_producer);
    qtest_add_func("/sgi-gbe/netbsd-vertical-phase",
                   test_netbsd_vertical_phase);
    qtest_add_func("/sgi-gbe/cursor-boundaries", test_cursor_boundaries);
    qtest_add_func("/sgi-gbe/fractional-clock",
                   test_fractional_and_changed_clock);
    qtest_add_func("/sgi-gbe/board-interrupts", test_board_interrupts);
    qtest_add_func("/sgi-gbe/dma-reset/board", test_board_dma_reset);
    qtest_add_func("/sgi-gbe/board-mte-scanout", test_board_mte_scanout);
    return g_test_run();
}
