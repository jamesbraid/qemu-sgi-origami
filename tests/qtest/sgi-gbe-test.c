/* SPDX-License-Identifier: BSD-3-Clause */
/* Synthetic register, clock and framebuffer fixtures, not SGI captures. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "memtx.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include <glib/gstdio.h>

#define BASE 0x16000000u
#define PATH "/machine/peripheral/gbe"
#define VC(n) (0x80000 + (n) * 4)
#define VC_MASK 0x00ffffffu
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

static QTestState *start_view(bool legacy, const char *memory,
                               const char *extra)
{
    QTestState *q = qtest_initf("-M none -cpu R10000 -m %s -display none "
        "-device sgi-o2-gbe,id=gbe,functional-display=on,dma-view=%s,"
        "ram=/objects/ram/ram[0] %s", memory,
        legacy ? "legacy-software" : "crime-1.5", extra);
    qtest_sysbus_mmio_map(q, PATH, 0, BASE);
    return q;
}

static QTestState *start(const char *extra)
{
    return start_view(false, "64M", extra);
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

static void capture_values(QTestState *q, uint32_t horizontal,
                            uint32_t vertical)
{
    g_assert_cmphex(rd(q, VC(0)) & VC_MASK, ==, horizontal);
    g_assert_cmphex(rd(q, VC(1)) & VC_MASK, ==, vertical);
}

static void test_capture_registers(void)
{
    const uint32_t values[] = {
        0, 0x00000fff, 0x00fff000, 0x00ffffff,
        0x00555555, 0x00a5312c, 0x0012ca53, 0x003d7e46,
    };

    for (unsigned functional = 0; functional < 2; functional++) {
        QTestState *q;

        if (functional) {
            q = start("");
        } else {
            q = qtest_init("-M none -cpu R10000 -display none "
                           "-device sgi-o2-gbe,id=gbe");
            qtest_sysbus_mmio_map(q, PATH, 0, BASE);
        }
        capture_values(q, 0, 0);
        for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
            wr(q, VC(0), values[i]);
            wr(q, VC(1), values[ARRAY_SIZE(values) - 1 - i]);
            capture_values(q, values[i], values[ARRAY_SIZE(values) - 1 - i]);
        }
        for (unsigned word = 0; word < 2; word++) {
            wr(q, VC(word ^ 1), 0x00c87654);
            for (unsigned field = 0; field < 2; field++) {
                wr(q, VC(word), 0x987u << (12 * field));
                g_assert_cmphex(rd(q, VC(word)) & VC_MASK, ==,
                                0x987u << (12 * field));
                g_assert_cmphex(rd(q, VC(word ^ 1)) & VC_MASK, ==, 0x00c87654);
            }
            for (unsigned upper = 0; upper < 256; upper += 85) {
                wr(q, VC(word), (upper << 24) | 0x003d7e46);
                g_assert_cmphex(rd(q, VC(word)) & VC_MASK, ==, 0x003d7e46);
                /* Unused-bit zeros are model policy, not silicon data. */
                g_assert_cmphex(rd(q, VC(word)) & ~VC_MASK, ==, 0);
            }
            wr(q, VC(word), UINT32_MAX);
            g_assert_cmphex(rd(q, VC(word)) & VC_MASK, ==, VC_MASK);
        }
        wr(q, VC(0), 0x00a5312c);
        wr(q, VC(1), 0x003d7e46);
        qtest_system_reset(q);
        capture_values(q, 0, 0);
        qtest_quit(q);
    }
}

static void test_capture_access(void)
{
    g_autofree char *dir = g_dir_make_tmp("gbe-capture-access-XXXXXX", NULL);
    g_autofree char *log = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *extra = g_strdup_printf(MEMTX_TEST_DEVICE
                                             " -d guest_errors -D %s", log);
    g_autofree char *text = NULL;
    QTestState *q = start(extra);
    uint64_t value;

    wr(q, VC(0), 0x00a5312c);
    wr(q, VC(1), 0x003d7e46);
    for (unsigned word = 0; word < 2; word++) {
        for (unsigned width = 1; width <= 8; width *= 2) {
            if (width == 4) {
                continue;
            }
            g_assert_cmpint(memtx_store(q, BASE + VC(word), UINT64_MAX, width),
                            !=, MEMTX_OK);
            g_assert_cmpint(memtx_load(q, BASE + VC(word), &value, width),
                            !=, MEMTX_OK);
            capture_values(q, 0x00a5312c, 0x003d7e46);
        }
        for (unsigned delta = 1; delta < 4; delta++) {
            g_assert_cmpint(memtx_store(q, BASE + VC(word) + delta,
                                        UINT32_MAX, 4), !=, MEMTX_OK);
            g_assert_cmpint(memtx_load(q, BASE + VC(word) + delta, &value, 4),
                            !=, MEMTX_OK);
            capture_values(q, 0x00a5312c, 0x003d7e46);
        }
    }
    qtest_quit(q);
    g_assert_true(g_file_get_contents(log, &text, NULL, NULL));
    g_assert_nonnull(strstr(text,
                            "unsupported access write at 0x080000 (8 bytes)"));
    g_assert_nonnull(strstr(text,
                            "unsupported access read at 0x080001 (4 bytes)"));
    g_unlink(log);
    g_rmdir(dir);
}

static void capture_unsupported(QTestState *q)
{
    const unsigned addresses[] = {
        0x80008, 0x8000c, 0x80010, 0x80014, 0x80018,
        0x8001c, 0x80020, 0x800fc, 0x88000,
    };

    for (unsigned i = 0; i < ARRAY_SIZE(addresses); i++) {
        wr(q, addresses[i], UINT32_MAX);
        g_assert_cmphex(rd(q, addresses[i]), ==, UINT32_MAX);
    }
}

static void test_capture_no_effects(void)
{
    g_autofree char *dir = g_dir_make_tmp("gbe-capture-effects-XXXXXX", NULL);
    g_autofree char *log = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *extra = g_strdup_printf("-d guest_errors -D %s", log);
    g_autofree char *text = NULL;
    QTestState *q = start(extra);
    uint8_t before[64], after[64];
    uint32_t counter, active, shadow;

    memset(before, 0x69, sizeof(before));
    qtest_memwrite(q, 0x90000, before, sizeof(before));
    setup(q);
    pixel(q, 0, 0x123456ab);
    wr(q, VT(0), rd(q, VT(0)) | 0x80000000u);
    counter = rd(q, VT(0));
    active = rd(q, 0x30008);
    shadow = rd(q, 0x3000c);
    qtest_irq_intercept_out_named(q, PATH, "sysbus-irq");
    capture_unsupported(q);
    capture_values(q, 0, 0);
    wr(q, VC(0), 0x00a5312c);
    wr(q, VC(1), 0x003d7e46);
    capture_unsupported(q);
    capture_values(q, 0x00a5312c, 0x003d7e46);
    for (unsigned i = 0; i < 3; i++) {
        capture_values(q, 0x00a5312c, 0x003d7e46);
        wr(q, VC(0), 0xffa5312c);
        wr(q, VC(1), 0x003d7e46);
    }
    g_assert_cmphex(rd(q, VT(0)), ==, counter);
    g_assert_cmphex(rd(q, 0x30008), ==, active);
    g_assert_cmphex(rd(q, 0x3000c), ==, shadow);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    qtest_memread(q, 0x90000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpuint(qtest_get_irq_raise_count(q, i), ==, 0);
    }
    qtest_quit(q);
    g_assert_true(g_file_get_contents(log, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "unsupported access write at 0x080008"));
    g_assert_nonnull(strstr(text, "unsupported access write at 0x088000"));
    g_unlink(log);
    g_rmdir(dir);
}

