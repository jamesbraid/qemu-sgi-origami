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

static QTestState *start_profile(const char *profile, bool banked,
                                 bool functional, const char *extra)
{
    QTestState *s = qtest_initf("-device memory-transaction-test,id=memtx,"
        "migrate-ram=/objects/fit/fit[0] -M none -cpu R10000 -S -d guest_errors,unimp "
        "-object memory-backend-ram,id=fit,size=%s "
        "-device sgi-crime,id=crime,ram=/objects/fit/fit[0],"
        "crime-profile=%s,functional-memory-transfer=%s%s %s",
        banked ? "1G" : "64M", profile, functional ? "on" : "off",
        banked ? ",eight-128m-banks=on,reset-low-ram-approx=on" : "", extra);

    qtest_sysbus_mmio_map(s, "/machine/peripheral/crime", 0, CRIME);
    return s;
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
            qtest_writel(s, DE + 0x10, (1U << (9 - mask)) |
                         (inside ? 1U << (4 - mask) : 0));
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
                qtest_writel(s, DE + 0x10, (1U << (9 - clip)) |
                             (1U << (4 - clip)));
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

            qtest_writel(s, DE + 0x10, (1U << (9 - n)) | (1U << (4 - n)));
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
    qtest_writel(s, MTE + 0x18, 0xb7);
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
                value = 0xb7;
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
        case 6:
            qtest_writel(s, MTE + 8, 0xfffffffe);
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
        start_before_failure(s);
        g_assert_cmpint(memtx_read(s, DE + 0x800 + registers[i].offset,
                                  &value, registers[i].width), !=, MEMTX_OK);
        assert_stopped(s);
        assert_failure(s, DE + 0x800 + registers[i].offset + 1, 0,
                       registers[i].width);
        assert_failure(s, DE + 0x800 + registers[i].offset, 0,
                       registers[i].width == 8 ? 4 : 8);
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
    qtest_writel(from, DE + 0x10, 0x210);
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
        0x2870, 0x2874, 0x28a8, 0x29f0, 0x29f8,
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
            qtest_writel(s, DE + 0x10, (1U << (9 - n)) | (1U << (4 - n)));
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
    unsigned bits = c->bytes * 8;
    uint32_t mask = UINT32_MAX >> (32 - bits);
    uint32_t old, result, plane;
    unsigned index;

    if (x < 0 || y < 0 || x >= 2048 || y >= 2048 ||
        x < c->x || y < c->y || x >= c->x + c->width ||
        y >= c->y + c->height) {
        return;
    }
    if ((c->draw & (1U << 20)) && !inside_bounds(c->scissor, x, y)) {
        return;
    }
    if (c->clip) {
        for (unsigned n = 0; n < 5; n++) {
            if ((c->clip & (1U << (9 - n))) &&
                inside_bounds(c->screen, x, y) !=
                !!(c->clip & (1U << (4 - n)))) {
                return;
            }
        }
    }
    if (c->entries) {
        uint32_t enabled = 0;
        uint32_t effective = c->draw & 0x80 ? c->plane & mask : mask;
        unsigned tile_index = (y / 128) * 16 + (x * c->bytes) / 512;

        for (unsigned b = 0; b < c->bytes; b++) {
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
    result = ((old & ~plane) | (result & plane)) & mask;
    for (unsigned b = 0; b < c->bytes; b++) {
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
        unsigned position = first + rank % (last - first + 1);

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

    setup(s, modes[depth] | (c->bank << 10), false);
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
        qtest_writeq(s, DE + 0x20 + n * 8, c->screen);
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
                c.clip = (1U << (9 - n)) | (inside << (4 - n));
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
        assert_failure(s, DE + 0x800 + offset, 0x1122334455667788ULL, 8);
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

            c.clip = (1U << (9 - n)) | (1U << (4 - n));
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
            prospective.clip = 0x210;
            prospective.screen = bounds(4, 2, 7, 5);
            break;
        case 0x18:
            prospective.draw &= ~(1U << 17);
            break;
        case 0x20: case 0x28: case 0x30: case 0x38: case 0x40: {
            unsigned n = (reg->offset - 0x20) / 8;

            prospective.clip = (1U << (9 - n)) | (1U << (4 - n));
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
                qtest_writel(q, DE + other_window, 1);
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
                c.clip = (1U << (9 - hide)) | (inside << (4 - hide));
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

            c.clip = (1U << (9 - n)) | (1U << (4 - n));
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/crime/render/fill", test_fill);
    qtest_add_func("/sgi/crime/render/registers", test_registers);
    qtest_add_func("/sgi/crime/render/tile-boundaries", test_tile_boundaries);
    qtest_add_func("/sgi/crime/render/rops", test_rops);
    qtest_add_func("/sgi/crime/render/masks", test_masks);
    qtest_add_func("/sgi/crime/render/overlap", test_overlap);
    qtest_add_func("/sgi/crime/render/clipping", test_clipping);
    qtest_add_func("/sgi/crime/render/actual-accesses", test_actual_accesses);
    qtest_add_func("/sgi/crime/render/start-forms", test_start_forms);
    qtest_add_func("/sgi/crime/render/atomic-commands", test_atomic_commands);
    qtest_add_func("/sgi/crime/render/mte-start-forms", test_mte_start_forms);
    qtest_add_func("/sgi/crime/render/mte-scroll", test_mte_scroll);
    qtest_add_func("/sgi/crime/render/mte-rejected", test_mte_rejected);
    qtest_add_func("/sgi/crime/render/malformed-access-stops",
                   test_malformed_access_stops);
    qtest_add_func("/sgi/crime/render/unsupported", test_unsupported);
    qtest_add_func("/sgi/crime/render/migration", test_migration);
    qtest_add_func("/sgi/crime/render/banked-decoder", test_banked_decoder);
    qtest_add_func("/sgi/crime/render/unselected-profile",
                   test_unselected_profile);
    qtest_add_func("/sgi/crime/render/coordinate-limit", test_coordinate_limit);
    if (qtest_has_machine("o2-test")) {
        qtest_add_func("/sgi/crime/render/rom-canary", test_rom_canary);
    }
    qtest_add_func("/sgi/crime/render/wide-slot", test_wide_slot);
    qtest_add_func("/sgi/crime/render/wide-slot-state", test_wide_slot_state);
    qtest_add_func("/sgi/crime/render/wide-pairs", test_wide_pairs);
    qtest_add_func("/sgi/crime/render/wide-rejected", test_wide_rejected);
    qtest_add_func("/sgi/crime/render/native64-start", test_native64_start);
    qtest_add_func("/sgi/crime/render/wide-migration", test_wide_migration);
    qtest_add_func("/sgi/crime/render/point", test_point);
    qtest_add_func("/sgi/crime/render/point-line-stipple/point-geometry",
                   test_point_geometry);
    qtest_add_func("/sgi/crime/render/point-line-stipple/line-geometry",
                   test_line_geometry);
    qtest_add_func("/sgi/crime/render/point-line-stipple/line-width",
                   test_line_width);
    qtest_add_func("/sgi/crime/render/point-line-stipple/stipple-phase",
                   test_stipple_phase);
    qtest_add_func("/sgi/crime/render/point-line-stipple/stipple-pipeline",
                   test_stipple_pipeline);
    qtest_add_func("/sgi/crime/render/point-line-stipple/stipple-clipping",
                   test_stipple_clipping);
    qtest_add_func("/sgi/crime/render/point-line-stipple/stipple-state",
                   test_stipple_state);
    qtest_add_func("/sgi/crime/render/point-line-stipple/stipple-masks-colors",
                   test_stipple_masks_colors);
    qtest_add_func("/sgi/crime/render/point-line-stipple/stipple-invalid",
                   test_stipple_invalid);
    qtest_add_func("/sgi/crime/render/point-line-stipple/stipple-accesses",
                   test_stipple_accesses);
    qtest_add_func("/sgi/crime/render/point-line-stipple/stipple-null",
                   test_stipple_null);
    qtest_add_func("/sgi/crime/render/point-line-stipple/line-shared-endpoint",
                   test_line_shared_endpoint);
    qtest_add_func("/sgi/crime/render/point-line-stipple/stipple-start-forms",
                   test_stipple_start_forms);
    qtest_add_func("/sgi/crime/render/point-line-stipple/stipple-migration",
                   test_stipple_migration);
    qtest_add_func("/sgi/crime/render/point-line-stipple/shape-tile-banks",
                   test_shape_tile_banks);
    qtest_add_func("/sgi/crime/render/point-line-stipple/stipple-inclusive",
                   test_stipple_inclusive);
    qtest_add_func("/sgi/crime/render/point-line-stipple/bank-decoder",
                   test_shape_bank_decoder);
    qtest_add_func("/sgi/crime/render/tiled-validity/fill",
                   test_tiled_validity_fill);
    qtest_add_func("/sgi/crime/render/tiled-validity/banks-depths",
                   test_validity_banks_depths);
    qtest_add_func("/sgi/crime/render/tiled-validity/geometry-phase",
                   test_validity_geometry_phase);
    qtest_add_func("/sgi/crime/render/tiled-validity/clips-masks",
                   test_validity_clips_masks);
    qtest_add_func("/sgi/crime/render/tiled-validity/faults",
                   test_validity_faults);
    qtest_add_func("/sgi/crime/render/tiled-validity/semantics",
                   test_validity_semantics);
    qtest_add_func("/sgi/crime/render/tiled-validity/copies",
                   test_validity_copies);
    qtest_add_func("/sgi/crime/render/tiled-validity/retirement",
                   test_validity_retirement);
    qtest_add_func("/sgi/crime/render/tiled-validity/migration",
                   test_validity_migration);
    qtest_add_func("/sgi/crime/render/tiled-validity/profiles",
                   test_validity_profiles);
    return g_test_run();
}
