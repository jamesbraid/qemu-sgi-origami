/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#include "qemu/osdep.h"
#include <math.h>
#ifdef CONFIG_LINUX
#include <sys/resource.h>
#endif
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "memtx.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include "sgi-crime-line-vectors.h"
#include "sgi-crime-texture-addresses.h"

#define CRIME 0x14000000ULL
#define RE 0x15000000ULL
#define DE (RE + 0x2000)
#define MTE (RE + 0x3000)
#define STATUS (RE + 0x4000)

static QTestState *start_profile_ram(const char *profile, bool banked,
                                     bool functional, bool migrate_ram,
                                     const char *extra)
{
    QTestState *s = qtest_initf("-device memory-transaction-test,id=memtx%s "
        "-M none -cpu R10000 -S -d guest_errors,unimp "
        "-object memory-backend-ram,id=fit,size=%s "
        "-device sgi-crime,id=crime,ram=/objects/fit/fit[0],"
        "crime-profile=%s,functional-memory-transfer=%s%s %s",
        migrate_ram ? ",migrate-ram=/objects/fit/fit[0]" : "",
        banked ? "1G" : "64M", profile, functional ? "on" : "off",
        banked ? ",eight-128m-banks=on,reset-low-ram-approx=on" : "", extra);

    qtest_sysbus_mmio_map(s, "/machine/peripheral/crime", 0, CRIME);
    return s;
}

static QTestState *start_profile(const char *profile, bool banked,
                                 bool functional, const char *extra)
{
    return start_profile_ram(profile, banked, functional, true, extra);
}

static QTestState *start_indexed_state(void)
{
    return start_profile_ram("legacy-software", false, true, false, "");
}

static QTestState *start_options(const char *extra)
{
    return start_profile("legacy-software", false, true, extra);
}

static QTestState *start(void)
{
    return start_options("");
}

static uint32_t xy(unsigned x, unsigned y)
{
    return (x << 16) | y;
}

static void tile(QTestState *s, unsigned bank, unsigned index, unsigned page)
{
    uint64_t address = RE + 0x1000 + bank * 0x200 + (index / 4) * 8;
    unsigned shift = 48 - (index % 4) * 16;
    uint64_t value = qtest_readq(s, address);

    value &= ~(0xffffULL << shift);
    value |= (uint64_t)(0x8000 | page) << shift;
    qtest_writeq(s, address, value);
}

static void setup(QTestState *s, unsigned mode, bool copy)
{
    qtest_writel(s, DE, mode);
    qtest_writel(s, DE + 8, mode);
    qtest_writel(s, DE + 0x10, 0);
    qtest_writel(s, DE + 0x18, copy ? 0x2002f8 : 0xf8);
    qtest_writel(s, DE + 0x50, 0);
    qtest_writel(s, DE + 0x58, 0);
    qtest_writel(s, DE + 0x60, 0x03020000);
    qtest_writel(s, DE + 0xa8, 1);
    qtest_writel(s, DE + 0xac, 1);
    qtest_writel(s, DE + 0x1b0, 3);
    qtest_writel(s, DE + 0x1b8, UINT32_MAX);
}

static void test_fill(void)
{
    QTestState *s = start();
    uint8_t bytes[12];

    tile(s, 0, 0, 3);
    qtest_memset(s, 0x30000, 0x5c, sizeof(bytes));
    setup(s, 0x228, false);
    qtest_writel(s, DE + 0xd0, 0x1234abcd);
    qtest_writel(s, DE + 0x70, xy(1, 0));
    qtest_writel(s, DE + 0x74, xy(1, 0));
    qtest_memread(s, 0x30000, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0x5c);
    }
    qtest_writel(s, DE + 0x874, xy(1, 0));
    qtest_memread(s, 0x30000, bytes, sizeof(bytes));
    g_assert_cmphex(ldl_be_p(bytes), ==, 0x5c5c5c5c);
    g_assert_cmphex(ldl_be_p(bytes + 4), ==, 0x1234abcd);
    g_assert_cmphex(ldl_be_p(bytes + 8), ==, 0x5c5c5c5c);
    g_assert_cmphex(qtest_readl(s, STATUS) & ~0x3ffffU, ==, 0x1e000000);
    qtest_quit(s);
}

static void test_indexed_word_regression(void)
{
    QTestState *s = start();

    tile(s, 0, 0, 3);
    setup(s, 0x200, false);
    qtest_writel(s, DE + 0x18, 0x208);
    qtest_writel(s, DE + 0x1b0, 6);
    qtest_writel(s, DE + 0x1b8, 0);
    qtest_writel(s, DE + 0xd0, 0x12345616);
    qtest_writel(s, DE + 0x70, xy(0, 0));
    qtest_writel(s, DE + 0x74, xy(0, 0));
    qtest_writel(s, 0x30000, 0xa1b2c35a);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, 0x30000), ==, 0xa1b2c34c);
    qtest_writel(s, 0x30000, 0xa1b2c35a);
    qtest_writel(s, DE + 0x18, 0x288);
    qtest_writel(s, DE + 0x1b8, 0x0f);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, 0x30000), ==, 0xa1b2c35c);
    qtest_quit(s);
}

/* Sparse published map, independent of the device's register decoder. */
typedef struct Register {
    unsigned offset;
    unsigned width;
    bool readable;
} Register;

static const Register registers[] = {
    { 0x000, 4, true }, { 0x008, 4, true },
    { 0x010, 4, true }, { 0x018, 4, true },
    { 0x020, 8, true }, { 0x028, 8, true },
    { 0x030, 8, true }, { 0x038, 8, true },
    { 0x040, 8, true }, { 0x048, 8, true },
    { 0x050, 4, true }, { 0x058, 4, true },
    { 0x060, 4, false }, { 0x070, 4, false },
    { 0x074, 4, false }, { 0x078, 4, false },
    { 0x0a0, 4, false }, { 0x0a8, 4, false },
    { 0x0ac, 4, false }, { 0x0d0, 4, false },
    { 0x0c0, 4, true }, { 0x0c4, 4, true },
    { 0x0d8, 4, true }, { 0x1b0, 4, true },
    { 0x1b8, 4, true },
};
static const unsigned modes[] = { 0, 0x124, 0x228 };
static const unsigned wide_slots[] = {
    0x060, 0x078, 0x0d0, 0x000, 0x008, 0x010,
    0x018, 0x050, 0x058, 0x0d8, 0x1b0, 0x1b8,
};

static bool wide_state_write(unsigned offset)
{
    for (unsigned i = 0; i < ARRAY_SIZE(wide_slots); i++) {
        if (wide_slots[i] == offset) {
            return true;
        }
    }
    return offset == 0x70 || offset == 0xa8 || offset == 0xc0;
}


static void assert_stopped(QTestState *s)
{
    g_autoptr(QDict) state =
        qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");

    g_assert_cmpstr(qdict_get_str(state, "status"), ==, "internal-error");
}

static void assert_idle(QTestState *s)
{
    g_assert_cmphex(qtest_readl(s, STATUS) & ~0x3ffffU, ==, 0x1e000000);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x10), ==, 0);
}

static uint64_t bounds(unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    return ((uint64_t)x0 << 48) | ((uint64_t)y0 << 32) |
           ((uint64_t)x1 << 16) | y1;
}

static unsigned page_for(unsigned bank, unsigned index)
{
    return 3 + bank * 256 + index;
}

static uint64_t pixel_address(unsigned bank, unsigned x, unsigned y,
                              unsigned bytes)
{
    unsigned bx = x * bytes;
    unsigned index = (y / 128) * 16 + bx / 512;

    return ((uint64_t)page_for(bank, index) << 16) +
           (y % 128) * 512 + bx % 512;
}

static void map_region(QTestState *s, unsigned bank, unsigned bytes,
                       unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    for (unsigned y = y0 / 128; y <= y1 / 128; y++) {
        for (unsigned x = x0 * bytes / 512; x <= x1 * bytes / 512; x++) {
            unsigned index = y * 16 + x;

            tile(s, bank, index, page_for(bank, index));
        }
    }
}

static void pixel_write(QTestState *s, uint64_t address, unsigned bytes,
                        uint32_t value)
{
    uint8_t data[4];

    stl_be_p(data, value);
    qtest_memwrite(s, address, data + 4 - bytes, bytes);
}

static uint32_t pixel_read(QTestState *s, uint64_t address, unsigned bytes)
{
    uint8_t data[4] = { 0 };

    qtest_memread(s, address, data + 4 - bytes, bytes);
    return ldl_be_p(data);
}

static void rectangle(QTestState *s, unsigned x0, unsigned y0,
                      unsigned x1, unsigned y1, unsigned sx, unsigned sy)
{
    unsigned direction = (x1 < x0 ? 1 : 0) | (y1 >= y0 ? 2 : 0);

    qtest_writel(s, DE + 0x60, 0x03000000 | (direction << 16));
    qtest_writel(s, DE + 0x70, xy(x0, y0));
    qtest_writel(s, DE + 0xa0, xy(sx, sy));
    qtest_writel(s, DE + 0x874, xy(x1, y1));
}

static void test_registers(void)
{
    QTestState *s = start();
    uint64_t value;

    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        const Register *r = &registers[i];
        uint64_t pattern = r->width == 8 ? 0x123456789abcdef0ULL : 0x13579bdf;

        if (r->readable) {
            g_assert_cmpint(memtx_read(s, DE + r->offset, &value, r->width),
                            ==, MEMTX_OK);
            g_assert_cmphex(value, ==, 0);
        } else {
            g_assert_cmpint(memtx_read(s, DE + r->offset, &value, r->width),
                            !=, MEMTX_OK);
        }
        g_assert_cmpint(memtx_write(s, DE + r->offset, pattern, r->width),
                        ==, MEMTX_OK);
        if (r->readable) {
            memtx_read(s, DE + r->offset, &value, r->width);
            g_assert_cmphex(value, ==, pattern);
        }
        for (unsigned width = 1; width <= 8; width *= 2) {
            if (width != r->width) {
                if (width != 8 || !wide_state_write(r->offset)) {
                    g_assert_cmpint(memtx_write(s, DE + r->offset, 0, width),
                                    !=, MEMTX_OK);
                }
                g_assert_cmpint(memtx_read(s, DE + r->offset, &value, width),
                                !=, MEMTX_OK);
            }
        }
        g_assert_cmpint(memtx_write(s, DE + r->offset + 1, 0, r->width),
                        !=, MEMTX_OK);
    }
    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned word = 0; word < 64; word++) {
            uint64_t address = RE + 0x1000 + bank * 512 + word * 8;
            uint64_t pattern = 0x8001800280038004ULL ^ ((uint64_t)word << 32);

            g_assert_cmphex(qtest_readq(s, address), ==, 0);
            qtest_writeq(s, address, pattern);
            g_assert_cmphex(qtest_readq(s, address), ==, pattern);
            g_assert_cmpint(memtx_writel(s, address, 0), !=, MEMTX_OK);
            g_assert_cmpint(memtx_writeq(s, address + 1, 0), !=, MEMTX_OK);
            g_assert_cmphex(qtest_readq(s, address), ==, pattern);
        }
    }
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        if (registers[i].readable) {
            memtx_read(s, DE + registers[i].offset, &value, registers[i].width);
            g_assert_cmphex(value, ==, 0);
        }
    }
    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned word = 0; word < 64; word++) {
            g_assert_cmphex(qtest_readq(s, RE + 0x1000 + bank * 512 + word * 8),
                            ==, 0);
        }
    }
    qtest_quit(s);
}

static void test_tile_boundaries(void)
{
    QTestState *s = start();

    for (unsigned depth = 0; depth < 3; depth++) {
        unsigned bytes = 1U << depth;
        unsigned crossing = 512 / bytes;

        for (unsigned bank = 0; bank < 3; bank++) {
            for (unsigned lane = 0; lane < 4; lane++) {
                unsigned x0 = (lane + 1) * crossing - 1;
                unsigned x1 = x0 + 1;
                unsigned destination = (bank + 1) % 3;

                if (x1 >= 2048) {
                    continue;
                }
                map_region(s, bank, bytes, x0, 127, x1, 128);
                setup(s, modes[depth] | (bank << 10), false);
                qtest_writel(s, DE + 0xd0, 0x1234abcd);
                for (unsigned y = 127; y <= 128; y++) {
                    for (unsigned x = x0; x <= x1; x++) {
                        pixel_write(s, pixel_address(bank, x, y, bytes),
                                    bytes, 0);
                    }
                }
                rectangle(s, x0, 127, x1, 128, 0, 0);
                for (unsigned y = 127; y <= 128; y++) {
                    for (unsigned x = x0; x <= x1; x++) {
                        uint32_t mask = UINT32_MAX >> (32 - bytes * 8);

                        g_assert_cmphex(pixel_read(s,
                            pixel_address(bank, x, y, bytes), bytes),
                            ==, 0x1234abcd & mask);
                    }
                }
                map_region(s, destination, bytes, x0, 127, x1, 128);
                setup(s, modes[depth] | (bank << 10), true);
                qtest_writel(s, DE + 8, modes[depth] | (destination << 10));
                for (unsigned y = 127; y <= 128; y++) {
                    for (unsigned x = x0; x <= x1; x++) {
                        pixel_write(s, pixel_address(destination, x, y, bytes),
                                    bytes, 0);
                    }
                }
                rectangle(s, x0, 127, x1, 128, x0, 127);
                for (unsigned y = 127; y <= 128; y++) {
                    for (unsigned x = x0; x <= x1; x++) {
                        g_assert_cmphex(pixel_read(s,
                            pixel_address(destination, x, y, bytes), bytes), ==,
                            0x1234abcd & (UINT32_MAX >> (32 - bytes * 8)));
                    }
                }
                assert_idle(s);
            }
        }
    }
    qtest_quit(s);
}

/* Each nibble encodes outputs for (s,d) = 00,01,10,11. */
static uint32_t rop_result(unsigned op, uint32_t source, uint32_t dest,
                           unsigned bits)
{
    static const uint8_t truth[] = {
        0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe,
        0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf,
    };
    uint32_t result = 0;

    for (unsigned bit = 0; bit < bits; bit++) {
        unsigned pair = (((source >> bit) & 1) << 1) | ((dest >> bit) & 1);

        result |= ((truth[op] >> pair) & 1U) << bit;
    }
    return result;
}

static void test_rops(void)
{
    QTestState *s = start();

    tile(s, 0, 0, 3);
    for (unsigned depth = 0; depth < 3; depth++) {
        unsigned bytes = 1U << depth;
        unsigned bits = bytes * 8;

        for (unsigned copy = 0; copy < 2; copy++) {
            for (unsigned op = 0; op < 16; op++) {
                uint32_t source = 0xcccccccc;
                uint32_t dest = 0xaaaaaaaa;

                setup(s, modes[depth], copy);
                qtest_writel(s, DE + 0x18, (copy ? 0x200000 : 0) | 0x2f8);
                qtest_writel(s, DE + 0x1b0, op);
                qtest_writel(s, DE + 0xd0, source);
                pixel_write(s, 0x30000, bytes, source);
                pixel_write(s, 0x30010, bytes, dest);
                rectangle(s, 16 / bytes, 0, 16 / bytes, 0, 0, 0);
                g_assert_cmphex(pixel_read(s, 0x30010, bytes), ==,
                    rop_result(op, source, dest, bits));
            }
        }
        setup(s, modes[depth], true);
        qtest_writel(s, DE + 0x1b0, 12);
        pixel_write(s, 0x30000, bytes, 0x12345678);
        rectangle(s, 0, 0, 0, 0, 0, 0);
        g_assert_cmphex(pixel_read(s, 0x30000, bytes), ==,
            rop_result(12, 0x12345678, 0x12345678, bits));
        qtest_writel(s, DE + 0x18, 0x2000f8);
        qtest_writel(s, DE + 0x1b0, 0xf0000000);
        pixel_write(s, 0x30000, bytes, 0x9abcdef0);
        rectangle(s, 16 / bytes, 0, 16 / bytes, 0, 0, 0);
        g_assert_cmphex(pixel_read(s, 0x30010, bytes), ==,
                       0x9abcdef0 & (UINT32_MAX >> (32 - bits)));
    }
    assert_idle(s);
    qtest_quit(s);
}

static void test_masks(void)
{
    QTestState *s = start();

    tile(s, 0, 0, 3);
    tile(s, 0, 1, 9);
    for (unsigned depth = 0; depth < 3; depth++) {
        unsigned bytes = 1U << depth;
        unsigned bits = bytes * 8;
        unsigned x0 = 512 / bytes - 4;

        for (unsigned mask_bit = 0; mask_bit <= bits; mask_bit++) {
            uint32_t plane = mask_bit == bits ? 0 : 1U << mask_bit;

            for (unsigned lanes = 0; lanes < 16; lanes++) {
                setup(s, modes[depth], false);
                qtest_writel(s, DE + 0x18, 0x80 | (lanes << 3));
                qtest_writel(s, DE + 0x1b8, plane);
                qtest_writel(s, DE + 0xd0, UINT32_MAX);
                for (unsigned x = x0; x < x0 + 8; x++) {
                    uint64_t address = x * bytes < 512 ?
                        0x30000 + x * bytes : 0x90000 + x * bytes - 512;

                    pixel_write(s, address, bytes, 0);
                }
                rectangle(s, x0, 0, x0 + 7, 0, 0, 0);
                for (unsigned x = x0; x < x0 + 8; x++) {
                    uint64_t address = x * bytes < 512 ?
                        0x30000 + x * bytes : 0x90000 + x * bytes - 512;
                    uint32_t expected = 0;

                    for (unsigned b = 0; b < bytes; b++) {
                        if (lanes & (1U << (3 - ((address + b) & 3)))) {
                            expected |= plane &
                                        (0xffU << ((bytes - b - 1) * 8));
                        }
                    }
                    g_assert_cmphex(pixel_read(s, address, bytes),
                                    ==, expected);
                }
            }
        }
        /* Disabling the plane mask ignores its zero contents. */
        setup(s, modes[depth], false);
        qtest_writel(s, DE + 0x18, 0x78);
        qtest_writel(s, DE + 0x1b8, 0);
        qtest_writel(s, DE + 0xd0, UINT32_MAX);
        rectangle(s, 0, 0, 0, 0, 0, 0);
        g_assert_cmphex(pixel_read(s, 0x30000, bytes), ==,
                       UINT32_MAX >> (32 - bits));
    }
    assert_idle(s);
    qtest_quit(s);
}
static void test_overlap(void)
{
    static const struct {
        unsigned x0, y0, x1, y1, sx, sy;
    } cases[] = {
        { 7, 0, 1, 0, 6, 0 }, { 0, 0, 6, 0, 1, 0 },
        { 0, 7, 0, 1, 0, 6 }, { 0, 0, 0, 6, 0, 1 },
        { 7, 7, 1, 1, 6, 6 },
        /* Wrong direction must propagate each earlier destination store. */
        { 1, 0, 7, 0, 0, 0 }, { 0, 1, 0, 7, 0, 0 },
        { 1, 1, 7, 7, 0, 0 },
    };
    QTestState *s = start();

    for (unsigned depth = 0; depth < 3; depth++) {
        unsigned bytes = 1U << depth;

        for (unsigned alias = 0; alias < 3; alias++) {
            for (unsigned c = 0; c < ARRAY_SIZE(cases); c++) {
                uint32_t expected[8][8];
                unsigned ox = 512 / bytes - 4;
                unsigned oy = 124;
                int dx = 1;
                int dy = 1;
                unsigned width = abs((int)cases[c].x1 - cases[c].x0) + 1;
                unsigned height = abs((int)cases[c].y1 - cases[c].y0) + 1;

                if (cases[c].x1 < cases[c].x0) {
                    dx = -1;
                }
                if (cases[c].y1 < cases[c].y0) {
                    dy = -1;
                }
                map_region(s, 0, bytes, ox, oy, ox + 7, oy + 7);
                for (unsigned ty = oy / 128; ty <= (oy + 7) / 128; ty++) {
                    for (unsigned tx = ox * bytes / 512;
                         tx <= (ox + 7) * bytes / 512; tx++) {
                        unsigned index = ty * 16 + tx;
                        unsigned page = page_for(0, index);

                        tile(s, 1, index, page + (alias == 2 ? 0x4000 : 0));
                    }
                }
                for (unsigned y = 0; y < 8; y++) {
                    for (unsigned x = 0; x < 8; x++) {
                        expected[y][x] = (y * 8 + x + 1) * 0x01010101U &
                                         (UINT32_MAX >> (32 - bytes * 8));
                        pixel_write(s, pixel_address(0, ox + x, oy + y, bytes),
                                    bytes, expected[y][x]);
                    }
                }
                for (unsigned y = 0; y < height; y++) {
                    for (unsigned x = 0; x < width; x++) {
                        expected[cases[c].y0 + y * dy][cases[c].x0 + x * dx] =
                            expected[cases[c].sy + y * dy]
                                    [cases[c].sx + x * dx];
                    }
                }
                setup(s, modes[depth], true);
                qtest_writel(s, DE, modes[depth] | (alias ? 0x400 : 0));
                rectangle(s, ox + cases[c].x0, oy + cases[c].y0,
                          ox + cases[c].x1, oy + cases[c].y1,
                          ox + cases[c].sx, oy + cases[c].sy);
                for (unsigned y = 0; y < 8; y++) {
                    for (unsigned x = 0; x < 8; x++) {
                        g_assert_cmphex(pixel_read(s,
                            pixel_address(0, ox + x, oy + y, bytes), bytes),
                            ==, expected[y][x]);
                    }
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_clipping(void)
{
    QTestState *s = start();

    tile(s, 0, 0, 3);
    for (unsigned mask = 0; mask < 5; mask++) {
        for (unsigned inside = 0; inside < 2; inside++) {
            setup(s, 0, false);
            qtest_writeq(s, DE + 0x20 + mask * 8, bounds(2, 2, 5, 5));
            qtest_writeq(s, DE + 0x48, bounds(1, 3, 6, 6));
            qtest_writel(s, DE + 0x10, (1U << (5 + mask)) |
                         (inside ? 1U << mask : 0));
            qtest_writel(s, DE + 0x18, 0x1000f8);
            qtest_writel(s, DE + 0xd0, 0xb7);
            for (unsigned y = 0; y < 7; y++) {
                qtest_memset(s, 0x30000 + y * 512, 0x5a, 7);
            }
            rectangle(s, 0, 0, 6, 6, 0, 0);
            for (unsigned y = 0; y < 7; y++) {
                uint8_t actual[7];

                qtest_memread(s, 0x30000 + y * 512, actual, sizeof(actual));
                for (unsigned x = 0; x < 7; x++) {
                    bool in_mask = x >= 2 && x < 5 && y >= 2 && y < 5;
                    bool eligible = x >= 1 && x < 6 && y >= 3 && y < 6 &&
                                    in_mask == inside;

                    g_assert_cmphex(actual[x], ==, eligible ? 0xb7 : 0x5a);
                }
            }
        }
    }
    qtest_quit(s);
}

/* QEMU requires reset before resuming an internal-error stop. */
static void start_before_failure(QTestState *s)
{
    g_autoptr(QDict) state =
        qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");

    if (strcmp(qdict_get_str(state, "status"), "internal-error")) {
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
    }
}

static void assert_failure(QTestState *s, uint64_t address,
                           uint64_t value, unsigned width)
{
    uint64_t error = qtest_readq(s, CRIME + 0x250);
    uint64_t error_address = qtest_readq(s, CRIME + 0x258);
    uint64_t irq = qtest_readq(s, CRIME + 0x10);
    uint64_t mask = qtest_readq(s, CRIME + 0x18);
    uint32_t status = qtest_readl(s, STATUS);

    start_before_failure(s);
    g_assert_cmpint(memtx_write(s, address, value, width), !=, MEMTX_OK);
    assert_stopped(s);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x250), ==, error);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x258), ==, error_address);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x10), ==, irq);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x18), ==, mask);
    g_assert_cmphex(qtest_readl(s, STATUS), ==, status);
}

static void test_actual_accesses(void)
{
    QTestState *s = start();

    tile(s, 0, 0, 3);
    tile(s, 0, 1, 0x400);
    for (unsigned copy = 0; copy < 2; copy++) {
        for (unsigned clip = 0; clip < 6; clip++) {
            setup(s, 0, copy);
            qtest_writel(s, DE + 0xd0, 0xa1);
            qtest_writel(s, DE + 0x70, xy(511, 0));
            qtest_writel(s, DE + 0xa0, xy(511, 0));
            if (clip == 5) {
                qtest_writel(s, DE + 0x18, (copy ? 0x2002f8 : 0xf8) | 0x100000);
                qtest_writeq(s, DE + 0x48, bounds(0, 0, 512, 1));
            } else {
                qtest_writel(s, DE + 0x10, (1U << (5 + clip)) |
                             (1U << clip));
                qtest_writeq(s, DE + 0x20 + clip * 8, bounds(0, 0, 512, 1));
            }
            qtest_writeb(s, 0x301ff, 0x5c);
            qtest_writel(s, DE + 0x874, xy(512, 0));
            g_assert_cmphex(qtest_readb(s, 0x301ff), ==, copy ? 0x5c : 0xa1);
            if (clip == 5) {
                qtest_writeq(s, DE + 0x48, bounds(0, 0, 513, 1));
            } else {
                qtest_writeq(s, DE + 0x20 + clip * 8, bounds(0, 0, 513, 1));
            }
            qtest_writeb(s, 0x301ff, 0x5c);
            assert_failure(s, DE + 0x874, xy(512, 0), 4);
            g_assert_cmphex(qtest_readb(s, 0x301ff), ==, 0x5c);
        }
    }
    for (unsigned op = 0; op <= 5; op += 5) {
        setup(s, 0, true);
        qtest_writel(s, DE, 0x400); /* Source TLB B stays invalid. */
        qtest_writel(s, DE + 0x1b0, op);
        qtest_writel(s, DE + 0x70, 0);
        qtest_writel(s, DE + 0x74, 0);
        qtest_writel(s, DE + 0xa0, 0);
        qtest_writel(s, DE + 0x1b8, 0);
        qtest_writeb(s, 0x30000, 0x5c);
        qtest_writel(s, DE + 0x9f0, 0x12345678);
        g_assert_cmphex(qtest_readb(s, 0x30000), ==, 0x5c);
        qtest_writel(s, DE + 0x1b8, 1);
        assert_failure(s, DE + 0x9f0, 0xdeadbeef, 4);
        g_assert_cmphex(qtest_readb(s, 0x30000), ==, 0x5c);
    }
    setup(s, 0, false);
    /* No byte enables; even a bad tile is unused. */
    qtest_writel(s, DE + 0x18, 0);
    rectangle(s, 512, 0, 512, 0, 0, 0);
    qtest_writel(s, DE + 8, 0xdeadbeef);
    assert_failure(s, DE + 0x874, xy(512, 0), 4);
    qtest_quit(s);
}

static uint64_t parameter_value(unsigned offset)
{
    switch (offset) {
    case 0x000: case 0x008: case 0x010: case 0x050: case 0x058:
    case 0x070: case 0x078: case 0x0a0: case 0x0d8:
        return 0;
    case 0x018:
        return 0x1002f8;
    case 0x020: case 0x028: case 0x030: case 0x038: case 0x040:
        return bounds(0, 0, 1, 1);
    case 0x048:
        return bounds(0, 0, 1, 1);
    case 0x060:
        return 0x03020000;
    case 0x074:
        return xy(1, 0);
    case 0x0a8: case 0x0ac:
        return 1;
    case 0x0c0: case 0x0c4:
        return 0;
    case 0x0d0:
        return 0xcc;
    case 0x1b0:
        return 6;
    case 0x1b8:
        return 0xff;
    default:
        g_assert_not_reached();
    }
}

static void test_start_forms(void)
{
    const unsigned source_parameters[] = { 0, 0xa0, 0xa8, 0xac };
    QTestState *s = start();

    tile(s, 0, 0, 3);
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        const Register *r = &registers[i];
        uint64_t value = parameter_value(r->offset);
        uint64_t readback;

        setup(s, 0, false);
        qtest_writel(s, DE + 0x18, 0x1002f8);
        qtest_writel(s, DE + 0x1b0, 6);
        qtest_writel(s, DE + 0xd0, 0xcc);
        qtest_writel(s, DE + 0x70, 0);
        qtest_writel(s, DE + 0x74, xy(1, 0));
        qtest_writeq(s, DE + 0x48, bounds(0, 0, 2, 1));
        for (unsigned n = 0; n < 5; n++) {
            qtest_writeq(s, DE + 0x20 + n * 8, bounds(0, 0, 2, 1));
        }
        if (r->offset >= 0x20 && r->offset <= 0x40) {
            unsigned n = (r->offset - 0x20) / 8;

            qtest_writel(s, DE + 0x10, (1U << (5 + n)) | (1U << n));
        }
        /* The old pending value must not control prospective execution. */
        switch (r->offset) {
        case 0x008: case 0x018:
            qtest_writel(s, DE + r->offset, 0xdeadbeef);
            break;
        case 0x050: case 0x058:
            qtest_writel(s, DE + r->offset, 1);
            break;
        case 0x060: case 0x1b8:
            qtest_writel(s, DE + r->offset, 0);
            break;
        case 0x1b0:
            qtest_writel(s, DE + 0x1b0, 3);
            break;
        case 0x070:
            qtest_writel(s, DE + 0x70, xy(1, 0));
            break;
        default:
            break;
        }
        if (r->offset == 0x74) {
            qtest_writel(s, DE + 0x74, 0);
        } else if (r->offset == 0xd0) {
            qtest_writel(s, DE + 0xd0, 0x11);
        }
        qtest_memset(s, 0x30000, 0x5a, 3);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset, value, r->width),
                        ==, MEMTX_OK);
        g_assert_cmphex(qtest_readb(s, 0x30000), ==, 0x96);
        g_assert_cmphex(qtest_readb(s, 0x30001), ==,
                       r->offset >= 0x20 && r->offset <= 0x48 ? 0x5a : 0x96);
        g_assert_cmphex(qtest_readb(s, 0x30002), ==, 0x5a);
        if (r->readable) {
            memtx_read(s, DE + r->offset, &readback, r->width);
            g_assert_cmphex(readback, ==, value);
        }
        assert_idle(s);
    }
    /* Source parameters require a copy to distinguish their pending values. */
    tile(s, 1, 0, 9);
    qtest_writeb(s, 0x90000, 0x91);
    qtest_writeb(s, 0x90001, 0x92);
    for (unsigned i = 0; i < ARRAY_SIZE(source_parameters); i++) {
        unsigned offset = source_parameters[i];
        uint32_t value = offset == 0 ? 0x400 : offset == 0xa0 ? 0 : 1;

        setup(s, 0, true);
        qtest_writel(s, DE, 0x400);
        qtest_writel(s, DE + 0x70, 0);
        qtest_writel(s, DE + 0x74, 0);
        qtest_writel(s, DE + 0xa0, 0);
        qtest_writel(s, DE + offset, offset == 0 ? 0x800 :
                     offset == 0xa0 ? xy(1, 0) : 0);
        qtest_writeb(s, 0x30000, 0x5a);
        g_assert_cmpint(memtx_writel(s, DE + 0x800 + offset, value),
                        ==, MEMTX_OK);
        g_assert_cmphex(qtest_readb(s, 0x30000), ==, 0x91);
    }
    /* Direct NULL ignores both payloads; START XOR commits exactly once. */
    setup(s, 0, false);
    qtest_writel(s, DE + 0x18, 0x2f8);
    qtest_writel(s, DE + 0x1b0, 6);
    qtest_writel(s, DE + 0xd0, 0xcc);
    qtest_writel(s, DE + 0x70, 0);
    qtest_writel(s, DE + 0x74, 0);
    qtest_writeb(s, 0x30000, 0x5a);
    qtest_writel(s, DE + 0x1f0, 0x12345678);
    qtest_writel(s, DE + 0x1f0, 0x87654321);
    g_assert_cmphex(qtest_readb(s, 0x30000), ==, 0x5a);
    qtest_writel(s, DE + 0x9f0, 0xdeadbeef);
    g_assert_cmphex(qtest_readb(s, 0x30000), ==, 0x96);
    qtest_quit(s);
}
static void mte_setup(QTestState *s, bool copy, unsigned source_bank,
                      unsigned destination_bank)
{
    qtest_writel(s, MTE, (copy ? 1U << 11 : 0) |
                 (source_bank << 5) | (destination_bank << 2));
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x18, 0xb7b7b7b7);
    qtest_writel(s, MTE + 0x40, 1);
    qtest_writel(s, MTE + 0x48, 1);
}

static void test_mte_start_forms(void)
{
    const unsigned offsets[] = {
        0, 8, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48,
    };
    const struct {
        unsigned offset;
        uint32_t old_value;
        uint32_t value;
    } copy_parameters[] = {
        { 0x20, 0x00010000, 0x00000000 },
        { 0x28, 0x00020001, 0x00030001 },
        { 0x40, UINT32_MAX, 1 },
        { 0x48, UINT32_MAX, 1 },
    };
    const uint8_t source[] = { 0x13, 0x27, 0x49, 0x6b, 0x8d, 0xaf, 0xc1, 0xe3 };
    QTestState *s = start();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
            unsigned offset = offsets[i];
            uint32_t value;

            map_region(s, bank, 1, 511, 127, 512, 128);
            mte_setup(s, false, 0, bank);
            qtest_writel(s, MTE + 0x30, xy(511, 127));
            qtest_writel(s, MTE + 0x38, xy(512, 128));
            switch (offset) {
            case 0:
                value = bank << 2;
                qtest_writel(s, MTE, 0x80000000);
                g_assert_cmphex(qtest_readl(s, MTE), ==, 0x80000000);
                break;
            case 8:
                value = UINT32_MAX;
                qtest_writel(s, MTE + 8, 0);
                g_assert_cmphex(qtest_readl(s, MTE + 8), ==, 0);
                break;
            case 0x18:
                value = 0xb7b7b7b7;
                qtest_writel(s, MTE + 0x18, 0x11);
                break;
            case 0x30:
                value = xy(511, 127);
                qtest_writel(s, MTE + 0x30, xy(512, 127));
                break;
            case 0x38:
                value = xy(512, 128);
                qtest_writel(s, MTE + 0x38, xy(511, 127));
                break;
            case 0x40: case 0x48:
                value = 1;
                break;
            default:
                value = 0xdeadbeef; /* Clear ignores stored source endpoints. */
                break;
            }
            for (unsigned y = 127; y <= 128; y++) {
                for (unsigned x = 511; x <= 512; x++) {
                    qtest_writeb(s, pixel_address(bank, x, y, 1), 0x5a);
                }
            }
            g_assert_cmpint(memtx_writel(s, MTE + 0x800 + offset, value),
                            ==, MEMTX_OK);
            for (unsigned y = 127; y <= 128; y++) {
                for (unsigned x = 511; x <= 512; x++) {
                    g_assert_cmphex(qtest_readb(s,
                        pixel_address(bank, x, y, 1)),
                                    ==, 0xb7);
                }
            }
            if (offset != 0x30 && offset != 0x38) {
                g_assert_cmphex(qtest_readl(s, MTE + offset), ==, value);
            }
            assert_idle(s);
        }
    }
    map_region(s, 0, 1, 0, 0, 3, 1);
    map_region(s, 1, 1, 0, 0, 3, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(copy_parameters); i++) {
        unsigned offset = copy_parameters[i].offset;

        mte_setup(s, true, 0, 1);
        qtest_writel(s, MTE + 0x20, xy(0, 0));
        qtest_writel(s, MTE + 0x28, xy(3, 1));
        qtest_writel(s, MTE + 0x30, xy(0, 0));
        qtest_writel(s, MTE + 0x38, xy(3, 1));
        qtest_writel(s, MTE + offset, copy_parameters[i].old_value);
        g_assert_cmphex(qtest_readl(s, MTE + offset), ==,
                       copy_parameters[i].old_value);
        for (unsigned row = 0; row < 2; row++) {
            qtest_memwrite(s, pixel_address(0, 0, row, 1), source + row * 4, 4);
            qtest_memset(s, pixel_address(1, 0, row, 1), 0x5a, 5);
        }
        g_assert_cmpint(memtx_writel(s, MTE + 0x800 + offset,
                                    copy_parameters[i].value), ==, MEMTX_OK);
        for (unsigned row = 0; row < 2; row++) {
            uint8_t actual[5];
            uint8_t retained_source[4];

            qtest_memread(s, pixel_address(1, 0, row, 1), actual,
                          sizeof(actual));
            g_assert_cmpmem(actual, 4, source + row * 4, 4);
            g_assert_cmphex(actual[4], ==, 0x5a);
            qtest_memread(s, pixel_address(0, 0, row, 1), retained_source,
                          sizeof(retained_source));
            g_assert_cmpmem(retained_source, sizeof(retained_source),
                            source + row * 4, 4);
        }
        g_assert_cmphex(qtest_readl(s, MTE + offset), ==,
                       copy_parameters[i].value);
        g_assert_cmphex(qtest_readl(s, MTE + 0x20), ==, xy(0, 0));
        g_assert_cmphex(qtest_readl(s, MTE + 0x28), ==, xy(3, 1));
        g_assert_cmphex(qtest_readl(s, MTE + 0x40), ==, 1);
        g_assert_cmphex(qtest_readl(s, MTE + 0x48), ==, 1);
        assert_idle(s);
    }
    qtest_quit(s);
}

static void test_mte_scroll(void)
{
    QTestState *s = start();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned downward = 0; downward < 2; downward++) {
            for (unsigned reverse = 0; reverse < 2; reverse++) {
                uint8_t expected[6][4];
                int step = reverse ? -1 : 1;
                unsigned src = downward ? 0 : 1;
                unsigned dst = downward ? 1 : 0;
                unsigned first = reverse ? 4 : 0;
                unsigned last = reverse ? 0 : 4;

                map_region(s, bank, 1, 510, 125, 513, 130);
                for (unsigned y = 0; y < 6; y++) {
                    for (unsigned x = 0; x < 4; x++) {
                        expected[y][x] = 0x20 + y * 4 + x;
                        qtest_writeb(s,
                            pixel_address(bank, 510 + x, 125 + y, 1),
                                     expected[y][x]);
                    }
                }
                for (unsigned row = 0; row < 5; row++) {
                    unsigned n = first + row * step;

                    for (unsigned x = 0; x < 4; x++) {
                        expected[dst + n][x] = expected[src + n][x];
                    }
                }
                mte_setup(s, true, bank, bank);
                qtest_writel(s, MTE + 0x40, reverse ? UINT32_MAX : 1);
                qtest_writel(s, MTE + 0x48, reverse ? UINT32_MAX : 1);
                qtest_writel(s, MTE + 0x20, xy(510, 125 + src + first));
                qtest_writel(s, MTE + 0x28, xy(513, 125 + src + last));
                qtest_writel(s, MTE + 0x30, xy(510, 125 + dst + first));
                qtest_writel(s, MTE + 0x38, xy(513, 125 + dst + last));
                qtest_writel(s, MTE + 0x70, 0x12345678);
                qtest_writel(s, MTE + 0x70, 0xfedcba98);
                g_assert_cmphex(qtest_readb(s,
                    pixel_address(bank, 510, 125, 1)), ==, 0x20);
                qtest_writel(s, MTE + 0x870, 0xabcdef01);
                for (unsigned y = 0; y < 6; y++) {
                    for (unsigned x = 0; x < 4; x++) {
                        g_assert_cmphex(qtest_readb(s,
                            pixel_address(bank, 510 + x, 125 + y, 1)),
                            ==, expected[y][x]);
                    }
                }
                assert_idle(s);
            }
        }
    }
    /* Disjoint copies between different TLB banks remain admitted. */
    mte_setup(s, true, 0, 1);
    qtest_writel(s, MTE + 0x20, xy(510, 125));
    qtest_writel(s, MTE + 0x28, xy(513, 125));
    qtest_writel(s, MTE + 0x30, xy(510, 125));
    qtest_writel(s, MTE + 0x838, xy(513, 125));
    for (unsigned x = 510; x <= 513; x++) {
        g_assert_cmphex(qtest_readb(s, pixel_address(1, x, 125, 1)), ==,
                       qtest_readb(s, pixel_address(0, x, 125, 1)));
    }
    qtest_quit(s);
}

static void test_atomic_commands(void)
{
    QTestState *s = start();
    const unsigned malformed[] = {
        0xc00, 0x100, 0x001, 0x300, 0x224, 0x1c00,
    };
    uint8_t bytes[16];

    tile(s, 0, 0, 3);
    tile(s, 0, 1, 0x400);
    qtest_memset(s, 0x30000, 0x5a, sizeof(bytes));
    qtest_writeq(s, CRIME + 0x18, 1U << 4);
    qtest_set_irq_in(s, "/machine/peripheral/crime", "mace", 4, 1);
    setup(s, 0, false);
    qtest_writel(s, DE + 0xd0, 0xcc);
    qtest_writel(s, DE + 0x70, 0);
    qtest_writel(s, DE + 0x74, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(malformed); i++) {
        assert_failure(s, DE + 0x808, malformed[i], 4);
        g_assert_cmphex(qtest_readl(s, DE + 8), ==, 0);
    }
    /* Valid color, bad final tile: rejected foreground must not be retained. */
    qtest_writel(s, DE + 0x74, xy(512, 0));
    assert_failure(s, DE + 0x8d0, 0xee, 4);
    qtest_writel(s, DE + 0x74, 0);
    qtest_writel(s, DE + 0x9f0, 0);
    g_assert_cmphex(qtest_readb(s, 0x30000), ==, 0xcc);
    /* Failed write-only final vertex must retain the earlier endpoint. */
    qtest_writeb(s, 0x30000, 0x5a);
    assert_failure(s, DE + 0x874, xy(512, 0), 4);
    qtest_writel(s, DE + 0x9f0, 0);
    g_assert_cmphex(qtest_readb(s, 0x30000), ==, 0xcc);
    qtest_writel(s, DE + 0x18, 0x2f8);
    assert_failure(s, DE + 0x9b0, 0x10006, 4);
    g_assert_cmphex(qtest_readl(s, DE + 0x1b0), ==, 3);
    qtest_writel(s, DE + 0x50, 1);
    assert_failure(s, DE + 0x9f0, 0x1234, 4);
    g_assert_cmphex(qtest_readl(s, DE + 0x50), ==, 1);
    qtest_memread(s, 0x30000, bytes, sizeof(bytes));
    for (unsigned i = 1; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0x5a);
    }
    /* Repeated destination backing identities reject before any write. */
    setup(s, 0, false);
    tile(s, 0, 1, 3);
    qtest_writel(s, DE + 0x70, 0);
    qtest_memset(s, 0x30000, 0x5a, 512);
    assert_failure(s, DE + 0x874, xy(512, 0), 4);
    qtest_memread(s, 0x30000, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0x5a);
    }
    qtest_quit(s);
}

static void test_mte_rejected(void)
{
    QTestState *s = start();

    tile(s, 0, 0, 3);
    tile(s, 1, 0, 3);
    for (unsigned fault = 0; fault < 7; fault++) {
        mte_setup(s, true, 0, 0);
        qtest_writel(s, MTE + 0x20, xy(0, 0));
        qtest_writel(s, MTE + 0x28, xy(3, 1));
        qtest_writel(s, MTE + 0x30, xy(0, 1));
        qtest_writel(s, MTE + 0x38, xy(3, 2));
        switch (fault) {
        case 0: /* Horizontal overlap. */
            qtest_writel(s, MTE + 0x30, xy(1, 0));
            qtest_writel(s, MTE + 0x38, xy(4, 1));
            break;
        case 1:
            qtest_writel(s, MTE + 0x40, UINT32_MAX);
            break;
        case 2:
            qtest_writel(s, MTE + 0x48, 2);
            break;
        case 3: /* Negative clear remains unsupported. */
            qtest_writel(s, MTE, 0);
            qtest_writel(s, MTE + 0x48, UINT32_MAX);
            qtest_writel(s, MTE + 0x38, xy(3, 0));
            break;
        case 4: /* Overlapping selectors from different banks. */
            qtest_writel(s, MTE, (1U << 11) | (1U << 5));
            break;
        case 5: /* Distinct touched indices alias one backing tile. */
            tile(s, 0, 16, 3);
            qtest_writel(s, MTE + 0x20, xy(0, 0));
            qtest_writel(s, MTE + 0x28, xy(3, 128));
            qtest_writel(s, MTE + 0x30, xy(0, 1));
            qtest_writel(s, MTE + 0x38, xy(3, 129));
            break;
        case 6: /* Zero mask cannot hide the invalid second source tile. */
            qtest_writel(s, MTE + 8, 0);
            qtest_writel(s, MTE + 0x20, xy(511, 0));
            qtest_writel(s, MTE + 0x28, xy(514, 1));
            break;
        }
        qtest_memset(s, 0x30000, 0x5a, 1536);
        assert_failure(s, MTE + 0x870, 0x12345678, 4);
        for (unsigned y = 0; y < 3; y++) {
            g_assert_cmphex(qtest_readl(s, 0x30000 + y * 512), ==, 0x5a5a5a5a);
        }
    }
    qtest_quit(s);
}

static void test_malformed_access_stops(void)
{
    const struct {
        unsigned offset;
        unsigned width;
    } accesses[] = {
        { 0x2008, 2 }, { 0x1000, 4 }, { 0x2874, 8 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(accesses); i++) {
        QTestState *s = start();
        uint8_t bytes[12];

        tile(s, 0, 0, 3);
        setup(s, 0x228, false);
        qtest_writel(s, DE + 0xd0, 0x1234abcd);
        qtest_writel(s, DE + 0x70, 0);
        qtest_writel(s, DE + 0x74, 0);
        qtest_memset(s, 0x30000, 0x5a, sizeof(bytes));
        /* A fresh running fixture proves this access caused the stop. */
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
        g_autoptr(QDict) state =
            qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");

        g_assert_cmpstr(qdict_get_str(state, "status"), ==, "running");
        assert_failure(s, RE + accesses[i].offset, xy(1, 0), accesses[i].width);
        g_assert_cmphex(qtest_readl(s, DE + 8), ==, 0x228);
        g_assert_cmphex(qtest_readq(s, RE + 0x1000), ==, 0x8003000000000000ULL);
        qtest_memread(s, 0x30000, bytes, sizeof(bytes));
        for (unsigned b = 0; b < sizeof(bytes); b++) {
            g_assert_cmphex(bytes[b], ==, 0x5a);
        }
        /* The rejected START write must not replace the staged endpoint. */
        qtest_writel(s, DE + 0x9f0, 0);
        g_assert_cmphex(qtest_readl(s, 0x30000), ==, 0x1234abcd);
        g_assert_cmphex(qtest_readl(s, 0x30004), ==, 0x5a5a5a5a);
        qtest_quit(s);
    }
}

static void test_unsupported(void)
{
    QTestState *s = start();
    const unsigned hard_offsets[] = {
        0x2804, 0x280c, 0x2880, 0x29f8,
        0x3810, 0x3850, 0x3878,
    };
    uint64_t value;
    uint32_t word;

    tile(s, 0, 0, 3);
    setup(s, 0, false);
    qtest_writel(s, DE + 0x70, 0);
    qtest_writel(s, DE + 0x74, 0);
    qtest_memset(s, 0x30000, 0x5a, 16);
    g_assert_cmpint(memtx_writel(s, RE + 0x400, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, RE + 0x1000), ==, 0x8003000000000000ULL);
    g_assert_cmphex(qtest_readl(s, DE + 0x70), ==, 0);
    g_assert_cmphex(qtest_readl(s, DE + 0x74), ==, 0);
    g_assert_cmphex(qtest_readl(s, STATUS) & ~0x3ffffU, ==, 0x1e000000);
    for (unsigned i = 0; i < 16; i++) {
        g_assert_cmphex(qtest_readb(s, 0x30000 + i), ==, 0x5a);
    }
    assert_failure(s, RE + 0x400, 0, 8);
    for (unsigned i = 0; i < ARRAY_SIZE(hard_offsets); i++) {
        assert_failure(s, RE + hard_offsets[i], 0, 4);
        start_before_failure(s);
        g_assert_cmpint(memtx_readl(s, RE + hard_offsets[i], &word),
                        !=, MEMTX_OK);
        assert_stopped(s);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        unsigned offset = registers[i].offset;
        bool paired = offset == 0x70 || offset == 0xa8 || offset == 0xc0;
        unsigned wrong_width = registers[i].width == 8 ? 4 : paired ? 2 : 8;

        start_before_failure(s);
        g_assert_cmpint(memtx_read(s, DE + 0x800 + registers[i].offset,
                                  &value, registers[i].width), !=, MEMTX_OK);
        assert_stopped(s);
        assert_failure(s, DE + 0x800 + registers[i].offset + 1, 0,
                       registers[i].width);
        assert_failure(s, DE + 0x800 + registers[i].offset, 0,
                       wrong_width);
    }
    for (unsigned engine = 0; engine < 2; engine++) {
        uint64_t null = engine ? MTE + 0x70 : DE + 0x1f0;
        uint64_t flush = null + 8;
        g_autoptr(QDict) state = NULL;

        qtest_quit(s);
        s = start();
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
        g_assert_cmpint(memtx_readl(s, null, &word), !=, MEMTX_OK);
        state = qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");
        g_assert_cmpstr(qdict_get_str(state, "status"), ==, "running");
        assert_failure(s, null, 0, 8);
        assert_failure(s, null + 1, 0, 4);
        qtest_writel(s, flush, 0);
        assert_failure(s, flush, 1, 4);
    }
    qtest_quit(s);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("crime-render-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = start();
    QTestState *to = start_options("-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    tile(from, 0, 0, 3);
    tile(from, 1, 0, 0x4003);
    setup(from, 0, true);
    qtest_writel(from, DE, 0x400);
    qtest_writel(from, DE + 0x1b0, 6);
    qtest_writel(from, DE + 0x1b8, 0x0f);
    qtest_writel(from, DE + 0x18, 0x3002f8);
    qtest_writeq(from, DE + 0x48, bounds(0, 0, 8, 1));
    qtest_writeq(from, DE + 0x20, bounds(0, 0, 8, 1));
    qtest_writel(from, DE + 0x10, 0x21);
    qtest_writel(from, DE + 0x70, xy(7, 0));
    qtest_writel(from, DE + 0x74, xy(1, 0));
    qtest_writel(from, DE + 0x60, 0x03010000);
    qtest_writel(from, DE + 0xa0, xy(6, 0));
    for (unsigned x = 0; x < 8; x++) {
        qtest_writeb(from, 0x30000 + x, 0xa0 + x);
    }
    /* Ordinary invalid state must survive and fail only at commitment. */
    qtest_writel(from, DE + 8, 0xdeadbeef);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readq(to, RE + 0x1000), ==, 0x8003000000000000ULL);
    g_assert_cmphex(qtest_readq(to, RE + 0x1200), ==, 0xc003000000000000ULL);
    g_assert_cmphex(qtest_readl(to, DE), ==, 0x400);
    g_assert_cmphex(qtest_readl(to, DE + 0x1b8), ==, 0xf);
    g_assert_cmphex(qtest_readl(to, DE + 0x1b0), ==, 6);
    g_assert_cmphex(qtest_readq(to, DE + 0x48), ==, bounds(0, 0, 8, 1));
    g_assert_cmphex(qtest_readl(to, DE + 8), ==, 0xdeadbeef);
    assert_failure(to, DE + 0x9f0, 0x12345678, 4);
    for (unsigned x = 0; x < 8; x++) {
        g_assert_cmphex(qtest_readb(to, 0x30000 + x), ==, 0xa0 + x);
    }
    qtest_writel(to, DE + 8, 0);
    qtest_writel(to, DE + 0x9f0, 0xdeadbeef);
    for (unsigned x = 1; x < 8; x++) {
        g_assert_cmphex(qtest_readb(to, 0x30000 + x), ==,
                       0xa0 | ((x ^ (x - 1)) & 0xf));
    }
    setup(to, 0, false);
    qtest_writel(to, DE + 0x18, 0x88); /* Only fourth backing-word byte. */
    qtest_writel(to, DE + 0x1b8, 0x55);
    qtest_writel(to, DE + 0xd0, 0x55);
    rectangle(to, 3, 0, 3, 0, 0, 0);
    g_assert_cmphex(qtest_readb(to, 0x30003), ==, (0xa1 & ~0x55) | 0x55);
    qtest_system_reset(to);
    g_assert_cmphex(qtest_readq(to, RE + 0x1000), ==, 0);
    g_assert_cmphex(qtest_readl(to, DE + 0x1b8), ==, 0);
    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(directory);
}

static void test_banked_decoder(void)
{
    QTestState *s = start_profile("legacy-software", true, true, "");
    uint8_t missed;

    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(s, CRIME + 0x208 + bank * 8, 0x100 | (bank * 4));
    }
    /* Adjacent tile rows straddle the two populated backing banks. */
    tile(s, 0, 0, 0x7ff);
    tile(s, 0, 16, 0x800);
    mte_setup(s, true, 0, 0);
    qtest_writel(s, MTE + 0x20, xy(510, 127));
    qtest_writel(s, MTE + 0x28, xy(511, 127));
    qtest_writel(s, MTE + 0x30, xy(510, 128));
    qtest_writeb(s, 0x07fffffe, 0x12);
    qtest_writeb(s, 0x07ffffff, 0x34);
    qtest_writeb(s, 0x080001fe, 0x5a);
    qtest_writeb(s, 0x080001ff, 0x5a);
    qtest_writel(s, MTE + 0x838, xy(511, 128));
    g_assert_cmphex(qtest_readw(s, 0x080001fe), ==, 0x1234);
    /* Remove bank one's placement; prevalidation must preserve latches. */
    qtest_writeq(s, CRIME + 0x210, 0x100);
    g_assert_cmpint(memtx_readb(s, 0x080001fe, &missed), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x250), !=, 0);
    assert_failure(s, MTE + 0x838, xy(511, 128), 4);
    qtest_writeq(s, CRIME + 0x210, 0x104);
    g_assert_cmphex(qtest_readw(s, 0x080001fe), ==, 0x1234);
    /* Wrong bank geometry and the ROM aperture are never renderer RAM. */
    qtest_writeq(s, CRIME + 0x210, 4);
    assert_failure(s, MTE + 0x838, xy(511, 128), 4);
    qtest_writeq(s, CRIME + 0x210, 0x104);
    setup(s, 0, false);
    qtest_writel(s, DE + 0x70, 0);
    tile(s, 0, 0, 0x1fc0);
    assert_failure(s, DE + 0x874, 0, 4);
    g_assert_cmphex(qtest_readw(s, 0x080001fe), ==, 0x1234);
    qtest_quit(s);
}

static void test_unselected_profile(void)
{
    QTestState *s = start_profile("1.5", false, true, "");
    uint32_t value;
    const unsigned writes[] = {
        0x1000, 0x2000, 0x2874, 0x21f0, 0x29f0, 0x3070, 0x3870,
    };

    for (unsigned i = 0; i < ARRAY_SIZE(writes); i++) {
        g_assert_cmpint(memtx_write(s, RE + writes[i], 0,
                                   writes[i] == 0x1000 ? 8 : 4), !=, MEMTX_OK);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(wide_slots); i++) {
        g_assert_cmpint(memtx_writeq(s, DE + wide_slots[i],
                                    0x1122334455667788ULL), !=, MEMTX_OK);
    }
    g_assert_cmpint(memtx_writeq(s, DE + 0x70, 0x1122334455667788ULL),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, DE + 0xa8, 0x1122334455667788ULL),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readl(s, DE, &value), !=, MEMTX_OK);
    qtest_quit(s);
}

static void test_coordinate_limit(void)
{
    QTestState *s = start();
    uint64_t last = pixel_address(2, 2047, 2047, 4);

    tile(s, 2, 255, page_for(2, 255));
    setup(s, 0xa28, false);
    qtest_writel(s, DE + 0xd0, 0x1234abcd);
    pixel_write(s, last, 4, 0x5a5a5a5a);
    rectangle(s, 2047, 2047, 2047, 2047, 0, 0);
    g_assert_cmphex(pixel_read(s, last, 4), ==, 0x1234abcd);
    assert_failure(s, DE + 0x874, xy(2048, 2047), 4);
    g_assert_cmphex(pixel_read(s, last, 4), ==, 0x1234abcd);
    qtest_quit(s);
}

static void test_rom_canary(void)
{
    const uint8_t image[] = { 0x43, 0x41, 0x4e, 0x41, 0x52, 0x59, 0xa5, 0x5a };
    g_autofree char *directory =
        g_dir_make_tmp("crime-render-rom-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "synthetic.bin", NULL);
    QTestState *s;

    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), NULL));
    s = qtest_initf(MEMTX_TEST_DEVICE
        "-M o2,register-test-profile=on,crime-profile=legacy-software,"
        "bank-profile=8x128m-synthetic,reset-low-ram-approx=on,"
        "functional-memory-transfer=on,prom-private-window=on,"
        "prom-sb64-reset=on,prom-id-word=on,gbe11-registers=on,"
        "password-clear-input=0,rom-image=%s,"
        "rom-size=524288,rom-offset=0,rom-pad-byte=238 "
        "-cpu R10000 -m 1G -nodefaults -S", path);
    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(s, CRIME + 0x208 + bank * 8, 0x100 | (bank * 4));
    }
    qtest_writeq(s, 0x30000, 0x1122334455667788ULL);
    for (unsigned alias = 0; alias < 8; alias++) {
        uint64_t address = 0x1fc00000ULL + alias * 0x80000;

        g_assert_cmphex(qtest_readq(s, address), ==, 0x43414e415259a55aULL);
        tile(s, 0, 0, address >> 16);
        setup(s, 0, false);
        qtest_writel(s, DE + 0xd0, 0xff);
        qtest_writel(s, DE + 0x70, 0);
        qtest_writel(s, DE + 0x74, xy(7, 0));
        assert_failure(s, DE + 0x9f0, 0x12345678, 4);
        mte_setup(s, false, 0, 0);
        qtest_writel(s, MTE + 0x30, 0);
        assert_failure(s, MTE + 0x838, xy(7, 0), 4);
        g_assert_cmphex(qtest_readq(s, address), ==, 0x43414e415259a55aULL);
        g_assert_cmphex(qtest_readq(s, 0x30000), ==, 0x1122334455667788ULL);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(directory);
}

static void test_wide_slot(void)
{
    QTestState *s = start();

    qtest_writel(s, DE + 0x50, 0x89abcdef);
    qtest_writel(s, DE + 0x58, 0x99aabbcc);
    qtest_writeq(s, 0x30000, 0x123456789abcdef0ULL);
    g_assert_cmpint(memtx_writeq(s, DE + 0x50, 0x1122334455667788ULL),
                    ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, DE + 0x50), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(s, DE + 0x58), ==, 0x99aabbcc);
    g_assert_cmphex(qtest_readq(s, 0x30000), ==, 0x123456789abcdef0ULL);
    assert_idle(s);
    qtest_quit(s);
}

/* Public same-build model format: 192 TLB and 128 logical state words. */
#define RENDER_WORDS (192 + 128)

static void saved_renderer(const char *path, uint64_t words[RENDER_WORDS])
{
    const char name[] = "sgi-crime/renderer";
    g_autofree char *stream = NULL;
    gsize length;
    const char *state;

    g_assert_true(g_file_get_contents(path, &stream, &length, NULL));
    state = memmem(stream, length, name, sizeof(name) - 1);
    g_assert_nonnull(state);
    state += sizeof(name) - 1;
    g_assert_cmpuint(length - (state - stream), >=, 4 + RENDER_WORDS * 8);
    g_assert_cmphex(ldl_be_p(state), ==, 1);
    for (unsigned i = 0; i < RENDER_WORDS; i++) {
        words[i] = ldq_be_p(state + 4 + i * 8);
    }
}

static void save_renderer(QTestState *s, const char *path,
                          uint64_t words[RENDER_WORDS])
{
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *uri = g_strdup_printf("exec:cat > %s", quoted);

    sgi_migrate_stopped(s, uri);
    saved_renderer(path, words);
}

static void test_wide_slot_state(void)
{
    const uint64_t patterns[] = {
        0x1122334455667788ULL, 0x5566778811223344ULL,
        0x89abcdef00000000ULL, 0x000000009abcdef0ULL,
    };
    g_autofree char *directory =
        g_dir_make_tmp("crime-wide-state-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "state", NULL);

    for (unsigned pattern = 0; pattern < ARRAY_SIZE(patterns); pattern++) {
        QTestState *s = start();
        uint64_t expected[RENDER_WORDS] = { 0 };
        uint64_t actual[RENDER_WORDS];

        qtest_writeq(s, 0x30000, 0x123456789abcdef0ULL);
        qtest_writeq(s, RE + 0x1000, 0x8001800280038004ULL);
        expected[0] = 0x8001800280038004ULL;
        for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
            const Register *r = &registers[i];
            uint64_t value = r->width == 8 ?
                0x123456789abcde00ULL | i : 0xa0000000U | r->offset;

            g_assert_cmpint(memtx_write(s, DE + r->offset, value, r->width),
                            ==, MEMTX_OK);
            expected[192 + r->offset / 4] = value;
        }
        for (unsigned i = 0; i < ARRAY_SIZE(wide_slots); i++) {
            unsigned offset = wide_slots[i];

            g_assert_cmpint(memtx_writeq(s, DE + offset, patterns[pattern]),
                            ==, MEMTX_OK);
            expected[192 + offset / 4] = patterns[pattern] >> 32;
            for (unsigned r = 0; r < ARRAY_SIZE(registers); r++) {
                if (registers[r].readable) {
                    uint64_t value;

                    g_assert_cmpint(memtx_read(s, DE + registers[r].offset,
                        &value, registers[r].width), ==, MEMTX_OK);
                    g_assert_cmphex(value, ==,
                        expected[192 + registers[r].offset / 4]);
                }
            }
            g_assert_cmphex(qtest_readq(s, 0x30000), ==,
                           0x123456789abcdef0ULL);
            assert_idle(s);
        }
        save_renderer(s, path, actual);
        for (unsigned i = 0; i < RENDER_WORDS; i++) {
            g_assert_cmphex(actual[i], ==, expected[i]);
        }
        qtest_quit(s);
        g_unlink(path);
    }
    g_rmdir(directory);
}

static void test_wide_pairs(void)
{
    QTestState *s = start();

    tile(s, 0, 0, 3);
    for (unsigned wide = 0; wide < 2; wide++) {
        setup(s, 0, false);
        qtest_writel(s, DE + 0xd0, 0x6b);
        for (unsigned y = 0; y < 4; y++) {
            qtest_memset(s, 0x30000 + y * 512, 0x5a, 8);
        }
        if (wide) {
            g_assert_cmpint(memtx_writeq(s, DE + 0x70,
                ((uint64_t)xy(2, 1) << 32) | xy(5, 2)), ==, MEMTX_OK);
        } else {
            qtest_writel(s, DE + 0x70, xy(2, 1));
            qtest_writel(s, DE + 0x74, xy(5, 2));
        }
        for (unsigned y = 0; y < 4; y++) {
            g_assert_cmphex(qtest_readb(s, 0x30002 + y * 512), ==, 0x5a);
        }
        qtest_writel(s, DE + 0x9f0, 0);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 8; x++) {
                g_assert_cmphex(qtest_readb(s, 0x30000 + y * 512 + x), ==,
                    y >= 1 && y <= 2 && x >= 2 && x <= 5 ? 0x6b : 0x5a);
            }
        }
    }
    /* A native low member changes only that member of the earlier pair. */
    qtest_writeq(s, DE + 0x70, ((uint64_t)xy(2, 1) << 32) | xy(5, 2));
    qtest_writel(s, DE + 0x74, xy(3, 1));
    qtest_memset(s, 0x30200, 0x5a, 8);
    qtest_writel(s, DE + 0x9f0, 0);
    g_assert_cmphex(qtest_readl(s, 0x30200), ==, 0x5a5a6b6b);
    g_assert_cmphex(qtest_readl(s, 0x30204), ==, 0x5a5a5a5a);
    for (unsigned low_first = 0; low_first < 2; low_first++) {
        setup(s, 0, true);
        qtest_writel(s, DE + 0x70, xy(4, 0));
        qtest_writel(s, DE + 0x74, xy(5, 0));
        qtest_writel(s, DE + 0xa0, xy(1, 0));
        qtest_writew(s, 0x30001, 0x1234);
        qtest_writew(s, 0x30004, 0x5a5a);
        qtest_writeq(s, DE + 0xa8, 0x0000000200000003ULL);
        g_assert_cmphex(qtest_readw(s, 0x30004), ==, 0x5a5a);
        assert_failure(s, DE + 0x9f0, 0, 4);
        qtest_writel(s, DE + (low_first ? 0xac : 0xa8), 1);
        assert_failure(s, DE + 0x9f0, 0, 4);
        qtest_writel(s, DE + (low_first ? 0xa8 : 0xac), 1);
        qtest_writel(s, DE + 0x9f0, 0);
        g_assert_cmphex(qtest_readw(s, 0x30004), ==, 0x1234);
    }
    qtest_quit(s);
}

static void test_wide_rejected(void)
{
    const unsigned rejected[] = {
        0x2054, 0x205c, 0x20a0, 0x20a4, 0x2074, 0x20ac,
        0x2874, 0x28ac, 0x28c4, 0x29f0, 0x29f8,
        0x3000, 0x3070, 0x3078, 0x3800, 0x400,
    };
    QTestState *s = start();
    uint64_t value;
    uint32_t word;

    tile(s, 0, 0, 3);
    setup(s, 0x228, false);
    qtest_writel(s, DE + 0x70, 0);
    qtest_writel(s, DE + 0x74, 0);
    qtest_writel(s, DE + 0xa0, xy(1, 0));
    qtest_writel(s, DE + 0xd0, 0x1234abcd);
    qtest_writeq(s, 0x30000, 0x5a5a5a5a5a5a5a5aULL);
    for (unsigned i = 0; i < ARRAY_SIZE(rejected); i++) {
        assert_failure(s, RE + rejected[i], 0x1122334455667788ULL, 8);
        g_assert_cmphex(qtest_readq(s, 0x30000), ==, 0x5a5a5a5a5a5a5a5aULL);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(wide_slots); i++) {
        unsigned offset = wide_slots[i];

        g_assert_cmpint(memtx_readq(s, DE + offset, &value), !=, MEMTX_OK);
        for (unsigned width = 1; width <= 2; width *= 2) {
            assert_failure(s, DE + offset, 0x1234, width);
        }
        for (unsigned delta = 1; delta <= 4; delta *= 2) {
            assert_failure(s, DE + offset + delta, 0x1122334455667788ULL, 8);
        }
    }
    for (unsigned pair = 0; pair < 2; pair++) {
        unsigned offset = pair ? 0xa8 : 0x70;

        g_assert_cmpint(memtx_readq(s, DE + offset, &value), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readl(s, DE + offset, &word), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readl(s, DE + offset + 4, &word), !=, MEMTX_OK);
        for (unsigned delta = 1; delta <= 4; delta *= 2) {
            assert_failure(s, DE + offset + delta, 0x1122334455667788ULL, 8);
        }
    }
    /* Refused source-address pair did not overwrite its admitted first word. */
    qtest_writel(s, DE + 0x18, 0x2002f8);
    qtest_writel(s, DE + 0x70, xy(2, 0));
    qtest_writel(s, DE + 0x74, xy(2, 0));
    qtest_writel(s, DE, 0x228);
    qtest_writeq(s, 0x30000, 0x1122334455667788ULL);
    qtest_writel(s, DE + 0x9f0, 0);
    g_assert_cmphex(qtest_readl(s, 0x30008), ==, 0x55667788);
    qtest_quit(s);
}

static void test_native64_start(void)
{
    for (unsigned n = 0; n < 6; n++) {
        QTestState *s = start();
        unsigned offset = 0x20 + n * 8;
        uint64_t value = bounds(1, 2, 4, 5);

        tile(s, 0, 0, 3);
        tile(s, 0, 1, 0x400);
        setup(s, 0, false);
        qtest_writel(s, DE + 0x18, 0x2f8 | (n == 5 ? 0x100000 : 0));
        qtest_writel(s, DE + 0x1b0, 6);
        qtest_writel(s, DE + 0xd0, 0xcc);
        if (n < 5) {
            qtest_writel(s, DE + 0x10, (1U << (5 + n)) | (1U << n));
        }
        qtest_writel(s, DE + 0x70, xy(0, 0));
        qtest_writel(s, DE + 0x74, xy(5, 5));
        qtest_writeq(s, DE + offset, bounds(3, 2, 4, 5));
        for (unsigned y = 0; y < 6; y++) {
            qtest_memset(s, 0x30000 + y * 512, 0x5a, 6);
        }
        g_assert_cmpint(memtx_writeq(s, DE + 0x800 + offset, value),
                        ==, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, DE + offset), ==, value);
        for (unsigned y = 0; y < 6; y++) {
            for (unsigned x = 0; x < 6; x++) {
                g_assert_cmphex(qtest_readb(s, 0x30000 + y * 512 + x), ==,
                    x >= 1 && x < 4 && y >= 2 && y < 5 ? 0x96 : 0x5a);
            }
        }
        assert_idle(s);
        assert_failure(s, DE + 0x800 + offset, bounds(5, 2, 4, 6), 8);
        g_assert_cmphex(qtest_readq(s, DE + offset), ==, value);
        for (unsigned y = 0; y < 6; y++) {
            for (unsigned x = 0; x < 6; x++) {
                g_assert_cmphex(qtest_readb(s, 0x30000 + y * 512 + x), ==,
                    x >= 1 && x < 4 && y >= 2 && y < 5 ? 0x96 : 0x5a);
            }
        }
        qtest_writel(s, DE + 0x9f0, 0);
        for (unsigned y = 0; y < 6; y++) {
            for (unsigned x = 0; x < 6; x++) {
                g_assert_cmphex(qtest_readb(s, 0x30000 + y * 512 + x),
                               ==, 0x5a);
            }
        }
        /* Prospective bounds expose a missing tile; failure keeps old clip. */
        value = bounds(511, 2, 512, 5);
        qtest_writeq(s, DE + offset, value);
        qtest_writel(s, DE + 0x70, xy(511, 2));
        qtest_writel(s, DE + 0x74, xy(512, 4));
        for (unsigned y = 2; y < 5; y++) {
            qtest_writeb(s, 0x301ff + y * 512, 0x5a);
        }
        assert_failure(s, DE + 0x800 + offset, bounds(511, 2, 513, 5), 8);
        g_assert_cmphex(qtest_readq(s, DE + offset), ==, value);
        for (unsigned y = 2; y < 5; y++) {
            g_assert_cmphex(qtest_readb(s, 0x301ff + y * 512), ==, 0x5a);
        }
        qtest_writel(s, DE + 0x9f0, 0);
        for (unsigned y = 2; y < 5; y++) {
            g_assert_cmphex(qtest_readb(s, 0x301ff + y * 512), ==, 0x96);
        }
        qtest_quit(s);
    }
}

static void test_wide_migration(void)
{
    g_autofree char *directory = g_dir_make_tmp("crime-wide-move-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "from", NULL);
    g_autofree char *next = g_build_filename(directory, "to", NULL);
    g_autofree char *zero = g_build_filename(directory, "reset", NULL);
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *uri = g_strdup_printf("exec:cat %s", quoted);
    QTestState *from = start();
    QTestState *to = start_options("-incoming defer");
    QTestState *reset;
    uint64_t expected[RENDER_WORDS];
    uint64_t actual[RENDER_WORDS];

    tile(from, 0, 0, 3);
    setup(from, 0x228, false);
    qtest_writel(from, DE + 0xd8, 0x99aabbcc);
    qtest_writeq(from, DE + 0xd0, 0x1234abcddecafbadULL);
    qtest_writeq(from, DE + 0x60, 0x03020000cafefeedULL);
    qtest_writeq(from, DE + 0x70, ((uint64_t)xy(1, 1) << 32) | xy(2, 2));
    qtest_writeq(from, DE + 0xa8, 0x2233444455667788ULL);
    qtest_writeq(from, DE + 0x50, 0xdeadbeef11223344ULL);
    qtest_writeq(from, DE + 0x58, 0x000000009abcdef0ULL);
    for (unsigned y = 0; y < 4; y++) {
        qtest_memset(from, 0x30000 + y * 512, 0x5a, 16);
    }
    save_renderer(from, path, expected);
    g_assert_cmphex(expected[192 + 0x70 / 4], ==, xy(1, 1));
    g_assert_cmphex(expected[192 + 0x74 / 4], ==, xy(2, 2));
    g_assert_cmphex(expected[192 + 0xa8 / 4], ==, 0x22334444);
    g_assert_cmphex(expected[192 + 0xac / 4], ==, 0x55667788);
    g_assert_cmphex(expected[192 + 0xd0 / 4], ==, 0x1234abcd);
    g_assert_cmphex(expected[192 + 0x60 / 4], ==, 0x03020000);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readl(to, DE + 0x50), ==, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(to, DE + 0x58), ==, 0);
    g_assert_cmphex(qtest_readl(to, DE + 0xd8), ==, 0x99aabbcc);
    save_renderer(to, next, actual);
    for (unsigned i = 0; i < RENDER_WORDS; i++) {
        g_assert_cmphex(actual[i], ==, expected[i]);
    }
    qtest_writel(to, DE + 0x50, 0);
    qtest_writel(to, DE + 0x9f0, 0);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            g_assert_cmphex(qtest_readl(to, 0x30000 + y * 512 + x * 4), ==,
                x >= 1 && x <= 2 && y >= 1 && y <= 2 ?
                0x1234abcd : 0x5a5a5a5a);
        }
    }
    qtest_quit(from);
    qtest_quit(to);
    reset = start();
    for (unsigned i = 0; i < ARRAY_SIZE(wide_slots); i++) {
        qtest_writeq(reset, DE + wide_slots[i], 0x1122334455667788ULL);
    }
    qtest_writeq(reset, DE + 0x70, 0x135724689abcdef0ULL);
    qtest_writeq(reset, DE + 0xa8, 0x1234567887654321ULL);
    qtest_system_reset(reset);
    save_renderer(reset, zero, actual);
    for (unsigned i = 0; i < RENDER_WORDS; i++) {
        g_assert_cmphex(actual[i], ==, 0);
    }
    qtest_quit(reset);
    g_unlink(path);
    g_unlink(next);
    g_unlink(zero);
    g_rmdir(directory);
}

static void test_point(void)
{
    QTestState *s = start();

    tile(s, 0, 0, 3);
    setup(s, 0x228, false);
    qtest_writel(s, DE + 0x60, 0);
    qtest_writel(s, DE + 0x70, xy(3, 2));
    qtest_writel(s, DE + 0x74, UINT32_MAX);
    qtest_writel(s, DE + 0x78, UINT32_MAX);
    qtest_writel(s, DE + 0xd0, 0x1234abcd);
    qtest_memset(s, 0x30408, 0x5a, 12);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0xdeadbeef), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, 0x30408), ==, 0x5a5a5a5a);
    g_assert_cmphex(qtest_readl(s, 0x3040c), ==, 0x1234abcd);
    g_assert_cmphex(qtest_readl(s, 0x30410), ==, 0x5a5a5a5a);
    assert_idle(s);
    qtest_quit(s);
}

typedef struct Shape {
    unsigned x, y, width, height, bytes, bank;
    unsigned x0, y0, x1, y1;
    uint32_t primitive, draw, foreground, background, old, plane, logic;
    uint32_t stipple_mode, pattern, clip;
    uint64_t screen, scissor;
    const uint16_t *entries;
    unsigned *suppressed;
    bool indexed;
    unsigned origin_x, origin_y;
    const uint64_t *screens;
} Shape;

static Shape shape_default(void)
{
    return (Shape) {
        .width = 40, .height = 40, .bytes = 1,
        .x0 = 13, .y0 = 17, .x1 = 26, .y1 = 21,
        .primitive = 0x01000000, .draw = 0x2f8,
        .foreground = 0xcccccccc, .background = 0x33333333,
        .old = 0xaaaaaaaa, .plane = UINT32_MAX, .logic = 6,
        .pattern = 0x96a53c81,
    };
}

static bool inside_bounds(uint64_t box, int x, int y)
{
    return x >= (box >> 48) && y >= ((box >> 32) & 0xffff) &&
           x < ((box >> 16) & 0xffff) && y < (box & 0xffff);
}

static void reference_fragment(const Shape *c, uint32_t *pixels,
                               int x, int y, uint32_t source)
{
    unsigned bits = c->indexed ? 8 : c->bytes * 8;
    unsigned first_byte = c->indexed ? 3 : 0;
    uint32_t mask = UINT32_MAX >> (32 - bits);
    uint32_t old, result, plane;
    unsigned index;
    bool screen_pass = !c->clip;

    if ((c->draw & (1U << 20)) && !inside_bounds(c->scissor, x, y)) {
        return;
    }
    x += c->origin_x;
    y += c->origin_y;
    if (x < 0 || y < 0 || x >= 2048 || y >= 2048 ||
        x < c->x || y < c->y || x >= c->x + c->width ||
        y >= c->y + c->height) {
        return;
    }
    for (unsigned n = 0; n < 5; n++) {
        if (c->clip & (1U << (5 + n))) {
            uint64_t box = c->screens ? c->screens[n] : c->screen;

            screen_pass |= inside_bounds(box, x, y) ==
                           !!(c->clip & (1U << n));
        }
    }
    if (!screen_pass) {
        return;
    }
    if (c->entries) {
        uint32_t enabled = 0;
        uint32_t effective = c->draw & 0x80 ? c->plane & mask : mask;
        unsigned tile_index = (y / 128) * 16 + (x * c->bytes) / 512;

        for (unsigned b = first_byte; b < c->bytes; b++) {
            unsigned lane = 3 - ((x * c->bytes + b) & 3);

            if (c->draw & (1U << (3 + lane))) {
                enabled |= 0xffU << ((c->bytes - 1 - b) * 8);
            }
        }
        if (!(effective & enabled)) {
            return;
        }
        if (!(c->entries[tile_index] & 0x8000)) {
            (*c->suppressed)++;
            return;
        }
    }
    index = (y - c->y) * c->width + x - c->x;
    old = pixels[index];
    result = c->draw & 0x200 ? rop_result(c->logic, source, old, bits) : source;
    plane = c->draw & 0x80 ? c->plane & mask : mask;
    result = (old & ~plane) | (result & plane);
    if (!c->indexed) {
        result &= mask;
    }
    for (unsigned b = first_byte; b < c->bytes; b++) {
        unsigned lane = 3 - ((x * c->bytes + b) & 3);
        uint32_t byte = 0xffU << ((c->bytes - 1 - b) * 8);

        if (!(c->draw & (1U << (3 + lane)))) {
            result = (result & ~byte) | (old & byte);
        }
    }
    pixels[index] = result;
}

/* Direct sample rank avoids sharing an incremental counter with the model. */
static bool reference_color(const Shape *c, unsigned sample, uint32_t *color)
{
    unsigned opcode = c->primitive >> 24;
    bool active = (opcode == 1 && (c->draw & (1U << 19))) ||
                  (opcode == 3 && (c->draw & (1U << 18)));
    bool bit = true;

    if (active) {
        unsigned first = (c->stipple_mode >> 24) & 31;
        unsigned last = (c->stipple_mode >> 16) & 31;
        unsigned repeat = (c->stipple_mode >> 8) & 255;
        unsigned period = (c->stipple_mode & 255) + 1;
        unsigned rank = (sample + repeat) / period;
        unsigned position = (first + rank) % (last + 1);

        bit = !!(c->pattern & (1U << (31 - position)));
    }
    *color = bit ? c->foreground : c->background;
    return bit || (c->draw & (1U << 17));
}

static void reference_shape(const Shape *c, uint32_t *pixels)
{
    unsigned opcode = c->primitive >> 24;
    uint32_t color;

    if (opcode == 0) {
        reference_fragment(c, pixels, c->x0, c->y0, c->foreground);
    } else if (opcode == 1) {
        int dx = (int)c->x1 - (int)c->x0;
        int dy = (int)c->y1 - (int)c->y0;
        unsigned count = MAX(abs(dx), abs(dy));
        unsigned samples = count + 1 - !!(c->primitive & (1U << 18));
        unsigned thickness = MAX(1, ((c->primitive & 0xffff) + 31) / 32);
        int lower = -(int)((thickness - 1) / 2);

        for (unsigned i = 0; i < samples; i++) {
            int x = count ? ((int64_t)count * c->x0 + (int64_t)i * dx) /
                            count : c->x0;
            int y = count ? ((int64_t)count * c->y0 + (int64_t)i * dy) /
                            count : c->y0;

            if (!reference_color(c, i, &color)) {
                continue;
            }
            for (unsigned k = 0; k < thickness; k++) {
                if (!count) {
                    for (unsigned j = 0; j < thickness; j++) {
                        reference_fragment(c, pixels, x + lower + (int)j,
                                           y + lower + (int)k, color);
                    }
                } else {
                    reference_fragment(c, pixels,
                        x + (abs(dx) < abs(dy) ? lower + (int)k : 0),
                        y + (abs(dx) >= abs(dy) ? lower + (int)k : 0), color);
                }
            }
        }
    } else {
        unsigned width = abs((int)c->x1 - (int)c->x0) + 1;
        unsigned height = abs((int)c->y1 - (int)c->y0) + 1;
        int dx = c->primitive & (1U << 16) ? -1 : 1;
        int dy = c->primitive & (1U << 17) ? 1 : -1;

        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                if (reference_color(c, y * width + x, &color)) {
                    reference_fragment(c, pixels, c->x0 + x * dx,
                                       c->y0 + y * dy, color);
                }
            }
        }
    }
}

static void shape_memory(QTestState *s, const Shape *c,
                          const uint32_t *expected)
{
    for (unsigned y = 0; y < c->height; y++) {
        for (unsigned x = 0; x < c->width;) {
            unsigned n = MIN(c->width - x,
                (512 - ((c->x + x) * c->bytes) % 512) / c->bytes);
            g_autofree uint8_t *buffer = g_malloc(n * c->bytes);
            uint64_t address = pixel_address(c->bank, c->x + x, c->y + y,
                                             c->bytes);

            if (expected) {
                qtest_memread(s, address, buffer, n * c->bytes);
            }
            for (unsigned i = 0; i < n; i++) {
                uint32_t value = c->old;

                if (expected) {
                    value = 0;
                    for (unsigned b = 0; b < c->bytes; b++) {
                        value = (value << 8) | buffer[i * c->bytes + b];
                    }
                    if (value != expected[y * c->width + x + i]) {
                        g_test_message("pixel (%u,%u), primitive 0x%x",
                                       c->x + x + i, c->y + y, c->primitive);
                    }
                    g_assert_cmphex(value, ==,
                                    expected[y * c->width + x + i]);
                } else {
                    for (unsigned b = 0; b < c->bytes; b++) {
                        buffer[i * c->bytes + b] =
                            value >> ((c->bytes - b - 1) * 8);
                    }
                }
            }
            if (!expected) {
                qtest_memwrite(s, address, buffer, n * c->bytes);
            }
            x += n;
        }
    }
}

static void shape_stage(QTestState *s, const Shape *c)
{
    unsigned depth = c->bytes == 4 ? 2 : c->bytes == 2 ? 1 : 0;

    setup(s, (c->indexed ? 0x200 : modes[depth]) | (c->bank << 10), false);
    qtest_writel(s, DE + 0x58, xy(c->origin_x, c->origin_y));
    qtest_writel(s, DE, UINT32_MAX);
    qtest_writel(s, DE + 0xa0, UINT32_MAX);
    qtest_writel(s, DE + 0xa8, UINT32_MAX);
    qtest_writel(s, DE + 0xac, UINT32_MAX);
    qtest_writel(s, DE + 0x60, c->primitive);
    qtest_writel(s, DE + 0x70, xy(c->x0, c->y0));
    qtest_writel(s, DE + 0x74, xy(c->x1, c->y1));
    qtest_writel(s, DE + 0x78, UINT32_MAX);
    qtest_writel(s, DE + 0x18, c->draw);
    qtest_writel(s, DE + 0xd0, c->foreground);
    qtest_writel(s, DE + 0xd8, c->background);
    qtest_writel(s, DE + 0x1b0, c->logic);
    qtest_writel(s, DE + 0x1b8, c->plane);
    qtest_writel(s, DE + 0xc0, c->stipple_mode);
    qtest_writel(s, DE + 0xc4, c->pattern);
    qtest_writel(s, DE + 0x10, c->clip);
    qtest_writeq(s, DE + 0x48, c->scissor);
    for (unsigned n = 0; n < 5; n++) {
        qtest_writeq(s, DE + 0x20 + n * 8,
                     c->screens ? c->screens[n] : c->screen);
    }
}

static uint32_t *shape_reference(const Shape *c)
{
    uint32_t mask = UINT32_MAX >> (32 - c->bytes * 8);
    uint32_t *expected = g_new(uint32_t, c->width * c->height);

    for (unsigned i = 0; i < c->width * c->height; i++) {
        expected[i] = c->old & mask;
    }
    reference_shape(c, expected);
    return expected;
}

static void shape_run(QTestState *s, const Shape *c)
{
    g_autofree uint32_t *expected = shape_reference(c);

    map_region(s, c->bank, c->bytes, c->x, c->y,
               c->x + c->width - 1, c->y + c->height - 1);
    shape_memory(s, c, NULL);
    shape_stage(s, c);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0x12345678), ==, MEMTX_OK);
    shape_memory(s, c, expected);
    g_assert_cmphex(qtest_readl(s, DE + 0xc0), ==, c->stipple_mode);
    g_assert_cmphex(qtest_readl(s, DE + 0xc4), ==, c->pattern);
    assert_idle(s);
}

static void test_point_geometry(void)
{
    const unsigned positions[][2] = {
        { 0, 0 }, { 2047, 2047 }, { 17, 23 }, { 0, 2047 }, { 2047, 0 },
    };
    QTestState *s = start();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned depth = 0; depth < 3; depth++) {
            for (unsigned i = 0; i < ARRAY_SIZE(positions); i++) {
                Shape c = shape_default();

                c.bank = bank;
                c.bytes = 1U << depth;
                c.x0 = positions[i][0];
                c.y0 = positions[i][1];
                c.x = MIN(c.x0, 2045);
                c.y = MIN(c.y0, 2045);
                c.width = c.height = 3;
                c.x1 = c.y1 = 65535;
                c.primitive = 0x0007ffff;
                c.draw |= 0xe0000;
                c.stipple_mode = UINT32_MAX;
                shape_run(s, &c);
            }
        }
    }
    qtest_quit(s);
}

static void test_line_geometry(void)
{
    const int deltas[][2] = {
        { 9, 4 }, { 4, 9 }, { -9, 4 }, { -4, 9 },
        { 9, -4 }, { 4, -9 }, { -9, -4 }, { -4, -9 },
        { 8, 4 }, { 4, 8 }, { 8, 0 }, { 0, 8 }, { 8, 8 },
        { 0, 0 }, { 1, 0 },
    };
    QTestState *s = start();

    for (unsigned i = 0; i < ARRAY_SIZE(deltas); i++) {
        for (unsigned reverse = 0; reverse < 2; reverse++) {
            for (unsigned skip = 0; skip < 2; skip++) {
                Shape c = shape_default();

                c.x0 = 20 + (reverse ? deltas[i][0] : 0);
                c.y0 = 20 + (reverse ? deltas[i][1] : 0);
                c.x1 = 20 + (reverse ? 0 : deltas[i][0]);
                c.y1 = 20 + (reverse ? 0 : deltas[i][1]);
                c.primitive |= (skip << 18) | ((i % 4) << 16);
                shape_run(s, &c);
            }
        }
    }
    qtest_quit(s);
}

static void test_line_width(void)
{
    const unsigned widths[] = { 0, 1, 31, 32, 33, 64, 65, 65535 };
    const unsigned ends[][2] = {
        { 23, 21 }, { 21, 23 }, { 23, 23 }, { 20, 20 },
    };
    QTestState *s = start();

    for (unsigned i = 0; i < ARRAY_SIZE(widths); i++) {
        for (unsigned j = 0; j < ARRAY_SIZE(ends); j++) {
            Shape c = shape_default();

            c.x0 = c.y0 = 20;
            c.x1 = ends[j][0];
            c.y1 = ends[j][1];
            c.primitive |= widths[i];
            c.draw |= 1U << 20;
            c.scissor = bounds(0, 0, 40, 40);
            shape_run(s, &c);
            c.primitive |= 1U << 18;
            shape_run(s, &c);
        }
    }
    Shape masked = shape_default();

    masked.primitive |= 65535;
    masked.plane = 0;
    shape_run(s, &masked);
    for (unsigned edge = 0; edge < 2; edge++) {
        Shape c = shape_default();

        c.x = c.y = edge ? 2040 : 0;
        c.width = c.height = 8;
        c.x0 = c.x + 1;
        c.x1 = c.x + 4;
        c.y0 = c.y1 = edge ? 2047 : 0;
        c.primitive |= 65;
        shape_run(s, &c);
    }
    qtest_quit(s);
}

static void test_stipple_phase(void)
{
    const unsigned repeats[][2] = { { 0, 0 }, { 0, 1 }, { 1, 1 },
                                    { 0, 255 }, { 254, 255 }, { 255, 255 } };
    QTestState *s = start();

    for (unsigned n = 0; n < ARRAY_SIZE(repeats); n++) {
        for (unsigned opaque = 0; opaque < 2; opaque++) {
            Shape c = shape_default();

            c.height = 26;
            c.x0 = 1;
            c.y0 = 1;
            c.x1 = 38;
            c.y1 = 24;
            c.primitive = 0x03020000;
            c.draw |= (1U << 18) | (opaque << 17);
            c.stipple_mode = (3U << 24) | (5U << 16) |
                             (repeats[n][0] << 8) | repeats[n][1];
            shape_run(s, &c);
            /* The next command restarts K/R rather than continuing rotation. */
            g_autofree uint32_t *twice = shape_reference(&c);

            reference_shape(&c, twice);
            qtest_writel(s, DE + 0x9f0, 0xfedcba98);
            shape_memory(s, &c, twice);
        }
    }
    for (unsigned direction = 0; direction < 4; direction++) {
        Shape c = shape_default();

        c.x0 = direction & 1 ? 38 : 1;
        c.x1 = direction & 1 ? 1 : 38;
        c.y0 = direction & 2 ? 1 : 4;
        c.y1 = direction & 2 ? 4 : 1;
        c.primitive = 0x03000000 | (direction << 16);
        c.draw |= 0x60000;
        c.stipple_mode = (7U << 24) | (21U << 16) | 0x101;
        shape_run(s, &c);
    }
    qtest_quit(s);
}

static void test_stipple_inclusive(void)
{
    Shape c = shape_default();
    QTestState *s = start();

    c.width = 12;
    c.height = 8;
    c.x0 = 3;
    c.y0 = c.y1 = 4;
    c.x1 = c.x0 + 4;
    c.primitive = 0x03020000;
    c.draw = 0x400f8;
    c.stipple_mode = 0x001f0000;
    c.pattern = UINT32_MAX;
    c.foreground = 0x39;
    shape_run(s, &c);
    g_assert_cmphex(qtest_readb(s, pixel_address(0, 7, 4, 1)), ==, 0x39);
    g_assert_cmphex(qtest_readb(s, pixel_address(0, 8, 4, 1)), ==, 0xaa);
    c.draw = 0xf8;
    shape_run(s, &c);
    qtest_quit(s);
}

static void test_stipple_pipeline(void)
{
    QTestState *s = start();

    for (unsigned depth = 0; depth < 3; depth++) {
        for (unsigned op = 0; op < 16; op++) {
            for (unsigned primitive = 0; primitive < 3; primitive++) {
                for (unsigned opaque = 0; opaque < 2; opaque++) {
                    Shape c = shape_default();
                    unsigned opcode = primitive == 2 ? 3 : primitive;

                    c.bytes = 1U << depth;
                    c.width = 12;
                    c.height = 8;
                    c.x0 = 2;
                    c.y0 = 3;
                    c.x1 = 9;
                    c.y1 = 4;
                    c.primitive = (opcode << 24) | (1U << 17);
                    c.logic = op;
                    c.draw = 0x200 | 0x80 | (op << 3) |
                             0xc0000 | (opaque << 17);
                    c.plane = 0x5a3cc3a5;
                    c.stipple_mode = (1U << 24) | (7U << 16);
                    shape_run(s, &c);
                    c.draw = (c.draw & ~0x78U) | 0x78;
                    shape_run(s, &c);
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_stipple_clipping(void)
{
    QTestState *s = start();

    for (unsigned n = 0; n < 5; n++) {
        for (unsigned inside = 0; inside < 2; inside++) {
            for (unsigned scissor = 0; scissor < 2; scissor++) {
                Shape c = shape_default();

                c.x0 = 2;
                c.x1 = 37;
                c.y0 = c.y1 = 12;
                c.primitive |= 65;
                c.draw |= (1U << 19) | (scissor << 20);
                c.stipple_mode = (3U << 24) | (9U << 16) | 0x101;
                c.clip = (1U << (5 + n)) | (inside << n);
                c.screen = bounds(12, 9, 23, 16);
                c.scissor = bounds(8, 10, 31, 15);
                shape_run(s, &c);
            }
        }
    }
    qtest_quit(s);
}

static void test_stipple_state(void)
{
    QTestState *s = start();
    uint64_t value;

    qtest_writeq(s, 0x30000, 0x123456789abcdef0ULL);
    qtest_writel(s, DE + 0xc0, 0x031f01ff);
    qtest_writel(s, DE + 0xc4, 0x96a53c81);
    g_assert_cmphex(qtest_readl(s, DE + 0xc0), ==, 0x031f01ff);
    g_assert_cmphex(qtest_readl(s, DE + 0xc4), ==, 0x96a53c81);
    qtest_writeq(s, DE + 0xc0, 0x0715020369a53c81ULL);
    g_assert_cmphex(qtest_readl(s, DE + 0xc0), ==, 0x07150203);
    g_assert_cmphex(qtest_readl(s, DE + 0xc4), ==, 0x69a53c81);
    g_assert_cmphex(qtest_readq(s, 0x30000), ==, 0x123456789abcdef0ULL);
    for (unsigned offset = 0xc0; offset <= 0xc4; offset += 4) {
        g_assert_cmpint(memtx_readq(s, DE + offset, &value), !=, MEMTX_OK);
        assert_failure(s, DE + 0x800 + offset, 0x1122334455667788ULL,
                       offset == 0xc0 ? 2 : 8);
        for (unsigned delta = 1; delta <= 4; delta *= 2) {
            if (offset + delta == 0xc8) {
                g_assert_cmpint(memtx_writeq(s, DE + 0xc8,
                    0x123456789abcdef0ULL), !=, MEMTX_OK);
            } else {
                assert_failure(s, DE + offset + delta,
                               0x123456789abcdef0ULL, 8);
            }
        }
        assert_failure(s, DE + offset, 0x1234, 2);
        g_assert_cmphex(qtest_readl(s, DE + 0xc0), ==, 0x07150203);
        g_assert_cmphex(qtest_readl(s, DE + 0xc4), ==, 0x69a53c81);
    }
    g_assert_cmpint(memtx_writel(s, DE + 0xc8, 0x12345678), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, DE + 0xc0), ==, 0x07150203);
    g_assert_cmphex(qtest_readl(s, DE + 0xc4), ==, 0x69a53c81);
    g_assert_cmphex(qtest_readq(s, 0x30000), ==, 0x123456789abcdef0ULL);
    qtest_quit(s);
}

static void test_stipple_masks_colors(void)
{
    QTestState *s = start();

    for (unsigned depth = 0; depth < 3; depth++) {
        for (unsigned bit = 0; bit < (8U << depth); bit++) {
            Shape c = shape_default();

            c.bytes = 1U << depth;
            c.width = 12;
            c.height = 8;
            c.x0 = 2;
            c.y0 = 3;
            c.x1 = 9;
            c.y1 = 3;
            c.draw |= 0xa0000;
            c.stipple_mode = 0x00070000;
            c.foreground = c.background = 0x55555555;
            c.logic = 3;
            c.plane = 1U << bit;
            shape_run(s, &c);
        }
        for (unsigned all_one = 0; all_one < 2; all_one++) {
            for (unsigned opaque = 0; opaque < 2; opaque++) {
                Shape c = shape_default();

                c.bytes = 1U << depth;
                c.width = 12;
                c.height = 8;
                c.x0 = 2;
                c.y0 = 3;
                c.x1 = 9;
                c.y1 = 3;
                c.draw |= (1U << 19) | (opaque << 17);
                c.stipple_mode = 0x001f0000;
                c.pattern = all_one ? UINT32_MAX : 0;
                c.foreground = 0;
                c.background = 0x1234abcd;
                c.logic = 3;
                shape_run(s, &c);
                c.background = 0;
                c.foreground = 0x1234abcd;
                shape_run(s, &c);
            }
        }
    }
    qtest_quit(s);
}

static void test_stipple_invalid(void)
{
    const uint32_t bad_modes[] = {
        1U << 31, 1U << 21, 0x08070000, 0x03050201,
    };
    QTestState *s = start();
    Shape c = shape_default();
    g_autofree uint32_t *unchanged = g_new(uint32_t, 1600);

    tile(s, 0, 0, 3);
    for (unsigned i = 0; i < 1600; i++) {
        unchanged[i] = 0xaa;
    }
    c.draw |= 1U << 19;
    for (unsigned i = 0; i < ARRAY_SIZE(bad_modes); i++) {
        for (unsigned hidden = 0; hidden < 3; hidden++) {
            Shape bad = c;

            bad.stipple_mode = bad_modes[i];
            if (hidden == 0) {
                bad.plane = 0;
            } else if (hidden == 1) {
                bad.draw |= 1U << 20;
                bad.scissor = bounds(1, 1, 1, 1);
            } else {
                bad.pattern = 0;
                bad.draw &= ~(1U << 17);
            }
            shape_memory(s, &bad, NULL);
            shape_stage(s, &bad);
            assert_failure(s, DE + 0x9f0, 0x1234, 4);
            shape_memory(s, &bad, unchanged);
            g_assert_cmphex(qtest_readl(s, DE + 0xc0), ==, bad.stipple_mode);
        }
    }
    c.x0 = c.x1 = 3;
    c.y0 = c.y1 = 4;
    c.primitive = 0x01040000;
    c.stipple_mode = UINT32_MAX;
    shape_stage(s, &c);
    assert_failure(s, DE + 0x9f0, 0, 4);
    c = shape_default();
    /* Known controls and invalid stipple words are inert when inapplicable. */
    c.stipple_mode = UINT32_MAX;
    c.draw = 0x600f8;
    shape_run(s, &c);
    c.primitive = 0;
    c.draw = 0xe00f8;
    shape_run(s, &c);
    c.primitive = 0x03020000;
    c.x0 = 2;
    c.y0 = 2;
    c.x1 = 5;
    c.y1 = 4;
    c.draw = 0xa00f8;
    shape_run(s, &c);
    for (unsigned flag = 0; flag < 4; flag++) {
        Shape bad = shape_default();

        bad.draw |= flag == 0 ? 0x200000 : flag == 1 ? 0x100 :
                    flag == 2 ? 1 : 0x10000;
        bad.plane = 0;
        shape_memory(s, &bad, NULL);
        shape_stage(s, &bad);
        assert_failure(s, DE + 0x9f0, 0, 4);
        shape_memory(s, &bad, unchanged);
    }
    c = shape_default();
    c.x0 = 2048;
    c.draw |= 1U << 20;
    c.scissor = 0;
    c.plane = 0;
    shape_stage(s, &c);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    c.primitive = 0;
    shape_stage(s, &c);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    /* A stippled framebuffer copy remains outside this source-color policy. */
    setup(s, 0, true);
    qtest_writel(s, DE + 0x18, 0x2402f8);
    qtest_writel(s, DE + 0x70, 0);
    qtest_writel(s, DE + 0x74, 0);
    assert_failure(s, DE + 0x9f0, 0, 4);
    qtest_quit(s);
}

static void test_stipple_accesses(void)
{
    QTestState *s = start();

    for (unsigned hide = 0; hide < 8; hide++) {
        Shape c = shape_default();

        qtest_writeq(s, RE + 0x1000, 0x8003000000000000ULL);
        qtest_writeq(s, RE + 0x1020, 0x0000840000000000ULL);
        c.x0 = 510;
        c.y0 = 127;
        c.x1 = 512;
        c.y1 = 128;
        c.draw = 0x800f8;
        c.foreground = 0xb7;
        c.stipple_mode = 0x00020000;
        c.pattern = hide == 0 ? 0xc0000000 : UINT32_MAX;
        if (hide >= 1 && hide <= 5) {
            unsigned n = hide - 1;

            c.clip = (1U << (5 + n)) | (1U << n);
            c.screen = bounds(1, 1, 512, 129);
        } else if (hide == 6) {
            c.draw |= 1U << 20;
            c.scissor = bounds(1, 1, 512, 129);
        } else if (hide == 7) {
            c.plane = 0;
        }
        qtest_writew(s, 0x3fffe, 0x5a5a);
        shape_stage(s, &c);
        qtest_writel(s, MTE, 0x12345678);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        g_assert_cmphex(qtest_readw(s, 0x3fffe), ==,
                       hide == 7 ? 0x5a5a : 0xb7b7);
        g_assert_cmphex(qtest_readl(s, MTE), ==, 0x12345678);
        /* Make exactly the formerly hidden invalid fragment accessible. */
        c.clip = 0;
        c.draw &= ~(1U << 20);
        c.pattern = UINT32_MAX;
        c.plane = UINT32_MAX;
        shape_stage(s, &c);
        qtest_writew(s, 0x3fffe, 0x5a5a);
        assert_failure(s, DE + 0x9f0, 0, 4);
        g_assert_cmphex(qtest_readw(s, 0x3fffe), ==, 0x5a5a);
        g_assert_cmphex(qtest_readl(s, MTE), ==, 0x12345678);
    }
    /* Distinct emitted coordinates must not alias one enabled backing byte. */
    tile(s, 0, 0, 3);
    tile(s, 0, 1, 3);
    setup(s, 0, false);
    qtest_writel(s, DE + 0x60, 0x01000000);
    qtest_writel(s, DE + 0x70, xy(0, 0));
    qtest_writel(s, DE + 0x74, xy(512, 0));
    qtest_writeb(s, 0x30000, 0x5a);
    assert_failure(s, DE + 0x9f0, 0, 4);
    g_assert_cmphex(qtest_readb(s, 0x30000), ==, 0x5a);
    qtest_quit(s);
}

static void test_stipple_null(void)
{
    Shape c = shape_default();
    QTestState *s = start();
    g_autofree uint32_t *expected = NULL;

    c.draw |= 0xa0000;
    c.primitive |= 33;
    c.stipple_mode = 0x03110102;
    map_region(s, 0, 1, 0, 0, 39, 39);
    shape_stage(s, &c);
    shape_memory(s, &c, NULL);
    for (unsigned i = 0; i < 5; i++) {
        qtest_writel(s, DE + 0x1f0, 0x12345678U + i);
        assert_idle(s);
    }
    expected = g_new(uint32_t, c.width * c.height);
    for (unsigned i = 0; i < c.width * c.height; i++) {
        expected[i] = c.old & 0xff;
    }
    shape_memory(s, &c, expected);
    for (unsigned pass = 0; pass < 2; pass++) {
        reference_shape(&c, expected);
        qtest_writel(s, DE + 0x9f0, 0x12345678);
        shape_memory(s, &c, expected);
        for (unsigned i = 0; i < 5; i++) {
            assert_idle(s);
        }
        shape_memory(s, &c, expected);
    }
    assert_failure(s, DE + 0x9f0, 0x123456789abcdef0ULL, 8);
    shape_memory(s, &c, expected);
    assert_failure(s, DE + 0x1f8, 1, 4);
    shape_memory(s, &c, expected);
    qtest_quit(s);
}

static void test_line_shared_endpoint(void)
{
    Shape c = shape_default();
    QTestState *s = start();
    g_autofree uint32_t *expected = NULL;

    c.x0 = 3;
    c.y0 = 4;
    c.x1 = 11;
    c.y1 = 8;
    shape_run(s, &c);
    expected = shape_reference(&c);
    c.x0 = c.x1;
    c.y0 = c.y1;
    c.x1 = 16;
    c.y1 = 12;
    reference_shape(&c, expected);
    shape_stage(s, &c);
    qtest_writel(s, DE + 0x9f0, 0);
    shape_memory(s, &c, expected);
    /* The shared endpoint receives the XOR of two distinct commands. */
    g_assert_cmphex(qtest_readb(s, pixel_address(0, 11, 8, 1)), ==, 0xaa);
    qtest_quit(s);
}

static uint64_t shape_parameter(const Shape *c, unsigned offset)
{
    switch (offset) {
    case 0: case 0xa0: case 0xa8: case 0xac: case 0x78:
        return UINT32_MAX;
    case 8:
        return modes[c->bytes == 4 ? 2 : c->bytes == 2 ? 1 : 0] |
               (c->bank << 10);
    case 0x10:
         return c->clip;
    case 0x18:
         return c->draw;
    case 0x20: case 0x28: case 0x30: case 0x38: case 0x40:
        return c->screen;
    case 0x48:
         return c->scissor;
    case 0x50: case 0x58:
         return 0;
    case 0x60:
         return c->primitive;
    case 0x70:
         return xy(c->x0, c->y0);
    case 0x74:
         return xy(c->x1, c->y1);
    case 0xc0:
         return c->stipple_mode;
    case 0xc4:
         return c->pattern;
    case 0xd0:
         return c->foreground;
    case 0xd8:
         return c->background;
    case 0x1b0:
         return c->logic;
    case 0x1b8:
         return c->plane;
    default:
        g_assert_not_reached();
    }
}

static void test_stipple_start_forms(void)
{
    g_autofree char *directory = g_dir_make_tmp("crime-shape-go-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "state", NULL);

    for (unsigned r = 0; r < ARRAY_SIZE(registers); r++) {
        const Register *reg = &registers[r];
        Shape base = shape_default();
        Shape prospective;
        uint64_t old, value;

        base.width = 12;
        base.height = 8;
        base.x0 = 2;
        base.y0 = 3;
        base.x1 = 8;
        base.y1 = 3;
        base.draw |= 0xa0000;
        base.stipple_mode = 0x01050000;
        prospective = base;
        old = shape_parameter(&base, reg->offset);
        switch (reg->offset) {
        case 8:
            prospective.bytes = 2;
            break;
        case 0x10:
            prospective.clip = 0x21;
            prospective.screen = bounds(4, 2, 7, 5);
            break;
        case 0x18:
            prospective.draw &= ~(1U << 17);
            break;
        case 0x20: case 0x28: case 0x30: case 0x38: case 0x40: {
            unsigned n = (reg->offset - 0x20) / 8;

            prospective.clip = (1U << (5 + n)) | (1U << n);
            prospective.screen = bounds(4, 2, 7, 5);
            break;
        }
        case 0x48:
            prospective.draw |= 1U << 20;
            prospective.scissor = bounds(4, 2, 7, 5);
            break;
        case 0x60:
            prospective.primitive |= 1U << 18;
            break;
        case 0x70:
            prospective.x0 = 3;
            prospective.y0 = 4;
            break;
        case 0x74:
            prospective.x1 = 9;
            prospective.y1 = 4;
            break;
        case 0xc0:
            prospective.stipple_mode = 0x03070101;
            break;
        case 0xc4:
            prospective.pattern = 0x69a53c81;
            break;
        case 0xd0:
            prospective.foreground = 0x1234abcd;
            break;
        case 0xd8:
            prospective.background = 0x87654321;
            break;
        case 0x1b0:
            prospective.logic = 12;
            break;
        case 0x1b8:
            prospective.plane = 0x55aa55aa;
            break;
        default:
            break;
        }
        value = shape_parameter(&prospective, reg->offset);
        if (reg->offset == 0 || reg->offset == 0x78 || reg->offset == 0xa0 ||
            reg->offset == 0xa8 || reg->offset == 0xac) {
            value = 0x13572468U ^ reg->offset;
        }
        if (reg->offset == 0x50 || reg->offset == 0x58) {
            old = 1;
        }
        for (unsigned fail = 0; fail < 2; fail++) {
            QTestState *q = start();
            Shape staged = fail ? base : prospective;
            g_autofree uint32_t *expected = NULL;
            uint64_t words[RENDER_WORDS];
            uint64_t readback;
            unsigned other_window = reg->offset == 0x50 ? 0x58 : 0x50;

            map_region(q, 0, staged.bytes, 0, 0, 11, 7);
            shape_memory(q, &staged, NULL);
            shape_stage(q, &staged);
            memtx_write(q, DE + reg->offset, old, reg->width);
            qtest_writel(q, MTE, 0x12345678);
            if (fail) {
                qtest_writel(q, DE + other_window, 0x80000000);
                assert_failure(q, DE + 0x800 + reg->offset, value, reg->width);
                expected = g_new(uint32_t, staged.width * staged.height);
                for (unsigned i = 0; i < staged.width * staged.height; i++) {
                    expected[i] = staged.old & 0xff;
                }
                shape_memory(q, &staged, expected);
                if (reg->readable) {
                    memtx_read(q, DE + reg->offset, &readback, reg->width);
                    g_assert_cmphex(readback, ==, old);
                }
                qtest_writel(q, DE + other_window, 0);
                if (reg->offset != 0x50 && reg->offset != 0x58) {
                    reference_shape(&base, expected);
                    qtest_writel(q, DE + 0x9f0, 0x12345678);
                    shape_memory(q, &base, expected);
                }
            } else {
                expected = shape_reference(&prospective);
                g_assert_cmpint(memtx_write(q, DE + 0x800 + reg->offset,
                                            value, reg->width), ==, MEMTX_OK);
                shape_memory(q, &prospective, expected);
                assert_idle(q);
            }
            g_assert_cmphex(qtest_readl(q, MTE), ==, 0x12345678);
            save_renderer(q, path, words);
            g_assert_cmphex(words[192 + reg->offset / 4], ==,
                           fail ? old : value);
            qtest_quit(q);
            g_unlink(path);
        }
    }
    g_rmdir(directory);
}

static void test_stipple_migration(void)
{
    g_autofree char *directory =
        g_dir_make_tmp("crime-stipple-move-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "state", NULL);
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *uri = g_strdup_printf("exec:cat %s", quoted);
    QTestState *from = start();
    QTestState *to = start_options("-incoming defer");
    QTestState *control = start();
    Shape c = shape_default();
    g_autofree uint32_t *expected = NULL;
    uint64_t words[RENDER_WORDS];

    c.bytes = 2;
    c.bank = 1;
    c.x = 250;
    c.y = 124;
    c.width = 16;
    c.height = 8;
    c.x0 = 252;
    c.y0 = 125;
    c.x1 = 260;
    c.y1 = 129;
    c.primitive |= 65;
    c.draw |= 0xa0000;
    c.plane = 0x5aa5;
    c.stipple_mode = 0x030b0101;
    for (unsigned i = 0; i < 2; i++) {
        QTestState *q = i ? control : from;

        map_region(q, 1, 2, 250, 124, 265, 131);
        shape_memory(q, &c, NULL);
        shape_stage(q, &c);
        qtest_writel(q, DE + 0xc0, UINT32_MAX);
    }
    save_renderer(from, path, words);
    g_assert_cmphex(words[192 + 0xc0 / 4], ==, UINT32_MAX);
    g_assert_cmphex(words[192 + 0xc4 / 4], ==, c.pattern);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    for (unsigned i = 0; i < 2; i++) {
        QTestState *q = i ? control : to;

        g_assert_cmphex(qtest_readl(q, DE + 0xc0), ==, UINT32_MAX);
        g_assert_cmphex(qtest_readl(q, DE + 0xc4), ==, c.pattern);
        assert_failure(q, DE + 0x9f0, 0, 4);
        qtest_writel(q, DE + 0xc0, c.stipple_mode);
        qtest_writel(q, DE + 0x9f0, 0x12345678);
        expected = shape_reference(&c);
        shape_memory(q, &c, expected);
        g_clear_pointer(&expected, g_free);
    }
    qtest_system_reset(to);
    g_assert_cmphex(qtest_readl(to, DE + 0xc0), ==, 0);
    g_assert_cmphex(qtest_readl(to, DE + 0xc4), ==, 0);
    shape_run(to, &c);
    qtest_quit(from);
    qtest_quit(to);
    qtest_quit(control);
    g_unlink(path);
    g_rmdir(directory);
}

static void test_shape_tile_banks(void)
{
    QTestState *s = start();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned depth = 0; depth < 3; depth++) {
            Shape c = shape_default();

            c.bytes = 1U << depth;
            c.bank = bank;
            c.x = 512 / c.bytes - 5;
            c.y = 124;
            c.width = 12;
            c.height = 8;
            c.x0 = c.x + 1;
            c.y0 = 125;
            c.x1 = c.x + 9;
            c.y1 = 129;
            c.primitive |= 65;
            c.draw |= 0xa0000;
            c.stipple_mode = 0x030b0101;
            shape_run(s, &c);
        }
    }
    qtest_quit(s);
}

static void test_shape_bank_decoder(void)
{
    QTestState *s = start_profile("legacy-software", true, true, "");

    for (unsigned i = 0; i < 8; i++) {
        qtest_writeq(s, CRIME + 0x208 + i * 8, 0x100 | (i * 4));
    }
    qtest_memset(s, 0x30000, 0x6c, 8);
    qtest_memset(s, 0x08030000, 0x5a, 8);
    qtest_writeq(s, CRIME + 0x208, 0x104);
    qtest_writeq(s, CRIME + 0x210, 0x100);
    tile(s, 0, 0, 3);
    setup(s, 0, false);
    qtest_writel(s, DE + 0x60, 0x01000000);
    qtest_writel(s, DE + 0x18, 0xa00f8);
    qtest_writel(s, DE + 0x70, xy(1, 0));
    qtest_writel(s, DE + 0x74, xy(3, 0));
    qtest_writel(s, DE + 0xd0, 0xb7);
    qtest_writel(s, DE + 0xd8, 0x29);
    qtest_writel(s, DE + 0xc0, 0x00030000);
    qtest_writel(s, DE + 0xc4, 0x90000000);
    qtest_writel(s, DE + 0x9f0, 0);
    g_assert_cmphex(qtest_readl(s, 0x30000), ==, 0x5ab72929);
    g_assert_cmphex(qtest_readl(s, 0x40030000), ==, 0x5ab72929);
    g_assert_cmphex(qtest_readl(s, 0x80030000), ==, 0x5ab72929);
    qtest_writel(s, DE + 0x60, 0);
    qtest_writel(s, DE + 0x70, xy(4, 0));
    qtest_writel(s, DE + 0xd0, 0x83);
    qtest_writel(s, DE + 0x9f0, 0);
    g_assert_cmphex(qtest_readb(s, 0x30004), ==, 0x83);
    qtest_writeq(s, CRIME + 0x208, 0x100);
    qtest_writeq(s, CRIME + 0x210, 0x104);
    g_assert_cmphex(qtest_readl(s, 0x30000), ==, 0x6c6c6c6c);
    g_assert_cmphex(qtest_readl(s, 0x08030000), ==, 0x5ab72929);
    g_assert_cmphex(qtest_readb(s, 0x08030004), ==, 0x83);
    tile(s, 0, 0, 0x803);
    qtest_writeq(s, CRIME + 0x210, 0x100);
    assert_failure(s, DE + 0x9f0, 0, 4);
    qtest_writeq(s, CRIME + 0x210, 0x104);
    g_assert_cmphex(qtest_readb(s, 0x08030004), ==, 0x83);
    qtest_quit(s);
}

static void test_tiled_validity_fill(void)
{
    QTestState *s = start();

    qtest_writeq(s, RE + 0x1000, 0x8003000900000000ULL);
    qtest_memset(s, 0x301fe, 0x5a, 3);
    qtest_memset(s, 0x90000, 0x6c, 8);
    qtest_memset(s, 0, 0x83, 8);
    setup(s, 0, false);
    qtest_writel(s, DE + 0xd0, 0xb7);
    qtest_writel(s, DE + 0x70, xy(511, 0));
    g_assert_cmpint(memtx_writel(s, DE + 0x874, xy(513, 0)), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readb(s, 0x301fe), ==, 0x5a);
    g_assert_cmphex(qtest_readb(s, 0x301ff), ==, 0xb7);
    g_assert_cmphex(qtest_readb(s, 0x30200), ==, 0x5a);
    g_assert_cmphex(qtest_readq(s, 0x90000), ==, 0x6c6c6c6c6c6c6c6cULL);
    g_assert_cmphex(qtest_readq(s, 0), ==, 0x8383838383838383ULL);
    g_assert_cmphex(qtest_readq(s, RE + 0x1000), ==, 0x8003000900000000ULL);
    assert_idle(s);
    qtest_quit(s);
}

typedef struct ValidityLog {
    QTestState *q;
    char *directory;
    char *path;
    GPtrArray *records;
} ValidityLog;

static ValidityLog validity_start(bool banked, const char *extra)
{
    ValidityLog v = { 0 };
    g_autofree char *options = NULL;

    v.directory = g_dir_make_tmp("crime-validity-XXXXXX", NULL);
    v.path = g_build_filename(v.directory, "device.txt", NULL);
    v.records = g_ptr_array_new_with_free_func(g_free);
    options = g_strdup_printf("-D %s %s", v.path, extra);
    v.q = start_profile("legacy-software", banked, true, options);
    qtest_writeq(v.q, CRIME + 0x18, 1U << 4);
    qtest_set_irq_in(v.q, "/machine/peripheral/crime", "mace", 4, 1);
    return v;
}

static void validity_record(ValidityLog *v, const Shape *c, unsigned count)
{
    unsigned opcode = c->primitive >> 24;
    const char *name = opcode == 0 ? "point" :
                       opcode == 1 ? "line" : "rectangle";

    if (count) {
        g_ptr_array_add(v->records, g_strdup_printf(
            "sgi-crime: %s suppressed %u invalid destination fragments",
            name, count));
    }
}

static void validity_finish(ValidityLog *v)
{
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    unsigned count = 0;

    qtest_quit(v->q);
    g_assert_true(g_file_get_contents(v->path, &contents, NULL, NULL));
    lines = g_strsplit(contents, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        if (strstr(lines[i], "invalid destination fragments")) {
            g_assert_cmpuint(count, <, v->records->len);
            g_assert_cmpstr(lines[i], ==, g_ptr_array_index(v->records, count));
            count++;
        }
    }
    g_assert_cmpuint(count, ==, v->records->len);
    g_ptr_array_free(v->records, true);
    g_unlink(v->path);
    g_rmdir(v->directory);
    g_free(v->path);
    g_free(v->directory);
}

static void validity_entries(uint16_t entries[256], unsigned bank)
{
    for (unsigned i = 0; i < 256; i++) {
        entries[i] = 0x8000 | page_for(bank, i);
    }
    entries[1] &= 0x7fff;
    entries[17] &= 0x7fff;
}

static void validity_program(QTestState *q, unsigned bank,
                              const uint16_t entries[256])
{
    for (unsigned word = 0; word < 64; word++) {
        uint64_t value = 0;

        for (unsigned lane = 0; lane < 4; lane++) {
            value |= (uint64_t)entries[word * 4 + lane] << (48 - lane * 16);
        }
        qtest_writeq(q, RE + 0x1000 + bank * 512 + word * 8, value);
    }
}

static void validity_words(QTestState *q, unsigned bank,
                            const uint16_t entries[256])
{
    for (unsigned word = 0; word < 64; word++) {
        uint64_t value = qtest_readq(q, RE + 0x1000 + bank * 512 + word * 8);

        for (unsigned lane = 0; lane < 4; lane++) {
            g_assert_cmphex((value >> (48 - lane * 16)) & 0xffff, ==,
                           entries[word * 4 + lane]);
        }
    }
}

static uint32_t *validity_reference(Shape *c, unsigned *count)
{
    *count = 0;
    c->suppressed = count;
    return shape_reference(c);
}

static unsigned validity_run(ValidityLog *v, Shape *c, uint16_t entries[256])
{
    unsigned count;
    g_autofree uint32_t *expected = NULL;
    uint64_t error = qtest_readq(v->q, CRIME + 0x250);
    uint64_t address = qtest_readq(v->q, CRIME + 0x258);

    c->entries = entries;
    expected = validity_reference(c, &count);
    validity_program(v->q, c->bank, entries);
    shape_memory(v->q, c, NULL);
    qtest_writeq(v->q, 0, 0x8383838383838383ULL);
    shape_stage(v->q, c);
    g_assert_cmpint(memtx_writel(v->q, DE + 0x9f0, 0x12345678), ==, MEMTX_OK);
    shape_memory(v->q, c, expected);
    validity_record(v, c, count);
    validity_words(v->q, c->bank, entries);
    g_assert_cmphex(qtest_readq(v->q, 0), ==, 0x8383838383838383ULL);
    g_assert_cmphex(qtest_readq(v->q, CRIME + 0x250), ==, error);
    g_assert_cmphex(qtest_readq(v->q, CRIME + 0x258), ==, address);
    g_assert_cmphex(qtest_readq(v->q, CRIME + 0x10), ==, 1U << 4);
    g_assert_cmphex(qtest_readq(v->q, CRIME + 0x18), ==, 1U << 4);
    g_assert_cmphex(qtest_readl(v->q, STATUS) & ~0x3ffffU, ==, 0x1e000000);
    g_assert_cmphex(qtest_readl(v->q, DE + 0xc0), ==, c->stipple_mode);
    g_assert_cmphex(qtest_readl(v->q, DE + 0xc4), ==, c->pattern);
    return count;
}

static void test_validity_banks_depths(void)
{
    ValidityLog v = validity_start(false, "");

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned depth = 0; depth < 3; depth++) {
            uint16_t entries[256];
            Shape c = shape_default();
            unsigned tile_width = 512 / (1U << depth);

            validity_entries(entries, bank);
            c.bank = bank;
            c.bytes = 1U << depth;
            c.x = tile_width - 2;
            c.y = 127;
            c.width = tile_width + 4;
            c.height = 2;
            c.x0 = c.x;
            c.x1 = c.x + c.width - 1;
            c.y0 = 127;
            c.y1 = 128;
            c.primitive = 0x03020000;
            validity_run(&v, &c, entries);
            for (unsigned position = 0; position < 3; position++) {
                c.primitive = 0;
                c.x0 = position == 0 ? c.x : position == 1 ?
                       tile_width : c.x + c.width - 1;
                c.y0 = 128;
                c.x1 = c.y1 = 65535;
                validity_run(&v, &c, entries);
            }
        }
    }
    validity_finish(&v);
}

static void test_validity_geometry_phase(void)
{
    const int delta[][2] = {
        { 18, 8 }, { 8, 18 }, { -18, 8 }, { -8, 18 },
        { 18, -8 }, { 8, -18 }, { -18, -8 }, { -8, -18 },
        { 0, 0 },
    };
    const unsigned widths[] = { 0, 33, 65 };
    ValidityLog v = validity_start(false, "");
    uint16_t entries[256];

    validity_entries(entries, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(delta); i++) {
        for (unsigned reverse = 0; reverse < 2; reverse++) {
            for (unsigned width = 0; width < ARRAY_SIZE(widths); width++) {
                for (unsigned skip = 0; skip < 2; skip++) {
                    Shape c = shape_default();

                    c.x = 492;
                    c.y = 108;
                    c.width = c.height = 40;
                    c.x0 = 512 + (reverse ? delta[i][0] : 0);
                    c.y0 = 128 + (reverse ? delta[i][1] : 0);
                    c.x1 = 512 + (reverse ? 0 : delta[i][0]);
                    c.y1 = 128 + (reverse ? 0 : delta[i][1]);
                    c.primitive |= widths[width] | (skip << 18);
                    c.draw |= 0xa0000;
                    c.stipple_mode = 0x030b0102;
                    validity_run(&v, &c, entries);
                }
            }
        }
    }
    for (unsigned direction = 0; direction < 4; direction++) {
        Shape c = shape_default();

        c.x = 508;
        c.y = 125;
        c.width = 520;
        c.height = 6;
        c.x0 = direction & 1 ? 1027 : 508;
        c.x1 = direction & 1 ? 508 : 1027;
        c.y0 = direction & 2 ? 125 : 130;
        c.y1 = direction & 2 ? 130 : 125;
        c.primitive = 0x03000000 | (direction << 16);
        c.draw |= 0x60000;
        c.stipple_mode = 0x030b0102;
        validity_run(&v, &c, entries);
        /* Command-local phase restarts even after a complete invalid tile. */
        validity_run(&v, &c, entries);
    }
    validity_finish(&v);
}

static void test_validity_clips_masks(void)
{
    ValidityLog v = validity_start(false, "");
    uint16_t entries[256];

    validity_entries(entries, 0);
    for (unsigned hide = 0; hide < 8; hide++) {
        for (unsigned inside = 0; inside < (hide < 5 ? 2 : 1); inside++) {
            Shape c = shape_default();

            c.x = 510;
            c.y = 2;
            c.width = 516;
            c.height = 2;
            c.x0 = 510;
            c.x1 = 1025;
            c.y0 = c.y1 = 2;
            c.primitive = 0x03020000;
            if (hide < 5) {
                c.clip = (1U << (5 + hide)) | (inside << hide);
                c.screen = inside ? bounds(510, 1, 512, 4) :
                                    bounds(512, 1, 1024, 4);
            } else if (hide == 5) {
                c.draw |= 1U << 20;
                c.scissor = bounds(1, 1, 512, 4);
            } else if (hide == 6) {
                c.plane = 0;
            } else {
                c.draw |= 1U << 18;
                c.stipple_mode = 0x001f0000;
                c.pattern = 0;
            }
            g_assert_cmpuint(validity_run(&v, &c, entries), ==, 0);
            if (hide < 5) {
                c.screen = inside ? bounds(510, 1, 513, 4) :
                                    bounds(513, 1, 1024, 4);
            } else if (hide == 5) {
                c.scissor = bounds(1, 1, 513, 4);
            } else if (hide == 6) {
                c.plane = UINT32_MAX;
                c.x1 = 512;
            } else {
                c.pattern = 0xe0000000;
                c.x1 = 512;
            }
            g_assert_cmpuint(validity_run(&v, &c, entries), ==, 1);
        }
    }
    for (unsigned depth = 0; depth < 3; depth++) {
        for (unsigned op = 0; op < 16; op++) {
            Shape c = shape_default();

            c.bytes = 1U << depth;
            c.x = 512 / c.bytes - 2;
            c.width = 512 / c.bytes + 4;
            c.y = 2;
            c.height = 2;
            c.x0 = c.x;
            c.x1 = c.x + c.width - 1;
            c.y0 = c.y1 = 2;
            c.primitive = 0x03020000;
            c.draw = 0x200 | 0x80 | (op << 3);
            c.logic = op;
            c.plane = 0x5a3cc3a5;
            validity_run(&v, &c, entries);
            c.draw |= 0x78;
            validity_run(&v, &c, entries);
        }
    }
    validity_finish(&v);
}

static void test_validity_faults(void)
{
    for (unsigned fault = 0; fault < 4; fault++) {
        bool banked = fault >= 2;
        ValidityLog v = validity_start(banked, "");
        uint16_t entries[256];
        Shape c = shape_default();
        g_autofree uint32_t *unchanged = NULL;
        g_autofree uint32_t *expected = NULL;
        unsigned count;

        if (banked) {
            for (unsigned i = 0; i < 8; i++) {
                qtest_writeq(v.q, CRIME + 0x208 + i * 8, 0x100 | (i * 4));
            }
            qtest_writeq(v.q, CRIME + 0x210, fault == 2 ? 0x100 : 4);
        }
        validity_entries(entries, 0);
        entries[2] = 0x8000 |
                     (fault == 0 ? 0x400 : fault == 1 ? 0x1fc0 : 0x800);
        c.x = 510;
        c.y = 2;
        c.width = 516;
        c.height = 2;
        c.x0 = 510;
        c.x1 = 1025;
        c.y0 = c.y1 = 2;
        c.primitive = 0x03020000;
        c.entries = entries;
        validity_program(v.q, 0, entries);
        shape_memory(v.q, &c, NULL);
        shape_stage(v.q, &c);
        unchanged = g_new(uint32_t, c.width * c.height);
        for (unsigned i = 0; i < c.width * c.height; i++) {
            unchanged[i] = 0xaa;
        }
        assert_failure(v.q, DE + 0x8d0, 0x6b, 4);
        shape_memory(v.q, &c, unchanged);
        validity_words(v.q, 0, entries);
        /* The next valid operation exposes the retained foreground. */
        entries[2] = 0x8000 | page_for(0, 2);
        validity_program(v.q, 0, entries);
        expected = validity_reference(&c, &count);
        g_assert_cmpint(memtx_writel(v.q, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(v.q, &c, expected);
        validity_record(&v, &c, count);
        validity_finish(&v);
    }
    /* Invalid payload aliases do not participate in valid-byte uniqueness. */
    ValidityLog v = validity_start(false, "");
    uint16_t entries[256];
    Shape c = shape_default();

    validity_entries(entries, 0);
    entries[1] = 3;
    c.width = 1025;
    c.height = 1;
    c.x0 = c.y0 = c.y1 = 0;
    c.x1 = 1024;
    c.primitive = 0x03020000;
    validity_run(&v, &c, entries);
    entries[2] = 0x8003;
    validity_program(v.q, 0, entries);
    shape_memory(v.q, &c, NULL);
    shape_stage(v.q, &c);
    assert_failure(v.q, DE + 0x9f0, 0, 4);
    g_assert_cmphex(qtest_readq(v.q, 0x30000), ==, 0xaaaaaaaaaaaaaaaaULL);
    g_assert_cmphex(qtest_readq(v.q, 0x40000), ==, 0xaaaaaaaaaaaaaaaaULL);
    validity_finish(&v);
}

static void test_validity_semantics(void)
{
    const uint32_t bad_draw[] = { 0x200000, 0x100, 1, 0x10000 };
    ValidityLog v = validity_start(false, "");
    uint16_t entries[256] = { 0 };
    Shape c = shape_default();

    for (unsigned i = 0; i < 256; i++) {
        entries[i] = 0x1fc0;
    }
    c.width = c.height = 8;
    c.x0 = 2;
    c.y0 = 3;
    c.x1 = 6;
    c.y1 = 4;
    validity_program(v.q, 0, entries);
    qtest_writeq(v.q, 0x30000, 0x8383838383838383ULL);
    for (unsigned i = 0; i < ARRAY_SIZE(bad_draw); i++) {
        shape_stage(v.q, &c);
        qtest_writel(v.q, DE + 0x18, c.draw | bad_draw[i]);
        qtest_writel(v.q, DE + 0x1b8, 0);
        assert_failure(v.q, DE + 0x9f0, 0, 4);
        g_assert_cmphex(qtest_readq(v.q, 0x30000), ==, 0x8383838383838383ULL);
    }
    shape_stage(v.q, &c);
    qtest_writel(v.q, DE + 0x18, c.draw | 0x80000);
    qtest_writel(v.q, DE + 0xc0, 0x08070000);
    assert_failure(v.q, DE + 0x9f0, 0, 4);
    shape_stage(v.q, &c);
    qtest_writel(v.q, DE + 0x58, 0x80000000);
    assert_failure(v.q, DE + 0x9f0, 0, 4);
    shape_stage(v.q, &c);
    assert_failure(v.q, DE + 0x874, 0x1122334455667788ULL, 8);
    assert_failure(v.q, DE + 0x9f8, 0, 4);
    g_assert_cmphex(qtest_readq(v.q, 0x30000), ==, 0x8383838383838383ULL);
    validity_finish(&v);
}

static void test_validity_copies(void)
{
    const unsigned rops[] = { 3, 5, 0 };

    for (unsigned invalid = 1; invalid <= 3; invalid++) {
        for (unsigned op = 0; op < ARRAY_SIZE(rops); op++) {
            for (unsigned null = 0; null < 2; null++) {
                ValidityLog v = validity_start(false, "");
                uint16_t source[256], dest[256];

                validity_entries(source, 0);
                validity_entries(dest, 1);
                source[1] = (invalid & 1 ? 0 : 0x8000) | page_for(0, 1);
                dest[1] = (invalid & 2 ? 0 : 0x8000) | page_for(1, 1);
                validity_program(v.q, 0, source);
                validity_program(v.q, 1, dest);
                setup(v.q, 0, true);
                qtest_writel(v.q, DE + 8, 0x400);
                qtest_writel(v.q, DE + 0x1b0, rops[op]);
                qtest_writel(v.q, DE + 0x70, xy(511, 0));
                qtest_writel(v.q, DE + 0x74, xy(512, 0));
                qtest_writel(v.q, DE + 0xa0, xy(511, 0));
                qtest_writeb(v.q, pixel_address(0, 511, 0, 1), 0xcc);
                qtest_writeb(v.q, pixel_address(0, 512, 0, 1), 0x33);
                qtest_writeb(v.q, pixel_address(1, 511, 0, 1), 0x5a);
                qtest_writeb(v.q, pixel_address(1, 512, 0, 1), 0x6c);
                assert_failure(v.q, DE + (null ? 0x9f0 : 0x874),
                               null ? 0x12345678 : xy(512, 0), 4);
                g_assert_cmphex(qtest_readb(v.q, pixel_address(1, 511, 0, 1)),
                                ==, 0x5a);
                g_assert_cmphex(qtest_readb(v.q, pixel_address(1, 512, 0, 1)),
                                ==, 0x6c);
                /* A zero mask skips both source and destination accesses. */
                qtest_writel(v.q, DE + 0x1b8, 0);
                g_assert_cmpint(memtx_writel(v.q, DE + 0x9f0, 0), ==, MEMTX_OK);
                qtest_writel(v.q, DE + 0x1b8, UINT32_MAX);
                qtest_writel(v.q, DE + 0x18, 0x3002f8);
                qtest_writeq(v.q, DE + 0x48, bounds(1, 0, 512, 1));
                g_assert_cmpint(memtx_writel(v.q, DE + 0x9f0, 0), ==, MEMTX_OK);
                g_assert_cmphex(qtest_readb(v.q, pixel_address(1, 511, 0, 1)),
                    ==, rops[op] == 3 ? 0xcc : rops[op] == 0 ? 0 : 0x5a);
                g_assert_cmphex(qtest_readb(v.q, pixel_address(1, 512, 0, 1)),
                                ==, 0x6c);
                validity_finish(&v);
            }
        }
    }
    for (unsigned copy = 0; copy < 2; copy++) {
        ValidityLog v = validity_start(false, "");
        uint16_t entries[256];

        validity_entries(entries, 0);
        validity_program(v.q, 0, entries);
        mte_setup(v.q, copy, 0, 0);
        qtest_writel(v.q, MTE + 0x20, xy(511, 0));
        qtest_writel(v.q, MTE + 0x28, xy(512, 0));
        qtest_writel(v.q, MTE + 0x30, xy(511, 0));
        qtest_writel(v.q, MTE + 0x38, xy(512, 0));
        qtest_writeb(v.q, 0x301ff, 0x5a);
        assert_failure(v.q, MTE + 0x870, 0, 4);
        g_assert_cmphex(qtest_readb(v.q, 0x301ff), ==, 0x5a);
        validity_finish(&v);
    }
}

static void test_validity_retirement(void)
{
    for (unsigned r = 0; r < ARRAY_SIZE(registers); r++) {
        const Register *reg = &registers[r];
        ValidityLog v = validity_start(false, "");
        g_autofree char *path = g_build_filename(v.directory, "state", NULL);
        uint16_t entries[256];
        Shape c = shape_default();
        g_autofree uint32_t *expected = NULL;
        uint64_t value, words[RENDER_WORDS];
        unsigned count;

        validity_entries(entries, 0);
        c.x = 510;
        c.y = 1;
        c.width = 516;
        c.height = 4;
        c.x0 = 510;
        c.x1 = 1025;
        c.y0 = 2;
        c.y1 = 3;
        c.primitive = 0x03020000;
        c.draw |= 0x60000;
        c.stipple_mode = 0x030b0102;
        c.screen = c.scissor = bounds(510, 1, 1026, 4);
        if (reg->offset >= 0x20 && reg->offset <= 0x40) {
            unsigned n = (reg->offset - 0x20) / 8;

            c.clip = (1U << (5 + n)) | (1U << n);
        } else if (reg->offset == 0x48) {
            c.draw |= 1U << 20;
        }
        c.entries = entries;
        expected = validity_reference(&c, &count);
        validity_program(v.q, 0, entries);
        shape_memory(v.q, &c, NULL);
        shape_stage(v.q, &c);
        value = shape_parameter(&c, reg->offset);
        if (reg->offset == 0 || reg->offset == 0x78 ||
            reg->offset == 0xa0 || reg->offset == 0xa8 || reg->offset == 0xac) {
            value = 0x13572468U ^ reg->offset;
        }
        if (reg->offset == 0x50 || reg->offset == 0x58) {
            qtest_writel(v.q, DE + reg->offset, 1);
        } else if (reg->width == 8) {
            qtest_writeq(v.q, DE + reg->offset, bounds(510, 1, 512, 4));
        } else if (reg->offset == 0x60) {
            qtest_writel(v.q, DE + 0x60, 0x02000000);
        } else if (reg->offset == 0xd0) {
            qtest_writel(v.q, DE + 0xd0, 0x76543210);
        } else if (reg->offset == 0x1b0) {
            qtest_writel(v.q, DE + 0x1b0, 3);
        }
        g_assert_cmpint(memtx_write(v.q, DE + 0x800 + reg->offset,
                                    value, reg->width), ==, MEMTX_OK);
        shape_memory(v.q, &c, expected);
        validity_record(&v, &c, count);
        save_renderer(v.q, path, words);
        g_assert_cmphex(words[192 + reg->offset / 4], ==, value);
        g_unlink(path);
        validity_finish(&v);
    }
    ValidityLog v = validity_start(false, "");
    uint16_t entries[256];
    Shape c = shape_default();
    unsigned count;
    g_autofree uint32_t *expected = NULL;

    validity_entries(entries, 0);
    c.x = 510;
    c.y = 1;
    c.width = 516;
    c.height = 2;
    c.x0 = 510;
    c.x1 = 1025;
    c.y0 = c.y1 = 2;
    c.primitive = 0x03020000;
    c.draw |= 0x60000;
    c.stipple_mode = 0x030b0102;
    c.entries = entries;
    expected = validity_reference(&c, &count);
    validity_program(v.q, 0, entries);
    shape_memory(v.q, &c, NULL);
    shape_stage(v.q, &c);
    qtest_writel(v.q, DE + 0x9f0, 0x11223344);
    validity_record(&v, &c, count);
    shape_memory(v.q, &c, expected);
    for (unsigned i = 0; i < 4; i++) {
        qtest_writel(v.q, DE + 0x1f0, 0x12345678 + i);
        qtest_writel(v.q, DE + 0x1f8, 0);
        g_assert_cmphex(qtest_readl(v.q, STATUS) & ~0x3ffffU, ==, 0x1e000000);
        shape_memory(v.q, &c, expected);
    }
    count = 0;
    reference_shape(&c, expected);
    qtest_writel(v.q, DE + 0x9f0, 0x55667788);
    validity_record(&v, &c, count);
    shape_memory(v.q, &c, expected);
    /* All-invalid GO publishes the new logical state without RAM access. */
    for (unsigned i = 0; i < 256; i++) {
        entries[i] = 0x1fc0;
    }
    validity_program(v.q, 0, entries);
    c.logic = 12;
    g_clear_pointer(&expected, g_free);
    expected = validity_reference(&c, &count);
    shape_memory(v.q, &c, NULL);
    qtest_writel(v.q, DE + 0x9b0, 12);
    g_assert_cmphex(qtest_readl(v.q, DE + 0x1b0), ==, 12);
    shape_memory(v.q, &c, expected);
    validity_record(&v, &c, count);
    for (unsigned i = 0; i < 256; i++) {
        entries[i] = 0x8000 | page_for(0, i);
    }
    validity_program(v.q, 0, entries);
    g_clear_pointer(&expected, g_free);
    expected = validity_reference(&c, &count);
    qtest_writel(v.q, DE + 0x9f0, 0);
    shape_memory(v.q, &c, expected);
    validity_record(&v, &c, count);
    validity_finish(&v);
}

static void test_validity_migration(void)
{
    ValidityLog from = validity_start(false, "");
    ValidityLog to = validity_start(false, "-incoming defer");
    ValidityLog control = validity_start(false, "");
    g_autofree char *path = g_build_filename(from.directory, "state", NULL);
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *uri = g_strdup_printf("exec:cat %s", quoted);
    uint16_t entries[256], zero[256] = { 0 };
    uint64_t words[RENDER_WORDS];
    Shape c = shape_default();

    validity_entries(entries, 2);
    c.bank = 2;
    c.bytes = 4;
    c.x = 126;
    c.y = 125;
    c.width = 132;
    c.height = 6;
    c.x0 = 126;
    c.x1 = 257;
    c.y0 = 125;
    c.y1 = 130;
    c.primitive = 0x03020000;
    c.draw |= 0x60000;
    c.stipple_mode = 0x030b0102;
    c.plane = 0x5a3cc3a5;
    c.entries = entries;
    for (unsigned i = 0; i < 2; i++) {
        QTestState *q = i ? control.q : from.q;

        validity_program(q, 2, entries);
        shape_memory(q, &c, NULL);
        shape_stage(q, &c);
    }
    save_renderer(from.q, path, words);
    qtest_qmp_assert_success(to.q,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to.q, "completed");
    for (unsigned i = 0; i < 2; i++) {
        ValidityLog *v = i ? &control : &to;
        unsigned count;
        g_autofree uint32_t *expected = NULL;

        validity_words(v->q, 2, entries);
        g_assert_cmphex(qtest_readl(v->q, DE + 0xc0), ==, c.stipple_mode);
        g_assert_cmphex(qtest_readl(v->q, DE + 0xc4), ==, c.pattern);
        c.pattern = 0x69a53c81;
        expected = validity_reference(&c, &count);
        qtest_writel(v->q, DE + 0x8c4, c.pattern);
        shape_memory(v->q, &c, expected);
        validity_record(v, &c, count);
        g_assert_cmphex(qtest_readl(v->q, DE + 0xc4), ==, c.pattern);
        g_assert_cmphex(qtest_readq(v->q, CRIME + 0x10), ==, 1U << 4);
        g_assert_cmphex(qtest_readq(v->q, CRIME + 0x18), ==, 1U << 4);
        g_assert_cmphex(qtest_readq(v->q, CRIME + 0x250), ==, 0);
        g_assert_cmphex(qtest_readq(v->q, CRIME + 0x258), ==, 0);
        c.pattern = 0x96a53c81;
    }
    qtest_system_reset(to.q);
    validity_words(to.q, 2, zero);
    qtest_writeq(to.q, CRIME + 0x18, 1U << 4);
    c.pattern = 0x69a53c81;
    c.entries = zero;
    unsigned count;
    g_autofree uint32_t *expected = validity_reference(&c, &count);

    shape_memory(to.q, &c, NULL);
    qtest_writeq(to.q, 0, 0x8383838383838383ULL);
    shape_stage(to.q, &c);
    qtest_writel(to.q, DE + 0x9f0, 0);
    shape_memory(to.q, &c, expected);
    g_assert_cmphex(qtest_readq(to.q, 0), ==, 0x8383838383838383ULL);
    validity_words(to.q, 2, zero);
    validity_record(&to, &c, count);
    for (unsigned i = 0; i < 256; i++) {
        entries[i] = 0x8000 | page_for(2, i);
    }
    validity_run(&to, &c, entries);
    g_unlink(path);
    validity_finish(&from);
    validity_finish(&to);
    validity_finish(&control);
}

static void test_validity_profiles(void)
{
    const char *profiles[] = { "1.5", "legacy-software" };

    for (unsigned i = 0; i < ARRAY_SIZE(profiles); i++) {
        g_autofree char *directory =
            g_dir_make_tmp("crime-validity-mode-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(directory, "device.txt", NULL);
        g_autofree char *options = g_strdup_printf("-D %s", path);
        g_autofree char *contents = NULL;
        QTestState *q = start_profile(profiles[i], false, i == 0, options);

        qtest_memset(q, 0, 0x83, 8);
        qtest_memset(q, 0x30000, 0x5a, 8);
        setup(q, 0, false);
        g_assert_cmpint(memtx_writel(q, DE + 0x9f0, 0), !=, MEMTX_OK);
        g_assert_cmphex(qtest_readq(q, 0), ==, 0x8383838383838383ULL);
        g_assert_cmphex(qtest_readq(q, 0x30000), ==, 0x5a5a5a5a5a5a5a5aULL);
        qtest_quit(q);
        g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
        g_assert_null(strstr(contents, "invalid destination fragments"));
        g_unlink(path);
        g_rmdir(directory);
    }
}

/* Restore a paused qtest source after observing its migration state. */
static void save_paired_renderer(QTestState *s, const char *path,
                                 uint64_t words[RENDER_WORDS])
{
    int64_t deadline;

    save_renderer(s, path, words);
    deadline = g_get_monotonic_time() +
               SGI_MIGRATION_WAIT_MIN_S * G_TIME_SPAN_SECOND;
    /* The source can report completion before leaving finish-migrate. */
    for (;;) {
        g_autoptr(QDict) state =
            qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");
        const char *status = qdict_get_str(state, "status");

        if (g_str_equal(status, "postmigrate")) {
            break;
        }
        g_assert_cmpstr(status, ==, "finish-migrate");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    }
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    qtest_qmp_assert_success(s, "{'execute':'stop'}");
}

static const unsigned paired_start_offsets[] = { 0x70, 0xa8, 0xc0 };

static void test_paired_start_shapes(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-paired-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *control_path = g_build_filename(dir, "control", NULL);

    for (unsigned pair = 0; pair < ARRAY_SIZE(paired_start_offsets); pair++) {
        unsigned offset = paired_start_offsets[pair];

        for (unsigned depth = 0; depth < 3; depth++) {
            for (unsigned kind = 0; kind < 3; kind++) {
                Shape c = shape_default();
                QTestState *s = start(), *control = start();
                uint64_t value, words[RENDER_WORDS], expected[RENDER_WORDS];
                g_autofree uint32_t *pixels = NULL;

                c.width = 12;
                c.height = 8;
                c.bytes = 1U << depth;
                c.x0 = 2;
                c.y0 = 1;
                c.x1 = 8;
                c.y1 = 5;
                c.primitive = kind == 2 ? 0x03020000 : kind << 24;
                c.stipple_mode = 0x01050000;
                c.pattern = 0xa53c96e1;
                c.draw |= kind == 1 ? 0xa0000 : kind == 2 ? 0x60000 : 0;
                c.plane = 0x5ac33ca5;
                c.draw |= 1U << 20;
                c.scissor = bounds(1, 1, 10, 7);
                value = pair == 0 ? ((uint64_t)xy(c.x0, c.y0) << 32) |
                                     xy(c.x1, c.y1) :
                        pair == 1 ? 0x1357246889abcdefULL :
                                    ((uint64_t)c.stipple_mode << 32) |
                                    c.pattern;
                pixels = shape_reference(&c);
                for (unsigned peer = 0; peer < 2; peer++) {
                    QTestState *q = peer ? control : s;

                    map_region(q, 0, c.bytes, 0, 0, 11, 7);
                    shape_memory(q, &c, NULL);
                    shape_stage(q, &c);
                    qtest_writel(q, DE + offset, pair == 0 ? xy(1, 2) : 2);
                    qtest_writel(q, DE + offset + 4,
                                 pair == 0 ? xy(4, 3) : 3);
                }
                g_assert_cmpint(memtx_writeq(s, DE + 0x800 + offset, value),
                                ==, MEMTX_OK);
                qtest_writeq(control, DE + offset, value);
                qtest_writel(control, DE + 0x9f0, 0);
                shape_memory(s, &c, pixels);
                shape_memory(control, &c, pixels);
                assert_idle(s);
                qtest_writel(s, DE + 0x1f0, 0);
                qtest_writel(s, DE + 0x1f8, 0);
                shape_memory(s, &c, pixels);
                save_paired_renderer(s, path, words);
                save_paired_renderer(control, control_path, expected);
                g_assert_cmpmem(words, sizeof(words),
                                expected, sizeof(expected));
                g_assert_cmphex(words[192 + offset / 4], ==, value >> 32);
                g_assert_cmphex(words[192 + offset / 4 + 1], ==,
                                (uint32_t)value);
                qtest_quit(s);
                qtest_quit(control);
            }
        }
    }
    g_unlink(path);
    g_unlink(control_path);
    g_rmdir(dir);
}

static void test_paired_start_rollback(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-paired-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "before", NULL);
    g_autofree char *after = g_build_filename(dir, "after", NULL);

    for (unsigned pair = 0; pair < ARRAY_SIZE(paired_start_offsets); pair++) {
        unsigned offset = paired_start_offsets[pair];

        for (unsigned fault = 0; fault < 3; fault++) {
            QTestState *s = start();
            Shape c = shape_default();
            uint64_t words[RENDER_WORDS], expected[RENDER_WORDS], value;
            g_autofree uint32_t *pixels = NULL;

            c.width = 12;
            c.height = 8;
            c.x0 = 2;
            c.y0 = 1;
            c.x1 = 8;
            c.y1 = 5;
            if (!pair) {
                c.primitive = 0x03020000;
            }
            c.draw |= 0xa0000;
            c.stipple_mode = 0x01050000;
            map_region(s, 0, 1, 0, 0, 11, 7);
            shape_memory(s, &c, NULL);
            shape_stage(s, &c);
            value = pair == 0 ? ((uint64_t)xy(3, 2) << 32) | xy(9, 6) :
                    pair == 1 ? 0x1357246889abcdefULL : 0x0206000096a53c81ULL;
            if (fault == 0) {
                qtest_writel(s, DE + 0x50, 1);
            } else if (pair == 0) {
                value = fault == 1 ? ((uint64_t)xy(2048, 2) << 32) |
                                        xy(9, 6) :
                                     ((uint64_t)xy(3, 2) << 32) | xy(2048, 6);
            } else if (pair == 2 && fault == 1) {
                value |= 1ULL << 63;
            } else {
                /* A valid tile payload outside the fixture's populated RAM. */
                tile(s, 0, 0, 0x400);
            }
            save_paired_renderer(s, path, expected);
            assert_failure(s, DE + 0x800 + offset, value, 8);
            pixels = g_new(uint32_t, c.width * c.height);
            for (unsigned i = 0; i < c.width * c.height; i++) {
                pixels[i] = c.old & 0xff;
            }
            /* Inspect backing RAM even when the programmed tile is bad. */
            for (unsigned y = 0; y < c.height; y++) {
                for (unsigned x = 0; x < c.width; x++) {
                    g_assert_cmphex(qtest_readb(s, 0x30000 + y * 512 + x),
                                    ==, c.old & 0xff);
                }
            }
            save_paired_renderer(s, after, words);
            g_assert_cmpmem(words, sizeof(words), expected, sizeof(expected));
            qtest_writel(s, DE + 0x50, 0);
            tile(s, 0, 0, 3);
            reference_shape(&c, pixels);
            qtest_writel(s, DE + 0x9f0, 0);
            shape_memory(s, &c, pixels);
            qtest_quit(s);
        }
    }
    g_unlink(path);
    g_unlink(after);
    g_rmdir(dir);
}

static void test_paired_start_copy(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-paired-copy-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "before", NULL);
    g_autofree char *after = g_build_filename(dir, "after", NULL);

    for (unsigned pair = 0; pair < ARRAY_SIZE(paired_start_offsets); pair++) {
        unsigned offset = paired_start_offsets[pair];
        QTestState *s = start();
        uint64_t words[RENDER_WORDS], expected[RENDER_WORDS];
        uint64_t value = pair == 0 ? ((uint64_t)xy(4, 0) << 32) | xy(5, 0) :
                         pair == 1 ? 0x0000000100000001ULL :
                                     0xfedcba9813579bdfULL;

        tile(s, 0, 0, 3);
        setup(s, 0, true);
        qtest_writel(s, DE + 0x1b0, 6);
        qtest_writel(s, DE + 0x70, xy(4, 0));
        qtest_writel(s, DE + 0x74, xy(5, 0));
        qtest_writel(s, DE + 0xa0, xy(1, 0));
        qtest_writew(s, 0x30001, 0x1234);
        qtest_writew(s, 0x30004, 0x5a5a);
        if (pair == 0) {
            qtest_writel(s, DE + 0x70, xy(6, 0));
            qtest_writel(s, DE + 0x74, xy(7, 0));
        } else if (pair == 1) {
            qtest_writeq(s, DE + 0xa8, 0x0000000200000003ULL);
        }
        g_assert_cmpint(memtx_writeq(s, DE + 0x800 + offset, value), ==,
                        MEMTX_OK);
        g_assert_cmphex(qtest_readw(s, 0x30004), ==, 0x486e);
        g_assert_cmphex(qtest_readw(s, 0x30001), ==, 0x1234);
        for (unsigned member = 0; member < 2; member++) {
            qtest_writew(s, 0x30004, 0x5a5a);
            save_paired_renderer(s, path, expected);
            if (pair == 1) {
                uint64_t invalid = member ? 0x0000000100000002ULL :
                                           0x0000000200000001ULL;

                assert_failure(s, DE + 0x8a8, invalid, 8);
            } else {
                /* The second source fragment fails after a valid first one. */
                qtest_writel(s, DE + 0xa0, xy(511, 0));
                save_paired_renderer(s, after, expected);
                assert_failure(s, DE + 0x800 + offset, value, 8);
            }
            g_assert_cmphex(qtest_readw(s, 0x30004), ==, 0x5a5a);
            g_assert_cmphex(qtest_readw(s, 0x30001), ==, 0x1234);
            save_paired_renderer(s, after, words);
            g_assert_cmpmem(words, sizeof(words), expected, sizeof(expected));
            qtest_writel(s, DE + 0xa0, xy(1, 0));
            qtest_writel(s, DE + 0x9f0, 0);
            g_assert_cmphex(qtest_readw(s, 0x30004), ==, 0x486e);
            qtest_qmp_assert_success(s, "{'execute':'cont'}");
            qtest_qmp_assert_success(s, "{'execute':'stop'}");
        }
        qtest_quit(s);
    }
    g_unlink(path);
    g_unlink(after);
    g_rmdir(dir);
}

static void test_paired_start_accesses(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-paired-access-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "before", NULL);
    g_autofree char *after = g_build_filename(dir, "after", NULL);

    for (unsigned pair = 0; pair < ARRAY_SIZE(paired_start_offsets); pair++) {
        unsigned offset = paired_start_offsets[pair];
        QTestState *s = start();
        uint64_t value, words[RENDER_WORDS], expected[RENDER_WORDS];

        tile(s, 0, 0, 3);
        setup(s, 0, false);
        qtest_writeq(s, DE + offset, 0x1357246889abcdefULL);
        save_paired_renderer(s, path, expected);
        for (unsigned width = 1; width <= 2; width *= 2) {
            assert_failure(s, DE + 0x800 + offset, 0, width);
        }
        for (unsigned delta = 1; delta <= 4; delta *= 2) {
            assert_failure(s, DE + 0x800 + offset + delta, 0, 8);
        }
        for (unsigned width = 4; width <= 8; width *= 2) {
            start_before_failure(s);
            g_assert_cmpint(memtx_read(s, DE + 0x800 + offset, &value, width),
                            ==, MEMTX_ERROR);
            assert_stopped(s);
        }
        save_paired_renderer(s, after, words);
        g_assert_cmpmem(words, sizeof(words), expected, sizeof(expected));
        qtest_quit(s);
        for (unsigned legacy = 0; legacy < 2; legacy++) {
            for (unsigned functional = 0; functional < 2; functional++) {
                if (legacy && functional) {
                    continue;
                }
                s = start_profile(legacy ? "legacy-software" : "1.5", false,
                                  functional, "");
                g_assert_cmpint(memtx_writeq(s, DE + 0x800 + offset,
                                            0x1357246889abcdefULL),
                                ==, MEMTX_ERROR);
                save_paired_renderer(s, after, words);
                for (unsigned i = 0; i < RENDER_WORDS; i++) {
                    g_assert_cmphex(words[i], ==, 0);
                }
                qtest_quit(s);
            }
        }
    }
    g_unlink(path);
    g_unlink(after);
    g_rmdir(dir);
}

static void test_paired_start_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-paired-migrate-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *incoming = g_strdup_printf("exec:cat %s", quoted);

    for (unsigned pair = 0; pair < ARRAY_SIZE(paired_start_offsets); pair++) {
        unsigned offset = paired_start_offsets[pair];
        QTestState *s = start(), *to = start_options("-incoming defer");
        Shape c = shape_default();
        uint64_t words[RENDER_WORDS], expected[RENDER_WORDS], value;
        g_autofree uint32_t *pixels = NULL;

        c.width = 12;
        c.height = 8;
        c.x0 = 2;
        c.y0 = 1;
        c.x1 = 8;
        c.y1 = 5;
        c.draw |= 0xa0000;
        c.stipple_mode = 0x01050000;
        value = pair == 0 ? ((uint64_t)xy(c.x0, c.y0) << 32) |
                             xy(c.x1, c.y1) :
                pair == 1 ? 0x1357246889abcdefULL :
                            ((uint64_t)c.stipple_mode << 32) | c.pattern;
        map_region(s, 0, 1, 0, 0, 11, 7);
        shape_stage(s, &c);
        shape_memory(s, &c, NULL);
        qtest_writeq(s, DE + 0x800 + offset, value);
        pixels = shape_reference(&c);
        shape_memory(s, &c, pixels);
        save_paired_renderer(s, path, expected);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", incoming);
        sgi_wait_migration(to, "completed");
        shape_memory(to, &c, pixels);
        assert_failure(to, DE + 0x804 + offset, value, 8);
        save_paired_renderer(to, path, words);
        g_assert_cmpmem(words, sizeof(words), expected, sizeof(expected));
        reference_shape(&c, pixels);
        qtest_writel(to, DE + 0x9f0, 0);
        shape_memory(to, &c, pixels);
        qtest_system_reset(to);
        save_paired_renderer(to, path, words);
        for (unsigned i = 0; i < RENDER_WORDS; i++) {
            g_assert_cmphex(words[i], ==, 0);
        }
        qtest_quit(s);
        qtest_quit(to);
    }
    g_unlink(path);
    g_rmdir(dir);
}

typedef struct IndexedRect {
    unsigned bank, x, y, width, height;
    uint64_t p[128];
    const uint16_t *entries;
} IndexedRect;

static IndexedRect indexed_default(unsigned bank)
{
    IndexedRect c = { .bank = bank, .x = 125, .y = 126,
                      .width = 8, .height = 5 };

    c.p[0] = 0xfedcba98;
    c.p[8 / 4] = (bank << 10) | (2U << 8);
    c.p[0x18 / 4] = 0x208;
    c.p[0x60 / 4] = 0x03020000;
    c.p[0x70 / 4] = xy(126, 127);
    c.p[0x74 / 4] = xy(130, 129);
    c.p[0xa0 / 4] = 0xfedcba98;
    c.p[0xa8 / 4] = 0x13572468;
    c.p[0xac / 4] = 0x89abcdef;
    c.p[0xc0 / 4] = 0xfedcba98;
    c.p[0xc4 / 4] = 0x96a53c81;
    c.p[0xd0 / 4] = 0x12345616;
    c.p[0xd8 / 4] = 0x98765432;
    c.p[0x1b0 / 4] = 6;
    return c;
}

static uint32_t indexed_old(const IndexedRect *c, unsigned x, unsigned y)
{
    return 0xa1b2c35a ^ ((x - c->x) + 13 * (y - c->y)) * 0x01030711U;
}

static bool indexed_eligible(const IndexedRect *c, unsigned x, unsigned y)
{
    unsigned x0 = c->p[0x70 / 4] >> 16, y0 = c->p[0x70 / 4] & 0xffff;
    unsigned x1 = c->p[0x74 / 4] >> 16, y1 = c->p[0x74 / 4] & 0xffff;
    unsigned clip = c->p[0x10 / 4];
    bool screen = !clip;

    if (x < MIN(x0, x1) || x > MAX(x0, x1) ||
        y < MIN(y0, y1) || y > MAX(y0, y1) ||
        ((c->p[0x18 / 4] & (1U << 20)) &&
         !inside_bounds(c->p[0x48 / 4], x, y))) {
        return false;
    }
    for (unsigned n = 0; n < 5; n++) {
        if (clip & (1U << (5 + n))) {
            bool interior = inside_bounds(c->p[(0x20 + n * 8) / 4], x, y);

            screen |= interior == !!(clip & (1U << n));
        }
    }
    return screen && (!c->entries ||
        (c->entries[(y / 128) * 16 + x / 128] & 0x8000));
}

static uint32_t indexed_expected(const IndexedRect *c, unsigned x, unsigned y,
                                  unsigned passes)
{
    uint32_t old = indexed_old(c, x, y), result = old;
    uint32_t draw = c->p[0x18 / 4];
    uint32_t plane = draw & 0x80 ? c->p[0x1b8 / 4] & 255 : 255;

    if (!(draw & 8) || !indexed_eligible(c, x, y)) {
        return old;
    }
    for (unsigned i = 0; i < passes; i++) {
        uint32_t source = c->p[0xd0 / 4] & 255;
        uint32_t rop = draw & 0x200 ?
            rop_result(c->p[0x1b0 / 4], source, result & 255, 8) : source;

        result = (result & ~plane) | (rop & plane);
    }
    return result;
}

/* Compare complete storage words and guards, not just the logical index. */
static void indexed_memory(QTestState *s, const IndexedRect *c,
                             bool initialize, unsigned passes)
{
    for (unsigned y = c->y; y < c->y + c->height; y++) {
        for (unsigned x = c->x; x < c->x + c->width;) {
            unsigned count = MIN(c->x + c->width - x, 128 - x % 128);
            g_autofree uint8_t *data = g_malloc(count * 4);
            uint64_t address = pixel_address(c->bank, x, y, 4);

            if (!initialize) {
                qtest_memread(s, address, data, count * 4);
            }
            for (unsigned n = 0; n < count; n++) {
                if (initialize) {
                    stl_be_p(data + n * 4, indexed_old(c, x + n, y));
                } else {
                    uint32_t actual = ldl_be_p(data + n * 4);

                    g_assert_cmphex(actual, ==,
                        indexed_expected(c, x + n, y, passes));
                }
            }
            if (initialize) {
                qtest_memwrite(s, address, data, count * 4);
            }
            x += count;
        }
    }
}

static void indexed_stage(QTestState *s, const IndexedRect *c)
{
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        const Register *r = &registers[i];

        if (r->width == 8) {
            qtest_writeq(s, DE + r->offset, c->p[r->offset / 4]);
        } else {
            qtest_writel(s, DE + r->offset, c->p[r->offset / 4]);
        }
    }
}

static void indexed_prepare(QTestState *s, const IndexedRect *c)
{
    map_region(s, c->bank, 4, c->x, c->y,
               c->x + c->width - 1, c->y + c->height - 1);
    indexed_memory(s, c, true, 0);
    indexed_stage(s, c);
}

static void indexed_run(QTestState *s, const IndexedRect *c)
{
    indexed_prepare(s, c);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0xfedcba98), ==, MEMTX_OK);
    indexed_memory(s, c, false, 1);
    assert_idle(s);
}

static void test_indexed_word_rops(void)
{
    QTestState *s = start_indexed_state();
    const uint32_t planes[] = { 0, 0xff, 0x0f, 0xa5, 0x1234565a };

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned op = 0; op < 16; op++) {
            for (unsigned mask = 0; mask < ARRAY_SIZE(planes); mask++) {
                IndexedRect c = indexed_default(bank);

                c.p[0x18 / 4] |= 0x80;
                c.p[0x1b0 / 4] = op;
                c.p[0x1b8 / 4] = planes[mask];
                indexed_run(s, &c);
            }
        }
        IndexedRect c = indexed_default(bank);

        indexed_run(s, &c);
        qtest_writel(s, DE + 0x9f0, 1);
        indexed_memory(s, &c, false, 2);
        /* Inactive ROP and plane masks do not consume their raw contents. */
        c.p[0x18 / 4] = 8;
        c.p[0x1b0 / 4] = 0xdeadbeef;
        c.p[0x1b8 / 4] = 0xffffff00;
        indexed_run(s, &c);
    }
    qtest_quit(s);
}

static void test_indexed_word_masks(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned bit = 0; bit <= 8; bit++) {
            for (unsigned lanes = 0; lanes < 16; lanes++) {
                IndexedRect c = indexed_default(bank);

                c.p[0x18 / 4] = 0x280 | (lanes << 3);
                c.p[0x1b0 / 4] = 15;
                c.p[0x1b8 / 4] = bit == 8 ? 0 : 1U << bit;
                indexed_run(s, &c);
            }
        }
        for (unsigned lanes = 0; lanes < 16; lanes++) {
            IndexedRect c = indexed_default(bank);

            c.p[0x18 / 4] = 0x200 | (lanes << 3);
            c.p[0x1b8 / 4] = 0xffffff00;
            indexed_run(s, &c);
        }
    }
    qtest_quit(s);
}

static void test_indexed_word_geometry(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned direction = 0; direction < 4; direction++) {
            for (unsigned vertical = 0; vertical < 2; vertical++) {
                IndexedRect c = indexed_default(bank);

                c.width = 132;
                c.p[0x70 / 4] = xy(direction & 1 ? 254 : 126,
                                      direction & 2 ? 127 : 129);
                c.p[0x74 / 4] = xy(direction & 1 ? 126 : 254,
                                      direction & 2 ? 129 : 127);
                c.p[0x60 / 4] = 0x03000000 | (direction << 16);
                if (vertical) {
                    c.p[0x70 / 4] = (c.p[0x70 / 4] & 0xffff) | (128U << 16);
                    c.p[0x74 / 4] = (c.p[0x74 / 4] & 0xffff) | (128U << 16);
                }
                indexed_run(s, &c);
                indexed_memory(s, &c, true, 0);
                /* Independent disjoint commands must match one wide command. */
                for (unsigned part = 0; part < (vertical ? 1 : 2); part++) {
                    IndexedRect split = c;
                    unsigned lo = vertical ? 128 : part ? 128 : 126;
                    unsigned hi = vertical ? 128 : part ? 254 : 127;

                    split.p[0x70 / 4] = (split.p[0x70 / 4] & 0xffff) |
                        ((direction & 1 ? hi : lo) << 16);
                    split.p[0x74 / 4] = (split.p[0x74 / 4] & 0xffff) |
                        ((direction & 1 ? lo : hi) << 16);
                    indexed_stage(s, &split);
                    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==,
                                    MEMTX_OK);
                }
                indexed_memory(s, &c, false, 1);
            }
        }
        IndexedRect edge = indexed_default(bank);

        edge.x = edge.y = 2046;
        edge.width = edge.height = 2;
        edge.p[0x70 / 4] = edge.p[0x74 / 4] = xy(2047, 2047);
        indexed_run(s, &edge);
    }
    qtest_quit(s);
}

static void test_indexed_word_clipping(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned selected = 1; selected < 32; selected++) {
            for (unsigned scissor = 0; scissor < 2; scissor++) {
                IndexedRect c = indexed_default(bank);

                c.p[0x10 / 4] = selected | (selected << 5);
                for (unsigned n = 0; n < 5; n++) {
                    c.p[(0x20 + n * 8) / 4] =
                        bounds(126 + n % 3, 127 + n / 3,
                               128 + n % 3, 129 + n / 3);
                }
                if (scissor) {
                    c.p[0x18 / 4] |= 1U << 20;
                    c.p[0x48 / 4] = bounds(127, 128, 131, 130);
                }
                indexed_run(s, &c);
                if (!(selected & (selected - 1))) {
                    c.p[0x10 / 4] = selected << 5;
                    indexed_run(s, &c);
                }
            }
        }
        for (unsigned slot = 0; slot < 5; slot++) {
            for (unsigned inside = 0; inside < 2; inside++) {
                IndexedRect c = indexed_default(bank);

                for (unsigned n = 0; n < 5; n++) {
                    c.p[(0x20 + n * 8) / 4] =
                        bounds(126 + n, 127, 127 + n, 130);
                }
                c.p[0x10 / 4] = (1U << (5 + slot)) | (inside << slot);
                indexed_run(s, &c);
            }
        }
        IndexedRect empty = indexed_default(bank);

        empty.p[0x10 / 4] = 0x3ff;
        for (unsigned n = 0; n < 5; n++) {
            empty.p[(0x20 + n * 8) / 4] = bounds(127, 128, 127, 128);
        }
        indexed_run(s, &empty);
    }
    qtest_quit(s);
}

static void indexed_raw(QTestState *s, const char *path,
                          const IndexedRect *c)
{
    uint64_t words[RENDER_WORDS];

    save_paired_renderer(s, path, words);
    for (unsigned i = 0; i < ARRAY_SIZE(c->p); i++) {
        g_assert_cmphex(words[192 + i], ==, c->p[i]);
    }
}

static void test_indexed_word_start_forms(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-indexed-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(registers) + 4; n++) {
        IndexedRect c = indexed_default(n % 3);
        unsigned offset = n < ARRAY_SIZE(registers) ? registers[n].offset :
            n == ARRAY_SIZE(registers) ? 0x1f0 :
            paired_start_offsets[n - ARRAY_SIZE(registers) - 1];
        unsigned width = n < ARRAY_SIZE(registers) ? registers[n].width :
            offset == 0x1f0 ? 4 : 8;
        uint64_t value;

        c.p[0x18 / 4] |= 0x80;
        c.p[0x1b8 / 4] = 0x5a;
        if (offset == 0x10) {
            c.p[0x10 / 4] = 0x63;
            c.p[0x20 / 4] = bounds(126, 127, 129, 129);
            c.p[0x28 / 4] = bounds(128, 128, 131, 130);
        } else if (offset >= 0x20 && offset <= 0x40) {
            unsigned slot = (offset - 0x20) / 8;

            c.p[0x10 / 4] = (1U << (5 + slot)) | (1U << slot);
            c.p[offset / 4] = bounds(127, 128, 130, 130);
        } else if (offset == 0x48) {
            c.p[0x18 / 4] |= 1U << 20;
            c.p[0x48 / 4] = bounds(127, 128, 130, 130);
        }
        indexed_prepare(s, &c);
        if (offset == 0x1f0) {
            value = 0x13579bdf;
        } else if (n > ARRAY_SIZE(registers)) {
            value = (c.p[offset / 4] << 32) | c.p[offset / 4 + 1];
            qtest_writeq(s, DE + offset, 0x1122334455667788ULL);
        } else {
            value = c.p[offset / 4];
            if (width == 8) {
                qtest_writeq(s, DE + offset, bounds(131, 130, 126, 127));
            } else {
                qtest_writel(s, DE + offset, ~value);
            }
        }
        indexed_memory(s, &c, false, 0);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + offset, value, width),
                        ==, MEMTX_OK);
        indexed_memory(s, &c, false, 1);
        indexed_raw(s, path, &c);
        /* Direct NULL, FLUSH and status reads cannot replay an XOR. */
        qtest_writel(s, DE + 0x1f0, 0xfedcba98);
        qtest_writel(s, DE + 0x1f8, 0);
        assert_idle(s);
        indexed_memory(s, &c, false, 1);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void indexed_failure(QTestState *s, const IndexedRect *c,
                              const char *path, uint64_t address,
                              uint64_t value, unsigned width)
{
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    uint64_t cpu_address = qtest_readq(s, CRIME + 0x40);
    uint64_t cpu_status = qtest_readq(s, CRIME + 0x48);
    unsigned outputs = 0;

    for (unsigned i = 0; i < 5; i++) {
        outputs |= qtest_get_irq(s, i) << i;
    }
    save_paired_renderer(s, path, before);
    assert_failure(s, address, value, width);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x40), ==, cpu_address);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x48), ==, cpu_status);
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmpint(qtest_get_irq(s, i), ==, !!(outputs & (1U << i)));
    }
    indexed_memory(s, c, false, 0);
    save_paired_renderer(s, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
}

static void indexed_fault_seed(QTestState *s)
{
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
        "'property':'cpu-error-address','value':305419896}}");
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
        "'property':'cpu-error-status','value':5}}");
    qtest_set_irq_in(s, "/machine/peripheral/crime", "mace", 4, 1);
    qtest_writeq(s, CRIME + 0x18, 1U << 4);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
        "'property':'memory-error-status','value':234881024}}");
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
        "'property':'memory-error-address','value':74565}}");
}

static void test_indexed_word_failure(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-indexed-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_profile_ram("legacy-software", false, true, false,
        "-device sgi-crime-test-fixture,id=inject,"
        "crime=/machine/peripheral/crime");
    const uint32_t bad_modes[] = { 0x100, 0x201, 0x204, 0x210, 0x300,
                                   0xe00, 0x80000210 };

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    for (unsigned n = 0; n < 54; n++) {
        unsigned fault = n % 27;
        IndexedRect c = indexed_default(fault % 3);

        qtest_system_reset(s);
        if (fault < ARRAY_SIZE(bad_modes)) {
            c.p[8 / 4] = bad_modes[fault];
        } else {
            switch (fault) {
            case 7:
                c.p[0x60 / 4] = 0x00080000;
                break;
            case 8:
                c.p[0x60 / 4] = 0x02000000;
                break;
            case 9:
                c.p[0x18 / 4] |= 1U << 18;
                break;
            case 10:
                c.p[0x18 / 4] |= 1U << 21;
                break;
            case 11:
                c.p[0x18 / 4] |= 1U << 16;
                break;
            case 12:
                c.p[0x18 / 4] |= 1U << 10;
                break;
            case 13:
                c.p[0x1b0 / 4] = 0x100;
                break;
            case 14:
                c.p[0x50 / 4] = 1;
                break;
            case 15:
                c.p[0x58 / 4] = 0x80000000;
                break;
            case 16:
                c.p[0x60 / 4] |= 1;
                break;
            case 17:
                c.p[0x74 / 4] = xy(2048, 129);
                break;
            case 18:
                c.p[0x70 / 4] = xy(126, 2048);
                break;
            case 19:
                c.p[0x10 / 4] = 1;
                break;
            case 20:
                c.p[0x10 / 4] = 0x22;
                break;
            case 21:
                c.p[0x10 / 4] = 0x400;
                break;
            case 22:
                c.p[0x10 / 4] = 0x60;
                break;
            case 23:
                c.p[0x10 / 4] = 0x61;
                break;
            case 24:
                c.p[0x10 / 4] = 0x3ff;
                c.p[0x40 / 4] = bounds(130, 129, 126, 127);
                break;
            case 25:
                c.p[0x18 / 4] |= 1U << 20;
                c.p[0x48 / 4] = bounds(130, 129, 126, 127);
                break;
            case 26:
                c.p[0x60 / 4] ^= 1U << 16;
                break;
            }
        }
        if (n >= 27) {
            c.p[0x18 / 4] &= ~8U;
        }
        indexed_prepare(s, &c);
        indexed_fault_seed(s);
        indexed_failure(s, &c, path, DE + 0x8d0, 0x23456789, 4);
        for (unsigned i = 0; i < 5; i++) {
            g_assert_cmpint(qtest_get_irq(s, i), ==, i == 0);
        }
        /* Repair only setup; a later valid command exposes the old color. */
        IndexedRect repaired = indexed_default(c.bank);

        indexed_stage(s, &repaired);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        indexed_memory(s, &repaired, false, 1);
    }
    for (unsigned n = 0; n < ARRAY_SIZE(registers) + 4; n++) {
        IndexedRect c = indexed_default(n % 3);
        unsigned offset = n < ARRAY_SIZE(registers) ? registers[n].offset :
            n == ARRAY_SIZE(registers) ? 0x1f0 :
            paired_start_offsets[n - ARRAY_SIZE(registers) - 1];
        unsigned width = n < ARRAY_SIZE(registers) ? registers[n].width :
            offset == 0x1f0 ? 4 : 8;
        uint64_t value = offset == 0x1f0 ? 0 : c.p[offset / 4];

        qtest_system_reset(s);
        c.p[(offset == 0x58 ? 0x50 : 0x58) / 4] = 0x80000000;
        indexed_prepare(s, &c);
        indexed_fault_seed(s);
        if (n > ARRAY_SIZE(registers)) {
            value = (value << 32) | c.p[offset / 4 + 1];
        }
        indexed_failure(s, &c, path, DE + 0x800 + offset, value, width);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_indexed_word_accesses(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-indexed-map-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned hidden = 0; hidden < 4; hidden++) {
            IndexedRect c = indexed_default(bank);
            unsigned index = 16 + 1;

            qtest_system_reset(s);
            c.p[0x1b0 / 4] = 5;
            if (hidden == 0) {
                c.p[0x18 / 4] |= 1U << 20;
                c.p[0x48 / 4] = bounds(0, 0, 128, 2048);
            } else if (hidden == 1) {
                c.p[0x10 / 4] = 0x21;
                c.p[0x20 / 4] = bounds(0, 0, 128, 2048);
            } else if (hidden == 2) {
                c.p[0x18 / 4] |= 0x80;
                c.p[0x1b8 / 4] = 0;
            } else {
                c.p[0x18 / 4] &= ~8U;
            }
            indexed_prepare(s, &c);
            tile(s, bank, index, 0x400);
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            indexed_memory(s, &c, false, 0);
            c.p[0x10 / 4] = 0;
            c.p[0x18 / 4] = 0x208;
            indexed_stage(s, &c);
            indexed_failure(s, &c, path, DE + 0x8d0, 0x23456789, 4);
            /* NOOP still reads an eligible byte and validates the decoder. */
            tile(s, bank, index, page_for(bank, index));
            c.p[0x1b0 / 4] = 6;
            indexed_stage(s, &c);
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            indexed_memory(s, &c, false, 1);
        }
        IndexedRect c = indexed_default(bank);
        uint16_t entries[256] = { 0 };

        qtest_system_reset(s);
        indexed_prepare(s, &c);
        for (unsigned i = 0; i < 256; i++) {
            entries[i] = 0x8000 | page_for(bank, i);
        }
        entries[17] &= 0x7fff;
        uint64_t address = RE + 0x1000 + bank * 512 + 4 * 8;
        uint64_t word = qtest_readq(s, address);

        qtest_writeq(s, address, word & ~(0x8000ULL << 32));
        c.entries = entries;
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        indexed_memory(s, &c, false, 1);
        /* Different tile indices naming one index byte must roll back. */
        qtest_system_reset(s);
        c.entries = NULL;
        c.x = c.y = 0;
        c.width = 258;
        c.height = 1;
        c.p[0x70 / 4] = xy(0, 0);
        c.p[0x74 / 4] = xy(256, 0);
        indexed_prepare(s, &c);
        tile(s, bank, 2, page_for(bank, 0));
        indexed_failure(s, &c, path, DE + 0x8d0, 0x23456789, 4);
    }
    for (unsigned bank = 0; bank < 3; bank++) {
        IndexedRect c = indexed_default(bank);

        indexed_prepare(s, &c);
        for (unsigned y = c.y / 128; y <= (c.y + c.height - 1) / 128; y++) {
            for (unsigned x = c.x / 128;
                 x <= (c.x + c.width - 1) / 128; x++) {
                unsigned index = y * 16 + x;

                tile(s, bank, index, 0x4000 + page_for(bank, index));
            }
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        indexed_memory(s, &c, false, 1);
    }
    for (unsigned profile = 0; profile < 2; profile++) {
        QTestState *control = start_profile_ram(profile ? "legacy-software" :
            "1.5", false, !profile, false, "");

        qtest_writeq(control, 0x30000, 0xa1b2c35a12345678ULL);
        g_assert_cmpint(memtx_writel(control, DE + 8, 0x200), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writel(control, DE + 0x9f0, 0), !=, MEMTX_OK);
        g_assert_cmphex(qtest_readq(control, 0x30000), ==,
                        0xa1b2c35a12345678ULL);
        qtest_quit(control);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_clip_unions_packed(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned depth = 0; depth < 3; depth++) {
        unsigned bytes = 1U << depth;

        for (unsigned selected = 1; selected < 32; selected++) {
            setup(s, modes[depth], false);
            map_region(s, 0, bytes, 0, 0, 8, 4);
            qtest_writel(s, DE + 0x18, 0x2f8);
            qtest_writel(s, DE + 0xd0, 0xcccccccc);
            qtest_writel(s, DE + 0x1b0, 6);
            qtest_writel(s, DE + 0x10, selected | (selected << 5));
            for (unsigned n = 0; n < 5; n++) {
                qtest_writeq(s, DE + 0x20 + n * 8,
                             bounds(n, n & 1, n + 3, 3));
            }
            for (unsigned y = 0; y < 4; y++) {
                qtest_memset(s, pixel_address(0, 0, y, bytes), 0x5a, 9 * bytes);
            }
            rectangle(s, 0, 0, 7, 3, 0, 0);
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 9; x++) {
                    bool eligible = false;
                    uint32_t mask = UINT32_MAX >> (32 - bytes * 8);

                    for (unsigned n = 0; n < 5; n++) {
                        eligible |= (selected & (1U << n)) && x >= n &&
                                    x < n + 3 && y >= (n & 1) && y < 3;
                    }
                    g_assert_cmphex(pixel_read(s,
                        pixel_address(0, x, y, bytes), bytes), ==,
                        (eligible ? 0x96969696U : 0x5a5a5a5aU) & mask);
                }
            }
        }
    }
    /* Mixed masks remain refused for every ordinary primitive and copy. */
    for (unsigned kind = 0; kind < 4; kind++) {
        setup(s, 0, kind == 0);
        qtest_writel(s, DE + 0x10, 0x61);
        qtest_writeq(s, DE + 0x20, bounds(0, 0, 2, 2));
        qtest_writeq(s, DE + 0x28, bounds(1, 0, 3, 2));
        qtest_writel(s, DE + 0x70, 0);
        qtest_writel(s, DE + 0x74, xy(2, 1));
        if (kind == 1 || kind == 2) {
            qtest_writel(s, DE + 0x60, kind == 1 ? 0 : 0x01000000);
        } else if (kind == 3) {
            qtest_writel(s, DE + 0x18, 0x400f8);
            qtest_writel(s, DE + 0xc0, 0x001f0000);
        }
        qtest_memset(s, 0x30000, 0x5a, 32);
        assert_failure(s, DE + 0x9f0, 0, 4);
        g_assert_cmphex(qtest_readq(s, 0x30000), ==, 0x5a5a5a5a5a5a5a5aULL);
    }
    qtest_quit(s);
}

static void test_indexed_word_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-indexed-migrate-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *next = g_build_filename(dir, "next", NULL);
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *incoming = g_strdup_printf("exec:cat %s", quoted);

    for (unsigned bank = 0; bank < 3; bank++) {
        QTestState *from = start(), *to = start_options("-incoming defer");
        QTestState *control = start_indexed_state();
        IndexedRect c = indexed_default(bank);
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS];

        c.p[0x10 / 4] = 0x3ff;
        c.p[0x18 / 4] |= 0x80 | (1U << 20);
        c.p[0x1b8 / 4] = 0x123456a5;
        c.p[0x48 / 4] = bounds(126, 127, 131, 130);
        for (unsigned n = 0; n < 5; n++) {
            c.p[(0x20 + n * 8) / 4] = bounds(126 + n % 3, 127,
                                             128 + n % 3, 130);
        }
        indexed_prepare(from, &c);
        indexed_prepare(control, &c);
        /* Unsupported pending words migrate raw and are consumed only at GO. */
        qtest_writel(from, DE + 8, c.p[8 / 4] | 1);
        qtest_writel(control, DE + 8, c.p[8 / 4] | 1);
        save_renderer(from, path, before);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", incoming);
        sgi_wait_migration(to, "completed");
        save_paired_renderer(to, next, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        indexed_memory(to, &c, false, 0);
        for (unsigned peer = 0; peer < 2; peer++) {
            QTestState *q = peer ? to : control;

            assert_failure(q, DE + 0x8d0, 0x23456789, 4);
            indexed_memory(q, &c, false, 0);
            qtest_writel(q, DE + 8, c.p[8 / 4]);
            g_assert_cmpint(memtx_writel(q, DE + 0x9f0, 0), ==, MEMTX_OK);
            indexed_memory(q, &c, false, 1);
            qtest_writel(q, DE + 0x1f0, 0x12345678);
            qtest_writel(q, DE + 0x1f8, 0);
            indexed_memory(q, &c, false, 1);
        }
        save_paired_renderer(to, next, after);
        save_paired_renderer(control, path, before);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        qtest_system_reset(to);
        indexed_memory(to, &c, false, 1);
        save_paired_renderer(to, next, after);
        for (unsigned i = 0; i < RENDER_WORDS; i++) {
            g_assert_cmphex(after[i], ==, 0);
        }
        /* Reprogramming the reset device must restore the same operation. */
        indexed_run(to, &c);
        qtest_quit(from);
        qtest_quit(to);
        qtest_quit(control);
    }
    g_unlink(path);
    g_unlink(next);
    g_rmdir(dir);
}

static void test_clip_slot_shapes(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned kind = 0; kind < 4; kind++) {
        for (unsigned slot = 0; slot < 5; slot++) {
            for (unsigned inside = 0; inside < 2; inside++) {
                setup(s, 0, kind == 0);
                map_region(s, 0, 1, 0, 0, 8, 4);
                map_region(s, 1, 1, 0, 0, 8, 4);
                qtest_writel(s, DE, 0x400);
                qtest_writel(s, DE + 0xd0, 0xcc);
                for (unsigned n = 0; n < 5; n++) {
                    qtest_writeq(s, DE + 0x20 + n * 8,
                                 bounds(n + 1, 1, n + 2, 4));
                }
                qtest_writel(s, DE + 0x10,
                             (1U << (5 + slot)) | (inside << slot));
                qtest_writel(s, DE + 0x70,
                             kind == 1 ? xy(slot + 1, 2) : xy(0, 2));
                qtest_writel(s, DE + 0x74, xy(7, 2));
                qtest_writel(s, DE + 0x60, kind == 1 ? 0 :
                             kind == 2 ? 0x01000000 : 0x03020000);
                qtest_writel(s, DE + 0xa0, xy(0, 2));
                if (kind == 3) {
                    qtest_writel(s, DE + 0x18, 0x400f8);
                    qtest_writel(s, DE + 0xc0, 0x001f0000);
                    qtest_writel(s, DE + 0xc4, UINT32_MAX);
                }
                for (unsigned y = 0; y < 5; y++) {
                    qtest_memset(s, pixel_address(0, 0, y, 1), 0x5a, 9);
                    qtest_memset(s, pixel_address(1, 0, y, 1), 0xcc, 9);
                }
                g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
                for (unsigned y = 0; y < 5; y++) {
                    uint8_t data[9];

                    qtest_memread(s, pixel_address(0, 0, y, 1), data, 9);
                    for (unsigned x = 0; x < 9; x++) {
                        bool interior = x == slot + 1 && y >= 1 && y < 4;
                        bool drawn = y == 2 && x <= 7 &&
                            (kind != 1 || x == slot + 1) &&
                            interior == inside;

                        g_assert_cmphex(data[x], ==, drawn ? 0xcc : 0x5a);
                    }
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_transfer_upload_regression(void)
{
    QTestState *s = start_indexed_state();
    const uint8_t source[] = { 0x13, 0x57, 0x9b, 0xed, 0xda, 0x24, 0x68, 0xac };

    qtest_writeq(s, RE + 0x1700, 0x0000000080000040ULL);
    qtest_writeq(s, RE + 0x1708, 0x8000004200000000ULL);
    qtest_memwrite(s, 0x40ffd, source, 3);
    qtest_memwrite(s, 0x42000, source + 3, 5);
    map_region(s, 0, 4, 126, 126, 130, 129);
    for (unsigned y = 126; y <= 129; y++) {
        for (unsigned x = 126; x <= 130; x++) {
            pixel_write(s, pixel_address(0, x, y, 4), 4, 0xa1b2c35a);
        }
    }
    setup(s, 0x200, true);
    qtest_writel(s, DE, 4U << 10);
    qtest_writel(s, DE + 0x18, 0x200008);
    qtest_writel(s, DE + 0x70, xy(127, 127));
    qtest_writel(s, DE + 0x74, xy(129, 128));
    qtest_writel(s, DE + 0xa8, 1);
    qtest_writel(s, DE + 0xac, 3);
    qtest_writel(s, DE + 0xd0, 0xdeadc0de);
    g_assert_cmpint(memtx_writel(s, DE + 0x8a0, 0x1ffd), ==, MEMTX_OK);
    for (unsigned y = 126; y <= 129; y++) {
        for (unsigned x = 126; x <= 130; x++) {
            uint32_t expected = 0xa1b2c35a;

            if (x >= 127 && x <= 129 && y >= 127 && y <= 128) {
                expected = 0xa1b2c300 | source[(y - 127) * 5 + x - 127];
            }
            g_assert_cmphex(pixel_read(s, pixel_address(0, x, y, 4), 4), ==,
                            expected);
        }
    }
    qtest_quit(s);
}

static const struct {
    unsigned mode, bytes;
    uint32_t source, converted[4];
} color_formats[] = {
    { 0x010, 1, 0xb5, { 0xb5, 0xdaca, 0xb6b655ff, 0xb6b65500 } },
    { 0x124, 2, 0xd2a7, { 0xb4, 0xd2a7, 0xa5ad39ff, 0xa5ad3900 } },
    { 0x228, 4, 0x935dc17e, { 0x8b, 0x4978, 0x935dc17e, 0x935dc100 } },
    { 0x218, 4, 0xa9537bc1, { 0xa9, 0xd54f, 0xa9537bff, 0xa9537b00 } },
};

static uint64_t color_linear(unsigned bank, uint32_t address)
{
    return 0x400000 + bank * 0x40000 +
           ((address >> 12) & 31) * 0x2000 + (address & 0xfff);
}

static void color_bytes(QTestState *s, bool write, unsigned bank,
                        uint32_t address, unsigned bytes, uint32_t *value)
{
    uint8_t data[4];

    stl_be_p(data, *value);
    for (unsigned b = 0; b < bytes; b++) {
        uint64_t physical = color_linear(bank, address + b);
        if (write) {
            qtest_writeb(s, physical, data[4 - bytes + b]);
        } else {
            data[4 - bytes + b] = qtest_readb(s, physical);
        }
    }
    if (!write) {
        *value = bytes == 1 ? data[3] : bytes == 2 ?
                 lduw_be_p(data + 2) : ldl_be_p(data);
    }
}

static void color_format_cases(uint32_t source_upper,
                                uint32_t destination_upper)
{
    QTestState *s = start_indexed_state();

    for (unsigned bank = 0; bank < 2; bank++) {
        for (unsigned word = 0; word < 16; word++) {
            uint32_t page = 0x400 + bank * 0x40 + word * 4;
            qtest_writeq(s, RE + 0x1700 + bank * 128 + word * 8,
                         ((uint64_t)(0x80000000U | page) << 32) |
                         (0x80000000U | (page + 2)));
        }
    }
    for (unsigned direction = 0; direction < 3; direction++) {
        for (unsigned bank = 0; bank < 3; bank++) {
            for (unsigned sf = 0; sf < 4; sf++) {
                for (unsigned df = 0; df < 4; df++) {
                    unsigned sb = color_formats[sf].bytes;
                    unsigned db = color_formats[df].bytes;
                    unsigned linear = bank & 1;
                    unsigned tx = 512 / (direction == 1 ? sb : db) - 1;
                    uint32_t source = color_formats[sf].source;
                    uint32_t old = 0x56789abc, actual = 0;
                    uint32_t expected = color_formats[sf].converted[df];
                    uint32_t width_mask = UINT32_MAX >> (32 - db * 8);
                    unsigned source_bank = direction == 1 ? bank : 4 + linear;
                    unsigned dest_bank = direction == 0 ? bank : 4 + !linear;

                    if (df == 3) {
                        expected |= old & 0xff;
                    }
                    setup(s, color_formats[df].mode, true);
                    uint32_t source_mode = (source_bank << 10) |
                        color_formats[sf].mode | source_upper;
                    uint32_t destination_mode = (dest_bank << 10) |
                        color_formats[df].mode | destination_upper;
                    qtest_writel(s, DE, source_mode);
                    qtest_writel(s, DE + 8, destination_mode);
                    g_assert_cmphex(qtest_readl(s, DE), ==, source_mode);
                    g_assert_cmphex(qtest_readl(s, DE + 8), ==,
                                    destination_mode);
                    qtest_writel(s, DE + 0x70, xy(tx, 127));
                    qtest_writel(s, DE + 0x74, xy(tx + 2, 128));
                    qtest_writel(s, DE + 0xa0, direction == 1 ?
                                 xy(tx, 127) : 0x1fff);
                    qtest_writel(s, DE + 0xa8, direction == 1 ? 1 : sb);
                    qtest_writel(s, DE + 0xac, direction == 1 ? 1 : sb + 3);
                    qtest_writel(s, DE + 0xb0, 0x1fff);
                    qtest_writel(s, DE + 0xb4, db);
                    if (direction == 1) {
                        map_region(s, bank, sb, tx - 1, 126, tx + 3, 129);
                    } else if (direction == 0) {
                        map_region(s, bank, db, tx - 1, 126, tx + 3, 129);
                    }
                    for (unsigned i = 0; i < 6; i++) {
                        if (direction == 1) {
                            pixel_write(s, pixel_address(bank, tx + i % 3,
                                        127 + i / 3, sb), sb, source);
                        } else {
                            color_bytes(s, true, linear,
                                        0x1fff + i * sb + (i / 3) * 3,
                                        sb, &source);
                        }
                        if (direction == 0) {
                            pixel_write(s, pixel_address(bank, tx + i % 3,
                                        127 + i / 3, db), db, old);
                        } else {
                            color_bytes(s, true, !linear, 0x1fff + i * db,
                                        db, &old);
                        }
                    }
                    g_test_message("direction %u bank %u format %u -> %u",
                                   direction, bank, sf, df);
                    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==,
                                    MEMTX_OK);
                    g_assert_cmphex(qtest_readl(s, DE), ==, source_mode);
                    g_assert_cmphex(qtest_readl(s, DE + 8), ==,
                                    destination_mode);
                    for (unsigned i = 0; i < 6; i++) {
                        if (direction == 0) {
                            actual = pixel_read(s, pixel_address(bank,
                                tx + i % 3, 127 + i / 3, db), db);
                        } else {
                            color_bytes(s, false, !linear, 0x1fff + i * db,
                                        db, &actual);
                        }
                        g_assert_cmphex(actual, ==, expected & width_mask);
                    }
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_color_formats(void)
{
    color_format_cases(0, 0);
}

static void test_buffer_modes_color(void)
{
    static const uint32_t upper[] = {
        0x80000000U, 0x2000, 0xaaaaa000U, 0xffffe000U,
    };

    for (unsigned pattern = 0; pattern < ARRAY_SIZE(upper); pattern++) {
        for (unsigned role = 1; role <= 3; role++) {
            color_format_cases(role & 1 ? upper[pattern] : 0,
                               role & 2 ? upper[pattern] : 0);
        }
    }
}

static void color_map_linear(QTestState *s)
{
    for (unsigned bank = 0; bank < 2; bank++) {
        for (unsigned word = 0; word < 16; word++) {
            uint32_t page = 0x400 + bank * 0x40 + word * 4;
            qtest_writeq(s, RE + 0x1700 + bank * 128 + word * 8,
                         ((uint64_t)(0x80000000U | page) << 32) |
                         (0x80000000U | (page + 2)));
        }
    }
}

static void color_linear_setup(QTestState *s, unsigned sf, unsigned df,
                                uint32_t source, uint32_t destination,
                                unsigned width, unsigned height)
{
    color_map_linear(s);
    setup(s, color_formats[df].mode | 0x1400, true);
    qtest_writel(s, DE, color_formats[sf].mode | 0x1000);
    qtest_writel(s, DE + 0x70, xy(7, 13));
    qtest_writel(s, DE + 0x74, xy(6 + width, 12 + height));
    qtest_writel(s, DE + 0xa0, source);
    qtest_writel(s, DE + 0xa8, color_formats[sf].bytes);
    qtest_writel(s, DE + 0xac, color_formats[sf].bytes);
    qtest_writel(s, DE + 0xb0, destination);
    qtest_writel(s, DE + 0xb4, color_formats[df].bytes);
}

static void test_color_rops_masks(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned sf = 0; sf < 4; sf++) {
        for (unsigned df = 0; df < 4; df++) {
            unsigned sb = color_formats[sf].bytes, db = color_formats[df].bytes;
            uint32_t source = color_formats[sf].source, old = 0x56789abc;
            uint32_t meaningful = df == 3 ? 0xffffff00 :
                                  UINT32_MAX >> (32 - db * 8);
            for (unsigned op = 0; op < 16; op++) {
                for (unsigned lane = 0; lane < 5; lane++) {
                    uint32_t mask = meaningful & 0xa55a6996;
                    uint32_t actual = 0, expected;
                    uint32_t enables = lane == 4 ? 0x78 : 1U << (6 - lane);
                    color_linear_setup(s, sf, df, 0x1fff, 0x1fff, 1, 1);
                    qtest_writel(s, DE + 0x18, 0x200280 | enables);
                    qtest_writel(s, DE + 0x1b0, op);
                    qtest_writel(s, DE + 0x1b8, 0xa55a6996);
                    color_bytes(s, true, 0, 0x1fff, sb, &source);
                    color_bytes(s, true, 1, 0x1fff, db, &old);
                    qtest_writeb(s, color_linear(1, 0x1ffe), 0x6d);
                    qtest_writeb(s, color_linear(1, 0x1fff + db), 0xe3);
                    for (unsigned b = 0; b < db; b++) {
                        if (!(enables & (1U << (6 - ((0x1fff + b) & 3))))) {
                            mask &= ~(0xffU << (8 * (db - 1 - b)));
                        }
                    }
                    expected = (old & ~mask) |
                        (rop_result(op, color_formats[sf].converted[df], old,
                                    db * 8) & mask);
                    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==,
                                    MEMTX_OK);
                    color_bytes(s, false, 1, 0x1fff, db, &actual);
                    g_assert_cmphex(actual, ==,
                                   expected & (UINT32_MAX >> (32 - db * 8)));
                    g_assert_cmphex(qtest_readb(s, color_linear(1, 0x1ffe)), ==,
                                    0x6d);
                    g_assert_cmphex(qtest_readb(s,
                                    color_linear(1, 0x1fff + db)),
                                    ==, 0xe3);
                }
            }
        }
    }
    /* A disabled ROP and plane mask must ignore their retained parameters. */
    color_linear_setup(s, 3, 2, 0x1fff, 0x1fff, 1, 1);
    qtest_writel(s, DE + 0x18, 0x200078);
    qtest_writel(s, DE + 0x1b0, 0xffffffff);
    qtest_writel(s, DE + 0x1b8, 0);
    uint32_t source = 0x12345600, old = 0;
    color_bytes(s, true, 0, 0x1fff, 4, &source);
    color_bytes(s, true, 1, 0x1fff, 4, &old);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    color_bytes(s, false, 1, 0x1fff, 4, &old);
    g_assert_cmphex(old, ==, 0x123456ff);
    qtest_quit(s);
}

static void test_color_geometry(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned sf = 0; sf < 4; sf++) {
        unsigned df = (sf + 1) % 4;
        unsigned sb = color_formats[sf].bytes, db = color_formats[df].bytes;
        for (unsigned direction = 0; direction < 4; direction++) {
            color_linear_setup(s, sf, df, 0x1ffff, 0x1ffff, 3, 2);
            qtest_writel(s, DE + 0x60, 0x03000000 | direction << 16);
            qtest_writel(s, DE + 0x70, xy(direction & 1 ? 9 : 7,
                                         direction & 2 ? 13 : 14));
            qtest_writel(s, DE + 0x74, xy(direction & 1 ? 7 : 9,
                                         direction & 2 ? 14 : 13));
            qtest_writel(s, DE + 0xac, sb + 3);
            qtest_writel(s, DE + 0x18, 0x3002f8);
            qtest_writeq(s, DE + 0x48, bounds(8, 13, 10, 15));
            for (unsigned i = 0; i < 6; i++) {
                uint32_t source = i & 1 ? 0 : color_formats[sf].source;
                uint32_t old = 0x56789abc;
                color_bytes(s, true, 0, 0x1ffff + i * sb + (i / 3) * 3,
                            sb, &source);
                color_bytes(s, true, 1, 0x1ffff + i * db, db, &old);
            }
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            for (unsigned i = 0; i < 6; i++) {
                unsigned x = direction & 1 ? 9 - i % 3 : 7 + i % 3;
                uint32_t expected = 0x56789abc, actual = 0;
                if (x >= 8) {
                    expected = i & 1 ? (df == 2 && sf != 1 ? 255 :
                                        df == 1 && sf != 1 ? 0x8000 : 0) :
                               color_formats[sf].converted[df];
                    if (df == 3) {
                        expected |= 0xbc;
                    }
                }
                color_bytes(s, false, 1, 0x1ffff + i * db, db, &actual);
                g_assert_cmphex(actual, ==,
                               expected & (UINT32_MAX >> (32 - db * 8)));
            }
        }
    }
    qtest_quit(s);
}

static void test_color_failures(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-color-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned fault = 0; fault < 23; fault++) {
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
        uint32_t source = 0x1234567a, old = 0x56789abc;
        color_linear_setup(s, 3, 2, 0x1ff0, 0x1ff0, 3, 2);
        for (unsigned i = 0; i < 6; i++) {
            color_bytes(s, true, 0, 0x1ff0 + 4 * i, 4, &source);
            color_bytes(s, true, 1, 0x1ff0 + 4 * i, 4, &old);
        }
        switch (fault) {
        case 0:
            qtest_writel(s, DE, 0x1238);
            break;
        case 1:
            qtest_writel(s, DE, 0x1318);
            break;
        case 2:
            qtest_writel(s, DE, 0x1219);
            break;
        case 3:
            qtest_writel(s, DE + 8, 0x162a);
            break;
        case 4:
            qtest_writel(s, DE, 0x80001238);
            break;
        case 5:
            qtest_writel(s, DE + 0xa8, 1);
            break;
        case 6:
            qtest_writel(s, DE + 0xac, 3);
            break;
        case 7:
            qtest_writel(s, DE + 0xb4, 1);
            break;
        case 8:
            qtest_writel(s, DE + 0xac, 0x10000000);
            break;
        case 9:
            qtest_writel(s, DE + 0xa0, 0xfffffffe);
            break;
        case 10:
            qtest_writel(s, DE + 0xb0, 0xfffffffe);
            break;
        case 11:
            qtest_writel(s, DE + 0xac, 32 * 4096);
            break;
        case 12:
            qtest_writeq(s, RE + 0x1708, 0x0000000080000406ULL);
            break;
        case 13:
            qtest_writeq(s, RE + 0x1788, 0x0000000080000446ULL);
            break;
        case 14:
            qtest_writeq(s, RE + 0x1708, 0x8000400080000406ULL);
            break;
        case 15:
            qtest_writeq(s, RE + 0x1780, qtest_readq(s, RE + 0x1700));
            qtest_writeq(s, RE + 0x1788, qtest_readq(s, RE + 0x1708));
            break;
        case 16:
            qtest_writel(s, DE + 0x18, 0x2002f8);
            qtest_writel(s, DE + 0x1b0, 16);
            break;
        case 17:
            qtest_writel(s, DE + 0x18, 0x2003f8);
            break;
        case 18:
            qtest_writel(s, DE + 0x18, 0x3002f8);
            qtest_writeq(s, DE + 0x48, bounds(0, 0, 0, 0));
            qtest_writel(s, DE + 0xa0, 0xfffffffe);
            break;
        case 19:
            qtest_writel(s, DE + 0x18, 0x200280);
            qtest_writel(s, DE + 0x1b8, 0);
            qtest_writel(s, DE + 0xb0, 0xfffffffe);
            break;
        case 20:
        case 21:
            qtest_writel(s, DE + 0x1b0, fault == 20 ? 0 : 5);
            qtest_writeq(s, RE + 0x1708, 0x0000000080000406ULL);
            break;
        case 22:
            qtest_writeq(s, RE + 0x1788, 0x8000044280000446ULL);
            qtest_writel(s, DE + 0xb0, 0x1ffc);
            qtest_writel(s, DE + 0x74, xy(9, 355));
            break;
        }
        save_paired_renderer(s, path, before);
        g_test_message("color rejection %u", fault);
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, 0xdeadbeef), ==,
                        MEMTX_ERROR);
        save_paired_renderer(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        for (unsigned i = 0; i < 6; i++) {
            uint32_t actual = 0;
            color_bytes(s, false, 1, 0x1ff0 + 4 * i, 4, &actual);
            if (fault != 15) {
                g_assert_cmphex(actual, ==, old);
            }
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_color_starts_accesses(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned sf = 0; sf < 4; sf++) {
        unsigned df = (sf + 1) % 4;
        for (unsigned form = 0; form < 5; form++) {
            unsigned sb = color_formats[sf].bytes, db = color_formats[df].bytes;
            uint32_t source = color_formats[sf].source, old = 0x56789abc;
            uint32_t actual = 0, expected;
            color_linear_setup(s, sf, df, 0x1fff, 0x1fff, 1, 1);
            qtest_writel(s, DE + 0x1b0, 6);
            color_bytes(s, true, 0, 0x1fff, sb, &source);
            color_bytes(s, true, 1, 0x1fff, db, &old);
            color_bytes(s, false, 1, 0x1fff, db, &actual);
            g_assert_cmphex(actual, ==, old & (UINT32_MAX >> (32 - 8 * db)));
            switch (form) {
            case 0:
                qtest_writel(s, DE + 0xa0, 0);
                g_assert_cmpint(memtx_writel(s, DE + 0x8a0, 0x1fff), ==,
                                MEMTX_OK);
                break;
            case 1:
                qtest_writel(s, DE + 0xb0, 0);
                g_assert_cmpint(memtx_writel(s, DE + 0x8b0, 0x1fff), ==,
                                MEMTX_OK);
                break;
            case 2:
                qtest_writel(s, DE + 0xa8, 0);
                qtest_writel(s, DE + 0xac, 0);
                g_assert_cmpint(memtx_writeq(s, DE + 0x8a8,
                                             ((uint64_t)sb << 32) | sb), ==,
                                MEMTX_OK);
                break;
            case 3:
                qtest_writel(s, DE + 0xb4, 0);
                g_assert_cmpint(memtx_writel(s, DE + 0x8b4, db), ==,
                                MEMTX_OK);
                break;
            default:
                g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
                break;
            }
            expected = old ^ color_formats[sf].converted[df];
            if (df == 3) {
                expected = (expected & 0xffffff00) | (old & 255);
            }
            color_bytes(s, false, 1, 0x1fff, db, &actual);
            g_assert_cmphex(actual, ==,
                           expected & (UINT32_MAX >> (32 - 8 * db)));
        }
    }
    /* Clipped and masked pixels do not consume otherwise invalid mappings. */
    for (unsigned masked = 0; masked < 2; masked++) {
        color_linear_setup(s, 3, 2, 0x1fff, 0x1fff, 3, 2);
        qtest_writeq(s, RE + 0x1708, 0);
        if (masked) {
            qtest_writel(s, DE + 0x1b8, 0);
        } else {
            qtest_writel(s, DE + 0x18, 0x3002f8);
            qtest_writeq(s, DE + 0x48, bounds(0, 0, 0, 0));
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    }
    qtest_quit(s);
}

static void test_color_ram_views(void)
{
    QTestState *s = start_profile_ram("legacy-software", true, true, false, "");
    const unsigned views[] = { 0, 0x40000, 0x80000 };

    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(s, CRIME + 0x208 + bank * 8, 0x100 | (bank * 4));
    }
    for (unsigned view = 0; view < ARRAY_SIZE(views); view++) {
        uint32_t source = 0xa9537b00, old = 0x56789abc, actual = 0;
        uint64_t tlb[32];
        color_linear_setup(s, 3, 2, 0x1fff, 0x1fff, 1, 1);
        color_bytes(s, true, 0, 0x1fff, 4, &source);
        color_bytes(s, true, 1, 0x1fff, 4, &old);
        qtest_writeq(s, RE + 0x1700,
                     0x8000040000000000ULL | 0x80000402U | views[view]);
        qtest_writeq(s, RE + 0x1708,
                     ((uint64_t)(0x80000404U | views[view]) << 32));
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        color_bytes(s, false, 1, 0x1fff, 4, &actual);
        g_assert_cmphex(actual, ==, 0xa9537bff);
        qtest_writeq(s, RE + 0x1780, 0x8000040080000402ULL);
        qtest_writeq(s, RE + 0x1788, 0x8000040400000000ULL);
        for (unsigned word = 0; word < ARRAY_SIZE(tlb); word++) {
            tlb[word] = qtest_readq(s, RE + 0x1700 + word * 8);
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, 0xdeadbeef), ==,
                        MEMTX_ERROR);
        for (unsigned word = 0; word < ARRAY_SIZE(tlb); word++) {
            g_assert_cmphex(qtest_readq(s, RE + 0x1700 + word * 8), ==,
                            tlb[word]);
        }
        color_bytes(s, false, 0, 0x1fff, 4, &actual);
        g_assert_cmphex(actual, ==, source);
    }
    for (unsigned bank = 1; bank < 8; bank++) {
        uint64_t boundary = 0x40000000ULL + (uint64_t)bank * 0x08000000;
        uint32_t page = boundary >> 12;
        uint32_t old = 0x56789abc;
        uint8_t source[] = { 0xa9, 0x53, 0x7b, 0x12 };
        color_linear_setup(s, 3, 2, 0x1fff, 0x1fff, 1, 1);
        color_bytes(s, true, 1, 0x1fff, 4, &old);
        qtest_writeq(s, RE + 0x1700,
                     0x8000040000000000ULL | 0x80000000U | (page - 1));
        qtest_writeq(s, RE + 0x1708, (uint64_t)(0x80000000U | page) << 32);
        qtest_memwrite(s, boundary - 1, source, sizeof(source));
        for (unsigned fault = 0; fault < 2; fault++) {
            uint32_t actual = 0;
            qtest_writeq(s, CRIME + 0x208 + bank * 8,
                         fault ? bank * 4 : 0x100);
            g_assert_cmpint(memtx_writel(s, DE + 0x8d0, 0xdeadbeef), ==,
                            MEMTX_ERROR);
            qtest_writeq(s, CRIME + 0x208 + bank * 8, 0x100 | bank * 4);
            color_bytes(s, false, 1, 0x1fff, 4, &actual);
            g_assert_cmphex(actual, ==, old);
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        uint32_t actual = 0;
        color_bytes(s, false, 1, 0x1fff, 4, &actual);
        g_assert_cmphex(actual, ==, 0xa9537bff);
    }
    qtest_quit(s);
}

static void test_color_components(void)
{
    static const struct {
        unsigned source_format, destination_format;
        uint32_t source, expected;
    } cases[] = {
        { 0, 2, 0xe0, 0xff0000ff },
        { 0, 2, 0x1c, 0x00ff00ff },
        { 0, 2, 0x03, 0x0000ffff },
        { 0, 2, 0xff, 0xffffffff },
        { 1, 2, 0x52a7, 0xa5ad3900 },
        { 1, 2, 0xd2a7, 0xa5ad39ff },
        { 2, 1, 0x0102037f, 0x0000 },
        { 2, 1, 0x01020380, 0x8000 },
        { 3, 2, 0x12345600, 0x123456ff },
        { 3, 2, 0x123456ff, 0x123456ff },
    };
    QTestState *s = start_indexed_state();

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        unsigned sf = cases[i].source_format, df = cases[i].destination_format;
        uint32_t source = cases[i].source, old = 0xa5a5a5a5;
        color_linear_setup(s, sf, df, 0x1fff, 0x1fff, 1, 1);
        color_bytes(s, true, 0, 0x1fff, color_formats[sf].bytes, &source);
        color_bytes(s, true, 1, 0x1fff, color_formats[df].bytes, &old);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        color_bytes(s, false, 1, 0x1fff, color_formats[df].bytes, &old);
        g_assert_cmphex(old, ==, cases[i].expected);
    }
    qtest_quit(s);
}

static void test_color_tiled_traversal(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned download = 0; download < 2; download++) {
        for (unsigned direction = 0; direction < 4; direction++) {
            color_linear_setup(s, 3, 2, 0x1fff, 0x1fff, 3, 2);
            qtest_writel(s, DE + (download ? 0 : 8), download ? 0x218 : 0x228);
            qtest_writel(s, DE + 0x60, 0x03000000 | direction << 16);
            qtest_writel(s, DE + 0x70, xy(direction & 1 ? 129 : 127,
                                         direction & 2 ? 127 : 128));
            qtest_writel(s, DE + 0x74, xy(direction & 1 ? 127 : 129,
                                         direction & 2 ? 128 : 127));
            if (download) {
                qtest_writel(s, DE + 0xa0, xy(127, 127));
                qtest_writel(s, DE + 0xa8, 1);
                qtest_writel(s, DE + 0xac, 1);
            }
            map_region(s, 0, 4, 126, 126, 130, 129);
            for (unsigned i = 0; i < 6; i++) {
                uint32_t value = 0x12345600 + i * 0x11030729;
                uint32_t old = 0x56789abc;
                if (download) {
                    pixel_write(s, pixel_address(0, 127 + i % 3,
                                                127 + i / 3, 4), 4, value);
                    color_bytes(s, true, 1, 0x1fff + 4 * i, 4, &old);
                } else {
                    color_bytes(s, true, 0, 0x1fff + 4 * i, 4, &value);
                    pixel_write(s, pixel_address(0, 127 + i % 3,
                                                127 + i / 3, 4), 4, old);
                }
            }
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            for (unsigned i = 0; i < 6; i++) {
                unsigned x = direction & 1 ? 2 - i % 3 : i % 3;
                unsigned y = direction & 2 ? i / 3 : 1 - i / 3;
                unsigned source = download ? y * 3 + x : i;
                uint32_t expected = (0x12345600 + source * 0x11030729) |
                                    0xff;
                uint32_t actual = 0;
                if (download) {
                    color_bytes(s, false, 1, 0x1fff + 4 * i, 4, &actual);
                } else {
                    actual = pixel_read(s, pixel_address(0, 127 + x,
                                                         127 + y, 4), 4);
                }
                g_assert_cmphex(actual, ==, expected);
            }
        }
    }
    qtest_quit(s);
}

typedef struct DeTransfer {
    bool download;
    unsigned bank, linear, storage, direction, width, height;
    unsigned gx, gy, tx, ty, pitch, salt;
    uint32_t address;
    uint64_t p[128];
} DeTransfer;

static void de_transfer_geometry(DeTransfer *c)
{
    unsigned x1 = c->gx + c->width - 1, y1 = c->gy + c->height - 1;

    c->p[0x60 / 4] = 0x03000000 | (c->direction << 16);
    c->p[0x70 / 4] = xy(c->direction & 1 ? x1 : c->gx,
                          c->direction & 2 ? c->gy : y1);
    c->p[0x74 / 4] = xy(c->direction & 1 ? c->gx : x1,
                          c->direction & 2 ? y1 : c->gy);
    c->p[0xa0 / 4] = c->download ? xy(c->tx, c->ty) : c->address;
    c->p[0xa8 / 4] = 1;
    c->p[0xac / 4] = c->download ? 1 : c->pitch - c->width + 1;
    c->p[0xb0 / 4] = c->download ? c->address : 0x89abcdef;
    c->p[0xb4 / 4] = c->download ? 1 : 0xfedcba98;
}

static DeTransfer de_transfer_default(bool download, unsigned bank,
                                       unsigned linear, unsigned storage,
                                       unsigned direction)
{
    DeTransfer c = { .download = download, .bank = bank, .linear = linear,
                    .storage = storage, .direction = direction,
                    .width = 3, .height = 2, .address = 0x1ffd,
                    .gx = download ? 7 : 127, .gy = download ? 13 : 127,
                    .tx = 127, .ty = 127, .pitch = download ? 3 : 5 };
    uint32_t tiled = (bank << 10) | (storage == 4 ? 0x200 : 0);
    uint32_t packed = (4 + linear) << 10;

    c.p[0] = download ? tiled : packed;
    c.p[8 / 4] = download ? packed : tiled;
    c.p[0x18 / 4] = 0x2002f8;
    c.p[0xc0 / 4] = 0xfedcba98;
    c.p[0xc4 / 4] = 0x96a53c81;
    c.p[0xd0 / 4] = 0xdeadc0de;
    c.p[0xd8 / 4] = 0x12345678;
    c.p[0x1b0 / 4] = 3;
    c.p[0x1b8 / 4] = UINT32_MAX;
    de_transfer_geometry(&c);
    return c;
}

static uint64_t de_linear_address(const DeTransfer *c, uint32_t address)
{
    return 0x400000 + c->linear * 0x40000 +
           ((address >> 12) & 31) * 0x2000 + (address & 0xfff);
}

static uint32_t de_tile_old(unsigned x, unsigned y, unsigned salt)
{
    return (0xa1b2c35a ^ (x * 3 + y * 13) * 0x01030711U) ^ salt;
}

static uint8_t de_linear_old(uint32_t address, unsigned salt)
{
    return ((address * 37 + 17) ^ ((address & 0xfff) >> 8)) ^ salt;
}

static bool de_transfer_visible(const DeTransfer *c, unsigned x, unsigned y)
{
    uint32_t draw = c->p[0x18 / 4], clip = c->p[0x10 / 4];

    if ((draw & (1U << 20)) && !inside_bounds(c->p[0x48 / 4], x, y)) {
        return false;
    }
    for (unsigned n = 0; n < 5; n++) {
        if ((clip & (1U << (5 + n))) &&
            inside_bounds(c->p[(0x20 + n * 8) / 4], x, y) !=
            !!(clip & (1U << n))) {
            return false;
        }
    }
    return true;
}

static uint32_t de_transfer_result(const DeTransfer *c, uint32_t old,
                                    uint8_t source, unsigned lane,
                                    unsigned passes)
{
    uint32_t mask = c->p[0x18 / 4] & 0x80 ? c->p[0x1b8 / 4] & 255 : 255;

    if (!(c->p[0x18 / 4] & (1U << (6 - lane)))) {
        return old;
    }
    for (unsigned i = 0; i < passes; i++) {
        uint32_t value = c->p[0x18 / 4] & 0x200 ?
            rop_result(c->p[0x1b0 / 4], source, old & 255, 8) : source;

        old = (old & ~mask) | (value & mask);
    }
    return old;
}

static void de_transfer_memory(QTestState *s, const DeTransfer *c,
                                bool initialize, unsigned passes)
{
    uint64_t first = c->address ? c->address - 1 : 0;
    uint64_t last = (uint64_t)c->address +
                    (c->height - 1) * c->pitch + c->width;
    unsigned tx0 = c->tx ? c->tx - 1 : 0, ty0 = c->ty ? c->ty - 1 : 0;
    unsigned tx1 = MIN(2047, c->tx + c->width);
    unsigned ty1 = MIN(2047, c->ty + c->height);

    g_assert_cmpuint(last - first, <=, 0x21000);
    if (c->download && last - c->address == 0x20000) {
        /* The table wraps both outside guards onto active bytes. */
        first = c->address;
        last--;
    }
    for (uint64_t v = first; v <= last;) {
        unsigned count = MIN(last - v + 1, 4096 - (v & 0xfff));
        g_autofree uint8_t *data = g_malloc(count);

        if (!initialize) {
            qtest_memread(s, de_linear_address(c, v), data, count);
        }
        for (unsigned n = 0; n < count; n++) {
            uint32_t address = v + n;
            uint32_t expected = de_linear_old(address,
                                               c->download ? 0 : c->salt);

            if (passes && c->download && address >= c->address) {
                unsigned delta = address - c->address;
                unsigned row = delta / c->width, col = delta % c->width;
                unsigned x = c->gx +
                    (c->direction & 1 ? c->width - 1 - col : col);
                unsigned y = c->gy +
                    (c->direction & 2 ? row : c->height - 1 - row);

                if (row < c->height && de_transfer_visible(c, x, y)) {
                    uint8_t source = de_tile_old(c->tx + x - c->gx,
                                                 c->ty + y - c->gy, c->salt);

                    expected = de_transfer_result(c, expected, source,
                                                   address & 3, passes);
                }
            }
            if (initialize) {
                data[n] = expected;
            } else {
                g_assert_cmphex(data[n], ==, expected);
            }
        }
        if (initialize) {
            qtest_memwrite(s, de_linear_address(c, v), data, count);
        }
        v += count;
    }
    for (unsigned y = ty0; y <= ty1; y++) {
        for (unsigned x = tx0; x <= tx1;) {
            unsigned count = MIN(tx1 - x + 1,
                                  (512 - x * c->storage % 512) / c->storage);
            g_autofree uint8_t *data = g_malloc(count * c->storage);
            uint64_t physical = pixel_address(c->bank, x, y, c->storage);

            if (!initialize) {
                qtest_memread(s, physical, data, count * c->storage);
            }
            for (unsigned n = 0; n < count; n++) {
                unsigned px = x + n;
                uint32_t expected = de_tile_old(px, y,
                                                 c->download ? c->salt : 0);

                if (passes && !c->download && px >= c->tx &&
                    px < c->tx + c->width &&
                    y >= c->ty && y < c->ty + c->height &&
                    de_transfer_visible(c, px, y)) {
                    unsigned col = c->direction & 1 ?
                        c->tx + c->width - 1 - px : px - c->tx;
                    unsigned row = c->direction & 2 ?
                        y - c->ty : c->ty + c->height - 1 - y;
                    uint8_t source = de_linear_old(
                        c->address + row * c->pitch + col, c->salt);

                    expected = de_transfer_result(c, expected, source,
                        (px * c->storage + c->storage - 1) & 3, passes);
                }
                if (c->storage == 1) {
                    expected &= 255;
                }
                if (initialize) {
                    if (c->storage == 1) {
                        data[n] = expected;
                    } else {
                        stl_be_p(data + n * 4, expected);
                    }
                } else {
                    uint32_t actual = c->storage == 1 ? data[n] :
                                      (uint32_t)ldl_be_p(data + n * 4);

                    g_assert_cmphex(actual, ==, expected);
                }
            }
            if (initialize) {
                qtest_memwrite(s, physical, data, count * c->storage);
            }
            x += count;
        }
    }
}

static void de_transfer_stage(QTestState *s, const DeTransfer *c)
{
    IndexedRect raw = { 0 };

    memcpy(raw.p, c->p, sizeof(raw.p));
    indexed_stage(s, &raw);
    qtest_writel(s, DE + 0xb0, c->p[0xb0 / 4]);
    qtest_writel(s, DE + 0xb4, c->p[0xb4 / 4]);
}

static void de_transfer_prepare(QTestState *s, const DeTransfer *c)
{
    map_region(s, c->bank, c->storage, c->tx ? c->tx - 1 : 0,
               c->ty ? c->ty - 1 : 0, MIN(2047, c->tx + c->width),
               MIN(2047, c->ty + c->height));
    for (unsigned word = 0; word < 16; word++) {
        uint32_t page = 0x400 + c->linear * 0x40 + word * 4;

        qtest_writeq(s, RE + 0x1700 + c->linear * 128 + word * 8,
                     ((uint64_t)(0x80000000U | page) << 32) |
                     (0x80000000U | (page + 2)));
    }
    de_transfer_memory(s, c, true, 0);
    de_transfer_stage(s, c);
}

static void de_transfer_run(QTestState *s, const DeTransfer *c)
{
    de_transfer_prepare(s, c);
    de_transfer_memory(s, c, false, 0);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0xdeadbeef), ==, MEMTX_OK);
    de_transfer_memory(s, c, false, 1);
    assert_idle(s);
}

static void test_transfer_formats(gconstpointer data)
{
    bool download = GPOINTER_TO_UINT(data);
    QTestState *s = start_indexed_state();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned linear = 0; linear < 2; linear++) {
            for (unsigned storage = 1; storage <= 4; storage += 3) {
                for (unsigned direction = 0; direction < 4; direction++) {
                    DeTransfer c = de_transfer_default(download, bank, linear,
                                                        storage, direction);

                    if (storage == 1) {
                        c.tx = 511;
                        if (!download) {
                            c.gx = c.tx;
                        }
                        de_transfer_geometry(&c);
                    }
                    de_transfer_run(s, &c);
                    c.salt = 0x80;
                    de_transfer_memory(s, &c, true, 0);
                    /* Repeating START must read the source's current bytes. */
                    qtest_writel(s, DE + 0x9f0, 0);
                    de_transfer_memory(s, &c, false, 1);
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_transfer_rops(gconstpointer data)
{
    bool download = GPOINTER_TO_UINT(data);
    QTestState *s = start_indexed_state();
    const uint32_t masks[] = { 0, 255, 0xa5, 0x0f, 0x1234565a };

    for (unsigned storage = 1; storage <= 4; storage += 3) {
        for (unsigned op = 0; op < 16; op++) {
            for (unsigned mask = 0; mask < ARRAY_SIZE(masks); mask++) {
                DeTransfer c = de_transfer_default(download, op % 3, op % 2,
                                                    storage, op % 4);

                c.p[0x1b0 / 4] = op;
                c.p[0x1b8 / 4] = masks[mask];
                de_transfer_run(s, &c);
            }
        }
        DeTransfer c = de_transfer_default(download, 0, 1, storage, 2);

        c.p[0x1b0 / 4] = 6;
        de_transfer_run(s, &c);
        qtest_writel(s, DE + 0x9f0, 0);
        de_transfer_memory(s, &c, false, 2);
        c.p[0x18 / 4] &= ~(0x200U | 0x80U);
        c.p[0x1b0 / 4] = 0xdeadbeef;
        c.p[0x1b8 / 4] = 0;
        de_transfer_run(s, &c);
    }
    qtest_quit(s);
}

static void test_transfer_masks(gconstpointer data)
{
    bool download = GPOINTER_TO_UINT(data);
    QTestState *s = start_indexed_state();

    for (unsigned storage = 1; storage <= 4; storage += 3) {
        for (unsigned bit = 0; bit <= 8; bit++) {
            for (unsigned lanes = 0; lanes < 16; lanes++) {
                DeTransfer c = de_transfer_default(download, bit % 3, lanes % 2,
                                                    storage, lanes % 4);

                c.p[0x18 / 4] = 0x200280 | (lanes << 3);
                c.p[0x1b0 / 4] = 15;
                c.p[0x1b8 / 4] = bit == 8 ? 0 : 1U << bit;
                de_transfer_run(s, &c);
            }
        }
    }
    qtest_quit(s);
}

static void test_transfer_clipping(gconstpointer data)
{
    bool download = GPOINTER_TO_UINT(data);
    QTestState *s = start_indexed_state();

    for (unsigned direction = 0; direction < 4; direction++) {
        for (unsigned slot = 0; slot < 6; slot++) {
            for (unsigned inside = 0; inside < 2; inside++) {
                DeTransfer c = de_transfer_default(download, slot % 3, slot % 2,
                                                    inside ? 4 : 1, direction);
                uint64_t box = bounds(c.gx + 1, c.gy, c.gx + 3, c.gy + 1);

                c.p[0x18 / 4] |= 1U << 20;
                c.p[0x48 / 4] = bounds(c.gx + 1, c.gy, c.gx + 3, c.gy + 2);
                if (slot < 5) {
                    c.p[0x10 / 4] = (1U << (5 + slot)) | (inside << slot);
                    c.p[(0x20 + slot * 8) / 4] = box;
                }
                de_transfer_run(s, &c);
            }
        }
    }
    qtest_quit(s);
}

static void de_transfer_raw(QTestState *s, const char *path,
                            const DeTransfer *c)
{
    IndexedRect raw = { 0 };

    memcpy(raw.p, c->p, sizeof(raw.p));
    indexed_raw(s, path, &raw);
}

static void de_transfer_failure(QTestState *s, const DeTransfer *c,
                                const char *path, uint64_t address,
                                uint64_t value, unsigned width)
{
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    uint64_t cpu_address = qtest_readq(s, CRIME + 0x40);
    uint64_t cpu_status = qtest_readq(s, CRIME + 0x48);
    uint64_t memory_status = qtest_readq(s, CRIME + 0x250);
    uint64_t memory_address = qtest_readq(s, CRIME + 0x258);
    uint64_t interrupts = qtest_readq(s, CRIME + 0x10);
    unsigned outputs = 0;

    for (unsigned i = 0; i < 5; i++) {
        outputs |= qtest_get_irq(s, i) << i;
    }
    save_paired_renderer(s, path, before);
    assert_failure(s, address, value, width);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x40), ==, cpu_address);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x48), ==, cpu_status);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x250), ==, memory_status);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x258), ==, memory_address);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x10), ==, interrupts);
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmpint(qtest_get_irq(s, i), ==, !!(outputs & (1U << i)));
    }
    de_transfer_memory(s, c, false, 0);
    save_paired_renderer(s, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
}

static void de_transfer_entry(QTestState *s, unsigned bank, unsigned slot,
                              uint32_t entry)
{
    uint64_t address = RE + 0x1700 + bank * 128 + (slot / 2) * 8;
    unsigned shift = slot & 1 ? 0 : 32;
    uint64_t old = qtest_readq(s, address);

    qtest_writeq(s, address,
                 (old & ~(UINT64_C(0xffffffff) << shift)) |
                 ((uint64_t)entry << shift));
}

static void test_transfer_boundaries(gconstpointer data)
{
    bool download = GPOINTER_TO_UINT(data);
    QTestState *s = start_indexed_state();

    for (unsigned linear = 0; linear < 2; linear++) {
        for (unsigned storage = 1; storage <= 4; storage += 3) {
            DeTransfer c = de_transfer_default(download, linear, linear,
                                                storage, 2);

            c.address = 0x1fffd;
            de_transfer_geometry(&c);
            de_transfer_run(s, &c);
            /* An odd initial slot must not consume its unused companion. */
            c.address = 0x1ffd;
            de_transfer_geometry(&c);
            de_transfer_prepare(s, &c);
            de_transfer_entry(s, linear, 0, 0);
            de_transfer_entry(s, linear, 3, 0);
            qtest_writel(s, DE + 0x9f0, 0);
            de_transfer_memory(s, &c, false, 1);
            /* Include every page in the envelope, even upload row gaps. */
            c.gx = c.tx = c.gy = c.ty = 0;
            c.address = 0x1000;
            c.width = download ? 2048 : 1;
            c.height = download ? 64 : 32;
            c.pitch = download ? c.width : 4096;
            de_transfer_geometry(&c);
            de_transfer_run(s, &c);
            c.gx = c.gy = download ? 7 : 2047;
            c.tx = c.ty = 2047;
            c.width = c.height = c.pitch = 1;
            de_transfer_geometry(&c);
            de_transfer_run(s, &c);
        }
    }
    qtest_quit(s);
}

static void test_transfer_ram_views(gconstpointer data)
{
    bool download = GPOINTER_TO_UINT(data);
    QTestState *s = start_profile_ram("legacy-software", true, true, false, "");
    const unsigned views[] = { 0, 0x40000, 0x80000 };

    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(s, CRIME + 0x208 + bank * 8, 0x100 | (bank * 4));
    }
    for (unsigned view = 0; view < ARRAY_SIZE(views); view++) {
        DeTransfer c = de_transfer_default(download, view, view & 1, 4, 2);

        de_transfer_prepare(s, &c);
        for (unsigned slot = 0; slot < 32; slot++) {
            de_transfer_entry(s, c.linear, slot, 0x80000000U | views[view] |
                              (0x400 + c.linear * 0x40 + slot * 2));
        }
        /* Tiled entries can name the full view, linear entries also no-ECC. */
        for (unsigned y = 0; y < 2; y++) {
            for (unsigned x = 0; x < 2; x++) {
                unsigned index = 16 * y + x;

                tile(s, c.bank, index, 0x4000 + page_for(c.bank, index));
            }
        }
        qtest_writel(s, DE + 0x9f0, 0);
        de_transfer_memory(s, &c, false, 1);
    }
    for (unsigned bank = 1; bank < 8; bank++) {
        DeTransfer c = de_transfer_default(download, 0, bank & 1, 4, 2);
        uint64_t boundary = 0x40000000ULL + (uint64_t)bank * 0x08000000;
        unsigned page = boundary >> 12;
        uint8_t before[10], after[10];
        const uint8_t source[] = { 0x13, 0x57, 0x9b, 0x24, 0x68, 0xac };

        de_transfer_prepare(s, &c);
        de_transfer_entry(s, c.linear, 1, 0x80000000U | (page - 1));
        de_transfer_entry(s, c.linear, 2, 0x80000000U | page);
        qtest_memset(s, boundary - 4, 0x5a, sizeof(before));
        if (!download) {
            qtest_memwrite(s, boundary - 3, source, 3);
            qtest_memwrite(s, boundary + 2, source + 3, 3);
        }
        qtest_memread(s, boundary - 4, before, sizeof(before));
        /* A late missing placement or wrong geometry must write nothing. */
        for (unsigned fault = 0; fault < 2; fault++) {
            qtest_writeq(s, CRIME + 0x208 + bank * 8,
                         fault ? bank * 4 : 0x100);
            assert_failure(s, DE + 0x8d0, 0x12345678, 4);
            qtest_writeq(s, CRIME + 0x208 + bank * 8, 0x100 | bank * 4);
            qtest_memread(s, boundary - 4, after, sizeof(after));
            g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
            de_transfer_memory(s, &c, false, 0);
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        for (unsigned y = 0; y < 2; y++) {
            for (unsigned x = 0; x < 3; x++) {
                uint64_t tiled = pixel_address(c.bank, c.tx + x, c.ty + y, 4);
                uint64_t linear = boundary - 3 + (download ? 3 : 5) * y + x;

                if (download) {
                    g_assert_cmphex(qtest_readb(s, linear), ==,
                                    de_tile_old(c.tx + x, c.ty + y, 0) & 255);
                } else {
                    uint32_t expected = de_tile_old(c.tx + x, c.ty + y, 0);

                    g_assert_cmphex(qtest_readl(s, tiled), ==,
                                    (expected & ~255U) | source[y * 3 + x]);
                }
            }
        }
        g_assert_cmphex(qtest_readb(s, boundary - 4), ==, 0x5a);
        g_assert_cmphex(qtest_readb(s, boundary + (download ? 3 : 5)), ==,
                        0x5a);
    }
    qtest_quit(s);
}

static void test_transfer_semantics(gconstpointer data)
{
    bool download = GPOINTER_TO_UINT(data);
    g_autofree char *dir = g_dir_make_tmp("crime-transfer-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_profile_ram("legacy-software", false, true, false,
        "-device sgi-crime-test-fixture,id=inject,"
        "crime=/machine/peripheral/crime");

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    for (unsigned fault = 0; fault < 36; fault++) {
        DeTransfer c = de_transfer_default(download, fault % 3, fault % 2,
                                            fault & 1 ? 4 : 1, 2);
        unsigned source = 0, destination = 8 / 4;

        de_transfer_prepare(s, &c);
        switch (fault) {
        case 0:
            c.p[source] |= 1;
            break;
        case 1:
            c.p[destination] |= 2;
            break;
        case 2:
            c.p[source] |= 4;
            break;
        case 3:
            c.p[destination] |= 0x100;
            break;
        case 4:
            c.p[source] |= 0x10;
            break;
        case 5:
            c.p[destination] |= 0x30;
            break;
        case 6:
            c.p[0x18 / 4] |= 1;
            break;
        case 7:
            c.p[0x18 / 4] |= 1U << 18;
            break;
        case 8:
            c.p[0x18 / 4] |= 1U << 19;
            break;
        case 9:
            c.p[0x50 / 4] = 1;
            break;
        case 10:
            c.p[0x58 / 4] = 1;
            break;
        case 11:
            c.p[0x60 / 4] = 0;
            break;
        case 12:
            c.p[0x60 / 4] = 0x01000000;
            break;
        case 13:
            c.p[0x60 / 4] |= 1;
            break;
        case 14:
            c.p[0x70 / 4] = xy(2048, 127);
            break;
        case 15:
            c.p[0x74 / 4] = xy(129, 2048);
            break;
        case 16:
            c.p[0x1b0 / 4] = 16;
            break;
        case 17:
            c.p[0xa8 / 4] = 0;
            break;
        case 18:
            c.p[0xa8 / 4] = 2;
            break;
        case 19:
            c.p[0xac / 4] = 0;
            break;
        case 20:
            c.p[0xac / 4] = 0xffffffff;
            break;
        case 21:
            c.p[0xac / 4] = 0x10000000;
            break;
        case 22:
            c.p[0x10 / 4] = 1;
            break;
        case 23:
            c.p[0x10 / 4] = 0x63;
            break;
        case 24:
            c.p[0x10 / 4] = 0x60;
            break;
        case 25:
            c.p[0x10 / 4] = 1U << 10;
            break;
        case 26:
            c.p[0x10 / 4] = 0x21;
            c.p[0x20 / 4] = bounds(3, 2, 1, 4);
            break;
        case 27:
            c.p[0x18 / 4] |= 1U << 20;
            c.p[0x48 / 4] = bounds(3, 2, 1, 4);
            break;
        case 28:
            c.p[download ? source : destination] = (4 + c.linear) << 10;
            break;
        case 29:
            c.p[download ? destination : source] =
                (c.bank << 10) | (c.storage == 4 ? 0 : 0x200);
            break;
        case 30:
            c.p[download ? 0xb0 / 4 : 0xa0 / 4] = 0xfffffffe;
            break;
        case 31:
            c.p[0x74 / 4] = xy(c.gx + 1023, c.gy + 127);
            c.p[0xac / 4] = download ? 1 : 1024;
            c.p[download ? 0xb0 / 4 : 0xa0 / 4] = 0x1001;
            break;
        case 32:
            if (download) {
                c.p[0xa0 / 4] = xy(2047, 127);
            } else {
                c.p[0xac / 4] = 32 * 4096;
            }
            break;
        case 33:
            if (download) {
                c.p[0xa0 / 4] = xy(127, 2047);
            } else {
                c.p[0xa8 / 4] = 0x10000001;
            }
            break;
        case 34:
            if (download) {
                c.p[0xb4 / 4] = 2;
            } else {
                c.p[0xac / 4] = 0x0fffffff;
            }
            break;
        case 35:
            c.p[0x18 / 4] |= 1U << 20;
            c.p[0x48 / 4] = bounds(0, 0, 0, 0);
            c.p[download ? 0xb0 / 4 : 0xa0 / 4] = 0xfffffffe;
            break;
        }
        g_test_message("%s rejected state %u", download ? "download" : "upload",
                       fault);
        de_transfer_stage(s, &c);
        indexed_fault_seed(s);
        de_transfer_failure(s, &c, path, DE + 0x8d0, 0x23456789, 4);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_transfer_actual_accesses(gconstpointer data)
{
    bool download = GPOINTER_TO_UINT(data);
    g_autofree char *dir = g_dir_make_tmp("crime-transfer-access-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    for (unsigned storage = 1; storage <= 4; storage += 3) {
        for (unsigned fault = 0; fault < 7; fault++) {
            for (unsigned suppression = 0; suppression < 4; suppression++) {
                DeTransfer c = de_transfer_default(download, fault % 3,
                                                    fault % 2, storage, 2);
                unsigned slot = 2;
                unsigned index = (128 / 128) * 16 + 128 * storage / 512;

                de_transfer_prepare(s, &c);
                switch (fault) {
                case 0:
                    de_transfer_entry(s, c.linear, slot, 0);
                    break;
                case 1:
                    de_transfer_entry(s, c.linear, slot, 0x80010000);
                    break;
                case 2:
                    de_transfer_entry(s, c.linear, slot, 0x80004000);
                    break;
                case 3:
                    de_transfer_entry(s, c.linear, slot, 0x800c0000);
                    break;
                case 4: {
                    uint64_t address = RE + 0x1000 +
                                       c.bank * 512 + index / 4 * 8;

                    qtest_writeq(s, address, qtest_readq(s, address) &
                        ~(UINT64_C(0x8000) << (48 - index % 4 * 16)));
                    break;
                }
                case 5:
                    tile(s, c.bank, index, 0x400);
                    break;
                case 6:
                    tile(s, c.bank, index, 0x1000);
                    break;
                }
                /* Hide the late fault without requiring any translation. */
                if (suppression == 0) {
                    c.p[0x18 / 4] |= 1U << 20;
                    c.p[0x48 / 4] = bounds(0, 0, 0, 0);
                } else if (suppression == 1) {
                    c.p[0x1b8 / 4] = 0;
                } else if (suppression == 2) {
                    c.p[0x18 / 4] &= ~0x78U;
                } else {
                    c.p[0x10 / 4] = 0x21;
                    c.p[0x20 / 4] = bounds(0, 0, 0, 0);
                }
                de_transfer_stage(s, &c);
                g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
                de_transfer_memory(s, &c, false, 0);
                /* A mapped prefix can pass with the faulty suffix clipped. */
                c.p[0x18 / 4] = 0x3002f8;
                c.p[0x10 / 4] = 0;
                c.p[0x1b0 / 4] = 5;
                c.p[0x1b8 / 4] = 255;
                c.p[0x48 / 4] = bounds(c.gx, c.gy,
                                         c.gx + c.width, c.gy + 1);
                de_transfer_stage(s, &c);
                g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
                de_transfer_memory(s, &c, false, 0);
                /* CLEAR and NOOP still consume every active mapping. */
                c.p[0x18 / 4] = 0x2002f8;
                c.p[0x10 / 4] = 0;
                c.p[0x1b8 / 4] = 255;
                for (unsigned op = 0; op <= 5; op += 5) {
                    c.p[0x1b0 / 4] = op;
                    de_transfer_stage(s, &c);
                    de_transfer_failure(s, &c, path, DE + 0x8d0,
                                        0x12345678, 4);
                }
            }
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_transfer_aliases(gconstpointer data)
{
    bool download = GPOINTER_TO_UINT(data);
    g_autofree char *dir = g_dir_make_tmp("crime-transfer-alias-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    for (unsigned view = 0; view < 3; view++) {
        DeTransfer c = de_transfer_default(download, 0, 0, 4, 2);
        uint32_t page = 0x3f | (view == 1 ? 0x40000 :
                                  view == 2 ? 0x80000 : 0);

        de_transfer_prepare(s, &c);
        /* A late active byte aliases the other layout's first index. */
        de_transfer_entry(s, 0, 1, 0x80000000U | page);
        de_transfer_failure(s, &c, path, DE + 0x8d0, 0x12345678, 4);
        c.width = c.height = c.pitch = 1;
        c.address = 0x1fff;
        de_transfer_geometry(&c);
        de_transfer_prepare(s, &c);
        de_transfer_entry(s, 0, 1, 0x80000000U | page);
        de_transfer_failure(s, &c, path, DE + 0x8d0, 0x12345678, 4);
    }
    for (unsigned storage = 1; storage <= 4; storage += 3) {
        DeTransfer c = de_transfer_default(download, 0, 0, storage, 2);

        c.gx = c.tx = c.gy = c.ty = 0;
        c.address = 0x1000;
        c.width = download ? 2048 : 512 / storage + 1;
        c.height = download ? 3 : 1;
        c.pitch = c.width;
        de_transfer_geometry(&c);
        de_transfer_prepare(s, &c);
        if (download) {
            de_transfer_entry(s, 0, 2, 0x80040402);
        } else {
            tile(s, 0, 1, 0x4000 + page_for(0, 0));
        }
        de_transfer_failure(s, &c, path, DE + 0x8d0, 0x12345678, 4);
        /* Repeated source identities remain legal with disjoint output. */
        c.width = download ? 512 / storage + 1 : 1;
        c.height = download ? 1 : 2;
        c.pitch = download ? c.width : 4096;
        de_transfer_geometry(&c);
        de_transfer_prepare(s, &c);
        c.p[0x18 / 4] &= ~0x200U;
        de_transfer_stage(s, &c);
        if (download) {
            tile(s, 0, 1, page_for(0, 0));
            for (unsigned x = 0; x < 512 / storage; x++) {
                pixel_write(s, pixel_address(0, x, 0, storage), storage,
                            0xa1b2c300 | ((x * 37 + 0x79) & 255));
            }
        } else {
            de_transfer_entry(s, 0, 2, 0x80000402);
            qtest_writeb(s, de_linear_address(&c, 0x1000), 0x79);
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        if (download) {
            for (unsigned x = 0; x < c.width; x++) {
                uint64_t address = de_linear_address(&c, c.address + x);

                g_assert_cmphex(qtest_readb(s, address), ==,
                                ((x % (512 / storage)) * 37 + 0x79) & 255);
            }
        } else {
            for (unsigned y = 0; y < 2; y++) {
                uint32_t expected = de_tile_old(0, y, 0);

                expected = (expected & ~255U) | 0x79;
                if (storage == 1) {
                    expected &= 255;
                }
                g_assert_cmphex(pixel_read(s, pixel_address(0, 0, y, storage),
                                           storage), ==, expected);
            }
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_transfer_start_forms(gconstpointer data)
{
    bool download = GPOINTER_TO_UINT(data);
    g_autofree char *dir = g_dir_make_tmp("crime-transfer-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    unsigned total = ARRAY_SIZE(registers) + 4 + (download ? 2 : 0);

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    for (unsigned n = 0; n < total; n++) {
        DeTransfer c = de_transfer_default(download, n % 3, n % 2,
                                            n & 1 ? 4 : 1, n % 4);
        unsigned offset = n < ARRAY_SIZE(registers) ? registers[n].offset :
            n == ARRAY_SIZE(registers) ? 0x1f0 :
            n < ARRAY_SIZE(registers) + 4 ?
                paired_start_offsets[n - ARRAY_SIZE(registers) - 1] :
                0xb0 + 4 * (n - ARRAY_SIZE(registers) - 4);
        unsigned width = n < ARRAY_SIZE(registers) ? registers[n].width :
            n > ARRAY_SIZE(registers) && n < ARRAY_SIZE(registers) + 4 ? 8 : 4;
        bool pair = width == 8 && (offset == 0x70 || offset == 0xa8 ||
                                   offset == 0xc0);
        uint64_t value, old, words[RENDER_WORDS];

        c.p[0x1b0 / 4] = 6;
        c.p[0x1b8 / 4] = 0xa5;
        if (offset == 0x10 || (offset >= 0x20 && offset <= 0x40)) {
            unsigned slot = offset == 0x10 ? 4 : (offset - 0x20) / 8;

            c.p[0x10 / 4] = (1U << (5 + slot)) | (1U << slot);
            c.p[(0x20 + slot * 8) / 4] =
                bounds(c.gx + 1, c.gy, c.gx + 3, c.gy + 1);
        } else if (offset == 0x48) {
            c.p[0x18 / 4] |= 1U << 20;
            c.p[0x48 / 4] = bounds(c.gx + 1, c.gy, c.gx + 3, c.gy + 1);
        }
        de_transfer_prepare(s, &c);
        value = offset == 0x1f0 ? 0x13579bdf : pair ?
            (c.p[offset / 4] << 32) | c.p[offset / 4 + 1] : c.p[offset / 4];
        old = ~value;
        if (offset != 0x1f0) {
            g_assert_cmpint(memtx_write(s, DE + offset, old, width), ==,
                            MEMTX_OK);
        }
        /* Invalid prospective state cannot publish either paired member. */
        qtest_writel(s, DE + (offset == 0x58 ? 0x50 : 0x58), 1);
        de_transfer_failure(s, &c, path, DE + 0x800 + offset, value, width);
        save_paired_renderer(s, path, words);
        if (offset != 0x1f0) {
            g_assert_cmphex(words[192 + offset / 4], ==,
                            pair ? old >> 32 :
                            width == 4 ? (uint32_t)old : old);
            if (pair) {
                g_assert_cmphex(words[192 + offset / 4 + 1], ==, (uint32_t)old);
            }
        }
        qtest_writel(s, DE + (offset == 0x58 ? 0x50 : 0x58), 0);
        de_transfer_memory(s, &c, false, 0);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + offset, value, width), ==,
                        MEMTX_OK);
        de_transfer_memory(s, &c, false, 1);
        de_transfer_raw(s, path, &c);
        qtest_writel(s, DE + 0x1f0, 0xfedcba98);
        qtest_writel(s, DE + 0x1f8, 0);
        assert_idle(s);
        de_transfer_memory(s, &c, false, 1);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_transfer_accesses(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-transfer-map-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    for (unsigned download = 0; download < 2; download++) {
        DeTransfer c = de_transfer_default(download, 1, 1, 4, 2);

        de_transfer_prepare(s, &c);
        de_transfer_raw(s, path, &c);
        for (unsigned offset = 0xb0; offset <= 0xb4; offset += 4) {
            for (unsigned width = 1; width <= 8; width *= 2) {
                uint64_t readback;

                g_assert_cmpint(memtx_read(s, DE + offset, &readback, width),
                                !=, MEMTX_OK);
                g_assert_cmpint(memtx_read(s, DE + 0x800 + offset, &readback,
                                           width), !=, MEMTX_OK);
                if (width != 4) {
                    de_transfer_failure(s, &c, path, DE + offset,
                                        0x1122334455667788ULL, width);
                    de_transfer_failure(s, &c, path, DE + 0x800 + offset,
                                        0x1122334455667788ULL, width);
                }
            }
            for (unsigned delta = 1; delta <= 3; delta++) {
                uint64_t readback;

                g_assert_cmpint(memtx_read(s, DE + offset + delta,
                                           &readback, 4), !=, MEMTX_OK);
                g_assert_cmpint(memtx_read(s, DE + 0x800 + offset + delta,
                                           &readback, 4), !=, MEMTX_OK);
                de_transfer_failure(s, &c, path, DE + offset + delta,
                                    0x12345678, 4);
                de_transfer_failure(s, &c, path, DE + 0x800 + offset + delta,
                                    0x12345678, 4);
            }
            if (!download) {
                de_transfer_failure(s, &c, path, DE + 0x800 + offset, 1, 4);
            }
        }
        de_transfer_raw(s, path, &c);
        for (unsigned n = 0; n < 3; n++) {
            unsigned offset = n == 0 ? 0xa4 : n == 1 ? 0xa0 : 0x80;
            unsigned width = n == 1 ? 8 : 4;

            if (n == 1) {
                de_transfer_failure(s, &c, path, DE + offset,
                                    0x11223344, width);
            } else if (n == 2) {
                uint64_t readback, interrupts = qtest_readq(s, CRIME + 0x10);
                unsigned outputs = 0;

                for (unsigned i = 0; i < 5; i++) {
                    outputs |= qtest_get_irq(s, i) << i;
                }
                /* Native GL state is retained without admitting its START. */
                g_assert_cmpint(memtx_write(s, DE + offset, 0x11223344, width),
                                ==, MEMTX_OK);
                c.p[offset / 4] = 0x11223344;
                g_assert_cmpint(memtx_read(s, DE + offset, &readback, width),
                                !=, MEMTX_OK);
                de_transfer_raw(s, path, &c);
                de_transfer_memory(s, &c, false, 0);
                assert_idle(s);
                g_assert_cmphex(qtest_readq(s, CRIME + 0x10), ==, interrupts);
                for (unsigned i = 0; i < 5; i++) {
                    g_assert_cmpint(qtest_get_irq(s, i), ==,
                                    !!(outputs & (1U << i)));
                }
            } else {
                g_assert_cmpint(memtx_write(s, DE + offset, 0x11223344, width),
                                !=, MEMTX_OK);
                de_transfer_raw(s, path, &c);
                de_transfer_memory(s, &c, false, 0);
            }
            de_transfer_failure(s, &c, path, DE + 0x800 + offset,
                                0x11223344, width);
            if (n == 2) {
                g_assert_cmpint(memtx_write(s, DE + offset, 0, width), ==,
                                MEMTX_OK);
                c.p[offset / 4] = 0;
                de_transfer_raw(s, path, &c);
                de_transfer_memory(s, &c, false, 0);
            }
        }
    }
    /* The new aliases cannot execute fills or an existing tiled copy. */
    for (unsigned copy = 0; copy < 2; copy++) {
        DeTransfer c = de_transfer_default(false, 0, 0, 1, 2);

        de_transfer_prepare(s, &c);
        c.p[0] = c.p[8 / 4];
        c.p[0xa0 / 4] = xy(c.tx, c.ty);
        c.p[0xac / 4] = 1;
        if (!copy) {
            c.p[0x18 / 4] &= ~0x200000U;
        }
        de_transfer_stage(s, &c);
        for (unsigned offset = 0xb0; offset <= 0xb4; offset += 4) {
            de_transfer_failure(s, &c, path, DE + 0x800 + offset, 1, 4);
        }
    }
    qtest_quit(s);
    for (unsigned profile = 0; profile < 2; profile++) {
        QTestState *other = start_profile_ram(profile ? "legacy-software" :
            "1.5", false, !profile, false, "");
        uint64_t readback;

        qtest_writeq(other, 0x30000, 0xa1b2c35a12345678ULL);
        for (unsigned offset = 0xb0; offset <= 0xb4; offset += 4) {
            for (unsigned start = 0; start < 2; start++) {
                uint64_t address = DE + start * 0x800 + offset;

                g_assert_cmpint(memtx_write(other, address, 1, 4), !=,
                                MEMTX_OK);
                g_assert_cmpint(memtx_read(other, address, &readback, 4), !=,
                                MEMTX_OK);
                g_assert_cmphex(qtest_readq(other, 0x30000), ==,
                                0xa1b2c35a12345678ULL);
            }
        }
        qtest_quit(other);
    }
    g_unlink(path);
    g_rmdir(dir);
}

static void test_transfer_migration(gconstpointer data)
{
    bool download = GPOINTER_TO_UINT(data) & 1;
    bool upper = GPOINTER_TO_UINT(data) & 2;
    g_autofree char *dir =
        g_dir_make_tmp("crime-transfer-migrate-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *next = g_build_filename(dir, "next", NULL);
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *incoming = g_strdup_printf("exec:cat %s", quoted);

    for (unsigned linear = 0; linear < 2; linear++) {
        QTestState *from = start(), *to = start_options("-incoming defer");
        QTestState *control = start_indexed_state();
        DeTransfer c = de_transfer_default(download, linear + 1, linear, 4, 1);
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS];

        if (upper) {
            c.p[0] |= 0xaaaaa000U;
            c.p[8 / 4] |= 0xffffe000U;
        }
        c.p[0x18 / 4] |= 1U << 20;
        c.p[0x48 / 4] = bounds(c.gx + 1, c.gy, c.gx + 3, c.gy + 2);
        c.p[0x1b0 / 4] = 6;
        c.p[0x1b8 / 4] = 0x123456a5;
        for (unsigned peer = 0; peer < 2; peer++) {
            QTestState *q = peer ? control : from;

            de_transfer_prepare(q, &c);
            /* Both banks and unused destination words survive exactly. */
            de_transfer_entry(q, !linear, 3, 0x80000654);
            qtest_writel(q, DE + 0xa8, 0xf1234567);
        }
        save_renderer(from, path, before);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", incoming);
        sgi_wait_migration(to, "completed");
        save_paired_renderer(to, next, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        de_transfer_memory(to, &c, false, 0);
        for (unsigned word = 0; word < 32; word++) {
            g_assert_cmphex(qtest_readq(to, RE + 0x1700 + word * 8), ==,
                            qtest_readq(control, RE + 0x1700 + word * 8));
        }
        for (unsigned peer = 0; peer < 2; peer++) {
            QTestState *q = peer ? to : control;

            qtest_irq_intercept_out_named(q, "/machine/peripheral/crime",
                                           "cpu-ip");
            de_transfer_failure(q, &c, next, DE + 0x8d0, 0x12345678, 4);
            qtest_writel(q, DE + 0xa8, 1);
            qtest_writel(q, DE + 0x9f0, 0);
            de_transfer_memory(q, &c, false, 1);
            qtest_writel(q, DE + 0x1f0, 0x12345678);
            qtest_writel(q, DE + 0x1f8, 0);
            assert_idle(q);
            de_transfer_memory(q, &c, false, 1);
        }
        save_paired_renderer(to, next, after);
        save_paired_renderer(control, path, before);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        qtest_system_reset(to);
        de_transfer_memory(to, &c, false, 1);
        save_paired_renderer(to, next, after);
        for (unsigned i = 0; i < RENDER_WORDS; i++) {
            g_assert_cmphex(after[i], ==, 0);
        }
        for (unsigned word = 0; word < 32; word++) {
            g_assert_cmphex(qtest_readq(to, RE + 0x1700 + word * 8), ==, 0);
        }
        de_transfer_run(to, &c);
        qtest_quit(from);
        qtest_quit(to);
        qtest_quit(control);
    }
    g_unlink(path);
    g_unlink(next);
    g_rmdir(dir);
}


static Shape ordinary_shape(unsigned bank)
{
    Shape c = shape_default();

    c.indexed = true;
    c.bytes = 4;
    c.bank = bank;
    c.old = 0xa1b2c35a;
    c.foreground = 0x12345607;
    c.background = 0x9876541a;
    c.draw = 0xa0208;
    c.logic = 3;
    c.primitive = 0x01040020;
    c.stipple_mode = 0x011f0000;
    c.pattern = 0x55555555;
    c.x = 96;
    c.y = 96;
    c.width = 24;
    c.height = 12;
    c.x0 = 101;
    c.y0 = c.y1 = 100;
    c.x1 = 113;
    return c;
}

static void test_ordinary_pattern_start(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned opaque = 0; opaque < 2; opaque++) {
            Shape c = ordinary_shape(bank);

            c.draw = 0x80208 | (opaque << 17);
            map_region(s, bank, 4, 96, 96, 119, 107);
            shape_memory(s, &c, NULL);
            shape_stage(s, &c);
            qtest_writel(s, DE + 0xc4, 0xaaaaaaaa);
            g_assert_cmpint(memtx_writel(s, DE + 0x8c4, c.pattern), ==,
                            MEMTX_OK);
            for (unsigned y = 96; y < 108; y++) {
                for (unsigned x = 96; x < 120; x++) {
                    uint32_t expected = c.old;

                    if (y == 100 && x >= 101 && x < 113) {
                        if ((x - 101) % 2 == 0) {
                            expected = 0xa1b2c307;
                        } else if (opaque) {
                            expected = 0xa1b2c31a;
                        }
                    }
                    g_assert_cmphex(qtest_readl(s,
                        pixel_address(bank, x, y, 4)), ==, expected);
                }
            }
            g_assert_cmphex(qtest_readl(s, DE + 0xc0), ==, c.stipple_mode);
            g_assert_cmphex(qtest_readl(s, DE + 0xc4), ==, c.pattern);
        }
    }
    qtest_quit(s);
}

static void test_ordinary_phase(void)
{
    QTestState *s = start_indexed_state();
    const unsigned repeats[][2] = { {0, 0}, {1, 2}, {2, 2}, {254, 255} };

    for (unsigned format = 0; format < 4; format++) {
        for (unsigned kind = 0; kind < 2; kind++) {
            for (unsigned r = 0; r < ARRAY_SIZE(repeats); r++) {
                Shape c = ordinary_shape(format % 3);
                g_autofree uint32_t *expected = NULL;

                c.indexed = format == 3;
                c.bytes = format == 3 ? 4 : 1U << format;
                c.primitive = kind ? 0x03020000 : 0x01040040;
                c.draw = kind ? 0x602f8 : 0xa02f8;
                c.width = 64;
                c.x = c.y = 0;
                c.height = 8;
                c.x0 = 0;
                c.x1 = 62;
                c.y0 = c.y1 = 3;
                c.stipple_mode = 0x02050000 | (repeats[r][0] << 8) |
                                  repeats[r][1];
                c.pattern = 0xa4000000;
                shape_run(s, &c);
                expected = shape_reference(&c);
                reference_shape(&c, expected);
                qtest_writel(s, DE + 0x9f0, 0);
                shape_memory(s, &c, expected);
            }
        }
    }
    qtest_quit(s);
}

static void test_ordinary_geometry(void)
{
    QTestState *s = start_indexed_state();
    const unsigned widths[] = {0, 1, 31, 32, 33, 64, 65, 127};
    const int endpoints[][2] = {
        {7, 3}, {3, 7}, {-3, 7}, {-7, 3}, {-7, -3}, {-3, -7},
        {3, -7}, {7, -3}, {7, 0}, {0, -7}, {0, 0},
    };

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned n = 0; n < ARRAY_SIZE(endpoints); n++) {
            for (unsigned w = 0; w < ARRAY_SIZE(widths); w++) {
                Shape c = ordinary_shape(bank);

                c.x = c.y = 120;
                c.width = c.height = 20;
                c.x0 = c.y0 = 129;
                c.x1 = c.x0 + endpoints[n][0];
                c.y1 = c.y0 + endpoints[n][1];
                c.primitive = 0x01000000 | widths[w] |
                              ((w & 1) << 18);
                c.draw = 0x208;
                c.logic = 6;
                shape_run(s, &c);
                c.primitive = 0x00071234;
                c.x1 = c.y1 = 0xffff;
                c.stipple_mode = UINT32_MAX;
                c.draw |= 0xe0000;
                shape_run(s, &c);
            }
        }
    }
    qtest_quit(s);
}

static void test_ordinary_origins(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned format = 0; format < 4; format++) {
        for (unsigned kind = 0; kind < 4; kind++) {
            Shape c = ordinary_shape(format % 3);
            uint64_t screens[5] = { bounds(126, 126, 131, 130), 0, 0, 0, 0 };

            c.indexed = format == 3;
            c.bytes = format == 3 ? 4 : 1U << format;
            c.x = c.y = 120;
            c.width = c.height = 20;
            c.origin_x = 127;
            c.origin_y = 128;
            c.x0 = c.y0 = 0;
            c.x1 = 5;
            c.y1 = 0;
            c.primitive = kind == 0 ? 0 : kind == 1 ? 0x010000a0 :
                          kind == 2 ? 0x01040020 : 0x03020000;
            c.draw = 0x1002f8;
            c.scissor = bounds(0, 0, 4, 2);
            c.clip = 0x21;
            c.screens = screens;
            shape_run(s, &c);
            /* A negative local width fragment is visible after translation. */
            c.draw = 0x2f8;
            c.clip = 0;
            shape_run(s, &c);
            c.origin_x = c.origin_y = 32767;
            shape_run(s, &c);
            c.origin_x = 2047;
            c.origin_y = 2047;
            c.x = c.y = 2040;
            c.width = c.height = 8;
            shape_run(s, &c);
        }
    }
    /* Suppression still refers to the original final center. */
    for (unsigned first = 2046; first <= 2048; first++) {
        Shape c = ordinary_shape(0);

        c.x = 2044;
        c.y = 98;
        c.width = 4;
        c.height = 4;
        c.x0 = first;
        c.x1 = 2048;
        c.primitive = 0x01040020;
        c.draw = 0x208;
        shape_run(s, &c);
    }
    qtest_quit(s);
}


static void test_buffer_modes_indexed(void)
{
    static const uint32_t upper[] = {
        0, 0x80000000U, 0x2000, 0xaaaaa000U, 0xffffe000U,
    };
    QTestState *s = start_indexed_state();

    for (unsigned download = 0; download < 2; download++) {
        for (unsigned bank = 0; bank < 3; bank++) {
            for (unsigned linear = 0; linear < 2; linear++) {
                for (unsigned pattern = 0; pattern < ARRAY_SIZE(upper);
                     pattern++) {
                    for (unsigned role = 1; role <= 3; role++) {
                        DeTransfer c = de_transfer_default(download, bank,
                                                           linear, 4, 2);
                        c.p[0] |= role & 1 ? upper[pattern] : 0;
                        c.p[8 / 4] |= role & 2 ? upper[pattern] : 0;
                        de_transfer_run(s, &c);
                        g_assert_cmphex(qtest_readl(s, DE), ==, c.p[0]);
                        g_assert_cmphex(qtest_readl(s, DE + 8), ==, c.p[8 / 4]);
                    }
                }
            }
        }
    }
    DeTransfer actual = de_transfer_default(false, 0, 1, 4, 2);
    actual.p[8 / 4] = 0x80000200;
    actual.p[0x18 / 4] = 0x200208;
    de_transfer_prepare(s, &actual);
    g_assert_cmpint(memtx_writel(s, DE + 0x8a0, actual.address), ==, MEMTX_OK);
    de_transfer_memory(s, &actual, false, 1);
    g_assert_cmphex(qtest_readl(s, DE), ==, 0x1400);
    g_assert_cmphex(qtest_readl(s, DE + 8), ==, 0x80000200);
    qtest_quit(s);
}

static void test_buffer_modes_transactions(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-buffer-state-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    for (unsigned role = 0; role < 2; role++) {
        for (unsigned pattern = 0; pattern < 2; pattern++) {
            uint32_t upper = pattern ? 0xffffe000U : 0;
            const uint32_t bad_source[] = {
                0xc00, 0x1800, 0x1c00, 0x1200, 0x1004,
                0x1030, 0x1001, 0x1002, 0x130c,
            };
            const uint32_t bad_destination[] = {
                0xc00, 0x1800, 0x1c00, 0x100, 0x210,
                0x204, 0x201, 0x202, 0x230,
            };
            for (unsigned fault = 0; fault < ARRAY_SIZE(bad_source); fault++) {
                DeTransfer c = de_transfer_default(false, 0, 1, 4, 2);
                c.p[0] |= upper;
                c.p[8 / 4] |= upper;
                de_transfer_prepare(s, &c);
                uint32_t value = upper | (role ? bad_destination[fault] :
                                                bad_source[fault]);
                de_transfer_failure(s, &c, path, DE + 0x800 + role * 8,
                                    value, 4);
                g_assert_cmphex(qtest_readl(s, DE), ==, c.p[0]);
                g_assert_cmphex(qtest_readl(s, DE + 8), ==, c.p[8 / 4]);
            }
        }
        for (unsigned fault = 0; fault < 3; fault++) {
            DeTransfer c = de_transfer_default(false, 0, 1, 4, 2);
            c.p[0] |= 0xaaaaa000U;
            c.p[8 / 4] |= 0xffffe000U;
            de_transfer_prepare(s, &c);
            if (!fault) {
                de_transfer_entry(s, 1, 2, 0);
            } else if (fault == 1) {
                qtest_writel(s, DE + 0x74, xy(2048, 128));
            } else {
                de_transfer_entry(s, 1, 1, 0x8000003f);
            }
            de_transfer_failure(s, &c, path, DE + 0x800 + role * 8,
                                c.p[role ? 8 / 4 : 0], 4);
        }
    }
    for (unsigned download = 0; download < 2; download++) {
        for (unsigned role = 0; role < 2; role++) {
            DeTransfer c = de_transfer_default(download, 0, 1, 4, 2);
            uint32_t raw = c.p[role ? 8 / 4 : 0] | 0xffffe000U;
            c.p[0x1b0 / 4] = 6;
            de_transfer_prepare(s, &c);
            g_assert_cmpint(memtx_writel(s, DE + 0x800 + role * 8, raw), ==,
                            MEMTX_OK);
            de_transfer_memory(s, &c, false, 1);
            g_assert_cmphex(qtest_readl(s, DE + role * 8), ==, raw);
        }
    }
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, DE), ==, 0);
    g_assert_cmphex(qtest_readl(s, DE + 8), ==, 0);
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
    test_transfer_migration(GUINT_TO_POINTER(2));
    test_transfer_migration(GUINT_TO_POINTER(3));
}

static void test_buffer_modes_diagnostics(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-buffer-log-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "diagnostics", NULL);
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *extra = g_strdup_printf("-D %s", quoted);
    g_autofree char *contents = NULL;
    QTestState *s = start_profile_ram("legacy-software", false, true, false,
                                     extra);

    tile(s, 0, 0, 3);
    setup(s, 0x200, false);
    qtest_writel(s, DE, 0xfffff400U);
    qtest_writel(s, DE + 8, 0x2200);
    qtest_writel(s, DE + 0x70, xy(1, 0));
    qtest_writel(s, DE + 0x74, xy(1, 0));
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    DeTransfer c = de_transfer_default(false, 0, 1, 4, 2);
    c.p[0] |= 0xaaaaa000U;
    c.p[8 / 4] |= 0x80000000U;
    de_transfer_prepare(s, &c);
    for (unsigned i = 0; i < 20; i++) {
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    }
    assert_idle(s);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents,
        "destination buffer mode raw 0x00002200 ignores reserved upper bits "
        "0x00002000"));
    g_assert_nonnull(strstr(contents,
        "source buffer mode raw 0xaaaab400 ignores reserved upper bits "
        "0xaaaaa000"));
    g_assert_null(strstr(contents, "source buffer mode raw 0xfffff400"));
    unsigned observations = 0;
    for (const char *p = contents; (p = strstr(p, "buffer mode raw")); p++) {
        observations++;
    }
    g_assert_cmpuint(observations, ==, 2);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_ordinary_logic(void)
{
    QTestState *s = start_indexed_state();
    const uint32_t masks[] = {0, 255, 0xa5, 0x0f, 0x1234565a};

    for (unsigned kind = 0; kind < 3; kind++) {
        for (unsigned op = 0; op < 16; op++) {
            for (unsigned m = 0; m < ARRAY_SIZE(masks); m++) {
                Shape c = ordinary_shape(op % 3);

                c.primitive = kind == 0 ? 0 : kind == 1 ? 0x01000040 :
                              0x03020000;
                c.y1 = c.y0 + (kind == 2 ? 3 : 0);
                c.draw |= 0x80;
                c.logic = op;
                c.plane = masks[m];
                shape_run(s, &c);
            }
        }
        for (unsigned bit = 0; bit <= 8; bit++) {
            for (unsigned lanes = 0; lanes < 16; lanes++) {
                Shape c = ordinary_shape(bit % 3);

                c.primitive = kind == 0 ? 0 : kind == 1 ? 0x01000040 :
                              0x03020000;
                c.y1 = c.y0 + (kind == 2 ? 3 : 0);
                c.draw = 0x280 | lanes << 3;
                c.logic = 15;
                c.plane = bit == 8 ? 0 : 1U << bit;
                shape_run(s, &c);
                if (bit == 8) {
                    c.draw &= ~0x280U;
                    c.logic = UINT32_MAX;
                    shape_run(s, &c);
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_ordinary_unions(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned format = 0; format < 4; format++) {
        for (unsigned kind = 0; kind < 5; kind++) {
            for (unsigned selected = 1; selected < 32; selected++) {
                Shape c = ordinary_shape(format % 3);
                uint64_t screens[5];

                c.indexed = format == 3;
                c.bytes = format == 3 ? 4 : 1U << format;
                c.x = c.y = 0;
                c.width = 12;
                c.height = 8;
                c.x0 = c.y0 = 2;
                c.x1 = 8;
                c.y1 = kind < 3 ? 2 : 4;
                c.origin_x = 1;
                c.origin_y = 2;
                c.primitive = kind == 0 ? 0 : kind < 3 ? 0x01000040 :
                              0x03020000;
                c.draw = 0x1002f8 | (kind == 2 ? 0xa0000 :
                                       kind == 3 ? 0x60000 : 0);
                c.scissor = bounds(1, 1, 8, 5);
                c.logic = 6;
                c.clip = selected | selected << 5;
                c.screens = screens;
                c.stipple_mode = 0x02050000;
                for (unsigned n = 0; n < 5; n++) {
                    screens[n] = bounds(2 + n, 2 + n % 2, 5 + n, 6);
                }
                shape_run(s, &c);
                if (!(selected & (selected - 1))) {
                    c.clip &= ~31U;
                    shape_run(s, &c);
                }
                screens[0] = screens[1] = screens[2] = screens[3] =
                    screens[4] = bounds(1, 1, 1, 1);
                shape_run(s, &c);
            }
        }
    }
    qtest_quit(s);
}

static uint32_t *ordinary_initial(const Shape *c)
{
    uint32_t *pixels = g_new(uint32_t, c->width * c->height);
    uint32_t mask = UINT32_MAX >> (32 - c->bytes * 8);

    for (unsigned i = 0; i < c->width * c->height; i++) {
        pixels[i] = c->old & mask;
    }
    return pixels;
}

static void ordinary_prepare(QTestState *s, const Shape *c)
{
    map_region(s, c->bank, c->bytes, c->x, c->y,
               c->x + c->width - 1, c->y + c->height - 1);
    shape_memory(s, c, NULL);
    shape_stage(s, c);
}

static void ordinary_failure(QTestState *s, const Shape *c, const char *path,
                             uint64_t address, uint64_t value, unsigned width)
{
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    g_autofree uint32_t *pixels = ordinary_initial(c);
    uint64_t cpu_address = qtest_readq(s, CRIME + 0x40);
    uint64_t cpu_status = qtest_readq(s, CRIME + 0x48);
    unsigned outputs = 0;

    for (unsigned i = 0; i < 5; i++) {
        outputs |= qtest_get_irq(s, i) << i;
    }
    save_paired_renderer(s, path, before);
    assert_failure(s, address, value, width);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x40), ==, cpu_address);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x48), ==, cpu_status);
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmpint(qtest_get_irq(s, i), ==, !!(outputs & (1U << i)));
    }
    shape_memory(s, c, pixels);
    save_paired_renderer(s, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
}

static void test_ordinary_faults(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-ordinary-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_profile_ram("legacy-software", false, true, false,
        "-device sgi-crime-test-fixture,id=inject,"
        "crime=/machine/peripheral/crime");

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    for (unsigned fault = 0; fault < 25; fault++) {
        Shape c = ordinary_shape(fault % 3);

        ordinary_prepare(s, &c);
        switch (fault) {
        case 0:
            qtest_writel(s, DE + 0x58, 0x80000000);
            break;
        case 1:
            qtest_writel(s, DE + 0x58, 0x8000);
            break;
        case 2:
            qtest_writel(s, DE + 0x50, 1);
            break;
        case 3:
            qtest_writel(s, DE + 0x60, c.primitive | 1U << 19);
            break;
        case 4:
            qtest_writel(s, DE + 0x10, 0x800);
            break;
        case 5:
            qtest_writel(s, DE + 0x10, 0xc00);
            break;
        case 6:
            qtest_writel(s, DE + 0x10, 0x61);
            break;
        case 7:
            qtest_writel(s, DE + 0x10, 0x60);
            break;
        case 8:
            qtest_writel(s, DE + 0x10, 1);
            break;
        case 9:
            qtest_writel(s, DE + 0xc0, 0x08070000);
            break;
        case 10:
            qtest_writel(s, DE + 0xc0, 0x02050100);
            break;
        case 11:
            qtest_writel(s, DE + 0xc0, 0x00200000);
            break;
        case 12:
            qtest_writel(s, DE + 0x60, 0x03020000);
            qtest_writel(s, DE + 0x74, xy(2048, 101));
            break;
        case 13:
            qtest_writel(s, DE + 0x60, 0x03020000);
            qtest_writel(s, DE + 0x74, xy(2048, 100));
            break;
        case 14:
            qtest_writel(s, DE + 0x60, 0x03020000);
            qtest_writel(s, DE + 0x70, xy(100, 2046));
            qtest_writel(s, DE + 0x74, xy(100, 2048));
            break;
        case 15:
            qtest_writel(s, DE + 0x60, 0x03020000);
            qtest_writel(s, DE + 0x70, xy(2048, 100));
            qtest_writel(s, DE + 0x74, xy(2046, 100));
            break;
        case 16:
            qtest_writel(s, DE + 8, (c.bank << 10) | 0x100);
            break;
        case 17:
            qtest_writel(s, DE + 8, (c.bank << 10) | 0x210);
            break;
        case 18:
            qtest_writel(s, DE + 0x18, c.draw | 1U << 21);
            break;
        case 19:
            qtest_writel(s, DE + 0x10, 0x21);
            qtest_writeq(s, DE + 0x20, bounds(4, 3, 2, 1));
            break;
        case 20:
            qtest_writel(s, DE + 0x18, c.draw | 1U << 20);
            qtest_writeq(s, DE + 0x48, bounds(4, 3, 2, 1));
            break;
        default:
            /* A late valid entry must still pass the physical RAM decoder. */
            c.x = c.y = 124;
            c.width = c.height = 12;
            c.x0 = 126;
            c.x1 = 131;
            c.y0 = c.y1 = 128;
            ordinary_prepare(s, &c);
            tile(s, c.bank, 17, fault == 21 ? 0x400 :
                 fault == 22 ? 0x1000 : fault == 23 ? 0x1fc0 : 0x7fff);
            break;
        }
        indexed_fault_seed(s);
        ordinary_failure(s, &c, path, DE + 0x8d0, 0xdeadbeef, 4);
        if (fault < 21) {
            qtest_writel(s, DE + 0x18, qtest_readl(s, DE + 0x18) | 0x80);
            qtest_writel(s, DE + 0x1b8, 0);
            ordinary_failure(s, &c, path, DE + 0x8d0, 0xdeadbeef, 4);
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_ordinary_suppression(void)
{
    ValidityLog v = validity_start(false, "");

    for (unsigned kind = 0; kind < 3; kind++) {
        for (unsigned hidden = 0; hidden < 5; hidden++) {
            Shape c = ordinary_shape(kind);
            uint16_t entries[256];

            validity_entries(entries, kind);
            c.x = c.y = 124;
            c.width = 24;
            c.height = 12;
            c.x0 = 126;
            c.x1 = 145;
            c.y0 = 128;
            c.y1 = kind == 2 ? 130 : 128;
            c.primitive = kind == 0 ? 0 : kind == 1 ? 0x01040040 :
                          0x03020000;
            c.draw = 0xa0208 | (kind == 2 ? 1U << 18 : 0);
            c.stipple_mode = 0x02050000;
            c.pattern = 0xa4000000;
            /* Invalid entries never turn their lower payload into RAM. */
            entries[16] = 0x7fff;
            entries[17] = 0x8000 | page_for(kind, 17);
            if (hidden == 1) {
                c.draw |= 1U << 20;
                c.scissor = bounds(128, 128, 144, 130);
            } else if (hidden == 2) {
                c.draw &= ~0x20000U;
                c.pattern = 0;
            } else if (hidden == 3) {
                c.draw |= 0x80;
                c.plane = 0;
            } else if (hidden == 4) {
                c.draw &= ~8U;
            }
            validity_run(&v, &c, entries);
        }
    }
    validity_finish(&v);
}

static void test_ordinary_start_forms(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-ordinary-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    for (unsigned n = 0; n < ARRAY_SIZE(registers) + 4; n++) {
        Shape c = ordinary_shape(n % 3);
        unsigned offset = n < ARRAY_SIZE(registers) ? registers[n].offset :
            n == ARRAY_SIZE(registers) ? 0x1f0 :
            paired_start_offsets[n - ARRAY_SIZE(registers) - 1];
        unsigned width = n < ARRAY_SIZE(registers) ? registers[n].width :
            offset == 0x1f0 ? 4 : 8;
        bool pair = width == 8 && (offset == 0x70 || offset == 0xa8 ||
                                   offset == 0xc0);
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS], value;
        g_autofree uint32_t *expected = NULL;

        c.logic = 6;
        c.origin_x = 3;
        c.origin_y = 2;
        c.draw |= 1U << 20;
        c.scissor = bounds(101, 100, 111, 101);
        c.clip = 0x63;
        c.screen = bounds(104, 102, 116, 104);
        ordinary_prepare(s, &c);
        save_paired_renderer(s, path, before);
        value = offset == 0x1f0 ? 0x13579bdf :
            pair ? (before[192 + offset / 4] << 32) |
                   before[192 + offset / 4 + 1] : before[192 + offset / 4];
        if (offset != 0x1f0) {
            g_assert_cmpint(memtx_write(s, DE + offset, ~value, width), ==,
                            MEMTX_OK);
        }
        /* Neither member can publish while another consumed origin is bad. */
        qtest_writel(s, DE + (offset == 0x50 ? 0x58 : 0x50), 0x80000000);
        ordinary_failure(s, &c, path, DE + 0x800 + offset, value, width);
        qtest_writel(s, DE + (offset == 0x50 ? 0x58 : 0x50),
                     offset == 0x50 ? xy(c.origin_x, c.origin_y) : 0);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + offset, value, width), ==,
                        MEMTX_OK);
        expected = shape_reference(&c);
        shape_memory(s, &c, expected);
        save_paired_renderer(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        qtest_writel(s, DE + 0x1f0, 0xfedcba98);
        qtest_writel(s, DE + 0x1f8, 0);
        assert_idle(s);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_ordinary_lifecycle(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-ordinary-move-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *next = g_build_filename(dir, "next", NULL);
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *uri = g_strdup_printf("exec:cat %s", quoted);
    QTestState *from = start(), *to = start_options("-incoming defer");
    QTestState *peer = start_indexed_state();
    Shape c = ordinary_shape(2);
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    g_autofree uint32_t *expected = NULL;

    c.origin_x = 3;
    c.origin_y = 2;
    c.clip = 0x63;
    c.screen = bounds(104, 102, 116, 104);
    c.draw |= 1U << 20;
    c.scissor = bounds(102, 100, 111, 101);
    c.logic = 6;
    for (unsigned i = 0; i < 2; i++) {
        QTestState *q = i ? peer : from;

        ordinary_prepare(q, &c);
        qtest_writel(q, DE + 0x58, 0x80000001);
        qtest_writel(q, DE + 0xc0, UINT32_MAX);
    }
    save_renderer(from, path, before);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    save_paired_renderer(to, next, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    expected = ordinary_initial(&c);
    shape_memory(to, &c, expected);
    for (unsigned i = 0; i < 2; i++) {
        QTestState *q = i ? peer : to;

        qtest_irq_intercept_out_named(q, "/machine/peripheral/crime", "cpu-ip");
        ordinary_failure(q, &c, next, DE + 0x8d0, 0xdeadbeef, 4);
        qtest_writel(q, DE + 0x58, xy(c.origin_x, c.origin_y));
        ordinary_failure(q, &c, next, DE + 0x8d0, 0xdeadbeef, 4);
        qtest_writel(q, DE + 0xc0, c.stipple_mode);
        qtest_writel(q, DE + 0x9f0, 0);
        reference_shape(&c, expected);
        shape_memory(q, &c, expected);
        /* Each peer starts with the same reference image. */
        if (!i) {
            g_clear_pointer(&expected, g_free);
            expected = ordinary_initial(&c);
        }
    }
    save_paired_renderer(peer, path, before);
    save_paired_renderer(to, next, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    qtest_system_reset(to);
    shape_memory(to, &c, expected);
    save_paired_renderer(to, next, after);
    for (unsigned i = 0; i < RENDER_WORDS; i++) {
        g_assert_cmphex(after[i], ==, 0);
    }
    ordinary_prepare(to, &c);
    qtest_writel(to, DE + 0x9f0, 0);
    shape_memory(to, &c, expected);
    qtest_quit(from);
    qtest_quit(to);
    qtest_quit(peer);
    g_unlink(path);
    g_unlink(next);
    g_rmdir(dir);
}


static void test_ordinary_copies(void)
{
    const unsigned corners[][6] = {
        {7, 0, 1, 0, 6, 0}, {0, 0, 6, 0, 1, 0},
        {0, 7, 0, 1, 0, 6}, {0, 0, 0, 6, 0, 1},
        {7, 7, 1, 1, 6, 6}, {1, 0, 7, 0, 0, 0},
        {0, 1, 0, 7, 0, 0}, {1, 1, 7, 7, 0, 0},
    };
    QTestState *s = start_indexed_state();
    g_autofree char *dir = g_dir_make_tmp("crime-copy-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);

    for (unsigned alias = 0; alias < 4; alias++) {
        for (unsigned op = 0; op < 16; op++) {
            for (unsigned m = 0; m < 3; m++) {
                const unsigned *v = corners[op % ARRAY_SIZE(corners)];
                unsigned bank = op % 3;
                unsigned source_bank = alias ? (bank + 1) % 3 : bank;
                uint32_t expected[8][8], source[8][8];
                int dx = v[2] < v[0] ? -1 : 1;
                int dy = v[3] < v[1] ? -1 : 1;
                Shape c = ordinary_shape(bank);

                c.x = c.y = 124;
                c.width = c.height = 8;
                c.x0 = 124 + v[0];
                c.y0 = 124 + v[1];
                c.x1 = 124 + v[2];
                c.y1 = 124 + v[3];
                c.primitive = 0x03000000 | (dx < 0 ? 1U << 16 : 0) |
                              (dy > 0 ? 1U << 17 : 0);
                c.draw = 0x200288;
                c.logic = op;
                c.plane = m == 0 ? 255 : m == 1 ? 0xa5 : 0;
                c.clip = (op + 1) | (op + 1) << 5;
                c.screen = bounds(124, 124, 132, 132);
                c.draw |= 1U << 20;
                c.scissor = bounds(124, 124, 131, 132);
                ordinary_prepare(s, &c);
                for (unsigned y = 0; y < 2; y++) {
                    for (unsigned x = 0; x < 2; x++) {
                        unsigned index = y * 16 + x;
                        unsigned page = alias == 3 ?
                            page_for(source_bank, index) :
                            page_for(bank, index);

                        tile(s, source_bank, index,
                             page + (alias == 2 ? 0x4000 : 0));
                    }
                }
                for (unsigned y = 0; y < 8; y++) {
                    for (unsigned x = 0; x < 8; x++) {
                        expected[y][x] = 0xa1b2c300 | (y * 8 + x + 1);
                        source[y][x] = 0x98765400 |
                                       ((y * 8 * 37 + x * 37 + 0x71) & 255);
                        qtest_writel(s, pixel_address(bank,
                            124 + x, 124 + y, 4), expected[y][x]);
                        if (alias == 3) {
                            qtest_writel(s, pixel_address(source_bank,
                                124 + x, 124 + y, 4), source[y][x]);
                        }
                    }
                }
                for (unsigned y = 0; y <= abs((int)v[3] - v[1]); y++) {
                    for (unsigned x = 0; x <= abs((int)v[2] - v[0]); x++) {
                        unsigned px = v[0] + (int)x * dx;
                        unsigned py = v[1] + (int)y * dy;
                        unsigned sx = v[4] + (int)x * dx;
                        unsigned sy = v[5] + (int)y * dy;
                        uint32_t old = expected[py][px];
                        uint32_t index = alias == 3 ? source[sy][sx] :
                                                     expected[sy][sx];

                        if (124 + px < 131) {
                            uint32_t result = rop_result(op, index & 255,
                                                        old & 255, 8);

                            expected[py][px] = (old & ~c.plane) |
                                              (result & c.plane);
                        }
                    }
                }
                qtest_writel(s, DE, (source_bank << 10) | 0x200);
                qtest_writel(s, DE + 0xa8, 1);
                qtest_writel(s, DE + 0xac, 1);
                qtest_writel(s, DE + 0xa0, xy(124 + v[4], 124 + v[5]));
                {
                    unsigned n = ((alias * 16 + op) * 3 + m) %
                                 (ARRAY_SIZE(registers) + 4);
                    unsigned offset = n < ARRAY_SIZE(registers) ?
                        registers[n].offset : n == ARRAY_SIZE(registers) ?
                        0x1f0 :
                        paired_start_offsets[n - ARRAY_SIZE(registers) - 1];
                    unsigned width = n < ARRAY_SIZE(registers) ?
                        registers[n].width : offset == 0x1f0 ? 4 : 8;
                    bool pair = width == 8 &&
                        (offset == 0x70 || offset == 0xa8 || offset == 0xc0);
                    uint64_t before[RENDER_WORDS], after[RENDER_WORDS], value;

                    save_paired_renderer(s, path, before);
                    value = offset == 0x1f0 ? 0 : pair ?
                        (before[192 + offset / 4] << 32) |
                        before[192 + offset / 4 + 1] : before[192 + offset / 4];
                    if (offset != 0x1f0) {
                        g_assert_cmpint(memtx_write(s, DE + offset,
                                                   ~value, width),
                                        ==, MEMTX_OK);
                    }
                    g_assert_cmpint(memtx_write(s, DE + 0x800 + offset,
                                               value, width), ==, MEMTX_OK);
                    save_paired_renderer(s, path, after);
                    g_assert_cmpmem(before, sizeof(before),
                                    after, sizeof(after));
                }
                for (unsigned y = 0; y < 8; y++) {
                    for (unsigned x = 0; x < 8; x++) {
                        g_assert_cmphex(qtest_readl(s,
                            pixel_address(bank, 124 + x, 124 + y, 4)), ==,
                            expected[y][x]);
                        if (alias == 3) {
                            g_assert_cmphex(qtest_readl(s,
                                pixel_address(source_bank,
                                    124 + x, 124 + y, 4)), ==, source[y][x]);
                        }
                    }
                }
            }
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_ordinary_copy_faults(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-copy-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    for (unsigned fault = 0; fault < 12; fault++) {
        Shape c = ordinary_shape(0);

        c.x = c.y = c.x0 = c.y0 = 0;
        c.width = 130;
        c.height = 2;
        c.x1 = 128;
        c.y1 = 0;
        c.primitive = 0x03020000;
        c.draw = 0x200288;
        c.logic = fault & 1 ? 5 : 0;
        c.plane = 255;
        ordinary_prepare(s, &c);
        map_region(s, 1, 4, 0, 0, 129, 1);
        qtest_writel(s, DE, 0x600);
        qtest_writel(s, DE + 0xa0, 0);
        qtest_writel(s, DE + 0xa8, 1);
        qtest_writel(s, DE + 0xac, 1);
        if (fault < 4) {
            unsigned bank = fault < 2 ? 0 : 1;
            uint64_t address = RE + 0x1000 + bank * 512;

            qtest_writeq(s, address,
                qtest_readq(s, address) & ~(UINT64_C(0x8000) << 32));
        } else if (fault < 6) {
            tile(s, 0, 1, 0x4000 + page_for(0, 0));
        } else if (fault < 8) {
            qtest_writel(s, DE + (fault & 1 ? 0x50 : 0x58), 1);
        } else if (fault < 10) {
            qtest_writel(s, DE, fault & 1 ? 0x400 : 0x628);
        } else {
            qtest_writel(s, DE + (fault & 1 ? 0xa8 : 0xac), 2);
        }
        /* Invalid mappings can be hidden, but semantic failures cannot. */
        if (fault < 6) {
            qtest_writel(s, DE + 0x1b8, 0);
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            qtest_writel(s, DE + 0x1b8, 255);
        }
        ordinary_failure(s, &c, path, DE + 0x8d0, 0xdeadbeef, 4);
    }
    /* Repeated source identities do not create duplicate destinations. */
    {
        Shape c = ordinary_shape(0);

        c.x = c.y = c.x0 = c.y0 = 0;
        c.width = 130;
        c.height = 2;
        c.x1 = 128;
        c.y1 = 0;
        c.primitive = 0x03020000;
        c.draw = 0x200208;
        c.logic = 3;
        ordinary_prepare(s, &c);
        map_region(s, 1, 4, 0, 0, 127, 0);
        tile(s, 1, 1, 0x4000 + page_for(1, 0));
        for (unsigned x = 0; x < 128; x++) {
            qtest_writel(s, pixel_address(1, x, 0, 4), 0x12345600 | x);
        }
        qtest_writel(s, DE, 0x600);
        qtest_writel(s, DE + 0xa0, 0);
        qtest_writel(s, DE + 0xa8, 1);
        qtest_writel(s, DE + 0xac, 1);
        qtest_writel(s, DE + 0x9f0, 0);
        for (unsigned x = 0; x <= 128; x++) {
            g_assert_cmphex(qtest_readl(s, pixel_address(0, x, 0, 4)), ==,
                            0xa1b2c300 | (x % 128));
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_ordinary_copy_banks(void)
{
    QTestState *s = start_profile_ram("legacy-software", true, true, false, "");

    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(s, CRIME + 0x208 + bank * 8, 0x100 | bank * 4);
    }
    for (unsigned bank = 1; bank < 8; bank++) {
        unsigned page = (0x40000000ULL + bank * 0x08000000ULL) >> 16;
        uint64_t first = ((uint64_t)(page - 1) << 16) + 65532;
        uint64_t second = ((uint64_t)page << 16) + 65024;
        uint64_t destination[2] = {pixel_address(1, 127, 128, 4),
                                   pixel_address(1, 128, 128, 4)};

        setup(s, 0x600, true);
        qtest_writel(s, DE, 0x200);
        qtest_writel(s, DE + 0x18, 0x200208);
        qtest_writel(s, DE + 0x70, xy(127, 128));
        qtest_writel(s, DE + 0x74, xy(128, 128));
        qtest_writel(s, DE + 0xa0, xy(127, 127));
        tile(s, 0, 0, page - 1);
        tile(s, 0, 1, page);
        map_region(s, 1, 4, 127, 128, 128, 128);
        qtest_writel(s, first, 0x1234569b);
        qtest_writel(s, second, 0x987654ac);
        for (unsigned x = 0; x < 2; x++) {
            qtest_writel(s, destination[x], 0xa1b2c35a);
        }
        for (unsigned fault = 0; fault < 2; fault++) {
            qtest_writeq(s, CRIME + 0x208 + bank * 8,
                         fault ? bank * 4 : 0x100);
            assert_failure(s, DE + 0x8d0, 0xdeadbeef, 4);
            qtest_writeq(s, CRIME + 0x208 + bank * 8, 0x100 | bank * 4);
            for (unsigned x = 0; x < 2; x++) {
                g_assert_cmphex(qtest_readl(s, destination[x]), ==, 0xa1b2c35a);
            }
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        g_assert_cmphex(qtest_readl(s, destination[0]), ==, 0xa1b2c39b);
        g_assert_cmphex(qtest_readl(s, destination[1]), ==, 0xa1b2c3ac);
    }
    qtest_quit(s);
}

/* Native storage does not grant these parameters a START command. */
static const Register native_registers[] = {
    { 0x080, 4, false }, { 0x084, 4, false }, { 0x088, 4, false },
    { 0x08c, 4, false }, { 0x090, 4, false }, { 0x094, 4, false },
    { 0x0e0, 4, false }, { 0x0e4, 4, false }, { 0x0e8, 4, false },
    { 0x0ec, 4, false }, { 0x0f0, 4, false }, { 0x0f4, 4, false },
    { 0x0f8, 4, false }, { 0x0fc, 4, false }, { 0x100, 4, false },
    { 0x104, 4, false }, { 0x108, 4, false }, { 0x10c, 4, false },
    { 0x110, 4, true }, { 0x118, 8, true },
    { 0x120, 8, false }, { 0x128, 8, false }, { 0x130, 4, false },
    { 0x134, 4, false },
    { 0x138, 8, false }, { 0x140, 8, false }, { 0x148, 8, false },
    { 0x150, 8, false }, { 0x158, 4, false }, { 0x15c, 4, false },
    { 0x160, 4, true }, { 0x168, 4, true }, { 0x170, 4, true },
    { 0x178, 4, false }, { 0x180, 4, false }, { 0x188, 4, false },
    { 0x190, 4, false }, { 0x194, 4, false }, { 0x198, 4, true },
    { 0x1a0, 4, true }, { 0x1a8, 4, true }, { 0x1c0, 4, true },
    { 0x1c8, 8, false }, { 0x1d0, 8, false }, { 0x1d8, 8, false },
    { 0x1e0, 4, true }, { 0x1e8, 4, true },
};

static uint64_t native_pattern(unsigned index)
{
    return 0x123456789abcdef0ULL ^ ((uint64_t)(index + 1) << 40) ^
           ((index + 1) * 0x01020305ULL);
}

static void native_translations(QTestState *s, bool write, bool zero)
{
    for (unsigned i = 0; i < 256; i++) {
        uint64_t address = RE + 0x1000 + 8 * i;
        uint64_t value = zero ? 0 : native_pattern(i);

        if (write) {
            g_assert_cmpint(memtx_writeq(s, address, value), ==, MEMTX_OK);
        } else {
            uint64_t actual;

            g_assert_cmpint(memtx_readq(s, address, &actual), ==, MEMTX_OK);
            g_assert_cmphex(actual, ==, value);
        }
    }
}

static void test_native_translations(void)
{
    QTestState *s = start();
    uint64_t value;

    qtest_writeq(s, 0x30000, 0x76543210fedcba98ULL);
    native_translations(s, false, true);
    native_translations(s, true, false);
    for (unsigned i = 0; i < 256; i++) {
        uint64_t address = RE + 0x1000 + 8 * i;
        uint8_t bytes[8];

        qtest_memread(s, address, bytes, sizeof(bytes));
        g_assert_cmphex(ldq_be_p(bytes), ==, native_pattern(i));
        for (unsigned width = 1; width < 8; width *= 2) {
            g_assert_cmpint(memtx_write(s, address, 0, width), !=, MEMTX_OK);
            g_assert_cmpint(memtx_read(s, address, &value, width),
                            !=, MEMTX_OK);
        }
        for (unsigned delta = 1; delta < 8; delta++) {
            g_assert_cmpint(memtx_writeq(s, address + delta, 0), !=, MEMTX_OK);
            g_assert_cmpint(memtx_readq(s, address + delta, &value),
                            !=, MEMTX_OK);
        }
    }
    g_assert_cmpint(memtx_writeq(s, RE + 0x1800, UINT64_MAX), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readq(s, RE + 0x1800, &value), !=, MEMTX_OK);
    native_translations(s, false, false);
    g_assert_cmphex(qtest_readq(s, 0x30000), ==, 0x76543210fedcba98ULL);
    assert_idle(s);
    qtest_system_reset(s);
    native_translations(s, false, true);
    qtest_quit(s);
}

static void native_parameters(QTestState *s, bool write, bool zero)
{
    for (unsigned i = 0; i < ARRAY_SIZE(native_registers); i++) {
        const Register *r = &native_registers[i];
        uint64_t value = zero ? 0 : native_pattern(r->offset);
        uint64_t actual;

        if (r->width == 4) {
            value = (uint32_t)value;
        }
        if (write) {
            g_assert_cmpint(memtx_write(s, DE + r->offset, value, r->width),
                            ==, MEMTX_OK);
        } else if (r->readable) {
            g_assert_cmpint(memtx_read(s, DE + r->offset, &actual, r->width),
                            ==, MEMTX_OK);
            g_assert_cmphex(actual, ==, value);
        } else {
            g_assert_cmpint(memtx_read(s, DE + r->offset, &actual, r->width),
                            !=, MEMTX_OK);
        }
    }
}

static void test_native_parameters(void)
{
    const unsigned holes[] = { 0x2054, 0x205c, 0x2064, 0x2098,
                               0x20a4, 0x2114, 0x2174, 0x21ec, 0x408 };
    QTestState *s = start();
    uint64_t value;

    native_parameters(s, false, true);
    native_parameters(s, true, false);
    for (unsigned i = 0; i < ARRAY_SIZE(native_registers); i++) {
        const Register *r = &native_registers[i];

        for (unsigned width = 1; width <= 8; width *= 2) {
            if (width != r->width) {
                bool gl_pair = width == 8 && (r->offset == 0x80 ||
                    r->offset == 0x88 || r->offset == 0x90 ||
                    r->offset == 0xe0 || r->offset == 0xe8 ||
                    r->offset == 0xf0 || r->offset == 0xf8 ||
                    r->offset == 0x100 || r->offset == 0x108 ||
                    r->offset == 0x190);

                if (gl_pair) {
                    g_assert_cmpint(memtx_write(s, DE + r->offset, 0, width),
                                    ==, MEMTX_OK);
                } else {
                    g_assert_cmpint(memtx_write(s, DE + r->offset, 0, width),
                                    !=, MEMTX_OK);
                }
                g_assert_cmpint(memtx_read(s, DE + r->offset, &value, width),
                                !=, MEMTX_OK);
            }
        }
        for (unsigned delta = 1; delta < r->width; delta++) {
            g_assert_cmpint(memtx_write(s, DE + r->offset + delta, 0,
                                        r->width), !=, MEMTX_OK);
            g_assert_cmpint(memtx_read(s, DE + r->offset + delta, &value,
                                       r->width), !=, MEMTX_OK);
        }
        if (r->readable) {
            uint8_t bytes[8];
            uint64_t expected = native_pattern(r->offset);

            qtest_memread(s, DE + r->offset, bytes, r->width);
            uint64_t actual = r->width == 8 ? ldq_be_p(bytes) :
                              (uint32_t)ldl_be_p(bytes);

            g_assert_cmphex(actual, ==,
                            r->width == 8 ? expected : (uint32_t)expected);
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(holes); i++) {
        g_assert_cmpint(memtx_read(s, RE + holes[i], &value, 4), !=, MEMTX_OK);
        /* Only +0x408 admits a zero write, as a logged no-op. */
        g_assert_cmpint(memtx_writel(s, RE + holes[i], 0) == MEMTX_OK, ==,
                        holes[i] == 0x408);
        g_assert_cmpint(memtx_writel(s, RE + holes[i], UINT32_MAX),
                        !=, MEMTX_OK);
    }
    native_parameters(s, false, false);
    qtest_system_reset(s);
    native_parameters(s, false, true);
    qtest_quit(s);
}

typedef struct TexturePlaneStart {
    unsigned offset, width, header;
    uint64_t value;
} TexturePlaneStart;

static const TexturePlaneStart texture_plane_starts[] = {
    {0x120, 8, 0x3a24, (UINT64_C(3) << 42) + 0x800},
    {0x128, 8, 0x3a25, (UINT64_C(3) << 42) + 0x800},
    {0x130, 4, 0x3226, 0x1f3fe5c0},
    {0x138, 8, 0x3a27, UINT64_C(1) << 41},
    {0x140, 8, 0x3a28, UINT64_C(1) << 39},
    {0x148, 8, 0x3a29, UINT64_C(1) << 39},
    {0x150, 8, 0x3a2a, UINT64_C(1) << 41},
    {0x158, 4, 0x322b, 0x02000000},
    {0x15c, 4, 0x2a2b, 0x01000000},
};

static bool texture_plane_start(unsigned offset)
{
    for (unsigned i = 0; i < ARRAY_SIZE(texture_plane_starts); i++) {
        if (texture_plane_starts[i].offset == offset) {
            return true;
        }
    }
    return false;
}

static void test_native_isolation(void)
{
    g_autofree char *dir =
        g_dir_make_tmp("crime-native-isolation-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start();
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];

    native_translations(s, true, false);
    native_parameters(s, true, false);
    tile(s, 0, 0, 3);
    setup(s, 0x228, false);
    qtest_writel(s, DE + 0x18, 0x2f8);
    qtest_writel(s, DE + 0x1b0, 6);
    qtest_writel(s, DE + 0xd0, 0x1234abcd);
    qtest_writel(s, DE + 0x70, xy(0, 0));
    qtest_writel(s, DE + 0x74, xy(1, 0));
    qtest_memset(s, 0x30000, 0x5a, 16);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    for (unsigned x = 0; x < 2; x++) {
        g_assert_cmphex(qtest_readl(s, 0x30000 + 4 * x), ==,
                        0x5a5a5a5a ^ 0x1234abcd);
    }
    native_parameters(s, true, false);
    native_parameters(s, false, false);
    qtest_writel(s, DE + 0x60, 0x01000000);
    qtest_writel(s, DE + 0x70, xy(2, 0));
    qtest_writel(s, DE + 0x74, xy(3, 0));
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    save_paired_renderer(s, path, before);
    for (unsigned i = 0; i < ARRAY_SIZE(native_registers); i++) {
        const Register *r = &native_registers[i];
        uint64_t value = r->width == 8 ? UINT64_MAX : UINT32_MAX;

        /* Cleared families have their own prospective START checks. */
        if ((r->offset >= 0xe0 && r->offset <= 0x10c) ||
            (r->offset >= 0x1c0 && r->offset <= 0x1e8) ||
            texture_plane_start(r->offset)) {
            continue;
        }
        g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset, value,
                                    r->width), !=, MEMTX_OK);
    }
    save_paired_renderer(s, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    for (unsigned x = 0; x < 4; x++) {
        g_assert_cmphex(qtest_readl(s, 0x30000 + 4 * x), ==,
                        0x5a5a5a5a ^ 0x1234abcd);
    }
    assert_idle(s);
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_native_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-native-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *from = start();
    QTestState *to = start_options("-incoming defer");
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];

    native_translations(from, true, false);
    native_parameters(from, true, false);
    qtest_writel(from, DE + 8, 0xdeadbeef);
    qtest_writeq(from, 0x30000, 0x1122334455667788ULL);
    save_paired_renderer(from, path, before);
    for (unsigned i = 0; i < ARRAY_SIZE(native_registers); i++) {
        const Register *r = &native_registers[i];
        uint64_t expected = native_pattern(r->offset);

        g_assert_cmphex(before[192 + r->offset / 4], ==,
                        r->width == 8 ? expected : (uint32_t)expected);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    native_translations(to, false, false);
    native_parameters(to, false, false);
    save_paired_renderer(to, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    g_assert_cmphex(qtest_readl(to, DE + 8), ==, 0xdeadbeef);
    g_assert_cmpint(memtx_writel(to, DE + 0x9f0, 0), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(to, 0x30000), ==, 0x1122334455667788ULL);
    tile(to, 0, 0, 3);
    setup(to, 0x228, false);
    qtest_writel(to, DE + 0x18, 0x2f8);
    qtest_writel(to, DE + 0x1b0, 6);
    qtest_writel(to, DE + 0xd0, 0x1234abcd);
    qtest_writel(to, DE + 0x70, xy(0, 0));
    qtest_writel(to, DE + 0x74, xy(1, 0));
    g_assert_cmpint(memtx_writel(to, DE + 0x9f0, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(to, 0x30000), ==,
                    0x1122334455667788ULL ^ 0x1234abcd1234abcdULL);
    qtest_system_reset(to);
    native_translations(to, false, true);
    native_parameters(to, false, true);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_native_clear_sources(void)
{
    QTestState *s = start();
    const unsigned sources[] = { 3, 6 };

    qtest_writeq(s, RE + 0x1600, 0x8003000000000000ULL);
    qtest_writeq(s, RE + 0x16e0, 0x8003000000000000ULL);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x18, 0x12345655);
    qtest_writel(s, MTE + 0x20, UINT32_MAX);
    qtest_writel(s, MTE + 0x28, UINT32_MAX);
    qtest_writel(s, MTE + 0x30, xy(7, 0));
    qtest_writel(s, MTE + 0x38, xy(9, 0));
    qtest_writel(s, MTE + 0x40, UINT32_MAX);
    qtest_writel(s, MTE + 0x48, 1);
    for (unsigned n = 0; n < ARRAY_SIZE(sources); n++) {
        qtest_memset(s, 0x30000, 0x5a, 16);
        qtest_writel(s, MTE, sources[n] << 5 | 6 << 2);
        g_assert_cmpint(memtx_writel(s, MTE + 0x870, 0), ==, MEMTX_OK);
        for (unsigned i = 0; i < 16; i++) {
            g_assert_cmphex(qtest_readb(s, 0x30000 + i), ==,
                            i >= 7 && i <= 9 ? 0x55 : 0x5a);
        }
        g_assert_cmphex(qtest_readq(s, RE + 0x16e0), ==,
                        0x8003000000000000ULL);
        qtest_writel(s, MTE, 0x800 | sources[n] << 5 | 6 << 2);
        g_assert_cmpint(memtx_writel(s, MTE + 0x870, 0), !=, MEMTX_OK);
        for (unsigned i = 0; i < 16; i++) {
            g_assert_cmphex(qtest_readb(s, 0x30000 + i), ==,
                            i >= 7 && i <= 9 ? 0x55 : 0x5a);
        }
    }
    qtest_writel(s, MTE, 3 << 2);
    g_assert_cmpint(memtx_writel(s, MTE + 0x870, 0), !=, MEMTX_OK);
    qtest_quit(s);
}

static void test_native_profiles(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *s = start_profile(legacy ? "legacy-software" : "1.5",
                                      false, !legacy, "");
        uint64_t value;

        for (unsigned i = 0; i < ARRAY_SIZE(native_registers); i++) {
            const Register *r = &native_registers[i];

            g_assert_cmpint(memtx_write(s, DE + r->offset, UINT64_MAX,
                                        r->width), !=, MEMTX_OK);
            g_assert_cmpint(memtx_read(s, DE + r->offset, &value, r->width),
                            !=, MEMTX_OK);
        }
        for (unsigned i = 0; i < 32; i++) {
            g_assert_cmpint(memtx_writeq(s, RE + 0x1600 + 8 * i, UINT64_MAX),
                            !=, MEMTX_OK);
            g_assert_cmpint(memtx_readq(s, RE + 0x1600 + 8 * i, &value),
                            !=, MEMTX_OK);
        }
        qtest_quit(s);
    }
}

#define ALPHA_ENABLE (1U << 11)
#define ALPHA_STATE (DE + 0x198)

static Shape alpha_shape(void)
{
    Shape c = shape_default();

    c.bytes = 4;
    c.width = 10;
    c.height = 6;
    c.x0 = c.y0 = 1;
    c.x1 = 7;
    c.y1 = 3;
    c.primitive = 0x03020000;
    c.draw |= ALPHA_ENABLE;
    c.foreground = 0x12345680;
    c.old = 0x89abcdef;
    return c;
}

static uint32_t *alpha_expected(const Shape *c, bool pass)
{
    uint32_t *expected;

    if (pass) {
        return shape_reference(c);
    }
    expected = g_new(uint32_t, c->width * c->height);
    for (unsigned i = 0; i < c->width * c->height; i++) {
        expected[i] = c->old;
    }
    return expected;
}

static void alpha_prepare(QTestState *s, const Shape *c, uint32_t alpha)
{
    map_region(s, c->bank, c->bytes, c->x, c->y,
               c->x + c->width - 1, c->y + c->height - 1);
    shape_memory(s, c, NULL);
    shape_stage(s, c);
    qtest_writel(s, ALPHA_STATE, alpha);
}

static void alpha_draw(QTestState *s, const Shape *c, uint32_t alpha, bool pass)
{
    g_autofree uint32_t *expected = alpha_expected(c, pass);

    alpha_prepare(s, c, alpha);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    shape_memory(s, c, expected);
    g_assert_cmphex(qtest_readl(s, ALPHA_STATE), ==, alpha);
    assert_idle(s);
}

static void test_alpha_comparisons(void)
{
    /* The column is LESS/EQUAL/GREATER; the function encodes that truth set. */
    const struct {
        uint8_t reference, alpha, column;
    } values[] = {
        { 0x80, 0x7f, 0 }, { 0x80, 0x80, 1 }, { 0x80, 0x81, 2 },
        { 0, 0, 1 }, { 0, 255, 2 }, { 255, 0, 0 }, { 255, 255, 1 },
    };
    QTestState *s = start();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned function = 0; function < 8; function++) {
            for (unsigned n = 0; n < ARRAY_SIZE(values); n++) {
                for (unsigned old_alpha = 0; old_alpha < 2; old_alpha++) {
                    Shape c = alpha_shape();
                    bool pass = function & (1U << values[n].column);

                    c.bank = bank;
                    c.primitive = 0;
                    c.x1 = c.y1 = 65535; /* Unused point endpoint. */
                    c.draw &= ~0x200U;
                    c.foreground = 0x12345600 | values[n].alpha;
                    c.old = 0x89abcd00 | (old_alpha ? 255 : 0);
                    alpha_draw(s, &c, function << 8 | values[n].reference,
                               pass);
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_alpha_geometry(void)
{
    QTestState *s = start();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned primitive = 0; primitive < 4; primitive++) {
            for (unsigned mask = 0; mask < 3; mask++) {
                Shape c = alpha_shape();

                c.bank = bank;
                c.x = 123;
                c.y = 125;
                c.width = 15;
                c.height = 8;
                c.x0 = 124;
                c.y0 = 127;
                c.x1 = 135;
                c.y1 = primitive == 1 ? 127 : 130;
                c.primitive = primitive == 0 ? 0 :
                              primitive == 3 ? 0x03020000 : 0x01000021;
                c.old = 0x89abcd00 | (mask ? 255 : 0);
                if (primitive == 2 && mask == 2) {
                    c.primitive |= 1U << 18;
                }
                if (mask == 1) {
                    c.draw &= ~8U; /* Preserve the stored alpha byte. */
                    c.plane = 0x0ff0a55a;
                } else if (mask == 2) {
                    c.plane = 0x00ff0000;
                    c.draw |= 1U << 20;
                    c.scissor = bounds(125, 126, 135, 131);
                    c.clip = 0x21;
                    c.screen = bounds(125, 127, 136, 132);
                }
                alpha_draw(s, &c, 0x181, true);  /* 0x80 < 0x81. */
                alpha_draw(s, &c, 0x180, false); /* Strict comparison. */
            }
        }
    }
    Shape c = alpha_shape();

    c.primitive = 0;
    /* Stipple and opaque controls are inactive for points. */
    c.draw |= 0xe0000;
    c.stipple_mode = UINT32_MAX;
    alpha_draw(s, &c, 0x700, true);
    qtest_quit(s);
}

static void alpha_failure(QTestState *s, const Shape *c, const char *path,
                           uint64_t address, uint64_t value, unsigned width)
{
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    uint64_t error = qtest_readq(s, CRIME + 0x250);
    uint64_t error_address = qtest_readq(s, CRIME + 0x258);
    uint64_t cause = qtest_readq(s, CRIME + 0x10);
    uint32_t status = qtest_readl(s, STATUS);
    g_autofree uint32_t *expected = alpha_expected(c, false);

    save_paired_renderer(s, path, before);
    g_assert_cmpint(memtx_write(s, address, value, width), !=, MEMTX_OK);
    save_paired_renderer(s, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    shape_memory(s, c, expected);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x250), ==, error);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x258), ==, error_address);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x10), ==, cause);
    g_assert_cmphex(qtest_readl(s, STATUS), ==, status);
}

static void test_alpha_validity(void)
{
    ValidityLog v = validity_start(false, "");
    g_autofree char *path = g_build_filename(v.directory, "state", NULL);
    uint16_t entries[256];

    for (unsigned form = 0; form < 2; form++) {
        for (unsigned mapping = 0; mapping < 4; mapping++) {
            for (unsigned pass = 0; pass < 2; pass++) {
                Shape c = alpha_shape();
                unsigned suppressed = 0;
                uint64_t words[RENDER_WORDS];
                uint32_t alpha = pass ? 0x780 : 0x080;
                uint64_t address = form ? DE + 0x9b8 : DE + 0x9f0;
                uint32_t value = form ? 0x0fffffff : 0;
                g_autofree uint32_t *expected = NULL;

                c.x = 123;
                c.y = 126;
                c.width = 266;
                c.height = 4;
                c.x0 = 124;
                c.x1 = 387;
                c.y0 = 127;
                c.y1 = 128;
                c.plane = form ? value : UINT32_MAX;
                c.entries = entries;
                c.suppressed = &suppressed;
                for (unsigned i = 0; i < 256; i++) {
                    entries[i] = 0x8000 | page_for(0, i);
                    if (mapping == 0 || i == 1 || i == 17) {
                        entries[i] &= 0x7fff;
                    }
                    if (mapping == 1) {
                        entries[i] = 0x8400; /* Set-valid, absent RAM. */
                    }
                }
                if (mapping == 3) {
                    entries[2] = 0x9fc0; /* Late set-valid ROM aperture. */
                }
                alpha_prepare(v.q, &c, alpha);
                validity_program(v.q, 0, entries);
                qtest_writeq(v.q, 0, 0x8383838383838383ULL);
                if (form) {
                    qtest_writel(v.q, DE + 0x1b8, 0);
                }
                if (pass && (mapping == 1 || mapping == 3)) {
                    alpha_failure(v.q, &c, path, address, value, 4);
                } else {
                    expected = alpha_expected(&c, pass);
                    g_assert_cmpint(memtx_writel(v.q, address, value),
                                    ==, MEMTX_OK);
                    shape_memory(v.q, &c, expected);
                    validity_record(&v, &c, suppressed);
                    save_paired_renderer(v.q, path, words);
                    if (form) {
                        g_assert_cmphex(words[192 + 0x1b8 / 4], ==, value);
                    }
                }
                validity_words(v.q, 0, entries);
                g_assert_cmphex(qtest_readq(v.q, 0), ==,
                                0x8383838383838383ULL);
                g_assert_cmphex(qtest_readq(v.q, CRIME + 0x10), ==, 1U << 4);
            }
        }
    }
    for (unsigned hidden = 0; hidden < 2; hidden++) {
        Shape c = alpha_shape();
        g_autofree uint32_t *expected = NULL;
        unsigned suppressed = 0;

        c.entries = entries;
        c.suppressed = &suppressed;
        if (hidden) {
            c.draw |= 1U << 20;
            c.scissor = bounds(0, 0, 0, 0);
        } else {
            c.plane = 0;
        }
        for (unsigned i = 0; i < 256; i++) {
            entries[i] = 0x8400;
        }
        expected = alpha_expected(&c, true);
        alpha_prepare(v.q, &c, 0x700);
        validity_program(v.q, 0, entries);
        g_assert_cmpint(memtx_writel(v.q, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(v.q, &c, expected);
        validity_record(&v, &c, suppressed);
    }
    g_unlink(path);
    validity_finish(&v);
}

static void test_alpha_admission(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-alpha-admission-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    const uint32_t bad_alpha[] = {
        0x880, 0x980, 0xa80, 0xb80, 0xc80, 0xd80, 0xe80, 0xf80,
        0x1000, 0x80000000, UINT32_MAX,
    };
    const uint32_t bad_modes[] = { 0, 0x124, 0x200, 0x224, 0x229, 0x1028 };

    for (unsigned n = 0; n < ARRAY_SIZE(bad_alpha); n++) {
        for (unsigned hidden = 0; hidden < 2; hidden++) {
            Shape c = alpha_shape();

            if (hidden) {
                c.draw |= 1U << 20;
                c.scissor = bounds(0, 0, 0, 0);
            } else {
                c.plane = 0;
            }
            alpha_prepare(s, &c, bad_alpha[n]);
            alpha_failure(s, &c, path, DE + 0x9f0, 0, 4);
        }
    }
    for (unsigned n = 0; n < ARRAY_SIZE(bad_modes); n++) {
        Shape c = alpha_shape();

        alpha_prepare(s, &c, 0); /* NEVER cannot hide an unsupported format. */
        qtest_writel(s, DE + 8, bad_modes[n]);
        alpha_failure(s, &c, path, DE + 0x9f0, 0, 4);
    }
    for (unsigned bit = 0; bit < 32; bit++) {
        Shape c = alpha_shape();

        if (0x3e0af8U & (1U << bit)) {
            continue;
        }
        c.draw |= 1U << bit;
        alpha_prepare(s, &c, 0);
        if (bit == 22) {
            g_autofree uint32_t *expected = alpha_expected(&c, false);

            /* Native GL rectangles can now consume foreground alpha. */
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            shape_memory(s, &c, expected);
        } else {
            alpha_failure(s, &c, path, DE + 0x9f0, 0, 4);
        }
    }
    for (unsigned primitive = 1; primitive <= 3; primitive++) {
        Shape c = alpha_shape();

        c.primitive = primitive == 3 ? 0x03020000 : primitive << 24;
        c.draw |= primitive == 1 ? 1U << 19 : 1U << 18;
        alpha_prepare(s, &c, 0);
        alpha_failure(s, &c, path, DE + 0x9f0, 0, 4);
    }
    Shape c = alpha_shape();

    c.draw |= 1U << 21;
    alpha_prepare(s, &c, 0);
    map_region(s, 1, 4, 0, 0, 15, 15);
    qtest_writel(s, DE, 0x628);
    qtest_writel(s, DE + 0xa0, xy(c.x0, c.y0));
    qtest_writel(s, DE + 0xa8, 1);
    qtest_writel(s, DE + 0xac, 1);
    alpha_failure(s, &c, path, DE + 0x9f0, 0, 4);
    for (unsigned y = c.y0; y <= c.y1; y++) {
        for (unsigned x = c.x0; x <= c.x1; x++) {
            qtest_writel(s, pixel_address(1, x, y, 4), 0x10203040);
        }
    }
    c.draw &= ~ALPHA_ENABLE;
    c.foreground = 0x10203040;
    g_autofree uint32_t *copied = alpha_expected(&c, true);

    qtest_writel(s, DE + 0x18, c.draw);
    qtest_writel(s, ALPHA_STATE, UINT32_MAX);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    shape_memory(s, &c, copied);
    for (unsigned bytes = 1; bytes <= 4; bytes *= 2) {
        c = alpha_shape();
        c.bytes = bytes;
        c.draw &= ~ALPHA_ENABLE;
        c.draw |= 1U << 18;
        c.stipple_mode = 0x030b0102;
        qtest_writel(s, ALPHA_STATE, UINT32_MAX);
        shape_run(s, &c);
    }
    qtest_writel(s, DE + 0x18, ALPHA_ENABLE);
    qtest_writel(s, ALPHA_STATE, UINT32_MAX);
    qtest_writeq(s, RE + 0x1700, 0x8000003080000031ULL);
    qtest_writel(s, MTE, 4 << 2);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x18, 0x55);
    qtest_writel(s, MTE + 0x30, 0);
    qtest_writel(s, MTE + 0x48, 1);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 3), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, 0x30000), ==, 0x55555555);
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_alpha_start_forms(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-alpha-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(registers) + 4; n++) {
        const unsigned pairs[] = { 0x70, 0xa8, 0xc0 };
        Shape c = alpha_shape();
        unsigned offset = n < ARRAY_SIZE(registers) ? registers[n].offset :
            n == ARRAY_SIZE(registers) ? 0x1f0 :
            pairs[n - ARRAY_SIZE(registers) - 1];
        unsigned width = n < ARRAY_SIZE(registers) ? registers[n].width :
            n > ARRAY_SIZE(registers) ? 8 : 4;
        bool pair = width == 8 && (offset == 0x70 || offset == 0xa8 ||
                                   offset == 0xc0);
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
        uint64_t value = offset == 0x1f0 ? 0 : shape_parameter(&c, offset);
        g_autofree uint32_t *expected = alpha_expected(&c, true);

        if (pair) {
            value = (value << 32) | shape_parameter(&c, offset + 4);
        }
        alpha_prepare(s, &c, 0x280);
        if (offset != 0x1f0) {
            g_assert_cmpint(memtx_write(s, DE + offset, ~value, width),
                            ==, MEMTX_OK);
        }
        save_paired_renderer(s, path, before);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + offset, value, width),
                        ==, MEMTX_OK);
        shape_memory(s, &c, expected);
        save_paired_renderer(s, path, after);
        if (offset != 0x1f0) {
            before[192 + offset / 4] = pair ? value >> 32 : value;
            if (pair) {
                before[192 + offset / 4 + 1] = (uint32_t)value;
            }
        }
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        qtest_writel(s, ALPHA_STATE, 0x12345678);
        qtest_writel(s, DE + 0x1f0, 0x87654321);
        qtest_writel(s, DE + 0x1f8, 0);
        assert_idle(s);
        shape_memory(s, &c, expected);
    }
    for (unsigned enable = 0; enable < 2; enable++) {
        Shape c = alpha_shape();
        g_autofree uint32_t *expected = NULL;

        if (!enable) {
            c.draw &= ~ALPHA_ENABLE;
        }
        expected = alpha_expected(&c, !enable);
        alpha_prepare(s, &c, 0);
        qtest_writel(s, DE + 0x18, c.draw ^ ALPHA_ENABLE);
        g_assert_cmpint(memtx_writel(s, DE + 0x818, c.draw), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_alpha_state(void)
{
    const uint32_t values[] = { 0, UINT32_MAX, 0xa5b6c7d8 };
    g_autofree char *dir = g_dir_make_tmp("crime-alpha-state-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    Shape c = alpha_shape();

    for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
        alpha_prepare(s, &c, values[i]);
        g_assert_cmphex(qtest_readl(s, ALPHA_STATE), ==, values[i]);
        for (unsigned width = 1; width <= 8; width *= 2) {
            uint64_t value;

            if (width != 4) {
                alpha_failure(s, &c, path, ALPHA_STATE, UINT64_MAX, width);
                g_assert_cmpint(memtx_read(s, ALPHA_STATE, &value, width),
                                !=, MEMTX_OK);
            }
        }
        for (unsigned delta = 1; delta < 4; delta++) {
            alpha_failure(s, &c, path, ALPHA_STATE + delta, 0, 4);
        }
        alpha_failure(s, &c, path, ALPHA_STATE - 4, UINT64_MAX, 8);
        for (unsigned alias = 0; alias < 2; alias++) {
            uint64_t value;
            uint64_t address = ALPHA_STATE + (alias ? 0x800 : 4);

            alpha_failure(s, &c, path, address, UINT32_MAX, 4);
            g_assert_cmpint(memtx_read(s, address, &value, 4), !=, MEMTX_OK);
        }
        g_assert_cmphex(qtest_readl(s, ALPHA_STATE), ==, values[i]);
    }
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, ALPHA_STATE), ==, 0);
    g_assert_cmphex(qtest_readl(s, DE + 0x18), ==, 0);
    g_autofree uint32_t *expected = alpha_expected(&c, false);

    shape_memory(s, &c, expected);
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_alpha_migration(void)
{
    const uint32_t values[] = { 0x280, 0, UINT32_MAX, 0xa5b6c7d8 };

    for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
        g_autofree char *dir =
            g_dir_make_tmp("crime-alpha-migrate-XXXXXX", NULL);
        g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
        g_autofree char *uri = g_strdup_printf("unix:%s", sock);
        g_autofree char *path = g_build_filename(dir, "state", NULL);
        QTestState *from = start();
        QTestState *to = start_options("-incoming defer");
        Shape c = alpha_shape();
        g_autofree uint32_t *expected = alpha_expected(&c, i != 1);
        g_autofree uint32_t *unchanged = alpha_expected(&c, false);

        alpha_prepare(from, &c, values[i]);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        g_assert_cmphex(qtest_readl(to, ALPHA_STATE), ==, values[i]);
        g_assert_cmphex(qtest_readl(to, DE + 0x18), ==, c.draw);
        shape_memory(to, &c, unchanged);
        if (i >= 2) {
            alpha_failure(to, &c, path, DE + 0x9f0, 0, 4);
            qtest_writel(to, DE + 0x18, c.draw & ~ALPHA_ENABLE);
        }
        g_assert_cmpint(memtx_writel(to, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(to, &c, expected);
        qtest_readl(to, ALPHA_STATE);
        qtest_writel(to, DE + 0x1f0, 0x87654321);
        qtest_writel(to, DE + 0x1f8, 0);
        assert_idle(to);
        shape_memory(to, &c, expected);
        qtest_system_reset(to);
        g_assert_cmphex(qtest_readl(to, ALPHA_STATE), ==, 0);
        g_assert_cmphex(qtest_readl(to, DE + 0x18), ==, 0);
        shape_memory(to, &c, expected);
        qtest_quit(from);
        qtest_quit(to);
        g_unlink(sock);
        g_unlink(path);
        g_rmdir(dir);
    }
}

/* Use the native Meson groups while retaining one TAP plan per process. */
static GPtrArray *group_paths;

static void take_group_paths(int *argc, char **argv)
{
    int kept = 1;

    for (int i = 1; i < *argc; i++) {
        if (g_str_equal(argv[i], "--group-path") && i + 1 < *argc) {
            if (!group_paths) {
                group_paths = g_ptr_array_new();
            }
            g_ptr_array_add(group_paths, argv[++i]);
        } else {
            argv[kept++] = argv[i];
        }
    }
    argv[kept] = NULL;
    *argc = kept;
}

static bool in_group(const char *path)
{
    g_autofree char *full = g_strdup_printf("/%s/%s", qtest_get_arch(),
                                            path);
    g_autofree char *suite = g_path_get_dirname(full);

    if (!group_paths) {
        return true;
    }
    for (guint i = 0; i < group_paths->len; i++) {
        const char *taken = g_ptr_array_index(group_paths, i);

        if (g_str_equal(taken, full) || g_str_equal(taken, suite)) {
            return true;
        }
    }
    return false;
}

static void add_case(const char *path, GTestFunc fn)
{
    if (in_group(path)) {
        qtest_add_func(path, fn);
    }
}

static void add_data_case(const char *path, gconstpointer data,
                          GTestDataFunc fn)
{
    if (in_group(path)) {
        qtest_add_data_func(path, data, fn);
    }
}


#define TRIANGLE_DRAW 0x00400078U
#define TRIANGLE_COLOR 0x1234abcdU


static void triangle_irq_levels(QTestState *s, unsigned levels)
{
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmpint(qtest_get_irq(s, i), ==, !!(levels & (1U << i)));
    }
}

static void triangle_irq_baseline(QTestState *s)
{
    qtest_writeq(s, CRIME + 0x18, 1U << 4);
    qtest_set_irq_in(s, "/machine/peripheral/crime", "mace", 4, 0);
    qtest_set_irq_in(s, "/machine/peripheral/crime", "mace", 4, 1);
    triangle_irq_levels(s, 1);
}

static QTestState *triangle_start(const char *extra)
{
    QTestState *s = start_options(extra);

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    triangle_irq_baseline(s);
    return s;
}

static uint32_t triangle_raw(int units)
{
    g_assert_cmpint(units, >=, -262144);
    g_assert_cmpint(units, <=, 262143);
    return 0x48040000U + units;
}

static Shape triangle_prepare(QTestState *s, unsigned bank,
                               unsigned x, unsigned y)
{
    Shape c = shape_default();

    c.bank = bank;
    c.bytes = 4;
    c.x = x;
    c.y = y;
    c.width = c.height = 8;
    c.old = 0x89674523;
    map_region(s, bank, 4, x, y, x + 7, y + 7);
    shape_memory(s, &c, NULL);
    setup(s, 0x228 | (bank << 10), false);
    qtest_writel(s, DE + 0x58, 0xf000f000U);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW);
    qtest_writel(s, DE + 0x60, 0x02020000);
    return c;
}

static void triangle_vertices(QTestState *s, const int vertex[3][2],
                               const unsigned order[3], bool paired)
{
    for (unsigned i = 0; i < 3; i++) {
        unsigned n = order ? order[i] : i;
        uint32_t x = triangle_raw(vertex[n][0]);
        uint32_t y = triangle_raw(vertex[n][1]);

        if (paired) {
            g_assert_cmpint(memtx_writeq(s, DE + 0x80 + 8 * i,
                                        (uint64_t)x << 32 | y), ==, MEMTX_OK);
        } else {
            qtest_writel(s, DE + 0x80 + 8 * i, x);
            qtest_writel(s, DE + 0x84 + 8 * i, y);
        }
    }
}

/* Explicit row fixtures, rather than the device's edge equations. */
static void triangle_pixels(QTestState *s, const Shape *c,
                             const uint8_t rows[8], uint32_t color,
                             unsigned rop, uint32_t mask)
{
    uint32_t expected[64];

    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            uint32_t value = rop_result(rop, color, c->old, 32);

            expected[8 * y + x] = rows[y] & (1U << x) ?
                (c->old & ~mask) | (value & mask) : c->old;
        }
    }
    shape_memory(s, c, expected);
}

static const int triangle_integer[3][2] = {
    { 64, 64 }, { 320, 64 }, { 64, 320 },
};
static const uint8_t triangle_ten[8] = { 0, 0x1e, 0x0e, 0x06, 0x02 };
static const uint8_t triangle_empty[8];

static void test_triangle_packet(void)
{
    QTestState *s = triangle_start("");
    Shape c;

    native_parameters(s, true, false);
    c = triangle_prepare(s, 0, 0, 0);

    qtest_writel(s, DE + 0xd0, 0xdeadbeef);
    for (unsigned i = 0; i < 3; i++) {
        uint64_t pair = (uint64_t)triangle_raw(triangle_integer[i][0]) << 32 |
                        triangle_raw(triangle_integer[i][1]);

        g_assert_cmpint(memtx_writeq(s, DE + 0x80 + 8 * i, pair),
                        ==, MEMTX_OK);
        triangle_pixels(s, &c, triangle_empty, 0, 3, UINT32_MAX);
    }
    qtest_writel(s, DE + 0x1b0, UINT32_MAX);
    qtest_writel(s, DE + 0x1b8, 0);
    g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==, MEMTX_OK);
    triangle_pixels(s, &c, triangle_ten, TRIANGLE_COLOR, 3, UINT32_MAX);
    shape_memory(s, &c, NULL);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x200);
    qtest_writel(s, DE + 0x50, UINT32_MAX);
    qtest_writel(s, DE, UINT32_MAX);
    qtest_writel(s, DE + 0xa8, UINT32_MAX);
    qtest_writel(s, DE + 0xac, UINT32_MAX);
    qtest_writel(s, DE + 0xc0, UINT32_MAX);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x0a0200);
    qtest_writel(s, DE + 0x1b0, 6);
    g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==, MEMTX_OK);
    triangle_pixels(s, &c, triangle_ten, TRIANGLE_COLOR, 6, UINT32_MAX);
    qtest_writel(s, DE + 0x1f0, UINT32_MAX);
    qtest_writel(s, DE + 0x1f8, 0);
    g_assert_cmphex(qtest_readl(s, STATUS) & ~0x3ffffU, ==, 0x1e000000);
    g_assert_cmphex(qtest_readq(s, CRIME + 0x10), ==, 1U << 4);
    triangle_irq_levels(s, 1);
    triangle_pixels(s, &c, triangle_ten, TRIANGLE_COLOR, 6, UINT32_MAX);
    shape_memory(s, &c, NULL);
    qtest_writel(s, DE + 0xd0, TRIANGLE_COLOR);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0xdeadbeef), ==, MEMTX_OK);
    triangle_pixels(s, &c, triangle_ten, TRIANGLE_COLOR, 6, UINT32_MAX);
    triangle_irq_levels(s, 1);
    qtest_quit(s);
}

static void test_triangle_coverage(void)
{
    static const unsigned orders[][3] = {
        { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 },
        { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 },
    };
    static const struct {
        int vertex[3][2];
        uint8_t rows[8];
    } cases[] = {
        { { {64, 64}, {320, 64}, {64, 320} },
          { 0, 0x1e, 0x0e, 6, 2 } },
        { { {16, 16}, {208, 16}, {16, 208} }, { 0, 6, 2 } },
        { { {-64, -64}, {256, -64}, {-64, 256} }, { 7, 3, 1 } },
        { { {64, 64}, {320, 64}, {64, 65} }, { 0, 0x1e } },
        { { {64, 64}, {320, 320}, {192, 192} }, { 0 } },
        { { {64, 64}, {64, 64}, {64, 64} }, { 0 } },
    };
    QTestState *s = triangle_start("");

    for (unsigned n = 0; n < ARRAY_SIZE(cases); n++) {
        for (unsigned order = 0; order < ARRAY_SIZE(orders); order++) {
            for (unsigned direction = 0; direction < 4; direction++) {
                Shape c = triangle_prepare(s, direction % 3, 0, 0);

                qtest_writel(s, DE + 0x60, 0x02000000 |
                            direction << 16 | 0x4ffff);
                triangle_vertices(s, cases[n].vertex, orders[order],
                                    !!(order & 1));
                g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                                ==, MEMTX_OK);
                triangle_pixels(s, &c, cases[n].rows, TRIANGLE_COLOR,
                                  3, UINT32_MAX);
            }
        }
    }

    for (unsigned delta = 0; delta <= 4; delta += delta ? 3 : 1) {
        int vertex[3][2] = {
            {16 * 64 + delta, 16 * 64},
            {20 * 64 + delta, 16 * 64},
            {16 * 64 + delta, 20 * 64},
        };
        const uint8_t integer_rows[8] = { 15, 7, 3, 1 };
        const uint8_t fractional_rows[8] = { 0x1e, 0x0e, 6, 2 };
        Shape c = triangle_prepare(s, 0, 16, 16);

        g_assert_cmphex(triangle_raw(vertex[0][0]), ==, 0x48040400 + delta);
        triangle_vertices(s, vertex, NULL, true);
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        triangle_pixels(s, &c, delta ? fractional_rows : integer_rows,
                          TRIANGLE_COLOR, 3, UINT32_MAX);
    }
    {
        const int vertex[3][2] = {
            {2046 * 64, 2046 * 64}, {2050 * 64, 2046 * 64},
            {2046 * 64, 2050 * 64},
        };
        const uint8_t rows[8] = { 0, 0, 0, 0, 0, 0, 0xc0, 0xc0 };
        Shape c = triangle_prepare(s, 2, 2040, 2040);

        triangle_vertices(s, vertex, NULL, true);
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        triangle_pixels(s, &c, rows, TRIANGLE_COLOR, 3, UINT32_MAX);
    }
    /* Both full prefix endpoints participate in products wider than 32 bits. */
    {
        const int vertex[3][2] = {
            { -262144, -262144 }, { 262143, -262144 }, { 0, 262143 },
        };
        const uint8_t rows[8] = {
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        };
        Shape c = triangle_prepare(s, 0, 0, 0);

        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | (1U << 20));
        qtest_writeq(s, DE + 0x48, bounds(4096, 4096, 4104, 4104));
        triangle_vertices(s, vertex, NULL, true);
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        triangle_pixels(s, &c, rows, TRIANGLE_COLOR, 3, UINT32_MAX);
    }
    triangle_irq_levels(s, 1);
    qtest_quit(s);
}

static void test_triangle_neighbors(void)
{
    static const int corners[][2] = {
        {64, 64}, {320, 64}, {320, 320}, {64, 320},
        {192, 64}, {192, 320}, {64, 192}, {320, 192},
    };
    static const unsigned meshes[][4][3] = {
        { {0, 1, 3}, {1, 2, 3} },
        { {0, 1, 2}, {0, 2, 3} },
        { {0, 4, 3}, {4, 5, 3}, {4, 1, 5}, {1, 2, 5} },
        { {0, 1, 6}, {1, 7, 6}, {6, 7, 3}, {7, 2, 3} },
    };
    const uint8_t square[8] = { 0, 0x1e, 0x1e, 0x1e, 0x1e };
    QTestState *s = triangle_start("");

    for (unsigned mesh = 0; mesh < ARRAY_SIZE(meshes); mesh++) {
        for (unsigned direction = 0; direction < 4; direction++) {
            Shape c = triangle_prepare(s, 0, 0, 0);

            qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x200);
            qtest_writel(s, DE + 0x1b0, 6);
            qtest_writel(s, DE + 0x60, 0x02000000 | direction << 16);
            for (unsigned n = 0; n < (mesh < 2 ? 2 : 4); n++) {
                int vertex[3][2];

                for (unsigned i = 0; i < 3; i++) {
                    unsigned corner = meshes[mesh][n][i];

                    vertex[i][0] = corners[corner][0];
                    vertex[i][1] = corners[corner][1];
                }
                triangle_vertices(s, vertex, NULL, true);
                g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                                ==, MEMTX_OK);
            }
            triangle_pixels(s, &c, square, TRIANGLE_COLOR, 6, UINT32_MAX);
        }
    }
    triangle_irq_levels(s, 1);
    qtest_quit(s);
}

static void test_triangle_pipeline(void)
{
    const uint32_t planes[] = { 0, 0x00ff3c5a, UINT32_MAX };
    const unsigned lanes[] = { 0, 1, 2, 4, 8, 15 };
    QTestState *s = triangle_start("");

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned op = 0; op < 16; op++) {
            for (unsigned n = 0; n < ARRAY_SIZE(lanes); n++) {
                Shape c = triangle_prepare(s, bank, 0, 0);
                uint32_t mask = 0;

                for (unsigned b = 0; b < 4; b++) {
                    if (lanes[n] & (8 >> b)) {
                        mask |= 0xff000000U >> (8 * b);
                    }
                }
                mask &= planes[n % 3];
                qtest_writel(s, DE + 0x18, 0x400280 | lanes[n] << 3);
                qtest_writel(s, DE + 0x1b0, op);
                qtest_writel(s, DE + 0x1b8, planes[n % 3]);
                triangle_vertices(s, triangle_integer, NULL, true);
                g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                                ==, MEMTX_OK);
                triangle_pixels(s, &c, triangle_ten, TRIANGLE_COLOR, op, mask);
            }
        }
    }
    for (unsigned j = 0; j < 5; j++) {
        for (unsigned inside = 0; inside < 2; inside++) {
            Shape c = triangle_prepare(s, 0, 0, 0);
            uint8_t rows[8];

            memcpy(rows, triangle_ten, sizeof(rows));
            for (unsigned y = 0; y < 8; y++) {
                for (unsigned x = 0; x < 8; x++) {
                    if ((x >= 2 && x < 4 && y >= 2 && y < 4) != inside) {
                        rows[y] &= ~(1U << x);
                    }
                }
            }
            qtest_writel(s, DE + 0x10, (1U << (5 + j)) | inside << j);
            qtest_writeq(s, DE + 0x20 + 8 * j, bounds(2, 2, 4, 4));
            triangle_vertices(s, triangle_integer, NULL, true);
            g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                            ==, MEMTX_OK);
            triangle_pixels(s, &c, rows, TRIANGLE_COLOR, 3, UINT32_MAX);
        }
    }
    for (unsigned empty = 0; empty < 2; empty++) {
        const uint8_t rows[8] = { 0, 0, 0x0c, 4 };
        Shape c = triangle_prepare(s, 0, 0, 0);

        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x100000);
        qtest_writeq(s, DE + 0x48,
                     empty ? bounds(0, 0, 0, 0) :
                             bounds(4098, 4098, 4100, 4100));
        triangle_vertices(s, triangle_integer, NULL, true);
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        triangle_pixels(s, &c, empty ? triangle_empty : rows,
                          TRIANGLE_COLOR, 3, UINT32_MAX);
    }
    triangle_irq_levels(s, 1);
    qtest_quit(s);
}

static void triangle_failure(QTestState *s, const Shape *c, const char *path,
                              uint64_t addr, uint64_t value, unsigned width)
{
    static const unsigned status[] = { 0x10, 0x40, 0x48, 0x250, 0x258 };
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS], old[5];
    unsigned irq = 0;

    for (unsigned i = 0; i < 5; i++) {
        old[i] = qtest_readq(s, CRIME + status[i]);
        irq |= qtest_get_irq(s, i) << i;
    }
    save_paired_renderer(s, path, before);
    assert_failure(s, addr, value, width);
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmphex(qtest_readq(s, CRIME + status[i]), ==, old[i]);
        g_assert_cmpint(qtest_get_irq(s, i), ==, !!(irq & (1U << i)));
    }
    triangle_pixels(s, c, triangle_empty, 0, 3, UINT32_MAX);
    save_paired_renderer(s, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
}

static void test_triangle_admission(void)
{
    const uint32_t invalid[] = {
        0x00080000, 0x47ffffff, 0x48080000, 0x480c0400, 0x7f800000, 0x7fc00000,
    };
    g_autofree char *dir = g_dir_make_tmp("crime-triangle-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = triangle_start("");

    for (unsigned word = 0; word < 6; word++) {
        for (unsigned n = 0; n < ARRAY_SIZE(invalid); n++) {
            Shape c = triangle_prepare(s, 0, 0, 0);

            triangle_vertices(s, triangle_integer, NULL, true);
            qtest_writel(s, DE + 0x80 + 4 * word, invalid[n]);
            qtest_writel(s, DE + 0x18, TRIANGLE_DRAW & ~0x78U);
            triangle_failure(s, &c, path, DE + 0x8d0, TRIANGLE_COLOR, 4);
        }
    }
    for (unsigned bit = 0; bit < 32; bit++) {
        Shape c = triangle_prepare(s, 0, 0, 0);

        if ((1U << bit) & 0x5b17ffU) {
            continue;
        }
        triangle_vertices(s, triangle_integer, NULL, true);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 1U << bit);
        triangle_failure(s, &c, path, DE + 0x8d0, TRIANGLE_COLOR, 4);
    }
    for (unsigned n = 0; n < 7; n++) {
        Shape c = triangle_prepare(s, 0, 0, 0);

        triangle_vertices(s, triangle_integer, NULL, true);
        switch (n) {
        case 0:
            qtest_writel(s, DE + 0x18, 0x78);
            break;
        case 1:
            qtest_writel(s, DE + 0x10, 0x800);
            break;
        case 2:
            qtest_writel(s, DE + 8, 0x114);
            break;
        case 3:
            qtest_writel(s, DE + 8, 0xe28);
            break;
        case 4:
            qtest_writel(s, DE + 0x10, 0x61);
            break;
        case 5:
            qtest_writel(s, DE + 0x10, 1);
            break;
        case 6:
            qtest_writel(s, DE + 0x60, 0x02080000);
            break;
        }
        triangle_failure(s, &c, path, DE + 0x8d0, TRIANGLE_COLOR, 4);
    }
    triangle_irq_levels(s, 1);
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_triangle_transactions(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-triangle-state-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = triangle_start("");
    uint64_t words[RENDER_WORDS], value;

    for (unsigned n = 0; n < 3; n++) {
        Shape c = triangle_prepare(s, 0, 0, 0);
        uint64_t raw = (uint64_t)(0xdeadbeefU + n) << 32 | (0x7fc00000U + n);

        g_assert_cmpint(memtx_writeq(s, DE + 0x80 + 8 * n, raw),
                        ==, MEMTX_OK);
        save_paired_renderer(s, path, words);
        g_assert_cmphex(words[192 + (0x80 + 8 * n) / 4], ==, raw >> 32);
        g_assert_cmphex(words[192 + (0x84 + 8 * n) / 4], ==, (uint32_t)raw);
        triangle_pixels(s, &c, triangle_empty, 0, 3, UINT32_MAX);
        g_assert_cmpint(memtx_readq(s, DE + 0x80 + 8 * n, &value),
                        !=, MEMTX_OK);
        triangle_failure(s, &c, path, DE + 0x84 + 8 * n, 0, 8);
        for (unsigned member = 0; member < 2; member++) {
            for (unsigned width = 4; width <= 8; width *= 2) {
                triangle_failure(s, &c, path, DE + 0x880 + 8 * n + 4 * member,
                                  0, width);
            }
        }
    }
    for (unsigned n = 0; n < 5; n++) {
        Shape c = triangle_prepare(s, 0, 0, 0);

        triangle_vertices(s, triangle_integer, NULL, true);
        qtest_writeq(s, DE + 0x70, 0x0011223300445566ULL);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x200);
        qtest_writel(s, DE + 0x1b0, 6);
        qtest_writel(s, DE + 0xd0, TRIANGLE_COLOR);
        switch (n) {
        case 0:
            g_assert_cmpint(memtx_writel(s, DE + 0x860, 0x02030000),
                               ==, MEMTX_OK);
            break;
        case 1:
            g_assert_cmpint(memtx_writeq(s, DE + 0x870, UINT64_MAX),
                               ==, MEMTX_OK);
            break;
        case 2:
            g_assert_cmpint(memtx_writeq(s, DE + 0x820, bounds(0, 0, 8, 8)),
                               ==, MEMTX_OK);
            break;
        case 3:
            g_assert_cmpint(memtx_writeq(s, DE + 0x848, bounds(0, 0, 8, 8)),
                               ==, MEMTX_OK);
            break;
        case 4:
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0),
                               ==, MEMTX_OK);
            break;
        }
        triangle_pixels(s, &c, triangle_ten, TRIANGLE_COLOR, 6, UINT32_MAX);
        save_paired_renderer(s, path, words);
        if (n == 0) {
            g_assert_cmphex(words[192 + 0x60 / 4], ==, 0x02030000);
        } else if (n == 1) {
            g_assert_cmphex(words[192 + 0x70 / 4], ==, UINT32_MAX);
            g_assert_cmphex(words[192 + 0x74 / 4], ==, UINT32_MAX);
        } else if (n == 2 || n == 3) {
            g_assert_cmphex(words[192 + (n == 2 ? 0x20 : 0x48) / 4],
                            ==, bounds(0, 0, 8, 8));
        }

        shape_memory(s, &c, NULL);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x80);
        triangle_failure(s, &c, path, DE + 0x9b8, 0, 8);
    }
    {
        Shape c = triangle_prepare(s, 0, 0, 0);

        triangle_vertices(s, triangle_integer, NULL, true);
        g_assert_cmpint(memtx_writel(s, DE + 0x858, 1), ==, MEMTX_OK);
        triangle_pixels(s, &c, triangle_empty, 0, 3, UINT32_MAX);
        g_assert_cmphex(qtest_readl(s, DE + 0x58), ==, 1);
        qtest_writel(s, DE + 0x58, 0xf000f000U);
        triangle_failure(s, &c, path, DE + 0x8d0,
                          0x123456789abcdef0ULL, 8);
        triangle_failure(s, &c, path, DE + 0x9f0, 0, 8);

    }
    triangle_irq_levels(s, 1);
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_triangle_tiles(void)
{
    ValidityLog v = validity_start(false, "");
    g_autofree char *path = g_build_filename(v.directory, "state", NULL);

    qtest_irq_intercept_out_named(v.q, "/machine/peripheral/crime", "cpu-ip");
    triangle_irq_baseline(v.q);

    for (unsigned bank = 0; bank < 3; bank++) {
        Shape c = triangle_prepare(v.q, bank, 125, 125);
        const int vertex[3][2] = {
            {126 * 64, 126 * 64}, {130 * 64, 126 * 64},
            {126 * 64, 130 * 64},
        };
        uint8_t rows[8];

        memcpy(rows, triangle_ten, sizeof(rows));
        triangle_vertices(v.q, vertex, NULL, true);
        g_assert_cmpint(memtx_writel(v.q, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        triangle_pixels(v.q, &c, rows, TRIANGLE_COLOR, 3, UINT32_MAX);
        shape_memory(v.q, &c, NULL);
        tile(v.q, bank, 1, 0x7fff);
        /* Clear validity while retaining the deliberately hostile payload. */
        {
            uint64_t a = RE + 0x1000 + bank * 512;
            uint64_t w = qtest_readq(v.q, a);

            qtest_writeq(v.q, a, w & ~(0x8000ULL << 32));
        }
        g_assert_cmpint(memtx_writel(v.q, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        rows[1] &= 7;
        rows[2] &= 7;
        triangle_pixels(v.q, &c, rows, TRIANGLE_COLOR, 3, UINT32_MAX);
        g_ptr_array_add(v.records, g_strdup(
            "sgi-crime: triangle suppressed 3 invalid destination fragments"));
        shape_memory(v.q, &c, NULL);
        {
            uint64_t a = RE + 0x1000 + bank * 512;
            uint64_t w = qtest_readq(v.q, a);

            w &= ~(0xffffULL << 32);
            w |= (uint64_t)page_for(bank, 0) << 32;
            qtest_writeq(v.q, a, w);
        }
        g_assert_cmpint(memtx_writel(v.q, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        triangle_pixels(v.q, &c, rows, TRIANGLE_COLOR, 3, UINT32_MAX);
        g_ptr_array_add(v.records, g_strdup(
            "sgi-crime: triangle suppressed 3 invalid destination fragments"));
        shape_memory(v.q, &c, NULL);
        tile(v.q, bank, 16, 0x7fff);
        triangle_failure(v.q, &c, path, DE + 0x8d0, 0xfeedbeef, 4);
        triangle_failure(v.q, &c, path, DE + 0x870,
                          0x123456789abcdef0ULL, 8);
        /* Zero coverage must not consult even a valid bad mapping. */
        qtest_writel(v.q, DE + 0x18, TRIANGLE_DRAW & ~0x78U);
        g_assert_cmpint(memtx_writel(v.q, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        triangle_pixels(v.q, &c, triangle_empty, 0, 3, UINT32_MAX);
    }
    for (unsigned bank = 0; bank < 3; bank++) {
        Shape c = triangle_prepare(v.q, bank, 0, 0);
        const int vertex[3][2] = { {0, 0}, {130 * 64, 0}, {0, 2 * 64} };

        tile(v.q, bank, 1, page_for(bank, 0));
        triangle_vertices(v.q, vertex, NULL, true);
        triangle_failure(v.q, &c, path, DE + 0x8d0, TRIANGLE_COLOR, 4);
        triangle_vertices(v.q, triangle_integer, NULL, true);
        qtest_writeq(v.q, RE + 0x1000 + bank * 512, 0x7fff000000000000ULL);
        g_assert_cmpint(memtx_writel(v.q, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        triangle_pixels(v.q, &c, triangle_empty, 0, 3, UINT32_MAX);
        g_ptr_array_add(v.records, g_strdup(
            "sgi-crime: triangle suppressed 10 invalid destination fragments"));
        tile(v.q, bank, 0, 0x7fff);
        for (unsigned i = 0; i < 3; i++) {
            qtest_writeq(v.q, DE + 0x80 + 8 * i,
                         (uint64_t)triangle_raw(64) << 32 | triangle_raw(64));
        }
        g_assert_cmpint(memtx_writel(v.q, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        triangle_pixels(v.q, &c, triangle_empty, 0, 3, UINT32_MAX);
    }
    triangle_irq_levels(v.q, 1);
    validity_finish(&v);
}

static void test_triangle_migration(void)
{
    g_autofree char *dir =
        g_dir_make_tmp("crime-triangle-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *from = triangle_start("");
    QTestState *to = triangle_start("-incoming defer");
    Shape c = triangle_prepare(from, 0, 0, 0);
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];

    triangle_vertices(from, triangle_integer, NULL, true);
    qtest_writel(from, DE + 0xd0, TRIANGLE_COLOR);
    qtest_writel(from, DE + 0x94, 0x480c0400);
    qtest_writel(from, DE + 0x134, 0xdeadbeef);
    save_paired_renderer(from, path, before);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    save_paired_renderer(to, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    triangle_pixels(to, &c, triangle_empty, 0, 3, UINT32_MAX);
    triangle_failure(to, &c, path, DE + 0x8d0, 0xdeadbeef, 4);
    qtest_writel(to, DE + 0x94, triangle_raw(320));
    g_assert_cmpint(memtx_writel(to, DE + 0x9f0, 0), ==, MEMTX_OK);
    triangle_pixels(to, &c, triangle_ten, TRIANGLE_COLOR, 3, UINT32_MAX);
    qtest_system_reset(to);
    shape_memory(to, &c, NULL);
    qtest_writel(to, DE + 8, 0x228);
    qtest_writel(to, DE + 0x18, TRIANGLE_DRAW);
    qtest_writel(to, DE + 0x60, 0x02000000);
    g_assert_cmpint(memtx_writel(to, DE + 0x8d0, 0xdeadbeef), ==, MEMTX_OK);
    triangle_pixels(to, &c, triangle_empty, 0, 3, UINT32_MAX);
    save_paired_renderer(to, path, after);
    g_assert_cmphex(after[192 + 0x134 / 4], ==, 0);
    for (unsigned i = 0; i < 6; i++) {
        g_assert_cmphex(after[192 + (0x80 + i * 4) / 4], ==, 0);
    }
    triangle_irq_levels(from, 1);
    triangle_irq_levels(to, 0);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_unlink(path);
    g_rmdir(dir);
}


static void gl_field_vertices(QTestState *s, uint32_t prefix,
                               const unsigned vertex[3][2])
{
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeq(s, DE + 0x80 + 8 * i,
                     (uint64_t)(prefix + vertex[i][0]) << 32 |
                     (prefix + vertex[i][1]));
    }
}

static uint32_t gl_window_offset(int x, int y)
{
    return (uint32_t)(uint16_t)x << 16 | (uint16_t)y;
}

static void gl_window_case(QTestState *s, uint32_t prefix,
                            const unsigned vertex[3][2], int ox, int oy,
                            unsigned x, unsigned y, const uint8_t rows[8])
{
    Shape c = triangle_prepare(s, 0, x, y);

    qtest_writel(s, DE + 0x58, gl_window_offset(ox, oy));
    gl_field_vertices(s, prefix, vertex);
    g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==, MEMTX_OK);
    triangle_pixels(s, &c, rows, TRIANGLE_COLOR, 3, UINT32_MAX);
}

static void test_gl_window_coordinates(void)
{
    const uint32_t prefixes[] = { 0, 0x00800000, 0x48000000 };
    const unsigned at16[3][2] = {
        { 0x40400, 0x40400 }, { 0x40500, 0x40400 }, { 0x40400, 0x40500 },
    };
    const unsigned zero[3][2] = { {0, 0}, {256, 0}, {0, 256} };
    const unsigned local[3][2] = { {128, 128}, {384, 128}, {128, 384} };
    const unsigned upper[3][2] = {
        {0x7feff, 0x7feff}, {0x7ffff, 0x7feff}, {0x7feff, 0x7ffff},
    };
    const uint8_t four_rows[8] = { 15, 7, 3, 1 };
    const uint8_t upper_rows[8] = { 3, 1 };
    const int invisible[] = { -32768, -1, 0, 1, 32767 };
    QTestState *s = triangle_start("");

    /* The superseded implicit -4096 must not survive a zero origin. */
    gl_window_case(s, prefixes[2], at16, 0, 0, 16, 16, triangle_empty);
    for (unsigned p = 0; p < ARRAY_SIZE(prefixes); p++) {
        gl_window_case(s, prefixes[p], at16, -4096, -4096,
                       16, 16, four_rows);
        gl_window_case(s, prefixes[p], at16, -3840, -3840,
                       272, 272, four_rows);
        gl_window_case(s, prefixes[p], zero, 0, 0, 0, 0, four_rows);
        gl_window_case(s, prefixes[p], zero, 1, 1, 0, 0, triangle_ten);
        gl_window_case(s, prefixes[p], local, -1, -1, 0, 0, triangle_ten);
        gl_window_case(s, prefixes[p], upper, -8189, -8189,
                       0, 0, upper_rows);
        for (unsigned n = 0; n < ARRAY_SIZE(invisible); n++) {
            gl_window_case(s, prefixes[p], at16,
                           invisible[n], invisible[n], 0, 0, triangle_empty);
        }
        for (int delta = -1; delta <= 1; delta++) {
            unsigned base = 0x40000 + delta;
            const unsigned vertex[3][2] = {
                {base, base}, {base + 256, base}, {base, base + 256},
            };
            const uint8_t positive[8] = { 0, 14, 6, 2 };

            gl_window_case(s, prefixes[p], vertex, -4096, -4096, 0, 0,
                           delta > 0 ? positive : four_rows);
        }
    }
    {
        Shape c = triangle_prepare(s, 0, 0, 0);

        for (unsigned v = 0; v < 3; v++) {
            for (unsigned axis = 0; axis < 2; axis++) {
                qtest_writel(s, DE + 0x80 + v * 8 + axis * 4,
                    prefixes[(v * 2 + axis) % 3] + 0x40000 +
                    triangle_integer[v][axis]);
            }
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        triangle_pixels(s, &c, triangle_ten, TRIANGLE_COLOR, 3, UINT32_MAX);
    }
    /* A 128-pixel window, with a literal right-triangle span fixture. */
    for (unsigned bank = 0; bank < 3; bank++) {
        Shape c = triangle_prepare(s, bank, 256, 256);
        const unsigned vertex[3][2] = {
            {0x40000, 0x40000}, {0x42000, 0x40000}, {0x40000, 0x42000},
        };
        g_autofree uint32_t *expected = g_new(uint32_t, 128 * 128);

        c.width = c.height = 128;
        map_region(s, bank, 4, 256, 256, 383, 383);
        shape_memory(s, &c, NULL);
        qtest_writel(s, DE + 0x58, gl_window_offset(-3840, -3840));
        qtest_writel(s, DE + 0x18, 0x00400070);
        gl_field_vertices(s, prefixes[bank], vertex);
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        for (unsigned y = 0; y < 128; y++) {
            for (unsigned x = 0; x < 128; x++) {
                expected[y * 128 + x] = x + y < 128 ?
                    (TRIANGLE_COLOR & 0xffffff00) | (c.old & 255) : c.old;
            }
        }
        shape_memory(s, &c, expected);
    }
    triangle_irq_levels(s, 1);
    qtest_quit(s);
}

static void test_gl_window_clipping(void)
{
    const unsigned vertex[3][2] = {
        {0x40040, 0x40040}, {0x40140, 0x40040}, {0x40040, 0x40140},
    };
    QTestState *s = triangle_start("");

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned masks = 0; masks <= 5; masks++) {
            Shape c = triangle_prepare(s, bank, 256, 256);
            uint8_t rows[8];
            uint32_t clip = 0x400; /* An inactive CID selector is not enable. */

            memcpy(rows, triangle_ten, sizeof(rows));
            qtest_writel(s, DE + 0x58, gl_window_offset(-3840, -3840));
            qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x100200);
            qtest_writel(s, DE + 0x1b0, 6);
            qtest_writeq(s, DE + 0x48, bounds(4098, 4097, 4101, 4100));
            for (unsigned j = 0; j < masks; j++) {
                clip |= (1U << (5 + j)) | (1U << j);
                qtest_writeq(s, DE + 0x20 + 8 * j,
                             bounds(257 + j % 3, 257, 259 + j % 3, 260));
            }
            qtest_writel(s, DE + 0x10, clip);
            for (unsigned y = 0; y < 8; y++) {
                for (unsigned x = 0; x < 8; x++) {
                    bool covered = !masks;

                    for (unsigned j = 0; j < masks; j++) {
                        covered |= x >= 1 + j % 3 && x < 3 + j % 3 &&
                                   y >= 1 && y < 4;
                    }
                    if (!covered || x < 2 || x >= 5 || y < 1 || y >= 4) {
                        rows[y] &= ~(1U << x);
                    }
                }
            }
            gl_field_vertices(s, 0x48000000, vertex);
            g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                            ==, MEMTX_OK);
            triangle_pixels(s, &c, rows, TRIANGLE_COLOR, 6, UINT32_MAX);
        }
    }
    {
        g_autofree char *dir = g_dir_make_tmp("crime-gl-window-XXXXXX", NULL);
        g_autofree char *path = g_build_filename(dir, "state", NULL);
        const unsigned bad_clip[] = { 0x800, 0xc00, 0x1000, 0x61 };

        for (unsigned i = 0; i < ARRAY_SIZE(bad_clip); i++) {
            Shape c = triangle_prepare(s, 0, 0, 0);

            gl_field_vertices(s, 0x48000000, vertex);
            qtest_writel(s, DE + 0x58, 0);
            qtest_writel(s, DE + 0x18, TRIANGLE_DRAW & ~0x78U);
            qtest_writel(s, DE + 0x10, bad_clip[i]);
            triangle_failure(s, &c, path, DE + 0x8d0, TRIANGLE_COLOR, 4);
            triangle_failure(s, &c, path, DE + 0x858,
                             gl_window_offset(-3840, -3840), 4);
        }
        g_unlink(path);
        g_rmdir(dir);
    }
    triangle_irq_levels(s, 1);
    qtest_quit(s);
}

static void test_gl_restore_word(void)
{
    const uint32_t payloads[] = { 0, 0x11223344, 0xdeadbeef, UINT32_MAX };
    g_autofree char *dir = g_dir_make_tmp("crime-gl-restore-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = triangle_start("");
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS], value;

    for (unsigned i = 0; i < ARRAY_SIZE(payloads); i++) {
        Shape c = triangle_prepare(s, 0, 0, 0);

        triangle_vertices(s, triangle_integer, NULL, true);
        g_assert_cmpint(memtx_writel(s, DE + 0x134, payloads[i]),
                        ==, MEMTX_OK);
        triangle_pixels(s, &c, triangle_empty, 0, 3, UINT32_MAX);
        save_paired_renderer(s, path, before);
        g_assert_cmphex(before[192 + 0x134 / 4], ==, payloads[i]);
        g_assert_cmpint(memtx_read(s, DE + 0x134, &value, 4), !=, MEMTX_OK);
        for (unsigned width = 1; width <= 8; width *= 2) {
            if (width != 4) {
                triangle_failure(s, &c, path, DE + 0x134, 0, width);
            }
        }
        triangle_failure(s, &c, path, DE + 0x130, UINT64_MAX, 8);
        triangle_failure(s, &c, path, DE + 0x138 - 1, UINT32_MAX, 4);
        triangle_failure(s, &c, path, DE + 0x934, UINT32_MAX, 4);
        save_paired_renderer(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        triangle_pixels(s, &c, triangle_ten, TRIANGLE_COLOR, 3, UINT32_MAX);
        shape_memory(s, &c, NULL);
        qtest_writel(s, MTE, 0);
        qtest_writel(s, MTE + 8, UINT32_MAX);
        qtest_writel(s, MTE + 0x18, 0x11223344);
        qtest_writel(s, MTE + 0x30, xy(0, 0));
        qtest_writel(s, MTE + 0x48, 1);
        qtest_writel(s, MTE + 0x838, xy(3, 0));
        {
            const uint8_t first_pixel[8] = { 1 };

            triangle_pixels(s, &c, first_pixel, 0x11223344, 3, UINT32_MAX);
        }
        save_paired_renderer(s, path, after);
        g_assert_cmphex(after[192 + 0x134 / 4], ==, payloads[i]);
    }
    triangle_irq_levels(s, 1);
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

typedef struct IfbSnapshot {
    uint8_t ram[1024];
    uint32_t status;
} IfbSnapshot;

static void ifb_snapshot(QTestState *s, IfbSnapshot *state)
{
    qtest_memread(s, RE, state->ram, sizeof(state->ram));
    state->status = qtest_readl(s, STATUS);
}

static void ifb_head(QTestState *s, unsigned head)
{
    uint32_t status = qtest_readl(s, STATUS);

    g_assert_cmphex(status & ~0x3ffffU, ==, 0x1e000000);
    g_assert_cmphex(status & 63, ==, head);
    g_assert_cmphex((status >> 6) & 63, ==, head);
    g_assert_cmphex((status >> 12) & 63, ==, head);
}

static unsigned ifb_next(QTestState *s)
{
    unsigned head = qtest_readl(s, STATUS) & 63;

    ifb_head(s, head);
    return head;
}

static void ifb_entry(QTestState *s, unsigned index,
                      uint64_t address, uint64_t data)
{
    g_assert_cmphex(qtest_readq(s, RE + 8 * index), ==, data);
    g_assert_cmphex(qtest_readq(s, RE + 0x200 + 8 * index), ==, address);
}

static void test_ifb_ram(void)
{
    QTestState *s = triangle_start("");

    ifb_head(s, 0);
    for (unsigned bank = 0; bank < 2; bank++) {
        for (unsigned index = 0; index < 64; index++) {
            uint64_t address = RE + bank * 512 + index * 8;
            uint64_t pattern = 0x123456789abcdef0ULL ^
                               ((uint64_t)(bank * 64 + index) << 40);
            uint32_t high = 0xa13579bdU ^ (bank * 64 + index);
            uint32_t low = 0x6e2c4a80U ^ (bank * 64 + index);
            uint32_t actual;

            g_assert_cmpint(memtx_writeq(s, address, pattern), ==, MEMTX_OK);
            g_assert_cmphex(qtest_readq(s, address), ==, pattern);
            g_assert_cmpint(memtx_readl(s, address, &actual), ==, MEMTX_OK);
            g_assert_cmphex(actual, ==, pattern >> 32);
            g_assert_cmpint(memtx_readl(s, address + 4, &actual), ==, MEMTX_OK);
            g_assert_cmphex(actual, ==, (uint32_t)pattern);
            g_assert_cmpint(memtx_writel(s, address, high), ==, MEMTX_OK);
            g_assert_cmphex(qtest_readq(s, address), ==,
                            (uint64_t)high << 32 | (uint32_t)pattern);
            g_assert_cmpint(memtx_writel(s, address + 4, low), ==, MEMTX_OK);
            g_assert_cmphex(qtest_readq(s, address), ==,
                            (uint64_t)high << 32 | low);
            ifb_head(s, 0);
        }
    }
    for (unsigned index = 0; index < 128; index++) {
        uint64_t expected = (uint64_t)(0xa13579bdU ^ index) << 32 |
                            (0x6e2c4a80U ^ index);

        g_assert_cmphex(qtest_readq(s, RE + index * 8), ==, expected);
    }
    triangle_irq_levels(s, 1);
    qtest_quit(s);
}

static void test_ifb_transactions(void)
{
    /* Literal headers exercise independent known bus/address classes. */
    static const struct {
        unsigned offset, size;
        uint64_t value, header, data;
    } writes[] = {
        { 0x1000, 8, 0x1122334455667788ULL, 0x1900,
          0x1122334455667788ULL },
        { 0x2084, 4, 0x89abcdef, 0x0a10, 0x89abcdef },
        { 0x2008, 8, 0x0000022811223344ULL, 0x1a01,
          0x0000022811223344ULL },
        { 0x2070, 8, 0x0102030405060708ULL, 0x1a0e,
          0x0102030405060708ULL },
        { 0x2020, 8, 0x8899aabbccddeeffULL, 0x1a04,
          0x8899aabbccddeeffULL },
        { 0x2134, 4, 0x13579bdf, 0x0a26, 0x13579bdf },
        { 0x3008, 4, 0xdeadbeef, 0x1301, 0xdeadbeef00000000ULL },
        { 0x3048, 4, 1, 0x1309, 0x0000000100000000ULL },
        { 0x21f0, 4, 0x76543210, 0x123e, 0x7654321000000000ULL },
        { 0x21f8, 4, 0, 0x123f, 0 },
        { 0x3070, 4, 0x10203040, 0x130e, 0x1020304000000000ULL },
        { 0x3078, 4, 0, 0x130f, 0 },
    };
    QTestState *s = triangle_start("");
    IfbSnapshot before, after;

    for (unsigned i = 0; i < ARRAY_SIZE(writes); i++) {
        qtest_writeq(s, RE + i * 8, 0xf00dface87654321ULL);
        qtest_writeq(s, RE + 0x200 + i * 8, UINT64_MAX);
        ifb_head(s, i);
        g_assert_cmpint(memtx_write(s, RE + writes[i].offset,
                                    writes[i].value, writes[i].size),
                        ==, MEMTX_OK);
        ifb_entry(s, i, writes[i].header << 32, writes[i].data);
        ifb_head(s, i + 1);
    }
    tile(s, 0, 0, 3);
    qtest_writel(s, MTE, 0);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x18, 0x11223344);
    qtest_writel(s, MTE + 0x48, 1);
    qtest_writel(s, MTE + 0x30, xy(0, 0));
    {
        unsigned head = ifb_next(s);

        g_assert_cmpint(memtx_writel(s, MTE + 0x838, xy(3, 0)),
                        ==, MEMTX_OK);
        ifb_entry(s, head, 0x330700000000ULL, 0x0003000000000000ULL);
        ifb_head(s, (head + 1) % 64);
        g_assert_cmphex(qtest_readl(s, 0x30000), ==, 0x11223344);
        qtest_writel(s, MTE + 8, 0);
        head = ifb_next(s);
        g_assert_cmpint(memtx_writel(s, MTE + 0x810, 0x89abcdef),
                        ==, MEMTX_OK);
        ifb_entry(s, head, 0x330200000000ULL, 0x89abcdef00000000ULL);
        ifb_head(s, (head + 1) % 64);
        g_assert_cmphex(qtest_readl(s, 0x30000), ==, 0x11223344);
    }
    tile(s, 0, 0, 0x7fff);
    ifb_snapshot(s, &before);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, xy(3, 0)),
                    ==, MEMTX_ERROR);
    ifb_snapshot(s, &after);
    g_assert_cmpmem(&before, sizeof(before), &after, sizeof(after));
    g_assert_cmphex(qtest_readl(s, 0x30000), ==, 0x11223344);
    {
        unsigned head = ifb_next(s);

        for (unsigned n = 0; n < 2; n++) {
            qtest_writel(s, DE + 0xd0, 0x01020304);
            ifb_entry(s, (head + n) % 64, 0x121a00000000ULL,
                       0x0102030400000000ULL);
            ifb_head(s, (head + n + 1) % 64);
        }
    }
    ifb_snapshot(s, &before);
    g_assert_cmphex(qtest_readl(s, DE + 8), ==, 0x228);
    g_assert_cmpint(memtx_writel(s, RE + 0x400, UINT32_MAX), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, RE + 0x400), ==, 0);
    g_assert_cmpint(memtx_writel(s, STATUS + 8, 0x12345678), ==, MEMTX_OK);
    qtest_writeq(s, CRIME + 0x18, 1U << 4);
    g_assert_cmpint(memtx_writel(s, RE + 0x1000, 1), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writel(s, DE + 0x20, 1), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writel(s, DE + 0x1f8, 1), !=, MEMTX_OK);
    /* The admitted zero write at +0x408 records no entry either. */
    g_assert_cmpint(memtx_writel(s, RE + 0x408, 0), ==, MEMTX_OK);
    ifb_snapshot(s, &after);
    g_assert_cmpmem(&before, sizeof(before), &after, sizeof(after));
    triangle_irq_levels(s, 1);
    qtest_quit(s);
}

static void ifb_varied_write(QTestState *s, unsigned n)
{
    switch (n % 4) {
    case 0:
        qtest_writel(s, DE + 0xd0, 0x54000000U + n);
        break;
    case 1:
        qtest_writel(s, DE + 0x84, 0x65000000U + n);
        break;
    case 2:
        qtest_writeq(s, RE + 0x1008, 0x1122334455000000ULL | n);
        break;
    case 3:
        qtest_writel(s, MTE + 8, 0x87000000U + n);
        break;
    }
}

/*
 * Checks drawn from Origami's O2 graphics driver evidence datasheet, which
 * records what the IRIX 6.5.30 kernel, X server and OpenGL library write.
 */
#define LINEAR_A (RE + 0x1700)
#define LINEAR_B (RE + 0x1780)
#define EVIDENCE_A 0x400000ULL
#define EVIDENCE_B 0x420000ULL
#define EVIDENCE_CHUNK 0x20000U

/* Map all 32 pages of one linear TLB onto themselves from @physical. */
static void linear_identity(QTestState *s, uint64_t tlb, uint64_t physical)
{
    for (unsigned i = 0; i < 16; i++) {
        uint64_t page = (physical >> 12) + 2 * i;

        qtest_writeq(s, tlb + 8 * i, (0x80000000ULL | page) << 32 |
                                     (0x80000000ULL | (page + 1)));
    }
}

static void linear_clear(QTestState *s, uint64_t physical, uint32_t length,
                         uint8_t value, bool ok)
{
    qtest_writel(s, MTE, 0x11);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x18, value);
    qtest_writel(s, MTE + 0x48, 1);
    qtest_writel(s, MTE + 0x30, physical);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, physical + length - 1) ==
                    MEMTX_OK, ==, ok);
}

static void assert_bytes(QTestState *s, uint64_t physical,
                         const uint8_t *expected, size_t length)
{
    g_autofree uint8_t *actual = g_malloc(length);

    qtest_memread(s, physical, actual, length);
    g_assert_cmpmem(actual, length, expected, length);
}

/*
 * The kernel zeroes and copies host memory with MTE modes 0x11 (clear
 * through linear A) and 0x897 (copy linear A to linear B), in chunks of at
 * most 128 KiB whose SRC and DST words are physical addresses the linear
 * TLBs map onto themselves. After a failed buffer load it zeroes every
 * linear TLB register, which must leave no mapping behind.
 */
static void test_evidence_linear_identity(void)
{
    QTestState *s = start();
    g_autofree uint8_t *pattern = g_malloc(EVIDENCE_CHUNK);
    g_autofree uint8_t *fill = g_malloc(EVIDENCE_CHUNK);

    for (unsigned i = 0; i < EVIDENCE_CHUNK; i++) {
        pattern[i] = i * 7 ^ i >> 9;
    }
    linear_identity(s, LINEAR_A, EVIDENCE_A);
    linear_identity(s, LINEAR_B, EVIDENCE_B);
    qtest_memset(s, EVIDENCE_A - 16, 0x77, EVIDENCE_CHUNK + 32);
    linear_clear(s, EVIDENCE_A, EVIDENCE_CHUNK, 0, true);
    memset(fill, 0, EVIDENCE_CHUNK);
    assert_bytes(s, EVIDENCE_A, fill, EVIDENCE_CHUNK);
    g_assert_cmphex(qtest_readb(s, EVIDENCE_A - 1), ==, 0x77);
    g_assert_cmphex(qtest_readb(s, EVIDENCE_A + EVIDENCE_CHUNK), ==, 0x77);

    qtest_memwrite(s, EVIDENCE_A, pattern, EVIDENCE_CHUNK);
    qtest_memset(s, EVIDENCE_B, 0xc3, EVIDENCE_CHUNK);
    qtest_writel(s, MTE, 0x897);
    qtest_writel(s, MTE + 0x20, EVIDENCE_A);
    qtest_writel(s, MTE + 0x28, EVIDENCE_A + EVIDENCE_CHUNK - 1);
    qtest_writel(s, MTE + 0x40, 1);
    qtest_writel(s, MTE + 0x30, EVIDENCE_B);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838,
                                 EVIDENCE_B + EVIDENCE_CHUNK - 1),
                    ==, MEMTX_OK);
    assert_bytes(s, EVIDENCE_B, pattern, EVIDENCE_CHUNK);

    for (unsigned i = 0; i < 16; i++) {
        qtest_writeq(s, LINEAR_A + 8 * i, 0);
        qtest_writeq(s, LINEAR_B + 8 * i, 0);
    }
    memset(fill, 0xc3, EVIDENCE_CHUNK);
    qtest_memset(s, EVIDENCE_B, 0xc3, EVIDENCE_CHUNK);
    qtest_memset(s, 0, 0xc3, EVIDENCE_CHUNK);
    /* A clear through invalid entries writes nothing, not even page 0. */
    linear_clear(s, EVIDENCE_A, EVIDENCE_CHUNK, 0x5a, true);
    assert_bytes(s, EVIDENCE_A, pattern, EVIDENCE_CHUNK);
    assert_bytes(s, 0, fill, EVIDENCE_CHUNK);
    /* A copy through them is refused before moving any byte. */
    qtest_writel(s, MTE, 0x897);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838,
                                 EVIDENCE_B + EVIDENCE_CHUNK - 1),
                    !=, MEMTX_OK);
    assert_bytes(s, EVIDENCE_B, fill, EVIDENCE_CHUNK);
    qtest_quit(s);
}

/*
 * The X server copies whole 256-byte blocks of 8 or 16 rows with MTE mode
 * 0x820 (copy, source TLB B, destination TLB A, 8-bit), its SRC and DST
 * words a byte column in bits 31:16 and a row in bits 15:0.
 */
static void test_evidence_mte_block_copy(void)
{
    QTestState *s = start();
    uint8_t row[256], expected[256];

    tile(s, 0, 0, 0x50);
    tile(s, 1, 0, 0x60);
    qtest_memset(s, 0x500000, 0x3c, 0x10000);
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 256; x++) {
            row[x] = x ^ y * 37;
        }
        qtest_memwrite(s, 0x600000 + y * 512, row, sizeof(row));
    }
    qtest_writel(s, MTE, 0x820);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x20, xy(0, 0));
    qtest_writel(s, MTE + 0x28, xy(255, 7));
    qtest_writel(s, MTE + 0x40, 1);
    qtest_writel(s, MTE + 0x48, 1);
    qtest_writel(s, MTE + 0x30, xy(256, 8));
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, xy(511, 15)), ==, MEMTX_OK);
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 256; x++) {
            expected[x] = x ^ y * 37;
        }
        assert_bytes(s, 0x500000 + (y + 8) * 512 + 256, expected, 256);
    }
    memset(expected, 0x3c, sizeof(expected));
    assert_bytes(s, 0x500000 + 7 * 512 + 256, expected, 256);
    assert_bytes(s, 0x500000 + 16 * 512 + 256, expected, 256);
    assert_bytes(s, 0x500000 + 8 * 512, expected, 256);
    qtest_quit(s);
}

/*
 * IRIX 6.5.30 drops a client's uncommitted parameters when a kernel MTE
 * copy or clear runs between them and the client's START; that effect is
 * inferred from the kernel's calling sequence. The model need not drop
 * them, and keeps them: the client's later START still uses them.
 */
static void test_evidence_parameters_across_mte(void)
{
    QTestState *s = start();
    uint8_t bytes[12];
    uint8_t zero[64] = { 0 };

    tile(s, 0, 0, 3);
    qtest_memset(s, 0x30000, 0x5c, sizeof(bytes));
    setup(s, 0x228, false);
    qtest_writel(s, DE + 0xd0, 0x1234abcd);
    qtest_writel(s, DE + 0x70, xy(1, 0));
    linear_identity(s, LINEAR_A, EVIDENCE_A);
    qtest_memset(s, EVIDENCE_A, 0xee, sizeof(zero));
    linear_clear(s, EVIDENCE_A, sizeof(zero), 0, true);
    assert_bytes(s, EVIDENCE_A, zero, sizeof(zero));
    qtest_writel(s, DE + 0x874, xy(1, 0));
    qtest_memread(s, 0x30000, bytes, sizeof(bytes));
    g_assert_cmphex(ldl_be_p(bytes), ==, 0x5c5c5c5c);
    g_assert_cmphex(ldl_be_p(bytes + 4), ==, 0x1234abcd);
    g_assert_cmphex(ldl_be_p(bytes + 8), ==, 0x5c5c5c5c);
    qtest_quit(s);
}

/*
 * Retained fog and texture words remain writable. Active triangle fog is
 * unsupported. Line AA control is inactive for triangles, and a line-only
 * START must not turn a triangle into a line.
 */
static void test_evidence_unsupported_stages(void)
{
    static const unsigned types[] = { 1, 2, 4, 5, 6, 7, 8 };
    QTestState *s = triangle_start("");
    Shape c;

    native_parameters(s, true, false);
    c = triangle_prepare(s, 0, 0, 0);
    triangle_vertices(s, triangle_integer, NULL, false);
    g_assert_cmpint(memtx_writel(s, DE + 0x170, 0x11223300), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, DE + 0x170), ==, 0x11223300);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmpint(memtx_writel(s, DE + 0x178 + 8 * i, 0x000ff000),
                        ==, MEMTX_OK);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(types); i++) {
        for (unsigned depth = 1; depth <= 2; depth++) {
            uint32_t mode = types[i] << 20 | depth << 18;

            g_assert_cmpint(memtx_writel(s, DE + 0x110, mode), ==, MEMTX_OK);
            g_assert_cmphex(qtest_readl(s, DE + 0x110), ==, mode);
        }
    }
    qtest_writel(s, DE + 0x1b0, 3);
    qtest_writel(s, DE + 0x1b8, UINT32_MAX);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | (1U << 14));
    g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), !=, MEMTX_OK);
    triangle_pixels(s, &c, triangle_empty, 0, 3, UINT32_MAX);
    g_assert_cmpint(memtx_writeq(s, DE + 0x990, 0x12345678abcd0100ULL),
                    !=, MEMTX_OK);
    triangle_pixels(s, &c, triangle_empty, 0, 3, UINT32_MAX);
    for (unsigned aa = 0; aa < 2; aa++) {
        shape_memory(s, &c, NULL);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | aa << 12);
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR),
                        ==, MEMTX_OK);
        triangle_pixels(s, &c, triangle_ten, TRIANGLE_COLOR, 3, UINT32_MAX);
    }
    qtest_quit(s);
}

/*
 * The IRIX 6.5.30 kernel writes zero to the engine-status bank after
 * translation loads and at the end of context save, once it has copied the
 * entries from the status start pointer up to the write pointer. Whether
 * the write moves either pointer is unknown. Synchronous commits keep the
 * two equal, so here it must leave the ring and both pointers unchanged.
 */
static void test_ifb_status_bank_zero(void)
{
    QTestState *s = triangle_start("");

    for (unsigned n = 0; n < 5; n++) {
        qtest_writel(s, DE + 0xd0, 0x22000000U + n);
    }
    ifb_head(s, 5);
    g_assert_cmpint(memtx_writel(s, STATUS + 8, 0), ==, MEMTX_OK);
    ifb_head(s, 5);
    for (unsigned n = 0; n < 5; n++) {
        ifb_entry(s, n, 0x121a00000000ULL,
                  (uint64_t)(0x22000000U + n) << 32);
    }
    qtest_quit(s);
}

static void test_ifb_wrap(void)
{
    QTestState *s = triangle_start("");

    for (unsigned n = 0; n < 64; n++) {
        qtest_writel(s, DE + 0xd0, 0x11000000U + n);
        ifb_head(s, (n + 1) % 64);
    }
    for (unsigned n = 0; n < 64; n++) {
        ifb_entry(s, n, 0x121a00000000ULL,
                   (uint64_t)(0x11000000U + n) << 32);
    }
    qtest_system_reset(s);
    ifb_head(s, 0);
    for (unsigned n = 0; n < 64; n++) {
        ifb_entry(s, n, 0, 0);
    }
    for (unsigned n = 0; n < 200; n++) {
        ifb_varied_write(s, n);
    }
    ifb_head(s, 8);
    for (unsigned slot = 0; slot < 64; slot++) {
        unsigned n = slot < 8 ? 192 + slot : 128 + slot;
        uint64_t header = 0, data = 0;

        switch (n % 4) {
        case 0:
            header = 0x121a;
            data = (uint64_t)(0x54000000U + n) << 32;
            break;
        case 1:
            header = 0x0a10;
            data = 0x65000000U + n;
            break;
        case 2:
            header = 0x1901;
            data = 0x1122334455000000ULL | n;
            break;
        case 3:
            header = 0x1301;
            data = (uint64_t)(0x87000000U + n) << 32;
            break;
        }
        ifb_entry(s, slot, header << 32, data);
    }
    qtest_quit(s);
}

static void ifb_refusal(QTestState *s, const Shape *c, const char *path,
                        uint64_t address, unsigned size, bool write)
{
    const unsigned status[] = { 0x10, 0x18, 0x40, 0x48, 0x250, 0x258 };
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS], saved[6], value;
    IfbSnapshot old_ifb, new_ifb;
    unsigned irq = 0;

    save_paired_renderer(s, path, before);
    ifb_snapshot(s, &old_ifb);
    for (unsigned i = 0; i < ARRAY_SIZE(status); i++) {
        saved[i] = qtest_readq(s, CRIME + status[i]);
    }
    for (unsigned i = 0; i < 5; i++) {
        irq |= qtest_get_irq(s, i) << i;
    }
    g_assert_cmpint(write ? memtx_write(s, address, UINT64_MAX, size) :
                    memtx_read(s, address, &value, size), !=, MEMTX_OK);
    ifb_snapshot(s, &new_ifb);
    g_assert_cmpmem(&old_ifb, sizeof(old_ifb), &new_ifb, sizeof(new_ifb));
    for (unsigned i = 0; i < ARRAY_SIZE(status); i++) {
        g_assert_cmphex(qtest_readq(s, CRIME + status[i]), ==, saved[i]);
    }
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmpint(qtest_get_irq(s, i), ==, !!(irq & (1U << i)));
    }
    triangle_pixels(s, c, triangle_empty, 0, 3, UINT32_MAX);
    save_paired_renderer(s, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
}

static void test_ifb_atomic(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-ifb-atomic-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    const struct { unsigned offset, size; } bad[] = {
        {0, 1}, {0x200, 2}, {1, 4}, {4, 8}, {0x1fc, 8},
        {0x3fc, 8}, {0x3fd, 4}, {0x404, 4}, {0x408, 4}, {0x800, 8},
    };
    QTestState *s = triangle_start("");
    Shape c = triangle_prepare(s, 0, 0, 0);
    unsigned head;
    IfbSnapshot before, after;

    triangle_vertices(s, triangle_integer, NULL, true);
    head = ifb_next(s);
    qtest_writeq(s, RE + 0x200, 0x321a00000000ULL);
    qtest_writeq(s, RE, (uint64_t)TRIANGLE_COLOR << 32);
    ifb_head(s, head);
    triangle_pixels(s, &c, triangle_empty, 0, 3, UINT32_MAX);
    for (unsigned n = 0; n < ARRAY_SIZE(bad); n++) {
        ifb_refusal(s, &c, path, RE + bad[n].offset, bad[n].size, true);
        ifb_refusal(s, &c, path, RE + bad[n].offset, bad[n].size, false);
    }
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x200);
    qtest_writel(s, DE + 0x1b0, 6);
    head = ifb_next(s);
    g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==, MEMTX_OK);
    ifb_entry(s, head, 0x321a00000000ULL, (uint64_t)TRIANGLE_COLOR << 32);
    ifb_head(s, (head + 1) % 64);
    triangle_pixels(s, &c, triangle_ten, TRIANGLE_COLOR, 6, UINT32_MAX);
    shape_memory(s, &c, NULL);
    head = ifb_next(s);
    g_assert_cmpint(memtx_writeq(s, DE + 0x870, 0x1122334455667788ULL),
                    ==, MEMTX_OK);
    ifb_entry(s, head, 0x3a0e00000000ULL, 0x1122334455667788ULL);
    ifb_head(s, (head + 1) % 64);
    triangle_pixels(s, &c, triangle_ten, TRIANGLE_COLOR, 6, UINT32_MAX);
    c = triangle_prepare(s, 0, 125, 125);
    {
        const int vertex[3][2] = {
            {126 * 64, 126 * 64}, {130 * 64, 126 * 64},
            {126 * 64, 130 * 64},
        };

        triangle_vertices(s, vertex, NULL, true);
    }
    tile(s, 0, 16, 0x7fff);
    ifb_snapshot(s, &before);
    triangle_failure(s, &c, path, DE + 0x8d0, 0xfeedbeef, 4);
    ifb_snapshot(s, &after);
    g_assert_cmpmem(&before, sizeof(before), &after, sizeof(after));
    triangle_irq_levels(s, 1);
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_ifb_restore(void)
{
    QTestState *s = triangle_start("");
    Shape c = triangle_prepare(s, 0, 0, 0);
    uint64_t payload;
    unsigned marked, head;

    triangle_vertices(s, triangle_integer, NULL, true);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x200);
    qtest_writel(s, DE + 0x1b0, 6);
    marked = ifb_next(s);
    qtest_writel(s, DE + 0x8d0, TRIANGLE_COLOR);
    qtest_writel(s, DE + 0xd0, 0xaabbccdd);
    qtest_writel(s, DE + 0x84, triangle_raw(64));
    ifb_entry(s, marked, 0x321a00000000ULL,
               (uint64_t)TRIANGLE_COLOR << 32);
    ifb_entry(s, (marked + 1) % 64, 0x121a00000000ULL,
               0xaabbccdd00000000ULL);
    ifb_entry(s, (marked + 2) % 64, 0x0a1000000000ULL, triangle_raw(64));
    head = ifb_next(s);
    shape_memory(s, &c, NULL);
    payload = qtest_readq(s, RE + marked * 8);
    qtest_writel(s, DE + 0xd0, payload >> 32);
    ifb_entry(s, head, 0x121a00000000ULL, payload);
    ifb_head(s, (head + 1) % 64);
    triangle_pixels(s, &c, triangle_empty, 0, 3, UINT32_MAX);
    head = ifb_next(s);
    qtest_writel(s, DE + 0x9f0, 0xabcdef01);
    ifb_entry(s, head, 0x323e00000000ULL, 0xabcdef0100000000ULL);
    ifb_head(s, (head + 1) % 64);
    triangle_pixels(s, &c, triangle_ten, TRIANGLE_COLOR, 6, UINT32_MAX);
    triangle_irq_levels(s, 1);
    qtest_quit(s);
}

static void test_ifb_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-ifb-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *from = triangle_start("");
    QTestState *to = triangle_start("-incoming defer");
    uint64_t words[RENDER_WORDS];
    IfbSnapshot before, after;

    for (unsigned n = 0; n < 97; n++) {
        ifb_varied_write(from, n);
    }
    qtest_writeq(from, RE + 0x3f8, 0xfedcba9876543210ULL);
    qtest_writeq(from, RE + 0x18, 0x1020304050607080ULL);
    ifb_snapshot(from, &before);
    save_paired_renderer(from, path, words);
    ifb_snapshot(from, &after);
    g_assert_cmpmem(&before, sizeof(before), &after, sizeof(after));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    ifb_snapshot(to, &after);
    g_assert_cmpmem(&before, sizeof(before), &after, sizeof(after));
    ifb_varied_write(from, 97);
    ifb_varied_write(to, 97);
    ifb_snapshot(from, &before);
    ifb_snapshot(to, &after);
    g_assert_cmpmem(&before, sizeof(before), &after, sizeof(after));
    ifb_head(to, 34);
    qtest_system_reset(to);
    ifb_head(to, 0);
    for (unsigned n = 0; n < 64; n++) {
        ifb_entry(to, n, 0, 0);
    }
    triangle_irq_levels(to, 0);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_ifb_profiles(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *s = start_profile(legacy ? "legacy-software" : "1.5",
                                      false, !legacy, "");
        uint64_t value;

        g_assert_cmpint(memtx_writeq(s, RE, UINT64_MAX), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readq(s, RE + 0x3f8, &value), !=, MEMTX_OK);
        if (!legacy) {
            qtest_writeq(s, RE + 0x1700, 0x8000000380000009ULL);
            qtest_writel(s, MTE + 8, UINT32_MAX);
            ifb_head(s, 0);
        }
        qtest_quit(s);
    }
}

static void test_ifb_invalid_stream(void)
{
    const char name[] = "sgi-crime/renderer";
    g_autofree char *dir = g_dir_make_tmp("crime-ifb-invalid-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *incoming = g_strdup_printf("exec:cat %s", quoted);
    g_autofree char *stream = NULL;
    QTestState *from = triangle_start("");
    QTestState *to = triangle_start("-incoming defer");
    uint64_t words[RENDER_WORDS];
    gsize length;
    char *state;

    qtest_writel(from, DE + 0xd0, 0x12345678);
    save_paired_renderer(from, path, words);
    g_assert_true(g_file_get_contents(path, &stream, &length, NULL));
    state = memmem(stream, length, name, sizeof(name) - 1);
    g_assert_nonnull(state);
    state += sizeof(name) - 1;
    g_assert_cmpuint(length - (state - stream), >=, 4 + 476 * 8 + 1);
    g_assert_cmphex((uint8_t)state[4 + 476 * 8], ==, 1);
    state[4 + 476 * 8] = 64;
    g_assert_true(g_file_set_contents(path, stream, length, NULL));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s,'exit-on-error':false}}",
        incoming);
    sgi_wait_migration(to, "failed");
    ifb_head(to, 0);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_buffer_modes_primitives(void)
{
    const uint32_t upper[] = { 0x80000000U, 0x2000, 0xaaaaa000U, 0xffffe000U };
    QTestState *s = start();

    for (unsigned pattern = 0; pattern < ARRAY_SIZE(upper); pattern++) {
        for (unsigned bank = 0; bank < 3; bank++) {
            Shape a = alpha_shape();
            a.bank = bank;
            alpha_prepare(s, &a, 0x700);
            qtest_writel(s, DE + 8, upper[pattern] | (bank << 10) | 0x228);
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            g_autofree uint32_t *expected = alpha_expected(&a, true);
            shape_memory(s, &a, expected);
            Shape t = triangle_prepare(s, bank, 0, 0);
            triangle_vertices(s, triangle_integer, NULL, true);
            qtest_writel(s, DE + 8, upper[pattern] | (bank << 10) | 0x228);
            g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==,
                            MEMTX_OK);
            triangle_pixels(s, &t, triangle_ten, TRIANGLE_COLOR, 3, UINT32_MAX);
        }
    }
    qtest_quit(s);
}

static void test_x_domain_invisible(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-x-domain-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree uint8_t *data = g_malloc(65536);
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    QTestState *s = start_indexed_state();

    for (unsigned index = 0; index < 256; index++) {
        tile(s, 0, index, page_for(0, index));
    }
    qtest_memset(s, 0x30000, 0xa5, 256 * 65536);
    setup(s, 0x200, false);
    qtest_writel(s, DE + 0x18, 0x208);
    qtest_writel(s, DE + 0x58, 0x026c03c4);
    qtest_writel(s, DE + 0x60, 0x01000020);
    qtest_writel(s, DE + 0xd0, 0x73);
    qtest_writeq(s, DE + 0x48, bounds(9, 9, 1, 1));
    qtest_writel(s, DE + 0x70, xy(5, 6));
    qtest_writel(s, DE + 0x74, xy(4, 32768));
    save_paired_renderer(s, path, before);
    g_assert_cmpint(memtx_writel(s, DE + 0x870, xy(1, 32768)), ==, MEMTX_OK);
    save_paired_renderer(s, path, after);
    before[192 + 0x70 / 4] = xy(1, 32768);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    for (unsigned index = 0; index < 256; index++) {
        qtest_memread(s, 0x30000 + index * 65536, data, 65536);
        for (unsigned byte = 0; byte < 65536; byte++) {
            g_assert_cmphex(data[byte], ==, 0xa5);
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_x_domain_coordinates(void)
{
    const unsigned coordinates[] = { 2047, 2048, 32767, 32768, 65535 };
    QTestState *s = start_indexed_state();

    for (unsigned axis = 0; axis < 2; axis++) {
        for (unsigned n = 0; n < ARRAY_SIZE(coordinates); n++) {
            Shape c = shape_default();
            c.width = axis ? 4 : 2048;
            c.height = axis ? 2048 : 4;
            c.primitive = 0;
            c.x0 = axis ? 2 : coordinates[n];
            c.y0 = axis ? coordinates[n] : 2;
            c.x1 = c.y1 = 65535;
            if (coordinates[n] == 65535) {
                c.origin_x = !axis;
                c.origin_y = axis;
            }
            shape_run(s, &c);
            for (unsigned reverse = 0; reverse < 2; reverse++) {
                c.origin_x = c.origin_y = 0;
                c.primitive = 0x01000020;
                c.x0 = reverse ? (axis ? 2 : coordinates[n]) : 0;
                c.y0 = reverse ? (axis ? coordinates[n] : 2) : 0;
                c.x1 = reverse ? 0 : (axis ? 2 : coordinates[n]);
                c.y1 = reverse ? 0 : (axis ? coordinates[n] : 2);
                shape_run(s, &c);
                if (coordinates[n] == 65535 && !axis && !reverse) {
                    g_assert_cmphex(qtest_readb(s,
                                    pixel_address(0, 2047, 0, 1)),
                                    ==, 0x66);
                    g_assert_cmphex(qtest_readb(s,
                                    pixel_address(0, 2047, 1, 1)),
                                    ==, 0xaa);
                }
            }
        }
        for (unsigned sign = 0; sign < 2; sign++) {
            for (unsigned reverse = 0; reverse < 2; reverse++) {
                Shape c = shape_default();
                unsigned a[2] = { 0, sign ? 2 : 0 };
                unsigned b[2] = { 65535, sign ? 0 : 2 };
                c.width = axis ? 4 : 2048;
                c.height = axis ? 2048 : 4;
                c.primitive = 0x01030020;
                c.x0 = (reverse ? b : a)[axis];
                c.y0 = (reverse ? b : a)[!axis];
                c.x1 = (reverse ? a : b)[axis];
                c.y1 = (reverse ? a : b)[!axis];
                shape_run(s, &c);
            }
        }
    }
    qtest_quit(s);
}

static void test_x_domain_stipple_origins(void)
{
    QTestState *s = start_indexed_state();
    const unsigned widths[] = { 32, 64, 65 };

    for (unsigned width = 0; width < ARRAY_SIZE(widths); width++) {
        for (unsigned opaque = 0; opaque < 2; opaque++) {
            for (unsigned skip = 0; skip < 2; skip++) {
                Shape c = shape_default();
                c.width = 32;
                c.height = 8;
                c.x0 = 65535;
                c.y0 = 2;
                c.x1 = c.y1 = 0;
                c.primitive = 0x01000000 | (skip << 18) | widths[width];
                c.draw |= (1U << 19) | (opaque << 17);
                c.stipple_mode = 0x05150102;
                shape_run(s, &c);
            }
        }
    }
    for (unsigned axis = 0; axis < 2; axis++) {
        Shape c = shape_default();
        c.x = axis ? 0 : 2046;
        c.y = axis ? 2046 : 0;
        c.width = axis ? 8 : 2;
        c.height = axis ? 2 : 8;
        c.x0 = axis ? 0 : 2048;
        c.y0 = axis ? 2048 : 0;
        c.x1 = axis ? 5 : 2048;
        c.y1 = axis ? 2048 : 5;
        c.primitive = 0x01000041;
        shape_run(s, &c);
    }
    for (unsigned clip = 0; clip < 4; clip++) {
        Shape c = shape_default();
        c.width = c.height = 12;
        c.x0 = c.x1 = c.y0 = 0;
        c.y1 = 65535;
        c.primitive = 0x01000041;
        c.origin_x = 3;
        c.origin_y = 2;
        if (clip & 1) {
            c.draw |= 1U << 20;
            c.scissor = bounds(0, 0, 8, 8);
        }
        if (clip & 2) {
            c.clip = 0x21;
            c.screen = bounds(2, 2, 3, 10);
        }
        shape_run(s, &c);
    }
    qtest_quit(s);
}

static void test_x_domain_bounds(void)
{
    QTestState *s = start_indexed_state();
    Shape c = shape_default();
    uint32_t unchanged[64];

    c.width = c.height = 8;
    c.x0 = c.y0 = 0;
    c.x1 = c.y1 = 65535;
    c.primitive = 0x0100ffff;
    shape_memory(s, &c, NULL);
    for (unsigned i = 0; i < ARRAY_SIZE(unchanged); i++) {
        unchanged[i] = c.old & 255;
    }
    /* All center/width products are invisible; no tile is needed. */
    c.y0 = c.y1 = 65535;
    shape_stage(s, &c);
    for (unsigned repeat = 0; repeat < 256; repeat++) {
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    }
    shape_memory(s, &c, unchanged);
    c.x0 = c.x1 = c.y0 = c.y1 = 65535;
    shape_stage(s, &c);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    for (unsigned fault = 0; fault < 4; fault++) {
        shape_stage(s, &c);
        if (!fault) {
            qtest_writel(s, DE + 0x58, 0x80000000);
        } else if (fault == 1) {
            qtest_writel(s, DE + 0x58, 0x8000);
        } else if (fault == 2) {
            qtest_writel(s, DE + 0x18, c.draw | 0x100);
        } else {
            qtest_writel(s, DE + 0x18, c.draw | (1U << 19));
            qtest_writel(s, DE + 0xc0, 0x08070000);
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_ERROR);
        shape_memory(s, &c, unchanged);
    }
    for (unsigned width = 65504; width <= 65535; width += 31) {
        c.x0 = c.x1 = c.y0 = c.y1 = 0;
        c.origin_x = c.origin_y = 1;
        c.primitive = 0x01000000 | width;
        map_region(s, 0, 1, 0, 0, 1025, 1025);
        shape_run(s, &c);
    }
    qtest_quit(s);
}

static void test_x_domain_transactions(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-x-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    Shape c = shape_default();
    c.width = 2048;
    c.height = 3;
    c.x0 = c.y0 = 0;
    c.x1 = 65535;
    c.y1 = 2;
    c.primitive = 0x01000020;
    g_autofree uint32_t *expected = shape_reference(&c);
    g_autofree uint32_t *unchanged = g_new(uint32_t, c.width * c.height);
    for (unsigned i = 0; i < c.width * c.height; i++) {
        unchanged[i] = c.old & 255;
    }
    for (unsigned form = 0; form < 3; form++) {
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
        uint64_t value = form == 2 ? xy(65535, 2) :
                         form ? xy(65535, 2) : xy(0, 0);
        unsigned offset = form == 1 ? 0x74 : 0x70;
        unsigned width = form == 2 ? 8 : 4;
        map_region(s, 0, 1, 0, 0, 2047, 2);
        shape_memory(s, &c, NULL);
        shape_stage(s, &c);
        if (form == 2) {
            qtest_writeq(s, DE + 0x70, (uint64_t)xy(9, 6) << 32 | xy(10, 7));
        } else {
            qtest_writel(s, DE + offset, xy(9, 6));
        }
        shape_memory(s, &c, unchanged);
        qtest_writel(s, DE + 0x58, 0x80000000);
        save_paired_renderer(s, path, before);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + offset, value, width), ==,
                        MEMTX_ERROR);
        save_paired_renderer(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        shape_memory(s, &c, unchanged);
        qtest_writel(s, DE + 0x58, 0);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + offset, value, width), ==,
                        MEMTX_OK);
        shape_memory(s, &c, expected);
        save_paired_renderer(s, path, after);
        g_assert_cmphex(after[192 + 0x70 / 4], ==, xy(0, 0));
        g_assert_cmphex(after[192 + 0x74 / 4], ==, xy(65535, 2));
    }
    for (unsigned alias = 0; alias < 2; alias++) {
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
        map_region(s, 0, 1, 0, 0, 2047, 2);
        shape_memory(s, &c, NULL);
        shape_stage(s, &c);
        qtest_writel(s, DE + 0x74, xy(3, 0));
        tile(s, 0, 3, alias ? page_for(0, 0) : 0x400);
        save_paired_renderer(s, path, before);
        g_assert_cmpint(memtx_writel(s, DE + 0x874, xy(65535, 2)), ==,
                        MEMTX_ERROR);
        save_paired_renderer(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        shape_memory(s, &c, unchanged);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static Shape pipeline_prepare(QTestState *s, unsigned mode, unsigned bank)
{
    Shape c = shape_default();

    c.bank = bank;
    c.bytes = 1U << ((mode >> 8) & 3);
    c.width = c.height = 4;
    c.old = c.bytes == 4 ? 0x9d3b76a5 : 0x8e7b;
    map_region(s, bank, c.bytes, 0, 0, 3, 3);
    shape_memory(s, &c, NULL);
    setup(s, mode | bank << 10, false);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW);
    qtest_writel(s, DE + 0x60, 0x02020000);
    qtest_writel(s, DE + 0x80, 0);
    qtest_writel(s, DE + 0x84, 0);
    qtest_writel(s, DE + 0x88, 512);
    qtest_writel(s, DE + 0x8c, 0);
    qtest_writel(s, DE + 0x90, 0);
    qtest_writel(s, DE + 0x94, 512);
    return c;
}

static const unsigned shade_dx[] = { 0xf0, 0xf4, 0x100, 0x104 };
static const unsigned shade_dy[] = { 0xf8, 0xfc, 0x108, 0x10c };

static void pipeline_shade(QTestState *s, const int start[4],
                             const int dx[4], const int dy[4])
{
    for (unsigned k = 0; k < 4; k++) {
        qtest_writel(s, DE + 0xe0 + 4 * k, start[k]);
        qtest_writel(s, DE + shade_dx[k], dx[k]);
        qtest_writel(s, DE + shade_dy[k], dy[k]);
    }
}

static void test_pipeline_smooth(void)
{
    const int start[4] = { 10 * 4096, 20 * 4096, 30 * 4096, 40 * 4096 };
    const int dx[4] = { 4096, -8192, 12288, -4096 };
    const int dy[4] = { 8192, 4096, -4096, -8192 };
    QTestState *s = start_indexed_state();
    Shape c = pipeline_prepare(s, 0x228, 0);

    pipeline_shade(s, start, dx, dy);
    qtest_writel(s, DE + 0x80, 32);
    qtest_writel(s, DE + 0x84, 16);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x10000);
    for (unsigned word = 0; word < 12; word++) {
        unsigned offset = 0xe0 + 4 * word;
        int value = word < 4 ? start[word] : 0;

        for (unsigned k = 0; k < 4; k++) {
            if (offset == shade_dx[k]) {
                value = dx[k];
            } else if (offset == shade_dy[k]) {
                value = dy[k];
            }
        }
        shape_memory(s, &c, NULL);
        g_assert_cmpint(memtx_writel(s, DE + 0x800 + offset, value),
                        ==, MEMTX_OK);
        /* Independent four-channel fixture from the cleared contract. */
        g_assert_cmphex(qtest_readl(s, pixel_address(0, 2, 3, 4)),
                        ==, 0x12132120);
        g_assert_cmphex(qtest_readl(s, pixel_address(0, 0, 0, 4)), ==, c.old);
    }
    qtest_quit(s);
}

static void test_pipeline_blend_fixture(void)
{
    QTestState *s = start_indexed_state();
    Shape c = pipeline_prepare(s, 0x124, 0);

    c.old = 0x8000 | 3 << 10 | 19 << 5 | 27;
    shape_memory(s, &c, NULL);
    qtest_writel(s, DE + 0xd0, 0x1153c940);
    qtest_writel(s, DE + 0x1a8, 0x45);
    for (unsigned dither = 0; dither < 2; dither++) {
        shape_memory(s, &c, NULL);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x400 | dither << 8);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        g_assert_cmphex(qtest_readw(s, pixel_address(0, 0, 0, 2)), ==,
                        0x8000 | (dither ? 3 : 2) << 10 |
                        (dither ? 17 : 16) << 5 | (dither ? 27 : 26));
        if (dither) {
            g_assert_cmphex(qtest_readw(s, pixel_address(0, 0, 3, 2)), ==,
                            0x8000 | 2 << 10 | 16 << 5 | 26);
        }
    }
    qtest_quit(s);
}

/* Reduced fractions keep this reference independent of the device's scale. */
typedef struct PipelineFraction {
    int64_t n, d;
} PipelineFraction;

static PipelineFraction pipeline_fraction(int64_t n, int64_t d)
{
    int64_t a = n < 0 ? -n : n, b = d;

    while (b) {
        int64_t r = a % b;

        a = b;
        b = r;
    }
    return (PipelineFraction) { n / a, d / a };
}

static PipelineFraction pipeline_product(PipelineFraction a, PipelineFraction b)
{
    return pipeline_fraction(a.n * b.n, a.d * b.d);
}

static PipelineFraction pipeline_sum(PipelineFraction a, PipelineFraction b,
                                      int sign)
{
    return pipeline_fraction(a.n * b.d + sign * b.n * a.d, a.d * b.d);
}

static PipelineFraction pipeline_factor(unsigned selector, unsigned channel,
                                         const PipelineFraction *s,
                                         const PipelineFraction *d,
                                         const PipelineFraction *c,
                                         bool source)
{
    PipelineFraction one = { 1, 1 };
    PipelineFraction value;

    switch (selector) {
    case 0:
        return (PipelineFraction) { 0, 1 };
    case 1:
        return one;
    case 2:
    case 3:
        value = source ? d[channel] : s[channel];
        break;
    case 4:
    case 5:
        value = s[3];
        break;
    case 6:
    case 7:
        value = d[3];
        break;
    case 8:
    case 9:
        value = c[channel];
        break;
    case 10:
    case 11:
        value = c[3];
        break;
    case 12:
        if (channel == 3) {
            return one;
        }
        value = pipeline_sum(one, d[3], -1);
        return s[3].n * value.d < value.n * s[3].d ? s[3] : value;
    default:
        g_assert_not_reached();
    }
    return selector & 1 ? pipeline_sum(one, value, -1) : value;
}

static unsigned pipeline_rank(unsigned x, unsigned y)
{
    return 8 * ((x ^ y) & 1) + 4 * (y & 1) +
           2 * (((x ^ y) >> 1) & 1) + ((y >> 1) & 1);
}

static uint32_t pipeline_fraction_reference(unsigned mode,
                                              const PipelineFraction src[4],
                                              uint32_t old, uint32_t constant,
                                              unsigned function, unsigned draw,
                                              unsigned logic, uint32_t plane,
                                              unsigned x, unsigned y,
                                              unsigned byte_phase)
{
    bool packed16 = ((mode >> 2) & 3) == 1;
    bool rgb = ((mode >> 4) & 15) == 1;
    unsigned bytes = 1U << ((mode >> 8) & 3);
    unsigned half = packed16 && bytes == 4 && (mode & 3) == 3 ? 16 : 0;
    uint32_t meaningful = packed16 ? 0xffffU << half :
                          rgb ? 0xffffff00U : UINT32_MAX;
    uint32_t selected = old >> half, packed = 0, mask = meaningful;
    PipelineFraction dst[4], con[4];

    for (unsigned k = 0; k < 4; k++) {
        unsigned shift = 24 - 8 * k;

        con[k] = pipeline_fraction((constant >> shift) & 255, 255);
        if (packed16) {
            dst[k] = k == 3 ?
                pipeline_fraction((selected >> 15) & 1, 1) :
                pipeline_fraction((selected >> (10 - 5 * k)) & 31, 31);
        } else {
            dst[k] = rgb && k == 3 ? pipeline_fraction(1, 1) :
                     pipeline_fraction((selected >> shift) & 255, 255);
        }
    }
    for (unsigned k = 0; k < 4; k++) {
        PipelineFraction v = src[k];
        unsigned q;

        if (draw & 0x400) {
            unsigned op = function >> 8;
            PipelineFraction a = pipeline_product(src[k], pipeline_factor(
                (function >> 4) & 15, k, src, dst, con, true));
            PipelineFraction b = pipeline_product(dst[k], pipeline_factor(
                function & 15, k, src, dst, con, false));
            bool less = src[k].n * dst[k].d < dst[k].n * src[k].d;

            switch (op) {
            case 0:
                v = pipeline_sum(a, b, 1);
                break;
            case 1:
                v = less ? src[k] : dst[k];
                break;
            case 2:
                v = less ? dst[k] : src[k];
                break;
            case 3:
                v = pipeline_sum(a, b, -1);
                break;
            case 4:
                v = pipeline_sum(b, a, -1);
                break;
            default:
                g_assert_not_reached();
            }
            v.n = CLAMP(v.n, 0, v.d);
        }
        if (packed16 && k < 3) {
            int64_t scaled = 31 * v.n;

            q = scaled / v.d;
            if ((draw & 0x100) &&
                32 * (scaled - q * v.d) >
                (2 * pipeline_rank(x, y) + 1) * v.d) {
                q++;
            }
            packed |= MIN(q, 31) << (10 - 5 * k);
        } else if (packed16) {
            packed |= MIN(1, 2 * v.n / v.d) << 15;
        } else {
            q = (510 * v.n + v.d) / (2 * v.d);
            packed |= q << (24 - 8 * k);
        }
    }
    packed <<= half;
    if (draw & 0x80) {
        mask &= plane;
    }
    for (unsigned b = 0; b < bytes; b++) {
        if (!(draw & (1U << (6 - ((byte_phase + b) & 3))))) {
            mask &= ~(0xffU << (8 * (bytes - 1 - b)));
        }
    }
    if (draw & 0x200) {
        packed = rop_result(logic, packed, old, bytes * 8);
    }
    return (old & ~mask) | (packed & mask);
}

static uint32_t pipeline_reference(unsigned mode, uint32_t source,
                                     uint32_t old, uint32_t constant,
                                     unsigned function, unsigned draw,
                                     unsigned logic, uint32_t plane,
                                     unsigned x, unsigned y)
{
    PipelineFraction normalized[4];
    unsigned bytes = 1U << ((mode >> 8) & 3);

    for (unsigned k = 0; k < 4; k++) {
        normalized[k] = pipeline_fraction((source >> (24 - 8 * k)) & 255, 255);
    }
    return pipeline_fraction_reference(mode, normalized, old, constant,
        function, draw, logic, plane, x, y, x * bytes);
}

static void pipeline_check(QTestState *s, const Shape *c, unsigned mode,
                            uint32_t color, uint32_t constant, unsigned func,
                            unsigned draw, unsigned logic, uint32_t plane)
{
    uint32_t expected[16];

    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            expected[4 * y + x] = pipeline_reference(mode, color, c->old,
                constant, func, draw, logic, plane, x, y);
        }
    }
    shape_memory(s, c, NULL);
    qtest_writel(s, DE + 0xd0, color);
    qtest_writel(s, DE + 0x18, draw);
    qtest_writel(s, DE + 0x1a0, constant);
    qtest_writel(s, DE + 0x1a8, func);
    qtest_writel(s, DE + 0x1b0, logic);
    qtest_writel(s, DE + 0x1b8, plane);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    shape_memory(s, c, expected);
}

static void test_pipeline_factors(gconstpointer selection)
{
    const unsigned modes[] = { 0x228, 0x218, 0x124, 0x224, 0x226, 0x227 };
    QTestState *s = start_indexed_state();

    {
        unsigned m = GPOINTER_TO_UINT(selection);
        Shape c = pipeline_prepare(s, modes[m], m % 3);

        /* Asymmetric factors distinguish source, destination and constant. */
        for (unsigned op = 0; op < 5; op++) {
            for (unsigned sf = 0; sf <= 12; sf++) {
                for (unsigned df = 0; df <= 11; df++) {
                    pipeline_check(s, &c, modes[m], 0x1153c940, 0x9b2371d6,
                        op << 8 | sf << 4 | df,
                        TRIANGLE_DRAW | 0x400 | ((sf ^ df) & 1) << 8,
                        3, UINT32_MAX);
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_pipeline_quantizer(void)
{
    QTestState *s = start_indexed_state();
    Shape c = pipeline_prepare(s, 0x124, 0);

    for (unsigned v = 0; v <= 255; v++) {
        unsigned total = 0;
        uint32_t color = v * 0x01010100U | 255;

        pipeline_check(s, &c, 0x124, color, 0, 0,
                       TRIANGLE_DRAW | 0x100, 3, UINT32_MAX);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                unsigned q = qtest_readw(s, pixel_address(0, x, y, 2)) & 31;

                g_assert_true(q == 31 * v / 255 || q == (31 * v + 254) / 255);
                total += q;
            }
        }
        /* |mean - ideal| <= 1/32 in code units, without floating point. */
        g_assert_cmpint(abs((int)(2 * 255 * total - 32 * 31 * v)), <=, 255);
    }
    qtest_quit(s);
}

static void test_pipeline_identity(gconstpointer selection)
{
    const unsigned modes[] = { 0x124, 0x224, 0x226, 0x227, 0x228, 0x218 };
    QTestState *s = start_indexed_state();

    {
        unsigned m = GPOINTER_TO_UINT(selection);
        Shape c = pipeline_prepare(s, modes[m], m % 3);
        unsigned codes = modes[m] == 0x228 || modes[m] == 0x218 ? 256 : 32;

        for (unsigned code = 0; code < codes; code++) {
            for (unsigned alpha = 0; alpha < 2; alpha++) {
                uint32_t selected = code | ((code * 7) & 31) << 5 |
                                    ((code * 13) & 31) << 10 | alpha << 15;

                if (codes == 256) {
                    c.old = code << 24 | (255 - code) << 16 |
                            ((code * 73) & 255) << 8 | code;
                } else if (c.bytes == 2) {
                    c.old = selected;
                } else {
                    c.old = modes[m] == 0x227 ? selected << 16 | 0x6b95 :
                                              0x6b950000 | selected;
                }
                for (unsigned dither = 0; dither < 2; dither++) {
                    for (unsigned function = 0; function < 2; function++) {
                        unsigned draw = TRIANGLE_DRAW | 0x400 | dither << 8;

                        pipeline_check(s, &c, modes[m], 0x3279e100, 0,
                                       function ? 0x45 : 0x01, draw,
                                       3, UINT32_MAX);
                        /* Repeated transparent redraw must remain exact. */
                        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0),
                                        ==, MEMTX_OK);
                        for (unsigned y = 0; y < 4; y++) {
                            for (unsigned x = 0; x < 4; x++) {
                                uint8_t data[4];

                                qtest_memread(s, pixel_address(c.bank, x, y,
                                              c.bytes), data, c.bytes);
                                uint32_t value = c.bytes == 2 ?
                                    lduw_be_p(data) : ldl_be_p(data);

                                g_assert_cmphex(value, ==, c.old);
                            }
                        }
                    }
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_pipeline_masks(gconstpointer selection)
{
    const unsigned modes[] = {
        0x228, 0x218, 0x124, 0x224, 0x225, 0x226, 0x227,
    };
    QTestState *s = start_indexed_state();

    {
        unsigned m = GPOINTER_TO_UINT(selection);
        Shape c = pipeline_prepare(s, modes[m], m % 3);

        for (unsigned rop = 0; rop < 16; rop++) {
            for (unsigned mask = 0; mask < 16; mask++) {
                unsigned draw = (TRIANGLE_DRAW & ~0x78U) | mask << 3 |
                                0x780;
                uint32_t plane = mask & 1 ? 0xa56b379d : UINT32_MAX;

                pipeline_check(s, &c, modes[m], 0x537b9146, 0x7135b9e3,
                               0x89, draw, rop, plane);
            }
        }
    }
    qtest_quit(s);
}

static uint32_t pipeline_plane_color(const int *start, const int *dx,
                                       const int *dy, int ax, int ay,
                                       int x, int y)
{
    uint32_t color = 0;

    for (unsigned k = 0; k < 4; k++) {
        int64_t n = start[k] + (int64_t)dx[k] * (x - ax) +
                    (int64_t)dy[k] * (y - ay);
        int64_t divisor = 4096;
        int64_t v = n < 0 ? -((-n + divisor - 1) / divisor) : n / divisor;

        color |= CLAMP(v, 0, 255) << (24 - 8 * k);
    }
    return color;
}

static void test_pipeline_planes(void)
{
    const int starts[] = { 10 * 4096, 20 * 4096, 30 * 4096, 40 * 4096 };
    const int dx[] = { 4096, -8192, 12288, -4096 };
    const int dy[] = { 8192, 4096, -4096, -8192 };
    const int origins[] = { 0, 1, -1, -4096, -4097 };
    const unsigned modes[] = { 0x228, 0x124, 0x227, 0x218 };
    QTestState *s = start_indexed_state();

    for (unsigned m = 0; m < ARRAY_SIZE(modes); m++) {
        for (unsigned origin = 0; origin < ARRAY_SIZE(origins); origin++) {
            for (unsigned direction = 0; direction < 4; direction++) {
                Shape c = pipeline_prepare(s, modes[m], m % 3);
                unsigned draw = TRIANGLE_DRAW | 0x10700;
                uint32_t expected[16];
                int o = origins[origin];
                const int vertices[3][2] = {
                    {96, 80}, {576, 64}, {64, 576},
                };

                const int anchors[4][2] = { {1, 9}, {9, 1}, {1, 1}, {1, 1} };
                int loaded[4];

                for (unsigned k = 0; k < 4; k++) {
                    loaded[k] = starts[k] + dx[k] * anchors[direction][0] +
                                dy[k] * anchors[direction][1];
                }
                pipeline_shade(s, loaded, dx, dy);
                qtest_writel(s, DE + 0x58, xy((uint16_t)o, (uint16_t)o));
                qtest_writel(s, DE + 0x60, 0x02000000 | direction << 16);
                qtest_writel(s, DE + 0x18, draw);
                qtest_writel(s, DE + 0x1a0, 0x9b2371d6);
                qtest_writel(s, DE + 0x1a8, 0x45);
                qtest_writel(s, DE + 0x1b0, 6);
                qtest_writel(s, DE, UINT32_MAX);
                for (unsigned v = 0; v < 3; v++) {
                    unsigned index = direction & 1 && v ? 3 - v : v;

                    for (unsigned axis = 0; axis < 2; axis++) {
                        qtest_writel(s, DE + 0x80 + 8 * v + 4 * axis,
                                    vertices[index][axis] - 64 * o);
                    }
                }
                for (unsigned y = 0; y < 4; y++) {
                    for (unsigned x = 0; x < 4; x++) {
                        uint32_t color = pipeline_plane_color(starts, dx, dy,
                                                              0, 0, x, y);
                        uint32_t old = c.old ^ ((x + 7 * y) * 0x01234567U);

                        if (c.bytes == 2) {
                            old &= 0xffff;
                        }
                        pixel_write(s, pixel_address(c.bank, x, y, c.bytes),
                                    c.bytes, old);
                        expected[y * 4 + x] = x >= 2 && y >= 2 ?
                            pipeline_reference(modes[m], color, old,
                                0x9b2371d6, 0x45, draw, 6, UINT32_MAX, x, y) :
                            old;
                    }
                }
                g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
                shape_memory(s, &c, expected);
            }
        }
    }
    /* Extreme origins produce empty coverage without changing admission. */
    for (unsigned positive = 0; positive < 2; positive++) {
        Shape c = pipeline_prepare(s, 0x227, 0);
        uint32_t expected[16];

        pipeline_shade(s, starts, dx, dy);
        qtest_writel(s, DE + 0x58, positive ? 0x7fff7fff : 0x80008000);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x10700);
        qtest_writel(s, DE + 0x1a8, 0x45);
        for (unsigned i = 0; i < 16; i++) {
            expected[i] = c.old;
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
}

static void pipeline_failure(QTestState *s, const Shape *c, const char *path,
                               uint64_t addr, uint64_t value, unsigned width)
{
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    uint32_t *old = g_new(uint32_t, c->width * c->height);
    IfbSnapshot first, last;

    for (unsigned i = 0; i < c->width * c->height; i++) {
        old[i] = c->old;
    }
    save_paired_renderer(s, path, before);
    ifb_snapshot(s, &first);
    g_assert_cmpint(memtx_write(s, addr, value, width), !=, MEMTX_OK);
    ifb_snapshot(s, &last);
    g_assert_cmpmem(&first, sizeof(first), &last, sizeof(last));
    shape_memory(s, c, old);
    save_paired_renderer(s, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    g_free(old);
}

static void test_pipeline_shade_bounds(void)
{
    static const struct {
        int start, dx, dy, anchor;
        unsigned color;
    } cases[] = {
        { -(1 << 20), 0, 0, 65, 0 },
        { -(1 << 20), 1, 0, 65, 255 },
        { (1 << 20) - 1, 0, 0, 65, 255 },
        { (1 << 20) - 1, -1, 0, 65, 0 },
        { 4096, 4096, 0, -1, 2 },
        { -256 * 4096, -1, -1, 65, 0 },
        { -255 * 4096, -4096, -4096, 65, 0 },
        { -129 * 4096, 4096, -8192, 65, 0 },
        { -128 * 4096, -4096, 8192, 65, 0 },
        { -127 * 4096, 8192, -4096, 65, 0 },
    };
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(cases); n++) {
        for (unsigned clipped = 0; clipped < 2; clipped++) {
            Shape c = pipeline_prepare(s, 0x228, 0);
            int start[4], dx[4], dy[4];
            int side = 256 * (cases[n].anchor < 0) - 128;
            const int vertices[3][2] = {
                {cases[n].anchor, cases[n].anchor},
                {side, 128},
                {side, -256},
            };

            for (unsigned k = 0; k < 4; k++) {
                start[k] = cases[n].start;
                dx[k] = cases[n].dx;
                dy[k] = cases[n].dy;
            }
            pipeline_shade(s, start, dx, dy);
            qtest_writel(s, DE + 0x58, 0xfff8fff8);
            for (unsigned v = 0; v < 3; v++) {
                for (unsigned axis = 0; axis < 2; axis++) {
                    qtest_writel(s, DE + 0x80 + 8 * v + 4 * axis,
                                vertices[v][axis] + 512);
                }
            }
            qtest_writel(s, DE + 0x18,
                        (clipped ? TRIANGLE_DRAW & ~0x78U : TRIANGLE_DRAW) |
                        0x10000);
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            g_assert_cmphex(pixel_read(s, pixel_address(0, 0, 0, 4), 4),
                ==, clipped ? c.old : cases[n].color * 0x01010101U);
        }
    }
    qtest_quit(s);
}

static void test_pipeline_transactions(void)
{
    const unsigned invalid[] = { 0x1000, 0x500, 0xd0, 0xc };
    g_autofree char *dir = g_dir_make_tmp("crime-pipeline-state-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    uint64_t value;

    for (unsigned n = 0; n < 12; n++) {
        Shape c = pipeline_prepare(s, 0x228, 0);
        uint64_t address = DE + 0xe0 + 4 * n;

        qtest_writel(s, DE + 0xd0, 0x12345678);
        for (unsigned width = 1; width <= 8; width *= 2) {
            if (width != 4) {
                if (width != 8 || (n & 1)) {
                    pipeline_failure(s, &c, path, address, UINT64_MAX, width);
                }
                pipeline_failure(s, &c, path, address + 0x800,
                                 UINT64_MAX, width);
            }
        }
        for (unsigned delta = 1; delta < 4; delta++) {
            pipeline_failure(s, &c, path, address + delta, UINT32_MAX, 4);
            pipeline_failure(s, &c, path, address + 0x800 + delta,
                             UINT32_MAX, 4);
        }
        g_assert_cmpint(memtx_read(s, address, &value, 4), !=, MEMTX_OK);
        g_assert_cmpint(memtx_read(s, address + 0x800, &value, 4),
                        !=, MEMTX_OK);
        for (unsigned bad = 0; bad < ARRAY_SIZE(invalid); bad++) {
            qtest_writel(s, DE + 0x1a8, invalid[bad]);
            qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x400);
            pipeline_failure(s, &c, path, address + 0x800, 0xdeadbeef, 4);
            qtest_writel(s, DE + 0x18, (TRIANGLE_DRAW & ~0x78U) | 0x400);
            pipeline_failure(s, &c, path, address + 0x800, 0xdeadbeef, 4);
        }
        /* Smooth lines and texture stay unsupported even with no writes. */
        qtest_writel(s, DE + 0x60, 0x01000020);
        qtest_writel(s, DE + 0x18, 0x10000);
        pipeline_failure(s, &c, path, address + 0x800, 0, 4);
        qtest_writel(s, DE + 0x60, 0x02020000);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x2000);
        pipeline_failure(s, &c, path, address + 0x800, 0, 4);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_pipeline_late_faults(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-pipeline-tiles-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    const int starts[] = { 100 * 4096, 80 * 4096, 60 * 4096, 40 * 4096 };
    const int dx[] = { 100, -200, 300, -400 };
    const int dy[] = { -200, 100, -400, 300 };

    for (unsigned variant = 0; variant < 4; variant++) {
        unsigned alias = variant >> 1;
        Shape c = pipeline_prepare(s, 0x227, 0);
        unsigned left = alias ? 0 : 126, top = alias ? 0 : 126;
        unsigned span = alias ? 132 : 4;
        const unsigned vertex[3][2] = {
            {left * 64, top * 64}, {(left + 2 * span) * 64, top * 64},
            {left * 64, (top + 8) * 64},
        };

        c.x = left;
        c.y = top;
        c.width = span;
        c.height = 4;
        map_region(s, 0, 4, left, top, left + span - 1, top + 3);
        shape_memory(s, &c, NULL);
        for (unsigned v = 0; v < 3; v++) {
            for (unsigned axis = 0; axis < 2; axis++) {
                qtest_writel(s, DE + 0x80 + 8 * v + 4 * axis, vertex[v][axis]);
            }
        }
        pipeline_shade(s, starts, dx, dy);
        qtest_writel(s, DE + 0x18,
                    TRIANGLE_DRAW | 0x10700 | (variant & 1) << 12);
        qtest_writel(s, DE + 0x1a8, 0x45);
        tile(s, 0, 1, alias ? page_for(0, 0) : 0x7fff);
        pipeline_failure(s, &c, path, DE + 0x8ec, starts[3] + 17, 4);
        if (alias) {
            tile(s, 0, 1, page_for(0, 1));
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_pipeline_storage(void)
{
    const unsigned modes[] = {
        0x228, 0x218, 0x124, 0x224, 0x225, 0x226, 0x227,
    };
    QTestState *s = start_indexed_state();

    for (unsigned m = 0; m < ARRAY_SIZE(modes); m++) {
        for (unsigned bank = 0; bank < 3; bank++) {
            for (unsigned direction = 0; direction < 4; direction++) {
                Shape c = pipeline_prepare(s, modes[m], bank);
                uint32_t expected[16];
                const unsigned vertices[2][3][2] = {
                    { {126, 126}, {130, 126}, {126, 130} },
                    { {130, 130}, {126, 130}, {130, 126} },
                };
                unsigned draw = TRIANGLE_DRAW | 0x700;

                c.x = c.y = 126;
                map_region(s, bank, c.bytes, 126, 126, 129, 129);
                shape_memory(s, &c, NULL);
                qtest_writel(s, DE + 0x18, draw);
                qtest_writel(s, DE + 0x1a8, 0x45);
                qtest_writel(s, DE + 0x1b0, 6);
                qtest_writel(s, DE + 0xd0, 0x537b9146);
                qtest_writel(s, DE + 0x60, 0x02000000 | direction << 16);
                for (unsigned triangle = 0; triangle < 2; triangle++) {
                    for (unsigned v = 0; v < 3; v++) {
                        unsigned index = direction & 1 && v ? 3 - v : v;

                        for (unsigned axis = 0; axis < 2; axis++) {
                            qtest_writel(s, DE + 0x80 + 8 * v + 4 * axis,
                                        vertices[triangle][index][axis] * 64);
                        }
                    }
                    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0),
                                    ==, MEMTX_OK);
                }
                for (unsigned y = 0; y < 4; y++) {
                    for (unsigned x = 0; x < 4; x++) {
                        expected[y * 4 + x] = pipeline_reference(modes[m],
                            0x537b9146, c.old, 0, 0x45, draw, 6,
                            UINT32_MAX, x + 126, y + 126);
                    }
                }
                shape_memory(s, &c, expected);
            }
        }
    }
    qtest_quit(s);
}

static void test_pipeline_phase(void)
{
    const int origins[] = { 0, 1, -1, -4096 };
    QTestState *s = start_indexed_state();

    for (unsigned half = 0; half < 2; half++) {
        for (unsigned origin = 0; origin < ARRAY_SIZE(origins); origin++) {
            for (unsigned clip = 0; clip < 2; clip++) {
                unsigned mode = 0x226 | half;
                Shape c = pipeline_prepare(s, mode, 0);
                uint32_t expected[16];
                int o = origins[origin];
                const unsigned vertex[3][2] = { {4, 4}, {12, 4}, {4, 12} };
                unsigned draw = TRIANGLE_DRAW | 0x100 | clip << 20;

                c.x = c.y = 4;
                shape_memory(s, &c, NULL);
                qtest_writel(s, DE + 0x58, xy((uint16_t)o, (uint16_t)o));
                for (unsigned v = 0; v < 3; v++) {
                    for (unsigned axis = 0; axis < 2; axis++) {
                        qtest_writel(s, DE + 0x80 + 8 * v + 4 * axis,
                                    (vertex[v][axis] - o) * 64);
                    }
                }
                qtest_writel(s, DE + 0x18, draw);
                qtest_writeq(s, DE + 0x48, bounds(6 - o, 6 - o, 8 - o, 8 - o));
                qtest_writel(s, DE + 0x10, 0x21);
                qtest_writeq(s, DE + 0x20, bounds(4, 4, 8, 8));
                qtest_writel(s, DE + 0xd0, 0xe9e9e9ab);
                for (unsigned y = 0; y < 4; y++) {
                    for (unsigned x = 0; x < 4; x++) {
                        expected[y * 4 + x] = clip && (x < 2 || y < 2) ?
                            c.old : pipeline_reference(mode, 0xe9e9e9ab,
                                c.old, 0, 0, draw, 3, UINT32_MAX, x + 4, y + 4);
                    }
                }
                g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
                shape_memory(s, &c, expected);
            }
        }
    }
    qtest_quit(s);
}

static void test_pipeline_populated_banks(void)
{
    QTestState *s = start_profile_ram("legacy-software", true, true, false, "");
    Shape c = pipeline_prepare(s, 0x227, 0);
    uint32_t draw = TRIANGLE_DRAW | 0x500;
    const unsigned vertex[3][2] = { {126, 0}, {134, 0}, {126, 8} };

    for (unsigned i = 0; i < 8; i++) {
        qtest_writeq(s, CRIME + 0x208 + 8 * i, 0x100 | i * 4);
    }
    tile(s, 0, 0, 0x7ff);
    tile(s, 0, 1, 0x800);
    qtest_writel(s, DE + 0x18, draw);
    qtest_writel(s, DE + 0x1a8, 0x45);
    qtest_writel(s, DE + 0xd0, 0x1153c940);
    for (unsigned v = 0; v < 3; v++) {
        qtest_writel(s, DE + 0x80 + 8 * v, vertex[v][0] * 64);
        qtest_writel(s, DE + 0x84 + 8 * v, vertex[v][1] * 64);
    }
    for (unsigned x = 126; x < 130; x++) {
        uint64_t address = x < 128 ? 0x7ff0000ULL + 4 * x :
                                    0x8000000ULL + 4 * (x - 128);

        pixel_write(s, address, 4, c.old);
    }
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    for (unsigned x = 126; x < 130; x++) {
        uint64_t address = x < 128 ? 0x7ff0000ULL + 4 * x :
                                    0x8000000ULL + 4 * (x - 128);

        g_assert_cmphex(pixel_read(s, address, 4), ==,
            pipeline_reference(0x227, 0x1153c940, c.old, 0, 0x45,
                               draw, 3, UINT32_MAX, x, 0));
    }
    qtest_quit(s);
}

static void test_pipeline_migration(void)
{
    g_autofree char *dir =
        g_dir_make_tmp("crime-pipeline-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *from = start(), *to = start_options("-incoming defer");
    Shape c = pipeline_prepare(from, 0x227, 0);
    const int starts[] = { 100 * 4096, 80 * 4096, 60 * 4096, 40 * 4096 };
    const int dx[] = { 100, -200, 300, -400 };
    const int dy[] = { -200, 100, -400, 300 };
    unsigned draw = TRIANGLE_DRAW | 0x10700;
    uint32_t expected[16];
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];

    pipeline_shade(from, starts, dx, dy);
    qtest_writel(from, DE + 0x18, draw);
    qtest_writel(from, DE + 0x1a8, 0x45);
    qtest_writel(from, DE + 0x1b0, 6);
    /* Upper raw container bits survive GO and migration, never arithmetic. */
    g_assert_cmpint(memtx_writel(from, DE + 0x8ec,
                                0xc0000000U | starts[3]), ==, MEMTX_OK);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            uint32_t color = pipeline_plane_color(starts, dx, dy, 0, 0, x, y);

            expected[4 * y + x] = pipeline_reference(0x227, color, c.old,
                0, 0x45, draw, 6, UINT32_MAX, x, y);
        }
    }
    shape_memory(from, &c, expected);
    save_paired_renderer(from, path, before);
    g_assert_cmphex(before[192 + 0xec / 4], ==, 0xc0000000U | starts[3]);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    save_paired_renderer(to, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    shape_memory(to, &c, expected);
    qtest_system_reset(to);
    shape_memory(to, &c, expected);
    save_renderer(to, path, after);
    for (unsigned n = 0; n < 12; n++) {
        g_assert_cmphex(after[192 + (0xe0 + 4 * n) / 4], ==, 0);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_unlink(path);
    g_rmdir(dir);
}

static void pipeline_normalize_draw_ifb(IfbSnapshot *state)
{
    for (unsigned i = 0; i < 64; i++) {
        if (ldq_be_p(state->ram + 512 + 8 * i) == 0x120300000000ULL) {
            uint64_t value = ldq_be_p(state->ram + 8 * i);

            stq_be_p(state->ram + 8 * i, value & ~(0x1000ULL << 32));
        }
    }
}

static void test_pipeline_inactive_line_control(gconstpointer selection)
{
    const unsigned modes[] = {
        0x228, 0x218, 0x124, 0x224, 0x225, 0x226, 0x227,
    };
    const int starts[] = { 100 * 4096, 80 * 4096, 60 * 4096, 40 * 4096 };
    const int dx[] = { 100, -200, 300, -400 };
    const int dy[] = { -200, 100, -400, 300 };
    unsigned m = GPOINTER_TO_UINT(selection);
    g_autofree char *dir = g_dir_make_tmp("crime-inactive-line-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s[2] = { start_indexed_state(), start_indexed_state() };

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned direction = 0; direction < 4; direction++) {
            for (unsigned smooth = 0; smooth < 2; smooth++) {
                for (unsigned clipping = 0; clipping < 4; clipping++) {
                    Shape c[2];
                    uint32_t expected[16];
                    uint64_t raw[2][RENDER_WORDS];
                    IfbSnapshot journal[2] = { 0 };
                    const int anchors[4][2] = {
                        {0, 8}, {8, 0}, {0, 0}, {0, 0},
                    };
                    int loaded[4];
                    unsigned draw = TRIANGLE_DRAW | 0xa0080 | smooth << 16 |
                                    (direction & 1 ? 0x700 : 0x200);

                    if (clipping == 1) {
                        draw |= 0x100000;
                    } else if (clipping == 3) {
                        draw &= ~0x78U;
                    }
                    for (unsigned k = 0; k < 4; k++) {
                        loaded[k] = starts[k];
                        if (clipping != 2) {
                            loaded[k] += dx[k] * anchors[direction][0] +
                                         dy[k] * anchors[direction][1];
                        }
                    }
                    for (unsigned aa = 0; aa < 2; aa++) {
                        c[aa] = pipeline_prepare(s[aa], modes[m], bank);
                        pipeline_shade(s[aa], loaded, dx, dy);
                        qtest_writel(s[aa], DE + 0x190, 0xa579b3e1);
                        qtest_writel(s[aa], DE + 0x194, 0x6c28f04d);
                        qtest_writel(s[aa], DE + 0xc0, UINT32_MAX);
                        qtest_writel(s[aa], DE + 0xc4, 0x3971bd65);
                        qtest_writel(s[aa], DE + 0x18, draw | aa << 12);
                        qtest_writel(s[aa], DE + 0xd0, 0x1153c940);
                        qtest_writel(s[aa], DE + 0x1a8, 0x45);
                        qtest_writel(s[aa], DE + 0x1b0, 6);
                        qtest_writel(s[aa], DE + 0x1b8, 0xa5379b6d);
                        qtest_writel(s[aa], DE + 0x58, 0xf000f000);
                        qtest_writel(s[aa], DE + 0x60,
                                    0x02000000 | direction << 16);
                        qtest_writeq(s[aa], DE + 0x48,
                                     bounds(4098, 4098, 4100, 4100));
                        for (unsigned v = 0; v < 3; v++) {
                            unsigned index = direction & 1 && v ? 3 - v : v;
                            unsigned x = clipping != 2 && index == 1 ? 512 : 0;
                            unsigned y = clipping != 2 && index == 2 ? 512 : 0;

                            qtest_writel(s[aa], DE + 0x80 + 8 * v, 262144 + x);
                            qtest_writel(s[aa], DE + 0x84 + 8 * v, 262144 + y);
                        }
                    }
                    for (unsigned i = 0; i < 16; i++) {
                        expected[i] = c[0].old;
                    }
                    for (unsigned word = 0; word < 12; word++) {
                        unsigned offset = 0xe0 + 4 * word;
                        int value = word < 4 ? loaded[word] : 0;

                        for (unsigned k = 0; k < 4; k++) {
                            if (offset == shade_dx[k]) {
                                value = dx[k];
                            } else if (offset == shade_dy[k]) {
                                value = dy[k];
                            }
                        }
                        for (unsigned y = 0; y < 4; y++) {
                            for (unsigned x = 0; x < 4; x++) {
                                uint32_t color = smooth ? pipeline_plane_color(
                                    starts, dx, dy, 0, 0, x, y) : 0x1153c940;
                                bool visible = clipping != 2 &&
                                    (clipping != 1 || (x >= 2 && y >= 2));

                                if (visible) {
                                    expected[4 * y + x] = pipeline_reference(
                                        modes[m], color, expected[4 * y + x],
                                        0, 0x45, draw, 6, 0xa5379b6d, x, y);
                                }
                            }
                        }
                        for (unsigned aa = 0; aa < 2; aa++) {
                            g_assert_cmpint(memtx_writel(s[aa],
                                DE + 0x800 + offset, value), ==, MEMTX_OK);
                            shape_memory(s[aa], &c[aa], expected);
                        }
                    }
                    for (unsigned aa = 0; aa < 2; aa++) {
                        ifb_snapshot(s[aa], &journal[aa]);
                        pipeline_normalize_draw_ifb(&journal[aa]);
                        save_paired_renderer(s[aa], path, raw[aa]);
                        g_assert_cmphex(raw[aa][192 + 0x18 / 4],
                                        ==, draw | aa << 12);
                    }
                    raw[1][192 + 0x18 / 4] &= ~0x1000ULL;
                    g_assert_cmpmem(raw[0], sizeof(raw[0]),
                                    raw[1], sizeof(raw[1]));
                    g_assert_cmpmem(&journal[0], sizeof(journal[0]),
                                    &journal[1], sizeof(journal[1]));
                }
            }
        }
    }
    qtest_quit(s[0]);
    qtest_quit(s[1]);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_pipeline_line_control_refusals(void)
{
    const unsigned unsupported[] = { 0x2000, 0x800, 0x4000, 0x8000 };
    g_autofree char *dir =
        g_dir_make_tmp("crime-line-control-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned aa = 0; aa < 2; aa++) {
        for (unsigned bad = 0; bad < ARRAY_SIZE(unsupported); bad++) {
            Shape c = pipeline_prepare(s, 0x227, 0);

            qtest_writel(s, DE + 0x18,
                        TRIANGLE_DRAW | aa << 12 | unsupported[bad]);
            pipeline_failure(s, &c, path, DE + 0x8ec, 0x7351b9df, 4);
        }
    }
    {
        Shape c = pipeline_prepare(s, 0x228, 0);

        qtest_writel(s, DE + 0x60, 0x01000020);
        qtest_writel(s, DE + 0x70, xy(0, 0));
        qtest_writel(s, DE + 0x74, xy(3, 0));
        qtest_writel(s, DE + 0x18, 0x10f8);
        pipeline_failure(s, &c, path, DE + 0x8ec, 0x7351b9df, 4);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_pipeline_anchor_literals(void)
{
    const int start[] = { 10 * 4096, 20 * 4096, 30 * 4096, 40 * 4096 };
    const int dx[] = { 4096, -8192, 12288, -4096 };
    const int dy[] = { 8192, 4096, -4096, -8192 };
    const int vertex[3][2] = { {32, 16}, {352, 16}, {288, 304} };
    const int origins[] = { 0, -4096, -4097 };
    const uint32_t colors[2][2] = {
        { 0x02151b30, 0x091b1429 }, { 0x0e112324, 0x0e112324 },
    };
    QTestState *s = start_indexed_state();

    for (unsigned direction = 0; direction < 4; direction++) {
        for (unsigned winding = 0; winding < 2; winding++) {
            for (unsigned origin = 0; origin < ARRAY_SIZE(origins); origin++) {
                Shape c = pipeline_prepare(s, 0x228, 0);
                int o = origins[origin];

                pipeline_shade(s, start, dx, dy);
                qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x10000);
                qtest_writel(s, DE + 0x60, 0x02000000 | direction << 16);
                qtest_writel(s, DE + 0x58, xy((uint16_t)o, (uint16_t)o));
                for (unsigned v = 0; v < 3; v++) {
                    unsigned index = winding && v ? 3 - v : v;

                    for (unsigned axis = 0; axis < 2; axis++) {
                        qtest_writel(s, DE + 0x80 + 8 * v + 4 * axis,
                                    vertex[index][axis] - 64 * o);
                    }
                }
                g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
                /* Same loaded starts distinguish traversal-selected anchors. */
                g_assert_cmphex(pixel_read(s, pixel_address(0, 2, 1, 4), 4),
                                ==, colors[direction >> 1][winding]);
                g_assert_cmphex(pixel_read(s, pixel_address(0, 0, 0, 4), 4),
                                ==, c.old);
            }
        }
    }
    qtest_quit(s);
}

static void test_pipeline_anchor_floors(void)
{
    const int coordinates[] = { -1, 0, 1, 63, 64, 65 };
    const unsigned values[] = { 3, 2, 2, 2, 1, 1 };
    g_autofree char *dir = g_dir_make_tmp("crime-anchor-floor-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned axis = 0; axis < 2; axis++) {
        for (unsigned n = 0; n < ARRAY_SIZE(coordinates); n++) {
            for (unsigned direction = 0; direction < 4; direction++) {
                for (unsigned winding = 0; winding < 2; winding++) {
                    int vertices[3][2] = {
                        {coordinates[n], -64}, {-128, 256}, {256, 256},
                    };
                    const unsigned orders[2][2][3] = {
                        { {1, 2, 0}, {2, 1, 0} },
                        { {0, 1, 2}, {0, 2, 1} },
                    };
                    const int start[4] = { 4096, 4096, 4096, 4096 };
                    int dx[4], dy[4];
                    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];

                    pipeline_prepare(s, 0x228, 0);
                    for (unsigned k = 0; k < 4; k++) {
                        dx[k] = axis ? 0 : 4096;
                        dy[k] = axis ? 4096 : 0;
                    }
                    pipeline_shade(s, start, dx, dy);
                    qtest_writel(s, DE + 0x58, 0xfffcfffc);
                    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x10000);
                    qtest_writel(s, DE + 0x60, 0x02000000 | direction << 16);
                    for (unsigned v = 0; v < 3; v++) {
                        unsigned index = orders[direction >> 1][winding][v];

                        for (unsigned a = 0; a < 2; a++) {
                            qtest_writel(s, DE + 0x80 + 8 * v + 4 * a,
                                        vertices[index][axis ^ a] + 256);
                        }
                    }
                    save_paired_renderer(s, path, before);
                    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0),
                                    ==, MEMTX_OK);
                    g_assert_cmphex(pixel_read(s, pixel_address(0, 1, 1, 4), 4),
                                    ==, values[n] * 0x01010101U);
                    save_paired_renderer(s, path, after);
                    /* Loaded starts must not receive a second adjustment. */
                    g_assert_cmpmem(before, sizeof(before),
                                    after, sizeof(after));
                }
            }
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_pipeline_anchor_invariants(void)
{
    const int start[] = { 10 * 4096, 20 * 4096, 30 * 4096, 40 * 4096 };
    const int dx[] = { 4096, -8192, 12288, -4096 };
    const int dy[] = { 8192, 4096, -4096, -8192 };
    const int vertex[3][2] = { {32, 16}, {352, 16}, {288, 304} };
    const int origins[] = { 0, -4096 };
    const unsigned modes[] = { 0x228, 0x227 };
    const uint32_t colors[2][2] = {
        { 0x02151b30, 0x091b1429 }, { 0x0e112324, 0x0e112324 },
    };
    g_autofree char *dir = g_dir_make_tmp("crime-anchor-state-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned mode = 0; mode < ARRAY_SIZE(modes); mode++) {
        for (unsigned shift = 0; shift < 2; shift++) {
            for (unsigned origin = 0; origin < ARRAY_SIZE(origins); origin++) {
                for (unsigned direction = 0; direction < 4; direction++) {
                    for (unsigned winding = 0; winding < 2; winding++) {
                        Shape c = pipeline_prepare(s, modes[mode], 0);
                        int o = origins[origin];
                        unsigned draw = TRIANGLE_DRAW | 0x11700;
                        uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
                        unsigned x = 2 + shift, y = 1 + shift;

                        pipeline_shade(s, start, dx, dy);
                        qtest_writel(s, DE + 0x60,
                                    0x02000000 | direction << 16);
                        qtest_writel(s, DE + 0x58,
                                    xy((uint16_t)o, (uint16_t)o));
                        qtest_writel(s, DE + 0x1a8, 0x45);
                        qtest_writel(s, DE + 0x1b0, 6);
                        for (unsigned v = 0; v < 3; v++) {
                            unsigned index = winding && v ? 3 - v : v;

                            for (unsigned a = 0; a < 2; a++) {
                                qtest_writel(s, DE + 0x80 + 8 * v + 4 * a,
                                    vertex[index][a] + 64 * ((int)shift - o));
                            }
                        }
                        for (unsigned clip = 0; clip < 2; clip++) {
                            uint32_t expected = pipeline_reference(modes[mode],
                                colors[direction >> 1][winding], c.old, 0,
                                0x45, draw, 6, UINT32_MAX, x, y);

                            shape_memory(s, &c, NULL);
                            qtest_writel(s, DE + 0x18, draw | clip << 20);
                            qtest_writeq(s, DE + 0x48,
                                         bounds(x - o, y - o,
                                                4 + shift - o, 4 + shift - o));
                            save_paired_renderer(s, path, before);
                            /* Inactive foreground START still commits once. */
                            g_assert_cmpint(memtx_writel(s, DE + 0x8d0,
                                                        0xf13579bd),
                                            ==, MEMTX_OK);
                            g_assert_cmphex(pixel_read(s,
                                pixel_address(0, x, y, c.bytes), c.bytes),
                                ==, expected);
                            if (clip) {
                                g_assert_cmphex(pixel_read(s,
                                    pixel_address(0, x - 1, y, c.bytes),
                                    c.bytes),
                                    ==, c.old);
                            }
                            save_paired_renderer(s, path, after);
                            before[192 + 0xd0 / 4] = 0xf13579bd;
                            g_assert_cmpmem(before, sizeof(before),
                                            after, sizeof(after));
                        }
                    }
                }
            }
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_sz_depth_fixture(void)
{
    QTestState *s = start_indexed_state();
    Shape c = pipeline_prepare(s, 0x228, 0);
    uint32_t expected[16];

    map_region(s, 2, 4, 0, 0, 3, 3);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            pixel_write(s, pixel_address(2, x, y, 4), 4, 0x6b000011);
            expected[4 * y + x] = c.old;
        }
    }
    qtest_writel(s, DE + 0xd0, 0x13579bdf);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 6);
    qtest_writel(s, DE + 0x1c0, 1U << 25);
    qtest_writeq(s, DE + 0x1c8, 18 * 4096);
    qtest_writeq(s, DE + 0x1d0, 0);
    qtest_writeq(s, DE + 0x1d8, 0);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    shape_memory(s, &c, expected);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                            ==, 0x6b000011);
            expected[4 * y + x] = 0x13579bdf;
        }
    }
    g_assert_cmpint(memtx_writeq(s, DE + 0x9c8, 16 * 4096), ==, MEMTX_OK);
    shape_memory(s, &c, expected);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                            ==, 0x6b000010);
        }
    }
    qtest_quit(s);
}

typedef struct SZCase {
    unsigned draw, control, stencil, mask, bank, mode;
    int64_t start, dx, dy;
    uint32_t old, color, plane;
    bool rectangle;
} SZCase;

static SZCase sz_case(void)
{
    return (SZCase) {
        .draw = TRIANGLE_DRAW | 6, .control = 7U << 25,
        .stencil = 0x62ff7000, .mask = 255, .mode = 0x228,
        .start = 18 * 4096, .old = 0x53000011,
        .color = 0x1153c940, .plane = UINT32_MAX,
    };
}

static bool sz_reference_compare(unsigned function, uint32_t a, uint32_t b)
{
    const bool result[8] = {
        false, a < b, a == b, a <= b, a > b, a != b, a >= b, true,
    };

    return result[function];
}

static uint32_t sz_reference_depth(int64_t z)
{
    if (z <= 0) {
        return 0;
    }
    return MIN(z / 4096, 0xffffff);
}

static uint32_t sz_reference(const SZCase *v, uint32_t old, uint32_t z,
                               bool *color)
{
    unsigned stencil = old >> 24, reference = v->stencil >> 24;
    unsigned cmpmask = (v->stencil >> 16) & 255;
    bool depth_enabled = v->draw & 4, stencil_enabled = v->draw & 1;
    bool write = depth_enabled && (v->draw & 2);
    bool sp, zp;
    unsigned outcome, op, next;
    const unsigned candidates[6] = {
        stencil, 0, reference, stencil == 255 ? 255 : stencil + 1,
        stencil == 0 ? 0 : stencil - 1, 255 - stencil,
    };

    if (v->control & 0x1000000) {
        bool select = ((v->stencil >> 12) & 7) == 7 || stencil != reference;
        uint32_t effective = select ? v->control & 0xffffff : old & 0xffffff;
        bool replace = (v->stencil & 0xf0) == 0x20;
        bool pass = sz_reference_compare((v->control >> 25) & 7, z, effective);

        *color = pass && (!replace || select);
        if (!write || (!select && replace) || (!pass && !replace)) {
            return old;
        }
        return reference << 24 | (pass ? z : effective);
    }
    sp = !stencil_enabled || sz_reference_compare(
        (v->stencil >> 12) & 7, reference & cmpmask, stencil & cmpmask);
    zp = !depth_enabled || sz_reference_compare(
        (v->control >> 25) & 7, z, old & 0xffffff);
    *color = sp && zp;
    outcome = !sp ? 8 : !zp ? 4 : 0;
    op = (v->stencil >> outcome) & 15;
    next = stencil_enabled ? candidates[op] : stencil;
    next = (stencil & ~(stencil_enabled ? v->mask : 0)) |
           (next & (stencil_enabled ? v->mask : 0));
    return next << 24 | (*color && write ? z : old & 0xffffff);
}

static void sz_program(QTestState *s, const SZCase *v)
{
    qtest_writel(s, DE + 0x18, v->draw);
    qtest_writel(s, DE + 0xd0, v->color);
    qtest_writel(s, DE + 0x1c0, v->control);
    qtest_writel(s, DE + 0x1e0, v->stencil);
    qtest_writel(s, DE + 0x1e8, v->mask);
    qtest_writeq(s, DE + 0x1c8, v->start);
    qtest_writeq(s, DE + 0x1d0, v->dx);
    qtest_writeq(s, DE + 0x1d8, v->dy);
    qtest_writel(s, DE + 0x1b8, v->plane);
    qtest_writel(s, DE + 0x1b0, 6);
    qtest_writel(s, DE + 0x1a8, 0x45);
}

static void sz_run(QTestState *s, const SZCase *v)
{
    Shape c = pipeline_prepare(s, v->mode, v->bank);
    uint32_t expected[16], zs[16];

    map_region(s, 2, 4, 0, 0, 3, 3);
    sz_program(s, v);
    if (v->rectangle) {
        qtest_writel(s, DE + 0x60, 0x03020000);
        qtest_writel(s, DE + 0x88, 3 * 64);
        qtest_writel(s, DE + 0x8c, 3 * 64);
    }
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            bool color;
            uint32_t z = sz_reference_depth(v->start + v->dx * x + v->dy * y);

            pixel_write(s, pixel_address(2, x, y, 4), 4, v->old);
            zs[4 * y + x] = sz_reference(v, v->old, z, &color);
            expected[4 * y + x] = color ? pipeline_reference(v->mode,
                v->color, c.old, 0, 0x45, v->draw, 6, v->plane, x, y) : c.old;
        }
    }
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    shape_memory(s, &c, expected);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                            ==, zs[4 * y + x]);
        }
    }
}

static void test_sz_comparisons(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned function = 0; function < 8; function++) {
        for (unsigned value = 16; value <= 18; value++) {
            for (unsigned write = 0; write < 2; write++) {
                SZCase v = sz_case();

                v.draw = TRIANGLE_DRAW | 4 | write << 1;
                v.control = function << 25;
                v.start = value * 4096;
                v.bank = value & 1;
                sz_run(s, &v);
            }
        }
    }
    qtest_quit(s);
}

static void test_sz_stencil(void)
{
    const unsigned masks[] = { 0, 0x0f, 0xf0, 255 };
    const unsigned values[] = { 0, 1, 0x53, 254, 255 };
    QTestState *s = start_indexed_state();

    for (unsigned function = 0; function < 8; function++) {
        for (unsigned mask = 0; mask < ARRAY_SIZE(masks); mask++) {
            for (unsigned old = 0; old < ARRAY_SIZE(values); old++) {
                for (unsigned ref = 0; ref < ARRAY_SIZE(values); ref++) {
                    SZCase v = sz_case();

                    v.draw |= 1;
                    v.stencil = values[ref] << 24 | masks[mask] << 16 |
                                function << 12 | 0x532;
                    v.old = values[old] << 24 | 17;
                    v.mask = 0x5a;
                    v.bank = ref & 1;
                    sz_run(s, &v);
                }
            }
        }
    }
    for (unsigned op = 0; op < 6; op++) {
        for (unsigned outcome = 0; outcome < 3; outcome++) {
            for (unsigned old = 0; old < ARRAY_SIZE(values); old++) {
                for (unsigned mask = 0; mask < ARRAY_SIZE(masks); mask++) {
                    SZCase v = sz_case();

                    v.draw |= 1;
                    v.old = values[old] << 24 | 17;
                    v.mask = masks[mask];
                    v.control = outcome == 1 ? 0 : 7U << 25;
                    v.stencil = 0x62ff0000 | (outcome ? 7 : 0) << 12 |
                                op << 8 | op << 4 | op;
                    sz_run(s, &v);
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_sz_tags(gconstpointer selection)
{
    unsigned comparison = GPOINTER_TO_UINT(selection);
    QTestState *s = start_indexed_state();

    for (unsigned function = 0; function < 8; function++) {
        for (unsigned row = 0; row < 2; row++) {
            for (unsigned write = 0; write < 2; write++) {
                for (unsigned match = 0; match < 2; match++) {
                    for (unsigned incoming = 0; incoming < 3; incoming++) {
                        for (unsigned ref = 0; ref < 4; ref++) {
                            const unsigned references[] = { 0, 1, 254, 255 };
                            SZCase v = sz_case();
                            unsigned r = references[ref];

                            v.draw = TRIANGLE_DRAW | 5 | write << 1;
                            v.control = function << 25 | 0x1000000 | 0x654321;
                            v.stencil = r << 24 | 0xff0000 | comparison << 12 |
                                        row << 5 | 2;
                            v.mask = write ? 255 : 0;
                            v.old = (match ? r : r ^ 255) << 24 | 0x123456;
                            v.start = (incoming == 0 ? 0x100000LL :
                                       incoming == 1 ? 0x654321LL :
                                                       0x700000LL) * 4096;
                            sz_run(s, &v);
                        }
                    }
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_sz_all_references(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned reference = 0; reference <= 256; reference++) {
        unsigned r = reference & 255;

        for (unsigned path = 0; path < 3; path++) {
            for (unsigned match = 0; match < 2; match++) {
                SZCase v = sz_case();

                v.draw = (TRIANGLE_DRAW & ~0x78U) | 7;
                v.rectangle = path != 0;
                v.control = 0x0f654321;
                v.stencil = r << 24 | 0xff0000 |
                            (path == 2 ? 7 : 5) << 12 | (path == 1 ? 0x22 : 2);
                v.old = (match ? r : r ^ 255) << 24 | 0x123456;
                v.start = 0x654321LL * 4096;
                sz_run(s, &v);
                /* Clearing tag mode cannot perform hidden conversion. */
                qtest_writel(s, DE + 0x1c0, v.control & ~0x1000000U);
                for (unsigned y = 0; y < 4; y++) {
                    for (unsigned x = 0; x < 4; x++) {
                        bool color;

                        g_assert_cmphex(pixel_read(s,
                            pixel_address(2, x, y, 4), 4), ==,
                            sz_reference(&v, v.old, 0x654321, &color));
                    }
                }
            }
        }
    }
    qtest_quit(s);
}

static void sz_window_bytes(QTestState *s, bool updated)
{
    const unsigned indices[] = { 0, 1, 16, 17 };
    g_autofree uint8_t *data = g_malloc(65536);

    for (unsigned n = 0; n < ARRAY_SIZE(indices); n++) {
        qtest_memread(s, (uint64_t)page_for(0, indices[n]) << 16, data, 65536);
        for (unsigned i = 0; i < 65536; i++) {
            g_assert_cmphex(data[i], ==, 0x5a);
        }
        qtest_memread(s, (uint64_t)page_for(2, indices[n]) << 16, data, 65536);
        for (unsigned i = 0; i < 65536; i += 4) {
            g_assert_cmphex(ldl_be_p(data + i), ==,
                            updated ? 0x53123456 : 0x53535353);
        }
    }
}

static void test_sz_window(void)
{
    const unsigned indices[] = { 0, 1, 16, 17 };
    g_autofree char *dir = g_dir_make_tmp("crime-sz-window-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    SZCase v = sz_case();
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];

    pipeline_prepare(s, 0x228, 0);
    map_region(s, 2, 4, 0, 0, 255, 255);
    /* Color remains semantically valid but has no actual RAM accesses. */
    qtest_writeq(s, RE + 0x1000, 0);
    v.draw = (TRIANGLE_DRAW & ~0x78U) | 0x100006;
    v.start = 0x123456LL * 4096;
    sz_program(s, &v);
    qtest_writeq(s, DE + 0x48, bounds(0, 0, 256, 256));
    qtest_writel(s, DE + 0x88, 512 * 64);
    qtest_writel(s, DE + 0x94, 512 * 64);
    for (unsigned fault = 0; fault < 2; fault++) {
        for (unsigned n = 0; n < ARRAY_SIZE(indices); n++) {
            qtest_memset(s, (uint64_t)page_for(0, indices[n]) << 16,
                         0x5a, 65536);
            qtest_memset(s, (uint64_t)page_for(2, indices[n]) << 16,
                         0x53, 65536);
        }
        tile(s, 2, 17, fault ? 0x7fff : page_for(2, 17));
        save_paired_renderer(s, path, before);
        g_assert_cmpint(memtx_writeq(s, DE + 0x9c8, v.start),
                        ==, fault ? MEMTX_ERROR : MEMTX_OK);
        sz_window_bytes(s, !fault);
        save_paired_renderer(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_sz_gl_rectangles(void)
{
    const int origins[] = { 0, 1, -1, -4096 };
    QTestState *s = start_indexed_state();

    for (unsigned direction = 0; direction < 4; direction++) {
        for (unsigned origin = 0; origin < ARRAY_SIZE(origins); origin++) {
            Shape c = pipeline_prepare(s, 0x228, 0);
            SZCase v = sz_case();
            int o = origins[origin];
            unsigned xs = direction & 1 ? 3 : 1;
            unsigned xe = direction & 1 ? 1 : 3;
            unsigned ys = direction & 2 ? 1 : 2;
            unsigned ye = direction & 2 ? 2 : 1;

            map_region(s, 2, 4, 0, 0, 7, 7);
            for (unsigned y = 0; y < 8; y++) {
                for (unsigned x = 0; x < 8; x++) {
                    pixel_write(s, pixel_address(2, x, y, 4), 4, 0x6b000011);
                }
            }
            v.draw = 0x400006;
            v.start = 16 * 4096;
            sz_program(s, &v);
            qtest_writel(s, DE + 0x60, 0x03000000 | direction << 16);
            qtest_writel(s, DE + 0x58, xy((uint16_t)o, (uint16_t)o));
            qtest_writel(s, DE + 0x80, (xs - o) * 64);
            qtest_writel(s, DE + 0x84, (ys - o) * 64);
            qtest_writel(s, DE + 0x88, (xe - o) * 64);
            qtest_writel(s, DE + 0x8c, (ye - o) * 64);
            qtest_writeq(s, DE + 0x90, 0x7fc00000deadbeefULL);
            qtest_writeq(s, DE + 0x70, UINT64_MAX);
            qtest_writel(s, DE, UINT32_MAX);
            qtest_writel(s, DE + 0x50, UINT32_MAX);
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            for (unsigned y = 0; y < 8; y++) {
                for (unsigned x = 0; x < 8; x++) {
                    g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                        ==, x >= 1 && x <= 3 && y >= 1 && y <= 2 ?
                            0x6b000010 : 0x6b000011);
                }
            }
            g_assert_cmphex(pixel_read(s, pixel_address(0, 1, 1, 4), 4),
                            ==, c.old);
        }
    }
    qtest_quit(s);
}

static void sz_initialize(QTestState *s, const Shape *c, uint32_t value)
{
    map_region(s, 2, 4, c->x, c->y, c->x + c->width - 1, c->y + c->height - 1);
    for (unsigned y = 0; y < c->height; y++) {
        for (unsigned x = 0; x < c->width; x++) {
            pixel_write(s, pixel_address(2, c->x + x, c->y + y, 4), 4, value);
        }
    }
}

static void sz_atomic_failure(QTestState *s, const Shape *c, const char *path,
                                uint64_t address, uint64_t value,
                                unsigned width)
{
    unsigned count = c->width * c->height;
    g_autofree uint32_t *colors = g_new(uint32_t, count);
    g_autofree uint32_t *depths = g_new(uint32_t, count);
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    IfbSnapshot a, b;

    for (unsigned y = 0; y < c->height; y++) {
        for (unsigned x = 0; x < c->width; x++) {
            colors[y * c->width + x] = pixel_read(s,
                pixel_address(c->bank, c->x + x, c->y + y, c->bytes), c->bytes);
            depths[y * c->width + x] = pixel_read(s,
                pixel_address(2, c->x + x, c->y + y, 4), 4);
        }
    }
    save_paired_renderer(s, path, before);
    ifb_snapshot(s, &a);
    g_assert_cmpint(memtx_write(s, address, value, width), !=, MEMTX_OK);
    ifb_snapshot(s, &b);
    g_assert_cmpmem(&a, sizeof(a), &b, sizeof(b));
    for (unsigned y = 0; y < c->height; y++) {
        for (unsigned x = 0; x < c->width; x++) {
            g_assert_cmphex(pixel_read(s,
                pixel_address(c->bank, c->x + x, c->y + y, c->bytes), c->bytes),
                ==, colors[y * c->width + x]);
            g_assert_cmphex(pixel_read(s,
                pixel_address(2, c->x + x, c->y + y, 4), 4),
                ==, depths[y * c->width + x]);
        }
    }
    save_paired_renderer(s, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
}

static void test_sz_transport(void)
{
    const int64_t limit = INT64_C(1) << 36;
    const int64_t values[] = {
        -limit, -limit + 1, -4097, -4096, -1, 0, 1, 4095, 4096, 4097,
        0xffffffLL * 4096 - 1, 0xffffffLL * 4096,
        0xffffffLL * 4096 + 4095, limit - 4096, limit - 64, limit - 1, limit,
    };
    const uint64_t invalid[] = {
        (uint64_t)(-limit - 1), (uint64_t)(limit + 1),
        0x8000000000000000ULL, 0x1000001000000000ULL,
    };
    g_autofree char *dir = g_dir_make_tmp("crime-sz-transport-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    uint64_t raw[RENDER_WORDS];

    for (unsigned n = 0; n < ARRAY_SIZE(values); n++) {
        SZCase v = sz_case();

        v.start = values[n];
        sz_run(s, &v);
        save_paired_renderer(s, path, raw);
        g_assert_cmphex(raw[192 + 0x1c8 / 4], ==, (uint64_t)values[n]);
    }
    for (unsigned word = 1; word < 3; word++) {
        for (unsigned negative = 0; negative < 2; negative++) {
            SZCase v = sz_case();
            Shape c = pipeline_prepare(s, v.mode, 0);
            int64_t slope = negative ? -limit : limit;

            sz_initialize(s, &c, v.old);
            if (word == 1) {
                v.dx = slope;
            } else {
                v.dy = slope;
            }
            v.draw &= ~0x78U;
            sz_program(s, &v);
            qtest_writel(s, DE + 0x88, 64);
            qtest_writel(s, DE + 0x94, 64);
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            g_assert_cmphex(pixel_read(s, pixel_address(2, 0, 0, 4), 4),
                            ==, 0x53000012);
            save_paired_renderer(s, path, raw);
            g_assert_cmphex(raw[192 + (0x1c8 + 8 * word) / 4],
                            ==, (uint64_t)slope);
        }
    }
    for (unsigned word = 0; word < 3; word++) {
        for (unsigned n = 0; n < ARRAY_SIZE(invalid); n++) {
            SZCase v = sz_case();
            Shape c = pipeline_prepare(s, v.mode, 0);

            sz_initialize(s, &c, v.old);
            sz_program(s, &v);
            sz_atomic_failure(s, &c, path, DE + 0x9c8 + 8 * word,
                              invalid[n], 8);
        }
    }
    /* Interpolation can overflow only at a late sample hidden by scissor. */
    {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, v.mode, 0);

        sz_initialize(s, &c, v.old);
        v.start = limit - 3;
        v.dx = v.dy = 1;
        v.draw |= 0x100000;
        sz_program(s, &v);
        qtest_writeq(s, DE + 0x48, bounds(0, 0, 1, 1));
        sz_atomic_failure(s, &c, path, DE + 0x9c8, v.start, 8);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_sz_planes(void)
{
    const unsigned vertices[3][2] = { {112, 144}, {608, 96}, {480, 624} };
    const int origins[] = { 0, 1, -1, -4096, -4097 };
    const unsigned literal[2][2] = { {18, 6}, {23, 23} };
    const unsigned loaded[2][2] = { {25, 37}, {20, 20} };
    QTestState *s = start_indexed_state();

    for (unsigned direction = 0; direction < 4; direction++) {
        for (unsigned winding = 0; winding < 2; winding++) {
            for (unsigned origin = 0; origin < ARRAY_SIZE(origins); origin++) {
                for (unsigned global = 0; global < 2; global++) {
                    Shape c = pipeline_prepare(s, 0x228, 0);
                    SZCase v = sz_case();
                    int o = origins[origin];

                    c.width = c.height = 12;
                    sz_initialize(s, &c, v.old);
                    v.draw = (TRIANGLE_DRAW & ~0x78U) | 6;
                    v.start = (global ? loaded[direction >> 1][winding] : 20) *
                              4096LL;
                    v.dx = 2 * 4096;
                    v.dy = -4096;
                    sz_program(s, &v);
                    qtest_writel(s, DE + 0x60, 0x02000000 | direction << 16);
                    qtest_writel(s, DE + 0x58, xy((uint16_t)o, (uint16_t)o));
                    for (unsigned i = 0; i < 3; i++) {
                        unsigned index = winding && i ? 3 - i : i;

                        for (unsigned axis = 0; axis < 2; axis++) {
                            qtest_writel(s, DE + 0x80 + 8 * i + 4 * axis,
                                        vertices[index][axis] - 64 * o);
                        }
                    }
                    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0),
                                    ==, MEMTX_OK);
                    g_assert_cmphex(pixel_read(s, pixel_address(2, 4, 5, 4), 4),
                        ==, (v.old & 0xff000000U) |
                            (global ? 23 : literal[direction >> 1][winding]));
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_sz_fragment_order(void)
{
    const int start[] = { 100 * 4096, 80 * 4096, 60 * 4096, 20 * 4096 };
    const int dx[] = { 4096, -8192, 12288, 30 * 4096 };
    const int dy[] = { 8192, 4096, -4096, -3 * 4096 };
    QTestState *s = start_indexed_state();

    for (unsigned masked = 0; masked < 2; masked++) {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);
        uint32_t expected[16];

        v.draw = TRIANGLE_DRAW | 0x10f87;
        if (masked) {
            v.draw &= ~0x78U;
        }
        v.stencil = 0x62ff7003;
        v.plane = 0xa579b3e1;
        sz_initialize(s, &c, v.old);
        sz_program(s, &v);
        pipeline_shade(s, start, dx, dy);
        qtest_writel(s, DE + 0x198, 0x164);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                uint32_t source = pipeline_plane_color(start, dx, dy,
                                                       0, 0, x, y);
                bool alpha = (source & 255) < 100;

                expected[4 * y + x] = alpha ? pipeline_reference(0x228,
                    source, c.old, 0, 0x45, v.draw, 6, v.plane, x, y) : c.old;
            }
        }
        if (masked) {
            qtest_writeq(s, RE + 0x1000, 0);
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                uint32_t source = pipeline_plane_color(start, dx, dy,
                                                       0, 0, x, y);

                g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                    ==, (source & 255) < 100 ? 0x54000012 : v.old);
            }
        }
    }
    /* A rejected alpha test never accesses even an invalid SZ tile. */
    {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);
        uint32_t old[16];

        sz_initialize(s, &c, v.old);
        v.draw |= 0x800;
        sz_program(s, &v);
        qtest_writel(s, DE + 0x198, 0);
        qtest_writeq(s, RE + 0x1400, 0);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        for (unsigned i = 0; i < 16; i++) {
            old[i] = c.old;
        }
        shape_memory(s, &c, old);
    }
    qtest_quit(s);
}

static void test_sz_aliases(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-sz-alias-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned write = 0; write < 2; write++) {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);

        c.width = 130;
        c.height = 1;
        sz_initialize(s, &c, v.old);
        tile(s, 2, 1, page_for(2, 0));
        v.draw = (TRIANGLE_DRAW & ~0x78U) | 4 | write << 1;
        sz_program(s, &v);
        qtest_writel(s, DE + 0x88, 260 * 64);
        qtest_writel(s, DE + 0x94, 2 * 64);
        qtest_writel(s, DE + 0x18, v.draw | 0x100000);
        qtest_writeq(s, DE + 0x48, bounds(0, 0, 130, 1));
        if (write) {
            sz_atomic_failure(s, &c, path, DE + 0x9c8, v.start, 8);
        } else {
            g_assert_cmpint(memtx_writeq(s, DE + 0x9c8, v.start), ==, MEMTX_OK);
            for (unsigned x = 0; x < 128; x++) {
                g_assert_cmphex(pixel_read(s, pixel_address(2, x, 0, 4), 4),
                                ==, v.old);
            }
        }
    }
    /* Opposing color/SZ write masks do not legalize shared backing bytes. */
    {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x227, 0);

        sz_initialize(s, &c, v.old);
        tile(s, 2, 0, page_for(0, 0));
        v.draw |= 0x80;
        v.plane = 0xff000000;
        sz_program(s, &v);
        sz_atomic_failure(s, &c, path, DE + 0x9c8, v.start, 8);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void sz_rectangle(QTestState *s, unsigned direction,
                           unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    qtest_writel(s, DE + 0x60, 0x03000000 | direction << 16);
    qtest_writel(s, DE + 0x80, x0 * 64);
    qtest_writel(s, DE + 0x84, y0 * 64);
    qtest_writel(s, DE + 0x88, x1 * 64);
    qtest_writel(s, DE + 0x8c, y1 * 64);
}

static void test_sz_rect_controls(void)
{
    const unsigned modes[] = {
        0x228, 0x218, 0x124, 0x224, 0x225, 0x226, 0x227,
    };
    const unsigned unsupported[] = {
        0x2000, 0x4000, 0x8000, 0x20000, 0x40000, 0x80000,
        0x200000, 0x800000,
    };
    g_autofree char *dir = g_dir_make_tmp("crime-sz-rect-state-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned m = 0; m < ARRAY_SIZE(modes); m++) {
        for (unsigned bank = 0; bank < 3; bank++) {
            SZCase v = sz_case();
            Shape c = pipeline_prepare(s, modes[m], bank);

            sz_initialize(s, &c, v.old);
            v.draw = 0x410087;
            v.plane = 0;
            v.stencil = 0x62ff7002;
            sz_program(s, &v);
            sz_rectangle(s, 2, 0, 0, 3, 3);
            qtest_writel(s, DE, UINT32_MAX);
            qtest_writel(s, DE + 0x50, UINT32_MAX);
            qtest_writeq(s, DE + 0x70, UINT64_MAX);
            qtest_writeq(s, DE + 0x90, UINT64_MAX);
            qtest_writel(s, DE + 0x1a8, UINT32_MAX);
            qtest_writel(s, DE + 0xe0, UINT32_MAX);
            qtest_writel(s, DE + 0x100, UINT32_MAX);
            /* No color permission, including when color selects bank C. */
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 4; x++) {
                    g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                                    ==, 0x62000012);
                }
            }
        }
    }
    for (unsigned bad = 0; bad < ARRAY_SIZE(unsupported); bad++) {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);

        sz_initialize(s, &c, v.old);
        v.draw = 0x400007 | unsupported[bad];
        v.plane = 0;
        sz_program(s, &v);
        sz_rectangle(s, 2, 0, 0, 3, 3);
        sz_atomic_failure(s, &c, path, DE + 0x9c8, v.start, 8);
    }
    for (unsigned bad = 0; bad < 8; bad++) {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);

        sz_initialize(s, &c, v.old);
        v.draw = 0x400007;
        sz_program(s, &v);
        sz_rectangle(s, 2, 0, 0, 3, 3);
        switch (bad) {
        case 0:
            qtest_writel(s, DE + 8, 0x234);
            qtest_writeq(s, RE + 0x1000, 0);
            break;
        case 1:
            qtest_writel(s, DE + 0x80, 1);
            break;
        case 2:
            qtest_writel(s, DE + 0x8c, 0x7fc00000);
            break;
        case 3:
            qtest_writeq(s, DE + 0x1d0, 1);
            break;
        case 4:
            qtest_writeq(s, DE + 0x1d8, -1);
            break;
        case 5:
            qtest_writel(s, DE + 0x18, 7);
            qtest_writel(s, DE + 0x70, xy(0, 0));
            qtest_writel(s, DE + 0x74, xy(3, 3));
            break;
        case 6:
            qtest_writel(s, DE + 0x60, 0x03020001);
            break;
        case 7:
            qtest_writeq(s, RE + 0x1400, 0);
            break;
        }
        sz_atomic_failure(s, &c, path, DE + 0x9c8, v.start, 8);
    }
    for (unsigned direction = 0; direction < 4; direction++) {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);

        sz_initialize(s, &c, v.old);
        v.draw = 0x400007;
        sz_program(s, &v);
        sz_rectangle(s, direction, direction & 1 ? 0 : 3,
                     direction & 2 ? 0 : 3, direction & 1 ? 3 : 0,
                     direction & 2 ? 3 : 0);
        sz_atomic_failure(s, &c, path, DE + 0x9c8, v.start, 8);
    }
    /* Inactive Z/control payloads retain every raw bit. */
    {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);
        uint64_t raw[RENDER_WORDS];

        sz_initialize(s, &c, v.old);
        v.draw = 0x400001;
        v.control = 0xf0000000;
        v.start = INT64_MIN;
        v.dx = INT64_MAX;
        v.dy = -1;
        v.stencil = 0x62ff7003;
        sz_program(s, &v);
        sz_rectangle(s, 2, 0, 0, 3, 3);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        g_assert_cmphex(pixel_read(s, pixel_address(2, 1, 1, 4), 4),
                        ==, 0x54000011);
        save_paired_renderer(s, path, raw);
        g_assert_cmphex(raw[192 + 0x1c8 / 4], ==, (uint64_t)INT64_MIN);
        g_assert_cmphex(raw[192 + 0x1d0 / 4], ==, (uint64_t)INT64_MAX);
        g_assert_cmphex(raw[192 + 0x1d8 / 4], ==, UINT64_MAX);
        sz_atomic_failure(s, &c, path, DE + 0x818, v.draw | 4, 4);
    }
    /* Alpha-byte-only output is an actual color access, not SZ-only. */
    {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);

        sz_initialize(s, &c, v.old);
        v.draw = 0x40000e;
        sz_program(s, &v);
        tile(s, 0, 0, 0x7fff);
        sz_atomic_failure(s, &c, path, DE + 0x9c8, v.start, 8);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static const unsigned sz_offsets[] = {
    0x1c0, 0x1c8, 0x1d0, 0x1d8, 0x1e0, 0x1e8,
};
static const unsigned sz_widths[] = { 4, 8, 8, 8, 4, 4 };

static void test_sz_start_forms(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-sz-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(sz_offsets); n++) {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS], value;
        SZCase prospective;
        uint32_t expected[16];

        sz_initialize(s, &c, v.old);
        v.draw |= 0x201;
        v.stencil = 0x62ff7003;
        prospective = v;
        switch (n) {
        case 0:
            v.control = 0;
            value = prospective.control;
            break;
        case 1:
            v.start = 19 * 4096;
            value = prospective.start;
            break;
        case 2:
            prospective.dx = 4096;
            value = prospective.dx;
            break;
        case 3:
            prospective.dy = -4096;
            value = prospective.dy;
            break;
        case 4:
            v.stencil &= ~15U;
            value = prospective.stencil;
            break;
        case 5:
            v.mask = 0;
            value = prospective.mask;
            break;
        default:
            g_assert_not_reached();
        }
        sz_program(s, &v);
        save_paired_renderer(s, path, before);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + sz_offsets[n],
                                    value, sz_widths[n]), ==, MEMTX_OK);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                bool pass;
                uint32_t z = sz_reference_depth(prospective.start +
                    prospective.dx * x + prospective.dy * y);

                expected[4 * y + x] = pipeline_reference(0x228, v.color,
                    c.old, 0, 0x45, v.draw, 6, v.plane, x, y);
                g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                    ==, sz_reference(&prospective, v.old, z, &pass));
            }
        }
        shape_memory(s, &c, expected);
        save_paired_renderer(s, path, after);
        before[192 + sz_offsets[n] / 4] = value;
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    }
    for (unsigned n = 0; n < ARRAY_SIZE(sz_offsets); n++) {
        Shape c = shape_default();
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
        uint64_t value = sz_widths[n] == 8 ? UINT64_MAX : UINT32_MAX;
        g_autofree uint32_t *expected = NULL;

        c.bytes = 4;
        c.width = c.height = 4;
        c.primitive = 0;
        c.x0 = c.y0 = 1;
        expected = shape_reference(&c);
        map_region(s, 0, 4, 0, 0, 3, 3);
        shape_memory(s, &c, NULL);
        shape_stage(s, &c);
        save_paired_renderer(s, path, before);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + sz_offsets[n], value,
                                    sz_widths[n]), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
        save_paired_renderer(s, path, after);
        before[192 + sz_offsets[n] / 4] = value;
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_sz_widths_reserved(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-sz-widths-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(sz_offsets); n++) {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);
        uint64_t value;

        sz_initialize(s, &c, v.old);
        sz_program(s, &v);
        for (unsigned width = 1; width <= 8; width *= 2) {
            if (width != sz_widths[n]) {
                sz_atomic_failure(s, &c, path, DE + sz_offsets[n], 0, width);
                sz_atomic_failure(s, &c, path, DE + 0x800 + sz_offsets[n],
                                  0, width);
            }
        }
        for (unsigned delta = 1; delta < sz_widths[n]; delta++) {
            sz_atomic_failure(s, &c, path, DE + sz_offsets[n] + delta,
                              0, sz_widths[n]);
            sz_atomic_failure(s, &c, path, DE + 0x800 + sz_offsets[n] + delta,
                              0, sz_widths[n]);
        }
        g_assert_cmpint(memtx_read(s, DE + 0x800 + sz_offsets[n], &value,
                                   sz_widths[n]), !=, MEMTX_OK);
        if (sz_widths[n] == 8) {
            g_assert_cmpint(memtx_read(s, DE + sz_offsets[n], &value, 8),
                            !=, MEMTX_OK);
        }
    }
    for (unsigned reserved = 0; reserved < 12; reserved++) {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);

        v.draw |= 1;
        sz_initialize(s, &c, v.old);
        switch (reserved) {
        case 0:
            v.control |= 0x10000000;
            break;
        case 1:
            v.stencil |= 0x8000;
            break;
        case 2:
            v.stencil |= 0x600;
            break;
        case 3:
            v.stencil |= 0x60;
            break;
        case 4:
            v.stencil |= 6;
            break;
        case 5:
            v.mask |= 0x100;
            break;
        case 6:
            v.control |= 0x1000000;
            v.stencil = 0x62ff7002;
            v.mask = 1;
            break;
        case 7:
            v.control |= 0x1000000;
            v.stencil = 0x62ff6002;
            break;
        case 8:
            v.control |= 0x1000000;
            v.stencil = 0x62fe7002;
            break;
        case 9:
            v.control |= 0x1000000;
            v.stencil = 0x62ff7102;
            break;
        case 10:
            v.control |= 0x1000000;
            v.stencil = 0x62ff7032;
            break;
        case 11:
            v.control |= 0x1000000;
            v.draw &= ~1U;
            break;
        }
        sz_program(s, &v);
        qtest_writel(s, DE + 0x18, v.draw & ~0x78U);
        sz_atomic_failure(s, &c, path, DE + 0x9c8, v.start, 8);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_sz_late_faults(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-sz-late-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned lane = 0; lane < 4; lane++) {
        for (unsigned fault = 0; fault < 3; fault++) {
            SZCase v = sz_case();
            Shape c = pipeline_prepare(s, 0x228, lane & 1);
            unsigned x = 126 + 128 * lane, y = 126;
            unsigned index = 16 + (x + 3) / 128;

            c.x = x;
            c.y = y;
            map_region(s, c.bank, 4, x, y, x + 3, y + 3);
            shape_memory(s, &c, NULL);
            sz_initialize(s, &c, v.old);
            sz_program(s, &v);
            qtest_writel(s, DE + 0x80, x * 64);
            qtest_writel(s, DE + 0x84, y * 64);
            qtest_writel(s, DE + 0x88, (x + 8) * 64);
            qtest_writel(s, DE + 0x8c, y * 64);
            qtest_writel(s, DE + 0x90, x * 64);
            qtest_writel(s, DE + 0x94, (y + 8) * 64);
            if (fault < 2) {
                tile(s, 2, index, 0x7fff);
                if (fault) {
                    uint64_t address = RE + 0x1400 + (index / 4) * 8;
                    uint64_t tlb = qtest_readq(s, address);

                    qtest_writeq(s, address, tlb & ~(0x8000ULL <<
                                 (48 - 16 * (index % 4))));
                }
            } else {
                tile(s, c.bank, index, 0x7fff);
            }
            sz_atomic_failure(s, &c, path, DE + 0x9c8, v.start, 8);
        }
    }
    for (unsigned row = 0; row < 2; row++) {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);

        c.x = c.y = 126;
        sz_initialize(s, &c, 0x10123456);
        v.draw = 0x400007;
        v.control = 0x03654321;
        v.stencil = 0x20ff5002 | row << 5;
        v.start = 0x700000LL * 4096;
        sz_program(s, &v);
        sz_rectangle(s, 2, 126, 126, 129, 129);
        tile(s, 2, 17, 0x7fff);
        sz_atomic_failure(s, &c, path, DE + 0x9c8, v.start, 8);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_sz_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-sz-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);

    for (unsigned depth = 0; depth < 2; depth++) {
        QTestState *from = start(), *to = start_options("-incoming defer");
        SZCase v = sz_case();
        Shape c = pipeline_prepare(from, 0x228, 0);
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
        uint32_t color[16], expected;

        sz_initialize(from, &c, v.old);
        v.draw = TRIANGLE_DRAW | 0x201 | (depth ? 6 : 0);
        v.stencil = 0x62ff7003;
        if (!depth) {
            v.control = 0xf0000000;
            v.start = INT64_MIN;
            v.dx = INT64_MAX;
            v.dy = -1;
        }
        sz_program(from, &v);
        g_assert_cmpint(memtx_writel(from, DE + 0x9f0, 0), ==, MEMTX_OK);
        expected = depth ? 0x54000012 : 0x54000011;
        for (unsigned i = 0; i < 16; i++) {
            color[i] = c.old ^ v.color;
        }
        save_paired_renderer(from, path, before);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        save_paired_renderer(to, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        shape_memory(to, &c, color);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                g_assert_cmphex(pixel_read(to, pixel_address(2, x, y, 4), 4),
                                ==, expected);
            }
        }
        qtest_system_reset(to);
        shape_memory(to, &c, color);
        g_assert_cmphex(pixel_read(to, pixel_address(2, 1, 1, 4), 4),
                        ==, expected);
        save_renderer(to, path, after);
        for (unsigned n = 0; n < ARRAY_SIZE(sz_offsets); n++) {
            g_assert_cmphex(after[192 + sz_offsets[n] / 4], ==, 0);
        }
        qtest_quit(from);
        qtest_quit(to);
        g_unlink(sock);
    }
    g_unlink(path);
    g_rmdir(dir);
}

static void test_sz_rect_clipping(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-sz-rect-clip-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned outside = 0; outside < 2; outside++) {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS];

        sz_initialize(s, &c, v.old);
        v.draw = 0x500006;
        sz_program(s, &v);
        sz_rectangle(s, 2, 0, 0, 3, 3);
        qtest_writel(s, DE + 0x58, outside ? 0xf000f000 : 0xffffffff);
        qtest_writeq(s, DE + 0x48, bounds(1, 1, 3, 3));
        qtest_writel(s, DE + 0x10, 0x21);
        qtest_writeq(s, DE + 0x20, bounds(1, 1, 3, 3));
        qtest_writeq(s, RE + 0x1000, 0);
        save_paired_renderer(s, path, before);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        save_paired_renderer(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                    ==, !outside && x == 1 && y == 1 ? 0x53000012 : v.old);
            }
        }
        if (outside) {
            sz_atomic_failure(s, &c, path, DE + 0x9c8,
                              (INT64_C(1) << 36) + 1, 8);
        }
    }
    for (unsigned direction = 0; direction < 4; direction++) {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);

        sz_initialize(s, &c, v.old);
        v.draw = 0x400006;
        sz_program(s, &v);
        sz_rectangle(s, direction, 1, 2, 1, 2);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                    ==, x == 1 && y == 2 ? 0x53000012 : v.old);
            }
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_sz_populated_banks(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-sz-banks-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_profile_ram("legacy-software", true, true, false, "");
    SZCase v = sz_case();
    Shape c = pipeline_prepare(s, 0x228, 0);

    for (unsigned i = 0; i < 8; i++) {
        qtest_writeq(s, CRIME + 0x208 + i * 8, 0x100 | i * 4);
    }
    v.draw = 0x400007;
    v.control = 0x0f654321;
    v.stencil = 0x20ff5022;
    v.start = 0x654321LL * 4096;
    sz_program(s, &v);
    sz_rectangle(s, 2, 126, 0, 129, 0);
    tile(s, 2, 0, 0x7ff);
    tile(s, 2, 1, 0x800);
    for (unsigned x = 126; x < 130; x++) {
        uint64_t address = x < 128 ? 0x7ff0000ULL + 4 * x :
                                    0x8000000ULL + 4 * (x - 128);

        pixel_write(s, address, 4, 0x10123456);
    }
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    for (unsigned x = 126; x < 130; x++) {
        uint64_t address = x < 128 ? 0x7ff0000ULL + 4 * x :
                                    0x8000000ULL + 4 * (x - 128);

        g_assert_cmphex(pixel_read(s, address, 4), ==, 0x20654321);
        pixel_write(s, address, 4, 0x10123456);
    }
    qtest_writeq(s, CRIME + 0x210, 0);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_ERROR);
    for (unsigned x = 126; x < 128; x++) {
        g_assert_cmphex(pixel_read(s, 0x7ff0000ULL + 4 * x, 4), ==, 0x10123456);
    }
    qtest_writeq(s, CRIME + 0x210, 0x104);
    /* Different physical views of the same RAM still form a forbidden alias. */
    c = pipeline_prepare(s, 0x228, 0);
    sz_initialize(s, &c, v.old);
    v = sz_case();
    v.draw |= 0x80;
    v.plane = 0xff000000;
    sz_program(s, &v);
    tile(s, 2, 0, 0x4003);
    g_assert_cmphex(pixel_read(s, 0x40030000, 4), ==, c.old);
    {
        IfbSnapshot before, after;
        uint32_t old_color[16];

        for (unsigned i = 0; i < 16; i++) {
            old_color[i] = c.old;
        }
        ifb_snapshot(s, &before);
        g_assert_cmpint(memtx_writeq(s, DE + 0x9c8, 19 * 4096),
                        ==, MEMTX_ERROR);
        ifb_snapshot(s, &after);
        g_assert_cmpmem(&before, sizeof(before), &after, sizeof(after));
        shape_memory(s, &c, old_color);
        /* Banked CRIME is deliberately non-migratable; observe raw rollback. */
        tile(s, 2, 0, page_for(2, 0));
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        g_assert_cmphex(pixel_read(s, pixel_address(2, 1, 1, 4), 4),
                        ==, 0x53000012);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_sz_tag_literals(void)
{
    QTestState *s = start_indexed_state();
    const unsigned references[] = { 0, 1, 254, 255, 0 };

    for (unsigned match = 0; match < 2; match++) {
        for (unsigned row = 0; row < 2; row++) {
            SZCase v = sz_case();

            v.rectangle = true;
            v.draw = 0x400007;
            v.control = 0x03654321;
            v.stencil = 0x20ff5002 | row << 5;
            v.old = match ? 0x20123456 : 0x10123456;
            v.start = (match ? 0x100000LL : 0x700000LL) * 4096;
            sz_run(s, &v);
            /* Failed-depth materialization writes clear Z and tag together. */
            g_assert_cmphex(pixel_read(s, pixel_address(2, 1, 1, 4), 4), ==,
                match ? (row ? 0x20123456 : 0x20100000) :
                        (row ? 0x20654321 : 0x10123456));
        }
    }
    {
        SZCase v = sz_case();
        Shape c = pipeline_prepare(s, 0x228, 0);
        uint32_t old[16];

        sz_initialize(s, &c, 0);
        v.draw = 0x400007;
        v.control = 0x0f654321;
        v.stencil = 0x20ff5022;
        v.start = 0x654321LL * 4096;
        sz_program(s, &v);
        sz_rectangle(s, 2, 0, 0, 3, 3);
        qtest_writeq(s, RE + 0x1000, 0);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                old[4 * y + x] = ((x + y) & 1 ? 0x20000000 : 0x10000000) |
                                 (0x123456 + 17 * (4 * y + x));
                pixel_write(s, pixel_address(2, x, y, 4), 4, old[4 * y + x]);
            }
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        qtest_writel(s, DE + 0x1c0, v.control & ~0x1000000U);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                    ==, (x + y) & 1 ? old[4 * y + x] : 0x20654321);
            }
        }
        for (unsigned n = 0; n < ARRAY_SIZE(references); n++) {
            uint32_t physical = references[n] << 24 | 0x654321;

            v.control = 0x0f654321;
            v.stencil = references[n] << 24 | 0xff7002;
            sz_program(s, &v);
            sz_rectangle(s, 2, 0, 0, 3, 3);
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 4; x++) {
                    g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                                    ==, physical);
                    pixel_write(s, pixel_address(2, x, y, 4), 4, 0x10123456);
                }
            }
            for (unsigned band = 0; band < 2; band++) {
                sz_rectangle(s, 2, 0, 2 * band, 3, 2 * band + 1);
                g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            }
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 4; x++) {
                    g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                                    ==, physical);
                    pixel_write(s, pixel_address(2, x, y, 4), 4, 0x10123456);
                }
            }
            /* MTE clears the same physical bytes without tag history. */
            qtest_writel(s, MTE, 0x208);
            qtest_writel(s, MTE + 8, UINT32_MAX);
            qtest_writel(s, MTE + 0x18, physical);
            qtest_writel(s, MTE + 0x30, xy(0, 0));
            qtest_writel(s, MTE + 0x48, 1);
            g_assert_cmpint(memtx_writel(s, MTE + 0x838, xy(15, 3)),
                            ==, MEMTX_OK);
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 4; x++) {
                    g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                                    ==, physical);
                }
            }
        }
    }
    qtest_quit(s);
}

static void texture_constant(QTestState *s)
{
    /* A 1x1 RGBA8 image occupies the final packed mip allocation. */
    uint64_t tlb = UINT64_C(0x800a) << 32;

    qtest_writeq(s, RE + 0x1600 + (93 / 4) * 8, tlb);
    qtest_writel(s, 0xafc00, 0x2371b9d5);
    qtest_writel(s, DE + 0x110, (2U << 20) | (2U << 18) |
                 (10U << 14) | (2U << 4) | (2U << 2) | 3U);
    qtest_writeq(s, DE + 0x118, UINT64_C(0xaa00000f000f));
    qtest_writeq(s, DE + 0x120, (UINT64_C(1) << 43) + 0x800);
    qtest_writeq(s, DE + 0x128, (UINT64_C(1) << 43) + 0x800);
    qtest_writel(s, DE + 0x130, 0x20000800);
    qtest_writel(s, DE + 0x134, 0);
    qtest_writeq(s, DE + 0x138, 0);
    qtest_writeq(s, DE + 0x140, 0);
    qtest_writeq(s, DE + 0x148, 0);
    qtest_writeq(s, DE + 0x150, 0);
    qtest_writel(s, DE + 0x158, 0);
    qtest_writel(s, DE + 0x15c, 0);
}

static void test_texture_constant(void)
{
    QTestState *s = start_indexed_state();
    Shape c = triangle_prepare(s, 0, 0, 0);

    triangle_vertices(s, triangle_integer, NULL, true);
    texture_constant(s);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000);
    g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==,
                    MEMTX_OK);
    triangle_pixels(s, &c, triangle_ten, 0x2371b9d5, 3, UINT32_MAX);
    qtest_quit(s);
}

static void test_texture_start_q(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-texture-start-q-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    Shape c = triangle_prepare(s, 0, 0, 0);
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];

    triangle_vertices(s, triangle_integer, NULL, true);
    texture_constant(s);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000);
    qtest_writel(s, DE + 0xd0, TRIANGLE_COLOR);
    qtest_writel(s, DE + 0x130, 0);
    qtest_writel(s, DE + 0x134, 3);
    save_paired_renderer(s, path, before);
    g_assert_cmpint(memtx_writel(s, DE + 0x930, 0x1f3fe5c0), ==, MEMTX_OK);
    triangle_pixels(s, &c, triangle_ten, 0x2371b9d5, 3, UINT32_MAX);
    save_paired_renderer(s, path, after);
    before[192 + 0x130 / 4] = 0x1f3fe5c0;
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static uint32_t texture_environment(unsigned type, unsigned depth,
                                    unsigned function, uint32_t stored)
{
    const uint64_t one = 7905; /* lcm(15, 31, 255) */
    uint64_t texel[4];
    uint32_t result = 0;
    bool alpha_only = type == 5;
    bool rgb_only = type == 1 || type == 7;

    for (unsigned c = 0; c < 4; c++) {
        unsigned shift = 24 - 8 * c;
        if (type == 4) {
            texel[c] = ((stored >> (12 - 4 * c)) & 15) * 527;
        } else if (type >= 5) {
            texel[c] = (c == 3 ? stored & 255 : stored >> 8 & 255) * 31;
            if (type == 6) {
                texel[c] = (stored >> 8 & 255) * 31;
            }
        } else if (depth == 2) {
            texel[c] = (stored >> shift & 255) * 31;
        } else {
            texel[c] = c == 3 ? (stored >> 15) * one :
                       (stored >> (10 - 5 * c) & 31) * 255;
        }
    }
    for (unsigned c = 0; c < 4; c++) {
        unsigned shift = 24 - 8 * c;
        uint64_t f = TRIANGLE_COLOR >> shift & 255;
        uint64_t e = 0x39c765a1U >> shift & 255;
        uint64_t n, d = one;
        bool present = c == 3 ? !rgb_only : !alpha_only;

        if (!present) {
            n = f * one;
        } else if (function == 3 || (function == 1 && type == 1)) {
            n = 255 * texel[c];
        } else if (function == 0 || (function == 2 && c == 3 &&
                                             type != 6)) {
            n = f * texel[c];
        } else if (function == 1) {
            n = c == 3 ? f * one * one :
                f * (one - texel[3]) * one + 255 * texel[c] * texel[3];
            d = one * one;
        } else {
            n = f * (one - texel[c]) + e * texel[c];
        }
        result |= ((2 * n + d) / (2 * d)) << shift;
    }
    return result;
}

static void test_texture_formats(void)
{
    const unsigned formats[][2] = {
        {1, 1}, {1, 2}, {2, 1}, {2, 2}, {4, 1},
        {5, 1}, {6, 1}, {7, 1}, {8, 1},
    };
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(formats); n++) {
        unsigned type = formats[n][0], depth = formats[n][1];
        uint32_t stored = depth == 2 ? 0x235bad79 : 0xad39;

        for (unsigned function = 0; function < 4; function++) {
            Shape c = triangle_prepare(s, 0, 0, 0);
            uint32_t mode = (type << 20) | (depth << 18) | (10U << 14) |
                            (2U << 4) | (2U << 2) | function;
            bool undefined = function == 1 && type >= 5;

            triangle_vertices(s, triangle_integer, NULL, true);
            texture_constant(s);
            pixel_write(s, 0xafc00, 1U << depth, stored);
            qtest_writel(s, DE + 0x110, mode);
            qtest_writel(s, DE + 0x168, 0x39c765a1);
            qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000);
            g_test_message("type %u depth %u function %u",
                           type, depth, function);
            g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==,
                            undefined ? MEMTX_ERROR : MEMTX_OK);
            if (undefined) {
                shape_memory(s, &c, NULL);
            } else {
                triangle_pixels(s, &c, triangle_ten,
                    texture_environment(type, depth, function, stored),
                    3, UINT32_MAX);
            }
        }
    }
    qtest_quit(s);
}

static uint64_t texture_format(unsigned u, unsigned v)
{
    return (uint64_t)(10 - u) << 44 | (uint64_t)(10 - v) << 40 |
           (uint64_t)u << 36 | (uint64_t)v << 32 |
           (uint64_t)((1U << (u + 4)) - 1) << 16 |
           ((1U << (v + 4)) - 1);
}

static void texture_two_by_two(QTestState *s)
{
    texture_constant(s);
    qtest_writel(s, DE + 0x110, (2U << 20) | (2U << 18) | (9U << 14) |
                 (1U << 10) | (2U << 4) | (2U << 2) | 3U);
    qtest_writeq(s, DE + 0x118, texture_format(1, 1));
    qtest_writel(s, 0xaf800, 0x23456789);
    qtest_writel(s, 0xaf804, 0x91b3d5f7);
    qtest_writel(s, 0xafa00, 0x17395b7d);
    qtest_writel(s, 0xafa04, 0x8caec0e2);
}

static void test_texture_perspective(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned flags = 0; flags < 4; flags++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            Shape c = triangle_prepare(s, 0, 0, 0);
            uint32_t expected[64];
            int64_t uscale = INT64_C(1) << (43 - 4 * (flags & 1));
            int64_t vscale = INT64_C(1) << (43 - 4 * ((flags >> 1) & 1));

            triangle_vertices(s, triangle_integer, NULL, true);
            texture_two_by_two(s);
            qtest_writeq(s, DE + 0x120, sign * 2 * uscale + 0x800);
            qtest_writeq(s, DE + 0x128, sign * (vscale + vscale / 4) + 0x800);
            qtest_writeq(s, DE + 0x148, sign * (vscale * 5 / 16));
            qtest_writel(s, DE + 0x130, sign * 0x20000000 + 0x800);
            qtest_writel(s, DE + 0x158, sign * 0x08000000);
            qtest_writel(s, DE + 0x134, flags);
            qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000);
            g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==,
                            MEMTX_OK);
            for (unsigned y = 0; y < 8; y++) {
                for (unsigned x = 0; x < 8; x++) {
                    expected[y * 8 + x] = triangle_ten[y] & (1U << x) ?
                        (x <= 2 ? 0x91b3d5f7 : 0x23456789) : c.old;
                }
            }
            shape_memory(s, &c, expected);
        }
    }
    qtest_quit(s);
}

static void test_texture_filtering(void)
{
    const unsigned wraps[] = {0, 1, 2, 3};
    QTestState *s = start_indexed_state();

    for (unsigned wrap = 0; wrap < ARRAY_SIZE(wraps); wrap++) {
        for (unsigned linear = 0; linear < 2; linear++) {
            Shape c = triangle_prepare(s, 0, 0, 0);
            uint32_t expected;

            triangle_vertices(s, triangle_integer, NULL, true);
            texture_two_by_two(s);
            qtest_writel(s, DE + 0x160, 0x17395b7d);
            qtest_writel(s, DE + 0x110, (2U << 20) | (2U << 18) |
                         (9U << 14) | (1U << 10) | linear << 6 |
                         linear << 7 | wraps[wrap] << 4 | (2U << 2) | 3U);
            /* S=-1/4, T=1/4: one tap is border under every clamp mode. */
            qtest_writeq(s, DE + 0x120, (UINT64_C(3) << 41) + 0x800);
            qtest_writeq(s, DE + 0x128, (UINT64_C(5) << 41) + 0x800);
            expected = wrap == 1 ? 0x91b3d5f7 : wrap == 2 ? 0x23456789 :
                       wrap == 0 && !linear ? 0x23456789 :
                       wrap == 0 ? 0x1d3f6183 : 0x17395b7d;
            qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000);
            g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==,
                            MEMTX_OK);
            triangle_pixels(s, &c, triangle_ten, expected, 3, UINT32_MAX);
        }
    }
    qtest_quit(s);
}

static void texture_mip_colors(QTestState *s)
{
    const uint32_t colors[] = {0x20406080, 0x6080a0c0, 0xa0c0e0f0};
    const unsigned offsets[] = {0xf000, 0xf800, 0xfc00};

    texture_constant(s);
    qtest_writeq(s, DE + 0x118, texture_format(2, 2));
    for (unsigned level = 0; level < 3; level++) {
        unsigned size = 4U >> level;
        for (unsigned y = 0; y < size; y++) {
            for (unsigned x = 0; x < size; x++) {
                qtest_writel(s, 0xa0000 + offsets[level] + y * 512 + x * 4,
                             colors[level]);
            }
        }
    }
}

static uint32_t texture_mip_expected(unsigned minimum, unsigned maximum,
                                      int lod)
{
    const uint32_t colors[] = {0x20406080, 0x6080a0c0, 0xa0c0e0f0};
    unsigned level, next, weight;
    uint32_t result = 0;
    int transition = maximum && (minimum == 2 || minimum == 4) ? 128 : 0;

    if (lod <= transition || minimum < 2) {
        return colors[0];
    }
    if (minimum == 2 || minimum == 3) {
        level = MIN((lod + 127) / 256, 2);
        return colors[level];
    }
    level = MIN(lod / 256, 2);
    next = MIN(level + 1, 2);
    weight = level == next ? 0 : lod % 256;
    for (unsigned c = 0; c < 4; c++) {
        unsigned shift = 24 - 8 * c;
        unsigned a = colors[level] >> shift & 255;
        unsigned b = colors[next] >> shift & 255;
        result |= ((a * (256 - weight) + b * weight + 128) / 256) << shift;
    }
    return result;
}

static void test_texture_lod(void)
{
    /* Each pair is twice the base-image texel-space X derivative. */
    const int slopes[][3] = {
        {0, 0, -4096}, {1, 0, -256}, {2, 0, 0}, {2, 2, 128},
        {4, 0, 256}, {3, 0, 149}, {4, 4, 384}, {8, 0, 512},
    };
    QTestState *s = start_indexed_state();

    for (unsigned min = 0; min < 6; min++) {
        for (unsigned mag = 0; mag < 2; mag++) {
            for (unsigned n = 0; n < ARRAY_SIZE(slopes); n++) {
                Shape c = triangle_prepare(s, 0, 0, 0);
                triangle_vertices(s, triangle_integer, NULL, true);
                texture_mip_colors(s);
                qtest_writel(s, DE + 0x110, (2U << 20) | (2U << 18) |
                             (8U << 14) | (2U << 10) | min << 7 | mag << 6 |
                             (2U << 4) | (2U << 2) | 3U);
                qtest_writeq(s, DE + 0x138,
                             slopes[n][0] * (UINT64_C(1) << 40));
                qtest_writeq(s, DE + 0x148,
                             slopes[n][1] * (UINT64_C(1) << 40));
                qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000);
                g_test_message("min %u mag %u footprint %u", min, mag, n);
                g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==,
                                MEMTX_OK);
                triangle_pixels(s, &c, triangle_ten,
                    texture_mip_expected(min, mag, slopes[n][2]),
                    3, UINT32_MAX);
            }
        }
    }
    /* At x=0, 1/(1+x) has finite difference -1/2, analytic derivative -1. */
    {
        const int vertices[3][2] = { {64, 64}, {128, 64}, {64, 128} };
        const uint8_t one_pixel[8] = {0, 2};
        Shape c = triangle_prepare(s, 0, 0, 0);
        triangle_vertices(s, vertices, NULL, true);
        texture_mip_colors(s);
        qtest_writel(s, DE + 0x110, (2U << 20) | (2U << 18) |
                     (8U << 14) | (2U << 10) | (2U << 7) |
                     (2U << 4) | (2U << 2) | 3U);
        qtest_writeq(s, DE + 0x120, (UINT64_C(1) << 44) + 0x800);
        qtest_writeq(s, DE + 0x138, UINT64_C(1) << 43);
        qtest_writeq(s, DE + 0x148, UINT64_C(1) << 43);
        qtest_writel(s, DE + 0x158, 0x20000000);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000);
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==,
                        MEMTX_OK);
        triangle_pixels(s, &c, one_pixel, 0x6080a0c0, 3, UINT32_MAX);
    }
    qtest_quit(s);
}

static void test_texture_transactions(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-texture-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned failure = 0; failure < 13; failure++) {
        Shape c = triangle_prepare(s, 0, 0, 0);
        triangle_vertices(s, triangle_integer, NULL, true);
        texture_two_by_two(s);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000);
        qtest_writel(s, DE + 0xd0, 0x76543210);
        switch (failure) {
        case 0:
            qtest_writel(s, DE + 0x130, 0);
            break;
        case 1:
            /* Q is nonzero at the first sample, zero at its X neighbor. */
            qtest_writel(s, DE + 0x158, 0xe0000000);
            break;
        case 2:
            qtest_writeq(s, DE + 0x138, INT64_MAX);
            break;
        case 3:
            qtest_writeq(s, RE + 0x1600 + (93 / 4) * 8, 0);
            break;
        case 4:
            qtest_writeq(s, RE + 0x1600 + (93 / 4) * 8,
                         UINT64_C(0xffff) << 32);
            break;
        case 5:
            qtest_writel(s, DE + 0x110, 0x28452b | (6U << 7));
            break;
        case 6:
            qtest_writeq(s, DE + 0x118, texture_format(1, 1) ^ 1);
            break;
        case 7:
            qtest_writel(s, DE + 0x60, 0x01000020);
            break;
        case 8:
            qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0xa000);
            break;
        case 9:
            qtest_writel(s, DE + 0x134, 4);
            break;
        case 10:
            qtest_writeq(s, DE + 0x118, texture_format(8, 0));
            qtest_writel(s, DE + 0x110, (2U << 20) | (2U << 18) |
                         (2U << 14) | (8U << 10) |
                         (2U << 4) | (2U << 2) | 3U);
            qtest_writeq(s, DE + 0x120, (UINT64_C(765) << 34) + 0x800);
            qtest_writeq(s, DE + 0x138, UINT64_C(1) << 35);
            qtest_writeq(s, RE + 0x1600 + (80 / 4) * 8,
                         UINT64_C(0x800a) << 48);
            qtest_writel(s, 0xa01f8, 0x2371b9d5);
            qtest_writel(s, 0xa01fc, 0x91b3d5f7);
            break;
        case 11:
            qtest_writel(s, DE + 0x130, 0x60000800);
            qtest_writel(s, DE + 0x158, 0xe0000000);
            break;
        case 12:
            qtest_writel(s, DE + 0x15c, 0xe0000000);
            break;
        }
        pipeline_failure(s, &c, path, DE + 0x8d0, TRIANGLE_COLOR, 4);
    }
    {
        const int vertices[3][2] = { {0, 8064}, {256, 8064}, {0, 8320} };
        const uint8_t rows[8] = {15, 7, 3, 1};
        Shape c = triangle_prepare(s, 0, 0, 126);
        triangle_vertices(s, vertices, NULL, true);
        texture_constant(s);
        qtest_writeq(s, RE + 0x1600 + (93 / 4) * 8,
                     UINT64_C(0x8003) << 32);
        qtest_writel(s, DE + 0x110, (2U << 20) | (2U << 18) |
                     (10U << 14) | (2U << 4) | (2U << 2));
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000);
        /* The first output overwrites every later sample's source texel. */
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==,
                        MEMTX_OK);
        triangle_pixels(s, &c, rows,
            texture_environment(2, 2, 0, c.old), 3, UINT32_MAX);
        shape_memory(s, &c, NULL);
        /* Restore the CPU upload after clearing the framebuffer fixture. */
        qtest_writel(s, 0x3fc00, 0x31a7d95b);
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==,
                        MEMTX_OK);
        triangle_pixels(s, &c, rows,
            texture_environment(2, 2, 0, 0x31a7d95b), 3, UINT32_MAX);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_texture_addresses(void)
{
    QTestState *s = start_indexed_state();
    for (unsigned n = 0; n < ARRAY_SIZE(texture_addresses); n++) {
        const CrimeTextureAddressVector *v = &texture_addresses[n];
        for (unsigned subtiled = 0; subtiled < 2; subtiled++) {
            Shape c = triangle_prepare(s, 0, 0, 0);
            unsigned page = 0x100 + v->entry * 3;
            unsigned offset = subtiled ? v->subtiled : v->ordinary;
            uint64_t address = ((uint64_t)page << 16) + offset;
            uint64_t word;
            unsigned shift = 48 - 16 * (v->entry % 4);
            uint32_t raw = v->bytes == 4 ? 0x315b9de7 : 0x39be;
            uint32_t expected = v->bytes == 4 ? raw : 0x3399bbee;
            uint8_t guard[12];
            unsigned before = offset ? 4 : 0;

            triangle_vertices(s, triangle_integer, NULL, true);
            texture_constant(s);
            qtest_writeq(s, DE + 0x118, texture_format(v->u, v->v));
            qtest_writel(s, DE + 0x110, (subtiled << 24) |
                         ((v->bytes == 4 ? 2U : 4U) << 20) |
                         ((v->bytes == 4 ? 2U : 1U) << 18) |
                         ((10 - v->u) << 14) | (v->u << 10) |
                         (2U << 4) | (2U << 2) | 3U);
            word = qtest_readq(s, RE + 0x1600 + (v->entry / 4) * 8);
            word &= ~(UINT64_C(65535) << shift);
            word |= (uint64_t)(0x8000 | page) << shift;
            qtest_writeq(s, RE + 0x1600 + (v->entry / 4) * 8, word);
            qtest_writeq(s, DE + 0x120, (UINT64_C(1) << 43) +
                         ((uint64_t)(2 * v->x + 1) << (42 - v->u)) + 0x800);
            qtest_writeq(s, DE + 0x128, (UINT64_C(1) << 43) +
                         ((uint64_t)(2 * v->y + 1) << (42 - v->v)) + 0x800);
            qtest_memset(s, address - before, 0xa5, before + v->bytes + 4);
            pixel_write(s, address, v->bytes, raw);
            qtest_memread(s, address - before, guard, before + v->bytes + 4);
            qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000);
            g_test_message("address vector %u layout %u", n, subtiled);
            g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==,
                            MEMTX_OK);
            triangle_pixels(s, &c, triangle_ten, expected, 3, UINT32_MAX);
            {
                uint8_t after[12];
                qtest_memread(s, address - before, after,
                              before + v->bytes + 4);
                g_assert_cmpmem(guard, before + v->bytes + 4,
                                after, before + v->bytes + 4);
            }
        }
    }
    qtest_quit(s);
}

static void texture_half_lsb(QTestState *s, bool alpha)
{
    texture_two_by_two(s);
    qtest_writel(s, DE + 0x110, ((alpha ? 5U : 1U) << 20) |
                 ((alpha ? 1U : 2U) << 18) | (9U << 14) | (1U << 10) |
                 (1U << 7) | (1U << 6) | (2U << 4) | (2U << 2));
    qtest_writeq(s, DE + 0x120, (UINT64_C(3) << 42) + 0x800);
    qtest_writeq(s, DE + 0x128, (UINT64_C(5) << 41) + 0x800);
    pixel_write(s, 0xaf800, alpha ? 2 : 4, 0);
    pixel_write(s, alpha ? 0xaf802 : 0xaf804,
                alpha ? 2 : 4, alpha ? 1 : 0x01000000);
}

static void test_texture_output_order(void)
{
    QTestState *s = start_indexed_state();
    for (unsigned zero = 0; zero < 2; zero++) {
        Shape c = pipeline_prepare(s, 0x228, 0);
        SZCase v = sz_case();
        uint32_t expected[16];
        v.draw = TRIANGLE_DRAW | 0x8000 | 0x800 | 0x400 | 7;
        v.stencil = 0x62ff7002;
        map_region(s, 2, 4, 0, 0, 3, 3);
        sz_program(s, &v);
        texture_half_lsb(s, true);
        qtest_writel(s, DE + 0x198, 0x400);
        if (zero) {
            qtest_writew(s, 0xaf802, 0);
            tile(s, 2, 0, 0);
            qtest_writeq(s, RE + 0x1000 + 2 * 0x200, 0);
        }
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                pixel_write(s, pixel_address(2, x, y, 4), 4, v.old);
                expected[4 * y + x] = zero ? c.old : pipeline_reference(
                    0x228, 0x42719b01, c.old, 0, 0x45, v.draw,
                    6, UINT32_MAX, x, y);
            }
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, 0x42719bff), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                                ==, zero ? v.old : 0x62000012);
            }
        }
    }
    for (unsigned n = 0; n < 3; n++) {
        const unsigned modes[] = {0x228, 0x124, 0x227};
        Shape c = pipeline_prepare(s, modes[n], 0);
        uint32_t expected[16];
        unsigned draw = TRIANGLE_DRAW | 0x8000 | 0x400 | 0x100;
        texture_half_lsb(s, false);
        qtest_writel(s, DE + 0x18, draw);
        qtest_writel(s, DE + 0x1a8, 0x11);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                expected[4 * y + x] = pipeline_reference(modes[n],
                    0x010000ff, c.old, 0, 0x11, draw, 3, UINT32_MAX, x, y);
            }
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, UINT32_MAX), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
}

static void test_texture_anchor(void)
{
    const int scenes[][3][2] = {
        { {64, 64}, {320, 64}, {64, 320} },
        { {-1, -1}, {255, -1}, {-1, 255} },
        { {65, 65}, {321, 65}, {65, 321} },
    };
    const uint8_t rows[][8] = {
        {0, 0x1e, 0x0e, 0x06, 0x02}, {15, 7, 3, 1},
        {0, 0, 0x1c, 0x0c, 0x04},
    };
    const int origins[][2] = { {0, 0}, {3, -5}, {-7, 11} };
    const uint32_t texels[2][2] = {
        {0x23456789, 0x91b3d5f7}, {0x17395b7d, 0x8caec0e2},
    };
    QTestState *s = start_indexed_state();

    for (unsigned scene = 0; scene < ARRAY_SIZE(scenes); scene++) {
        for (unsigned traversal = 0; traversal < 4; traversal++) {
            for (unsigned reverse = 0; reverse < 2; reverse++) {
                for (unsigned origin = 0; origin < ARRAY_SIZE(origins);
                     origin++) {
                    for (unsigned clipped = 0; clipped < 2; clipped++) {
                        unsigned order[] = {
                            0, reverse ? 2 : 1, reverse ? 1 : 2
                        };
                        unsigned selected = order[traversal & 2 ? 0 : 2];
                        int ax = scenes[scene][selected][0];
                        int ay = scenes[scene][selected][1];
                        int vertices[3][2];
                        Shape c = triangle_prepare(s, 0, 0, 0);
                        uint32_t expected[64];
                        int ox = origins[origin][0], oy = origins[origin][1];

                        ax = ax / 64 - (ax % 64 < 0);
                        ay = ay / 64 - (ay % 64 < 0);
                        for (unsigned v = 0; v < 3; v++) {
                            vertices[v][0] = scenes[scene][v][0] - 64 * ox;
                            vertices[v][1] = scenes[scene][v][1] - 64 * oy;
                        }
                        qtest_writel(s, DE + 0x58,
                            (uint32_t)(uint16_t)(-4096 + ox) << 16 |
                            (uint16_t)(-4096 + oy));
                        triangle_vertices(s, vertices, order, true);
                        texture_two_by_two(s);
                        qtest_writel(s, DE + 0x60,
                                     0x02000000 | traversal << 16);
                        qtest_writeq(s, DE + 0x120, (INT64_C(1) << 43) +
                                     ax * (INT64_C(1) << 41) + 0x800);
                        qtest_writeq(s, DE + 0x128, (INT64_C(1) << 43) +
                                     ay * (INT64_C(1) << 41) + 0x800);
                        qtest_writeq(s, DE + 0x138, INT64_C(1) << 41);
                        qtest_writeq(s, DE + 0x150, INT64_C(1) << 41);
                        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000 |
                                     (clipped ? 0x100000 : 0));
                        qtest_writeq(s, DE + 0x48,
                            bounds(4098 - ox, 4098 - oy, 4100 - ox, 4100 - oy));
                        for (unsigned y = 0; y < 8; y++) {
                            for (unsigned x = 0; x < 8; x++) {
                                bool covered = rows[scene][y] & (1U << x);
                                covered &= !clipped ||
                                    (x >= 2 && x <= 3 && y >= 2 && y <= 3);
                                expected[y * 8 + x] = covered ?
                                    texels[y >= 2][x >= 2] : c.old;
                            }
                        }
                        g_test_message("scene %u traversal %u winding %u "
                                       "origin %u clip %u", scene, traversal,
                                       reverse, origin, clipped);
                        g_assert_cmpint(memtx_writel(s, DE + 0x8d0,
                                                    TRIANGLE_COLOR), ==,
                                        MEMTX_OK);
                        shape_memory(s, &c, expected);
                    }
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_texture_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-texture-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *from = start(), *to = start_options("-incoming defer");
    Shape c = triangle_prepare(from, 0, 0, 0);
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS], translations[28];

    triangle_vertices(from, triangle_integer, NULL, true);
    texture_constant(from);
    qtest_writel(from, DE + 0x18, TRIANGLE_DRAW | 0x8000);
    g_assert_cmpint(memtx_writel(from, DE + 0x8d0, TRIANGLE_COLOR), ==,
                    MEMTX_OK);
    for (unsigned i = 0; i < ARRAY_SIZE(translations); i++) {
        translations[i] = qtest_readq(from, RE + 0x1600 + 8 * i);
    }
    save_paired_renderer(from, path, before);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s} }", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    save_paired_renderer(to, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    for (unsigned i = 0; i < ARRAY_SIZE(translations); i++) {
        g_assert_cmphex(qtest_readq(to, RE + 0x1600 + 8 * i), ==,
                        translations[i]);
    }
    triangle_pixels(to, &c, triangle_ten, 0x2371b9d5, 3, UINT32_MAX);
    qtest_writel(to, 0xafc00, 0x7193b5d7);
    g_assert_cmpint(memtx_writel(to, DE + 0x8d0, 0x12345678), ==, MEMTX_OK);
    triangle_pixels(to, &c, triangle_ten, 0x7193b5d7, 3, UINT32_MAX);
    qtest_system_reset(to);
    save_renderer(to, path, after);
    for (unsigned i = 0; i < RENDER_WORDS; i++) {
        g_assert_cmphex(after[i], ==, 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(translations); i++) {
        g_assert_cmphex(qtest_readq(to, RE + 0x1600 + 8 * i), ==, 0);
    }
    triangle_pixels(to, &c, triangle_ten, 0x7193b5d7, 3, UINT32_MAX);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_texture_border_components(void)
{
    const unsigned formats[][2] = {
        {1, 1}, {1, 2}, {2, 1}, {2, 2}, {4, 1},
        {5, 1}, {6, 1}, {7, 1}, {8, 1},
    };
    QTestState *s = start_indexed_state();
    for (unsigned n = 0; n < ARRAY_SIZE(formats); n++) {
        unsigned type = formats[n][0], depth = formats[n][1];
        for (unsigned function = 0; function < 4; function++) {
            Shape c = triangle_prepare(s, 0, 0, 0);
            uint32_t mode = type << 20 | depth << 18 | 10U << 14 |
                            3U << 4 | 2U << 2 | function;
            bool undefined = function == 1 && type >= 5;
            unsigned expected_type = type == 4 ? 2 : type;
            unsigned expected_depth = type >= 5 ? 1 : 2;
            uint32_t expected_raw = type >= 5 ? 0x17c9 : 0x175badc9;
            triangle_vertices(s, triangle_integer, NULL, true);
            texture_constant(s);
            qtest_writeq(s, RE + 0x1600 + (93 / 4) * 8, 0);
            qtest_writel(s, DE + 0x110, mode);
            qtest_writeq(s, DE + 0x120, 0x800);
            qtest_writel(s, DE + 0x160, 0x175badc9);
            qtest_writel(s, DE + 0x168, 0x39c765a1);
            qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000);
            g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==,
                            undefined ? MEMTX_ERROR : MEMTX_OK);
            if (undefined) {
                shape_memory(s, &c, NULL);
            } else {
                triangle_pixels(s, &c, triangle_ten,
                    texture_environment(expected_type, expected_depth,
                                        function, expected_raw),
                    3, UINT32_MAX);
            }
        }
    }
    qtest_quit(s);
}

static void test_texture_height_one(void)
{
    QTestState *s = start_indexed_state();
    for (unsigned wrap = 0; wrap < 4; wrap++) {
        for (unsigned linear = 0; linear < 2; linear++) {
            for (int t = -1; t <= 2; t++) {
                Shape c = triangle_prepare(s, 0, 0, 0);
                uint32_t expected = 0x2371b9d5;
                triangle_vertices(s, triangle_integer, NULL, true);
                texture_constant(s);
                qtest_writel(s, DE + 0x110, (2U << 20) | (2U << 18) |
                             (10U << 14) | linear << 7 | linear << 6 |
                             2U << 4 | wrap << 2 | 3U);
                qtest_writel(s, DE + 0x160, 0x175badc9);
                qtest_writeq(s, DE + 0x128,
                             ((int64_t)t + 1) * (INT64_C(1) << 43) + 0x800);
                if (wrap == 3 && (t < 0 || t > 1)) {
                    expected = 0x175badc9;
                } else if (linear && (wrap == 0 || wrap == 3)) {
                    expected = 0x1d66b3cf;
                }
                qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000);
                g_test_message("height one: T=%d wrap=%u linear=%u", t,
                               wrap, linear);
                g_assert_cmpint(memtx_writel(s, DE + 0x8d0, TRIANGLE_COLOR), ==,
                                MEMTX_OK);
                triangle_pixels(s, &c, triangle_ten, expected,
                                3, UINT32_MAX);
            }
        }
    }
    qtest_quit(s);
}

static void test_gl_line_first(void)
{
    QTestState *s = start_indexed_state();
    Shape c = pipeline_prepare(s, 0x228, 0);
    uint32_t expected[16];

    qtest_writel(s, DE + 0x60, 0x01000020);
    qtest_writel(s, DE + 0x80, 64);
    qtest_writel(s, DE + 0x84, 2 * 64);
    qtest_writel(s, DE + 0x88, 4 * 64);
    qtest_writel(s, DE + 0x8c, 2 * 64);
    qtest_writeq(s, DE + 0x90, UINT64_MAX);
    qtest_writel(s, DE + 0xd0, 0x13579bdf);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            expected[y * 4 + x] = y == 2 && x >= 1 ? 0x13579bdf : c.old;
        }
    }
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    shape_memory(s, &c, expected);
    qtest_quit(s);
}

static void test_gl_line_pair_staging(void)
{
    const unsigned pairs[] = { 0xe0, 0xe8, 0xf0, 0xf8, 0x100, 0x108, 0x190 };
    g_autofree char *dir = g_dir_make_tmp("crime-line-pairs-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    uint64_t raw[RENDER_WORDS];
    Shape c = pipeline_prepare(s, 0x228, 0);
    uint32_t old[16];

    qtest_writel(s, DE + 0x60, 0x01000020);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x200);
    qtest_writel(s, DE + 0x1b0, 6);
    qtest_writel(s, DE + 0xd0, 0x13579bff);
    qtest_writeq(s, DE + 0x80, UINT64_C(64) << 32 | 128);
    qtest_writeq(s, DE + 0x88, UINT64_C(256) << 32 | 128);
    for (unsigned n = 0; n < 16; n++) {
        old[n] = c.old;
    }
    for (unsigned n = 0; n < ARRAY_SIZE(pairs); n++) {
        uint64_t value = 0xa579b3e16c28f04dULL ^ (UINT64_C(0x123456789) * n);

        unsigned head = ifb_next(s);

        g_assert_cmpint(memtx_writeq(s, DE + pairs[n], value), ==, MEMTX_OK);
        ifb_head(s, (head + 1) & 63);
        shape_memory(s, &c, old);
        g_assert_cmphex(qtest_readq(s, CRIME + 0x10), ==, 0);
        save_paired_renderer(s, path, raw);
        g_assert_cmphex(raw[192 + pairs[n] / 4], ==, value >> 32);
        g_assert_cmphex(raw[192 + pairs[n] / 4 + 1], ==, (uint32_t)value);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_line_aa_horizontal(void)
{
    QTestState *s = start_indexed_state();
    Shape c = pipeline_prepare(s, 0x228, 0);
    uint32_t expected[16];

    qtest_writel(s, DE + 0x60, 0x01000000);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x1200);
    qtest_writel(s, DE + 0x1b0, 6);
    qtest_writel(s, DE + 0x80, 0);
    qtest_writel(s, DE + 0x84, 64);
    qtest_writel(s, DE + 0x88, 3 * 64);
    qtest_writel(s, DE + 0x8c, 64);
    qtest_writel(s, DE + 0xd0, 0x13579bff);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned alpha = x == 0 || x == 3 ? 127 : 254;

            if (y != 1) {
                alpha = x == 0 || x == 3 ? 63 : 127;
            }
            expected[4 * y + x] = y < 3 ?
                c.old ^ (0x13579b00 | alpha) : c.old;
        }
    }
    g_assert_cmpint(memtx_writeq(s, DE + 0x990,
                                0x0000400000002020ULL), ==, MEMTX_OK);
    shape_memory(s, &c, expected);
    qtest_quit(s);
}


static void gl_line_vertices(QTestState *s, const int a[2], const int b[2],
                                int ox, int oy, uint32_t prefix)
{
    const int *vertices[2] = { a, b };
    const int origins[2] = { ox, oy };

    qtest_writel(s, DE + 0x58, xy((uint16_t)ox, (uint16_t)oy));
    for (unsigned v = 0; v < 2; v++) {
        uint32_t raw[2];

        for (unsigned axis = 0; axis < 2; axis++) {
            int field = vertices[v][axis] - 64 * origins[axis];

            g_assert_cmpint(field, >=, 0);
            g_assert_cmpint(field, <=, 0x7ffff);
            raw[axis] = prefix + field;
        }
        qtest_writeq(s, DE + 0x80 + 8 * v, (uint64_t)raw[0] << 32 | raw[1]);
    }
}

static void test_gl_line_nonaa_vectors(void)
{
    const uint32_t prefixes[] = { 0, 0x00800000, 0x48000000 };
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(gl_nonaa_vectors); n++) {
        const GLNonAAVector *v = &gl_nonaa_vectors[n];
        Shape c = pipeline_prepare(s, 0x228, n % 3);
        uint32_t expected[256];

        c.width = c.height = 16;
        map_region(s, c.bank, 4, 0, 0, 15, 15);
        shape_memory(s, &c, NULL);
        gl_line_vertices(s, v->a, v->b, -4096, -4096, prefixes[n % 3]);
        qtest_writel(s, DE + 0x60, 0x01000000 | v->halfwidth |
                    (n % 4) << 16 | (n & 1) << 18);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x200);
        qtest_writel(s, DE + 0x1b0, 6);
        qtest_writel(s, DE + 0xd0, 0x13579bdf);
        qtest_writeq(s, DE + 0x190, UINT64_MAX);
        qtest_writeq(s, DE + 0x90, UINT64_MAX);
        qtest_writel(s, DE + 0x50, UINT32_MAX);
        for (unsigned y = 0; y < 16; y++) {
            for (unsigned x = 0; x < 16; x++) {
                expected[16 * y + x] = v->rows[y] & (1U << x) ?
                                      c.old ^ 0x13579bdf : c.old;
            }
        }
        g_test_message("non-AA vector %u, H=%u", n, v->halfwidth);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
}

static QTestState *gl_line_numeric_start(void)
{
    return start_profile_ram("legacy-software", false, true, false,
        "-device sgi-crime-test-fixture,id=inject,crime=/machine/peripheral/crime");
}

static void gl_line_aa_program(QTestState *s, const int a[2], const int b[2],
                                  int ox, int oy, int slope, unsigned ideal,
                                  unsigned cs, unsigned ce)
{
    setup(s, 0x228, false);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x1000);
    qtest_writel(s, DE + 0x60, 0x01000020);
    gl_line_vertices(s, a, b, ox, oy, 0);
    qtest_writeq(s, DE + 0x190,
        (uint64_t)((uint32_t)(uint16_t)slope << 16 | ideal) << 32 |
        (cs << 8) | ce);
}

static uint64_t gl_line_area_bits(QTestState *s, int x, int y)
{
    uint64_t point = (uint64_t)(uint32_t)x << 32 | (uint32_t)y;
    g_autoptr(QDict) reply = NULL;

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
        "'property':'line-sample','value':%" PRIu64 "}}", point);
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/inject',"
        "'property':'line-area'}}");
    g_assert_true(qdict_haskey(reply, "return"));
    return qdict_get_int(reply, "return");
}

static unsigned gl_line_area_code(uint64_t bits)
{
    double area;

    memcpy(&area, &bits, sizeof(area));
    return MIN(255, (unsigned)(256 * area));
}

static void test_gl_line_precision(void)
{
    QTestState *s = gl_line_numeric_start();

    for (unsigned n = 0; n < ARRAY_SIZE(gl_aa_vectors); n++) {
        const GLAAVector *v = &gl_aa_vectors[n];
        uint64_t bits;
        double area;
        unsigned encoded;

        gl_line_aa_program(s, v->a, v->b, v->origin[0], v->origin[1],
                           v->slope, v->ideal, v->cs, v->ce);
        bits = gl_line_area_bits(s, v->x, v->y);
        memcpy(&area, &bits, sizeof(area));
        encoded = gl_line_area_code(bits);
        g_test_message("AA sample %u at (%d,%d), q14=%d, %.17g vs %.17g",
                       n, v->x, v->y, v->slope, area, v->area);
        g_assert_cmpfloat(fabs(area - v->area), <=, 1.0 / 4294967296.0);
        if (v->boundary) {
            g_assert_cmpint(abs((int)encoded - (int)v->encoded), <=, 1);
        } else {
            g_assert_cmpuint(encoded, ==, v->encoded);
        }
    }
    qtest_quit(s);
}

static int64_t gl_line_reference_floor(int64_t n, int64_t d)
{
    return n < 0 ? -((-n + d - 1) / d) : n / d;
}

static void test_gl_line_canonical(void)
{
    const int translations[][2] = { {1, 3}, {-32768, -32768}, {32767, 32767} };
    QTestState *s = gl_line_numeric_start();

    for (unsigned n = 0; n < ARRAY_SIZE(gl_aa_vectors); n++) {
        const GLAAVector *v = &gl_aa_vectors[n];
        unsigned major = abs(v->b[0] - v->a[0]) >=
                         abs(v->b[1] - v->a[1]) ? 0 : 1;
        unsigned minor = 1 - major;
        int d = v->b[major] > v->a[major] ? 1 : -1;
        int64_t k0 = d * gl_line_reference_floor(d * v->a[major] + 32, 64);
        int64_t reverse_k0 = -d * gl_line_reference_floor(
            -d * v->b[major] + 32, 64);
        unsigned reverse_ideal =
            (v->ideal + v->slope * (reverse_k0 - k0)) & 65535;
        uint64_t bits;

        gl_line_aa_program(s, v->a, v->b, v->origin[0], v->origin[1],
                           v->slope, v->ideal, v->cs, v->ce);
        bits = gl_line_area_bits(s, v->x, v->y);
        gl_line_aa_program(s, v->b, v->a, v->origin[0], v->origin[1],
                           v->slope, reverse_ideal, v->ce, v->cs);
        g_test_message("canonical AA sample %u", n);
        g_assert_cmphex(gl_line_area_bits(s, v->x, v->y), ==, bits);
        {
            const int change[2] = { -1, -3 };
            unsigned ideal = (v->ideal - 16384 * change[minor]) & 65535;

            gl_line_aa_program(s, v->a, v->b, v->origin[0] + change[0],
                v->origin[1] + change[1], v->slope, ideal, v->cs, v->ce);
            g_assert_cmphex(gl_line_area_bits(s, v->x, v->y), ==, bits);
        }
        for (unsigned t = 0; t < ARRAY_SIZE(translations); t++) {
            int a[2], b[2], pixel[2] = { v->x, v->y };
            int origin[2];

            for (unsigned axis = 0; axis < 2; axis++) {
                int delta = translations[t][axis] - v->origin[axis];

                a[axis] = v->a[axis] + 64 * delta;
                b[axis] = v->b[axis] + 64 * delta;
                pixel[axis] += delta;
                origin[axis] = translations[t][axis];
            }
            gl_line_aa_program(s, a, b, origin[0], origin[1], v->slope,
                               v->ideal, v->cs, v->ce);
            g_assert_cmphex(gl_line_area_bits(s, pixel[0], pixel[1]), ==, bits);
        }
        {
            int b[2] = { v->b[0], v->b[1] };
            const int origin[2] = { -4096, -4096 };
            int change = origin[minor] - v->origin[minor];
            unsigned ideal = (v->ideal - 16384 * change) & 65535;
            uint64_t extended;

            b[major] += d * 4 * 64;
            gl_line_aa_program(s, v->a, b, origin[0], origin[1], v->slope,
                               ideal, v->cs, v->ce);
            extended = gl_line_area_bits(s, v->x, v->y);
            g_assert_cmpuint(gl_line_area_code(extended), >=,
                             gl_line_area_code(bits));
        }
    }
    qtest_quit(s);
}

static uint32_t gl_line_reference_color(const int *start, const int *dx,
                                         const int *dy, const int anchor[2],
                                         int x, int y)
{
    uint32_t color = 0;

    for (unsigned k = 0; k < 4; k++) {
        int64_t n = 64LL * start[k] + (int64_t)dx[k] * (64 * x - anchor[0]) +
                    (int64_t)dy[k] * (64 * y - anchor[1]);
        int64_t v = gl_line_reference_floor(n, 64 * 4096);

        color |= CLAMP(v, 0, 255) << (24 - 8 * k);
    }
    return color;
}

static void gl_line_shade_pairs(QTestState *s, const int *start,
                                  const int *dx, const int *dy)
{
    const unsigned bases[] = { 0xe0, 0xe8, 0xf0, 0xf8, 0x100, 0x108 };
    const int words[6][2] = {
        { start[0], start[1] }, { start[2], start[3] },
        { dx[0], dx[1] }, { dy[0], dy[1] },
        { dx[2], dx[3] }, { dy[2], dy[3] },
    };

    for (unsigned n = 0; n < ARRAY_SIZE(bases); n++) {
        qtest_writeq(s, DE + bases[n],
            (uint64_t)(uint32_t)words[n][0] << 32 | (uint32_t)words[n][1]);
    }
}

static void test_gl_line_output(gconstpointer selection)
{
    const unsigned modes[] = {
        0x228, 0x218, 0x124, 0x224, 0x225, 0x226, 0x227
    };
    const unsigned widths[] = { 0, 1, 16, 32, 65, 65535 };
    const int starts[] = { 100 * 4096, 80 * 4096, 60 * 4096, 128 * 4096 };
    const int dx[] = { 4096, -8192, 12288, -16384 };
    const int dy[] = { 8192, 4096, -4096, -8192 };
    unsigned mode = modes[GPOINTER_TO_UINT(selection)];
    QTestState *s = start_indexed_state();

    for (unsigned rop = 0; rop < 16; rop++) {
        for (unsigned mask = 0; mask < 16; mask++) {
            Shape c = pipeline_prepare(s, mode, (rop + mask) % 3);
            int a[2] = { 32, 80 }, b[2] = { 224, 80 }, loaded[4];
            uint32_t expected[16];
            bool transpose = mask & 2, smooth = mask & 4, reverse = rop & 4;
            unsigned draw = 0x411680 | mask << 3 | (rop & 1) << 8 |
                            (smooth ? 0x10000 : 0) | (mask & 1 ? 0x800 : 0);
            unsigned function = rop & 1 ? 0x45 : 0x89;
            uint32_t plane = mask & 8 ? 0xa579b3e1 : UINT32_MAX;

            /* The analytic coverage rectangle is [.5,3.5] by [.25,2.25]. */
            draw &= ~0x10000U;
            if (smooth) {
                draw |= 0x10000;
            }
            if (transpose) {
                a[0] = 80;
                a[1] = 32;
                b[0] = 80;
                b[1] = 224;
            }
            for (unsigned k = 0; k < 4; k++) {
                loaded[k] = starts[k] + (reverse ?
                    dx[k] * (b[0] - a[0]) / 64 +
                    dy[k] * (b[1] - a[1]) / 64 : 0);
            }
            if (rop & 1) {
                gl_line_shade_pairs(s, loaded, dx, dy);
            } else {
                pipeline_shade(s, loaded, dx, dy);
            }
            gl_line_vertices(s, reverse ? b : a, reverse ? a : b,
                             -4096, -4096, 0x48000000);
            qtest_writel(s, DE + 0x60, 0x01000000 | widths[mask % 6] |
                        (rop % 4) << 16 | (mask & 1) << 18);
            qtest_writel(s, DE + 0x18, draw);
            qtest_writel(s, DE + 0xd0, 0x13579b80);
            qtest_writel(s, DE + 0x1a0, 0x9b2371d6);
            qtest_writel(s, DE + 0x1a8, function);
            qtest_writel(s, DE + 0x1b0, rop);
            qtest_writel(s, DE + 0x1b8, plane);
            qtest_writel(s, DE + 0x198, 0x164);
            qtest_writeq(s, DE + 0x90, UINT64_MAX);
            qtest_writel(s, DE + 0x50, UINT32_MAX);
            qtest_writeq(s, DE + 0x190, 0x0000500000004040ULL);
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 4; x++) {
                    unsigned major = transpose ? y : x;
                    unsigned minor = transpose ? x : y;
                    unsigned coverage =
                        minor == 0 ? 64 : minor == 1 ? 255 : 192;
                    uint32_t source = smooth ? gl_line_reference_color(
                        starts, dx, dy, a, x, y) : 0x13579b80;

                    source = (source & 0xffffff00U) |
                             (source & 255) * coverage / 256;
                    expected[4 * y + x] =
                        major >= 1 && major <= 3 && minor < 3 &&
                        (!(draw & 0x800) || (source & 255) < 100) ?
                        pipeline_reference(mode, source, c.old, 0x9b2371d6,
                            function, draw, rop, plane, x, y) : c.old;
                }
            }
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            shape_memory(s, &c, expected);
        }
    }
    /* Coverage preserves transparent source-over identity. */
    {
        Shape c = pipeline_prepare(s, mode, 0);
        const int a[] = { 32, 80 }, b[] = { 224, 80 };
        uint32_t expected[16];

        gl_line_vertices(s, a, b, 0, 0, 0);
        qtest_writel(s, DE + 0x60, 0x01000000);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x1500);
        qtest_writel(s, DE + 0xd0, 0x3579b100);
        qtest_writel(s, DE + 0x1a8, 0x45);
        qtest_writeq(s, DE + 0x190, 0x0000500000004040ULL);
        for (unsigned i = 0; i < 16; i++) {
            expected[i] = c.old;
        }
        for (unsigned repeat = 0; repeat < 2; repeat++) {
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            shape_memory(s, &c, expected);
        }
    }
    qtest_quit(s);
}

static void test_gl_line_alpha_edges(void)
{
    QTestState *s = start_indexed_state();
    const int a[] = { 0, 64 }, b[] = { 192, 64 };
    Shape c = pipeline_prepare(s, 0x228, 0);

    gl_line_vertices(s, a, b, 0, 0, 0);
    qtest_writel(s, DE + 0x60, 0x01000000);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x1a00);
    qtest_writel(s, DE + 0x1b0, 6);
    qtest_writel(s, DE + 0xd0, 0x13579b80);
    qtest_writel(s, DE + 0x198, 0x141);
    qtest_writeq(s, DE + 0x190, 0x0000400000002020ULL);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    g_assert_cmphex(pixel_read(s, pixel_address(0, 0, 1, 4), 4),
                    ==, c.old ^ 0x13579b40);
    g_assert_cmphex(pixel_read(s, pixel_address(0, 1, 1, 4), 4), ==, c.old);
    /* A computed positive area may encode q=0 and still performs the ROP. */
    c = pipeline_prepare(s, 0x228, 0);
    gl_line_vertices(s, a, b, 0, 0, 0);
    qtest_writel(s, DE + 0x60, 0x01000000);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x1200);
    qtest_writel(s, DE + 0x1b0, 6);
    qtest_writel(s, DE + 0xd0, 0x13579bff);
    qtest_writeq(s, DE + 0x190, 0x00005fff00002020ULL);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    g_assert_cmphex(pixel_read(s, pixel_address(0, 1, 0, 4), 4),
                    ==, c.old ^ 0x13579b00);
    qtest_quit(s);
}

static Shape gl_line_prepare(QTestState *s, bool aa)
{
    Shape c = pipeline_prepare(s, 0x228, 0);
    const int a[] = { 0, 64 }, b[] = { 192, 64 };

    gl_line_vertices(s, a, b, 0, 0, 0);
    qtest_writel(s, DE + 0x60, 0x01000020);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x200 | (aa ? 0x1000 : 0));
    qtest_writel(s, DE + 0x1b0, 6);
    qtest_writel(s, DE + 0xd0, 0x13579bff);
    qtest_writeq(s, DE + 0x190, 0x0000400000002020ULL);
    return c;
}

static void test_gl_line_depth_stipple(void)
{
    QTestState *s = start_indexed_state();
    const int a[] = { 0, 64 }, b[] = { 8 * 64, 64 };

    for (unsigned reverse = 0; reverse < 2; reverse++) {
        Shape c = gl_line_prepare(s, false);
        SZCase v = sz_case();
        uint32_t expected[48];

        c.width = 12;
        map_region(s, 0, 4, 0, 0, 11, 3);
        shape_memory(s, &c, NULL);
        sz_initialize(s, &c, v.old);
        v.draw = TRIANGLE_DRAW | 0x80000 | 7;
        v.stencil = 0x62ff7002;
        v.dx = 4096;
        v.dy = 8192;
        v.start += reverse ? 8 * v.dx : 0;
        sz_program(s, &v);
        gl_line_vertices(s, reverse ? b : a, reverse ? a : b, 0, 0, 0);
        qtest_writel(s, DE + 0x60, 0x01000040);
        qtest_writeq(s, DE + 0xc0, UINT64_C(0x02040102) << 32 | 0x28000000);
        for (unsigned repetition = 0; repetition < 2; repetition++) {
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 12; x++) {
                    int i = reverse ? 7 - (int)x : (int)x;
                    bool selected = i >= 0 && i < 8 &&
                                    (i < 2 || i >= 5) && (y == 1 || y == 2);
                    uint32_t z = 0x62000000 | (18 + x + 2 * ((int)y - 1));

                    expected[12 * y + x] = selected ? v.color : c.old;
                    if (repetition) {
                        g_assert_cmphex(pixel_read(s,
                            pixel_address(2, x, y, 4), 4), ==,
                            selected ? z : v.old);
                    }
                }
            }
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0xdeadbeef),
                            ==, MEMTX_OK);
            shape_memory(s, &c, expected);
            g_assert_cmphex(qtest_readl(s, DE + 0xc0), ==, 0x02040102);
            g_assert_cmphex(qtest_readl(s, DE + 0xc4), ==, 0x28000000);
        }
    }
    qtest_quit(s);
}

static void test_gl_line_pairs_atomic(void)
{
    const unsigned pairs[] = { 0xe0, 0xe8, 0xf0, 0xf8, 0x100, 0x108, 0x190 };
    const uint64_t invalid[] = {
        0x4001400000002020ULL, 0xbfff400000002020ULL,
        0x0000400000000020ULL, 0x0000400000002000ULL,
        0x0000400000004120ULL, 0x0000400000002041ULL,
        0x000040000000a020ULL,
    };
    g_autofree char *dir = g_dir_make_tmp("crime-gl-pair-atomic-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    uint64_t value;

    for (unsigned n = 0; n < ARRAY_SIZE(pairs); n++) {
        Shape c = gl_line_prepare(s, true);

        for (unsigned width = 1; width <= 2; width *= 2) {
            pipeline_failure(s, &c, path, DE + pairs[n], 0, width);
            pipeline_failure(s, &c, path, DE + 0x800 + pairs[n], 0, width);
        }
        for (unsigned delta = 1; delta < 8; delta++) {
            pipeline_failure(s, &c, path, DE + pairs[n] + delta,
                             UINT64_MAX, 8);
            pipeline_failure(s, &c, path, DE + 0x800 + pairs[n] + delta,
                             UINT64_MAX, 8);
        }
        if (pairs[n] != 0x190) {
            pipeline_failure(s, &c, path, DE + 0x800 + pairs[n],
                             UINT64_MAX, 8);
        }
        for (unsigned width = 4; width <= 8; width *= 2) {
            g_assert_cmpint(memtx_read(s, DE + pairs[n], &value, width),
                            !=, MEMTX_OK);
            g_assert_cmpint(memtx_read(s, DE + 0x800 + pairs[n], &value, width),
                            !=, MEMTX_OK);
        }
    }
    for (unsigned n = 0; n < ARRAY_SIZE(invalid); n++) {
        for (unsigned empty = 0; empty < 2; empty++) {
            Shape c = gl_line_prepare(s, true);

            if (empty) {
                qtest_writeq(s, DE + 0x88, 64);
                qtest_writel(s, DE + 0x18, 0x401000);
            }
            pipeline_failure(s, &c, path, DE + 0x990, invalid[n], 8);
        }
    }
    for (unsigned nonaa = 0; nonaa < 2; nonaa++) {
        for (unsigned late = 0; late < 2; late++) {
            Shape c = gl_line_prepare(s, !nonaa);
            const int a[] = { 16, 64 }, b[] = { 272, 64 };
            int starts[4], dx[4], dy[4] = { 0, 0, 0, 0 };

            gl_line_vertices(s, a, b, 0, 0, 0);
            qtest_writel(s, DE + 0x18,
                0x510278 | (nonaa ? 0 : 0x1000));
            qtest_writeq(s, DE + 0x48, bounds(1, 0, 2, 4));
            for (unsigned k = 0; k < 4; k++) {
                starts[k] = late ? (1 << 20) - 2 : -(1 << 20);
                dx[k] = 1;
            }
            pipeline_shade(s, starts, dx, dy);
            pipeline_failure(s, &c, path, DE + 0x990,
                             0x0000400000002020ULL, 8);
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_line_failures(void)
{
    const unsigned unsupported[] = {
        1, 2, 4, 0x2000, 0x4000, 0x8000, 0x80000, 0x200000,
    };
    g_autofree char *dir = g_dir_make_tmp("crime-gl-line-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(unsupported); n++) {
        Shape c = gl_line_prepare(s, true);

        qtest_writel(s, DE + 0x18, 0x401000 | unsupported[n]);
        pipeline_failure(s, &c, path, DE + 0x990, 0x0000400000002020ULL, 8);
    }
    for (unsigned bad = 0; bad < 5; bad++) {
        Shape c = gl_line_prepare(s, true);

        switch (bad) {
        case 0:
            qtest_writel(s, DE + 8, 0x234);
            break;
        case 1:
            qtest_writel(s, DE + 0x18, 0x401478);
            qtest_writel(s, DE + 0x1a8, 0x1000);
            break;
        case 2:
            qtest_writel(s, DE + 0x18, 0x401878);
            qtest_writel(s, DE + 0x198, 0x800);
            break;
        case 3:
            qtest_writel(s, DE + 0x80, 0x7fc00000);
            break;
        case 4:
            qtest_writel(s, DE + 0x60, 0x01080020);
            break;
        }
        pipeline_failure(s, &c, path, DE + 0x990, 0x0000400000002020ULL, 8);
    }
    /* Bad final RAM mappings cannot retire the earlier line fragments. */
    for (unsigned aa = 0; aa < 2; aa++) {
        Shape c = gl_line_prepare(s, aa);
        const int a[] = { 126 * 64, 127 * 64 }, b[] = { 131 * 64, 127 * 64 };
        unsigned index = aa ? 17 : 1;

        c.x = 125;
        c.y = 125;
        c.width = c.height = 8;
        map_region(s, 0, 4, 125, 125, 132, 132);
        shape_memory(s, &c, NULL);
        gl_line_vertices(s, a, b, 0, 0, 0);
        qtest_writeq(s, DE + 0x190, 0x0000c00000002020ULL);
        tile(s, 0, index, 0x7fff);
        pipeline_failure(s, &c, path, DE + 0x990, 0x0000c00000002020ULL, 8);
    }
    /* A one-cell packet consumes cap fractions as endpoints. */
    {
        Shape c = gl_line_prepare(s, true);
        const int a[] = { 6, 64 }, b[] = { 13, 64 };

        gl_line_vertices(s, a, b, 0, 0, 0);
        pipeline_failure(s, &c, path, DE + 0x990, 0x0000400000000101ULL, 8);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_line_color_anchor(void)
{
    const int starts[] = { 100 * 4096, 80 * 4096, 60 * 4096, 128 * 4096 };
    const int dx[] = { 4096, -8192, 12288, -16384 };
    const int dy[] = { 8192, 4096, -4096, -8192 };
    const int a[] = { 32, 80 }, b[] = { 224, 80 };
    const unsigned caps[] = { 0x4040, 0x2020, 0x0140 };
    const int slopes[] = { 0, 8192, -8192 };
    QTestState *s = start_indexed_state();

    for (unsigned cap = 0; cap < ARRAY_SIZE(caps); cap++) {
        for (unsigned slope = 0; slope < ARRAY_SIZE(slopes); slope++) {
            for (unsigned clipped = 0; clipped < 2; clipped++) {
                Shape c = pipeline_prepare(s, 0x218, 0);
                uint32_t expected = gl_line_reference_color(starts, dx, dy,
                                                           a, 2, 1);

                gl_line_vertices(s, a, b, -4096, -4096, 0);
                pipeline_shade(s, starts, dx, dy);
                qtest_writel(s, DE + 0x60, 0x01000020);
                qtest_writel(s, DE + 0x18, 0x411078 | clipped << 20);
                qtest_writel(s, DE + 0xd0, cap * 0x13579bdf);
                qtest_writeq(s, DE + 0x90, UINT64_MAX);
                qtest_writeq(s, DE + 0x48, bounds(4098, 4096, 4100, 4100));
                qtest_writeq(s, DE + 0x190,
                    (uint64_t)((uint32_t)(uint16_t)slopes[slope] << 16 |
                               20480) << 32 |
                    caps[cap]);
                g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
                g_assert_cmphex(pixel_read(s, pixel_address(0, 2, 1, 4), 4),
                                ==, (expected & 0xffffff00U) | (c.old & 255));
                if (clipped) {
                    g_assert_cmphex(pixel_read(s, pixel_address(0, 1, 1, 4), 4),
                                    ==, c.old);
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_gl_line_migration(void)
{
    const unsigned pairs[] = { 0xe0, 0xe8, 0xf0, 0xf8, 0x100, 0x108 };
    g_autofree char *dir = g_dir_make_tmp("crime-gl-line-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *from = start(), *to = start_options("-incoming defer");
    Shape c = gl_line_prepare(from, true);
    uint32_t expected[16], old[16];
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];

    for (unsigned n = 0; n < ARRAY_SIZE(pairs); n++) {
        qtest_writeq(from, DE + pairs[n], 0xa579b3e16c28f04dULL ^ n);
    }
    g_assert_cmpint(memtx_writeq(from, DE + 0x990, 0x0000400000002020ULL),
                    ==, MEMTX_OK);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned alpha = y == 1 ? (x == 0 || x == 3 ? 127 : 254) :
                                     (x == 0 || x == 3 ? 63 : 127);

            expected[4 * y + x] = y < 3 ?
                c.old ^ (0x13579b00 | alpha) : c.old;
            old[4 * y + x] = c.old;
        }
    }
    qtest_writeq(from, DE + 0x190, 0x4001000000000000ULL);
    save_paired_renderer(from, path, before);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    save_paired_renderer(to, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    shape_memory(to, &c, expected);
    g_assert_cmpint(memtx_writeq(to, DE + 0x990, 0x0000400000002020ULL),
                    ==, MEMTX_OK);
    shape_memory(to, &c, old);
    qtest_system_reset(to);
    shape_memory(to, &c, old);
    save_renderer(to, path, after);
    for (unsigned n = 0; n < 12; n++) {
        g_assert_cmphex(after[192 + (0xe0 + 4 * n) / 4], ==, 0);
    }
    g_assert_cmphex(after[192 + 0x190 / 4], ==, 0);
    g_assert_cmphex(after[192 + 0x194 / 4], ==, 0);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_unlink(path);
    g_rmdir(dir);
}

typedef struct TextureStartSnapshot {
    uint64_t words[RENDER_WORDS], texture[28], global[7];
    IfbSnapshot ifb;
    uint32_t status;
    unsigned irq;
} TextureStartSnapshot;

static QTestState *texture_start_machine(void)
{
    QTestState *s = start_indexed_state();
    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    triangle_irq_baseline(s);
    return s;
}

static void texture_start_snapshot(QTestState *s, const char *path,
                                    TextureStartSnapshot *state)
{
    const unsigned global[] = {0x10, 0x18, 0x20, 0x40, 0x48, 0x250, 0x258};
    memset(state, 0, sizeof(*state));
    save_paired_renderer(s, path, state->words);
    for (unsigned i = 0; i < ARRAY_SIZE(state->texture); i++) {
        state->texture[i] = qtest_readq(s, RE + 0x1600 + 8 * i);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(global); i++) {
        state->global[i] = qtest_readq(s, CRIME + global[i]);
    }
    ifb_snapshot(s, &state->ifb);
    state->status = qtest_readl(s, STATUS);
    for (unsigned i = 0; i < 5; i++) {
        state->irq |= qtest_get_irq(s, i) << i;
    }
}

static void texture_start_pixels(QTestState *s, const Shape *c,
                                  uint32_t pixels[64])
{
    g_assert_cmpuint(c->width * c->height, <=, 64);
    for (unsigned y = 0; y < c->height; y++) {
        for (unsigned x = 0; x < c->width; x++) {
            pixels[y * c->width + x] = pixel_read(s,
                pixel_address(c->bank, c->x + x, c->y + y, 4), 4);
        }
    }
}

static void texture_start_failure(QTestState *s, const Shape *c,
                                   const char *path, uint64_t address,
                                   uint64_t value, unsigned width, bool read)
{
    TextureStartSnapshot before, after;
    uint32_t pixels[64];
    g_autofree uint8_t *source = g_malloc(65536);
    g_autofree uint8_t *next = g_malloc(65536);
    uint64_t ignored;

    texture_start_snapshot(s, path, &before);
    texture_start_pixels(s, c, pixels);
    qtest_memread(s, 0xa0000, source, 65536);
    g_assert_cmpint(read ? memtx_read(s, address, &ignored, width) :
                          memtx_write(s, address, value, width), !=, MEMTX_OK);
    texture_start_snapshot(s, path, &after);
    g_assert_cmpmem(&before, sizeof(before), &after, sizeof(after));
    shape_memory(s, c, pixels);
    qtest_memread(s, 0xa0000, next, 65536);
    g_assert_cmpmem(source, 65536, next, 65536);
}

static Shape texture_start_prepare(QTestState *s)
{
    const uint64_t planes[] = {
        (UINT64_C(5) << 41) + 0x800, (UINT64_C(5) << 41) + 0x800,
        0x20000800, UINT64_C(1) << 40, UINT64_C(1) << 38,
        UINT64_C(1) << 38, UINT64_C(1) << 40, 0x00800000, 0x00400000,
    };
    Shape c = triangle_prepare(s, 0, 0, 0);
    triangle_vertices(s, triangle_integer, NULL, true);
    texture_two_by_two(s);
    for (unsigned i = 0; i < ARRAY_SIZE(texture_plane_starts); i++) {
        const TexturePlaneStart *r = &texture_plane_starts[i];
        g_assert_cmpint(memtx_write(s, DE + r->offset, planes[i], r->width),
                        ==, MEMTX_OK);
    }
    qtest_writel(s, DE + 0xd0, TRIANGLE_COLOR);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000 | 0x200);
    qtest_writel(s, DE + 0x1b0, 6);
    return c;
}

static uint64_t texture_start_invalid(const TexturePlaneStart *r)
{
    return r->width == 8 ? INT64_MAX : r->offset == 0x130 ? 0 : INT32_MAX;
}

static void texture_start_published(TextureStartSnapshot *state,
                                      const TexturePlaneStart *r,
                                      uint64_t value)
{
    unsigned head = state->status & 63, next = (head + 1) & 63;
    uint64_t payload = r->width == 8 ? value : r->offset == 0x15c ?
                       (uint32_t)value : (uint64_t)(uint32_t)value << 32;

    state->words[192 + r->offset / 4] = value;
    stq_be_p(state->ifb.ram + head * 8, payload);
    stq_be_p(state->ifb.ram + 0x200 + head * 8, (uint64_t)r->header << 32);
    state->status = (state->status & ~0x3ffffU) |
                    next | (next << 6) | (next << 12);
    state->ifb.status = state->status;
}

static void test_texture_start_matrix(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-texture-matrix-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = texture_start_machine();

    for (unsigned i = 0; i < ARRAY_SIZE(texture_plane_starts); i++) {
        const TexturePlaneStart *r = &texture_plane_starts[i];
        for (unsigned invalid_old = 0; invalid_old < 2; invalid_old++) {
            TextureStartSnapshot expected, actual, before;
            uint32_t pixels[64];
            Shape c = texture_start_prepare(s);

            g_assert_cmpint(memtx_write(s, DE + r->offset, r->value, r->width),
                            ==, MEMTX_OK);
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            texture_start_snapshot(s, path, &expected);
            texture_start_pixels(s, &c, pixels);
            c = texture_start_prepare(s);
            if (invalid_old) {
                g_assert_cmpint(memtx_write(s, DE + r->offset,
                    texture_start_invalid(r), r->width), ==, MEMTX_OK);
            }
            texture_start_snapshot(s, path, &before);
            g_test_message("plane 0x%x width %u invalid old %u", r->offset,
                           r->width, invalid_old);
            g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset,
                                        r->value, r->width), ==, MEMTX_OK);
            shape_memory(s, &c, pixels);
            texture_start_snapshot(s, path, &actual);
            g_assert_cmpmem(expected.words, sizeof(expected.words),
                            actual.words, sizeof(actual.words));
            g_assert_cmpmem(expected.texture, sizeof(expected.texture),
                            actual.texture, sizeof(actual.texture));
            texture_start_published(&before, r, r->value);
            g_assert_cmpmem(&before, sizeof(before), &actual, sizeof(actual));
            for (unsigned poll = 0; poll < 3; poll++) {
                qtest_readl(s, STATUS);
                qtest_readq(s, RE + 0x1600 + (93 / 4) * 8);
                qtest_writel(s, DE + 0x1f0, 0);
                qtest_writel(s, DE + 0x1f8, 0);
            }
            g_assert_cmpint(memtx_write(s, DE + r->offset,
                                        r->value ^ 0x1000, r->width), ==,
                            MEMTX_OK);
            shape_memory(s, &c, pixels);
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_texture_start_widths(gconstpointer selection)
{
    const TexturePlaneStart *r =
        &texture_plane_starts[GPOINTER_TO_UINT(selection)];
    g_autofree char *dir = g_dir_make_tmp("crime-texture-width-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = texture_start_machine();
    Shape c = texture_start_prepare(s);
    uint64_t address = DE + 0x800 + r->offset;

    for (unsigned width = 1; width <= 8; width *= 2) {
        texture_start_failure(s, &c, path, address, 0, width, true);
        if (width != r->width) {
            texture_start_failure(s, &c, path, address, UINT64_MAX,
                                  width, false);
        }
    }
    for (unsigned delta = 1; delta < r->width; delta++) {
        texture_start_failure(s, &c, path, address + delta, UINT64_MAX,
                              r->width, false);
        texture_start_failure(s, &c, path, address + delta, 0,
                              r->width, true);
    }
    if (r->width == 8) {
        texture_start_failure(s, &c, path, address + 4, UINT32_MAX, 4, false);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_texture_start_unlisted(void)
{
    const Register closed[] = {
        {0x110, 4, false}, {0x118, 8, false}, {0x134, 4, false},
        {0x160, 4, false}, {0x168, 4, false},
    };
    g_autofree char *dir = g_dir_make_tmp("crime-texture-closed-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = texture_start_machine();
    Shape c = texture_start_prepare(s);

    for (unsigned i = 0; i < ARRAY_SIZE(closed); i++) {
        for (unsigned width = 1; width <= 8; width *= 2) {
            texture_start_failure(s, &c, path, DE + 0x800 + closed[i].offset,
                                  UINT64_MAX, width, false);
            texture_start_failure(s, &c, path, DE + 0x800 + closed[i].offset,
                                  0, width, true);
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_texture_start_inactive(void)
{
    const uint8_t rows[8] = {0, 0x1e, 0x1e, 0x1e, 0x1e};
    g_autofree char *dir = g_dir_make_tmp("crime-texture-dormant-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = texture_start_machine();

    for (unsigned i = 0; i < ARRAY_SIZE(texture_plane_starts); i++) {
        const TexturePlaneStart *r = &texture_plane_starts[i];
        Shape c = triangle_prepare(s, 0, 0, 0);
        TextureStartSnapshot before, after;
        uint64_t value = r->width == 8 ? UINT64_C(0xfedcba9876543210) :
                         r->offset == 0x130 ? 0 : 0xfedcba98;

        setup(s, 0x228, false);
        qtest_writel(s, DE + 0x70, xy(1, 1));
        qtest_writel(s, DE + 0x74, xy(4, 4));
        qtest_writel(s, DE + 0x18, 0x278);
        qtest_writel(s, DE + 0x1b0, 6);
        qtest_writel(s, DE + 0xd0, TRIANGLE_COLOR);
        qtest_writel(s, DE + 0x110, UINT32_MAX);
        qtest_writeq(s, DE + 0x118, UINT64_MAX);
        qtest_writel(s, DE + 0x130, 0);
        qtest_writel(s, DE + 0x134, UINT32_MAX);
        qtest_writeq(s, RE + 0x1600 + (93 / 4) * 8, UINT64_MAX);
        for (unsigned d = 3; d < ARRAY_SIZE(texture_plane_starts); d++) {
            const TexturePlaneStart *derivative = &texture_plane_starts[d];
            g_assert_cmpint(memtx_write(s, DE + derivative->offset,
                derivative->value ^ 0x1234, derivative->width), ==, MEMTX_OK);
        }
        texture_start_snapshot(s, path, &before);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset, value, r->width),
                        ==, MEMTX_OK);
        triangle_pixels(s, &c, rows, TRIANGLE_COLOR, 6, UINT32_MAX);
        texture_start_snapshot(s, path, &after);
        texture_start_published(&before, r, value);
        g_assert_cmpmem(&before, sizeof(before), &after, sizeof(after));
        triangle_vertices(s, triangle_integer, NULL, true);
        qtest_writel(s, DE + 0x58, 0xf000f000);
        qtest_writel(s, DE + 0x60, 0x02020000);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000 | 0x200);
        texture_start_failure(s, &c, path, DE + 0x800 + r->offset,
                              r->value, r->width, false);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_texture_start_rollback(gconstpointer selection)
{
    const TexturePlaneStart *r =
        &texture_plane_starts[GPOINTER_TO_UINT(selection)];
    g_autofree char *dir =
        g_dir_make_tmp("crime-texture-rollback-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = texture_start_machine();

    for (unsigned failure = 0; failure < 11; failure++) {
        TextureStartSnapshot programmed;
        Shape c = texture_start_prepare(s);
        uint64_t value;

        switch (failure) {
        case 0:
            break;
        case 1:
            qtest_writeq(s, RE + 0x1600 + (93 / 4) * 8, 0);
            break;
        case 2:
            qtest_writeq(s, RE + 0x1600 + (93 / 4) * 8,
                         UINT64_C(0xffff) << 32);
            break;
        case 3:
            /* Two valid taps precede the first unavailable source tile. */
            texture_constant(s);
            qtest_writeq(s, DE + 0x118, texture_format(8, 0));
            qtest_writel(s, DE + 0x110, (2U << 20) | (2U << 18) |
                         (2U << 14) | (8U << 10) |
                         (2U << 4) | (2U << 2) | 3U);
            qtest_writeq(s, DE + 0x120, (UINT64_C(765) << 34) + 0x800);
            qtest_writeq(s, DE + 0x138, UINT64_C(1) << 35);
            qtest_writeq(s, RE + 0x1600 + (80 / 4) * 8,
                         UINT64_C(0x800a) << 48);
            qtest_writel(s, 0xa01f8, 0x2371b9d5);
            qtest_writel(s, 0xa01fc, 0x91b3d5f7);
            break;
        case 4:
            tile(s, 0, 0, 0x7fff);
            break;
        case 5: {
            const int vertices[3][2] = {
                {8128, 64}, {8384, 64}, {8128, 320},
            };
            c.x = 126;
            map_region(s, 0, 4, 126, 0, 133, 7);
            shape_memory(s, &c, NULL);
            triangle_vertices(s, vertices, NULL, true);
            tile(s, 0, 1, 0x7fff);
            break;
        }
        case 6:
            c = gl_line_prepare(s, false);
            texture_two_by_two(s);
            qtest_writel(s, DE + 0x18,
                         qtest_readl(s, DE + 0x18) | 0x8000);
            break;
        case 7:
            qtest_writeq(s, DE + 0x118, texture_format(1, 1) ^ 1);
            break;
        case 8:
            texture_constant(s);
            qtest_writel(s, DE + 0x130, 0x60000800);
            qtest_writel(s, DE + 0x158, 0xe0000000);
            break;
        case 9:
            qtest_writel(s, DE + 0x18,
                         TRIANGLE_DRAW | 0x8000 | 0x200 | 0x80);
            qtest_writel(s, DE + 0x1b8, 0);
            qtest_writeq(s, RE + 0x1600 + (93 / 4) * 8, 0);
            break;
        case 10:
            qtest_writel(s, DE + 0x18,
                         TRIANGLE_DRAW | 0x8000 | 0x100000);
            qtest_writeq(s, DE + 0x48, bounds(0, 0, 0, 0));
            qtest_writel(s, DE + 0x110, 0);
            break;
        }
        texture_start_snapshot(s, path, &programmed);
        value = programmed.words[192 + r->offset / 4];
        if (!failure) {
            value = texture_start_invalid(r);
        } else {
            g_assert_cmpint(memtx_write(s, DE + r->offset, value ^ 0x1000,
                                        r->width), ==, MEMTX_OK);
        }
        g_test_message("plane 0x%x failure %u", r->offset, failure);
        texture_start_failure(s, &c, path, DE + 0x800 + r->offset,
                              value, r->width, false);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_texture_start_aliases(void)
{
    const int vertices[3][2] = { {0, 8064}, {256, 8064}, {0, 8320} };
    const uint8_t rows[8] = {15, 7, 3, 1};
    QTestState *s = texture_start_machine();

    for (unsigned i = 0; i < ARRAY_SIZE(texture_plane_starts); i++) {
        const TexturePlaneStart *r = &texture_plane_starts[i];
        Shape c = triangle_prepare(s, 0, 0, 126);
        uint32_t expected[64];
        uint32_t uploaded = 0x31a7d95b;
        uint32_t second = texture_environment(2, 2, 0, uploaded);

        triangle_vertices(s, vertices, NULL, true);
        texture_constant(s);
        qtest_writeq(s, RE + 0x1600 + (93 / 4) * 8,
                     UINT64_C(0x8003) << 32);
        qtest_writel(s, DE + 0x110, (2U << 20) | (2U << 18) |
                     (10U << 14) | (2U << 4) | (2U << 2));
        qtest_writel(s, DE + 0xd0, TRIANGLE_COLOR);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x8000 | 0x200);
        qtest_writel(s, DE + 0x1b0, 6);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset, r->value,
                                    r->width), ==, MEMTX_OK);
        triangle_pixels(s, &c, rows,
            texture_environment(2, 2, 0, c.old), 6, UINT32_MAX);
        texture_start_pixels(s, &c, expected);
        qtest_writel(s, 0x3fc00, uploaded);
        expected[0] = uploaded;
        qtest_readl(s, STATUS);
        qtest_writel(s, DE + 0x1f0, 0);
        shape_memory(s, &c, expected);
        for (unsigned y = 0; y < 8; y++) {
            for (unsigned x = 0; x < 8; x++) {
                if (rows[y] & (1U << x)) {
                    expected[8 * y + x] ^= second;
                }
            }
        }
        g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset, r->value,
                                    r->width), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
}

static void test_texture_start_profiles(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *s = start_profile(legacy ? "legacy-software" : "1.5",
                                      false, !legacy, "");
        for (unsigned i = 0; i < ARRAY_SIZE(texture_plane_starts); i++) {
            const TexturePlaneStart *r = &texture_plane_starts[i];
            g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset,
                                        r->value, r->width), !=, MEMTX_OK);
        }
        qtest_quit(s);
    }
}

static unsigned shade_modular_reference(int64_t value)
{
    const int64_t modulus = INT64_C(1) << 21;
    int64_t residue = value % modulus;
    if (residue < 0) {
        residue += modulus;
    }
    if (residue >= modulus / 2) {
        residue -= modulus;
    }
    return CLAMP(residue / 4096 - (residue % 4096 < 0), 0, 255);
}

static uint32_t shade_modular_color(const int *start, const int *dx,
                                    const int *dy, int ax, int ay, int x, int y)
{
    uint32_t color = 0;

    for (unsigned k = 0; k < 4; k++) {
        int64_t value = start[k] + (int64_t)dx[k] * (x - ax) +
                        (int64_t)dy[k] * (y - ay);

        color |= shade_modular_reference(value) << (24 - 8 * k);
    }
    return color;
}

static void test_shade_iteration_boundaries(void)
{
    const int integers[] = {
        -256, -129, -128, -127, -1, 0, 127, 128, 255, 256, 382, 383, 384,
    };
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(integers); n++) {
        for (int neighbor = -1; neighbor <= 1; neighbor++) {
            for (unsigned axis = 0; axis < 3; axis++) {
                Shape c = pipeline_prepare(s, 0x228, 0);
                int start[4], dx[4], dy[4];
                uint32_t expected[16];

                for (unsigned k = 0; k < 4; k++) {
                    start[k] = integers[n] * 4096 + neighbor;
                    dx[k] = axis != 1 ? (k & 1 ? -1 : 1) : 0;
                    dy[k] = axis != 0 ? (k & 2 ? -1 : 1) : 0;
                }
                pipeline_shade(s, start, dx, dy);
                qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x10000);
                for (unsigned y = 0; y < 4; y++) {
                    for (unsigned x = 0; x < 4; x++) {
                        expected[4 * y + x] =
                            shade_modular_color(start, dx, dy, 0, 0, x, y);
                    }
                }
                g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
                shape_memory(s, &c, expected);
            }
        }
    }
    /* The selected clamp deliberately disagrees with the two-bit decoder. */
    for (unsigned n = 0; n < ARRAY_SIZE(integers); n++) {
        int v = integers[n];
        unsigned bits = v & 511;
        unsigned decoder = bits & 256 ? (bits & 128 ? 0 : 255) : bits;
        unsigned selected = shade_modular_reference((int64_t)v * 4096);

        if ((v >= -256 && v <= -129) || (v >= 256 && v <= 383)) {
            g_assert_cmpuint(selected, ==, 0);
            g_assert_cmpuint(decoder, ==, 255);
        } else {
            g_assert_cmpuint(selected, ==, decoder);
        }
    }
    qtest_quit(s);
}

static void test_shade_iteration_periods(void)
{
    const int vertex[3][2] = { {0, 0}, {1024 * 64, 0}, {0, 2 * 64} };
    QTestState *s = start_indexed_state();

    for (unsigned reversed = 0; reversed < 2; reversed++) {
        Shape c = pipeline_prepare(s, 0x228, 0);
        int start[] = {1048575, -1048576, 12345, -67890};
        int dx[] = {4096, -4096, 8192, -8192};
        int dy[] = {-12288, 16384, -20480, 24576};
        uint32_t expected[2048];

        c.width = 1024;
        c.height = 2;
        map_region(s, 0, 4, 0, 0, 1023, 1);
        shape_memory(s, &c, NULL);
        if (reversed) {
            for (unsigned k = 0; k < 4; k++) {
                dx[k] = -dx[k];
                dy[k] = -dy[k];
            }
        }
        pipeline_shade(s, start, dx, dy);
        qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x10000);
        for (unsigned v = 0; v < 3; v++) {
            qtest_writel(s, DE + 0x80 + 8 * v, vertex[v][0]);
            qtest_writel(s, DE + 0x84 + 8 * v, vertex[v][1]);
        }
        for (unsigned y = 0; y < 2; y++) {
            int64_t stepped[4];

            for (unsigned k = 0; k < 4; k++) {
                stepped[k] = start[k] + (int64_t)dy[k] * y;
            }
            for (unsigned x = 0; x < 1024; x++) {
                uint32_t color = 0;

                for (unsigned k = 0; k < 4; k++) {
                    color |= shade_modular_reference(stepped[k]) <<
                             (24 - 8 * k);
                    stepped[k] = (stepped[k] + dx[k]) % (1 << 21);
                }
                g_assert_cmphex(color, ==,
                    shade_modular_color(start, dx, dy, 0, 0, x, y));
                expected[1024 * y + x] = x < (y ? 512 : 1024) ? color : c.old;
            }
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
}

static void test_shade_iteration_capture(void)
{
    const int vertices[3][2] = {
        {37932, 32491}, {37583, 32810}, {37732, 33261},
    };
    const uint8_t rows[16] = {
        0, 0, 0, 0x40, 0x60, 0x30, 0x3c, 0x3c,
        0x3c, 0x18, 0x18, 0x18, 0x18, 0, 0, 0,
    };
    const int starts[] = {0x001001b2, 53 * 4096, 121 * 4096, 189 * 4096};
    const int dx[] = {-2057, 0, 0, 0}, dy[] = {-4637, 0, 0, 0};
    QTestState *s = start_indexed_state();
    Shape c = triangle_prepare(s, 0, 586, 506);
    uint32_t expected[128];
    unsigned covered = 0;
    int64_t minimum = INT64_MAX, maximum = INT64_MIN;

    c.height = 16;
    map_region(s, 0, 4, c.x, c.y, c.x + 7, c.y + 15);
    shape_memory(s, &c, NULL);
    triangle_vertices(s, vertices, NULL, true);
    pipeline_shade(s, starts, dx, dy);
    qtest_writel(s, DE + 0x60, 0x02030000);
    qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x10000);
    for (unsigned y = 0; y < 16; y++) {
        for (unsigned x = 0; x < 8; x++) {
            int64_t wide = -1048142 -
                INT64_C(2057) * ((int64_t)c.x + x - 592) -
                INT64_C(4637) * ((int64_t)c.y + y - 507);
            bool inside = rows[y] & (1U << x);
            expected[8 * y + x] = inside ?
                shade_modular_reference(wide) << 24 | 0x3579bd : c.old;
            if (inside) {
                int64_t iterated = wide + (INT64_C(1) << 21);

                minimum = MIN(minimum, iterated);
                maximum = MAX(maximum, iterated);
            }
            covered += inside;
        }
    }
    g_assert_cmpuint(covered, ==, 25);
    g_assert_cmpint(minimum, ==, 1002117);
    g_assert_cmpint(maximum, ==, 1039736);
    g_assert_cmpint(memtx_writeq(s, DE + 0x9c8, 0), ==, MEMTX_OK);
    shape_memory(s, &c, expected);
    g_assert_cmphex(pixel_read(s, pixel_address(0, 592, 509, 4), 4), ==,
                    0xfd3579bd);
    qtest_quit(s);
}

static void test_shade_iteration_invariants(void)
{
    const int vertices[3][2] = {
        {37932, 32491}, {37583, 32810}, {37732, 33261},
    };
    const uint8_t rows[16] = {
        0, 0, 0, 0x40, 0x60, 0x30, 0x3c, 0x3c,
        0x3c, 0x18, 0x18, 0x18, 0x18, 0, 0, 0,
    };
    const int base[] = {-1048142, 1048142, -1048576, 1048575};
    const int dx[] = {-2057, 2057, -524289, 524289};
    const int dy[] = {-4637, 4637, 700001, -700001};
    QTestState *s = start_indexed_state();

    for (unsigned direction = 0; direction < 4; direction++) {
        for (unsigned winding = 0; winding < 2; winding++) {
            for (unsigned origin = 0; origin < 2; origin++) {
                for (unsigned suppression = 0; suppression < 5; suppression++) {
                    Shape c = triangle_prepare(s, 0, 586, 506);
                    unsigned order[] = {0, winding ? 2 : 1, winding ? 1 : 2};
                    unsigned anchor = order[direction & 2 ? 0 : 2];
                    unsigned draw = TRIANGLE_DRAW | 0x10000;
                    uint32_t plane = UINT32_MAX, expected[128];
                    int loaded[4];
                    int o = origin ? -4097 : -4096;
                    int shifted[3][2];
                    int ax = vertices[anchor][0] / 64;
                    int ay = vertices[anchor][1] / 64;

                    c.height = 16;
                    map_region(s, 0, 4, c.x, c.y, c.x + 7, c.y + 15);
                    shape_memory(s, &c, NULL);
                    for (unsigned v = 0; v < 3; v++) {
                        for (unsigned a = 0; a < 2; a++) {
                            shifted[v][a] = vertices[v][a] - 64 * (o + 4096);
                        }
                    }
                    triangle_vertices(s, shifted, order, true);
                    qtest_writel(s, DE + 0x58, xy((uint16_t)o, (uint16_t)o));
                    for (unsigned k = 0; k < 4; k++) {
                        loaded[k] = base[k] + dx[k] * (ax - 592) +
                                    dy[k] * (ay - 507);
                    }
                    pipeline_shade(s, loaded, dx, dy);
                    qtest_writel(s, DE + 0x60, 0x02000000 | direction << 16);
                    if (suppression == 1) {
                        draw |= 1U << 20;
                        qtest_writeq(s, DE + 0x48,
                            bounds(589 - o, 512 - o, 592 - o, 519 - o));
                    } else if (suppression == 2) {
                        draw &= ~0x28U;
                    } else if (suppression == 3) {
                        draw |= 0x80;
                        plane = 0xa5379b6d;
                        qtest_writel(s, DE + 0x1b8, plane);
                    } else if (suppression == 4) {
                        uint64_t addr = RE + 0x1000 + (68 / 4) * 8;
                        uint64_t value = qtest_readq(s, addr);

                        qtest_writeq(s, addr,
                                     value & ~(UINT64_C(0xffff) << 48));
                    }
                    qtest_writel(s, DE + 0x18, draw);
                    for (unsigned y = 0; y < 16; y++) {
                        for (unsigned x = 0; x < 8; x++) {
                            unsigned px = c.x + x, py = c.y + y;
                            bool inside = rows[y] & (1U << x);
                            uint32_t color = shade_modular_color(
                                base, dx, dy, 592, 507, px, py);

                            inside &= suppression != 1 ||
                                (px >= 589 && px < 592 &&
                                 py >= 512 && py < 519);
                            inside &= suppression != 4 || py < 512;
                            expected[8 * y + x] = inside ?
                                pipeline_reference(0x228, color, c.old, 0,
                                    0x45, draw, 3, plane, px, py) : c.old;
                        }
                    }
                    g_assert_cmpint(memtx_writeq(s, DE + 0x9c8, 0),
                                    ==, MEMTX_OK);
                    shape_memory(s, &c, expected);
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_shade_iteration_raw(void)
{
    const int starts[] = {1048575, -1048576, 12345, -67890};
    const int dx[] = {1, -1, 524289, -524289};
    const int dy[] = {1, -1, -700001, 700001};
    g_autofree char *dir = g_dir_make_tmp("crime-shade-raw-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned word = 0; word < 12; word++) {
        for (unsigned prospective = 0; prospective < 2; prospective++) {
            Shape c = pipeline_prepare(s, 0x228, 0);
            uint32_t expected[16];
            uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
            unsigned offset = 0xe0 + 4 * word;
            uint32_t raw;

            pipeline_shade(s, starts, dx, dy);
            qtest_writel(s, DE + 0x18, TRIANGLE_DRAW | 0x10200);
            qtest_writel(s, DE + 0x1b0, 6);
            save_paired_renderer(s, path, before);
            raw = before[192 + offset / 4];
            /* Change one complete raw word while preserving its low 21 bits. */
            raw += 1U << 21;
            g_assert_cmpint(memtx_writel(s,
                DE + offset + (prospective ? 0x800 : 0), raw), ==, MEMTX_OK);
            if (!prospective) {
                for (unsigned n = 0; n < 16; n++) {
                    expected[n] = c.old;
                }
                shape_memory(s, &c, expected);
                g_assert_cmpint(memtx_writeq(s, DE + 0x9c8, 0), ==, MEMTX_OK);
            }
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 4; x++) {
                    expected[4 * y + x] = c.old ^
                        shade_modular_color(starts, dx, dy, 0, 0, x, y);
                }
            }
            shape_memory(s, &c, expected);
            save_paired_renderer(s, path, after);
            before[192 + offset / 4] = raw;
            g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
            qtest_readl(s, STATUS);
            qtest_writel(s, DE + 0x1f0, 0);
            shape_memory(s, &c, expected);
            g_assert_cmpint(memtx_writel(s, DE + 0x800 + offset, raw),
                            ==, MEMTX_OK);
            for (unsigned n = 0; n < 16; n++) {
                expected[n] = c.old;
            }
            shape_memory(s, &c, expected);
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_shade_iteration_rollback(void)
{
    const int starts[] = {1048575, -1048576, 12345, -67890};
    const int dx[] = {1, -1, 524289, -524289};
    const int dy[] = {1, -1, -700001, 700001};
    g_autofree char *dir = g_dir_make_tmp("crime-shade-rollback-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = texture_start_machine();

    for (unsigned failure = 0; failure < 10; failure++) {
        Shape c = texture_start_prepare(s);
        unsigned draw = TRIANGLE_DRAW | 0x10000;

        pipeline_shade(s, starts, dx, dy);
        if (failure == 1) {
            qtest_writel(s, DE + 8, 0x234);
        } else if (failure == 2) {
            tile(s, 0, 0, 0x7fff);
        } else if (failure == 3) {
            const int vertices[3][2] = {
                {8128, 64}, {8384, 64}, {8128, 320},
            };

            c.x = 126;
            map_region(s, 0, 4, 126, 0, 133, 7);
            shape_memory(s, &c, NULL);
            triangle_vertices(s, vertices, NULL, true);
            tile(s, 0, 1, 0x7fff);
        } else if (failure == 4 || failure == 5) {
            draw |= 0x8000;
            if (failure == 4) {
                qtest_writeq(s, RE + 0x1600 + (93 / 4) * 8, 0);
            } else {
                texture_constant(s);
                qtest_writeq(s, DE + 0x118, texture_format(8, 0));
                qtest_writel(s, DE + 0x110, (2U << 20) | (2U << 18) |
                    (2U << 14) | (8U << 10) | (2U << 4) | (2U << 2) | 3U);
                qtest_writeq(s, DE + 0x120, (UINT64_C(765) << 34) + 0x800);
                qtest_writeq(s, DE + 0x138, UINT64_C(1) << 35);
                qtest_writeq(s, RE + 0x1600 + (80 / 4) * 8,
                             UINT64_C(0x800a) << 48);
                qtest_writel(s, 0xa01f8, 0x2371b9d5);
                qtest_writel(s, 0xa01fc, 0x91b3d5f7);
            }
        }
        if (failure == 0 || failure >= 6) {
            draw |= 0x400;
            qtest_writel(s, DE + 0x1a8, 0x1000);
        }
        if (failure == 6) {
            draw |= 0x100000;
            qtest_writeq(s, DE + 0x48, bounds(0, 0, 0, 0));
        } else if (failure == 7) {
            draw &= ~0x78U;
        } else if (failure == 8) {
            qtest_writeq(s, RE + 0x1000, 0);
        } else if (failure == 9) {
            const int vertex[3][2] = { {64, 64}, {64, 64}, {64, 64} };

            triangle_vertices(s, vertex, NULL, true);
        }
        qtest_writel(s, DE + 0x18, draw);
        g_test_message("failure %u", failure);
        texture_start_failure(s, &c, path, DE + 0x8e0,
                              (uint32_t)starts[0] + (1U << 21), 4, false);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_shade_iteration_migration(void)
{
    const int starts[] = {1048575, -1048576, 12345, -67890};
    const int dx[] = {1, -1, 524289, -524289};
    const int dy[] = {1, -1, -700001, 700001};
    g_autofree char *dir =
        g_dir_make_tmp("crime-shade-mod-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *from = start(), *to = start_options("-incoming defer");
    Shape c = pipeline_prepare(from, 0x228, 0);
    uint32_t expected[16];
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];

    pipeline_shade(from, starts, dx, dy);
    qtest_writel(from, DE + 0x18, TRIANGLE_DRAW | 0x10200);
    qtest_writel(from, DE + 0x1b0, 6);
    save_paired_renderer(from, path, before);
    for (unsigned word = 0; word < 12; word++) {
        unsigned index = 192 + (0xe0 + 4 * word) / 4;

        before[index] = (uint32_t)before[index] ^ 0xa5e00000U;
        qtest_writel(from, DE + 0xe0 + 4 * word, before[index]);
    }
    g_assert_cmpint(memtx_writeq(from, DE + 0x9c8, 0), ==, MEMTX_OK);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            expected[4 * y + x] = c.old ^
                shade_modular_color(starts, dx, dy, 0, 0, x, y);
        }
    }
    shape_memory(from, &c, expected);
    save_paired_renderer(from, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    save_paired_renderer(to, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    shape_memory(to, &c, expected);
    qtest_readl(to, STATUS);
    qtest_writel(to, DE + 0x1f0, 0);
    shape_memory(to, &c, expected);
    g_assert_cmpint(memtx_writel(to, DE + 0x8e0, before[192 + 0xe0 / 4]),
                    ==, MEMTX_OK);
    for (unsigned n = 0; n < 16; n++) {
        expected[n] = c.old;
    }
    shape_memory(to, &c, expected);
    qtest_system_reset(to);
    shape_memory(to, &c, expected);
    save_renderer(to, path, after);
    for (unsigned word = 0; word < 12; word++) {
        g_assert_cmphex(after[192 + (0xe0 + 4 * word) / 4], ==, 0);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_unlink(path);
    g_rmdir(dir);
}

typedef struct GLTransferCase {
    unsigned source_mode, destination_mode, width, height, direction;
    unsigned kx, ky, advance, stride, clip;
    int x, y, sx, sy, ox, oy, sox, soy, hx, hy;
    uint32_t draw, plane, function, rop, constant, address, linear;
    uint32_t coordinate_class, primitive_extra, value;
    uint64_t scissor, screen[5];
    bool constant_source;
} GLTransferCase;

static GLTransferCase gl_transfer_case(void)
{
    return (GLTransferCase) {
        .source_mode = 0x1228, .destination_mode = 0x228,
        .width = 5, .height = 4, .direction = 2,
        .kx = 1, .ky = 1, .advance = 12, .stride = 20,
        .x = 126, .y = 126, .sx = 40, .sy = 40,
        .ox = -4096, .oy = -4096, .sox = -4096, .soy = -4096,
        .hx = 1, .hy = 1, .draw = 0x600078,
        .plane = UINT32_MAX, .function = 0x45, .rop = 3,
        .constant = 0x3971b580, .address = 0x12345ffd, .linear = 0x98765ffd,
        .coordinate_class = 0x48000000,
    };
}

static uint32_t gl_transfer_source(const GLTransferCase *t,
                                    unsigned c, unsigned r)
{
    if (t->constant_source) {
        return t->value;
    }
    if (((t->source_mode >> 2) & 3) == 1) {
        return (((c + r) & 1) << 15) | (((3 + 7 * c + r) & 31) << 10) |
               (((7 + c + 9 * r) & 31) << 5) | ((19 + 3 * c + 5 * r) & 31);
    }
    return (((35 + 37 * c + 11 * r) & 255) << 24) |
           (((87 + 13 * c + 41 * r) & 255) << 16) |
           (((169 + 17 * c + 29 * r) & 255) << 8) |
           ((211 + 23 * c + 19 * r) & 255);
}

static uint32_t gl_transfer_reference(const GLTransferCase *t, uint32_t source,
                                       uint32_t old, unsigned x, unsigned y,
                                       unsigned phase)
{
    PipelineFraction normalized[4];
    bool rgb5 = ((t->source_mode >> 2) & 3) == 1;

    for (unsigned k = 0; k < 4; k++) {
        normalized[k] = rgb5 ? (k == 3 ?
            pipeline_fraction((source >> 15) & 1, 1) :
            pipeline_fraction((source >> (10 - 5 * k)) & 31, 31)) :
            pipeline_fraction((source >> (24 - 8 * k)) & 255, 255);
    }
    return pipeline_fraction_reference(t->destination_mode, normalized,
        old, t->constant, t->function, t->draw, t->rop, t->plane, x, y, phase);
}

static void gl_transfer_stage(QTestState *s, const GLTransferCase *t)
{
    int dx = t->direction & 1 ? -1 : 1;
    int dy = t->direction & 2 ? 1 : -1;
    int x0 = t->x + (dx < 0 ? t->width - 1 : 0);
    int y0 = t->y + (dy < 0 ? t->height - 1 : 0);
    bool upload = ((t->source_mode >> 10) & 7) >= 4;

    setup(s, t->destination_mode, false);
    qtest_writel(s, DE, t->source_mode);
    qtest_writel(s, DE + 0x18, t->draw);
    qtest_writel(s, DE + 0x60,
                 0x03000000 | t->direction << 16 | t->primitive_extra);
    qtest_writel(s, DE + 0x58, xy((uint16_t)t->ox, (uint16_t)t->oy));
    qtest_writel(s, DE + 0x50, xy((uint16_t)t->sox, (uint16_t)t->soy));
    for (unsigned v = 0; v < 2; v++) {
        int x = x0 + (v ? dx * (int)(t->width - 1) : 0);
        int y = y0 + (v ? dy * (int)(t->height - 1) : 0);

        qtest_writel(s, DE + 0x80 + 8 * v,
                     t->coordinate_class + (x - t->ox) * 64);
        qtest_writel(s, DE + 0x84 + 8 * v,
                     t->coordinate_class + (y - t->oy) * 64);
    }
    qtest_writel(s, DE + 0xa0, upload ? t->address :
        xy(t->sx - 4096 - t->sox, t->sy - 4096 - t->soy));
    qtest_writel(s, DE + 0xa8,
                 upload ? ((t->kx - 1) << 28) | 4 : t->hx == 1 ? 1 : 0x7ff);
    qtest_writel(s, DE + 0xac, upload ? ((t->ky - 1) << 28) | t->advance :
                 t->hy == 1 ? 1 : 0x7ff);
    qtest_writel(s, DE + 0xb0, t->linear);
    qtest_writel(s, DE + 0xb4, t->stride);
    qtest_writel(s, DE + 0x10, t->clip);
    for (unsigned n = 0; n < 5; n++) {
        qtest_writeq(s, DE + 0x20 + 8 * n, t->screen[n]);
    }
    qtest_writeq(s, DE + 0x48, t->scissor);
    qtest_writel(s, DE + 0x1a0, t->constant);
    qtest_writel(s, DE + 0x1a8, t->function);
    qtest_writel(s, DE + 0x1b0, t->rop);
    qtest_writel(s, DE + 0x1b8, t->plane);
    qtest_writel(s, DE + 0xd0, 0xdeadbeef);
    for (unsigned n = 0; n < 12; n++) {
        qtest_writel(s, DE + 0xe0 + 4 * n, UINT32_MAX - n);
    }
}

static bool gl_transfer_visible(const GLTransferCase *t, int x, int y)
{
    if (x < 0 || x >= 2048 || y < 0 || y >= 2048 ||
        ((t->draw & 0x100000) &&
         !inside_bounds(t->scissor, x - t->ox, y - t->oy))) {
        return false;
    }
    for (unsigned n = 0; n < 5; n++) {
        if ((t->clip & (1U << (5 + n))) &&
            inside_bounds(t->screen[n], x, y) != !!(t->clip & (1U << n))) {
            return false;
        }
    }
    return true;
}

static void gl_transfer_run(QTestState *s, const GLTransferCase *t)
{
    unsigned sb = (t->source_mode >> 10) & 7;
    unsigned db = (t->destination_mode >> 10) & 7;
    unsigned source_bytes = 1U << ((t->source_mode >> 8) & 3);
    unsigned bytes = 1U << ((t->destination_mode >> 8) & 3);
    bool upload = sb >= 4, download = db >= 4;
    unsigned columns = upload ? DIV_ROUND_UP(t->width, t->kx) : t->width;
    unsigned rows = upload ? DIV_ROUND_UP(t->height, t->ky) : t->height;
    unsigned pitch = (columns - 1) * 4 + t->advance;
    unsigned destination_pitch = (t->width - 1) * 4 + t->stride;
    unsigned length = (t->height - 1) * destination_pitch + t->width * 4 + 8;
    int dx = t->direction & 1 ? -1 : 1;
    int dy = t->direction & 2 ? 1 : -1;
    int x0 = t->x + (dx < 0 ? t->width - 1 : 0);
    int y0 = t->y + (dy < 0 ? t->height - 1 : 0);
    Shape c = shape_default();
    g_autofree uint32_t *expected = NULL;
    g_autofree uint8_t *linear = download ? g_malloc(length) : NULL;

    color_map_linear(s);
    c.bank = db;
    c.bytes = bytes;
    c.x = MAX(0, t->x - 1);
    c.y = MAX(0, t->y - 1);
    c.width = MIN(2048, t->x + (int)t->width + 1) - c.x;
    c.height = MIN(2048, t->y + (int)t->height + 1) - c.y;
    c.old = bytes == 4 ? 0x9d3b76a5 : 0x8e7b;
    if (download) {
        memset(linear, 0x5c, length);
        for (unsigned b = 0; b < length; b++) {
            qtest_writeb(s, color_linear(db - 4, t->linear - 4 + b), 0x5c);
        }
    } else {
        map_region(s, db, bytes, c.x, c.y,
                   c.x + c.width - 1, c.y + c.height - 1);
        shape_memory(s, &c, NULL);
        expected = g_new(uint32_t, c.width * c.height);
        for (unsigned i = 0; i < c.width * c.height; i++) {
            expected[i] = c.old;
        }
    }
    if (!upload) {
        int last_x = t->sx + dx * t->hx * (int)(t->width - 1);
        int last_y = t->sy + dy * t->hy * (int)(t->height - 1);

        map_region(s, sb, source_bytes, MIN(t->sx, last_x), MIN(t->sy, last_y),
                   MAX(t->sx, last_x), MAX(t->sy, last_y));
    }
    for (unsigned r = 0; r < rows; r++) {
        for (unsigned col = 0; col < columns; col++) {
            uint32_t value = gl_transfer_source(t, col, r);

            if (upload) {
                color_bytes(s, true, sb - 4, t->address + r * pitch + col * 4,
                            4, &value);
            } else {
                if (source_bytes == 4 && ((t->source_mode >> 2) & 3) == 1) {
                    value = (t->source_mode & 3) == 3 ? value << 16 | 0x6b95 :
                            value | 0xa73c0000;
                }
                pixel_write(s, pixel_address(sb, t->sx + dx * t->hx * (int)col,
                    t->sy + dy * t->hy * (int)r, source_bytes),
                    source_bytes, value);
            }
        }
    }
    gl_transfer_stage(s, t);
    for (unsigned r = 0; r < t->height; r++) {
        for (unsigned col = 0; col < t->width; col++) {
            int x = x0 + dx * (int)col, y = y0 + dy * (int)r;
            uint32_t source = gl_transfer_source(t, upload ? col / t->kx : col,
                                                   upload ? r / t->ky : r);

            if (!gl_transfer_visible(t, x, y)) {
                continue;
            }
            if (download) {
                unsigned at = 4 + r * destination_pitch + col * 4;
                uint32_t value = gl_transfer_reference(t, source, 0x5c5c5c5c,
                    x, y, (t->linear + at - 4) & 3);

                stl_be_p(linear + at, value);
            } else {
                expected[(y - c.y) * c.width + x - c.x] =
                    gl_transfer_reference(t, source, c.old, x, y, x * bytes);
            }
        }
    }
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0xdeadbeef), ==, MEMTX_OK);
    if (download) {
        for (unsigned b = 0; b < length; b++) {
            g_assert_cmphex(qtest_readb(s,
                color_linear(db - 4, t->linear - 4 + b)), ==, linear[b]);
        }
    } else {
        shape_memory(s, &c, expected);
    }
}

static void test_gl_transfer_upload(void)
{
    const uint32_t source[] = {0x12345678, 0x9abcdef0, 0x13579bdf, 0x2468ace1};
    const uint32_t address = 0x12345ffd;
    const int vertices[3][2] = {
        {6 * 64, 7 * 64}, {7 * 64, 8 * 64}, {0, 0},
    };
    QTestState *s = start_indexed_state();
    Shape c = triangle_prepare(s, 0, 4, 4);
    uint32_t expected[64];

    color_map_linear(s);
    qtest_writel(s, DE, 0x1228);
    qtest_writel(s, DE + 0x60, 0x03020000);
    triangle_vertices(s, vertices, NULL, true);
    qtest_writel(s, DE + 0x18, 0x600078);
    qtest_writel(s, DE + 0xa0, address);
    qtest_writel(s, DE + 0xa8, 4);
    qtest_writel(s, DE + 0xac, 12);
    for (unsigned i = 0; i < ARRAY_SIZE(source); i++) {
        uint32_t value = source[i];

        color_bytes(s, true, 0, address + 16 * (i / 2) + 4 * (i % 2),
                    4, &value);
    }
    for (unsigned i = 0; i < 64; i++) {
        expected[i] = c.old;
    }
    shape_memory(s, &c, expected);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0xdeadbeef), ==, MEMTX_OK);
    expected[3 * 8 + 2] = source[0];
    expected[3 * 8 + 3] = source[1];
    expected[4 * 8 + 2] = source[2];
    expected[4 * 8 + 3] = source[3];
    shape_memory(s, &c, expected);
    qtest_quit(s);
}

static void test_gl_transfer_download(void)
{
    const uint32_t source[] = {0x12345678, 0x9abcdef0, 0x13579bdf, 0x2468ace1};
    const uint32_t address = 0x98765ffd;
    const int vertices[3][2] = {
        {6 * 64, 7 * 64}, {7 * 64, 8 * 64}, {0, 0},
    };
    QTestState *s = start_indexed_state();

    setup(s, 0x1628, false);
    qtest_writel(s, DE, 0x628);
    color_map_linear(s);
    map_region(s, 1, 4, 130, 126, 131, 127);
    qtest_writel(s, DE + 0x58, 0xf000f000);
    qtest_writel(s, DE + 0x50, 0xf000f000);
    triangle_vertices(s, vertices, NULL, true);
    qtest_writel(s, DE + 0x18, 0x600078);
    qtest_writel(s, DE + 0xa0, xy(130, 126));
    qtest_writel(s, DE + 0xa8, 1);
    qtest_writel(s, DE + 0xac, 1);
    qtest_writel(s, DE + 0xb0, address);
    qtest_writel(s, DE + 0xb4, 20);
    for (unsigned i = 0; i < ARRAY_SIZE(source); i++) {
        pixel_write(s, pixel_address(1, 130 + i % 2, 126 + i / 2, 4),
                    4, source[i]);
    }
    for (unsigned b = 0; b < 40; b++) {
        qtest_writeb(s, color_linear(1, address + b), 0x5c);
    }
    g_assert_cmpint(memtx_writel(s, DE + 0x8b0, address), ==, MEMTX_OK);
    for (unsigned b = 0; b < 40; b++) {
        unsigned row = b / 24, col = b % 24;
        unsigned expected = row < 2 && col < 8 ?
            (source[2 * row + col / 4] >> (24 - 8 * (col % 4))) & 255 : 0x5c;

        g_assert_cmphex(qtest_readb(s, color_linear(1, address + b)),
                        ==, expected);
    }
    qtest_quit(s);
}

static void test_gl_transfer_copy(void)
{
    const int vertices[3][2] = {
        {6 * 64, 7 * 64}, {8 * 64, 7 * 64}, {0, 0},
    };
    QTestState *s = start_indexed_state();
    Shape c = triangle_prepare(s, 0, 4, 4);
    uint32_t expected[64];

    qtest_writel(s, DE + 0x60, 0x03020000);
    triangle_vertices(s, vertices, NULL, true);
    qtest_writel(s, DE + 0x50, 0xf000f000);
    qtest_writel(s, DE + 0x18, 0x600078);
    qtest_writel(s, DE + 0xa0, xy(5, 7));
    qtest_writel(s, DE + 0xa8, 1);
    qtest_writel(s, DE + 0xac, 1);
    pixel_write(s, pixel_address(0, 5, 7, 4), 4, 0x12345678);
    for (unsigned i = 0; i < 64; i++) {
        expected[i] = c.old;
    }
    expected[3 * 8 + 1] = 0x12345678;
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 7), ==, MEMTX_OK);
    for (unsigned x = 2; x <= 4; x++) {
        expected[3 * 8 + x] = 0x12345678;
    }
    shape_memory(s, &c, expected);
    qtest_quit(s);
}

static void test_gl_transfer_geometry(void)
{
    const uint32_t classes[] = {0, 0x00800000, 0x48000000};
    QTestState *s = start_indexed_state();

    for (unsigned kind = 0; kind < 3; kind++) {
        for (unsigned direction = 0; direction < 4; direction++) {
            for (unsigned encoding = 0; encoding < ARRAY_SIZE(classes);
                 encoding++) {
                for (int origin = -1; origin <= 1; origin++) {
                    GLTransferCase t = gl_transfer_case();

                    if (kind) {
                        t.source_mode = 0x628;
                        t.destination_mode = kind == 1 ? 0x1628 : 0x228;
                    }
                    t.direction = direction;
                    t.coordinate_class = classes[encoding];
                    t.ox += origin;
                    t.oy -= 2 * origin;
                    t.sox -= 3 * origin;
                    t.soy += origin;
                    gl_transfer_run(s, &t);
                    t.width = t.height = 1;
                    gl_transfer_run(s, &t);
                }
            }
        }
        for (unsigned direction = 0; direction < 4; direction++) {
            for (unsigned edge = 0; edge < 4; edge++) {
                GLTransferCase t = gl_transfer_case();

                t.source_mode = kind ? 0x628 : 0x1228;
                t.destination_mode = kind == 1 ? 0x1628 : 0x228;
                t.direction = direction;
                t.x = edge == 0 ? -2 : edge == 1 ? 2046 : 126;
                t.y = edge == 2 ? -2 : edge == 3 ? 2046 : 126;
                gl_transfer_run(s, &t);
            }
        }
        for (unsigned bit = 0; bit <= 23; bit++) {
            GLTransferCase t = gl_transfer_case();

            if (bit == 16 || bit == 17) {
                continue;
            }
            t.source_mode = kind ? 0x628 : 0x1228;
            t.destination_mode = kind == 1 ? 0x1628 : 0x228;
            t.primitive_extra = 1U << bit;
            gl_transfer_run(s, &t);
        }
        for (unsigned slot = 0; slot < 5; slot++) {
            for (unsigned inside = 0; inside < 2; inside++) {
                GLTransferCase t = gl_transfer_case();

                t.source_mode = kind ? 0x628 : 0x1228;
                t.destination_mode = kind == 1 ? 0x1628 : 0x228;
                t.clip = (1U << (5 + slot)) | (inside << slot);
                t.screen[slot] = bounds(127, 127, 130, 129);
                t.draw |= 0x100000;
                t.scissor = bounds(4222, 4222, 4226, 4225);
                gl_transfer_run(s, &t);
            }
        }
    }
    qtest_quit(s);
}

static void test_gl_transfer_repeats(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned kx = 1; kx <= 16; kx++) {
        for (unsigned ky = 1; ky <= 16; ky++) {
            GLTransferCase t = gl_transfer_case();

            t.kx = kx;
            t.ky = ky;
            t.width = 2 * kx + 1;
            t.height = 2 * ky + 1;
            t.source_mode |= (kx & 1) << 10;
            t.address ^= kx & 1 ? 0x80000000U : 0;
            t.destination_mode |= ((kx + ky) % 3) << 10;
            t.direction = (kx + ky) % 4;
            t.draw |= 0x10000;
            if (kx & 1) {
                t.draw |= 0x80;
                t.plane = 0xa5379b6d;
            }
            if ((kx + ky) & 1) {
                t.draw |= 0x100000;
                t.scissor = bounds(t.x + 1 - t.ox, t.y + 1 - t.oy,
                    t.x + t.width - 1 - t.ox, t.y + t.height - 1 - t.oy);
            }
            gl_transfer_run(s, &t);
        }
    }
    qtest_quit(s);
}

static void test_gl_transfer_formats(void)
{
    const unsigned outputs[] = {
        0x228, 0x218, 0x124, 0x224, 0x225, 0x226, 0x227,
    };
    const unsigned inputs[] = {0x228, 0x124, 0x224, 0x225, 0x226, 0x227};
    QTestState *s = start_indexed_state();

    for (unsigned linear = 0; linear < 2; linear++) {
        for (unsigned bank = 0; bank < 3; bank++) {
            for (unsigned f = 0; f < ARRAY_SIZE(outputs); f++) {
                for (unsigned reserved = 0; reserved < 2; reserved++) {
                    GLTransferCase t = gl_transfer_case();

                    t.source_mode |= linear << 10;
                    t.destination_mode = outputs[f] | bank << 10;
                    if (reserved) {
                        t.source_mode |= 0xa5e00001;
                        t.destination_mode |= 0x5a5fe000;
                    }
                    gl_transfer_run(s, &t);
                }
            }
            for (unsigned f = 0; f < ARRAY_SIZE(inputs); f++) {
                GLTransferCase t = gl_transfer_case();

                t.source_mode = inputs[f] | bank << 10;
                t.destination_mode = 0x1228 | linear << 10;
                t.direction = (f + bank + linear) % 4;
                gl_transfer_run(s, &t);
            }
        }
    }
    for (unsigned sf = 0; sf < ARRAY_SIZE(inputs); sf++) {
        for (unsigned df = 0; df < ARRAY_SIZE(outputs); df++) {
            for (unsigned bank = 0; bank < 3; bank++) {
                GLTransferCase t = gl_transfer_case();

                t.source_mode = inputs[sf] | ((bank + 1) % 3) << 10;
                t.destination_mode = outputs[df] | bank << 10;
                t.direction = (sf + df + bank) % 4;
                t.hx = (sf + df) & 1 ? -1 : 1;
                t.hy = (sf + bank) & 2 ? -1 : 1;
                t.draw |= 0x100;
                gl_transfer_run(s, &t);
            }
        }
    }
    for (unsigned direction = 0; direction < 4; direction++) {
        for (int hx = -1; hx <= 1; hx += 2) {
            for (int hy = -1; hy <= 1; hy += 2) {
                GLTransferCase t = gl_transfer_case();

                t.source_mode = 0x628;
                t.direction = direction;
                t.hx = hx;
                t.hy = hy;
                gl_transfer_run(s, &t);
            }
        }
    }
    qtest_quit(s);
}

static void test_gl_transfer_precision(void)
{
    const unsigned codes[] = {3, 7};
    const unsigned bytes[] = {25, 58};
    QTestState *s = start_indexed_state();
    GLTransferCase t = gl_transfer_case();
    Shape c = pipeline_prepare(s, 0x228, 0);

    t.source_mode = 0x524;
    t.x = t.y = 0;
    t.width = t.height = 1;
    t.constant_source = true;
    c.old = 0;
    map_region(s, 1, 2, 40, 40, 40, 40);
    for (unsigned n = 0; n < ARRAY_SIZE(codes); n++) {
        shape_memory(s, &c, NULL);
        pixel_write(s, pixel_address(1, 40, 40, 2), 2,
                    0x8000 | codes[n] << 10);
        gl_transfer_stage(s, &t);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        g_assert_cmphex(pixel_read(s, pixel_address(0, 0, 0, 4), 4),
                        ==, bytes[n] << 24 | 255);
    }
    shape_memory(s, &c, NULL);
    pixel_write(s, pixel_address(1, 40, 40, 2), 2, 0x8c00);
    t.draw |= 0x400;
    t.function = 0xa0;
    t.constant = 0x00000080;
    gl_transfer_stage(s, &t);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    g_assert_cmphex(pixel_read(s, pixel_address(0, 0, 0, 4), 4),
                    ==, 0x0c000080);
    qtest_quit(s);
}

static void test_gl_transfer_masks(void)
{
    const unsigned modes[] = {0x228, 0x218, 0x124, 0x224, 0x225, 0x226, 0x227};
    QTestState *s = start_indexed_state();

    for (unsigned f = 0; f < ARRAY_SIZE(modes); f++) {
        for (unsigned rop = 0; rop < 16; rop++) {
            for (unsigned mask = 0; mask < 16; mask++) {
                GLTransferCase t = gl_transfer_case();

                t.source_mode = 0x524;
                t.destination_mode = modes[f];
                t.draw = 0x600380 | mask << 3;
                t.rop = rop;
                t.plane = (rop ^ mask) & 1 ? 0xa5379b6d : UINT32_MAX;
                t.direction = (rop + mask) % 4;
                gl_transfer_run(s, &t);
            }
        }
        for (unsigned bit = 0; bit < 32; bit++) {
            for (unsigned active = 0; active < 2; active++) {
                GLTransferCase t = gl_transfer_case();

                t.destination_mode = modes[f];
                t.draw |= active ? 0x80 : 0;
                t.plane = 1U << bit;
                gl_transfer_run(s, &t);
            }
        }
    }
    for (unsigned phase = 0; phase < 4; phase++) {
        for (unsigned mask = 0; mask < 16; mask++) {
            GLTransferCase t = gl_transfer_case();

            t.source_mode = 0x627;
            t.destination_mode = 0x1628;
            t.linear = (t.linear & ~3U) | phase;
            t.draw = 0x600280 | mask << 3;
            t.rop = (phase + mask) % 16;
            t.plane = 0xb375a91d;
            gl_transfer_run(s, &t);
        }
    }
    qtest_quit(s);
}

static void test_gl_transfer_blend(void)
{
    const unsigned sources[] = {0x524, 0x624, 0x625, 0x626, 0x627};
    const unsigned outputs[] = {
        0x228, 0x218, 0x124, 0x224, 0x225, 0x226, 0x227,
    };
    QTestState *s = start_indexed_state();

    for (unsigned op = 0; op < 5; op++) {
        for (unsigned sf = 0; sf < 13; sf++) {
            for (unsigned df = 0; df < 12; df++) {
                GLTransferCase t = gl_transfer_case();

                t.source_mode = sources[(op + sf + df) % ARRAY_SIZE(sources)];
                t.destination_mode = outputs[(op + 2 * sf + df) %
                                             ARRAY_SIZE(outputs)];
                t.draw |= 0x500;
                t.function = op << 8 | sf << 4 | df;
                gl_transfer_run(s, &t);
            }
        }
    }
    qtest_quit(s);
}

static GLTransferCase gl_transfer_probe(QTestState *s, unsigned kind)
{
    GLTransferCase t = gl_transfer_case();
    uint32_t source = 0x13579bdf, old = 0x89674523;

    t.width = t.height = 1;
    t.x = t.y = 0;
    t.draw |= 0x200;
    t.rop = 6;
    t.constant_source = true;
    t.value = source;
    t.source_mode = kind ? 0x628 : 0x1228;
    t.destination_mode = kind == 1 ? 0x1628 : 0x228;
    color_map_linear(s);
    if (kind) {
        map_region(s, 1, 4, 40, 40, 40, 40);
        pixel_write(s, pixel_address(1, 40, 40, 4), 4, source);
    } else {
        color_bytes(s, true, 0, t.address, 4, &source);
    }
    if (kind == 1) {
        color_bytes(s, true, 1, t.linear, 4, &old);
    } else {
        map_region(s, 0, 4, 0, 0, 0, 0);
        pixel_write(s, pixel_address(0, 0, 0, 4), 4, old);
    }
    gl_transfer_stage(s, &t);
    return t;
}

static uint32_t gl_transfer_probe_read(QTestState *s, const GLTransferCase *t)
{
    uint32_t value = 0;

    if (((t->destination_mode >> 10) & 7) >= 4) {
        color_bytes(s, false, 1, t->linear, 4, &value);
    } else {
        value = pixel_read(s, pixel_address(0, 0, 0, 4), 4);
    }
    return value;
}

static unsigned gl_transfer_starts(Register forms[64])
{
    const unsigned ordinary[] = {
        0, 8, 0x10, 0x18, 0x50, 0x58, 0x60, 0x70, 0x74, 0x78,
        0xa0, 0xa8, 0xac, 0xc0, 0xc4, 0xd0, 0xd8, 0x1b0, 0x1b8,
        0x1c0, 0x1e0, 0x1e8, 0x130, 0x158, 0x15c, 0x1f0, 0xb0, 0xb4,
    };
    const unsigned wide[] = {
        0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x70, 0xa8, 0xc0,
        0x1c8, 0x1d0, 0x1d8, 0x120, 0x128, 0x138, 0x140, 0x148, 0x150,
    };
    unsigned count = 0;

    for (unsigned n = 0; n < ARRAY_SIZE(ordinary); n++) {
        forms[count++] = (Register) {ordinary[n], 4};
    }
    for (unsigned n = 0; n < ARRAY_SIZE(wide); n++) {
        forms[count++] = (Register) {wide[n], 8};
    }
    for (unsigned offset = 0xe0; offset <= 0x10c; offset += 4) {
        forms[count++] = (Register) {offset, 4};
    }
    return count;
}

static void test_gl_transfer_starts(void)
{
    Register forms[64];
    unsigned count = gl_transfer_starts(forms);
    g_autofree char *dir =
        g_dir_make_tmp("crime-gl-transfer-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned kind = 0; kind < 3; kind++) {
        for (unsigned n = 0; n < count; n++) {
            const Register *form = &forms[n];
            GLTransferCase t = gl_transfer_probe(s, kind);
            uint64_t before[RENDER_WORDS], after[RENDER_WORDS], value;
            bool paired = form->width == 8 && (form->offset == 0x70 ||
                form->offset == 0xa8 || form->offset == 0xc0);

            if (kind != 1 && (form->offset == 0xb0 || form->offset == 0xb4)) {
                continue;
            }
            save_paired_renderer(s, path, before);
            value = form->offset == 0x1f0 ? 0xdeadbeef :
                    before[192 + form->offset / 4];
            if (paired) {
                value = value << 32 | before[192 + form->offset / 4 + 1];
            }
            if (form->offset != 0x1f0) {
                g_assert_cmpint(memtx_write(s, DE + form->offset,
                    value ^ 0x1000, form->width), ==, MEMTX_OK);
            }
            g_assert_cmphex(gl_transfer_probe_read(s, &t), ==, 0x89674523);
            g_test_message("kind %u offset 0x%x width %u", kind,
                           form->offset, form->width);
            g_assert_cmpint(memtx_write(s, DE + 0x800 + form->offset,
                                        value, form->width), ==, MEMTX_OK);
            g_assert_cmphex(gl_transfer_probe_read(s, &t), ==,
                            0x89674523 ^ 0x13579bdf);
            save_paired_renderer(s, path, after);
            g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
            for (unsigned poll = 0; poll < 3; poll++) {
                qtest_readl(s, STATUS);
                qtest_writel(s, DE + 0x1f0, 0xa5a5a5a5);
                qtest_writel(s, DE + 0x1f8, 0);
            }
            g_assert_cmphex(gl_transfer_probe_read(s, &t), ==,
                            0x89674523 ^ 0x13579bdf);
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0x12345678),
                            ==, MEMTX_OK);
            g_assert_cmphex(gl_transfer_probe_read(s, &t), ==, 0x89674523);
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

typedef struct GLTransferSnapshot {
    TextureStartSnapshot renderer;
    uint64_t linear[32], banks[8];
} GLTransferSnapshot;

static void gl_transfer_snapshot(QTestState *s, const char *path,
                                  GLTransferSnapshot *state)
{
    memset(state, 0, sizeof(*state));
    if (path) {
        texture_start_snapshot(s, path, &state->renderer);
    } else {
        const unsigned global[] = {0x10, 0x18, 0x20, 0x40, 0x48, 0x250, 0x258};

        /* Populated-bank fixtures deliberately block migration snapshots. */
        for (unsigned n = 0; n < 192; n++) {
            state->renderer.words[n] = qtest_readq(s, RE + 0x1000 + 8 * n);
        }
        for (unsigned set = 0; set < 2; set++) {
            const Register *regs = set ? native_registers : registers;
            unsigned count = set ? ARRAY_SIZE(native_registers) :
                                   ARRAY_SIZE(registers);

            for (unsigned n = 0; n < count; n++) {
                if (regs[n].readable) {
                    uint64_t value;

                    g_assert_cmpint(memtx_read(s, DE + regs[n].offset,
                        &value, regs[n].width), ==, MEMTX_OK);
                    state->renderer.words[192 + regs[n].offset / 4] = value;
                }
            }
        }
        for (unsigned n = 0; n < 28; n++) {
            state->renderer.texture[n] = qtest_readq(s, RE + 0x1600 + 8 * n);
        }
        for (unsigned n = 0; n < ARRAY_SIZE(global); n++) {
            state->renderer.global[n] = qtest_readq(s, CRIME + global[n]);
        }
        ifb_snapshot(s, &state->renderer.ifb);
        state->renderer.status = qtest_readl(s, STATUS);
        for (unsigned n = 0; n < 5; n++) {
            state->renderer.irq |= qtest_get_irq(s, n) << n;
        }
    }
    for (unsigned n = 0; n < 32; n++) {
        state->linear[n] = qtest_readq(s, RE + 0x1700 + 8 * n);
    }
    for (unsigned n = 0; n < 8; n++) {
        state->banks[n] = qtest_readq(s, CRIME + 0x208 + 8 * n);
    }
}

static void gl_transfer_ram(QTestState *s, uint8_t *data)
{
    qtest_memread(s, 0x30000, data, 0x120000);
    qtest_memread(s, 0x400000, data + 0x120000, 0x80000);
    qtest_memread(s, 0x1030000, data + 0x1a0000, 0x10000);
}

static void gl_transfer_failure(QTestState *s, const char *path,
                                 uint64_t address, uint64_t value,
                                 unsigned width, bool read, bool all_ram)
{
    GLTransferSnapshot before, after;
    size_t bytes = all_ram ? 0x1b0000 : 0;
    g_autofree uint8_t *old = bytes ? g_malloc(bytes) : NULL;
    g_autofree uint8_t *next = bytes ? g_malloc(bytes) : NULL;
    uint32_t tile_old = pixel_read(s, pixel_address(0, 0, 0, 4), 4);
    uint64_t ignored;

    gl_transfer_snapshot(s, path, &before);
    if (bytes) {
        gl_transfer_ram(s, old);
    }
    g_assert_cmpint(read ? memtx_read(s, address, &ignored, width) :
                          memtx_write(s, address, value, width), !=, MEMTX_OK);
    gl_transfer_snapshot(s, path, &after);
    g_assert_cmpmem(&before, sizeof(before), &after, sizeof(after));
    g_assert_cmphex(pixel_read(s, pixel_address(0, 0, 0, 4), 4), ==, tile_old);
    if (bytes) {
        gl_transfer_ram(s, next);
        g_assert_cmpmem(old, bytes, next, bytes);
    }
}

static void gl_transfer_linear_entry(QTestState *s, unsigned bank,
                                      unsigned index, uint32_t entry)
{
    uint64_t address = RE + 0x1700 + bank * 128 + (index / 2) * 8;
    uint64_t value = qtest_readq(s, address);

    value = index & 1 ? (value & 0xffffffff00000000ULL) | entry :
                       (value & UINT32_MAX) | (uint64_t)entry << 32;
    qtest_writeq(s, address, value);
}

static void test_gl_transfer_refusals(void)
{
    const unsigned unsupported[] = {
        0, 1, 2, 11, 12, 13, 14, 15, 17, 18, 19, 23,
    };
    g_autofree char *dir =
        g_dir_make_tmp("crime-gl-transfer-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = texture_start_machine();

    for (unsigned bit = 0; bit < ARRAY_SIZE(unsupported); bit++) {
        for (unsigned suppression = 0; suppression < 3; suppression++) {
            GLTransferCase t = gl_transfer_probe(s, 0);

            t.draw |= 1U << unsupported[bit];
            if (suppression == 0) {
                t.draw &= ~0x78U;
            } else if (suppression == 1) {
                t.draw |= 0x100000;
                t.scissor = bounds(0, 0, 0, 0);
            } else {
                t.x = t.y = 3000;
            }
            gl_transfer_stage(s, &t);
            gl_transfer_failure(s, path, DE + 0x8d0, 0x12345678,
                                4, false, true);
        }
    }
    for (unsigned failure = 0; failure < 40; failure++) {
        GLTransferCase t = gl_transfer_case();
        Shape c = triangle_prepare(s, 0, 124, 124);

        color_map_linear(s);
        map_region(s, 1, 4, 40, 40, 44, 43);
        if (failure == 0) {
            t.source_mode = 0x1218;
        } else if (failure == 1) {
            t.destination_mode = 0x010;
        } else if (failure == 2) {
            t.clip = 0x63;
        } else if (failure == 3) {
            t.clip = 0x400;
        } else if (failure == 4) {
            t.clip = 1;
        } else if (failure == 5) {
            t.draw |= 0x400;
            t.function = 0x1000;
        } else if (failure == 6) {
            t.draw |= 0x400;
            t.function = 0x500;
        } else if (failure == 7) {
            t.draw |= 0x400;
            t.function = 0xd0;
        } else if (failure == 8) {
            t.draw |= 0x400;
            t.function = 0xc;
        } else if (failure == 9) {
            t.draw |= 0x200;
            t.rop = 16;
        } else if (failure == 10) {
            t.address = 0xfffffffd;
        } else if (failure == 11) {
            t.width = 1;
            t.height = 3;
            t.advance = 0x10000;
        } else if (failure == 12) {
            t.width = 2049;
            t.height = 2048;
            t.x = t.y = 0;
        } else if (failure >= 13 && failure <= 18) {
            t.source_mode = 0x628;
            t.destination_mode = 0x1628;
            if (failure == 13) {
                t.stride = 0;
            } else if (failure == 14) {
                t.stride = 0x20000;
            } else if (failure == 15) {
                t.linear = 0xfffffffd;
            } else if (failure == 16) {
                t.stride = 0x10000;
            } else if (failure == 17) {
                t.hx = -1;
            } else {
                t.sox = 0;
            }
        } else if (failure == 19 || failure == 20) {
            t.draw |= 0x200;
            t.rop = failure == 19 ? 0 : 5;
        } else if (failure == 21) {
            t.address = 0x12345ff8;
        } else if (failure == 24 || failure == 25) {
            t.x = t.y = 0;
            t.width = 129;
            t.height = 1;
            map_region(s, 0, 4, 0, 0, 128, 0);
        } else if (failure == 26 || failure == 27) {
            t.address = 0x12345df8;
        } else if (failure == 28 || failure == 29) {
            t.source_mode = 0x628;
        }
        gl_transfer_stage(s, &t);
        switch (failure) {
        case 19: case 20:
            gl_transfer_linear_entry(s, 0, 5, 0);
            break;
        case 21:
            gl_transfer_linear_entry(s, 0, 6, 0);
            break;
        case 22:
            tile(s, 0, 0, 0x7fff);
            break;
        case 23:
            tile(s, 0, 17, 0x7fff);
            break;
        case 24: case 25:
            tile(s, 0, 1, failure == 24 ? 3 : 0x4003);
            break;
        case 26: case 27:
            gl_transfer_linear_entry(s, 0, 5,
                                     failure == 26 ? 0x8000003f : 0x8004003f);
            break;
        case 28:
            qtest_writel(s, DE + 0xa8, 2);
            break;
        case 29:
            qtest_writel(s, DE + 0xa0, 0x80000000);
            break;
        case 30:
            qtest_writel(s, DE + 0x80, 0x48041f81);
            break;
        case 31:
            qtest_writel(s, DE + 0x80, 0x49041f80);
            break;
        case 32:
            qtest_writel(s, DE + 0x60, 0x03030000);
            break;
        case 33:
            qtest_writel(s, DE + 0xa8, 0x20004);
            break;
        case 34:
            qtest_writel(s, DE + 0xac, 0x20004);
            break;
        case 35:
            qtest_writel(s, DE + 0xa8, 8);
            break;
        case 36:
            qtest_writel(s, DE + 0xac, 0);
            break;
        case 37:
            gl_transfer_linear_entry(s, 0, 5, 0xc000040a);
            break;
        case 38:
            qtest_writeq(s, RE + 0x1000, qtest_readq(s, RE + 0x1000) &
                         ~(UINT64_C(0xffff) << 48));
            break;
        case 39:
            qtest_writeq(s, RE + 0x1000 + (17 / 4) * 8,
                qtest_readq(s, RE + 0x1000 + (17 / 4) * 8) &
                ~(UINT64_C(0xffff) << 32));
            break;
        }
        g_test_message("failure %u", failure);
        gl_transfer_failure(s, path, DE + 0x8d0, 0x12345678, 4, false, true);
        if (failure <= 18) {
            for (unsigned clipped = 0; clipped < 2; clipped++) {
                qtest_writel(s, DE + 0x18, clipped ? t.draw | 0x100000 :
                                                  t.draw & ~0x78U);
                qtest_writeq(s, DE + 0x48, bounds(0, 0, 0, 0));
                gl_transfer_failure(s, path, DE + 0x8d0,
                                    0x12345678, 4, false, true);
            }
        }
        (void)c;
    }
    /* Masked or clipped pixels do not consume actual-access mappings. */
    for (unsigned clipped = 0; clipped < 2; clipped++) {
        GLTransferCase t = gl_transfer_probe(s, 0);

        if (clipped) {
            t.draw |= 0x100000;
            t.scissor = bounds(0, 0, 0, 0);
        } else {
            t.draw &= ~0x78U;
        }
        gl_transfer_stage(s, &t);
        gl_transfer_linear_entry(s, 0, 5, 0);
        qtest_writeq(s, RE + 0x1000, 0);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        g_assert_cmphex(gl_transfer_probe_read(s, &t), ==, 0x89674523);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_transfer_widths(gconstpointer selection)
{
    const unsigned forbidden[] = {
        0x80, 0x84, 0x88, 0x8c, 0x90, 0x94,
        0x134, 0x160, 0x168, 0x170, 0x178, 0x180, 0x188, 0x190, 0x194,
    };
    unsigned selected = GPOINTER_TO_UINT(selection), kind = selected / 3;
    Register forms[64];
    unsigned count = gl_transfer_starts(forms);
    g_autofree char *dir =
        g_dir_make_tmp("crime-gl-transfer-width-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = texture_start_machine();
    GLTransferCase t = gl_transfer_probe(s, kind);

    for (unsigned n = 0; n < count; n++) {
        const Register *r = &forms[n];
        bool pair = r->offset == 0x70 || r->offset == 0xa8 || r->offset == 0xc0;

        if (n % 3 != selected % 3) {
            continue;
        }
        for (unsigned width = 1; width <= 8; width *= 2) {
            gl_transfer_failure(s, path, DE + 0x800 + r->offset,
                                0, width, true, false);
            if (width != r->width && !((width == 4 || width == 8) && pair)) {
                gl_transfer_failure(s, path, DE + 0x800 + r->offset,
                                    0, width, false, false);
            }
        }
        for (unsigned delta = 1; delta < r->width; delta++) {
            gl_transfer_failure(s, path, DE + 0x800 + r->offset + delta,
                                0, r->width, false, false);
        }
    }
    for (unsigned n = 0; n < ARRAY_SIZE(forbidden); n++) {
        if (n % 3 == selected % 3) {
            gl_transfer_failure(s, path, DE + 0x800 + forbidden[n],
                                0, 4, false, false);
            gl_transfer_failure(s, path, DE + 0x800 + forbidden[n],
                                0, 8, false, false);
        }
    }
    if (selected % 3 == 0) {
        gl_transfer_failure(s, path, DE + 0x8a0, 0, 8, false, false);
        gl_transfer_failure(s, path, DE + 0x8b0, 0, 8, false, false);
        gl_transfer_failure(s, path, DE + 0x1f8, 1, 4, false, false);
        if (kind != 1) {
            gl_transfer_failure(s, path, DE + 0x8b0, t.linear, 4, false, false);
            gl_transfer_failure(s, path, DE + 0x8b4, t.stride, 4, false, false);
        }
    }
    g_assert_cmphex(gl_transfer_probe_read(s, &t), ==, 0x89674523);
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_transfer_order(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned alias = 0; alias < 3; alias++) {
        for (unsigned direction = 0; direction < 4; direction++) {
            for (unsigned signs = 0; signs < 4; signs++) {
                for (unsigned self = 0; self < 2; self++) {
                    GLTransferCase t = gl_transfer_case();
                    Shape c = triangle_prepare(s, 0, 0, 0);
                    uint32_t expected[256];
                    int dx = direction & 1 ? -1 : 1;
                    int dy = direction & 2 ? 1 : -1;
                    int x0 = dx > 0 ? 5 : 8, y0 = dy > 0 ? 5 : 7;

                    c.width = c.height = 16;
                    map_region(s, 0, 4, 0, 0, 15, 15);
                    for (unsigned y = 0; y < 16; y++) {
                        for (unsigned x = 0; x < 16; x++) {
                            expected[16 * y + x] =
                                0x17395b00 + 19 * x + 37 * y;
                            pixel_write(s, pixel_address(0, x, y, 4),
                                        4, expected[16 * y + x]);
                        }
                    }
                    t.source_mode = alias ? 0x628 : 0x228;
                    if (alias) {
                        tile(s, 1, 0, alias == 1 ? 3 : 0x4003);
                    }
                    t.width = 4;
                    t.height = 3;
                    t.x = t.y = 5;
                    t.direction = direction;
                    t.hx = signs & 1 ? -1 : 1;
                    t.hy = signs & 2 ? -1 : 1;
                    t.sx = t.hx == 1 ? (dx > 0 ? 4 : 7) : (dx > 0 ? 8 : 3);
                    t.sy = t.hy == 1 ? (dy > 0 ? 4 : 6) : (dy > 0 ? 8 : 3);
                    if (self) {
                        t.sx = x0;
                        t.sy = y0;
                        t.hx = t.hy = 1;
                    }
                    for (unsigned r = 0; r < t.height; r++) {
                        for (unsigned col = 0; col < t.width; col++) {
                            int x = x0 + dx * (int)col;
                            int y = y0 + dy * (int)r;
                            int sx = t.sx + dx * t.hx * (int)col;
                            int sy = t.sy + dy * t.hy * (int)r;

                            expected[16 * y + x] = expected[16 * sy + sx];
                        }
                    }
                    gl_transfer_stage(s, &t);
                    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0),
                                    ==, MEMTX_OK);
                    shape_memory(s, &c, expected);
                }
            }
        }
    }
    /* A source-only table alias may repeat a read without repeating a write. */
    {
        GLTransferCase t = gl_transfer_case();
        Shape c = triangle_prepare(s, 0, 0, 2);
        uint32_t expected[129];

        t.source_mode = 0x628;
        t.width = 129;
        t.height = 1;
        t.x = t.sx = t.sy = 0;
        t.y = 2;
        c.x = 0;
        c.width = 129;
        c.height = 1;
        map_region(s, 0, 4, 0, 2, 128, 2);
        shape_memory(s, &c, NULL);
        tile(s, 1, 0, page_for(1, 0));
        tile(s, 1, 1, page_for(1, 0));
        for (unsigned x = 0; x < 128; x++) {
            expected[x] = 0x13579b00 + x;
            pixel_write(s, pixel_address(1, x, 0, 4), 4, expected[x]);
        }
        expected[128] = expected[0];
        gl_transfer_stage(s, &t);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
}

static void test_gl_transfer_mixed_overlap(void)
{
    const uint32_t initial[8] = {
        0x8c009c00, 0xb4e78fff, 0xe4a19d03, 0x4a5371d5,
        0x2468ace1, 0x395b7d9f, 0xa5c7e901, 0x10325476,
    };
    QTestState *s = start_indexed_state();
    GLTransferCase t = gl_transfer_case();
    uint8_t expected_bytes[32];
    uint32_t expected[8];
    Shape c = triangle_prepare(s, 0, 0, 0);

    c.width = 8;
    c.height = 1;
    t.source_mode = 0x524;
    t.width = 4;
    t.height = 1;
    t.x = 1;
    t.y = t.sx = t.sy = 0;
    tile(s, 1, 0, 0x4003);
    for (unsigned x = 0; x < 8; x++) {
        pixel_write(s, pixel_address(0, x, 0, 4), 4, initial[x]);
        stl_be_p(expected_bytes + 4 * x, initial[x]);
    }
    for (unsigned x = 0; x < 4; x++) {
        unsigned source = lduw_be_p(expected_bytes + 2 * x);
        unsigned old = ldl_be_p(expected_bytes + 4 * (x + 1));
        uint32_t value = gl_transfer_reference(&t, source, old, x + 1, 0, 0);

        stl_be_p(expected_bytes + 4 * (x + 1), value);
    }
    g_assert_cmphex(ldl_be_p(expected_bytes + 4), ==, 0x190000ff);
    for (unsigned x = 0; x < 8; x++) {
        expected[x] = ldl_be_p(expected_bytes + 4 * x);
    }
    gl_transfer_stage(s, &t);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    shape_memory(s, &c, expected);
    for (unsigned high = 0; high < 2; high++) {
        t.source_mode = high ? 0x627 : 0x626;
        t.destination_mode = high ? 0x226 : 0x227;
        t.width = t.height = 1;
        t.sx = t.x = 1;
        t.sy = t.y = 0;
        pixel_write(s, pixel_address(0, 1, 0, 4), 4,
                    high ? 0x8c00347a : 0x347a8c00);
        gl_transfer_stage(s, &t);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        g_assert_cmphex(pixel_read(s, pixel_address(0, 1, 0, 4), 4),
                        ==, 0x8c008c00);
    }
    qtest_quit(s);
}

static void test_gl_transfer_banks(void)
{
    QTestState *s = start_profile_ram("legacy-software", true, true, false, "");
    GLTransferCase t = gl_transfer_case();

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    triangle_irq_baseline(s);
    for (unsigned n = 0; n < 8; n++) {
        qtest_writeq(s, CRIME + 0x208 + 8 * n, 0x100 | n * 4);
    }
    color_map_linear(s);
    t.y = 0;
    t.height = 1;
    tile(s, 0, 0, 0x7ff);
    tile(s, 0, 1, 0x800);
    pixel_write(s, 0x7ff01f8, 4, 0x89674523);
    for (unsigned n = 0; n < 6; n++) {
        uint32_t value = n ? 0x2468ace1 : 0x13579bdf;

        color_bytes(s, true, 0, t.address + 4 * n, 4, &value);
    }
    gl_transfer_stage(s, &t);
    qtest_writeq(s, CRIME + 0x210, 4);
    gl_transfer_failure(s, NULL, DE + 0x8a0, t.address + 4, 4, false, true);
    g_assert_cmphex(pixel_read(s, 0x7ff01f8, 4), ==, 0x89674523);
    /* Consuming the old address after repair checks its write-only rollback. */
    qtest_writeq(s, CRIME + 0x210, 0x104);
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    g_assert_cmphex(pixel_read(s, 0x7ff01f8, 4), ==, 0x13579bdf);
    qtest_quit(s);
}

static void test_gl_transfer_migration(void)
{
    g_autofree char *dir =
        g_dir_make_tmp("crime-gl-transfer-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);

    for (unsigned unsupported = 0; unsupported < 2; unsupported++) {
        QTestState *from = start(), *to = start_options("-incoming defer");
        GLTransferCase t = gl_transfer_probe(from, 0);
        GLTransferSnapshot before, after;

        qtest_irq_intercept_out_named(from, "/machine/peripheral/crime",
                                       "cpu-ip");
        qtest_irq_intercept_out_named(to, "/machine/peripheral/crime",
                                       "cpu-ip");
        triangle_irq_baseline(from);
        g_assert_cmpint(memtx_writel(from, DE + 0x9f0, 0xdeadbeef),
                        ==, MEMTX_OK);
        qtest_writeq(from, DE + 0x120, UINT64_MAX);
        qtest_writeq(from, DE + 0x1c8, UINT64_MAX);
        if (unsupported) {
            qtest_writel(from, DE + 0xa8, 0xf00bad00);
        }
        gl_transfer_snapshot(from, path, &before);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        gl_transfer_snapshot(to, path, &after);
        g_assert_cmpmem(&before, sizeof(before), &after, sizeof(after));
        g_assert_cmphex(gl_transfer_probe_read(to, &t), ==,
                        0x89674523 ^ 0x13579bdf);
        qtest_readl(to, STATUS);
        qtest_writel(to, DE + 0x1f0, UINT32_MAX);
        qtest_writel(to, DE + 0x1f8, 0);
        g_assert_cmphex(gl_transfer_probe_read(to, &t), ==,
                        0x89674523 ^ 0x13579bdf);
        if (unsupported) {
            gl_transfer_failure(to, path, DE + 0x9f0, 0x12345678,
                                4, false, true);
            qtest_writel(to, DE + 0xa8, 4);
        }
        g_assert_cmpint(memtx_writel(to, DE + 0x9f0, 7), ==, MEMTX_OK);
        g_assert_cmphex(gl_transfer_probe_read(to, &t), ==, 0x89674523);
        qtest_system_reset(to);
        g_assert_cmphex(gl_transfer_probe_read(to, &t), ==, 0x89674523);
        gl_transfer_snapshot(to, path, &after);
        for (unsigned n = 0; n < RENDER_WORDS; n++) {
            g_assert_cmphex(after.renderer.words[n], ==, 0);
        }
        for (unsigned n = 0; n < 32; n++) {
            g_assert_cmphex(after.linear[n], ==, 0);
        }
        qtest_quit(from);
        qtest_quit(to);
    }
    g_unlink(sock);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_transfer_envelopes(void)
{
    QTestState *s = start_indexed_state();
    GLTransferCase t = gl_transfer_case();

    t.width = 1;
    t.height = 32;
    t.address = 0xf2345000;
    t.advance = 4096;
    gl_transfer_run(s, &t);
    t.source_mode = 0x628;
    t.destination_mode = 0x1628;
    t.linear = 0x98765000;
    t.stride = 4096;
    gl_transfer_run(s, &t);
    qtest_quit(s);
}

static void test_gl_transfer_allocation(void)
{
#ifdef CONFIG_LINUX
    const uint64_t headroom[] = {16 * MiB, 320 * MiB};
    g_autofree char *dir =
        g_dir_make_tmp("crime-gl-transfer-alloc-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);

    for (unsigned n = 0; n < ARRAY_SIZE(headroom); n++) {
        g_autofree char *log = g_strdup_printf("%s/errors-%u.txt", dir, n);
        g_autofree char *args = g_strdup_printf("-D %s", log);
        QTestState *s = start_profile_ram("legacy-software", false,
                                          true, false, args);
        GLTransferCase t = gl_transfer_case();
        g_autofree char *stat_path =
            g_strdup_printf("/proc/%ld/statm", (long)qtest_pid(s));
        g_autofree char *stat = NULL, *errors = NULL;
        struct rlimit before, limited;
        unsigned long long pages;

        /* Limit this disposable child and restore its original allowance. */
        qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
        triangle_irq_baseline(s);
        t.source_mode = 0x628;
        t.width = t.height = 2048;
        t.x = t.y = t.sx = t.sy = 0;
        gl_transfer_stage(s, &t);
        g_assert_true(g_file_get_contents(stat_path, &stat, NULL, NULL));
        g_assert_cmpint(sscanf(stat, "%llu", &pages), ==, 1);
        g_assert_cmpint(prlimit(qtest_pid(s), RLIMIT_AS, NULL, &before), ==, 0);
        limited = before;
        limited.rlim_cur = pages * (uint64_t)qemu_real_host_page_size() +
                           headroom[n];
        g_assert_cmpuint(limited.rlim_cur, <=, before.rlim_max);
        g_assert_cmpint(prlimit(qtest_pid(s), RLIMIT_AS, &limited, NULL),
                        ==, 0);
        gl_transfer_failure(s, path, DE + 0x8d0, 0x12345678, 4, false, true);
        g_assert_cmpint(prlimit(qtest_pid(s), RLIMIT_AS, &before, NULL), ==, 0);
        qtest_quit(s);
        g_assert_true(g_file_get_contents(log, &errors, NULL, NULL));
        g_assert_nonnull(strstr(errors, "GL transfer plan allocation failed"));
        g_assert_cmpint(g_unlink(log), ==, 0);
    }
    g_unlink(path);
    g_rmdir(dir);
#else
    g_test_skip("requires Linux per-process address-space limits");
#endif
}

static void test_shade_start_family(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-shade-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    Shape c = shape_default();

    c.bytes = 4;
    c.width = c.height = 4;
    c.primitive = 0;
    c.x0 = c.y0 = 1;
    c.foreground = 0x1234ab67;
    c.old = 0x89abcdef;
    map_region(s, 0, 4, 0, 0, 3, 3);
    for (unsigned n = 0; n < 12; n++) {
        g_autofree uint32_t *expected = shape_reference(&c);

        shape_memory(s, &c, NULL);
        shape_stage(s, &c);
        for (unsigned word = 0; word < 12; word++) {
            qtest_writel(s, DE + 0xe0 + 4 * word, 0xa5000000U + word);
        }
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                g_assert_cmphex(pixel_read(s, pixel_address(0, x, y, 4), 4),
                                ==, c.old);
            }
        }
        save_paired_renderer(s, path, before);
        /* XOR distinguishes one execution from zero or two. */
        g_assert_cmpint(memtx_writel(s, DE + 0x8e0 + 4 * n,
                                    0xa50ff000U + n), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
        save_paired_renderer(s, path, after);
        before[192 + (0xe0 + 4 * n) / 4] = 0xa50ff000U + n;
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static const unsigned gl_rectangle_modes[] = {
    0x228, 0x229, 0x218, 0x219, 0x124,
    0x125, 0x224, 0x225, 0x226, 0x227,
};

static Shape gl_rectangle_prepare(QTestState *s, unsigned mode, unsigned bank)
{
    Shape c = pipeline_prepare(s, mode, bank);

    sz_rectangle(s, 2, 0, 0, 3, 3);
    qtest_writel(s, DE, UINT32_MAX);
    qtest_writel(s, DE + 0x50, UINT32_MAX);
    qtest_writeq(s, DE + 0x70, UINT64_MAX);
    qtest_writeq(s, DE + 0x90, UINT64_MAX);
    for (unsigned offset = 0xe0; offset <= 0x108; offset += 8) {
        qtest_writeq(s, DE + offset, UINT64_MAX);
    }
    qtest_writeq(s, DE + 0x190, UINT64_MAX);
    qtest_writel(s, DE + 0x198, UINT32_MAX);
    qtest_writel(s, DE + 0x1b8, 0);
    return c;
}

static void test_gl_rectangle_geometry(void)
{
    const uint32_t prefixes[] = { 0, 0x00800000, 0x48000000 };
    const int origins[] = { -4096, 0, 2 };
    QTestState *s = start_indexed_state();

    for (unsigned direction = 0; direction < 4; direction++) {
        for (unsigned form = 0; form < 3; form++) {
            for (unsigned o = 0; o < 3; o++) {
                Shape c = gl_rectangle_prepare(s, 0x228, 0);
                unsigned base = origins[o] < 0 ? 4096 : 0;
                unsigned low = base + origins[o] + 1, high = low + 2;
                unsigned x0 = base + (direction & 1 ? 3 : 1);
                unsigned x1 = base + (direction & 1 ? 1 : 3);
                unsigned y0 = base + (direction & 2 ? 1 : 3);
                unsigned y1 = base + (direction & 2 ? 3 : 1);
                uint32_t expected[64];
                uint64_t scissor = bounds(base + 1, base + 2,
                                           base + 3, base + 4);
                uint64_t a = bounds(low, low, low + 2, high + 1);
                uint64_t b = bounds(low + 1, low, high + 1, high + 1);

                c.width = c.height = 8;
                map_region(s, 0, 4, 0, 0, 7, 7);
                shape_memory(s, &c, NULL);
                qtest_writel(s, DE + 0x60, 0x03000000 | direction << 16);
                qtest_writel(s, DE + 0x58,
                            xy((uint16_t)origins[o], (uint16_t)origins[o]));
                qtest_writeq(s, DE + 0x80,
                    (uint64_t)(prefixes[form] + x0 * 64) << 32 |
                    (prefixes[form] + y0 * 64));
                qtest_writeq(s, DE + 0x88,
                    (uint64_t)(prefixes[form] + x1 * 64) << 32 |
                    (prefixes[form] + y1 * 64));
                qtest_writel(s, DE + 0x18, 0x511278);
                qtest_writel(s, DE + 0xd0, 0x13579bdf);
                qtest_writel(s, DE + 0x1b0, 6);
                qtest_writel(s, DE + 0x10, 0x63);
                qtest_writeq(s, DE + 0x20, a);
                qtest_writeq(s, DE + 0x28, b);
                qtest_writeq(s, DE + 0x48, scissor);
                for (unsigned y = 0; y < 8; y++) {
                    for (unsigned x = 0; x < 8; x++) {
                        bool covered = x >= low && x <= high &&
                            y >= low && y <= high &&
                            inside_bounds(scissor, x - origins[o],
                                          y - origins[o]) &&
                            (inside_bounds(a, x, y) || inside_bounds(b, x, y));

                        expected[8 * y + x] = c.old ^
                            (covered ? 0x13579bdf : 0);
                    }
                }
                g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
                shape_memory(s, &c, expected);
            }
        }
    }
    for (unsigned direction = 0; direction < 4; direction++) {
        Shape c = gl_rectangle_prepare(s, 0x228, 0);
        uint32_t expected[16];

        sz_rectangle(s, direction, 1, 2, 1, 2);
        qtest_writel(s, DE + 0xd0, 0x13579bdf);
        for (unsigned i = 0; i < 16; i++) {
            expected[i] = i == 9 ? 0x13579bdf : c.old;
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    /* Clip each framebuffer edge after composing a signed origin. */
    for (unsigned corner = 0; corner < 4; corner++) {
        Shape c = gl_rectangle_prepare(s, 0x228, 0);
        int lo_x = corner & 1 ? 2045 : -1;
        int lo_y = corner & 2 ? 2045 : -1;
        uint32_t expected[16];

        c.x = corner & 1 ? 2044 : 0;
        c.y = corner & 2 ? 2044 : 0;
        map_region(s, 0, 4, c.x, c.y, c.x + 3, c.y + 3);
        shape_memory(s, &c, NULL);
        qtest_writel(s, DE + 0x58, 0xf000f000);
        sz_rectangle(s, 2, lo_x + 4096, lo_y + 4096,
                     lo_x + 4101, lo_y + 4101);
        qtest_writel(s, DE + 0xd0, 0x13579bdf);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                expected[4 * y + x] = (int)(c.x + x) >= lo_x &&
                    (int)(c.x + x) <= lo_x + 5 &&
                    (int)(c.y + y) >= lo_y &&
                    (int)(c.y + y) <= lo_y + 5 ? 0x13579bdf : c.old;
            }
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
}

static void test_gl_rectangle_output(gconstpointer selection)
{
    unsigned mode = gl_rectangle_modes[GPOINTER_TO_UINT(selection)];
    QTestState *s = start_indexed_state();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned logic = 0; logic < 16; logic++) {
            for (unsigned lanes = 0; lanes < 16; lanes++) {
                unsigned upper = logic & 4 ? 0xffffe000U : 0;
                Shape c = gl_rectangle_prepare(s, mode | upper, bank);
                unsigned draw = 0x400280 | lanes << 3 |
                    (logic & 1 ? 0x1000 : 0) |
                    (logic & 2 ? 0x10000 : 0) |
                    (lanes & 1 ? 0x100 : 0) |
                    (lanes & 2 ? 0x400 : 0) | (logic & 4 ? 2 : 0);
                unsigned function = ((logic + lanes) % 5) << 8 |
                    (logic % 13) << 4 | lanes % 12;
                uint32_t plane = lanes & 4 ? 0xa579b3e1 : UINT32_MAX;

                if (!(draw & 0x400)) {
                    function = UINT32_MAX;
                }
                pipeline_check(s, &c, mode, 0x13579b80, 0x9b2371d6,
                               function, draw, logic, plane);
            }
        }
    }
    qtest_quit(s);
}

static void test_gl_rectangle_alpha(void)
{
    const unsigned alphas[] = { 0, 1, 127, 254, 255 };
    QTestState *s = start_indexed_state();

    for (unsigned m = 0; m < ARRAY_SIZE(gl_rectangle_modes); m++) {
        for (unsigned function = 0; function < 8; function++) {
            for (unsigned a = 0; a < ARRAY_SIZE(alphas); a++) {
                for (unsigned r = 0; r < 3; r++) {
                    unsigned mode = gl_rectangle_modes[m];
                    Shape c = gl_rectangle_prepare(s, mode, m % 3);
                    unsigned reference = r == 0 ? 0 : r == 1 ? alphas[a] : 255;
                    bool pass = sz_reference_compare(function, alphas[a],
                                                      reference);
                    unsigned draw = 0x411d78;
                    uint32_t source = 0x13579b00 | alphas[a], expected[16];

                    qtest_writel(s, DE + 0x18, draw);
                    qtest_writel(s, DE + 0x198, function << 8 | reference);
                    qtest_writel(s, DE + 0x1a8, 0x45);
                    qtest_writel(s, DE + 0xd0, source);
                    if (!pass) {
                        tile(s, m % 3, 0, 0x7fff);
                    }
                    for (unsigned y = 0; y < 4; y++) {
                        for (unsigned x = 0; x < 4; x++) {
                            expected[4 * y + x] = pass ? pipeline_reference(
                                mode, source, c.old, 0, 0x45, draw,
                                3, UINT32_MAX, x, y) : c.old;
                        }
                    }
                    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==,
                                    MEMTX_OK);
                    shape_memory(s, &c, expected);
                }
            }
        }
    }
    qtest_quit(s);
}

static void test_gl_rectangle_sz(void)
{
    QTestState *s = start_indexed_state();
    SZCase v = sz_case();

    v.rectangle = true;
    for (unsigned function = 0; function < 8; function++) {
        for (unsigned value = 16; value <= 18; value++) {
            v.draw = 0x41177f;
            v.mode = gl_rectangle_modes[(function + value) % 10];
            v.bank = function % 2;
            v.control = function << 25;
            v.start = value * 4096;
            v.stencil = 0x62ff7003;
            sz_run(s, &v);
        }
    }
    for (unsigned outcome = 0; outcome < 3; outcome++) {
        for (unsigned operation = 0; operation < 6; operation++) {
            for (unsigned old = 0; old < 3; old++) {
                v.draw = 0x41177f;
                v.control = (outcome == 1 ? 0 : 7) << 25;
                v.old = (old == 0 ? 0 : old == 1 ? 0x53 : 255) << 24 | 17;
                v.stencil = 0x62ff0000 | (outcome == 0 ? 0 : 7) << 12 |
                    operation << 8 | operation << 4 | operation;
                v.mask = 0x5a;
                sz_run(s, &v);
            }
        }
    }
    v = sz_case();
    v.rectangle = true;
    v.draw = 0x41177f;
    for (unsigned function = 5; function <= 7; function += 2) {
        for (unsigned materialize = 0; materialize < 2; materialize++) {
            for (unsigned write = 0; write < 2; write++) {
                for (unsigned pass = 0; pass < 2; pass++) {
                    for (unsigned equal = 0; equal < 2; equal++) {
                        v.draw = (v.draw & ~2U) | write << 1;
                        v.mask = write ? 255 : 0;
                        v.control = (pass ? 7 : 0) << 25 | 0x01000010;
                        v.stencil = 0x62ff0002 | function << 12 |
                                    (materialize ? 0x20 : 0);
                        v.old = (equal ? 0x62 : 0x53) << 24 | 17;
                        sz_run(s, &v);
                    }
                }
            }
        }
    }
    for (unsigned edge = 0; edge < 2; edge++) {
        v = sz_case();
        v.rectangle = true;
        v.draw = 0x41177f;
        v.start = edge ? INT64_C(1) << 36 : -(INT64_C(1) << 36);
        sz_run(s, &v);
    }
    v = sz_case();
    v.rectangle = true;
    v.draw = 0x411fff;
    qtest_writel(s, DE + 0x198, 0x240);
    sz_run(s, &v);
    /* Stencil alone never consumes the full-width dormant Z plane. */
    {
        Shape c = gl_rectangle_prepare(s, 0x227, 0);
        uint32_t expected[16];

        sz_initialize(s, &c, 0x53000011);
        v.draw = 0x411379;
        v.control = 0xfeffffff;
        v.stencil = 0x62ff7003;
        v.start = INT64_MIN;
        v.dx = INT64_MAX;
        v.dy = -1;
        sz_program(s, &v);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                expected[4 * y + x] = pipeline_reference(0x227, v.color,
                    c.old, 0, 0x45, v.draw, 6, v.plane, x, y);
            }
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
        g_assert_cmphex(pixel_read(s, pixel_address(2, 1, 1, 4), 4), ==,
                        0x54000011);
    }
    /* NEVER alpha precedes strict SZ access, but cannot hide bad Z state. */
    {
        Shape c = gl_rectangle_prepare(s, 0x227, 0);
        uint32_t expected[16];

        sz_initialize(s, &c, 0x53000011);
        v = sz_case();
        v.draw = 0x411fff;
        sz_program(s, &v);
        qtest_writel(s, DE + 0x198, 0);
        qtest_writeq(s, RE + 0x1400, 0);
        tile(s, 0, 0, 0x7fff);
        for (unsigned i = 0; i < 16; i++) {
            expected[i] = c.old;
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
        g_assert_cmphex(pixel_read(s, pixel_address(2, 1, 1, 4), 4), ==,
                        0x53000011);
    }
    qtest_quit(s);
}

static void test_gl_rectangle_failures(void)
{
    const unsigned unsupported[] = {
        0x2000, 0x4000, 0x8000, 0x20000, 0x40000, 0x80000,
        0x200000, 0x800000, 0x1000000, 0x80000000,
    };
    const Register triggers[] = {
        { 0x8d0, 4 }, { 0x870, 8 }, { 0x8a8, 8 }, { 0x8c0, 8 },
    };
    g_autofree char *dir = g_dir_make_tmp("crime-gl-rect-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned bad = 0; bad < 28 + ARRAY_SIZE(unsupported); bad++) {
        Shape c = gl_rectangle_prepare(s, 0x227, 0);
        SZCase v = sz_case();

        v.draw = 0x511fff;
        v.plane = 0;
        sz_initialize(s, &c, v.old);
        sz_program(s, &v);
        qtest_writel(s, DE + 0x198, 0);
        qtest_writeq(s, DE + 0x48, bounds(10, 10, 11, 11));
        switch (bad) {
        case 0:
            qtest_writel(s, DE + 8, 0x234);
            break;
        case 1:
            qtest_writel(s, DE + 8, 0x328);
            break;
        case 2:
            qtest_writel(s, DE + 0x60, 0x03020001);
            break;
        case 3:
            qtest_writel(s, DE + 0x60, 0x03060000);
            break;
        case 4:
            qtest_writel(s, DE + 0x80, 1);
            break;
        case 5:
            qtest_writel(s, DE + 0x84, 0x480c0000);
            break;
        case 6:
            qtest_writel(s, DE + 0x88, 0x7fc00000);
            break;
        case 7:
            sz_rectangle(s, 2, 3, 3, 0, 0);
            break;
        case 8:
            qtest_writel(s, DE + 0x10, 0x800);
            break;
        case 9:
            qtest_writel(s, DE + 0x10, 0x41);
            break;
        case 10:
            qtest_writel(s, DE + 0x10, 0x20);
            qtest_writeq(s, DE + 0x20, bounds(2, 2, 1, 3));
            break;
        case 11:
            qtest_writeq(s, DE + 0x48, bounds(2, 2, 1, 3));
            break;
        case 12:
            qtest_writel(s, DE + 0x1a8, 0x500);
            break;
        case 13:
            qtest_writel(s, DE + 0x1a8, 0xd0);
            break;
        case 14:
            qtest_writel(s, DE + 0x1a8, 0xc);
            break;
        case 15:
            qtest_writel(s, DE + 0x1a8, 0x1000);
            break;
        case 16:
            qtest_writel(s, DE + 0x198, 0x800);
            break;
        case 17:
            qtest_writel(s, DE + 0x198, 0x1000);
            break;
        case 18:
            qtest_writeq(s, DE + 0x1c8, (INT64_C(1) << 36) + 1);
            break;
        case 19:
            qtest_writeq(s, DE + 0x1c8, -(INT64_C(1) << 36) - 1);
            break;
        case 20:
            qtest_writeq(s, DE + 0x1d0, 1);
            break;
        case 21:
            qtest_writeq(s, DE + 0x1d8, -1);
            break;
        case 22:
            qtest_writel(s, DE + 0x1c0, 0x10000000);
            break;
        case 23:
            qtest_writel(s, DE + 0x1e0, 0x62ff7006);
            break;
        case 24:
            qtest_writel(s, DE + 0x1e8, 0x100);
            break;
        case 25:
            qtest_writel(s, DE + 0x1c0, 0x01000000);
            break;
        case 26:
            qtest_writel(s, DE + 0x1b0, 16);
            break;
        case 27:
            qtest_writel(s, DE + 8, 0x1228);
            break;
        default:
            qtest_writel(s, DE + 0x18, v.draw | unsupported[bad - 28]);
            break;
        }
        const Register *trigger = &triggers[bad % ARRAY_SIZE(triggers)];

        sz_atomic_failure(s, &c, path, DE + trigger->offset,
            trigger->width == 4 ? 0xf13579bd : 0xa579b3e16c28f04dULL,
            trigger->width);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_rectangle_mappings(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-gl-rect-map-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *log = g_build_filename(dir, "errors", NULL);
    g_autofree char *options = g_strdup_printf("-D %s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_profile_ram("legacy-software", false, true, false,
                                      options);

    for (unsigned kind = 0; kind < 14; kind++) {
        Shape c = gl_rectangle_prepare(s, 0x228, 0);
        SZCase v = sz_case();
        uint32_t expected[258];
        bool failed = (kind >= 2 && kind <= 5) || kind == 8 || kind == 10;

        c.width = 258;
        c.height = 1;
        map_region(s, 0, 4, 0, 0, 257, 0);
        shape_memory(s, &c, NULL);
        sz_initialize(s, &c, v.old);
        v.draw = 0x41177f;
        v.stencil = 0x62ff7003;
        sz_program(s, &v);
        sz_rectangle(s, 2, 0, 0, 257, 0);
        qtest_writeq(s, RE + 0x1000, 0x00007fff00000000ULL);
        /* Reprogram explicitly: invalid middle payload must not become RAM. */
        tile(s, 0, 0, page_for(0, 0));
        tile(s, 0, 2, page_for(0, 2));
        switch (kind) {
        case 1:
            qtest_writeq(s, RE + 0x1000, 0x7fff7fff7fff0000ULL);
            break;
        case 2:
            tile(s, 0, 2, 0x400);
            break;
        case 3:
            tile(s, 0, 2, page_for(0, 0));
            break;
        case 4:
            qtest_writeq(s, RE + 0x1000, 0x7fff7fff7fff0000ULL);
            qtest_writeq(s, RE + 0x1400, 0);
            break;
        case 5:
            tile(s, 2, 0, page_for(0, 0));
            break;
        case 6:
            qtest_writel(s, DE + 0x18, v.draw | 0x800);
            qtest_writel(s, DE + 0x198, 0);
            qtest_writeq(s, RE + 0x1400, 0);
            tile(s, 0, 0, 0x7fff);
            break;
        case 7:
            qtest_writel(s, DE + 0x1c0, 0);
            tile(s, 0, 0, 0x7fff);
            break;
        case 8:
            tile(s, 0, 2, 0x4000 | page_for(0, 0));
            break;
        case 9:
            qtest_writel(s, DE + 0x18, v.draw & ~3U);
            tile(s, 2, 2, page_for(2, 0));
            break;
        case 10:
            tile(s, 2, 2, page_for(2, 0));
            break;
        case 11:
        case 12:
            qtest_writel(s, DE + 0x18,
                        (kind == 11 ? v.draw : v.draw & ~7U) | 0x80);
            qtest_writel(s, DE + 0x1b8, 0);
            tile(s, 0, 0, 0x7fff);
            break;
        case 13:
            qtest_writel(s, DE + 8, 0x227);
            qtest_writel(s, DE + 0x18, (v.draw & ~0x7fU) | 0x18);
            tile(s, 0, 0, 0x7fff);
            break;
        }
        if (failed) {
            sz_atomic_failure(s, &c, path, DE + 0x8d0, 0xf13579bd, 4);
            continue;
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        for (unsigned x = 0; x < c.width; x++) {
            bool colored = (kind == 0 || kind == 9) && (x < 128 || x >= 256);

            expected[x] = colored ? pipeline_reference(0x228, v.color,
                c.old, 0, 0x45, v.draw, 6, v.plane, x, 0) : c.old;
            g_assert_cmphex(pixel_read(s, pixel_address(2, x, 0, 4), 4), ==,
                kind == 6 || kind == 9 || kind >= 12 ? v.old :
                kind == 7 ? 0x53000011 : 0x54000012);
        }
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
    g_assert_true(g_file_get_contents(log, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "rectangle suppressed 128 invalid"));
    g_assert_nonnull(strstr(contents, "rectangle suppressed 258 invalid"));
    {
        g_auto(GStrv) summaries = g_strsplit(contents,
            "invalid destination fragments", -1);

        g_assert_cmpuint(g_strv_length(summaries), ==, 4);
    }
    g_unlink(log);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_rectangle_starts(void)
{
    Register forms[64];
    unsigned count = gl_transfer_starts(forms);
    g_autofree char *dir = g_dir_make_tmp("crime-gl-rect-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < count; n++) {
        const Register *form = &forms[n];
        Shape c = gl_rectangle_prepare(s, 0x228, 0);
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS], value;
        bool pair = form->width == 8 && (form->offset == 0x70 ||
            form->offset == 0xa8 || form->offset == 0xc0);
        uint32_t expected[16];

        if (form->offset == 0xb0 || form->offset == 0xb4) {
            continue;
        }
        qtest_writel(s, DE + 0x18, 0x411278);
        qtest_writel(s, DE + 0xd0, 0x13579bdf);
        qtest_writel(s, DE + 0x1b0, 6);
        save_paired_renderer(s, path, before);
        value = form->offset == 0x1f0 ? 0xdeadbeef :
                before[192 + form->offset / 4];
        if (pair) {
            value = value << 32 | before[192 + form->offset / 4 + 1];
        }
        if (form->offset != 0x1f0) {
            g_assert_cmpint(memtx_write(s, DE + form->offset,
                value ^ (pair ? 0x100000001000ULL : 0x1000), form->width), ==,
                MEMTX_OK);
        }
        g_test_message("rectangle START +0x%x width %u", form->offset,
                       form->width);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + form->offset,
                                    value, form->width), ==, MEMTX_OK);
        for (unsigned i = 0; i < 16; i++) {
            expected[i] = c.old ^ 0x13579bdf;
        }
        shape_memory(s, &c, expected);
        save_paired_renderer(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        g_assert_cmpint(memtx_read(s, DE + 0x800 + form->offset,
                                   &value, form->width), !=, MEMTX_OK);
        g_assert_cmpint(memtx_write(s, DE + 0x801 + form->offset,
                                    value, form->width), !=, MEMTX_OK);
        for (unsigned poll = 0; poll < 2; poll++) {
            qtest_readl(s, STATUS);
            qtest_writel(s, DE + 0x1f0, 0xdeadbeef);
            qtest_writel(s, DE + 0x1f8, 0);
        }
        shape_memory(s, &c, expected);
        save_paired_renderer(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_rectangle_start_refusals(void)
{
    const Register forms[] = {
        { 0x990, 4 }, { 0x994, 4 }, { 0x990, 8 },
        { 0x998, 4 }, { 0x9a0, 4 }, { 0x9a8, 4 },
        { 0x8b0, 4 }, { 0x8b4, 4 }, { 0x880, 8 },
        { 0x8e0, 8 }, { 0x9f0, 8 }, { 0x8d0, 8 },
        { 0x9c8, 4 }, { 0x9c0, 8 }, { 0x820, 4 },
    };
    g_autofree char *dir = g_dir_make_tmp("crime-gl-rect-width-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    Shape c = gl_rectangle_prepare(s, 0x227, 0);

    sz_initialize(s, &c, 0x53000011);
    qtest_writel(s, DE + 0x18, 0x411578);
    qtest_writel(s, DE + 0x1a8, 0x45);
    for (unsigned n = 0; n < ARRAY_SIZE(forms); n++) {
        sz_atomic_failure(s, &c, path, DE + forms[n].offset,
                          0xa579b3e16c28f04dULL, forms[n].width);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_rectangle_allocation(void)
{
#ifdef CONFIG_LINUX
    g_autofree char *dir = g_dir_make_tmp("crime-gl-rect-alloc-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *log = g_build_filename(dir, "errors", NULL);
    g_autofree char *args = g_strdup_printf("-D %s", log);
    QTestState *s = start_profile_ram("legacy-software", false, true, false,
                                      args);
    Shape c = gl_rectangle_prepare(s, 0x227, 0);
    g_autofree char *stat_path =
        g_strdup_printf("/proc/%ld/statm", (long)qtest_pid(s));
    g_autofree char *stat = NULL, *errors = NULL;
    struct rlimit before, limited;
    unsigned long long pages;

    sz_initialize(s, &c, 0x53000011);
    sz_rectangle(s, 2, 0, 0, 2047, 2047);
    qtest_writel(s, DE + 0x18, 0x411578);
    qtest_writel(s, DE + 0x1a8, 0x45);
    g_assert_true(g_file_get_contents(stat_path, &stat, NULL, NULL));
    g_assert_cmpint(sscanf(stat, "%llu", &pages), ==, 1);
    g_assert_cmpint(prlimit(qtest_pid(s), RLIMIT_AS, NULL, &before), ==, 0);
    limited = before;
    limited.rlim_cur = pages * (uint64_t)qemu_real_host_page_size() + 16 * MiB;
    g_assert_cmpuint(limited.rlim_cur, <=, before.rlim_max);
    g_assert_cmpint(prlimit(qtest_pid(s), RLIMIT_AS, &limited, NULL), ==, 0);
    sz_atomic_failure(s, &c, path, DE + 0x8d0, 0xf13579bd, 4);
    g_assert_cmpint(prlimit(qtest_pid(s), RLIMIT_AS, &before, NULL), ==, 0);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(log, &errors, NULL, NULL));
    g_assert_nonnull(strstr(errors, "primitive allocation failed"));
    g_unlink(log);
    g_unlink(path);
    g_rmdir(dir);
#else
    g_test_skip("requires Linux per-process address-space limits");
#endif
}

static void test_gl_rectangle_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-gl-rect-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *from = start(), *to = start_options("-incoming defer");
    Shape c = gl_rectangle_prepare(from, 0x227, 0);
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    uint32_t expected[16];

    qtest_writel(from, DE + 0x18, 0x411378);
    qtest_writel(from, DE + 0x1b0, 6);
    qtest_writel(from, DE + 0xd0, 0x13579bdf);
    g_assert_cmpint(memtx_writel(from, DE + 0x9f0, 0), ==, MEMTX_OK);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            expected[4 * y + x] = pipeline_reference(0x227, 0x13579bdf,
                c.old, 0, 0, 0x411378, 6, UINT32_MAX, x, y);
        }
    }
    qtest_writel(from, DE + 0x80, 0x7fc00000);
    save_paired_renderer(from, path, before);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    save_paired_renderer(to, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    shape_memory(to, &c, expected);
    qtest_writel(to, DE + 0x80, 0);
    g_assert_cmpint(memtx_writel(to, DE + 0x9f0, 0), ==, MEMTX_OK);
    for (unsigned i = 0; i < 16; i++) {
        expected[i] = c.old;
    }
    shape_memory(to, &c, expected);
    qtest_system_reset(to);
    shape_memory(to, &c, expected);
    save_renderer(to, path, after);
    for (unsigned i = 0; i < RENDER_WORDS; i++) {
        g_assert_cmphex(after[i], ==, 0);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_rectangle_clock(void)
{
    QTestState *s = start_indexed_state();
    Shape c = pipeline_prepare(s, 0x227, 0);
    uint32_t expected[60];

    c.x = 79;
    c.y = 37;
    c.width = 6;
    c.height = 10;
    shape_memory(s, &c, NULL);
    qtest_writel(s, DE + 0x18, 0x00411578);
    qtest_writel(s, DE + 0x60, 0x03020000);
    qtest_writel(s, DE + 0x58, 0xf020f020);
    qtest_writeq(s, DE + 0x80, 0x48040c0048040180ULL);
    qtest_writeq(s, DE + 0x88, 0x48040cc048040340ULL);
    qtest_writel(s, DE + 0xd0, 0xccccccff);
    qtest_writel(s, DE + 0x1a8, 0x45);
    qtest_writel(s, DE + 0x1b8, 0);
    qtest_writel(s, DE + 0x10, 0x21);
    qtest_writeq(s, DE + 0x20, bounds(32, 32, 132, 132));
    for (unsigned y = 0; y < c.height; y++) {
        for (unsigned x = 0; x < c.width; x++) {
            bool covered = x >= 1 && x <= 4 && y >= 1 && y <= 8;

            expected[y * c.width + x] = covered ? pipeline_reference(
                0x227, 0xccccccff, c.old, 0, 0x45, 0x411578,
                3, UINT32_MAX, c.x + x, c.y + y) : c.old;
        }
    }
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0xdeadbeef), ==, MEMTX_OK);
    shape_memory(s, &c, expected);
    qtest_quit(s);
}

static Shape gl_point_prepare(QTestState *s, unsigned mode, unsigned bank)
{
    Shape c = gl_rectangle_prepare(s, mode, bank);

    qtest_writel(s, DE + 0x60, 0);
    qtest_writeq(s, DE + 0x80, UINT64_C(90) << 32 | 153);
    qtest_writeq(s, DE + 0x88, UINT64_MAX);
    qtest_writel(s, DE + 0xa0, UINT32_MAX);
    qtest_writeq(s, DE + 0xa8, UINT64_MAX);
    qtest_writeq(s, DE + 0xc0, UINT64_MAX);
    qtest_writel(s, DE + 0xd8, UINT32_MAX);
    return c;
}

static void gl_point_vertex(QTestState *s, int x64, int y64,
                              int ox, int oy, uint32_t prefix)
{
    int64_t ux = x64 - 64 * (int64_t)ox;
    int64_t uy = y64 - 64 * (int64_t)oy;

    g_assert_cmpint(ux, >=, 0);
    g_assert_cmpint(ux, <=, 0x7ffff);
    g_assert_cmpint(uy, >=, 0);
    g_assert_cmpint(uy, <=, 0x7ffff);
    qtest_writel(s, DE + 0x58, xy((uint16_t)ox, (uint16_t)oy));
    qtest_writeq(s, DE + 0x80,
        (uint64_t)(prefix + ux) << 32 | (prefix + uy));
}

static void gl_point_check(QTestState *s, const Shape *c, unsigned mode,
                             int x, int y, uint32_t source, unsigned draw,
                             unsigned function, unsigned logic, uint32_t plane)
{
    uint32_t expected[16];

    shape_memory(s, c, NULL);
    qtest_writel(s, DE + 0x18, draw);
    qtest_writel(s, DE + 0x1a0, 0x9b2371d6);
    qtest_writel(s, DE + 0x1a8, function);
    qtest_writel(s, DE + 0x1b0, logic);
    qtest_writel(s, DE + 0x1b8, plane);
    for (unsigned j = 0; j < 4; j++) {
        for (unsigned i = 0; i < 4; i++) {
            expected[4 * j + i] = (int)i == x && (int)j == y ?
                pipeline_reference(mode, source, c->old, 0x9b2371d6,
                    function, draw, logic, plane, i, j) : c->old;
        }
    }
    g_assert_cmpint(memtx_writel(s, DE + 0x8d0, source), ==, MEMTX_OK);
    shape_memory(s, c, expected);
}

static void test_gl_point_cells(void)
{
    const uint32_t prefixes[] = { 0, 0x00800000, 0x48000000 };
    QTestState *s = start_indexed_state();
    Shape c = gl_point_prepare(s, 0x228, 0);

    c.width = c.height = 2;
    qtest_writel(s, DE + 0x18, 0x411078);
    for (unsigned form = 0; form < 4; form++) {
        bool negative = form == 3;
        uint32_t prefix = prefixes[form % 3];

        for (unsigned fy = 0; fy < 64; fy++) {
            for (unsigned fx = 0; fx < 64; fx++) {
                uint32_t expected[4];
                int x = negative ? (fx >= 32 ? 0 : -1) : (fx >= 32);
                int y = negative ? (fy > 32 ? 0 : -1) : (fy > 32);

                shape_memory(s, &c, NULL);
                gl_point_vertex(s, (negative ? -64 : 0) + fx,
                    (negative ? -64 : 0) + fy, -4096, -4096, prefix);
                for (unsigned j = 0; j < 2; j++) {
                    for (unsigned i = 0; i < 2; i++) {
                        expected[2 * j + i] = (int)i == x && (int)j == y ?
                            0x13579bdf : c.old;
                    }
                }
                g_assert_cmpint(memtx_writel(s, DE + 0x8d0, 0x13579bdf), ==,
                                MEMTX_OK);
                shape_memory(s, &c, expected);
            }
        }
    }
    qtest_quit(s);
}

static void test_gl_point_edges(void)
{
    const struct {
        int x64, y64, ox, oy, x, y;
    } vectors[] = {
        { -33, 0, -4096, -4096, -1, 0 },
        { -32, 0, -4096, -4096, 0, 0 },
        { -31, 0, -4096, -4096, 0, 0 },
        { 0, -33, -4096, -4096, 0, -1 },
        { 0, -32, -4096, -4096, 0, -1 },
        { 0, -31, -4096, -4096, 0, 0 },
        { 2047 * 64 + 31, 2047 * 64, 0, 0, 2047, 2047 },
        { 2047 * 64 + 32, 2047 * 64, 0, 0, 2048, 2047 },
        { 2047 * 64, 2047 * 64 + 32, 0, 0, 2047, 2047 },
        { 2047 * 64, 2047 * 64 + 33, 0, 0, 2047, 2048 },
        { -32768 * 64, 0, -32768, 0, -32768, 0 },
        { 40958 * 64 + 63, 0, 32767, 0, 40959, 0 },
    };
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(vectors); n++) {
        Shape c = gl_point_prepare(s, 0x228, 0);
        uint32_t expected[4];

        c.width = c.height = 2;
        c.x = vectors[n].x >= 2047 ? 2046 : 0;
        c.y = vectors[n].y >= 2047 ? 2046 : 0;
        map_region(s, 0, 4, c.x, c.y, c.x + 1, c.y + 1);
        shape_memory(s, &c, NULL);
        gl_point_vertex(s, vectors[n].x64, vectors[n].y64,
                        vectors[n].ox, vectors[n].oy, 0x48000000);
        for (unsigned y = 0; y < 2; y++) {
            for (unsigned x = 0; x < 2; x++) {
                expected[2 * y + x] = (int)(c.x + x) == vectors[n].x &&
                    (int)(c.y + y) == vectors[n].y ? 0x13579bdf : c.old;
            }
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, 0x13579bdf), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
}

static void test_gl_point_clipping(void)
{
    const uint32_t prefixes[] = { 0, 0x00800000, 0x48000000 };
    const int origins[] = { -4096, -4064, 0, 80 };
    QTestState *s = start_indexed_state();

    for (unsigned encoding = 0; encoding < 12; encoding++) {
        for (unsigned flags = 0; flags < 32; flags++) {
            for (unsigned clip = 0; clip < 5; clip++) {
                Shape c = gl_point_prepare(s, 0x228, 0);
                int ox = origins[encoding % 4];
                int oy = origins[(encoding + 1) % 4];
                int x = flags & 1 ? 85 : 84;
                int y = flags & 2 ? 85 : 84;
                bool visible = clip == 0 || clip == 3 || clip == 4 ||
                    (clip == 1 ? x == 84 && y == 84 : x != 84 || y != 84);
                uint32_t draw = 0x400278 | (flags & 8 ? 0x1000 : 0) |
                                (flags & 16 ? 0x10000 : 0);
                uint32_t expected[16];

                c.x = c.y = 83;
                shape_memory(s, &c, NULL);
                gl_point_vertex(s, 84 * 64 + (flags & 1 ? 32 : 31),
                    84 * 64 + (flags & 2 ? 33 : 32), ox, oy,
                    prefixes[encoding / 4]);
                qtest_writel(s, DE + 0x60,
                            (flags % 4) << 16 | (flags & 4 ? 0x40000 : 0));
                qtest_writel(s, DE + 0x1b0, 6);
                qtest_writel(s, DE + 0x10, clip == 0 ? 0 :
                    clip == 1 ? 0x21 : clip == 2 ? 0x20 : 0x63 |
                    (clip == 4 ? 0x400 : 0));
                qtest_writeq(s, DE + 0x20, bounds(84, 84,
                                                  clip >= 3 ? 86 : 85,
                                                  clip >= 3 ? 86 : 85));
                qtest_writeq(s, DE + 0x28, bounds(84, 84, 85, 86));
                if (clip == 1) {
                    draw |= 0x100000;
                    qtest_writeq(s, DE + 0x48,
                        bounds(84 - ox, 84 - oy, 85 - ox, 85 - oy));
                }
                qtest_writel(s, DE + 0x18, draw);
                for (unsigned j = 0; j < 4; j++) {
                    for (unsigned i = 0; i < 4; i++) {
                        expected[4 * j + i] = c.old ^
                            (visible && c.x + i == x && c.y + j == y ?
                             0x13579bdf : 0);
                    }
                }
                g_assert_cmpint(memtx_writel(s, DE + 0x8d0, 0x13579bdf), ==,
                                MEMTX_OK);
                shape_memory(s, &c, expected);
            }
        }
    }
    qtest_quit(s);
}

static void test_gl_point_output(gconstpointer selection)
{
    unsigned mode = gl_rectangle_modes[GPOINTER_TO_UINT(selection)];
    QTestState *s = start_indexed_state();

    for (unsigned bank = 0; bank < 3; bank++) {
        for (unsigned logic = 0; logic < 16; logic++) {
            for (unsigned lanes = 0; lanes < 16; lanes++) {
                unsigned upper = logic & 4 ? 0xffffe000U : 0;
                Shape c = gl_point_prepare(s, mode | upper, bank);
                unsigned x = logic % 4, y = lanes % 4;
                unsigned draw = 0x400280 | lanes << 3 |
                    (logic & 1 ? 0x10000 : 0) | (lanes & 1 ? 0x1000 : 0) |
                    (logic & 2 ? 0x100 : 0) | (lanes & 2 ? 0x400 : 0) |
                    (logic & 8 ? 2 : 0);
                unsigned function = draw & 0x400 ?
                    ((logic + lanes) % 5) << 8 | (logic % 13) << 4 |
                    lanes % 12 : UINT32_MAX;
                uint32_t plane = lanes & 4 ? 0xa579b3e1 : UINT32_MAX;

                gl_point_vertex(s, x * 64 + 31, y * 64 + 32,
                                -4096, -4096, 0x48000000);
                qtest_writel(s, DE + 0x60,
                            (logic % 4) << 16 | (lanes & 1 ? 0x40000 : 0));
                gl_point_check(s, &c, mode, x, y, 0x13579b80, draw,
                               function, logic, plane);
            }
        }
    }
    qtest_quit(s);
}

static void test_gl_point_alpha(void)
{
    const unsigned alphas[] = { 0, 1, 127, 128, 254, 255 };
    QTestState *s = start_indexed_state();

    for (unsigned m = 0; m < ARRAY_SIZE(gl_rectangle_modes); m++) {
        for (unsigned function = 0; function < 8; function++) {
            for (unsigned a = 0; a < ARRAY_SIZE(alphas); a++) {
                for (unsigned r = 0; r < 3; r++) {
                    unsigned mode = gl_rectangle_modes[m];
                    Shape c = gl_point_prepare(s, mode, m % 3);
                    unsigned reference = r == 0 ? 0 : r == 1 ? alphas[a] : 255;
                    bool pass = sz_reference_compare(function, alphas[a],
                                                      reference);

                    qtest_writel(s, DE + 0xd0, 0xccccccff);
                    qtest_writel(s, DE + 0x198, function << 8 | reference);
                    if (!pass) {
                        tile(s, m % 3, 0, 0x7fff);
                    }
                    gl_point_check(s, &c, mode, pass ? 1 : -1, 2,
                        0x13579b00 | alphas[a], 0x411d78, 0x45, 3,
                        UINT32_MAX);
                }
            }
        }
    }
    qtest_quit(s);
}

static void gl_point_sz_run(QTestState *s, const SZCase *v)
{
    Shape c = gl_point_prepare(s, v->mode, v->bank);
    uint32_t expected[16];
    bool color;
    uint32_t z = sz_reference_depth(v->start);
    uint32_t sz = sz_reference(v, v->old, z, &color);

    sz_initialize(s, &c, v->old);
    sz_program(s, v);
    qtest_writel(s, DE + 0x198, 0x240);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            expected[4 * y + x] = x == 1 && y == 2 && color ?
                pipeline_reference(v->mode, v->color, c.old, 0, 0x45,
                    v->draw, 6, v->plane, x, y) : c.old;
        }
    }
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    shape_memory(s, &c, expected);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4), ==,
                            x == 1 && y == 2 ? sz : v->old);
        }
    }
}

static void test_gl_point_sz(void)
{
    QTestState *s = start_indexed_state();
    SZCase v = sz_case();

    for (unsigned function = 0; function < 8; function++) {
        for (unsigned value = 16; value <= 18; value++) {
            v.draw = 0x41177f;
            v.mode = gl_rectangle_modes[(function + value) % 10];
            v.bank = function % 2;
            v.control = function << 25;
            v.start = value * 4096;
            v.stencil = 0x62ff7003;
            gl_point_sz_run(s, &v);
        }
    }
    for (unsigned outcome = 0; outcome < 3; outcome++) {
        for (unsigned operation = 0; operation < 6; operation++) {
            for (unsigned old = 0; old < 3; old++) {
                v.draw = 0x41177f;
                v.control = (outcome == 1 ? 0 : 7) << 25;
                v.old = (old == 0 ? 0 : old == 1 ? 0x53 : 255) << 24 | 17;
                v.stencil = 0x62ff0000 | (outcome == 0 ? 0 : 7) << 12 |
                    operation << 8 | operation << 4 | operation;
                v.mask = 0x5a;
                gl_point_sz_run(s, &v);
            }
        }
    }
    v = sz_case();
    for (unsigned function = 5; function <= 7; function += 2) {
        for (unsigned replace = 0; replace < 2; replace++) {
            for (unsigned write = 0; write < 2; write++) {
                for (unsigned pass = 0; pass < 2; pass++) {
                    for (unsigned equal = 0; equal < 2; equal++) {
                        v.draw = 0x41177d | write << 1;
                        v.mask = write ? 255 : 0;
                        v.control = (pass ? 7 : 0) << 25 | 0x01000010;
                        v.stencil = 0x62ff0002 | function << 12 |
                                    (replace ? 0x20 : 0);
                        v.old = (equal ? 0x62 : 0x53) << 24 | 17;
                        gl_point_sz_run(s, &v);
                    }
                }
            }
        }
    }
    for (unsigned edge = 0; edge < 2; edge++) {
        v = sz_case();
        v.draw = 0x411fff;
        v.start = edge ? INT64_C(1) << 36 : -(INT64_C(1) << 36);
        gl_point_sz_run(s, &v);
    }
    v = sz_case();
    v.draw = 0x411379;
    v.control = 0xfeffffff;
    v.start = INT64_MIN;
    v.dx = INT64_MAX;
    v.dy = -1;
    v.stencil = 0x62ff7003;
    gl_point_sz_run(s, &v);
    qtest_quit(s);
}

static void test_gl_point_failures(gconstpointer selection)
{
    const Register triggers[] = {
        { 0x8d0, 4 }, { 0x870, 8 }, { 0x8a8, 8 }, { 0x8c0, 8 },
    };
    const unsigned primitives[] = {
        1U << 0, 1U << 1, 1U << 2, 1U << 3,
        1U << 4, 1U << 5, 1U << 6, 1U << 7,
        1U << 8, 1U << 9, 1U << 10, 1U << 11,
        1U << 12, 1U << 13, 1U << 14, 1U << 15,
        31, 65535, 1U << 19, 1U << 20, 1U << 21, 1U << 22, 1U << 23,
    };
    const struct {
        unsigned offset, width;
        uint64_t value;
    } faults[] = {
        { 8, 4, 0x234 }, { 8, 4, 0x328 }, { 8, 4, 0x1228 },
        { 8, 4, 0x22a }, { 0x80, 4, 0x480c0400 },
        { 0x84, 4, 0x80000 }, { 0x10, 4, 0x800 },
        { 0x10, 4, 0x41 }, { 0x20, 8, 0x0002000200010003ULL },
        { 0x48, 8, 0x0002000200010003ULL },
        { 0x1a8, 4, 0x500 }, { 0x1a8, 4, 0xd0 },
        { 0x1a8, 4, 0xc }, { 0x1a8, 4, 0x1000 },
        { 0x198, 4, 0x800 }, { 0x198, 4, 0x1000 },
        { 0x1c8, 8, (UINT64_C(1) << 36) + 1 },
        { 0x1c8, 8, -(UINT64_C(1) << 36) - 1 },
        { 0x1d0, 8, 1 }, { 0x1d8, 8, UINT64_MAX },
        { 0x1c0, 4, 0x10000000 }, { 0x1e0, 4, 0x62ff7006 },
        { 0x1e8, 4, 0x100 }, { 0x1c0, 4, 0x01000000 },
        { 0x1b0, 4, 16 },
    };
    unsigned count = ARRAY_SIZE(primitives) + ARRAY_SIZE(faults) + 32;
    unsigned hidden = GPOINTER_TO_UINT(selection);
    g_autofree char *dir = g_dir_make_tmp("crime-gl-point-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < count; n++) {
        if (n >= ARRAY_SIZE(primitives) + ARRAY_SIZE(faults) &&
            (0x511fffU & (1U << (n - ARRAY_SIZE(primitives) -
                                ARRAY_SIZE(faults))))) {
            continue;
        }
        Shape c = gl_point_prepare(s, 0x227, 0);
        SZCase v = sz_case();
        const Register *trigger = &triggers[n % ARRAY_SIZE(triggers)];

        sz_initialize(s, &c, v.old);
        v.draw = 0x511fff;
        v.plane = hidden ? UINT32_MAX : 0;
        sz_program(s, &v);
        qtest_writel(s, DE + 0x198, hidden == 2 ? 0 : 0x740);
        qtest_writeq(s, DE + 0x48, bounds(0, 0, 4, 4));
        qtest_writel(s, DE + 0x10, 0x21);
        qtest_writeq(s, DE + 0x20, bounds(0, 0, 4, 4));
        if (hidden == 1) {
            gl_point_vertex(s, 2048 * 64, 0, 0, 0, 0);
        }
        if (n < ARRAY_SIZE(primitives)) {
            qtest_writel(s, DE + 0x60, primitives[n]);
        } else if (n < ARRAY_SIZE(primitives) + ARRAY_SIZE(faults)) {
            unsigned f = n - ARRAY_SIZE(primitives);

            g_assert_cmpint(memtx_write(s, DE + faults[f].offset,
                faults[f].value, faults[f].width), ==, MEMTX_OK);
        } else {
            unsigned bit = n - ARRAY_SIZE(primitives) - ARRAY_SIZE(faults);

            qtest_writel(s, DE + 0x18, v.draw | (1U << bit));
        }
        sz_atomic_failure(s, &c, path, DE + trigger->offset,
            trigger->width == 4 ? 0x1153c940 : 0xa579b3e16c28f04dULL,
            trigger->width);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_mappings(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-gl-point-map-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *log = g_build_filename(dir, "errors", NULL);
    g_autofree char *args = g_strdup_printf("-D %s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_profile_ram("legacy-software", false, true, false,
                                      args);

    for (unsigned kind = 0; kind < 10; kind++) {
        Shape c = gl_point_prepare(s, 0x227, 0);
        SZCase v = sz_case();
        uint32_t expected[16];
        bool failure = kind >= 1 && kind <= 4;
        uint32_t want_sz = kind == 6 || kind == 9 ? v.old :
                           kind == 5 ? 0x54000011 : 0x54000012;

        sz_initialize(s, &c, v.old);
        v.draw = 0x41177f;
        v.stencil = 0x62ff7003;
        sz_program(s, &v);
        switch (kind) {
        case 0:
            qtest_writeq(s, RE + 0x1000, 0x7fff000000000000ULL);
            break;
        case 1:
            tile(s, 0, 0, 0x400);
            break;
        case 2:
            qtest_writeq(s, RE + 0x1000, 0x7fff000000000000ULL);
            qtest_writeq(s, RE + 0x1400, 0);
            break;
        case 3:
            tile(s, 2, 0, page_for(0, 0));
            break;
        case 4:
            tile(s, 0, 0, 0x4000 | page_for(0, 0));
            tile(s, 2, 0, page_for(0, 0));
            break;
        case 5:
            qtest_writel(s, DE + 0x1e0, 0x62ff0302);
            tile(s, 0, 0, 0x7fff);
            break;
        case 6:
            qtest_writel(s, DE + 0x18, v.draw | 0x800);
            qtest_writel(s, DE + 0x198, 0);
            qtest_writeq(s, RE + 0x1400, 0);
            tile(s, 0, 0, 0x7fff);
            break;
        case 7:
            qtest_writel(s, DE + 0x18, v.draw | 0x80);
            qtest_writel(s, DE + 0x1b8, 0);
            tile(s, 0, 0, 0x7fff);
            break;
        case 8:
            qtest_writel(s, DE + 0x18, v.draw & ~0x78U);
            tile(s, 0, 0, 0x7fff);
            break;
        case 9:
            qtest_writel(s, DE + 0x18, (v.draw & ~0x7fU) | 0x18);
            tile(s, 0, 0, 0x7fff);
            break;
        }
        if (failure) {
            sz_atomic_failure(s, &c, path, DE + 0x8d0, 0x1153c940, 4);
            continue;
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, 0x1153c940), ==, MEMTX_OK);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                expected[4 * y + x] = c.old;
                g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4), ==,
                                x == 1 && y == 2 ? want_sz : v.old);
            }
        }
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
    g_assert_true(g_file_get_contents(log, &contents, NULL, NULL));
    {
        g_auto(GStrv) summaries = g_strsplit(contents,
            "point suppressed 1 invalid destination fragments", -1);

        g_assert_cmpuint(g_strv_length(summaries), ==, 2);
    }
    g_unlink(log);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_starts(void)
{
    Register forms[64];
    unsigned count = gl_transfer_starts(forms);
    g_autofree char *dir = g_dir_make_tmp("crime-gl-point-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < count; n++) {
        const Register *form = &forms[n];
        Shape c = gl_point_prepare(s, 0x228, 0);
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS], value;
        bool pair = form->width == 8 && (form->offset == 0x70 ||
            form->offset == 0xa8 || form->offset == 0xc0);
        uint32_t expected[16];

        if (form->offset == 0xb0 || form->offset == 0xb4) {
            continue;
        }
        qtest_writel(s, DE + 0x18, 0x411278);
        qtest_writel(s, DE + 0xd0, 0x13579bdf);
        qtest_writel(s, DE + 0x1b0, 6);
        save_paired_renderer(s, path, before);
        value = form->offset == 0x1f0 ? 0xdeadbeef :
                before[192 + form->offset / 4];
        if (pair) {
            value = value << 32 | before[192 + form->offset / 4 + 1];
        }
        if (form->offset != 0x1f0) {
            g_assert_cmpint(memtx_write(s, DE + form->offset,
                value ^ (pair ? 0x100000001000ULL : 0x1000), form->width), ==,
                MEMTX_OK);
        }
        for (unsigned i = 0; i < 16; i++) {
            expected[i] = c.old;
        }
        shape_memory(s, &c, expected);
        g_test_message("point START +0x%x width %u", form->offset, form->width);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + form->offset,
                                    value, form->width), ==, MEMTX_OK);
        expected[9] ^= 0x13579bdf;
        shape_memory(s, &c, expected);
        save_paired_renderer(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        g_assert_cmpint(memtx_read(s, DE + 0x800 + form->offset,
                                   &value, form->width), !=, MEMTX_OK);
        g_assert_cmpint(memtx_write(s, DE + 0x801 + form->offset,
                                    value, form->width), !=, MEMTX_OK);
        qtest_readl(s, STATUS);
        qtest_writel(s, DE + 0x1f0, 0xdeadbeef);
        qtest_writel(s, DE + 0x1f8, 0);
        shape_memory(s, &c, expected);
        save_paired_renderer(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_start_refusals(void)
{
    const Register forms[] = {
        { 0x990, 4 }, { 0x994, 4 }, { 0x990, 8 },
        { 0x998, 4 }, { 0x9a0, 4 }, { 0x9a8, 4 },
        { 0x8b0, 4 }, { 0x8b4, 4 },
        { 0x8e0, 8 }, { 0x9f0, 8 }, { 0x8d0, 8 },
        { 0x9c8, 4 }, { 0x9c0, 8 }, { 0x820, 4 },
    };
    g_autofree char *dir = g_dir_make_tmp("crime-gl-point-width-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    Shape c = gl_point_prepare(s, 0x227, 0);

    sz_initialize(s, &c, 0x53000011);
    qtest_writel(s, DE + 0x18, 0x411578);
    qtest_writel(s, DE + 0x1a8, 0x45);
    for (unsigned n = 0; n < ARRAY_SIZE(forms); n++) {
        sz_atomic_failure(s, &c, path, DE + forms[n].offset,
                          0xa579b3e16c28f04dULL, forms[n].width);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_migration(void)
{
    g_autofree char *dir =
        g_dir_make_tmp("crime-gl-point-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *from = start(), *to = start_options("-incoming defer");
    Shape c = gl_point_prepare(from, 0x227, 0);
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    uint32_t expected[16];

    qtest_writel(from, DE + 0x18, 0x411378);
    qtest_writel(from, DE + 0x1b0, 6);
    g_assert_cmpint(memtx_writel(from, DE + 0x8d0, 0x13579bdf), ==, MEMTX_OK);
    for (unsigned i = 0; i < 16; i++) {
        expected[i] = i == 9 ? pipeline_reference(0x227, 0x13579bdf,
            c.old, 0, 0, 0x411378, 6, UINT32_MAX, 1, 2) : c.old;
    }
    qtest_writel(from, DE + 0x60, 1);
    save_paired_renderer(from, path, before);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    save_paired_renderer(to, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    shape_memory(to, &c, expected);
    qtest_writel(to, DE + 0x60, 0);
    g_assert_cmpint(memtx_writel(to, DE + 0x9f0, 0), ==, MEMTX_OK);
    for (unsigned i = 0; i < 16; i++) {
        expected[i] = c.old;
    }
    shape_memory(to, &c, expected);
    qtest_system_reset(to);
    shape_memory(to, &c, expected);
    save_renderer(to, path, after);
    for (unsigned i = 0; i < RENDER_WORDS; i++) {
        g_assert_cmphex(after[i], ==, 0);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_clock(void)
{
    QTestState *s = start_indexed_state();
    Shape c = gl_rectangle_prepare(s, 0x227, 0);
    uint32_t expected[9];

    c.x = c.y = 83;
    c.width = c.height = 3;
    c.old = 0x8e7b5aa5;
    qtest_writel(s, DE + 0x60, 0);
    qtest_writel(s, DE + 0x58, 0xf020f020);
    qtest_writeq(s, DE + 0x80, 0x48040d1a48040d19ULL);
    qtest_writel(s, DE + 0x10, 0x21);
    qtest_writeq(s, DE + 0x20, bounds(32, 32, 132, 132));
    qtest_writel(s, DE + 0x1a8, 0x45);
    for (unsigned dither = 0; dither < 2; dither++) {
        shape_memory(s, &c, NULL);
        qtest_writel(s, DE + 0xd0, 0xccccccff);
        qtest_writel(s, DE + 0x18, 0x411478 | dither << 8);
        for (unsigned i = 0; i < 9; i++) {
            expected[i] = i == 4 ?
                (dither ? 0x8e3b5aa5 : 0x8a1a5aa5) : c.old;
        }
        /* Chosen source color, not a reconstruction of the failed payload. */
        g_assert_cmpint(memtx_writel(s, DE + 0x8d0, 0x1153c940), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
}

typedef struct PointCoordinateStart {
    unsigned offset, width, x, y;
    uint64_t value;
} PointCoordinateStart;

static const PointCoordinateStart point_coordinate_starts[] = {
    { 0x80, 4, 2, 2, 0x4804009f },
    { 0x84, 4, 1, 0, 0x48040020 },
    { 0x88, 4, 1, 2, 0x7fc01234 },
    { 0x8c, 4, 1, 2, 0xdead5678 },
    { 0x90, 4, 1, 2, 0xa579b3e1 },
    { 0x94, 4, 1, 2, 0x6c28f04d },
    { 0x80, 8, 2, 0, 0x4804009f48040020ULL },
    { 0x88, 8, 1, 2, 0x7fc01234dead5678ULL },
    { 0x90, 8, 1, 2, 0xa579b3e16c28f04dULL },
};

static QTestState *point_coordinate_machine(void)
{
    QTestState *s = start_indexed_state();

    qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
    triangle_irq_baseline(s);
    return s;
}

static Shape point_coordinate_prepare(QTestState *s, unsigned mode,
                                        unsigned bank)
{
    Shape c = gl_point_prepare(s, mode, bank);

    gl_point_vertex(s, 90, 153, -4096, -4096, 0x48000000);
    qtest_writel(s, DE + 0x18, 0x411278);
    qtest_writel(s, DE + 0xd0, 0x13579bdf);
    qtest_writel(s, DE + 0x1b0, 6);
    return c;
}

static void point_coordinate_pixels(QTestState *s, const Shape *c,
                                      unsigned mode, int x, int y,
                                      uint32_t source, unsigned draw)
{
    uint32_t expected[16];

    for (unsigned j = 0; j < 4; j++) {
        for (unsigned i = 0; i < 4; i++) {
            expected[4 * j + i] = (int)i == x && (int)j == y ?
                pipeline_reference(mode, source, c->old, 0, 0x45,
                                   draw, 6, UINT32_MAX, i, j) : c->old;
        }
    }
    shape_memory(s, c, expected);
}

static void point_coordinate_publish(uint64_t words[RENDER_WORDS],
                                       const PointCoordinateStart *r)
{
    words[192 + r->offset / 4] = r->width == 8 ? r->value >> 32 : r->value;
    if (r->width == 8) {
        words[192 + r->offset / 4 + 1] = (uint32_t)r->value;
    }
}

static void point_coordinate_failure(QTestState *s, const Shape *c,
                                       const char *path, uint64_t address,
                                       uint64_t value, unsigned width)
{
    const unsigned registers[] = { 0x10, 0x18, 0x40, 0x48, 0x250, 0x258 };
    uint64_t before[ARRAY_SIZE(registers)];

    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        before[i] = qtest_readq(s, CRIME + registers[i]);
    }
    sz_atomic_failure(s, c, path, address, value, width);
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        g_assert_cmphex(qtest_readq(s, CRIME + registers[i]), ==, before[i]);
    }
    triangle_irq_levels(s, 1);
}

static void test_gl_point_coordinate_matrix(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-point-coords-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = point_coordinate_machine();

    for (unsigned n = 0; n < ARRAY_SIZE(point_coordinate_starts); n++) {
        const PointCoordinateStart *r = &point_coordinate_starts[n];
        unsigned mode = gl_rectangle_modes[n];
        Shape c = point_coordinate_prepare(s, mode, n % 3);
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
        unsigned head;

        save_paired_renderer(s, path, before);
        point_coordinate_pixels(s, &c, mode, -1, -1, 0, 0);
        head = ifb_next(s);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset,
                                    r->value, r->width), ==, MEMTX_OK);
        ifb_head(s, (head + 1) & 63);
        point_coordinate_pixels(s, &c, mode, r->x, r->y, 0x13579bdf, 0x411278);
        save_paired_renderer(s, path, after);
        point_coordinate_publish(before, r);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        qtest_readl(s, STATUS);
        qtest_writel(s, DE + 0x1f0, 0xdeadbeef);
        qtest_writel(s, DE + 0x1f8, 0);
        point_coordinate_pixels(s, &c, mode, r->x, r->y, 0x13579bdf, 0x411278);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_coordinate_equivalence(void)
{
    g_autofree char *dir =
        g_dir_make_tmp("crime-point-equivalent-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = point_coordinate_machine();

    for (unsigned n = 0; n < ARRAY_SIZE(point_coordinate_starts); n++) {
        const PointCoordinateStart *r = &point_coordinate_starts[n];
        uint64_t reference[RENDER_WORDS];

        for (unsigned trigger = 0; trigger < 4; trigger++) {
            Shape c = point_coordinate_prepare(s, 0x227, 0);
            SZCase v = sz_case();
            uint64_t after[RENDER_WORDS];
            unsigned head;

            sz_initialize(s, &c, v.old);
            v.draw = 0x411fff;
            v.stencil = 0x62ff7003;
            sz_program(s, &v);
            qtest_writel(s, DE + 0x198, 0x240);
            if (trigger) {
                g_assert_cmpint(memtx_write(s, DE + r->offset,
                    r->value, r->width), ==, MEMTX_OK);
            }
            if (trigger == 1) {
                qtest_writeq(s, DE + 0x1c8, 17 * 4096);
            } else if (trigger == 2) {
                qtest_writel(s, DE + 0xd0, 0xdeadbeef);
            }
            point_coordinate_pixels(s, &c, 0x227, -1, -1, 0, 0);
            head = ifb_next(s);
            if (!trigger) {
                g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset,
                    r->value, r->width), ==, MEMTX_OK);
            } else if (trigger == 1) {
                g_assert_cmpint(memtx_writeq(s, DE + 0x9c8, v.start), ==,
                                MEMTX_OK);
            } else {
                g_assert_cmpint(memtx_writel(s, DE +
                    (trigger == 2 ? 0x8d0 : 0x9f0),
                    trigger == 2 ? v.color : 0xdeadbeef), ==, MEMTX_OK);
            }
            ifb_head(s, (head + 1) & 63);
            point_coordinate_pixels(s, &c, 0x227, r->x, r->y, v.color, v.draw);
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 4; x++) {
                    g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                        ==, x == r->x && y == r->y ? 0x54000012 : v.old);
                }
            }
            save_paired_renderer(s, path, after);
            if (!trigger) {
                memcpy(reference, after, sizeof(reference));
            } else {
                g_assert_cmpmem(reference, sizeof(reference),
                                after, sizeof(after));
            }
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_coordinate_once(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-point-once-XXXXXX", NULL);
    g_autofree char *log = g_build_filename(dir, "errors", NULL);
    g_autofree char *args = g_strdup_printf("-D %s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_profile_ram("legacy-software", false, true, false,
                                      args);

    for (unsigned n = 0; n < ARRAY_SIZE(point_coordinate_starts); n++) {
        const PointCoordinateStart *r = &point_coordinate_starts[n];

        for (unsigned kind = 0; kind < 3; kind++) {
            Shape c = point_coordinate_prepare(s, 0x227, 0);
            SZCase v = sz_case();
            unsigned head;

            sz_initialize(s, &c, v.old);
            v.draw = 0x41177f;
            v.stencil = 0x62ff7003;
            sz_program(s, &v);
            if (kind == 1) {
                qtest_writel(s, DE + 0x18, v.draw | 0x80);
                qtest_writel(s, DE + 0x1b8, 0);
                tile(s, 0, 0, 0x7fff);
            } else if (kind == 2) {
                qtest_writeq(s, RE + 0x1000, 0x7fff000000000000ULL);
            }
            head = ifb_next(s);
            g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset,
                r->value, r->width), ==, MEMTX_OK);
            ifb_head(s, (head + 1) & 63);
            point_coordinate_pixels(s, &c, 0x227,
                kind ? -1 : (int)r->x, r->y, v.color, v.draw);
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 4; x++) {
                    g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                        ==, x == r->x && y == r->y ? 0x54000012 : v.old);
                }
            }
        }
    }
    qtest_quit(s);
    g_assert_true(g_file_get_contents(log, &contents, NULL, NULL));
    {
        g_auto(GStrv) summaries = g_strsplit(contents,
            "point suppressed 1 invalid destination fragments", -1);

        g_assert_cmpuint(g_strv_length(summaries), ==, 10);
    }
    g_unlink(log);
    g_rmdir(dir);
}

static void test_gl_point_coordinate_dormant(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-point-dormant-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = point_coordinate_machine();

    for (unsigned n = 0; n < ARRAY_SIZE(point_coordinate_starts); n++) {
        const PointCoordinateStart *r = &point_coordinate_starts[n];
        Shape c;

        if (r->offset == 0x80 || r->offset == 0x84) {
            continue;
        }
        c = point_coordinate_prepare(s, 0x228, 0);
        sz_initialize(s, &c, 0x53000011);
        qtest_writeq(s, DE + 0x88, 0x480400c048040080ULL);
        qtest_writeq(s, DE + 0x90, 0x4804004048040000ULL);
        g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset,
            r->value, r->width), ==, MEMTX_OK);
        point_coordinate_pixels(s, &c, 0x228, 1, 2, 0x13579bdf, 0x411278);
        qtest_writel(s, DE + 0x60, r->offset < 0x90 ? 0x01000000 : 0x02020000);
        qtest_writel(s, DE + 0x18, 0x400278);
        point_coordinate_failure(s, &c, path, DE + 0x9f0, 0, 4);
        qtest_writeq(s, DE + 0x88, 0x480400c048040080ULL);
        qtest_writeq(s, DE + 0x90, 0x4804004048040000ULL);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_coordinate_boundaries(void)
{
    const struct {
        int x64, y64, ox, oy, x, y;
    } vectors[] = {
        { 31, 32, -4096, -4096, 0, 0 },
        { 32, 33, -4096, -4096, 1, 1 },
        { -32, -31, -4096, -4096, 0, 0 },
        { -33, -32, -4096, -4096, -1, -1 },
        { 2047 * 64 + 31, 2047 * 64 + 32, 0, 0, 2047, 2047 },
        { 2047 * 64 + 32, 2047 * 64 + 33, 0, 0, 2048, 2048 },
        { 90, 153, 0, 0, 1, 2 },
        { 90, 153, 1, 2, 1, 2 },
        { 84 * 64 + 31, 84 * 64 + 32, -4064, -4064, 84, 84 },
    };
    const uint32_t prefixes[] = { 0, 0x00800000, 0x48000000 };
    QTestState *s = point_coordinate_machine();

    for (unsigned n = 0; n < ARRAY_SIZE(point_coordinate_starts); n++) {
        for (unsigned v = 0; v < ARRAY_SIZE(vectors); v++) {
            for (unsigned clip = 0; clip < 3; clip++) {
                const PointCoordinateStart *r = &point_coordinate_starts[n];
                uint32_t prefix = prefixes[v % 3];
                uint32_t raw_x = prefix + (int64_t)vectors[v].x64 -
                                 64 * (int64_t)vectors[v].ox;
                uint32_t raw_y = prefix + (int64_t)vectors[v].y64 -
                                 64 * (int64_t)vectors[v].oy;
                uint64_t value = r->offset == 0x80 ? raw_x :
                                 r->offset == 0x84 ? raw_y : r->value;
                Shape c = point_coordinate_prepare(s, 0x228, 0);
                uint32_t expected[16];
                bool visible = clip != 1;

                c.x = vectors[v].x >= 2047 ? 2044 :
                      vectors[v].x >= 84 ? 83 : 0;
                c.y = vectors[v].y >= 2047 ? 2044 :
                      vectors[v].y >= 84 ? 83 : 0;
                map_region(s, 0, 4, c.x, c.y, c.x + 3, c.y + 3);
                shape_memory(s, &c, NULL);
                gl_point_vertex(s, vectors[v].x64, vectors[v].y64,
                                vectors[v].ox, vectors[v].oy, prefix);
                if (r->offset == 0x80 && r->width == 8) {
                    value = (uint64_t)raw_x << 32 | raw_y;
                    qtest_writeq(s, DE + 0x80, 0);
                } else if (r->offset <= 0x84) {
                    qtest_writel(s, DE + r->offset, 0);
                }
                if (clip == 1) {
                    int wx = vectors[v].x - vectors[v].ox;
                    int wy = vectors[v].y - vectors[v].oy;

                    qtest_writel(s, DE + 0x18, 0x511278);
                    qtest_writeq(s, DE + 0x48,
                                bounds(wx + 1, wy, wx + 2, wy + 1));
                } else if (clip == 2) {
                    qtest_writel(s, DE + 0x10, 0x63);
                    qtest_writeq(s, DE + 0x20, bounds(0, 0, 2048, 2048));
                    qtest_writeq(s, DE + 0x28, bounds(0, 0, 2048, 2048));
                }
                for (unsigned y = 0; y < 4; y++) {
                    for (unsigned x = 0; x < 4; x++) {
                        expected[4 * y + x] = c.old ^
                            (visible && (int)(c.x + x) == vectors[v].x &&
                             (int)(c.y + y) == vectors[v].y ? 0x13579bdf : 0);
                    }
                }
                g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset,
                                            value, r->width), ==, MEMTX_OK);
                shape_memory(s, &c, expected);
            }
        }
    }
    qtest_quit(s);
}

static void test_gl_point_coordinate_failures(gconstpointer selection)
{
    const struct {
        unsigned offset, width;
        uint64_t value;
    } faults[] = {
        { 0x60, 4, 1 }, { 0x60, 4, 0x80000 },
        { 0x18, 4, 0x513fff }, { 0x18, 4, 0x515fff },
        { 0x18, 4, 0x519fff }, { 0x18, 4, 0x531fff },
        { 8, 4, 0x234 }, { 8, 4, 0x328 }, { 8, 4, 0x1228 },
        { 0x80, 4, 0x480c0400 }, { 0x84, 4, 0x80000 },
        { 0x10, 4, 0x800 }, { 0x10, 4, 0x41 },
        { 0x20, 8, 0x0002000200010003ULL },
        { 0x48, 8, 0x0002000200010003ULL },
        { 0x1a8, 4, 0x500 }, { 0x1a8, 4, 0xd0 },
        { 0x1a8, 4, 0xc }, { 0x1a8, 4, 0x1000 },
        { 0x198, 4, 0x800 }, { 0x198, 4, 0x1000 },
        { 0x1c8, 8, (UINT64_C(1) << 36) + 1 },
        { 0x1c8, 8, -(UINT64_C(1) << 36) - 1 },
        { 0x1d0, 8, 1 }, { 0x1d8, 8, UINT64_MAX },
        { 0x1c0, 4, 0x10000000 }, { 0x1e0, 4, 0x62ff7006 },
        { 0x1e8, 4, 0x100 }, { 0x1c0, 4, 0x01000000 },
        { 0x1b0, 4, 16 },
    };
    unsigned hidden = GPOINTER_TO_UINT(selection);
    g_autofree char *dir = g_dir_make_tmp("crime-coordinate-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = point_coordinate_machine();

    for (unsigned n = 0; n < ARRAY_SIZE(faults); n++) {
        const PointCoordinateStart *r = &point_coordinate_starts[
            faults[n].offset == 0x80 || faults[n].offset == 0x84 ? 8 : n % 9];
        Shape c = point_coordinate_prepare(s, 0x227, 0);
        SZCase v = sz_case();

        sz_initialize(s, &c, v.old);
        v.draw = 0x511fff;
        v.plane = hidden ? UINT32_MAX : 0;
        sz_program(s, &v);
        qtest_writel(s, DE + 0x198, hidden == 2 ? 0 : 0x740);
        qtest_writeq(s, DE + 0x48, bounds(0, 0, 8192, 8192));
        qtest_writel(s, DE + 0x10, 0x21);
        qtest_writeq(s, DE + 0x20, bounds(0, 0, 4, 4));
        if (hidden == 1) {
            qtest_writeq(s, DE + 0x20, bounds(10, 10, 11, 11));
        }
        g_assert_cmpint(memtx_write(s, DE + faults[n].offset,
            faults[n].value, faults[n].width), ==, MEMTX_OK);
        point_coordinate_failure(s, &c, path, DE + 0x800 + r->offset,
                                 r->value, r->width);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_coordinate_atomic(void)
{
    g_autofree char *dir =
        g_dir_make_tmp("crime-coordinate-atomic-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = point_coordinate_machine();

    for (unsigned kind = 0; kind < 4; kind++) {
        Shape c = point_coordinate_prepare(s, 0x228, 0);
        PointCoordinateStart r = point_coordinate_starts[kind == 3 ? 8 : 6];

        sz_initialize(s, &c, 0x53000011);
        if (!kind) {
            r.value = 0x4804009f7fc01234ULL;
        } else if (kind == 1) {
            r = point_coordinate_starts[0];
            qtest_writel(s, DE + 0x84, 0x7fc01234);
        } else {
            if (kind == 2) {
                r = point_coordinate_starts[1];
            }
            qtest_writel(s, DE + 0x80, 0x7fc01234);
        }
        point_coordinate_failure(s, &c, path, DE + 0x800 + r.offset,
                                 r.value, r.width);
        if (kind == 1) {
            qtest_writel(s, DE + 0x84, 0x48040099);
        } else if (kind >= 2) {
            qtest_writel(s, DE + 0x80, 0x4804005a);
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        point_coordinate_pixels(s, &c, 0x228, 1, 2, 0x13579bdf, 0x411278);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_coordinate_mapping_failures(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-coordinate-map-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = point_coordinate_machine();

    for (unsigned n = 0; n < ARRAY_SIZE(point_coordinate_starts); n++) {
        const PointCoordinateStart *r = &point_coordinate_starts[n];

        for (unsigned kind = 0; kind < 3; kind++) {
            Shape c = point_coordinate_prepare(s, 0x228, 0);
            SZCase v = sz_case();

            sz_initialize(s, &c, v.old);
            v.draw = 0x41177f;
            v.stencil = 0x62ff7003;
            sz_program(s, &v);
            if (!kind) {
                tile(s, 0, 0, 0x400);
            } else if (kind == 1) {
                qtest_writeq(s, RE + 0x1000, 0x7fff000000000000ULL);
                qtest_writeq(s, RE + 0x1400, 0);
            } else {
                tile(s, 0, 0, 0x4000 | page_for(0, 0));
                tile(s, 2, 0, page_for(0, 0));
            }
            point_coordinate_failure(s, &c, path, DE + 0x800 + r->offset,
                                     r->value, r->width);
            tile(s, 0, 0, page_for(0, 0));
            tile(s, 2, 0, page_for(2, 0));
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            point_coordinate_pixels(s, &c, 0x228, 1, 2, v.color, v.draw);
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_coordinate_widths(void)
{
    g_autofree char *dir =
        g_dir_make_tmp("crime-coordinate-width-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = point_coordinate_machine();
    Shape c = point_coordinate_prepare(s, 0x228, 0);
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    IfbSnapshot a, b;

    sz_initialize(s, &c, 0x53000011);
    save_paired_renderer(s, path, before);
    ifb_snapshot(s, &a);
    for (unsigned offset = 0x80; offset <= 0x94; offset += 4) {
        for (unsigned size = 1; size <= 8; size *= 2) {
            uint64_t value;

            g_assert_cmpint(memtx_read(s, DE + 0x800 + offset,
                                       &value, size), !=, MEMTX_OK);
            g_assert_cmpint(memtx_read(s, DE + offset, &value, size), !=,
                            MEMTX_OK);
            if (size < 4 || (size == 8 && (offset & 7))) {
                point_coordinate_failure(s, &c, path, DE + 0x800 + offset,
                                          UINT64_MAX, size);
            }
        }
        for (unsigned delta = 1; delta < 4; delta++) {
            point_coordinate_failure(s, &c, path, DE + 0x800 + offset + delta,
                                      UINT32_MAX, 4);
        }
        if (!(offset & 7)) {
            for (unsigned delta = 1; delta < 8; delta++) {
                point_coordinate_failure(s, &c, path,
                    DE + 0x800 + offset + delta, UINT64_MAX, 8);
            }
        }
    }
    point_coordinate_failure(s, &c, path, DE + 0x87c, UINT64_MAX, 8);
    point_coordinate_failure(s, &c, path, DE + 0x898, UINT32_MAX, 4);
    save_paired_renderer(s, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    ifb_snapshot(s, &b);
    g_assert_cmpmem(&a, sizeof(a), &b, sizeof(b));
    {
        g_autoptr(QDict) reply = qtest_qmp(s,
            "{'execute':'qom-set','arguments':{'path':%s,'property':'request',"
            "'value':'store -1 0x15002880 00000000000000000000000000000000'}}",
            MEMTX_TEST_PATH);

        /* The existing scalar test interface refuses 16 bytes before MMIO. */
        g_assert_true(qdict_haskey(reply, "error"));
        g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(reply, "error"),
            "desc"), "Transaction size must be 1, 2, 4 or 8 bytes"));
    }
    ifb_snapshot(s, &b);
    g_assert_cmpmem(&a, sizeof(a), &b, sizeof(b));
    save_paired_renderer(s, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    point_coordinate_pixels(s, &c, 0x228, -1, -1, 0, 0);
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_coordinate_other_consumers(void)
{
    g_autofree char *dir =
        g_dir_make_tmp("crime-coordinate-consumer-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = point_coordinate_machine();

    for (unsigned kind = 0; kind < 5; kind++) {
        for (unsigned n = 0; n < ARRAY_SIZE(point_coordinate_starts); n++) {
            const PointCoordinateStart *r = &point_coordinate_starts[n];
            Shape c = pipeline_prepare(s, 0x228, 0);

            sz_initialize(s, &c, 0x53000011);
            if (kind == 0) {
                setup(s, 0x228, false);
                qtest_writel(s, DE + 0x60, 0);
                qtest_writel(s, DE + 0x70, xy(1, 2));
            } else if (kind == 1) {
                c = gl_line_prepare(s, false);
            } else if (kind == 3) {
                c = gl_rectangle_prepare(s, 0x228, 0);
            } else if (kind == 4) {
                gl_transfer_probe(s, 2);
            }
            uint64_t raw[RENDER_WORDS], value;

            save_paired_renderer(s, path, raw);
            value = raw[192 + r->offset / 4];
            if (r->width == 8) {
                value = value << 32 | raw[192 + r->offset / 4 + 1];
            }
            point_coordinate_failure(s, &c, path, DE + 0x800 + r->offset,
                                     value, r->width);
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_coordinate_profiles(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *s = start_profile(legacy ? "legacy-software" : "1.5",
                                      false, !legacy, "");

        qtest_memset(s, 0x30000, 0x5a, 16);
        for (unsigned n = 0; n < ARRAY_SIZE(point_coordinate_starts); n++) {
            const PointCoordinateStart *r = &point_coordinate_starts[n];

            g_assert_cmpint(memtx_write(s, DE + 0x800 + r->offset,
                                        r->value, r->width), !=, MEMTX_OK);
            g_assert_cmphex(qtest_readq(s, 0x30000), ==, 0x5a5a5a5a5a5a5a5aULL);
            g_assert_cmphex(qtest_readq(s, 0x30008), ==, 0x5a5a5a5a5a5a5a5aULL);
        }
        qtest_quit(s);
    }
}

static void test_gl_point_coordinate_migration(gconstpointer selection)
{
    bool committed = GPOINTER_TO_UINT(selection);
    g_autofree char *dir = g_dir_make_tmp("crime-coordinate-save-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);

    for (unsigned n = 0; n < ARRAY_SIZE(point_coordinate_starts); n++) {
        const PointCoordinateStart *r = &point_coordinate_starts[n];
        QTestState *from = start(), *to = start_options("-incoming defer");
        Shape c = point_coordinate_prepare(from, 0x228, 0);
        uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
        uint32_t expected[16];

        if (committed) {
            g_assert_cmpint(memtx_write(from, DE + 0x800 + r->offset,
                r->value, r->width), ==, MEMTX_OK);
        }
        save_paired_renderer(from, path, before);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        save_paired_renderer(to, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        point_coordinate_pixels(to, &c, 0x228,
            committed ? (int)r->x : -1, r->y, 0x13579bdf, 0x411278);
        if (!committed) {
            g_assert_cmpint(memtx_write(to, DE + 0x800 + r->offset,
                r->value, r->width), ==, MEMTX_OK);
            point_coordinate_pixels(to, &c, 0x228,
                                     r->x, r->y, 0x13579bdf, 0x411278);
        }
        g_assert_cmpint(memtx_writel(to, DE + 0x9f0, 0), ==, MEMTX_OK);
        for (unsigned i = 0; i < 16; i++) {
            expected[i] = c.old;
        }
        shape_memory(to, &c, expected);
        qtest_system_reset(to);
        shape_memory(to, &c, expected);
        save_paired_renderer(to, path, after);
        for (unsigned i = 0; i < RENDER_WORDS; i++) {
            g_assert_cmphex(after[i], ==, 0);
        }
        qtest_quit(from);
        qtest_quit(to);
        g_unlink(sock);
    }
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_point_coordinate_active(void)
{
    const struct {
        unsigned offset, width, x, y;
        uint64_t value;
    } vectors[] = {
        { 0x880, 4, 85, 84, 0x48040d5a },
        { 0x884, 4, 84, 87, 0x48040dd9 },
        { 0x880, 8, 85, 87, 0x48040d5a48040dd9ULL },
    };
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(vectors); n++) {
        Shape c = gl_point_prepare(s, 0x228, 0);
        uint32_t expected[35];

        c.x = c.y = 83;
        c.width = 5;
        c.height = 7;
        shape_memory(s, &c, NULL);
        qtest_writel(s, DE + 0x18, 0x411278);
        qtest_writel(s, DE + 0x1b0, 6);
        qtest_writel(s, DE + 0xd0, 0x13579bdf);
        qtest_writel(s, DE + 0x58, 0xf020f020);
        qtest_writeq(s, DE + 0x80, 0x48040d1a48040d19ULL);
        for (unsigned y = 0; y < c.height; y++) {
            for (unsigned x = 0; x < c.width; x++) {
                expected[y * c.width + x] = c.old ^
                    (c.x + x == vectors[n].x && c.y + y == vectors[n].y ?
                     0x13579bdf : 0);
            }
        }
        g_assert_cmpint(memtx_write(s, DE + vectors[n].offset,
            vectors[n].value, vectors[n].width), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
}

/* Coverage comes from the independent rational fixtures, not the device. */
static void test_gl_line_composed_vectors(void)
{
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(gl_nonaa_vectors); n++) {
        const GLNonAAVector *v = &gl_nonaa_vectors[n];
        Shape c = pipeline_prepare(s, 0x228, n % 2);
        SZCase sz = sz_case();
        uint32_t expected[256], zs[256];
        bool major[16] = { false }, enabled[16] = { false };
        bool xmajor = abs(v->b[0] - v->a[0]) >= abs(v->b[1] - v->a[1]);
        bool reverse = v->b[xmajor ? 0 : 1] < v->a[xmajor ? 0 : 1];
        unsigned bit = 2, repeat = 1;

        c.width = c.height = 16;
        map_region(s, c.bank, 4, 0, 0, 15, 15);
        shape_memory(s, &c, NULL);
        sz_initialize(s, &c, sz.old);
        sz.draw = TRIANGLE_DRAW | 0x180007;
        sz.stencil = 0x62ff7002;
        sz.start = 100 * 4096;
        sz.dx = 4096;
        sz.dy = -8192;
        sz_program(s, &sz);
        qtest_writeq(s, DE + 0x48, bounds(4096, 4096, 4112, 4112));
        gl_line_vertices(s, v->a, v->b, -4096, -4096, 0x48000000);
        qtest_writel(s, DE + 0x60, 0x01000000 | v->halfwidth |
                    (n % 4) << 16 | (n & 1) << 18);
        qtest_writeq(s, DE + 0xc0, UINT64_C(0x02040102) << 32 | 0x28000000);
        for (unsigned y = 0; y < 16; y++) {
            for (unsigned x = 0; x < 16; x++) {
                if (v->rows[y] & (1U << x)) {
                    major[xmajor ? x : y] = true;
                }
            }
        }
        for (unsigned j = 0; j < 16; j++) {
            unsigned m = reverse ? 15 - j : j;

            if (!major[m]) {
                continue;
            }
            enabled[m] = 0x28000000 & (1U << (31 - bit));
            if (repeat++ == 2) {
                repeat = 0;
                bit = bit == 4 ? 2 : bit + 1;
            }
        }
        for (unsigned y = 0; y < 16; y++) {
            for (unsigned x = 0; x < 16; x++) {
                bool selected = (v->rows[y] & (1U << x)) &&
                                enabled[xmajor ? x : y];
                int64_t z = 64 * sz.start +
                    sz.dx * (64 * (int64_t)x - v->a[0]) +
                    sz.dy * (64 * (int64_t)y - v->a[1]);
                int64_t sample = z / 64 - (z % 64 < 0);

                expected[16 * y + x] = selected ? sz.color : c.old;
                zs[16 * y + x] = selected ? 0x62000000 |
                    sz_reference_depth(sample) : sz.old;
            }
        }
        g_test_message("composed line vector %u", n);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
        for (unsigned y = 0; y < 16; y++) {
            for (unsigned x = 0; x < 16; x++) {
                g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                                ==, zs[16 * y + x]);
            }
        }
    }
    qtest_quit(s);
}

static void gl_line_sz_alpha_run(QTestState *s, const SZCase *v, bool smooth,
                                  unsigned alpha_test)
{
    const int a[] = { 32, 80 }, b[] = { 224, 80 };
    const int starts[] = { 100 * 4096, 80 * 4096, 60 * 4096, 128 * 4096 };
    const int dx[] = { 4096, -8192, 12288, -16384 };
    const int dy[] = { 8192, 4096, -4096, -8192 };
    Shape c = pipeline_prepare(s, v->mode, v->bank);
    uint32_t expected[16], zs[16];

    sz_initialize(s, &c, v->old);
    sz_program(s, v);
    pipeline_shade(s, starts, dx, dy);
    gl_line_vertices(s, a, b, -4096, -4096, 0x48000000);
    qtest_writel(s, DE + 0x60, 0x01000060);
    qtest_writel(s, DE + 0x18, v->draw | (smooth ? 0x10000 : 0));
    qtest_writel(s, DE + 0x198, alpha_test);
    qtest_writeq(s, DE + 0xc0, UINT64_C(0x02040102) << 32 | UINT32_MAX);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            bool covered = x >= 1 && y <= 2, pass;
            int64_t n = v->draw & 4 ? 64 * v->start +
                v->dx * (64 * (int64_t)x - a[0]) +
                v->dy * (64 * (int64_t)y - a[1]) : 0;
            int64_t z = n / 64 - (n % 64 < 0);
            uint32_t source = smooth ?
                gl_line_reference_color(starts, dx, dy, a, x, y) : v->color;
            uint32_t old = sz_reference(v, v->old,
                                        sz_reference_depth(z), &pass);

            bool alpha = !(v->draw & 0x800) || sz_reference_compare(
                alpha_test >> 8, source & 255, alpha_test & 255);

            zs[4 * y + x] = covered && alpha ? old : v->old;
            expected[4 * y + x] = covered && alpha && pass ? pipeline_reference(
                v->mode, source, c.old, 0, 0x45, v->draw, 6,
                v->plane, x, y) : c.old;
        }
    }
    g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    shape_memory(s, &c, expected);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                            ==, zs[4 * y + x]);
        }
    }
}

static void gl_line_sz_run(QTestState *s, const SZCase *v, bool smooth)
{
    gl_line_sz_alpha_run(s, v, smooth, 0x740);
}

static void test_gl_line_sz_composition(void)
{
    QTestState *s = start_indexed_state();
    SZCase v = sz_case();

    for (unsigned function = 0; function < 8; function++) {
        for (unsigned value = 16; value <= 18; value++) {
            v.draw = TRIANGLE_DRAW | 0x80707;
            v.control = function << 25;
            v.start = value * 4096;
            v.dx = 4096;
            v.dy = -8192;
            v.mode = gl_rectangle_modes[(function + value) % 10];
            v.bank = function % 2;
            v.stencil = 0x62ff7003;
            gl_line_sz_run(s, &v, true);
        }
    }
    for (unsigned outcome = 0; outcome < 3; outcome++) {
        for (unsigned operation = 0; operation < 6; operation++) {
            v = sz_case();
            v.draw = TRIANGLE_DRAW | 0x80007;
            v.control = (outcome == 1 ? 0 : 7) << 25;
            v.stencil = 0x62ff0000 | (outcome == 0 ? 0 : 7) << 12 |
                operation << 8 | operation << 4 | operation;
            v.mask = 0x5a;
            gl_line_sz_run(s, &v, false);
        }
    }
    for (unsigned function = 5; function <= 7; function += 2) {
        for (unsigned materialize = 0; materialize < 2; materialize++) {
            for (unsigned write = 0; write < 2; write++) {
                for (unsigned pass = 0; pass < 2; pass++) {
                    for (unsigned equal = 0; equal < 2; equal++) {
                        v = sz_case();
                        v.draw = TRIANGLE_DRAW | 0x80005 | write << 1;
                        v.mask = write ? 255 : 0;
                        v.control = (pass ? 7 : 0) << 25 | 0x01000010;
                        v.stencil = 0x62ff0002 | function << 12 |
                                    (materialize ? 0x20 : 0);
                        v.old = (equal ? 0x62 : 0x53) << 24 | 17;
                        gl_line_sz_run(s, &v, true);
                    }
                }
            }
        }
    }
    for (unsigned edge = 0; edge < 2; edge++) {
        v = sz_case();
        v.draw |= 0x80000;
        v.start = edge ? INT64_C(1) << 36 : -(INT64_C(1) << 36);
        gl_line_sz_run(s, &v, false);
    }
    v = sz_case();
    v.draw = TRIANGLE_DRAW | 0x80001;
    v.start = INT64_MIN;
    v.dx = INT64_MAX;
    v.dy = INT64_MIN;
    v.stencil = 0x62ff7002;
    gl_line_sz_run(s, &v, true);
    qtest_quit(s);
}

static void test_gl_line_composed_clipping(void)
{
    QTestState *s = start_indexed_state();
    const int a[] = { -3 * 64, 64 }, b[] = { 9 * 64, 64 };

    for (unsigned cut = 0; cut < 4; cut++) {
        Shape c = pipeline_prepare(s, 0x228, 0);
        SZCase v = sz_case();
        uint32_t expected[48];
        uint64_t box = bounds(4097, 4096, 4103, 4100);

        c.width = 12;
        map_region(s, 0, 4, 0, 0, 11, 3);
        shape_memory(s, &c, NULL);
        sz_initialize(s, &c, v.old);
        v.draw = TRIANGLE_DRAW | 0x80007 | (cut == 1 ? 0x100000 : 0);
        v.stencil = 0x62ff7003;
        sz_program(s, &v);
        gl_line_vertices(s, a, b, -4096, -4096, 0);
        qtest_writel(s, DE + 0x60, 0x01000040);
        qtest_writeq(s, DE + 0xc0, UINT64_C(0x02040102) << 32 | 0x28000000);
        qtest_writeq(s, DE + 0x48, box);
        if (cut == 2) {
            qtest_writel(s, DE + 0x10, 0x63);
            qtest_writeq(s, DE + 0x20, bounds(1, 0, 7, 4));
            qtest_writeq(s, DE + 0x28, bounds(2, 0, 6, 4));
        }
        if (cut == 3) {
            qtest_writel(s, DE + 0x18, v.draw | 0x800);
            qtest_writel(s, DE + 0x198, 0);
            tile(s, 0, 0, 0x7fff);
            tile(s, 2, 0, 0x7fff);
        }
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 12; x++) {
                unsigned i = x + 3;
                /* Sequence 11 000 111 111 0 before framebuffer clipping. */
                bool selected = x < 9 && i >= 5 && i <= 10 &&
                    (y == 1 || y == 2) && cut != 3 &&
                    (!(cut == 1 || cut == 2) || (x >= 1 && x < 7));

                expected[12 * y + x] = selected ? v.color : c.old;
            }
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 12; x++) {
                bool selected = expected[12 * y + x] != c.old;

                g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                                ==, selected ? 0x54000012 : v.old);
            }
        }
    }
    qtest_quit(s);
}

static void test_gl_line_composed_failures(void)
{
    const int64_t limit = INT64_C(1) << 36;
    const uint32_t modes[] = { 0x04020102, 0x02040302, 0x22040102,
                               0x02240102 };
    const unsigned held[] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x20000,
                              0x40000, 0x200000, 0x800000, 0x1000000 };
    g_autofree char *dir =
        g_dir_make_tmp("crime-line-composed-fail-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < 24; n++) {
        Shape c = gl_line_prepare(s, false);
        SZCase v = sz_case();
        const int a[] = { 32, 80 }, b[] = { 224, 80 };

        sz_initialize(s, &c, v.old);
        v.draw = TRIANGLE_DRAW | 0x80007;
        v.stencil = 0x62ff7003;
        sz_program(s, &v);
        gl_line_vertices(s, a, b, 0, 0, 0);
        qtest_writel(s, DE + 0x60, 0x01000060);
        qtest_writeq(s, DE + 0xc0, UINT64_C(0x02040102) << 32 | UINT32_MAX);
        if (n < 4) {
            qtest_writel(s, DE + 0xc0, modes[n]);
            qtest_writel(s, DE + 0xc4, n & 1 ? 0 : UINT32_MAX);
        } else if (n < 13) {
            qtest_writel(s, DE + 0x18, v.draw | held[n - 4]);
        } else if (n < 19) {
            unsigned word = (n - 13) / 2;
            qtest_writeq(s, DE + 0x1c8 + 8 * word,
                         n & 1 ? limit + 1 : -limit - 1);
        } else if (n < 22) {
            /* A later geometric overflow remains illegal under each discard. */
            qtest_writeq(s, DE + 0x1c8, limit - 8192);
            qtest_writeq(s, DE + 0x1d0, 8192);
            if (n == 19) {
                qtest_writel(s, DE + 0xc4, 0);
            } else if (n == 20) {
                qtest_writel(s, DE + 0x18, v.draw | 0x800);
                qtest_writel(s, DE + 0x198, 0);
            } else {
                qtest_writel(s, DE + 0x18, v.draw | 0x100000);
                qtest_writeq(s, DE + 0x48, bounds(10, 10, 11, 11));
            }
        } else {
            const int starts[] = { (1 << 20) - 2, 0, 0, 0 };
            const int dx[] = { 2, 0, 0, 0 }, dy[] = { 0, 0, 0, 0 };

            qtest_writel(s, DE + 0x18, v.draw | 0x10000);
            pipeline_shade(s, starts, dx, dy);
            qtest_writel(s, DE + 0xc4, 0);
            if (n == 23) {
                qtest_writel(s, DE + 0x18, v.draw | 0x10800);
                qtest_writel(s, DE + 0x198, 0);
            }
        }
        g_test_message("composed line semantic refusal %u", n);
        sz_atomic_failure(s, &c, path, DE + 0x8d0, 0xf13579bd, 4);
    }
    /* Failed triggering companions are not published. */
    {
        Shape c = gl_line_prepare(s, false);
        SZCase v = sz_case();

        sz_initialize(s, &c, v.old);
        v.draw |= 0x80000;
        sz_program(s, &v);
        qtest_writeq(s, DE + 0xc0, UINT64_C(0x02040102) << 32 | UINT32_MAX);
        sz_atomic_failure(s, &c, path, DE + 0x8c0,
                          UINT64_C(0x04020102) << 32 | 0x13579bdf, 8);
        sz_atomic_failure(s, &c, path, DE + 0x9d8, limit + 1, 8);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_line_composed_starts(void)
{
    Register forms[64];
    unsigned count = gl_transfer_starts(forms);
    g_autofree char *dir =
        g_dir_make_tmp("crime-line-composed-start-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    const int a[] = { 0, 64 }, b[] = { 192, 64 };
    const int starts[] = { 0x11 * 4096, 0x53 * 4096,
                           0xc9 * 4096, 0x40 * 4096 };
    const int zero[] = { 0, 0, 0, 0 };

    for (unsigned smooth = 0; smooth < 2; smooth++) {
        for (unsigned n = 0; n < count; n++) {
            const Register *f = &forms[n];
            Shape c = gl_line_prepare(s, false);
            SZCase v = sz_case();
            uint64_t before[RENDER_WORDS], after[RENDER_WORDS], value;
            bool pair = f->width == 8 && (f->offset == 0x70 ||
                f->offset == 0xa8 || f->offset == 0xc0);
            uint32_t expected[16];

            if (f->offset == 0xb0 || f->offset == 0xb4 ||
                (f->offset >= 0x110 && f->offset <= 0x15c)) {
                continue;
            }
            sz_initialize(s, &c, v.old);
            v.draw = TRIANGLE_DRAW | 0x80207 | (smooth ? 0x10000 : 0);
            v.stencil = 0x62ff7003;
            sz_program(s, &v);
            pipeline_shade(s, starts, zero, zero);
            gl_line_vertices(s, a, b, 0, 0, 0);
            qtest_writel(s, DE + 0x60, 0x01000040);
            qtest_writeq(s, DE + 0xc0, UINT64_C(0x02040102) << 32 | UINT32_MAX);
            save_paired_renderer(s, path, before);
            value = f->offset == 0x1f0 ? 0xdeadbeef :
                    before[192 + f->offset / 4];
            if (pair) {
                value = value << 32 | before[192 + f->offset / 4 + 1];
            }
            if (f->offset != 0x1f0) {
                g_assert_cmpint(memtx_write(s, DE + f->offset,
                    value ^ (pair ? 0x100000001000ULL : 0x1000), f->width),
                    ==, MEMTX_OK);
            }
            g_test_message("line START +0x%x width %u smooth %u",
                           f->offset, f->width, smooth);
            g_assert_cmpint(memtx_write(s, DE + 0x800 + f->offset,
                                        value, f->width), ==, MEMTX_OK);
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 4; x++) {
                    bool covered = x < 3 && (y == 1 || y == 2);

                    expected[4 * y + x] = c.old ^ (covered ? v.color : 0);
                    g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4),
                                    ==, covered ? 0x54000012 : v.old);
                }
            }
            shape_memory(s, &c, expected);
            save_paired_renderer(s, path, after);
            g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
            for (unsigned poll = 0; poll < 3; poll++) {
                qtest_readl(s, STATUS);
                qtest_readl(s, DE + 0xc0);
                qtest_readl(s, DE + 0xc4);
                qtest_writel(s, DE + 0x1f0, UINT32_MAX);
                qtest_writel(s, DE + 0x1f8, 0);
            }
            shape_memory(s, &c, expected);
            save_paired_renderer(s, path, after);
            g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_line_composed_widths(gconstpointer selection)
{
    const Register held[] = {
        { 0x880, 4 }, { 0x884, 4 }, { 0x888, 4 }, { 0x88c, 4 },
        { 0x890, 4 }, { 0x894, 4 }, { 0x880, 8 }, { 0x888, 8 },
        { 0x990, 4 }, { 0x994, 4 }, { 0x990, 8 },
        { 0x910, 4 }, { 0x918, 8 }, { 0x920, 8 }, { 0x928, 8 },
        { 0x930, 4 }, { 0x934, 4 }, { 0x938, 8 }, { 0x940, 8 },
        { 0x948, 8 }, { 0x950, 8 }, { 0x958, 4 }, { 0x95c, 4 },
        { 0x960, 4 }, { 0x998, 4 }, { 0x9a0, 4 }, { 0x9a8, 4 },
        { 0x8b0, 4 }, { 0x8b4, 4 },
    };
    Register forms[64];
    unsigned count = gl_transfer_starts(forms);
    g_autofree char *dir =
        g_dir_make_tmp("crime-line-composed-width-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    Shape c = gl_line_prepare(s, false);
    SZCase v = sz_case();
    uint64_t value;

    sz_initialize(s, &c, v.old);
    v.draw |= 0x80000;
    sz_program(s, &v);
    qtest_writeq(s, DE + 0xc0, UINT64_C(0x02040102) << 32 | UINT32_MAX);
    unsigned group = GPOINTER_TO_UINT(selection);

    for (unsigned n = 0; group == 0 && n < ARRAY_SIZE(held); n++) {
        sz_atomic_failure(s, &c, path, DE + held[n].offset,
                          0xa579b3e16c28f04dULL, held[n].width);
    }
    for (unsigned n = count * group / 4; n < count * (group + 1) / 4; n++) {
        const Register *f = &forms[n];
        uint64_t address = DE + 0x800 + f->offset;

        if (f->offset == 0xb0 || f->offset == 0xb4 ||
            (f->offset >= 0x110 && f->offset <= 0x15c)) {
            continue;
        }
        for (unsigned width = 1; width <= 8; width *= 2) {
            bool admitted_pair = width == 8 && (f->offset == 0x70 ||
                f->offset == 0xa8 || f->offset == 0xc0);

            unsigned native_width = f->offset == 0x70 || f->offset == 0xa8 ||
                                    f->offset == 0xc0 ? 4 : f->width;

            if (width != native_width && !admitted_pair) {
                sz_atomic_failure(s, &c, path, address, UINT64_MAX, width);
            }
            g_assert_cmpint(memtx_read(s, address, &value, width),
                            !=, MEMTX_OK);
        }
        for (unsigned delta = 1; delta < 4; delta++) {
            sz_atomic_failure(s, &c, path, address + delta,
                              UINT64_MAX, f->width);
        }
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_line_composed_mappings(void)
{
    g_autofree char *dir =
        g_dir_make_tmp("crime-line-composed-map-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_indexed_state();
    const int a[] = { 0, 0 }, b[] = { 132 * 64, 0 };

    for (unsigned kind = 0; kind < 11; kind++) {
        Shape c = pipeline_prepare(s, 0x228, 0);
        SZCase v = sz_case();
        uint32_t expected[132];

        c.width = 132;
        c.height = 1;
        map_region(s, 0, 4, 0, 0, 131, 0);
        shape_memory(s, &c, NULL);
        sz_initialize(s, &c, v.old);
        v.draw = TRIANGLE_DRAW | 0x80007;
        v.stencil = 0x62ff7002;
        sz_program(s, &v);
        gl_line_vertices(s, a, b, 0, 0, 0);
        qtest_writel(s, DE + 0x60, 0x01000020);
        qtest_writeq(s, DE + 0xc0, UINT64_C(0x02040102) << 32 | UINT32_MAX);
        switch (kind) {
        case 0:
            tile(s, 0, 1, 0x7fff);
            break;
        case 1:
            qtest_writeq(s, RE + 0x1000 + 2 * 0x200,
                qtest_readq(s, RE + 0x1000 + 2 * 0x200) &
                ~(UINT64_C(0x8000) << 32));
            break;
        case 2:
            tile(s, 0, 1, page_for(0, 0));
            break;
        case 3:
            tile(s, 2, 1, page_for(2, 0));
            break;
        case 4:
            tile(s, 2, 0, page_for(0, 0));
            break;
        case 5:
            qtest_writeq(s, RE + 0x1000 + 0 * 0x200,
                qtest_readq(s, RE + 0x1000 + 0 * 0x200) &
                ~(UINT64_C(0x8000) << 32));
            break;
        case 6:
            qtest_writel(s, DE + 0xc4, 0);
            tile(s, 0, 0, 0x7fff);
            tile(s, 2, 0, 0x7fff);
            break;
        case 7:
            qtest_writel(s, DE + 0x18, v.draw | 0x800);
            qtest_writel(s, DE + 0x198, 0);
            tile(s, 0, 0, 0x7fff);
            tile(s, 2, 0, 0x7fff);
            break;
        case 8:
            qtest_writel(s, DE + 0x1c0, 0);
            qtest_writel(s, DE + 0x1e0, 0x62ff7030);
            tile(s, 0, 0, 0x7fff);
            break;
        case 9:
            qtest_writel(s, DE + 0x18, (v.draw & ~0x7bU) | 4);
            tile(s, 2, 1, page_for(2, 0));
            break;
        case 10:
            qtest_writeq(s, RE + 0x1000 + 0 * 0x200,
                qtest_readq(s, RE + 0x1000 + 0 * 0x200) &
                ~(UINT64_C(0x8000) << 48));
            qtest_writeq(s, RE + 0x1000 + 0 * 0x200,
                qtest_readq(s, RE + 0x1000 + 0 * 0x200) &
                ~(UINT64_C(0x8000) << 32));
            qtest_writeq(s, RE + 0x1000 + 2 * 0x200,
                qtest_readq(s, RE + 0x1000 + 2 * 0x200) &
                ~(UINT64_C(0x8000) << 32));
            break;
        }
        g_test_message("line mapping case %u", kind);
        if (kind < 5 || kind == 10) {
            sz_atomic_failure(s, &c, path, DE + 0x8d0, 0xf13579bd, 4);
            continue;
        }
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        for (unsigned x = 0; x < 132; x++) {
            expected[x] = kind == 5 && x < 128 ? v.color : c.old;
            if (kind != 9) {
                uint32_t z = kind == 5 ? 0x62000012 :
                             kind == 8 ? 0x54000011 : v.old;

                g_assert_cmphex(pixel_read(s, pixel_address(2, x, 0, 4), 4),
                                ==, z);
            }
        }
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_line_composed_migration(void)
{
    g_autofree char *dir =
        g_dir_make_tmp("crime-line-composed-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *from = start(), *to = start_options("-incoming defer");
    Shape c = gl_line_prepare(from, false);
    SZCase v = sz_case();
    uint64_t before[RENDER_WORDS], after[RENDER_WORDS];
    uint32_t expected[16];

    sz_initialize(from, &c, v.old);
    v.draw = TRIANGLE_DRAW | 0x80207;
    v.stencil = 0x62ff7003;
    v.dx = 4096;
    sz_program(from, &v);
    qtest_writel(from, DE + 0x60, 0x01000040);
    qtest_writeq(from, DE + 0xc0, UINT64_C(0x02040102) << 32 | 0x28000000);
    g_assert_cmpint(memtx_writel(from, DE + 0x9f0, 0), ==, MEMTX_OK);
    qtest_writel(from, DE + 0xc0, 0xe0e00000);
    sz_atomic_failure(from, &c, path, DE + 0x8c4, UINT32_MAX, 4);
    qtest_writeq(from, DE + 0x190, UINT64_MAX);
    qtest_writeq(from, DE + 0x90, UINT64_MAX);
    save_paired_renderer(from, path, before);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    save_paired_renderer(to, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            bool selected = x < 2 && (y == 1 || y == 2);

            expected[4 * y + x] = c.old ^ (selected ? v.color : 0);
            g_assert_cmphex(pixel_read(to, pixel_address(2, x, y, 4), 4), ==,
                            selected ? 0x54000000 | (18 + x) : v.old);
        }
    }
    shape_memory(to, &c, expected);
    sz_atomic_failure(to, &c, path, DE + 0x9f0, 0, 4);
    qtest_writel(to, DE + 0xc0, 0x02040102);
    g_assert_cmpint(memtx_writel(to, DE + 0x9f0, 0), ==, MEMTX_OK);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            bool selected = x < 2 && (y == 1 || y == 2);

            expected[4 * y + x] = c.old;
            g_assert_cmphex(pixel_read(to, pixel_address(2, x, y, 4), 4), ==,
                            selected ? 0x55000000 | (18 + x) : v.old);
        }
    }
    shape_memory(to, &c, expected);
    qtest_system_reset(to);
    shape_memory(to, &c, expected);
    save_renderer(to, path, after);
    for (unsigned i = 0; i < RENDER_WORDS; i++) {
        g_assert_cmphex(after[i], ==, 0);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(sock);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_gl_line_composed_lengths(void)
{
    const unsigned lengths[] = { 0, 1, 126, 127, 128, 129, 260 };
    QTestState *s = start_indexed_state();

    for (unsigned reverse = 0; reverse < 2; reverse++) {
        for (unsigned n = 0; n < ARRAY_SIZE(lengths); n++) {
            Shape c = pipeline_prepare(s, 0x228, 0);
            SZCase v = sz_case();
            const int a[] = { 0, 64 }, b[] = { lengths[n] * 64, 64 };
            uint32_t expected[261 * 4];

            c.width = 261;
            map_region(s, 0, 4, 0, 0, 260, 3);
            shape_memory(s, &c, NULL);
            sz_initialize(s, &c, v.old);
            v.draw |= 0x80000;
            sz_program(s, &v);
            gl_line_vertices(s, reverse ? b : a, reverse ? a : b, 0, 0, 0);
            qtest_writel(s, DE + 0x60, 0x01000020);
            qtest_writeq(s, DE + 0xc0, UINT64_C(0x1e1ffeff) << 32 | 2);
            for (unsigned y = 0; y < 4; y++) {
                for (unsigned x = 0; x < 261; x++) {
                    int i = reverse ? (int)lengths[n] - (int)x : (int)x;
                    bool selected = i >= 0 && i < lengths[n] &&
                                    (i < 2 || i >= 258) && y == 1;

                    expected[261 * y + x] = selected ? v.color : c.old;
                }
            }
            g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
            shape_memory(s, &c, expected);
        }
    }
    qtest_quit(s);
}

static void test_gl_line_composed_allocation(void)
{
#ifdef CONFIG_LINUX
    const uint64_t headroom[] = {
        16 * MiB, 160 * MiB, 16 * MiB, 160 * MiB,
    };
    g_autofree char *dir =
        g_dir_make_tmp("crime-line-composed-alloc-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    const int a[] = { 0, 1024 * 64 }, b[] = { 2048 * 64, 1024 * 64 };

    for (unsigned n = 0; n < ARRAY_SIZE(headroom); n++) {
        g_autofree char *log = g_strdup_printf("%s/errors-%u.txt", dir, n);
        g_autofree char *args = g_strdup_printf("-D %s", log);
        QTestState *s = start_profile_ram("legacy-software", false,
                                          true, false, args);
        Shape c = gl_line_prepare(s, false);
        SZCase v = sz_case();
        g_autofree char *stat_path = g_strdup_printf("/proc/%ld/statm",
                                                     (long)qtest_pid(s));
        g_autofree char *stat = NULL, *errors = NULL;
        struct rlimit before, limited;
        unsigned long long pages;

        sz_initialize(s, &c, v.old);
        v.draw |= 0x180000;
        sz_program(s, &v);
        gl_line_vertices(s, a, b, 0, 0, 0);
        qtest_writel(s, DE + 0x60, 0x0100ffff);
        qtest_writeq(s, DE + 0xc0, UINT64_C(0x02040102) << 32 | UINT32_MAX);
        qtest_writeq(s, DE + 0x48, bounds(0, 0, 4, 4));
        g_assert_true(g_file_get_contents(stat_path, &stat, NULL, NULL));
        g_assert_cmpint(sscanf(stat, "%llu", &pages), ==, 1);
        g_assert_cmpint(prlimit(qtest_pid(s), RLIMIT_AS, NULL, &before), ==, 0);
        limited = before;
        limited.rlim_cur = pages * (uint64_t)qemu_real_host_page_size() +
                           headroom[n];
        g_assert_cmpuint(limited.rlim_cur, <=, before.rlim_max);
        g_assert_cmpint(prlimit(qtest_pid(s), RLIMIT_AS, &limited, NULL),
                        ==, 0);
        sz_atomic_failure(s, &c, path, DE + 0x8d0, 0xf13579bd, 4);
        g_assert_cmpint(prlimit(qtest_pid(s), RLIMIT_AS, &before, NULL), ==, 0);
        qtest_quit(s);
        g_assert_true(g_file_get_contents(log, &errors, NULL, NULL));
        g_test_message("allocation %u diagnostic: %s", n, errors);
        g_assert_nonnull(strstr(errors, n % 2 ? "primitive allocation failed" :
                                           "invalid GL line producer"));
        g_unlink(log);
    }
    g_unlink(path);
    g_rmdir(dir);
#else
    g_test_skip("requires Linux per-process address-space limits");
#endif
}

static void test_gl_line_composed_alpha(void)
{
    const unsigned references[] = { 0, 64, 128, 255 };
    QTestState *s = start_indexed_state();

    for (unsigned smooth = 0; smooth < 2; smooth++) {
        for (unsigned function = 0; function < 8; function++) {
            for (unsigned r = 0; r < ARRAY_SIZE(references); r++) {
                SZCase v = sz_case();

                v.draw = TRIANGLE_DRAW | 0x80f87;
                v.draw = (v.draw & ~0x78U) | ((function * 7) % 16) << 3;
                v.stencil = 0x62ff7003;
                v.dx = 4096;
                v.dy = -8192;
                v.mode = gl_rectangle_modes[(function + r) % 10];
                v.bank = function % 2;
                v.plane = 0xa579b3e1;
                gl_line_sz_alpha_run(s, &v, smooth,
                                     function << 8 | references[r]);
            }
        }
    }
    qtest_quit(s);
}

static void test_gl_line_composed_depth_bounds(void)
{
    const int64_t limit = INT64_C(1) << 36;
    const int64_t values[] = {
        -limit, -limit + 1, -4097, -4096, -1, 0, 1, 4095, 4096, 4097,
        INT64_C(0xffffff) * 4096 - 1, INT64_C(0xffffff) * 4096,
        INT64_C(0xffffff) * 4096 + 4095, limit - 1, limit,
    };
    const int a[] = { 0, 64 }, b[] = { 64, 64 };
    QTestState *s = start_indexed_state();

    for (unsigned n = 0; n < ARRAY_SIZE(values); n++) {
        SZCase v = sz_case();

        v.draw |= 0x80000;
        v.start = values[n];
        gl_line_sz_run(s, &v, false);
    }
    for (unsigned signs = 0; signs < 8; signs++) {
        Shape c = gl_line_prepare(s, false);
        SZCase v = sz_case();
        uint32_t expected[16];

        sz_initialize(s, &c, v.old);
        v.draw |= 0x80000;
        v.start = signs & 1 ? limit : -limit;
        v.dx = signs & 2 ? limit : -limit;
        v.dy = signs & 4 ? limit : -limit;
        sz_program(s, &v);
        gl_line_vertices(s, a, b, 0, 0, 0);
        qtest_writel(s, DE + 0x60, 0x01000020);
        qtest_writeq(s, DE + 0xc0, UINT64_C(0x02040102) << 32 | UINT32_MAX);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                bool covered = x == 0 && y == 1;

                expected[4 * y + x] = covered ? v.color : c.old;
                g_assert_cmphex(pixel_read(s, pixel_address(2, x, y, 4), 4), ==,
                    covered ? 0x53000000 | sz_reference_depth(v.start) : v.old);
            }
        }
        shape_memory(s, &c, expected);
        /* A zero-length command consumes fields, but no nonexistent sample. */
        gl_line_vertices(s, a, a, 0, 0, 0);
        g_assert_cmpint(memtx_writel(s, DE + 0x9f0, 0), ==, MEMTX_OK);
        shape_memory(s, &c, expected);
    }
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    take_group_paths(&argc, argv);
    g_test_init(&argc, &argv, NULL);
    add_case("sgi/crime/render/alpha/comparisons", test_alpha_comparisons);
    add_case("sgi/crime/render/alpha/geometry", test_alpha_geometry);
    add_case("sgi/crime/render/alpha/validity", test_alpha_validity);
    add_case("sgi/crime/render/alpha/admission", test_alpha_admission);
    add_case("sgi/crime/render/alpha/start-forms", test_alpha_start_forms);
    add_case("sgi/crime/render/alpha/state", test_alpha_state);
    add_case("sgi/crime/render/alpha/migration", test_alpha_migration);
    add_case("sgi/crime/render/native/translations", test_native_translations);
    add_case("sgi/crime/render/native/parameters", test_native_parameters);
    add_case("sgi/crime/render/native/isolation", test_native_isolation);
    add_case("sgi/crime/render/native/migration", test_native_migration);
    add_case("sgi/crime/render/native/clear-sources",
             test_native_clear_sources);
    add_case("sgi/crime/render/native/profiles", test_native_profiles);
    add_case("sgi/crime/render/ordinary/copies",
             test_ordinary_copies);
    add_case("sgi/crime/render/ordinary/copy-faults",
             test_ordinary_copy_faults);
    add_case("sgi/crime/render/ordinary/copy-banks",
             test_ordinary_copy_banks);

    add_case("sgi/crime/render/ordinary/logic",
             test_ordinary_logic);
    add_case("sgi/crime/render/ordinary/unions",
             test_ordinary_unions);
    add_case("sgi/crime/render/ordinary/faults",
             test_ordinary_faults);
    add_case("sgi/crime/render/ordinary/suppression",
             test_ordinary_suppression);
    add_case("sgi/crime/render/ordinary/start-forms",
             test_ordinary_start_forms);
    add_case("sgi/crime/render/ordinary/lifecycle",
             test_ordinary_lifecycle);

    add_case("sgi/crime/render/ordinary/pattern-start",
             test_ordinary_pattern_start);
    add_case("sgi/crime/render/ordinary/phase", test_ordinary_phase);
    add_case("sgi/crime/render/ordinary/geometry", test_ordinary_geometry);
    add_case("sgi/crime/render/ordinary/origins", test_ordinary_origins);

    add_data_case("sgi/crime/render/transfer/upload/aliases",
                  GUINT_TO_POINTER(0), test_transfer_aliases);
    add_data_case("sgi/crime/render/transfer/upload/ram-views",
                  GUINT_TO_POINTER(0), test_transfer_ram_views);
    add_data_case("sgi/crime/render/transfer/upload/boundaries",
                  GUINT_TO_POINTER(0), test_transfer_boundaries);
    add_data_case("sgi/crime/render/transfer/upload/semantics",
                  GUINT_TO_POINTER(0), test_transfer_semantics);
    add_data_case("sgi/crime/render/transfer/upload/actual-accesses",
                  GUINT_TO_POINTER(0), test_transfer_actual_accesses);
    add_data_case("sgi/crime/render/transfer/upload/start-forms",
                  GUINT_TO_POINTER(0), test_transfer_start_forms);
    add_data_case("sgi/crime/render/transfer/upload/migration",
                  GUINT_TO_POINTER(0), test_transfer_migration);
    add_data_case("sgi/crime/render/transfer/download/aliases",
                  GUINT_TO_POINTER(1), test_transfer_aliases);
    add_data_case("sgi/crime/render/transfer/download/ram-views",
                  GUINT_TO_POINTER(1), test_transfer_ram_views);
    add_data_case("sgi/crime/render/transfer/download/boundaries",
                  GUINT_TO_POINTER(1), test_transfer_boundaries);
    add_data_case("sgi/crime/render/transfer/download/semantics",
                  GUINT_TO_POINTER(1), test_transfer_semantics);
    add_data_case("sgi/crime/render/transfer/download/actual-accesses",
                  GUINT_TO_POINTER(1), test_transfer_actual_accesses);
    add_data_case("sgi/crime/render/transfer/download/start-forms",
                  GUINT_TO_POINTER(1), test_transfer_start_forms);
    add_data_case("sgi/crime/render/transfer/download/migration",
                  GUINT_TO_POINTER(1), test_transfer_migration);
    add_case("sgi/crime/render/transfer/accesses", test_transfer_accesses);
    add_data_case("sgi/crime/render/transfer/upload/formats",
                  GUINT_TO_POINTER(0), test_transfer_formats);
    add_data_case("sgi/crime/render/transfer/upload/rops",
                  GUINT_TO_POINTER(0), test_transfer_rops);
    add_data_case("sgi/crime/render/transfer/upload/masks",
                  GUINT_TO_POINTER(0), test_transfer_masks);
    add_data_case("sgi/crime/render/transfer/upload/clipping",
                  GUINT_TO_POINTER(0), test_transfer_clipping);
    add_data_case("sgi/crime/render/transfer/download/formats",
                  GUINT_TO_POINTER(1), test_transfer_formats);
    add_data_case("sgi/crime/render/transfer/download/rops",
                  GUINT_TO_POINTER(1), test_transfer_rops);
    add_data_case("sgi/crime/render/transfer/download/masks",
                  GUINT_TO_POINTER(1), test_transfer_masks);
    add_data_case("sgi/crime/render/transfer/download/clipping",
                  GUINT_TO_POINTER(1), test_transfer_clipping);
    add_case("sgi/crime/render/x-domain/invisible", test_x_domain_invisible);
    add_case("sgi/crime/render/x-domain/coordinates",
             test_x_domain_coordinates);
    add_case("sgi/crime/render/x-domain/stipple-origins",
             test_x_domain_stipple_origins);
    add_case("sgi/crime/render/x-domain/bounds", test_x_domain_bounds);
    add_case("sgi/crime/render/x-domain/transactions",
             test_x_domain_transactions);
    add_case("sgi/crime/render/buffer-modes/indexed",
             test_buffer_modes_indexed);
    add_case("sgi/crime/render/buffer-modes/color", test_buffer_modes_color);
    add_case("sgi/crime/render/buffer-modes/transactions",
             test_buffer_modes_transactions);
    add_case("sgi/crime/render/buffer-modes/diagnostics",
             test_buffer_modes_diagnostics);
    add_case("sgi/crime/render/buffer-modes/primitives",
             test_buffer_modes_primitives);
    add_case("sgi/crime/render/color/components", test_color_components);
    add_case("sgi/crime/render/color/tiled-traversal",
             test_color_tiled_traversal);
    add_case("sgi/crime/render/color/formats", test_color_formats);
    add_case("sgi/crime/render/color/rops-masks", test_color_rops_masks);
    add_case("sgi/crime/render/color/geometry", test_color_geometry);
    add_case("sgi/crime/render/color/failures", test_color_failures);
    add_case("sgi/crime/render/color/starts-accesses",
             test_color_starts_accesses);
    add_case("sgi/crime/render/color/ram-views", test_color_ram_views);
    add_case("sgi/crime/render/transfer/upload-regression",
             test_transfer_upload_regression);
    add_case("sgi/crime/render/indexed-word/clip-slot-shapes",
                   test_clip_slot_shapes);
    add_case("sgi/crime/render/indexed-word/migration",
                   test_indexed_word_migration);
    add_case("sgi/crime/render/indexed-word/start-forms",
                   test_indexed_word_start_forms);
    add_case("sgi/crime/render/indexed-word/failure",
                   test_indexed_word_failure);
    add_case("sgi/crime/render/indexed-word/accesses",
                   test_indexed_word_accesses);
    add_case("sgi/crime/render/indexed-word/packed-unions",
                   test_clip_unions_packed);
    add_case("sgi/crime/render/indexed-word/rops",
                   test_indexed_word_rops);
    add_case("sgi/crime/render/indexed-word/masks",
                   test_indexed_word_masks);
    add_case("sgi/crime/render/indexed-word/geometry",
                   test_indexed_word_geometry);
    add_case("sgi/crime/render/indexed-word/clipping",
                   test_indexed_word_clipping);
    add_case("sgi/crime/render/indexed-word/regression",
                   test_indexed_word_regression);
    add_case("sgi/crime/render/paired-start/accesses",
                   test_paired_start_accesses);
    add_case("sgi/crime/render/paired-start/migration",
                   test_paired_start_migration);
    add_case("sgi/crime/render/paired-start/shapes",
                   test_paired_start_shapes);
    add_case("sgi/crime/render/paired-start/rollback",
                   test_paired_start_rollback);
    add_case("sgi/crime/render/paired-start/copy",
                   test_paired_start_copy);
    add_case("sgi/crime/render/fill", test_fill);
    add_case("sgi/crime/render/registers", test_registers);
    add_case("sgi/crime/render/tile-boundaries", test_tile_boundaries);
    add_case("sgi/crime/render/rops", test_rops);
    add_case("sgi/crime/render/masks", test_masks);
    add_case("sgi/crime/render/overlap", test_overlap);
    add_case("sgi/crime/render/clipping", test_clipping);
    add_case("sgi/crime/render/actual-accesses", test_actual_accesses);
    add_case("sgi/crime/render/start-forms", test_start_forms);
    add_case("sgi/crime/render/atomic-commands", test_atomic_commands);
    add_case("sgi/crime/render/mte-start-forms", test_mte_start_forms);
    add_case("sgi/crime/render/mte-scroll", test_mte_scroll);
    add_case("sgi/crime/render/mte-rejected", test_mte_rejected);
    add_case("sgi/crime/render/malformed-access-stops",
                   test_malformed_access_stops);
    add_case("sgi/crime/render/unsupported", test_unsupported);
    add_case("sgi/crime/render/migration", test_migration);
    add_case("sgi/crime/render/banked-decoder", test_banked_decoder);
    add_case("sgi/crime/render/unselected-profile",
                   test_unselected_profile);
    add_case("sgi/crime/render/coordinate-limit", test_coordinate_limit);
    if (qtest_has_machine("o2")) {
        add_case("sgi/crime/render/rom-canary", test_rom_canary);
    }
    add_case("sgi/crime/render/wide-slot", test_wide_slot);
    add_case("sgi/crime/render/wide-slot-state", test_wide_slot_state);
    add_case("sgi/crime/render/wide-pairs", test_wide_pairs);
    add_case("sgi/crime/render/wide-rejected", test_wide_rejected);
    add_case("sgi/crime/render/native64-start", test_native64_start);
    add_case("sgi/crime/render/wide-migration", test_wide_migration);
    add_case("sgi/crime/render/point", test_point);
    add_case("sgi/crime/render/point-line-stipple/point-geometry",
                   test_point_geometry);
    add_case("sgi/crime/render/point-line-stipple/line-geometry",
                   test_line_geometry);
    add_case("sgi/crime/render/point-line-stipple/line-width",
                   test_line_width);
    add_case("sgi/crime/render/point-line-stipple/stipple-phase",
                   test_stipple_phase);
    add_case("sgi/crime/render/point-line-stipple/stipple-pipeline",
                   test_stipple_pipeline);
    add_case("sgi/crime/render/point-line-stipple/stipple-clipping",
                   test_stipple_clipping);
    add_case("sgi/crime/render/point-line-stipple/stipple-state",
                   test_stipple_state);
    add_case("sgi/crime/render/point-line-stipple/stipple-masks-colors",
                   test_stipple_masks_colors);
    add_case("sgi/crime/render/point-line-stipple/stipple-invalid",
                   test_stipple_invalid);
    add_case("sgi/crime/render/point-line-stipple/stipple-accesses",
                   test_stipple_accesses);
    add_case("sgi/crime/render/point-line-stipple/stipple-null",
                   test_stipple_null);
    add_case("sgi/crime/render/point-line-stipple/line-shared-endpoint",
                   test_line_shared_endpoint);
    add_case("sgi/crime/render/point-line-stipple/stipple-start-forms",
                   test_stipple_start_forms);
    add_case("sgi/crime/render/point-line-stipple/stipple-migration",
                   test_stipple_migration);
    add_case("sgi/crime/render/point-line-stipple/shape-tile-banks",
                   test_shape_tile_banks);
    add_case("sgi/crime/render/point-line-stipple/stipple-inclusive",
                   test_stipple_inclusive);
    add_case("sgi/crime/render/point-line-stipple/bank-decoder",
                   test_shape_bank_decoder);
    add_case("sgi/crime/render/tiled-validity/fill",
                   test_tiled_validity_fill);
    add_case("sgi/crime/render/tiled-validity/banks-depths",
                   test_validity_banks_depths);
    add_case("sgi/crime/render/tiled-validity/geometry-phase",
                   test_validity_geometry_phase);
    add_case("sgi/crime/render/tiled-validity/clips-masks",
                   test_validity_clips_masks);
    add_case("sgi/crime/render/tiled-validity/faults",
                   test_validity_faults);
    add_case("sgi/crime/render/tiled-validity/semantics",
                   test_validity_semantics);
    add_case("sgi/crime/render/tiled-validity/copies",
                   test_validity_copies);
    add_case("sgi/crime/render/tiled-validity/retirement",
                   test_validity_retirement);
    add_case("sgi/crime/render/tiled-validity/migration",
                   test_validity_migration);
    add_case("sgi/crime/render/tiled-validity/profiles",
                   test_validity_profiles);
    add_case("sgi/crime/render/triangles/packet",
             test_triangle_packet);
    add_case("sgi/crime/render/triangles/coverage",
             test_triangle_coverage);
    add_case("sgi/crime/render/triangles/neighbors",
             test_triangle_neighbors);
    add_case("sgi/crime/render/triangles/pipeline",
             test_triangle_pipeline);
    add_case("sgi/crime/render/triangles/admission",
             test_triangle_admission);
    add_case("sgi/crime/render/triangles/transactions",
             test_triangle_transactions);
    add_case("sgi/crime/render/triangles/tiles",
             test_triangle_tiles);
    add_case("sgi/crime/render/triangles/migration",
             test_triangle_migration);
    add_case("sgi/crime/render/gl-window/coordinates",
             test_gl_window_coordinates);
    add_case("sgi/crime/render/gl-window/clipping", test_gl_window_clipping);
    add_case("sgi/crime/render/gl-window/restore-word", test_gl_restore_word);
    add_case("sgi/crime/render/ifb/ram", test_ifb_ram);
    add_case("sgi/crime/render/ifb/transactions", test_ifb_transactions);
    add_case("sgi/crime/render/ifb/wrap", test_ifb_wrap);
    add_case("sgi/crime/render/ifb/status-bank-zero",
             test_ifb_status_bank_zero);
    add_case("sgi/crime/render/driver-evidence/linear-identity",
             test_evidence_linear_identity);
    add_case("sgi/crime/render/driver-evidence/mte-block-copy",
             test_evidence_mte_block_copy);
    add_case("sgi/crime/render/driver-evidence/parameters-across-mte",
             test_evidence_parameters_across_mte);
    add_case("sgi/crime/render/driver-evidence/unsupported-stages",
             test_evidence_unsupported_stages);
    add_case("sgi/crime/render/ifb/atomic", test_ifb_atomic);
    add_case("sgi/crime/render/ifb/restore", test_ifb_restore);
    add_case("sgi/crime/render/ifb/migration", test_ifb_migration);
    add_case("sgi/crime/render/ifb/profiles", test_ifb_profiles);
    add_case("sgi/crime/render/ifb/invalid-stream",
             test_ifb_invalid_stream);
    add_case("sgi/crime/render/pipeline/start", test_shade_start_family);
    add_case("sgi/crime/render/pipeline/smooth",
             test_pipeline_smooth);
    add_case("sgi/crime/render/pipeline/blend-fixture",
             test_pipeline_blend_fixture);
    add_case("sgi/crime/render/pipeline/quantizer",
             test_pipeline_quantizer);
    add_data_case("sgi/crime/render/pipeline/factors/0",
                  GUINT_TO_POINTER(0), test_pipeline_factors);
    add_data_case("sgi/crime/render/pipeline/factors/1",
                  GUINT_TO_POINTER(1), test_pipeline_factors);
    add_data_case("sgi/crime/render/pipeline/factors/2",
                  GUINT_TO_POINTER(2), test_pipeline_factors);
    add_data_case("sgi/crime/render/pipeline/factors/3",
                  GUINT_TO_POINTER(3), test_pipeline_factors);
    add_data_case("sgi/crime/render/pipeline/factors/4",
                  GUINT_TO_POINTER(4), test_pipeline_factors);
    add_data_case("sgi/crime/render/pipeline/factors/5",
                  GUINT_TO_POINTER(5), test_pipeline_factors);
    add_data_case("sgi/crime/render/pipeline/identity/0",
                  GUINT_TO_POINTER(0), test_pipeline_identity);
    add_data_case("sgi/crime/render/pipeline/identity/1",
                  GUINT_TO_POINTER(1), test_pipeline_identity);
    add_data_case("sgi/crime/render/pipeline/identity/2",
                  GUINT_TO_POINTER(2), test_pipeline_identity);
    add_data_case("sgi/crime/render/pipeline/identity/3",
                  GUINT_TO_POINTER(3), test_pipeline_identity);
    add_data_case("sgi/crime/render/pipeline/identity/4",
                  GUINT_TO_POINTER(4), test_pipeline_identity);
    add_data_case("sgi/crime/render/pipeline/identity/5",
                  GUINT_TO_POINTER(5), test_pipeline_identity);
    add_data_case("sgi/crime/render/pipeline/masks/0",
                  GUINT_TO_POINTER(0), test_pipeline_masks);
    add_data_case("sgi/crime/render/pipeline/masks/1",
                  GUINT_TO_POINTER(1), test_pipeline_masks);
    add_data_case("sgi/crime/render/pipeline/masks/2",
                  GUINT_TO_POINTER(2), test_pipeline_masks);
    add_data_case("sgi/crime/render/pipeline/masks/3",
                  GUINT_TO_POINTER(3), test_pipeline_masks);
    add_data_case("sgi/crime/render/pipeline/masks/4",
                  GUINT_TO_POINTER(4), test_pipeline_masks);
    add_data_case("sgi/crime/render/pipeline/masks/5",
                  GUINT_TO_POINTER(5), test_pipeline_masks);
    add_data_case("sgi/crime/render/pipeline/masks/6",
                  GUINT_TO_POINTER(6), test_pipeline_masks);
    add_case("sgi/crime/render/pipeline/planes",
             test_pipeline_planes);
    add_case("sgi/crime/render/pipeline/shade-bounds",
             test_pipeline_shade_bounds);
    add_case("sgi/crime/render/pipeline/transactions",
             test_pipeline_transactions);
    add_case("sgi/crime/render/pipeline/late-faults",
             test_pipeline_late_faults);
    add_case("sgi/crime/render/pipeline/storage",
             test_pipeline_storage);
    add_case("sgi/crime/render/pipeline/phase",
             test_pipeline_phase);
    add_case("sgi/crime/render/pipeline/populated-banks",
             test_pipeline_populated_banks);
    add_case("sgi/crime/render/pipeline/migration",
             test_pipeline_migration);
    add_data_case("sgi/crime/render/pipeline/inactive-line/0",
                  GUINT_TO_POINTER(0), test_pipeline_inactive_line_control);
    add_data_case("sgi/crime/render/pipeline/inactive-line/1",
                  GUINT_TO_POINTER(1), test_pipeline_inactive_line_control);
    add_data_case("sgi/crime/render/pipeline/inactive-line/2",
                  GUINT_TO_POINTER(2), test_pipeline_inactive_line_control);
    add_data_case("sgi/crime/render/pipeline/inactive-line/3",
                  GUINT_TO_POINTER(3), test_pipeline_inactive_line_control);
    add_data_case("sgi/crime/render/pipeline/inactive-line/4",
                  GUINT_TO_POINTER(4), test_pipeline_inactive_line_control);
    add_data_case("sgi/crime/render/pipeline/inactive-line/5",
                  GUINT_TO_POINTER(5), test_pipeline_inactive_line_control);
    add_data_case("sgi/crime/render/pipeline/inactive-line/6",
                  GUINT_TO_POINTER(6), test_pipeline_inactive_line_control);
    add_case("sgi/crime/render/pipeline/line-control-refusals",
             test_pipeline_line_control_refusals);
    add_case("sgi/crime/render/pipeline/anchor-literals",
             test_pipeline_anchor_literals);
    add_case("sgi/crime/render/pipeline/anchor-floors",
             test_pipeline_anchor_floors);
    add_case("sgi/crime/render/pipeline/anchor-invariants",
             test_pipeline_anchor_invariants);
    add_case("sgi/crime/render/sz/depth-fixture", test_sz_depth_fixture);
    add_case("sgi/crime/render/sz/comparisons", test_sz_comparisons);
    add_case("sgi/crime/render/sz/stencil", test_sz_stencil);
    add_case("sgi/crime/render/sz/all-references", test_sz_all_references);
    add_case("sgi/crime/render/sz/window", test_sz_window);
    add_data_case("sgi/crime/render/sz/tags/5",
                  GUINT_TO_POINTER(5), test_sz_tags);
    add_data_case("sgi/crime/render/sz/tags/7",
                  GUINT_TO_POINTER(7), test_sz_tags);
    add_case("sgi/crime/render/sz/gl-rectangles", test_sz_gl_rectangles);
    add_case("sgi/crime/render/sz/transport", test_sz_transport);
    add_case("sgi/crime/render/sz/planes", test_sz_planes);
    add_case("sgi/crime/render/sz/fragment-order", test_sz_fragment_order);
    add_case("sgi/crime/render/sz/aliases", test_sz_aliases);
    add_case("sgi/crime/render/sz/rect-controls", test_sz_rect_controls);
    add_case("sgi/crime/render/sz/start-forms", test_sz_start_forms);
    add_case("sgi/crime/render/sz/widths-reserved", test_sz_widths_reserved);
    add_case("sgi/crime/render/sz/late-faults", test_sz_late_faults);
    add_case("sgi/crime/render/sz/migration", test_sz_migration);
    add_case("sgi/crime/render/sz/rect-clipping", test_sz_rect_clipping);
    add_case("sgi/crime/render/sz/populated-banks", test_sz_populated_banks);
    add_case("sgi/crime/render/sz/tag-literals", test_sz_tag_literals);
    add_case("sgi/crime/render/gl-line/composed-vectors",
             test_gl_line_composed_vectors);
    add_case("sgi/crime/render/gl-line/sz-composition",
             test_gl_line_sz_composition);
    add_case("sgi/crime/render/gl-line/composed-clipping",
             test_gl_line_composed_clipping);
    add_case("sgi/crime/render/gl-line/composed-failures",
             test_gl_line_composed_failures);
    add_case("sgi/crime/render/gl-line/composed-starts",
             test_gl_line_composed_starts);
    add_data_case("sgi/crime/render/gl-line/composed-widths/0",
                  GUINT_TO_POINTER(0), test_gl_line_composed_widths);
    add_data_case("sgi/crime/render/gl-line/composed-widths/1",
                  GUINT_TO_POINTER(1), test_gl_line_composed_widths);
    add_data_case("sgi/crime/render/gl-line/composed-widths/2",
                  GUINT_TO_POINTER(2), test_gl_line_composed_widths);
    add_data_case("sgi/crime/render/gl-line/composed-widths/3",
                  GUINT_TO_POINTER(3), test_gl_line_composed_widths);
    add_case("sgi/crime/render/gl-line/composed-mappings",
             test_gl_line_composed_mappings);
    add_case("sgi/crime/render/gl-line/composed-migration",
             test_gl_line_composed_migration);
    add_case("sgi/crime/render/gl-line/composed-lengths",
             test_gl_line_composed_lengths);
    add_case("sgi/crime/render/gl-line/composed-allocation",
             test_gl_line_composed_allocation);
    add_case("sgi/crime/render/gl-line/composed-alpha",
             test_gl_line_composed_alpha);
    add_case("sgi/crime/render/gl-line/composed-depth-bounds",
             test_gl_line_composed_depth_bounds);
    add_case("sgi/crime/render/gl-line/depth-stipple",
             test_gl_line_depth_stipple);
    add_case("sgi/crime/render/gl-line/first", test_gl_line_first);
    add_case("sgi/crime/render/gl-line/pair-staging",
             test_gl_line_pair_staging);
    add_case("sgi/crime/render/gl-line/aa-horizontal",
             test_gl_line_aa_horizontal);
    add_case("sgi/crime/render/gl-line/nonaa-vectors",
             test_gl_line_nonaa_vectors);
    add_case("sgi/crime/render/gl-line/precision", test_gl_line_precision);
    add_case("sgi/crime/render/gl-line/canonical", test_gl_line_canonical);
    add_data_case("sgi/crime/render/gl-line/output/0",
                  GUINT_TO_POINTER(0), test_gl_line_output);
    add_data_case("sgi/crime/render/gl-line/output/1",
                  GUINT_TO_POINTER(1), test_gl_line_output);
    add_data_case("sgi/crime/render/gl-line/output/2",
                  GUINT_TO_POINTER(2), test_gl_line_output);
    add_data_case("sgi/crime/render/gl-line/output/3",
                  GUINT_TO_POINTER(3), test_gl_line_output);
    add_data_case("sgi/crime/render/gl-line/output/4",
                  GUINT_TO_POINTER(4), test_gl_line_output);
    add_data_case("sgi/crime/render/gl-line/output/5",
                  GUINT_TO_POINTER(5), test_gl_line_output);
    add_data_case("sgi/crime/render/gl-line/output/6",
                  GUINT_TO_POINTER(6), test_gl_line_output);
    add_case("sgi/crime/render/gl-line/alpha-edges", test_gl_line_alpha_edges);
    add_case("sgi/crime/render/gl-line/pairs-atomic",
             test_gl_line_pairs_atomic);
    add_case("sgi/crime/render/gl-line/failures", test_gl_line_failures);
    add_case("sgi/crime/render/gl-line/color-anchor",
             test_gl_line_color_anchor);
    add_case("sgi/crime/render/gl-line/migration", test_gl_line_migration);
    add_case("sgi/crime/render/texture/constant", test_texture_constant);
    add_case("sgi/crime/render/texture/formats", test_texture_formats);
    add_case("sgi/crime/render/texture/perspective", test_texture_perspective);
    add_case("sgi/crime/render/texture/filtering", test_texture_filtering);
    add_case("sgi/crime/render/texture/lod", test_texture_lod);
    add_case("sgi/crime/render/texture/transactions",
             test_texture_transactions);
    add_case("sgi/crime/render/texture/addresses", test_texture_addresses);
    add_case("sgi/crime/render/texture/output-order",
             test_texture_output_order);
    add_case("sgi/crime/render/texture/anchor", test_texture_anchor);
    add_case("sgi/crime/render/texture/migration", test_texture_migration);
    add_case("sgi/crime/render/texture/border-components",
             test_texture_border_components);
    add_case("sgi/crime/render/texture/height-one", test_texture_height_one);
    add_case("sgi/crime/render/texture-start/q", test_texture_start_q);
    add_case("sgi/crime/render/texture-start/matrix",
             test_texture_start_matrix);
    add_case("sgi/crime/render/texture-start/unlisted",
             test_texture_start_unlisted);
    add_case("sgi/crime/render/texture-start/inactive",
             test_texture_start_inactive);
    add_data_case("sgi/crime/render/texture-start/widths/0",
                  GUINT_TO_POINTER(0), test_texture_start_widths);
    add_data_case("sgi/crime/render/texture-start/widths/1",
                  GUINT_TO_POINTER(1), test_texture_start_widths);
    add_data_case("sgi/crime/render/texture-start/widths/2",
                  GUINT_TO_POINTER(2), test_texture_start_widths);
    add_data_case("sgi/crime/render/texture-start/widths/3",
                  GUINT_TO_POINTER(3), test_texture_start_widths);
    add_data_case("sgi/crime/render/texture-start/widths/4",
                  GUINT_TO_POINTER(4), test_texture_start_widths);
    add_data_case("sgi/crime/render/texture-start/widths/5",
                  GUINT_TO_POINTER(5), test_texture_start_widths);
    add_data_case("sgi/crime/render/texture-start/widths/6",
                  GUINT_TO_POINTER(6), test_texture_start_widths);
    add_data_case("sgi/crime/render/texture-start/widths/7",
                  GUINT_TO_POINTER(7), test_texture_start_widths);
    add_data_case("sgi/crime/render/texture-start/widths/8",
                  GUINT_TO_POINTER(8), test_texture_start_widths);
    add_case("sgi/crime/render/texture-start/aliases",
             test_texture_start_aliases);
    add_case("sgi/crime/render/texture-start/profiles",
             test_texture_start_profiles);
    add_data_case("sgi/crime/render/texture-start/rollback/0",
                  GUINT_TO_POINTER(0), test_texture_start_rollback);
    add_data_case("sgi/crime/render/texture-start/rollback/1",
                  GUINT_TO_POINTER(1), test_texture_start_rollback);
    add_data_case("sgi/crime/render/texture-start/rollback/2",
                  GUINT_TO_POINTER(2), test_texture_start_rollback);
    add_data_case("sgi/crime/render/texture-start/rollback/3",
                  GUINT_TO_POINTER(3), test_texture_start_rollback);
    add_data_case("sgi/crime/render/texture-start/rollback/4",
                  GUINT_TO_POINTER(4), test_texture_start_rollback);
    add_data_case("sgi/crime/render/texture-start/rollback/5",
                  GUINT_TO_POINTER(5), test_texture_start_rollback);
    add_data_case("sgi/crime/render/texture-start/rollback/6",
                  GUINT_TO_POINTER(6), test_texture_start_rollback);
    add_data_case("sgi/crime/render/texture-start/rollback/7",
                  GUINT_TO_POINTER(7), test_texture_start_rollback);
    add_data_case("sgi/crime/render/texture-start/rollback/8",
                  GUINT_TO_POINTER(8), test_texture_start_rollback);
    add_case("sgi/crime/render/shade-iteration/capture",
             test_shade_iteration_capture);
    add_case("sgi/crime/render/shade-iteration/boundaries",
             test_shade_iteration_boundaries);
    add_case("sgi/crime/render/shade-iteration/periods",
             test_shade_iteration_periods);
    add_case("sgi/crime/render/shade-iteration/invariants",
             test_shade_iteration_invariants);
    add_case("sgi/crime/render/shade-iteration/raw",
             test_shade_iteration_raw);
    add_case("sgi/crime/render/shade-iteration/rollback",
             test_shade_iteration_rollback);
    add_case("sgi/crime/render/shade-iteration/migration",
             test_shade_iteration_migration);
    add_case("sgi/crime/render/gl-transfer/upload", test_gl_transfer_upload);
    add_case("sgi/crime/render/gl-transfer/download",
             test_gl_transfer_download);
    add_case("sgi/crime/render/gl-transfer/copy", test_gl_transfer_copy);
    add_case("sgi/crime/render/gl-transfer/geometry",
             test_gl_transfer_geometry);
    add_case("sgi/crime/render/gl-transfer/repeats",
             test_gl_transfer_repeats);
    add_case("sgi/crime/render/gl-transfer/formats",
             test_gl_transfer_formats);
    add_case("sgi/crime/render/gl-transfer/precision",
             test_gl_transfer_precision);
    add_case("sgi/crime/render/gl-transfer/masks", test_gl_transfer_masks);
    add_case("sgi/crime/render/gl-transfer/blend", test_gl_transfer_blend);
    add_case("sgi/crime/render/gl-transfer/starts", test_gl_transfer_starts);
    add_case("sgi/crime/render/gl-transfer/refusals",
             test_gl_transfer_refusals);
    add_data_case("sgi/crime/render/gl-transfer/widths/0",
                  GUINT_TO_POINTER(0), test_gl_transfer_widths);
    add_data_case("sgi/crime/render/gl-transfer/widths/1",
                  GUINT_TO_POINTER(1), test_gl_transfer_widths);
    add_data_case("sgi/crime/render/gl-transfer/widths/2",
                  GUINT_TO_POINTER(2), test_gl_transfer_widths);
    add_data_case("sgi/crime/render/gl-transfer/widths/3",
                  GUINT_TO_POINTER(3), test_gl_transfer_widths);
    add_data_case("sgi/crime/render/gl-transfer/widths/4",
                  GUINT_TO_POINTER(4), test_gl_transfer_widths);
    add_data_case("sgi/crime/render/gl-transfer/widths/5",
                  GUINT_TO_POINTER(5), test_gl_transfer_widths);
    add_data_case("sgi/crime/render/gl-transfer/widths/6",
                  GUINT_TO_POINTER(6), test_gl_transfer_widths);
    add_data_case("sgi/crime/render/gl-transfer/widths/7",
                  GUINT_TO_POINTER(7), test_gl_transfer_widths);
    add_data_case("sgi/crime/render/gl-transfer/widths/8",
                  GUINT_TO_POINTER(8), test_gl_transfer_widths);
    add_case("sgi/crime/render/gl-transfer/order", test_gl_transfer_order);
    add_case("sgi/crime/render/gl-transfer/mixed-overlap",
             test_gl_transfer_mixed_overlap);
    add_case("sgi/crime/render/gl-transfer/banks", test_gl_transfer_banks);
    add_case("sgi/crime/render/gl-transfer/migration",
             test_gl_transfer_migration);
    add_case("sgi/crime/render/gl-transfer/envelopes",
             test_gl_transfer_envelopes);
    add_case("sgi/crime/render/gl-transfer/allocation",
             test_gl_transfer_allocation);
    add_case("sgi/crime/render/gl-rectangle/clock", test_gl_rectangle_clock);
    add_case("sgi/crime/render/gl-rectangle/geometry",
             test_gl_rectangle_geometry);
    add_case("sgi/crime/render/gl-rectangle/alpha",
             test_gl_rectangle_alpha);
    add_case("sgi/crime/render/gl-rectangle/sz",
             test_gl_rectangle_sz);
    add_case("sgi/crime/render/gl-rectangle/failures",
             test_gl_rectangle_failures);
    add_case("sgi/crime/render/gl-rectangle/mappings",
             test_gl_rectangle_mappings);
    add_case("sgi/crime/render/gl-rectangle/starts",
             test_gl_rectangle_starts);
    add_case("sgi/crime/render/gl-rectangle/start-refusals",
             test_gl_rectangle_start_refusals);
    add_case("sgi/crime/render/gl-rectangle/migration",
             test_gl_rectangle_migration);
    add_data_case("sgi/crime/render/gl-rectangle/output/0",
                  GUINT_TO_POINTER(0), test_gl_rectangle_output);
    add_data_case("sgi/crime/render/gl-rectangle/output/1",
                  GUINT_TO_POINTER(1), test_gl_rectangle_output);
    add_data_case("sgi/crime/render/gl-rectangle/output/2",
                  GUINT_TO_POINTER(2), test_gl_rectangle_output);
    add_data_case("sgi/crime/render/gl-rectangle/output/3",
                  GUINT_TO_POINTER(3), test_gl_rectangle_output);
    add_data_case("sgi/crime/render/gl-rectangle/output/4",
                  GUINT_TO_POINTER(4), test_gl_rectangle_output);
    add_data_case("sgi/crime/render/gl-rectangle/output/5",
                  GUINT_TO_POINTER(5), test_gl_rectangle_output);
    add_data_case("sgi/crime/render/gl-rectangle/output/6",
                  GUINT_TO_POINTER(6), test_gl_rectangle_output);
    add_data_case("sgi/crime/render/gl-rectangle/output/7",
                  GUINT_TO_POINTER(7), test_gl_rectangle_output);
    add_data_case("sgi/crime/render/gl-rectangle/output/8",
                  GUINT_TO_POINTER(8), test_gl_rectangle_output);
    add_data_case("sgi/crime/render/gl-rectangle/output/9",
                  GUINT_TO_POINTER(9), test_gl_rectangle_output);
    add_case("sgi/crime/render/gl-rectangle/allocation",
             test_gl_rectangle_allocation);
    add_case("sgi/crime/render/gl-point/clock", test_gl_point_clock);
    add_case("sgi/crime/render/gl-point/cells",
             test_gl_point_cells);
    add_case("sgi/crime/render/gl-point/edges",
             test_gl_point_edges);
    add_case("sgi/crime/render/gl-point/clipping",
             test_gl_point_clipping);
    add_case("sgi/crime/render/gl-point/alpha",
             test_gl_point_alpha);
    add_case("sgi/crime/render/gl-point/sz",
             test_gl_point_sz);
    add_data_case("sgi/crime/render/gl-point/failures/masked",
                  GUINT_TO_POINTER(0), test_gl_point_failures);
    add_data_case("sgi/crime/render/gl-point/failures/clipped",
                  GUINT_TO_POINTER(1), test_gl_point_failures);
    add_data_case("sgi/crime/render/gl-point/failures/alpha-never",
                  GUINT_TO_POINTER(2), test_gl_point_failures);
    add_case("sgi/crime/render/gl-point/mappings",
             test_gl_point_mappings);
    add_case("sgi/crime/render/gl-point/starts",
             test_gl_point_starts);
    add_case("sgi/crime/render/gl-point/start-refusals",
             test_gl_point_start_refusals);
    add_case("sgi/crime/render/gl-point/migration",
             test_gl_point_migration);
    add_data_case("sgi/crime/render/gl-point/output/0",
                  GUINT_TO_POINTER(0), test_gl_point_output);
    add_data_case("sgi/crime/render/gl-point/output/1",
                  GUINT_TO_POINTER(1), test_gl_point_output);
    add_data_case("sgi/crime/render/gl-point/output/2",
                  GUINT_TO_POINTER(2), test_gl_point_output);
    add_data_case("sgi/crime/render/gl-point/output/3",
                  GUINT_TO_POINTER(3), test_gl_point_output);
    add_data_case("sgi/crime/render/gl-point/output/4",
                  GUINT_TO_POINTER(4), test_gl_point_output);
    add_data_case("sgi/crime/render/gl-point/output/5",
                  GUINT_TO_POINTER(5), test_gl_point_output);
    add_data_case("sgi/crime/render/gl-point/output/6",
                  GUINT_TO_POINTER(6), test_gl_point_output);
    add_data_case("sgi/crime/render/gl-point/output/7",
                  GUINT_TO_POINTER(7), test_gl_point_output);
    add_data_case("sgi/crime/render/gl-point/output/8",
                  GUINT_TO_POINTER(8), test_gl_point_output);
    add_data_case("sgi/crime/render/gl-point/output/9",
                  GUINT_TO_POINTER(9), test_gl_point_output);
    add_case("sgi/crime/render/gl-point-coordinate/active",
             test_gl_point_coordinate_active);
    add_case("sgi/crime/render/gl-point-coordinate/matrix",
             test_gl_point_coordinate_matrix);
    add_case("sgi/crime/render/gl-point-coordinate/equivalence",
             test_gl_point_coordinate_equivalence);
    add_case("sgi/crime/render/gl-point-coordinate/once",
             test_gl_point_coordinate_once);
    add_case("sgi/crime/render/gl-point-coordinate/dormant",
             test_gl_point_coordinate_dormant);
    add_case("sgi/crime/render/gl-point-coordinate/boundaries",
             test_gl_point_coordinate_boundaries);
    add_case("sgi/crime/render/gl-point-coordinate/atomic",
             test_gl_point_coordinate_atomic);
    add_case("sgi/crime/render/gl-point-coordinate/mapping-failures",
             test_gl_point_coordinate_mapping_failures);
    add_case("sgi/crime/render/gl-point-coordinate/widths",
             test_gl_point_coordinate_widths);
    add_case("sgi/crime/render/gl-point-coordinate/other-consumers",
             test_gl_point_coordinate_other_consumers);
    add_case("sgi/crime/render/gl-point-coordinate/profiles",
             test_gl_point_coordinate_profiles);
    add_data_case("sgi/crime/render/gl-point-coordinate/failures/masked",
                  GUINT_TO_POINTER(0), test_gl_point_coordinate_failures);
    add_data_case("sgi/crime/render/gl-point-coordinate/failures/clipped",
                  GUINT_TO_POINTER(1), test_gl_point_coordinate_failures);
    add_data_case("sgi/crime/render/gl-point-coordinate/failures/alpha-never",
                  GUINT_TO_POINTER(2), test_gl_point_coordinate_failures);
    add_data_case("sgi/crime/render/gl-point-coordinate/migration/setup",
                  GUINT_TO_POINTER(0), test_gl_point_coordinate_migration);
    add_data_case("sgi/crime/render/gl-point-coordinate/migration/committed",
                  GUINT_TO_POINTER(1), test_gl_point_coordinate_migration);
    return g_test_run();
}