static void test_capture_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("gbe-capture-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");
    uint8_t before[64], after[64];
    uint32_t counter;

    memset(before, 0x96, sizeof(before));
    qtest_memwrite(from, 0x90000, before, sizeof(before));
    setup(from);
    pixel(from, 0, 0x123456ab);
    wr(from, VT(0), rd(from, VT(0)) | 0x80000000u);
    counter = rd(from, VT(0));
    wr(from, VC(0), 0x0012ca53);
    wr(from, VC(1), 0x003d7e46);
    wr(to, VC(0), 0x00ffffff);
    wr(to, VC(1), 0x00555555);
    qtest_irq_intercept_out_named(to, PATH, "sysbus-irq");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    wr(from, VC(0), 0);
    wr(from, VC(1), 0);
    capture_values(to, 0x0012ca53, 0x003d7e46);
    g_assert_cmphex(rd(to, VT(0)), ==, counter);
    g_assert_cmphex(rd(to, 0x30008), ==, LIST | 1);
    g_assert_cmphex(rd(to, 0x3000c), ==, LIST | 1);
    g_assert_cmphex(screen(to, 31, 4, 0, 0), ==, 0x123456);
    qtest_memread(to, 0x90000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    capture_unsupported(to);
    capture_values(to, 0x0012ca53, 0x003d7e46);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpuint(qtest_get_irq_raise_count(to, i), ==, 0);
    }
    qtest_system_reset(to);
    capture_values(to, 0, 0);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_rmdir(dir);
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
        /* No pixel clock shows no picture (approximation). */
        g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==, 0);
        wr(q, 4, 3 | 3u << 14 | 1u << 20);
        g_assert_cmphex(screen(q, 31, 4, plane ? 0 : 1, 0), ==,
                        plane ? 0x123456 : 0);
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
    wr(q, 0x70000, 0);
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

static unsigned timing_reports(const char *path)
{
    g_autofree char *data = NULL;
    const char *p;
    unsigned count = 0;

    g_assert_true(g_file_get_contents(path, &data, NULL, NULL));
    p = data;
    while ((p = strstr(p, "sgi-o2-gbe: unsupported timing interval"))) {
        count++;
        p++;
    }
    return count;
}

