/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "memtx.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"

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
    g_assert_cmphex(qtest_readl(s, STATUS), ==, 0x1e000000);
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
    g_assert_cmphex(qtest_readl(s, STATUS), ==, 0x1e000000);
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
        0xc00, 0x100, 0x001, 0x300, 0x224, 0x2000,
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
    g_assert_cmphex(qtest_readl(s, STATUS), ==, 0x1e000000);
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
        "-M o2-test,crime-profile=legacy-software,"
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
    assert_failure(s, DE + 0x9f0, 0, 4);
    c.primitive = 0;
    shape_stage(s, &c);
    assert_failure(s, DE + 0x9f0, 0, 4);
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
    g_assert_cmphex(qtest_readl(v->q, STATUS), ==, 0x1e000000);
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
    qtest_writel(v.q, DE + 0x70, xy(2048, 3));
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
        g_assert_cmphex(qtest_readl(v.q, STATUS), ==, 0x1e000000);
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
    save_renderer(s, path, words);
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
                                   0xe00, 0x80000200 };

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
            c.p[destination] |= 1U << 31;
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
    bool download = GPOINTER_TO_UINT(data);
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
    /* Only the omitted last sample may use the right viewport boundary. */
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
            qtest_writel(s, DE + 0x74, xy(2048, 101));
            break;
        case 13:
            qtest_writel(s, DE + 0x60, 0x01000020);
            qtest_writel(s, DE + 0x74, xy(2048, 100));
            break;
        case 14:
            qtest_writel(s, DE + 0x70, xy(100, 2046));
            qtest_writel(s, DE + 0x74, xy(100, 2048));
            break;
        case 15:
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
                g_assert_cmpint(memtx_write(s, DE + r->offset, 0, width),
                                !=, MEMTX_OK);
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
        g_assert_cmpint(memtx_writel(s, RE + holes[i], 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writel(s, RE + holes[i], UINT32_MAX),
                        !=, MEMTX_OK);
    }
    native_parameters(s, false, false);
    qtest_system_reset(s);
    native_parameters(s, false, true);
    qtest_quit(s);
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
        alpha_failure(s, &c, path, DE + 0x9f0, 0, 4);
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
    if (qtest_has_machine("o2-test")) {
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
    return g_test_run();
}