static void test_invalid_timing_reports(void)
{
    g_autofree char *dir = g_dir_make_tmp("gbe-timing-reports-XXXXXX", NULL);
    g_autofree char *log = g_build_filename(dir, "timing.txt", NULL);
    g_autofree char *args = g_strdup_printf("-d unimp -D %s", log);
    QTestState *q = start(args);
    unsigned count;

    mode(q);
    count = timing_reports(log);
    wr(q, VT(2), 68 << 12);
    g_assert_cmpuint(timing_reports(log), ==, ++count);
    wr(q, VT(0), 0);
    qtest_clock_step(q, 1100);
    for (unsigned i = 0; i < 20; i++) {
        g_assert_cmphex(rd(q, VT(0)), ==, 11);
    }
    wr(q, VT(2), (68 << 12) | 0xff000000u);
    wr(q, VT(7), 3);
    g_assert_cmpuint(timing_reports(log), ==, count);

    /* Each interval reports independently, and changes rearm only its axis. */
    wr(q, VT(3), 98 << 12);
    g_assert_cmpuint(timing_reports(log), ==, ++count);
    wr(q, VT(2), 69 << 12);
    g_assert_cmpuint(timing_reports(log), ==, ++count);
    wr(q, VT(1), 96 | 67 << 12);
    g_assert_cmpuint(timing_reports(log), ==, ++count);
    wr(q, VT(1), 96 | 66 << 12);
    g_assert_cmpuint(timing_reports(log), ==, ++count);
    wr(q, VT(2), 0);
    g_assert_cmpuint(timing_reports(log), ==, count);
    wr(q, VT(2), 69 << 12);
    g_assert_cmpuint(timing_reports(log), ==, ++count);

    qtest_system_reset(q);
    mode(q);
    count = timing_reports(log);
    wr(q, VT(2), 69 << 12);
    g_assert_cmpuint(timing_reports(log), ==, count + 1);
    qtest_quit(q);
    g_unlink(log);
    g_rmdir(dir);
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

/*
 * The IRIX 6.5.30 kernel programs DOTCLOCK as (M - 1) | (N - 1) << 8 |
 * P << 14 | 1 << 20, first with run bit 20 clear, then set, then waits
 * 11 ms. Counters stop while the PLL is not running and then advance at
 * reference * M / (N * 2^P).
 */
static void test_dotclock_run(void)
{
    QTestState *q = start("");

    mode(q);
    wr(q, 4, 3 | 3u << 14); /* M 4, N 1, P 3: 10 MHz, not running. */
    g_assert_cmphex(rd(q, 4) & (3u << 22), ==, 1u << 23);
    wr(q, VT(0), 0);
    qtest_clock_step(q, 5000);
    g_assert_cmphex(rd(q, VT(0)), ==, 0);
    wr(q, 4, 3 | 3u << 14 | 1u << 20);
    g_assert_cmphex(rd(q, 4) & (3u << 22), ==, 0);
    qtest_clock_step(q, 11000000);
    wr(q, VT(0), 0);
    qtest_clock_step(q, 1100);
    g_assert_cmphex(rd(q, VT(0)), ==, 11);
    qtest_quit(q);
}

/*
 * IRIX 6.5.30 blanks by writing CTRLSTAT 0x000aa000, which selects the
 * external TTL clock that is absent here, and unblanks with 0x300aa000.
 * With no pixel clock the console shows no picture (an approximation; what
 * a monitor shows is not established) and the counters hold. Unblanking
 * restores the same mode without any other GBE write.
 */
static void test_blank_without_clock(void)
{
    QTestState *q = start("");
    uint32_t counter;

    setup(q);
    pixel(q, 0, 0x123456fe);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    wr(q, 0, 0x000aa000);
    counter = rd(q, VT(0));
    frame(q);
    g_assert_cmphex(rd(q, VT(0)), ==, counter);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
    g_assert_cmphex(screen(q, 31, 4, 30, 3), ==, 0);
    wr(q, 0, 0x300aa000);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
    frame(q);
    g_assert_cmphex(rd(q, VT(0)), !=, counter);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
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
    wr(q, 0x70000, 0);
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

/*
 * Both revisions count free entries of the 64-entry FIFO in six bits; when
 * it is empty revision 1 wraps to 0 and revision 2 saturates at 63. Writes
 * drain synchronously, so every read sees that empty value and each write
 * has already reached its CMAP entry. The IRIX 6.5.30 X server waits for at
 * least 48 free there and accepts zero only below revision 2 (Origami's O2
 * GBE datasheet).
 */
static void test_cmfifo_revision(void)
{
    static const struct {
        const char *extra;
        uint32_t revision, count;
    } cases[] = {
        { "", 1, 0 },
        { "-global sgi-o2-gbe.revision=1", 1, 0 },
        { "-global sgi-o2-gbe.revision=2", 2, 63 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        QTestState *q = start(cases[i].extra);

        g_assert_cmphex(rd(q, 0) & 15, ==, cases[i].revision);
        g_assert_cmphex(rd(q, 0x58000), ==, cases[i].count);
        for (unsigned n = 0; n < 80; n++) {
            wr(q, CMAP(n), n << 24);
            g_assert_cmphex(rd(q, 0x58000), ==, cases[i].count);
            g_assert_cmphex(rd(q, CMAP(n)), ==, n << 24);
        }
        /* The revision field is read-only. */
        wr(q, 0, 0x300aa00f);
        g_assert_cmphex(rd(q, 0) & 15, ==, cases[i].revision);
        qtest_system_reset(q);
        g_assert_cmphex(rd(q, 0) & 0x3ffffcf, ==,
                        0x3ffffc0 | cases[i].revision);
        g_assert_cmphex(rd(q, 0x58000), ==, cases[i].count);
        qtest_quit(q);
    }
}

/* Reset alone restores the revision field; nothing reads CTRLSTAT before. */
static void test_revision_reset(void)
{
    for (uint32_t revision = 1; revision <= 2; revision++) {
        g_autofree char *extra = g_strdup_printf(
            "-global sgi-o2-gbe.revision=%u", revision);
        QTestState *q = start(extra);

        wr(q, 0, 0);
        qtest_system_reset(q);
        g_assert_cmphex(rd(q, 0) & 0x3ffffcf, ==, 0x3ffffc0 | revision);
        qtest_quit(q);
    }
}

static void test_revision_rejected(void)
{
    static const unsigned revisions[] = { 0, 3 };

    for (unsigned i = 0; i < ARRAY_SIZE(revisions); i++) {
        g_autofree char *command = g_strdup_printf(
            "timeout 5 %s -nodefaults -display none -accel qtest -M none "
            "-device sgi-o2-gbe,revision=%u", qtest_qemu_binary(NULL),
            revisions[i]);
        g_auto(GStrv) argv = NULL;
        g_autofree char *errors = NULL;
        int status;

        g_assert_true(g_shell_parse_argv(command, NULL, &argv, NULL));
        g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                   NULL, NULL, NULL, &errors, &status, NULL));
        g_assert_true(WIFEXITED(status));
        g_assert_cmpint(WEXITSTATUS(status), !=, 0);
        g_assert_cmpint(WEXITSTATUS(status), !=, 124);
        g_assert_nonnull(errors);
        g_assert_nonnull(strstr(errors, "GBE revision must be 1 or 2"));
    }
}

/* The destination must refuse a stream from the other revision. */
static void test_revision_migration(void)
{
    for (uint32_t revision = 1; revision <= 2; revision++) {
        g_autofree char *dir = g_dir_make_tmp("gbe-rev-migrate-XXXXXX", NULL);
        g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
        g_autofree char *uri = g_strdup_printf("unix:%s", sock);
        g_autofree char *from_extra = g_strdup_printf(
            "-global sgi-o2-gbe.revision=%u", revision);
        g_autofree char *to_extra = g_strdup_printf(
            "-global sgi-o2-gbe.revision=%u -incoming defer", 3 - revision);
        QTestState *from = start(from_extra);
        QTestState *to = start(to_extra);
        int64_t deadline = g_get_monotonic_time() +
                           SGI_MIGRATION_WAIT_MIN_S * G_TIME_SPAN_SECOND;
        g_autofree char *seen = NULL;

        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s,"
            "'exit-on-error':false}}", uri);
        qtest_qmp_assert_success(from,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        for (;;) {
            QDict *ret = qtest_qmp_assert_success_ref(to,
                "{'execute':'query-migrate'}");

            g_free(seen);
            seen = g_strdup(qdict_get_try_str(ret, "status") ?: "");
            qobject_unref(ret);
            if (g_str_equal(seen, "failed") ||
                g_str_equal(seen, "completed")) {
                break;
            }
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
        }
        g_assert_cmpstr(seen, ==, "failed");
        g_assert_cmphex(rd(to, 0) & 15, ==, 3 - revision);
        qtest_quit(from);
        qtest_quit(to);
        g_unlink(sock);
        g_rmdir(dir);
    }
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

static void test_migration_clock(bool ttl)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-gbe-migration-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    QTestState *from = start(ttl ?
        "-global sgi-o2-gbe.external-ttl-hz=23000000" : "");
    QTestState *to = start(ttl ?
        "-global sgi-o2-gbe.external-ttl-hz=23000000 -incoming defer" :
        "-incoming defer");
    setup(from);
    if (ttl) {
        wr(from, 0, 0);
        qtest_clock_step(from, 44);
    }
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
    if (ttl) {
        /* The retained 0.012 dot makes the next dot arrive after 43 ns. */
        qtest_clock_step(from, 42);
        qtest_clock_step(to, 42);
        g_assert_cmphex(rd(to, VT(0)), ==, counter);
        g_assert_cmphex(rd(from, VT(0)), ==, counter);
        qtest_clock_step(from, 1);
        qtest_clock_step(to, 1);
        g_assert_cmphex(rd(to, VT(0)), ==, counter + 1);
        g_assert_cmphex(rd(from, VT(0)), ==, counter + 1);
        qtest_clock_step(from, 282566);
        qtest_clock_step(to, 282566);
    } else {
        frame(from);
        frame(to);
    }
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


static void test_migration(void)
{
    test_migration_clock(false);
}

static void test_ttl_migration(void)
{
    test_migration_clock(true);
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
    qtest_writeb(q, 0x70000, 255);
    wr(q, CMAP(4607), 0xa1b2c300);
    frame(q);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0xa1b2c3);
    wr(q, CMAP(4607), 0x89abcd00);
    g_assert_cmphex(rd(q, 0x58000), ==, 0);
    wr(q, GMAP(0x89), 0x05000000);
    wr(q, GMAP(0xab), 0x00060000);
    wr(q, GMAP(0xcd), 0x00000700);
    g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x050607);
    wr(q, GMAP(0x10), 0xff000000);
    wr(q, GMAP(0x20), 0x00ee0000);
    wr(q, GMAP(0x30), 0x0000dd00);
    wr(q, 0x70000, 0);
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
    g_assert_cmphex(rd(q, VT(19)), ==, 123);
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

static void test_did_prefix(void)
{
    const unsigned x[] = { 16, 31, 32, 63, 64 };
    const uint32_t expected[][5] = {
        { 0xabcdef, 0xabcdef, 0x123456, 0x123456, 0x123456 },
        { 0xabcdef, 0xabcdef, 0x765432, 0x765432, 0x123456 },
    };
    QTestState *q = start("");
    uint8_t table[518] = { 0 };

    mode(q);
    palette(q);
    wr(q, WID(0), 3);
    wr(q, WID(1), 3 | 1 << 5);
    wr(q, WID(2), 3 | 2 << 5);
    wr(q, CMAP(0), 0x12345600);
    wr(q, CMAP(256), 0xabcdef00);
    wr(q, CMAP(512), 0x76543200);
    wr(q, VT(16), 1 << 12);
    stl_be_p(table, 2047 | 1 << 19);
    stw_be_p(table + 512, 31 | 1 << 11);
    wr(q, 0x40004, 0x14006);
    wr(q, VT(0), 0);
    for (unsigned tail = 0; tail < ARRAY_SIZE(expected); tail++) {
        /* Neither tail has a terminator; the first also fails to advance. */
        stw_be_p(table + 514, (tail ? 63 : 31) | 2 << 11);
        qtest_memwrite(q, 0x60000, table, sizeof(table));
        for (unsigned i = 0; i < ARRAY_SIZE(x); i++) {
            wr(q, VT(15), x[i] | (x[i] + 1) << 12);
            frame(q);
            g_assert_cmphex(screen(q, 1, 1, 0, 0), ==, expected[tail][i]);
        }
    }
    qtest_quit(q);
}

static void test_did_prefix_final_halfword(void)
{
    g_autofree char *dir = g_dir_make_tmp("gbe-did-prefix-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "reads.trace", NULL);
    g_autofree char *trace = NULL;
    g_auto(GStrv) lines = NULL;
    QTestState *q = qtest_initf(
        "-M none -cpu R10000 -display none "
        "-device memory-transaction-test,id=memtx,fault-memory=on "
        "-device sgi-o2-gbe,id=gbe,functional-display=on,"
        "ram=/machine/peripheral/memtx/test-memory[0] "
        "-trace enable=memory_region_ops_read,file=%s", path);
    uint8_t entry[4];
    unsigned reads = 0;

    qtest_sysbus_mmio_map(q, PATH, 0, BASE);
    mode(q);
    palette(q);
    wr(q, WID(0), 3);
    wr(q, WID(1), 3 | 1 << 5);
    wr(q, CMAP(0), 0x12345600);
    wr(q, CMAP(256), 0xabcdef00);
    wr(q, VT(15), 16 | 17 << 12);
    wr(q, VT(16), 1 << 12);
    stl_be_p(entry, 2047 | 255 << 11 | 1 << 19);
    qtest_memwrite(q, 0x100000000ULL, entry, 4);
    stw_be_p(entry, 31 | 1 << 11);
    qtest_memwrite(q, 0x1000003feULL, entry, 2);
    /* A reader crossing the block would find a different valid DID. */
    stw_be_p(entry, 2047 | 2 << 11);
    qtest_memwrite(q, 0x100000400ULL, entry, 2);
    wr(q, 0x40004, 0x14000);
    wr(q, VT(0), 0);
    frame(q);
    g_assert_cmphex(screen(q, 1, 1, 0, 0), ==, 0xabcdef);
    qtest_quit(q);
    g_assert_true(g_file_get_contents(path, &trace, NULL, NULL));
    lines = g_strsplit(trace, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        uint64_t address, value;
        unsigned size;
        const char *fields;

        if (!strstr(lines[i], "name 'fault-memory-io'")) {
            continue;
        }
        fields = strstr(lines[i], "addr 0x");
        g_assert_nonnull(fields);
        g_assert_cmpint(sscanf(fields, "addr 0x%" SCNx64 " value 0x%" SCNx64
                              " size %u", &address, &value, &size), ==, 3);
        g_assert_cmpuint(reads, <, 2);
        g_assert_cmphex(address, ==, reads ? 0x1000003feULL : 0x100000000ULL);
        g_assert_cmpuint(size, ==, reads ? 2 : 4);
        reads++;
    }
    g_assert_cmpuint(reads, ==, 2);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_did_boundary_malformed(void)
{
    QTestState *q = start("");
    uint8_t entry[4];
    setup(q);
    pixel(q, 0, 1);
    pixel(q, 11 * 4, 1);
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
    /* A matching prefix survives; an unmatched X still exhausts the block. */
    stw_be_p(entry, 10 | 1 << 11);
    qtest_memwrite(q, 0x6fffe, entry, 2);
    wr(q, VT(15), 1 << 12);
    wr(q, VT(16), 1 << 12);
    frame(q);
    g_assert_cmphex(screen(q, 1, 1, 0, 0), ==, 0xabcdef);
    wr(q, VT(15), 11 | 12 << 12);
    frame(q);
    g_assert_cmphex(screen(q, 1, 1, 0, 0), ==, 0x123456);
    /* The next frame retries an invalid physical DID base without wrapping. */
    wr(q, 0x40004, 0x14400);
    frame(q);
    g_assert_cmphex(screen(q, 1, 1, 0, 0), ==, 0x123456);
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
    wr(q, 0x70000, 0);
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
    wr(q, 0x70000, 0);
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
    wr(q, 0x70000, 0xffff);
    g_assert_cmphex(screen(q, 32, 32, 0, 0), ==, 0x405060);
    wr(q, 0x70000, 0xffe0); /* Entire glyph is left of picture. */
    g_assert_cmphex(screen(q, 32, 32, 0, 0), ==, 0);
    wr(q, 0x70000, 5 | 6 << 16);
    wr(q, 0x70004, 3);
    g_assert_cmphex(screen(q, 32, 32, 5, 0), ==, 0x102030);
    g_assert_cmphex(screen(q, 32, 32, 0, 6), ==, 0x102030);
    g_assert_cmphex(screen(q, 32, 32, 0, 0), ==, 0);
    qtest_quit(q);
}

static const uint32_t projection_colors[] = { 0x123456, 0x789abc, 0xde4f80 };
static const struct {
    unsigned x, y, color;
} projection_markers[] = {
    { 0, 0, 1 }, { 31, 0, 2 }, { 0, 31, 2 }, { 31, 31, 3 },
    { 3, 5, 2 }, { 15, 17, 3 }, { 16, 18, 1 }, { 23, 7, 3 },
};

static void projection_mode(QTestState *q, unsigned w, unsigned h,
                             unsigned xp, unsigned yp)
{
    mode(q);
    wr(q, VT(0), 0x80000000);
    wr(q, VT(1), xp | yp << 12);
    wr(q, VT(4), h << 12);
    wr(q, VT(15), w << 12);
    wr(q, VT(16), h << 12);
    wr(q, VT(13), (w - 21) | (xp - 19) << 12);
    wr(q, VT(14), h);
    wr(q, VT(18), (xp - 54) | (-(yp - h) & 4095) << 12);
    wr(q, WID(0), 3 | 5 << 2);
    /* A cursor must bypass gamma; disabled plane inputs remain black. */
    for (unsigned i = 0; i < 256; i++) {
        wr(q, GMAP(i), 0);
    }
}

static void projection_glyph(QTestState *q)
{
    uint32_t words[64] = { 0 };

    for (unsigned i = 0; i < ARRAY_SIZE(projection_markers); i++) {
        unsigned x = projection_markers[i].x;
        unsigned y = projection_markers[i].y;

        words[y * 2 + x / 16] |= projection_markers[i].color <<
                                (30 - 2 * (x % 16));
    }
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        wr(q, 0x78000 + 4 * i, words[i]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(projection_colors); i++) {
        wr(q, 0x70008 + 4 * i, projection_colors[i] << 8);
    }
}

static uint32_t projection_position(int x, int y)
{
    return (uint16_t)x | (uint32_t)(uint16_t)y << 16;
}

static void projection_check(QTestState *q, unsigned w, unsigned h,
                              int ox, int oy, bool on, bool crosshair)
{
    g_autofree char *dir = g_dir_make_tmp("gbe-projection-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "frame.ppm", NULL);
    g_autofree char *header = g_strdup_printf("P6\n%u %u\n255\n", w, h);
    g_autofree char *data = NULL;
    gsize size;
    unsigned stride = QEMU_ALIGN_UP(w * 3, 4);

    qtest_qmp_assert_success(q,
        "{'execute':'screendump','arguments':{'filename':%s}}", path);
    g_assert_true(g_file_get_contents(path, &data, &size, NULL));
    g_assert_cmpuint(size, ==, strlen(header) + stride * h);
    g_assert_cmpmem(data, strlen(header), header, strlen(header));
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            const uint8_t *pixel = (uint8_t *)data + strlen(header) +
                                   y * stride + x * 3;
            uint32_t actual = pixel[0] << 16 | pixel[1] << 8 | pixel[2];
            uint32_t expected = 0;

            if (on && crosshair && ((int)x == ox || (int)y == oy)) {
                expected = projection_colors[0];
            } else if (on && !crosshair) {
                for (unsigned i = 0; i < ARRAY_SIZE(projection_markers); i++) {
                    if ((int)x == ox + (int)projection_markers[i].x &&
                        (int)y == oy + (int)projection_markers[i].y) {
                        expected = projection_colors[
                            projection_markers[i].color - 1];
                    }
                }
            }
            if (actual != expected) {
                g_test_message("cursor origin (%d,%d), pixel (%u,%u)",
                               ox, oy, x, y);
            }
            g_assert_cmphex(actual, ==, expected);
        }
    }
    g_unlink(path);
    g_rmdir(dir);
}

static void projection_delta(QTestState *q, unsigned xp, unsigned yp,
                              unsigned h, int dx, int dy)
{
    wr(q, VT(18), (xp - 54 - dx) |
        ((uint32_t)(-(int)(yp - h) + dy) & 4095) << 12);
}

static void test_cursor_projection(void)
{
    const struct { unsigned w, h, xp, yp; } modes[] = {
        { 96, 96, 137, 113 }, { 128, 104, 181, 127 },
    };
    const int positions[][2] = {
        { 0, 0 }, { 31, 31 }, { 64, 80 }, { -31, -31 },
        { -5, 7 }, { 11, -3 }, { -31, 11 }, { 8, -31 },
        { -32, 3 }, { 3, -32 }, { 32767, 32767 },
        { -32768, 0 }, { 0, -32768 }, { -32768, -32768 },
    };
    const unsigned hotspots[][2] = { { 0, 0 }, { 3, 5 }, { 31, 31 } };
    const int deltas[][2] = { { -3, 0 }, { 5, 0 }, { 0, -4 }, { 0, 6 } };
    QTestState *q = start("");

    for (unsigned m = 0; m < ARRAY_SIZE(modes); m++) {
        unsigned w = modes[m].w, h = modes[m].h;
        unsigned xp = modes[m].xp, yp = modes[m].yp;

        projection_mode(q, w, h, xp, yp);
        if (m) {
            /* Endpoints equal to the period retain their zero aliases. */
            wr(q, VT(15), xp | w << 12);
            wr(q, VT(16), yp | h << 12);
        }
        projection_glyph(q);
        wr(q, 0x70004, 1);
        for (unsigned i = 0; i < ARRAY_SIZE(positions); i++) {
            uint32_t value = projection_position(positions[i][0],
                                                  positions[i][1]);
            wr(q, 0x70000, value);
            projection_check(q, w, h, positions[i][0], positions[i][1],
                             true, false);
            g_assert_cmphex(rd(q, 0x70000), ==, value);
        }
        wr(q, 0x70000, projection_position(w - 1, h - 1));
        projection_check(q, w, h, w - 1, h - 1, true, false);
        wr(q, 0x70000, projection_position(w, h));
        projection_check(q, w, h, w, h, true, false);
        for (unsigned hot = 0; hot < ARRAY_SIZE(hotspots); hot++) {
            for (unsigned p = 0; p < 3; p++) {
                int px = p == 0 ? 4 : p == 1 ? 48 : w - 2;
                int py = p == 0 ? 6 : p == 1 ? 52 : h - 2;
                int x = px - (int)hotspots[hot][0];
                int y = py - (int)hotspots[hot][1];

                wr(q, 0x70000, projection_position(x, y));
                projection_check(q, w, h, x, y, true, false);
                g_assert_cmphex(screen(q, w, h, px, py), ==,
                               projection_colors[hot]);
            }
        }
        wr(q, 0x70000, projection_position(31, 31));
        for (unsigned d = 0; d < ARRAY_SIZE(deltas); d++) {
            projection_delta(q, xp, yp, h, deltas[d][0], deltas[d][1]);
            projection_check(q, w, h, 31 + deltas[d][0], 31 + deltas[d][1],
                             true, false);
        }
        projection_delta(q, xp, yp, h, 3, -2);
        wr(q, 0x70000, projection_position(7, 9));
        wr(q, 0x70004, 0);
        projection_check(q, w, h, 10, 7, false, false);
        wr(q, 0x70004, 1);
        projection_check(q, w, h, 10, 7, true, false);
        wr(q, 0x70004, 3);
        projection_check(q, w, h, 10, 7, true, true);
        wr(q, 0x70000, projection_position(-5, 9));
        projection_check(q, w, h, -2, 7, true, true);
        wr(q, 0x70004, 2);
        projection_check(q, w, h, -2, 7, false, true);
        projection_delta(q, xp, yp, h, 0, 0);
        wr(q, 0x70000, projection_position(31, 31));
        wr(q, VT(1), 54 | yp << 12);
        wr(q, VT(15), 32 << 12);
        wr(q, 0x70004, 1);
        projection_check(q, 32, h, 0, 0, false, false);
    }
    qtest_quit(q);
}

static void test_cursor_projection_migration(void)
{
    for (unsigned crosshair = 0; crosshair < 2; crosshair++) {
        g_autofree char *dir =
            g_dir_make_tmp("gbe-cursor-restore-XXXXXX", NULL);
        g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
        g_autofree char *uri = g_strdup_printf("unix:%s", sock);
        QTestState *from = start("");
        QTestState *to = start("-incoming defer");
        QTestState *control = start("");
        QTestState *program[] = { from, control };
        const unsigned regs[] = { VT(1), VT(4), VT(15), VT(16), VT(18),
                                  0x70000, 0x70004, 0x70008, 0x7000c,
                                  0x70010 };

        qtest_irq_intercept_out_named(to, PATH, "sysbus-irq");
        for (unsigned i = 0; i < ARRAY_SIZE(program); i++) {
            projection_mode(program[i], 96, 96, 137, 113);
            projection_glyph(program[i]);
            projection_delta(program[i], 137, 113, 96, -3, 11);
            wr(program[i], 0x70000, projection_position(27, -7));
            wr(program[i], 0x70004, crosshair ? 3 : 1);
        }
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
            g_assert_cmphex(rd(to, regs[i]), ==, rd(control, regs[i]));
        }
        for (unsigned i = 0; i < 64; i++) {
            g_assert_cmphex(rd(to, 0x78000 + 4 * i), ==,
                            rd(control, 0x78000 + 4 * i));
        }
        projection_check(to, 96, 96, 24, 4, true, crosshair);
        projection_check(control, 96, 96, 24, 4, true, crosshair);
        for (unsigned i = 0; i < 4; i++) {
            g_assert_cmpuint(qtest_get_irq_raise_count(to, i), ==, 0);
        }
        qtest_system_reset(to);
        g_assert_cmphex(rd(to, 0x70000), ==, 0);
        g_assert_cmphex(rd(to, 0x70004), ==, 0);
        projection_mode(to, 96, 96, 137, 113);
        projection_check(to, 96, 96, 0, 0, false, false);
        for (unsigned i = 0; i < 4; i++) {
            g_assert_cmpuint(qtest_get_irq_raise_count(to, i), ==, 0);
        }
        qtest_quit(from);
        qtest_quit(to);
        qtest_quit(control);
        g_unlink(sock);
        g_rmdir(dir);
    }
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


static void dma_address_setup(QTestState *q, bool legacy)
{
    uint32_t base = legacy ? 0 : 0x40000000u;

    mode(q);
    palette(q);
    qtest_writew(q, 0xd50020, (base + 0xd60000) >> 16);
    qtest_writew(q, 0xd50080, (base + 0xd70000) >> 16);
    qtest_writel(q, 0xd60000, 0x123456a7);
    qtest_writel(q, 0xd60004, 0xabcdefa7);
    qtest_writel(q, 0xd70000, 0x987654a7);
    qtest_writel(q, 0xd70004, 0x334455a7);
    wr(q, 0x30000, 2 << 13 | 1 << 5);
    wr(q, 0x30004, 128 << 16);
    wr(q, 0x3000c, (base + 0xd50020) | 1);
    wr(q, VT(0), 0);
    frame(q);
}

static void test_dma_address_fields(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        g_autofree char *dir = g_dir_make_tmp("gbe-address-log-XXXXXX", NULL);
        g_autofree char *log = g_build_filename(dir, "dma.txt", NULL);
        g_autofree char *args = g_strdup_printf("-d guest_errors -D %s", log);
        QTestState *q = start_view(legacy, "64M", args);
        unsigned faults = 0;
        uint32_t base = legacy ? 0 : 0x40000000u;
        uint32_t first = base + 0xd50020, second = base + 0xd50080;
        uint16_t entry = (base + 0xd60000) >> 16;
        uint8_t table[514] = { 0 };
        const uint32_t invalid_lists[] = {
            first ^ 0x40000000u, 0x80000020, 0xffffffe0,
            legacy ? 0x10000020 : 0x80000020,
        };
        const uint16_t invalid_tiles[] = {
            bswap16(entry), entry ^ 0x4000, 0x8000, 0xffff,
            legacy ? 0x1000 : 0x8000,
        };

        dma_address_setup(q, legacy);
        g_assert_cmphex(rd(q, 0x30008), ==, first | 1);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
        wr(q, 0x3000c, second | 1);
        g_assert_cmphex(rd(q, 0x30008), ==, first | 1);
        frame(q);
        g_assert_cmphex(rd(q, 0x30008), ==, second | 1);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x334455);
        wr(q, 0x3000c, first | 1);
        frame(q);
        for (unsigned i = 0; i < ARRAY_SIZE(invalid_lists); i++) {
            wr(q, 0x3000c, invalid_lists[i] | 1);
            frame(q);
            g_assert_cmphex(rd(q, 0x30008), ==, invalid_lists[i] | 1);
            g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
            g_assert_cmpuint(dma_faults(log), ==, ++faults);
        }
        wr(q, 0x3000c, first | 1);
        for (unsigned i = 0; i < ARRAY_SIZE(invalid_tiles); i++) {
            qtest_writew(q, 0xd50020, invalid_tiles[i]);
            frame(q);
            g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
            g_assert_cmpuint(dma_faults(log), ==, ++faults);
        }
        qtest_writew(q, 0xd50020, 0);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
        g_assert_cmpuint(dma_faults(log), ==, faults);
        qtest_writew(q, 0xd50020, entry);
        qtest_writew(q, 0xd50100, (base + 0xd80000) >> 16);
        qtest_writel(q, 0xd80000, 0x07000900);
        wr(q, CMAP(4359), 0x89abcd00);
        wr(q, CMAP(4361), 0x76543200);
        wr(q, 0x20000, 1 << 5);
        wr(q, 0x20008, (base + 0xd50100) | 1);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x89abcd);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0xabcdef);
        wr(q, 0x3000c, (first ^ 0x40000000u) | 1);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x89abcd);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0);
        wr(q, 0x3000c, first | 1);
        frame(q);
        qtest_writew(q, 0xd50100, ((base ^ 0x40000000u) + 0xd80000) >> 16);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
        qtest_writew(q, 0xd50100, 0);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
        wr(q, 0x20008, ((base ^ 0x40000000u) + 0xd50100) | 1);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
        wr(q, 0x20008, 0);
        stl_be_p(table, 2047 | 1 << 19);
        stw_be_p(table + 512, 2047 | 1 << 11);
        qtest_memwrite(q, 0xd90000, table, sizeof(table));
        wr(q, WID(0), 3);
        wr(q, WID(1), 3 | 1 << 5);
        wr(q, CMAP(0xa7), 0x13579b00);
        wr(q, CMAP(0x1a7), 0x24681300);
        wr(q, 0x40004, (base + 0xd90000) >> 16 | 1 << 16);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x246813);
        const uint32_t invalid_did[] = {
            ((base ^ 0x40000000u) + 0xd90000) >> 16,
            0x8000, 0xffff, legacy ? 0x1000 : 0x8000,
        };
        for (unsigned i = 0; i < ARRAY_SIZE(invalid_did); i++) {
            wr(q, 0x40004, invalid_did[i] | 1 << 16);
            frame(q);
            g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x13579b);
        }
        qtest_system_reset(q);
        g_autoptr(QDict) view = qtest_qmp(q,
            "{'execute':'qom-get','arguments':{'path':%s,"
            "'property':'dma-view'}}", PATH);
        g_assert_cmpstr(qdict_get_str(view, "return"), ==,
                        legacy ? "legacy-software" : "crime-1.5");
        g_autoptr(QDict) immutable = qtest_qmp(q,
            "{'execute':'qom-set','arguments':{'path':%s,"
            "'property':'dma-view','value':%s}}", PATH,
            legacy ? "crime-1.5" : "legacy-software");
        g_assert_true(qdict_haskey(immutable, "error"));
        qtest_quit(q);
        g_unlink(log);
        g_rmdir(dir);
    }
}

static void test_dma_address_boundaries(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *q = start_view(legacy, "1G", "");
        uint32_t base = legacy ? 0 : 0x40000000u;
        uint32_t end = legacy ? 0x10000000u : 0x80000000u;

        mode(q);
        palette(q);
        wr(q, WID(0), 3);
        wr(q, CMAP(0xa7), 0x12345600);
        wr(q, VT(1), 1024 | 200 << 12);
        wr(q, VT(4), 128 | 129 << 12);
        wr(q, VT(15), 511 | 512 << 12);
        wr(q, VT(16), 127 | 128 << 12);
        wr(q, VT(13), 491 | 1005 << 12);
        wr(q, VT(14), 128);
        qtest_writew(q, 0x10020, (end - 65536) >> 16);
        qtest_writeb(q, end - base - 1, 0xa7);
        /* Invalid domains must differ from widened or wrapped RAM reads. */
        qtest_writeb(q, 0xffff, 0xa7);
        qtest_writeb(q, 0xd6ffff, 0xa7);
        qtest_writew(q, 0x20, (base + 0xd60000) >> 16);
        qtest_writew(q, end - base - 32, (base + 0xd60000) >> 16);
        if (legacy) {
            qtest_writeb(q, 0x1000ffff, 0xa7);
            qtest_writew(q, 0x10000020, 0xd6);
        }
        wr(q, 0x30000, 1 << 5);
        wr(q, 0x30004, 128 << 16);
        wr(q, 0x3000c, (base + 0x10020) | 1);
        wr(q, VT(0), 0);
        qtest_clock_step(q, 20500000);
        g_assert_cmphex(screen(q, 1, 1, 0, 0), ==, 0x123456);
        qtest_writew(q, 0x10020, end >> 16);
        qtest_clock_step(q, 20500000);
        g_assert_cmphex(screen(q, 1, 1, 0, 0), ==, 0);
        wr(q, 0x3000c, (end + 32) | 1);
        qtest_clock_step(q, 20500000);
        g_assert_cmphex(screen(q, 1, 1, 0, 0), ==, 0);
        wr(q, 0x3000c, 0x80000021);
        qtest_clock_step(q, 20500000);
        g_assert_cmphex(screen(q, 1, 1, 0, 0), ==, 0);
        wr(q, 0x3000c, 0xffffffe1);
        qtest_clock_step(q, 20500000);
        g_assert_cmphex(screen(q, 1, 1, 0, 0), ==, 0);
        /* Crossing spans also fail the independent 8 KiB list-page bound. */
        wr(q, 0x30000, 17 << 5);
        wr(q, 0x3000c, (end - 32) | 1);
        qtest_clock_step(q, 20500000);
        g_assert_cmphex(screen(q, 1, 1, 0, 0), ==, 0);
        wr(q, 0x3000c, 0xffffffe1);
        qtest_clock_step(q, 20500000);
        g_assert_cmphex(screen(q, 1, 1, 0, 0), ==, 0);
        g_assert_cmphex(qtest_readb(q, end - base - 1), ==, 0xa7);
        qtest_quit(q);
    }
}

static void test_dma_address_migration(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned mismatch = 0; mismatch < 2; mismatch++) {
            g_autofree char *dir = g_dir_make_tmp("gbe-view-migrate-XXXXXX",
                                                 NULL);
            g_autofree char *sock = g_build_filename(dir, "migration.sock",
                                                    NULL);
            g_autofree char *uri = g_strdup_printf("unix:%s", sock);
            QTestState *from = start_view(legacy, "64M", "");
            QTestState *to = start_view(legacy ^ mismatch, "64M",
                                       "-incoming defer");
            uint32_t base = legacy ? 0 : 0x40000000u;
            QTestState *control = mismatch ? NULL :
                                  start_view(legacy, "64M", "");

            if (control) {
                dma_address_setup(control, legacy);
                qtest_writew(control, 0xd50020, (base + 0xd70000) >> 16);
                qtest_irq_intercept_out_named(control, PATH, "sysbus-irq");
            }
            dma_address_setup(from, legacy);
            qtest_writew(from, 0xd50020, (base + 0xd70000) >> 16);
            qtest_irq_intercept_out_named(to, PATH, "sysbus-irq");
            qtest_qmp_assert_success(to,
                "{'execute':'migrate-incoming','arguments':{'uri':%s,"
                "'exit-on-error':false}}", uri);
            qtest_qmp_assert_success(from,
                "{'execute':'migrate','arguments':{'uri':%s}}", uri);
            sgi_wait_migration(to, mismatch ? "failed" : "completed");
            if (mismatch) {
                g_assert_cmphex(rd(to, 0x30008), ==, 0);
                g_assert_cmphex(rd(to, 0x3000c), ==, 0);
                g_assert_cmphex(rd(to, VT(0)), ==, 0x80000000u);
            } else {
                g_assert_cmphex(rd(to, 0x30008), ==, (base + 0xd50020) | 1);
                g_assert_cmphex(screen(to, 31, 4, 0, 0), ==, 0x123456);
                g_assert_cmpuint(qtest_get_irq_raise_count(to, 0), ==, 0);
                wr(to, WID(0), 3 | 5 << 2);
                g_assert_cmphex(screen(to, 31, 4, 0, 0), ==, 0x123456);
                frame(to);
                frame(control);
                g_assert_cmphex(rd(to, VT(0)), ==, rd(control, VT(0)));
                g_assert_cmphex(screen(to, 31, 4, 0, 0), ==, 0x987654);
                g_assert_cmphex(screen(to, 31, 4, 0, 0), ==,
                                screen(control, 31, 4, 0, 0));
                g_assert_cmpuint(qtest_get_irq_raise_count(to, 0), ==,
                                 qtest_get_irq_raise_count(control, 0));
                qtest_quit(control);
            }
            qtest_quit(from);
            qtest_quit(to);
            g_unlink(sock);
            g_rmdir(dir);
        }
    }
}

#define CRIME_BASE 0x14000000u
#define MTE_BASE 0x15003000u
#define MTE_TLB 0x15001700u

static QTestState *start_board(const char *image, bool legacy)
{
    QTestState *q = qtest_initf(
        "-M o2,register-test-profile=on,bank-profile=8x128m-synthetic,"
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


static void test_dma_address_board_domains(void)
{
    g_autofree char *dir = g_dir_make_tmp("gbe-address-domain-XXXXXX", NULL);
    g_autofree char *image = g_build_filename(dir, "synthetic.bin", NULL);

    g_assert_true(g_file_set_contents(image, "\0", 1, NULL));
    for (int legacy = 1; legacy >= 0; legacy--) {
        QTestState *q = start_board(image, legacy);
        uint32_t base = legacy ? 0 : 0x40000000u;
        uint32_t list = base + 0xd50020;
        uint32_t tile = base + 0xd60000;

        mode(q);
        palette(q);
        qtest_writew(q, 0x40d50020, tile >> 16);
        qtest_writel(q, 0x40d60000, 0x123456ab);
        qtest_writel(q, 0x40d60004, 0xabcdef01);
        wr(q, 0x30000, 2 << 13 | 1 << 5);
        wr(q, 0x30004, 128 << 16);
        wr(q, 0x3000c, list | 1);
        wr(q, VT(0), 0);
        frame(q);
        g_assert_cmphex(rd(q, 0x30008), ==, list | 1);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0xabcdef);
        /* The opposite raw domain must not become a second RAM alias. */
        wr(q, 0x3000c, (list ^ 0x40000000u) | 1);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0);
        wr(q, 0x3000c, list | 1);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x123456);
        qtest_quit(q);
    }
    g_unlink(image);
    g_rmdir(dir);
}


static void dma_board_copy(QTestState *q, bool legacy, uint32_t source,
                           uint32_t destination, unsigned sx, unsigned dx)
{
    uint32_t mode = 1u << 11 | (legacy ? 1 << 2 : 4 << 5 | 5 << 2);

    if (legacy) {
        qtest_writeq(q, 0x15001000,
                     (uint64_t)(0x8000 | source >> 16) << 48);
        qtest_writeq(q, 0x15001200,
                     (uint64_t)(0x8000 | destination >> 16) << 48);
    } else {
        qtest_writeq(q, MTE_TLB,
                     (uint64_t)(0x80000000u | source >> 12) << 32);
        qtest_writeq(q, MTE_TLB + 128,
                     (uint64_t)(0x80000000u | destination >> 12) << 32);
    }
    qtest_writel(q, MTE_BASE + 8, UINT32_MAX);
    qtest_writel(q, MTE_BASE + 0x20, legacy ? sx << 16 : sx);
    qtest_writel(q, MTE_BASE + 0x28, legacy ? (sx + 7) << 16 : sx + 7);
    qtest_writel(q, MTE_BASE + 0x30, legacy ? dx << 16 : dx);
    qtest_writel(q, MTE_BASE + 0x38, legacy ? (dx + 7) << 16 : dx + 7);
    qtest_writel(q, MTE_BASE + 0x40, 1);
    qtest_writel(q, MTE_BASE + 0x48, 1);
    qtest_writel(q, MTE_BASE + 0x800, mode);
}

static void test_dma_address_mte_banks(void)
{
    g_autofree char *dir = g_dir_make_tmp("gbe-dma-banks-XXXXXX", NULL);
    g_autofree char *image = g_build_filename(dir, "synthetic.bin", NULL);
    const uint8_t source[] = {
        0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17,
        0xa7, 0xa7, 0xa7, 0xa7, 0xa7, 0xa7, 0xa7, 0xa7,
        0x5b, 0x5b, 0x5b, 0x5b, 0x5b, 0x5b, 0x5b, 0x5b,
    };

    g_assert_true(g_file_set_contents(image, "\0", 1, NULL));
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *q = start_board(image, legacy);
        uint32_t base = legacy ? 0 : 0x40000000u;
        uint8_t actual[24];

        mode(q);
        palette(q);
        wr(q, WID(0), 3);
        wr(q, CMAP(0), 0);
        wr(q, CMAP(0x17), 0x12345600);
        wr(q, CMAP(0xa7), 0xabcdef00);
        wr(q, CMAP(0x5b), 0x76543200);
        wr(q, CMAP(4359), 0x89abcd00);
        qtest_memwrite(q, 0x40030000, source, sizeof(source));
        qtest_writel(q, 0x40040000, 0x07000000);
        qtest_writew(q, LIST + 32, (base + 0x08020000) >> 16);
        qtest_writew(q, LIST + 128, (base + 0x40000) >> 16);
        wr(q, 0x30000, 1 << 5);
        wr(q, 0x30004, 128 << 16);
        wr(q, 0x3000c, (base + 0x10020) | 1);
        wr(q, 0x20000, 1 << 5);
        wr(q, 0x20008, (base + 0x10080) | 1);
        /* Only admitted MTE operations populate either destination tile. */
        dma_board_copy(q, legacy, 0x40030000, 0x48020000, 0, 0);
        dma_board_copy(q, legacy, 0x40030000, 0x48020000, 8, 16);
        dma_board_copy(q, legacy, 0x40030000, 0x50020000, 8, 0);
        dma_board_copy(q, legacy, 0x40030000, 0x50020000, 0, 16);
        /* A wrong fallback into bank zero must produce a nonzero pixel. */
        dma_board_copy(q, legacy, 0x40030000, 0x40020000, 16, 0);
        dma_board_copy(q, legacy, 0x40030000, 0x40020000, 16, 16);
        qtest_memread(q, 0x40020000, actual, sizeof(actual));
        g_assert_cmpmem(actual, 8, source + 16, 8);
        g_assert_cmpmem(actual + 16, 8, source + 16, 8);
        qtest_memread(q, 0x48020000, actual, sizeof(actual));
        g_assert_cmpmem(actual, 8, source, 8);
        g_assert_cmpmem(actual + 16, 8, source + 8, 8);
        wr(q, VT(0), 0);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x89abcd);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x123456);
        g_assert_cmphex(screen(q, 31, 4, 7, 0), ==, 0x123456);
        g_assert_cmphex(screen(q, 31, 4, 8, 0), ==, 0);
        g_assert_cmphex(screen(q, 31, 4, 16, 0), ==, 0xabcdef);
        g_assert_cmphex(screen(q, 31, 4, 23, 0), ==, 0xabcdef);
        /* Overlap keeps the lower numbered bank, then remapping selects 2. */
        qtest_writeq(q, CRIME_BASE + 0x208 + 2 * 8, 0x104);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x123456);
        qtest_writeq(q, CRIME_BASE + 0x208 + 1 * 8, 0x108);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0xabcdef);
        g_assert_cmphex(screen(q, 31, 4, 16, 0), ==, 0x123456);
        qtest_writeq(q, CRIME_BASE + 0x208 + 2 * 8, 0x108);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x89abcd);
        qtest_writeq(q, CRIME_BASE + 0x208 + 1 * 8, 4);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0);
        g_assert_cmphex(screen(q, 31, 4, 0, 0), ==, 0x89abcd);
        qtest_writeq(q, CRIME_BASE + 0x208 + 1 * 8, 0x104);
        qtest_writeq(q, CRIME_BASE + 0x208 + 2 * 8, 0x108);
        frame(q);
        g_assert_cmphex(screen(q, 31, 4, 1, 0), ==, 0x123456);
        g_assert_cmphex(screen(q, 31, 4, 16, 0), ==, 0xabcdef);
        qtest_memread(q, 0x40030000, actual, sizeof(source));
        g_assert_cmpmem(actual, sizeof(source), source, sizeof(source));
        qtest_quit(q);
    }
    g_unlink(image);
    g_rmdir(dir);
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

static const struct {
    unsigned first, count;
    uint32_t mask;
} indexed_tables[] = {
    { 0x48000, 32, 0x1fff }, { 0x50000, 4608, 0xffffff00 },
    { 0x60000, 256, 0xffffff00 }, { 0x70008, 3, 0xffffff00 },
    { 0x78000, 64, UINT32_MAX },
};

static uint32_t table_pattern(unsigned address)
{
    return 0x795b3d1f ^ (address * 0x1020305u);
}

static void indexed_values(QTestState *q, bool write, bool zero)
{
    for (unsigned t = 0; t < ARRAY_SIZE(indexed_tables); t++) {
        for (unsigned i = 0; i < indexed_tables[t].count; i++) {
            unsigned address = indexed_tables[t].first + 4 * i;
            uint32_t value = zero ? 0 : table_pattern(address);

            if (write) {
                wr(q, address, value);
            } else {
                g_assert_cmphex(rd(q, address) & indexed_tables[t].mask, ==,
                                value & indexed_tables[t].mask);
            }
        }
    }
}

static void test_indexed_coverage(void)
{
    QTestState *q = start(MEMTX_TEST_DEVICE);
    const unsigned holes[] = { 0x48080, 0x54800, 0x60400, 0x70014,
                               0x78100, 0x80008, 0x90000, 0x10050 };

    qtest_irq_intercept_out_named(q, PATH, "sysbus-irq");
    indexed_values(q, true, false);
    indexed_values(q, false, false);
    for (unsigned t = 0; t < ARRAY_SIZE(indexed_tables); t++) {
        unsigned address = indexed_tables[t].first +
                           4 * (indexed_tables[t].count - 1);
        uint8_t bytes[4];
        uint64_t value;

        qtest_memread(q, BASE + address, bytes, sizeof(bytes));
        g_assert_cmphex((uint32_t)ldl_be_p(bytes), ==,
                        table_pattern(address) & indexed_tables[t].mask);
        for (unsigned width = 1; width <= 8; width *= 2) {
            if (width == 4) {
                continue;
            }
            g_assert_cmpint(memtx_store(q, BASE + address, 0, width),
                            !=, MEMTX_OK);
            g_assert_cmpint(memtx_load(q, BASE + address, &value, width),
                            !=, MEMTX_OK);
        }
        for (unsigned delta = 1; delta < 4; delta++) {
            g_assert_cmpint(memtx_store(q, BASE + address + delta, 0, 4),
                            !=, MEMTX_OK);
            g_assert_cmpint(memtx_load(q, BASE + address + delta, &value, 4),
                            !=, MEMTX_OK);
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(holes); i++) {
        wr(q, holes[i], 0x12345678);
        g_assert_cmphex(rd(q, holes[i]), ==, UINT32_MAX);
    }
    indexed_values(q, false, false);
    g_assert_cmphex(rd(q, 0x58000), ==, 0);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpuint(qtest_get_irq_raise_count(q, i), ==, 0);
    }
    qtest_system_reset(q);
    indexed_values(q, false, true);
    qtest_quit(q);
}

static void test_scalar_coverage(void)
{
    const struct {
        unsigned address;
        uint32_t mask;
    } controls[] = {
        { 0x20000, 0x1fff }, { 0x20004, 0xffffffe1 },
        { 0x20008, 0xffffffe1 }, { 0x30000, 0x7fff },
        { 0x30004, 0xffff0000 }, { 0x30008, 0xffffffe1 },
        { 0x3000c, 0xffffffe1 }, { 0x40000, 0x1ffff },
        { 0x40004, 0x1ffff }, { 0x70000, UINT32_MAX }, { 0x70004, 3 },
    };
    QTestState *q = start(MEMTX_TEST_DEVICE);

    for (unsigned i = 0; i < ARRAY_SIZE(controls); i++) {
        unsigned address = controls[i].address;
        uint32_t raw = table_pattern(address);
        uint64_t value;

        /* FIFO reset has its own tests and is not ordinary storage. */
        if (address == 0x20000) {
            raw &= ~(1U << 13);
        } else if (address == 0x30000) {
            raw &= ~(1U << 15);
        }
        uint32_t pattern = raw & controls[i].mask;

        wr(q, address, raw);
        g_assert_cmphex(rd(q, address), ==, pattern);
        for (unsigned width = 1; width <= 8; width *= 2) {
            if (width != 4) {
                g_assert_cmpint(memtx_store(q, BASE + address, 0, width),
                                !=, MEMTX_OK);
                g_assert_cmpint(memtx_load(q, BASE + address, &value, width),
                                !=, MEMTX_OK);
            }
        }
        for (unsigned delta = 1; delta < 4; delta++) {
            g_assert_cmpint(memtx_store(q, BASE + address + delta, 0, 4),
                            !=, MEMTX_OK);
        }
        g_assert_cmphex(rd(q, address), ==, pattern);
    }
    qtest_system_reset(q);
    for (unsigned i = 0; i < ARRAY_SIZE(controls); i++) {
        g_assert_cmphex(rd(q, controls[i].address), ==, 0);
    }
    qtest_quit(q);
}

static void test_timing_coverage(void)
{
    QTestState *q = start(MEMTX_TEST_DEVICE);

    for (unsigned i = 0; i < 20; i++) {
        uint32_t mask = i == 0 ? 0x80ffffff : i == 6 ? 127 : 0xffffff;
        uint32_t pattern = table_pattern(VT(i)) | 0x80000000;
        uint64_t value;

        wr(q, VT(i), pattern);
        g_assert_cmphex(rd(q, VT(i)), ==, pattern & mask);
        for (unsigned width = 1; width <= 8; width *= 2) {
            if (width == 4) {
                continue;
            }
            g_assert_cmpint(memtx_store(q, BASE + VT(i), 0, width),
                            !=, MEMTX_OK);
            g_assert_cmpint(memtx_load(q, BASE + VT(i), &value, width),
                            !=, MEMTX_OK);
        }
        for (unsigned delta = 1; delta < 4; delta++) {
            g_assert_cmpint(memtx_store(q, BASE + VT(i) + delta, 0, 4),
                            !=, MEMTX_OK);
            g_assert_cmpint(memtx_load(q, BASE + VT(i) + delta, &value, 4),
                            !=, MEMTX_OK);
        }
        g_assert_cmphex(rd(q, VT(i)), ==, pattern & mask);
    }
    qtest_quit(q);
}

static void test_indexed_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("gbe-indexed-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");

    indexed_values(from, true, false);
    for (unsigned i = 0; i < 20; i++) {
        wr(from, VT(i), table_pattern(VT(i)) | 0x80000000);
    }
    wr(from, 0x30008, 0x12345661);
    wr(from, 0x3000c, 0x87654321);
    qtest_irq_intercept_out_named(to, PATH, "sysbus-irq");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    indexed_values(to, false, false);
    for (unsigned i = 0; i < 20; i++) {
        g_assert_cmphex(rd(to, VT(i)), ==, rd(from, VT(i)));
    }
    g_assert_cmphex(rd(to, 0x30008), ==, 0x12345661);
    g_assert_cmphex(rd(to, 0x3000c), ==, 0x87654321);
    g_assert_cmphex(rd(to, 0x58000), ==, 0);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpuint(qtest_get_irq_raise_count(to, i), ==, 0);
    }
    qtest_system_reset(to);
    indexed_values(to, false, true);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi-gbe/coverage/indexed", test_indexed_coverage);
    qtest_add_func("/sgi-gbe/coverage/timing", test_timing_coverage);
    qtest_add_func("/sgi-gbe/coverage/scalars", test_scalar_coverage);
    qtest_add_func("/sgi-gbe/coverage/migration", test_indexed_migration);
    qtest_add_func("/sgi-gbe/capture/registers", test_capture_registers);
    qtest_add_func("/sgi-gbe/capture/access", test_capture_access);
    qtest_add_func("/sgi-gbe/capture/no-effects", test_capture_no_effects);
    qtest_add_func("/sgi-gbe/capture/migration", test_capture_migration);
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
    qtest_add_func("/sgi-gbe/dotclock-run", test_dotclock_run);
    qtest_add_func("/sgi-gbe/blank-without-clock", test_blank_without_clock);
    qtest_add_func("/sgi-gbe/invalid-timing-reports",
                   test_invalid_timing_reports);
    qtest_add_func("/sgi-gbe/rgb8-geometry-cursor", test_rgb8_geometry);
    qtest_add_func("/sgi-gbe/formats", test_formats);
    qtest_add_func("/sgi-gbe/shadow-bounds", test_shadow_and_bounds);
    qtest_add_func("/sgi-gbe/cmfifo-revision", test_cmfifo_revision);
    qtest_add_func("/sgi-gbe/revision-reset", test_revision_reset);
    qtest_add_func("/sgi-gbe/revision-rejected", test_revision_rejected);
    qtest_add_func("/sgi-gbe/revision-migration", test_revision_migration);
    qtest_add_func("/sgi-gbe/i2c-idle", test_i2c_idle);
    qtest_add_func("/sgi-gbe/multitile", test_multitile);
    qtest_add_func("/sgi-gbe/i2c-slaves", test_i2c_slaves);
    qtest_add_func("/sgi-gbe/migration", test_migration);
    qtest_add_func("/sgi-gbe/ttl-migration", test_ttl_migration);
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
    qtest_add_func("/sgi-gbe/did-prefix", test_did_prefix);
    qtest_add_func("/sgi-gbe/did-prefix-final-halfword",
                   test_did_prefix_final_halfword);
    qtest_add_func("/sgi-gbe/did-boundary-malformed",
                   test_did_boundary_malformed);
    qtest_add_func("/sgi-gbe/wrapped-producer", test_wrapped_producer);
    qtest_add_func("/sgi-gbe/netbsd-vertical-phase",
                   test_netbsd_vertical_phase);
    qtest_add_func("/sgi-gbe/cursor-boundaries", test_cursor_boundaries);
    qtest_add_func("/sgi-gbe/cursor-projection", test_cursor_projection);
    qtest_add_func("/sgi-gbe/cursor-projection-migration",
                   test_cursor_projection_migration);
    qtest_add_func("/sgi-gbe/fractional-clock",
                   test_fractional_and_changed_clock);
    qtest_add_func("/sgi-gbe/dma-address/fields", test_dma_address_fields);
    qtest_add_func("/sgi-gbe/dma-address/boundaries",
                   test_dma_address_boundaries);
    qtest_add_func("/sgi-gbe/dma-address/migration",
                   test_dma_address_migration);
    qtest_add_func("/sgi-gbe/dma-address/mte-banks",
                   test_dma_address_mte_banks);
    qtest_add_func("/sgi-gbe/dma-address/board-domains",
                   test_dma_address_board_domains);
    qtest_add_func("/sgi-gbe/board-interrupts", test_board_interrupts);
    qtest_add_func("/sgi-gbe/dma-reset/board", test_board_dma_reset);
    qtest_add_func("/sgi-gbe/board-mte-scanout", test_board_mte_scanout);
    return g_test_run();
}
