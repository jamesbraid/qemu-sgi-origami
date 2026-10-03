/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA qtests for the X server's one-bit drawing: one-bit pixel
 * writes under source packing 4 and 5, as the X server sends them, the
 * X load scissor 0x12d the glyph senders
 * send with them, the stipple tokens 0x13c, 0x13d and 0x13e, whose
 * loaded pattern masks the fills after it, and the spans 0x156, 0x157
 * and 0x158.
 *
 * What a one-bit write paints is inferred there from the GC words each
 * sender fills: under packing 4 a set bit paints the foreground whose
 * index GC word 1 packs as ((v >> 4) & 0xfff) | ((v & 0xf) << 20), with
 * v = index << 8 at depth 8, and a clear bit leaves the pixel alone; under
 * packing 5 a set bit paints GC word 0's index and a clear bit GC word
 * 2's, each v = index << 8. These cases check the model's rule, not the
 * hardware. As for boxes, y counts down from the top display line.
 *
 * The fixture is sn0-origin2000-kona-pixel-test.c's: KONA as a -device at
 * the empty slot IO4, crossbow0's widget 11.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

#define SMALL_WINDOW_SIZE UINT64_C(0x1000000)
#define KONA_WIDGET 11
#define KONA_BASE   (SPACE_IO + (uint64_t)KONA_WIDGET * SMALL_WINDOW_SIZE)
#define KONA_REG(offset) (KONA_BASE + (offset))

#define REG_INT_CAUSE       KONA_REG(0x800238)
#define REG_MAILBOX         KONA_REG(0x800240)
#define REG_COMMAND         KONA_REG(0xc10000)
#define REG_COMMAND_DATA    KONA_REG(0xc1000c)
/* The slot store that opens token t: offset 0xc + 0x20 * t. */
#define REG_COMMAND_SLOT(t) KONA_REG(0xc1000c + 0x20 * (uint64_t)(t))

#define INT_CAUSE_MAILBOX   0x10

#define TOKEN_GC            0x129
#define TOKEN_POSITION      0x12a
#define TOKEN_CLIP          0x12b
#define TOKEN_SCISSOR       0x12d
#define TOKEN_COLOR_MAP     0x12e
#define TOKEN_PIXEL_SETUP   0x136
#define TOKEN_PIXEL_WRITE   0x139
#define TOKEN_PIXEL_FINISH  0x13a
#define TOKEN_LOAD_STIPPLE  0x13c
#define TOKEN_STORE_STIPPLE 0x13d
#define TOKEN_ROTATE_STIPPLE 0x13e
#define TOKEN_RECT          0x152
#define TOKEN_SPANS         0x156
#define TOKEN_SPANS2        0x157
#define TOKEN_SPANS6        0x158
#define TOKEN_BOX           0x154

/* Repeated as literals: a test that borrowed the model's could not fail. */
#define KONA_FB_WIDTH  1280
#define KONA_FB_HEIGHT 1024

#define PLANE_FRONT     0
#define PLANE_FRONT_TAG 2
#define TAG_X_INDEX     2

#define X_MAP_WINDOW    0x2d00

#define KONA_QOM_PATH "/machine/module0/kona0"

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4," \
    "fabric-hub-widget=9"

/* The one-node machine is small enough to migrate quickly as it is. */
#define MIGRATION_ARGS MACHINE_ARGS

/* A box word: two signed 16-bit halves. */
#define XY(x, y) (((uint32_t)(uint16_t)(x) << 16) | (uint16_t)(y))

/* Colour-map entries, 10:10:10 red high, and what they resolve to. */
#define ENTRY_BLACK     0x00000000
#define ENTRY_GREY_192  0x304c1304
#define ENTRY_RED       0x3ff00000
#define ENTRY_GREEN     0x000ffc00
#define ENTRY_BLUE      0x000003ff
#define RGB_BLACK       0x000000
#define RGB_GREY_192    0xc0c0c0
#define RGB_RED         0xff0000
#define RGB_GREEN       0x00ff00
#define RGB_BLUE        0x0000ff

/* The indices these cases paint, each with its own entry. */
#define INDEX_BLACK     0x00
#define INDEX_GREY      0x10
#define INDEX_RED       0x23
#define INDEX_GREEN     0x01
#define INDEX_BLUE      0x02

static QTestState *qts;

static uint64_t kona_qom_get_uint(QTestState *s, const char *property)
{
    QDict *resp = qtest_qmp(s,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': %s}}", KONA_QOM_PATH, property);
    uint64_t value;

    if (!qdict_haskey(resp, "return")) {
        g_autoptr(GString) str = qobject_to_json(QOBJECT(resp));
        g_error("qom-get %s failed: %s", property, str->str);
    }
    value = qdict_get_int(resp, "return");
    qobject_unref(resp);
    return value;
}

static void kona_qom_set_uint(QTestState *s, const char *property,
                              uint32_t value)
{
    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': %s, 'value': %u}}", KONA_QOM_PATH, property, value);
}

static uint32_t kona_plane_word(QTestState *s, uint32_t plane, uint32_t x,
                                uint32_t y)
{
    kona_qom_set_uint(s, "x-test-plane", plane);
    kona_qom_set_uint(s, "x-test-pixel-x", x);
    kona_qom_set_uint(s, "x-test-pixel-y", y);
    return kona_qom_get_uint(s, "x-test-plane-word");
}

/* The front colour word at X coordinates (x, y), y down from the top. */
static uint32_t front_at(QTestState *s, uint32_t x, uint32_t y)
{
    return kona_plane_word(s, PLANE_FRONT, x, KONA_FB_HEIGHT - 1 - y);
}

static uint32_t front_tag_at(QTestState *s, uint32_t x, uint32_t y)
{
    return kona_plane_word(s, PLANE_FRONT_TAG, x, KONA_FB_HEIGHT - 1 - y);
}

/* A machine logging guest errors and unimplemented use to *log. */
static QTestState *kona_init_logged(const char *base_args, char **log)
{
    int fd;

    *log = g_strdup_printf("%s/sn0-kona-glyph-XXXXXX.log", g_get_tmp_dir());
    fd = g_mkstemp(*log);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return qtest_initf("%s -d guest_errors,unimp -D %s", base_args, *log);
}

static char *kona_log_read(const char *log)
{
    char *found = NULL;

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_cmpint(g_unlink(log), ==, 0);
    return found;
}

/* The pipe's console as a PPM, pixels only; the file is removed. */
static uint8_t *kona_screendump(QTestState *s)
{
    static const char header[] = "P6\n1280 1024\n255\n";
    GError *err = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn0-kona-glyph-XXXXXX", &err);
    g_autofree char *path = NULL;
    gchar *data = NULL;
    gsize len = 0;
    uint8_t *pixels;

    g_assert_no_error(err);
    path = g_build_filename(dir, "frame.ppm", NULL);
    qtest_qmp_assert_success(s,
        "{'execute': 'screendump', 'arguments': {'filename': %s, "
        "'device': 'kona0'}}", path);
    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    g_assert_cmpuint(len, ==, strlen(header) +
                     (gsize)KONA_FB_WIDTH * KONA_FB_HEIGHT * 3);
    g_assert_cmpmem(data, strlen(header), header, strlen(header));
    pixels = g_memdup2(data + strlen(header), len - strlen(header));
    g_free(data);
    return pixels;
}

/* 0x00RRGGBB at display column x, display line `line` (0 is the top). */
static uint32_t ppm_pixel(const uint8_t *pixels, unsigned x, unsigned line)
{
    const uint8_t *p = pixels + ((size_t)line * KONA_FB_WIDTH + x) * 3;

    return (p[0] << 16) | (p[1] << 8) | p[2];
}

/* One command as kona.so stores it: first word in the slot, rest data. */
static void x_command(QTestState *s, uint32_t token, const uint32_t *args,
                      unsigned argc)
{
    qtest_writel(s, REG_COMMAND_SLOT(token), args[0]);
    for (unsigned i = 1; i < argc; i++) {
        qtest_writel(s, REG_COMMAND_DATA, args[i]);
    }
}

static void data_words(QTestState *s, const uint32_t *words, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        qtest_writel(s, REG_COMMAND_DATA, words[i]);
    }
}

/*
 * The PROM's sync probe, 1 9 0, as plain words: its mailbox reply shows
 * the command page's framing is back in step.
 */
static void assert_in_step(QTestState *s)
{
    qtest_writel(s, REG_COMMAND, 1);
    qtest_writel(s, REG_COMMAND, 9);
    qtest_writel(s, REG_COMMAND, 0);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX, ==,
                    INT_CAUSE_MAILBOX);
    g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0x1234abcd);
    qtest_writel(s, REG_MAILBOX, 0);
    qtest_writel(s, REG_INT_CAUSE, INT_CAUSE_MAILBOX);
}

/* The colour-map entries these cases resolve through. */
static void load_entries(QTestState *s)
{
    static const struct {
        uint32_t index, entry;
    } entries[] = {
        { INDEX_BLACK, ENTRY_BLACK }, { INDEX_GREY, ENTRY_GREY_192 },
        { INDEX_RED, ENTRY_RED }, { INDEX_GREEN, ENTRY_GREEN },
        { INDEX_BLUE, ENTRY_BLUE },
    };

    for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
        x_command(s, TOKEN_COLOR_MAP,
                  (uint32_t[]){ X_MAP_WINDOW + entries[i].index, 1,
                                entries[i].entry }, 3);
    }
}

/*
 * X load GC as kon_loadGC sends it for a solid fill of colour index
 * `index`: fg_h = index << 12 at depth 8, fg_l 0x1ffe, and the cfg and op
 * the greeter's fills carried.
 */
static void load_fill_gc(QTestState *s, uint32_t index)
{
    x_command(s, TOKEN_GC, (uint32_t[]){ index << 12, 0x1ffe, 0, 0x1ffe,
                                         0xf00fff, 0, 0x180c, 0x40001, 1, 6,
                                         5, 1, 1 }, 13);
}

/*
 * X load GC with words 0 to 3 as given and the rest as the greeter's
 * label GC carried (cfg 0x180c, op 0x40001, ifmt 0).
 */
static void load_bitmap_gc(QTestState *s, uint32_t w0, uint32_t w1,
                           uint32_t w2, uint32_t w3)
{
    x_command(s, TOKEN_GC, (uint32_t[]){ w0, w1, w2, w3, 0xf00fff, 0,
                                         0x180c, 0x40001, 1, 6, 0, 1, 1 },
              13);
}

/* An X box of colour index `index`, [x1, x2) by [y1, y2). */
static void fill_box(QTestState *s, uint32_t index, int x1, int y1, int x2,
                     int y2)
{
    load_fill_gc(s, index);
    x_command(s, TOKEN_BOX, (uint32_t[]){ XY(x1, y1), XY(x2, y2) }, 2);
}

static void full_screen_clip(QTestState *s)
{
    x_command(s, TOKEN_POSITION, (uint32_t[]){ 0, 0, 1280, 1024 }, 4);
    x_command(s, TOKEN_CLIP, (uint32_t[]){ 1, 0, 0, 1280, 1024 }, 5);
}

/* An X rect at (x, y), relative to the position, w by h. */
static void fill_rect(QTestState *s, int x, int y, uint32_t w, uint32_t h)
{
    x_command(s, TOKEN_RECT, (uint32_t[]){ XY(x, y), w << 16 | h }, 2);
}

/*
 * The grammar's stipple, 6 by 2: 0xfc000000 then 0x30000000, stored in
 * entry `entry`.
 */
static void store_grammar_stipple(QTestState *s, uint32_t entry)
{
    x_command(s, TOKEN_STORE_STIPPLE,
              (uint32_t[]){ entry, 6, 2, 0xfc000000, 0x30000000 }, 5);
}

/* Whether the grammar's stipple has its bit set at (col, row). */
static bool grammar_stipple_bit(int64_t col, int64_t row)
{
    static const uint32_t rows[2] = { 0xfc000000, 0x30000000 };

    return rows[((row % 2) + 2) % 2] & (0x80000000u >> (((col % 6) + 6) % 6));
}

/* One-bit pixel setup: packing `pack`, width by height, no pad. */
static void bitmap_setup(QTestState *s, uint32_t pack, uint32_t width,
                         uint32_t height)
{
    x_command(s, TOKEN_PIXEL_SETUP,
              (uint32_t[]){ 0, pack, 1, 0, width, height, 0 }, 7);
}

/* A pixel write at (x, y) with its rows, then its finish. */
static void pixel_write(QTestState *s, int32_t x, int32_t y,
                        const uint32_t *rows, unsigned n)
{
    x_command(s, TOKEN_PIXEL_WRITE, (uint32_t[]){ x, y }, 2);
    data_words(s, rows, n);
    x_command(s, TOKEN_PIXEL_FINISH, (uint32_t[]){ 0 }, 1);
}

/*
 * The "H" of the greeter's "Help" label, 32 by 20 one-bit rows, most
 * significant bit leftmost: 0x80800000 uprights and a 0xff800000 crossbar,
 * as "GC state, position and clip" describes the run's first image. The
 * blank rows above and below are this test's choice.
 */
static const uint32_t glyph_h[20] = {
    0, 0, 0,
    0x80800000, 0x80800000, 0x80800000, 0x80800000, 0x80800000, 0x80800000,
    0xff800000,
    0x80800000, 0x80800000, 0x80800000, 0x80800000, 0x80800000, 0x80800000,
    0, 0, 0, 0,
};

/*
 * Every pixel of a 32 by 20 one-bit image at (x0, y0) on the console: a
 * set bit shows `set`, a clear bit `clear`.
 */
static void assert_glyph(const uint8_t *frame, unsigned x0, unsigned y0,
                         const uint32_t *rows, uint32_t set, uint32_t clear)
{
    for (unsigned r = 0; r < 20; r++) {
        for (unsigned c = 0; c < 32; c++) {
            bool bit = rows[r] & (0x80000000u >> c);

            g_assert_cmphex(ppm_pixel(frame, x0 + c, y0 + r), ==,
                            bit ? set : clear);
        }
    }
}

/*
 * Packing 4, the glyph senders' form: the "H" written at (260, 304), the
 * label's first position in the run, over a grey box, with GC word 1
 * 0x230, which packs index 0x23. Each set bit paints index 0x23, tagged
 * as an X pixel, and each clear bit leaves the grey box's pixel as it
 * was, so the whole 32 by 20 cell reads back as the H in red on grey.
 */
static void test_packing4_help_glyph(void)
{
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    load_entries(qts);
    full_screen_clip(qts);
    fill_box(qts, INDEX_GREY, 250, 300, 300, 330);
    load_bitmap_gc(qts, 0xfff0, 0x230, 0, 0);
    bitmap_setup(qts, 4, 32, 20);
    pixel_write(qts, 260, 304, glyph_h, 20);

    g_assert_cmphex(front_at(qts, 260, 307), ==, INDEX_RED);
    g_assert_cmphex(front_tag_at(qts, 260, 307), ==, TAG_X_INDEX);
    g_assert_cmphex(front_at(qts, 268, 313), ==, INDEX_RED);
    g_assert_cmphex(front_at(qts, 261, 307), ==, INDEX_GREY);
    frame = kona_screendump(qts);
    assert_glyph(frame, 260, 304, glyph_h, RGB_RED, RGB_GREY_192);
    g_assert_cmphex(ppm_pixel(frame, 259, 307), ==, RGB_GREY_192);
    assert_in_step(qts);
}

/*
 * The label GCs of the acceptance run carry 0xfff0, 0: foreground index
 * 0. Through entry 0x2d00, zero in the default colour map, the "H" comes
 * out black on the grey button.
 */
static void test_packing4_run_label_is_black(void)
{
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    load_entries(qts);
    full_screen_clip(qts);
    fill_box(qts, INDEX_GREY, 240, 295, 312, 327);
    x_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 240, 295, 312, 327 }, 5);
    load_bitmap_gc(qts, 0xfff0, 0, 0, 0);
    bitmap_setup(qts, 4, 32, 20);
    pixel_write(qts, 260, 304, glyph_h, 20);

    g_assert_cmphex(front_tag_at(qts, 260, 307), ==, TAG_X_INDEX);
    frame = kona_screendump(qts);
    assert_glyph(frame, 260, 304, glyph_h, RGB_BLACK, RGB_GREY_192);
    assert_in_step(qts);
}

/*
 * The grammar's one-bit glyph colour row: at depth 8, words 0 to 3
 * 0xfff0, 0x230, 0, 0, then 0x136 0 with 4, 1, 0, 32, 1, 0 and 0x139 at
 * (10, 10) with 0x80000001 paints (10, 10) and (41, 10) with entry 0x2d23
 * and leaves (11, 10) unchanged. At depth 8 the index is bits 11:4 of
 * word 1, so a second GC with word 1 0x203 paints index 0x20, where a
 * model reading word 1's low byte would paint 3.
 */
static void test_packing4_grammar_row(void)
{
    qtest_system_reset(qts);
    fill_box(qts, INDEX_GREY, 0, 0, 64, 32);
    full_screen_clip(qts);
    load_bitmap_gc(qts, 0xfff0, 0x230, 0, 0);
    x_command(qts, TOKEN_PIXEL_SETUP, (uint32_t[]){ 0, 4, 1, 0, 32, 1, 0 },
              7);
    pixel_write(qts, 10, 10, (uint32_t[]){ 0x80000001 }, 1);
    g_assert_cmphex(front_at(qts, 10, 10), ==, 0x23);
    g_assert_cmphex(front_at(qts, 41, 10), ==, 0x23);
    g_assert_cmphex(front_at(qts, 11, 10), ==, INDEX_GREY);
    g_assert_cmphex(front_at(qts, 42, 10), ==, INDEX_GREY);

    load_bitmap_gc(qts, 0xfff0, 0x203, 0, 0);
    pixel_write(qts, 10, 11, (uint32_t[]){ 0x80000000 }, 1);
    g_assert_cmphex(front_at(qts, 10, 11), ==, 0x20);
    assert_in_step(qts);
}

/*
 * A box filled under a one-bit-form GC, fg_h 0xfff0 with word 1 0x230,
 * takes the foreground word 1 carries, index 0x23, not 0xfff0 read as a
 * solid fg_h, which would give index 0xf.
 */
static void test_bitmap_gc_fill_index(void)
{
    qtest_system_reset(qts);
    load_bitmap_gc(qts, 0xfff0, 0x230, 0, 0);
    x_command(qts, TOKEN_BOX, (uint32_t[]){ XY(5, 5), XY(7, 7) }, 2);
    g_assert_cmphex(front_at(qts, 5, 5), ==, 0x23);
    g_assert_cmphex(front_at(qts, 6, 6), ==, 0x23);
    assert_in_step(qts);
}

/*
 * The grammar's opaque mono image row: at depth 8, words 0 to 3 0x100, 0,
 * 0x200, 0, then 0x136 0 with 5, 1, 0, 32, 1, 0 and 0x139 with 0x80000000
 * paints (x, y) with entry 0x2d01 and (x + 1, y) with entry 0x2d02. The
 * whole 32-pixel row is painted: 31 pixels in the background index.
 */
static void test_packing5_paints_both(void)
{
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    load_entries(qts);
    fill_box(qts, INDEX_GREY, 0, 0, 64, 32);
    full_screen_clip(qts);
    load_bitmap_gc(qts, 0x100, 0, 0x200, 0);
    x_command(qts, TOKEN_PIXEL_SETUP, (uint32_t[]){ 0, 5, 1, 0, 32, 1, 0 },
              7);
    pixel_write(qts, 20, 5, (uint32_t[]){ 0x80000000 }, 1);

    g_assert_cmphex(front_at(qts, 20, 5), ==, INDEX_GREEN);
    g_assert_cmphex(front_tag_at(qts, 20, 5), ==, TAG_X_INDEX);
    g_assert_cmphex(front_at(qts, 21, 5), ==, INDEX_BLUE);
    frame = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(frame, 20, 5), ==, RGB_GREEN);
    for (unsigned c = 1; c < 32; c++) {
        g_assert_cmphex(ppm_pixel(frame, 20 + c, 5), ==, RGB_BLUE);
    }
    g_assert_cmphex(ppm_pixel(frame, 52, 5), ==, RGB_GREY_192);
    g_assert_cmphex(ppm_pixel(frame, 20, 6), ==, RGB_GREY_192);
    assert_in_step(qts);
}

/*
 * A one-bit write is clipped like any other: a packing-5 row under a clip
 * of (100, 0)-(104, 1024) lands in four columns only, and a packing-4 one
 * starting left of the screen keeps its on-screen bits.
 */
static void test_bitmap_clipped(void)
{
    qtest_system_reset(qts);
    fill_box(qts, INDEX_GREY, 0, 0, 200, 32);
    x_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 100, 0, 104, 1024 }, 5);
    load_bitmap_gc(qts, 0x100, 0, 0x200, 0);
    bitmap_setup(qts, 5, 32, 1);
    pixel_write(qts, 98, 3, (uint32_t[]){ 0xa0000000 }, 1);
    g_assert_cmphex(front_at(qts, 98, 3), ==, INDEX_GREY);
    g_assert_cmphex(front_at(qts, 99, 3), ==, INDEX_GREY);
    g_assert_cmphex(front_at(qts, 100, 3), ==, INDEX_GREEN);
    g_assert_cmphex(front_at(qts, 101, 3), ==, INDEX_BLUE);
    g_assert_cmphex(front_at(qts, 103, 3), ==, INDEX_BLUE);
    g_assert_cmphex(front_at(qts, 104, 3), ==, INDEX_GREY);

    full_screen_clip(qts);
    load_bitmap_gc(qts, 0xfff0, 0x230, 0, 0);
    bitmap_setup(qts, 4, 32, 1);
    pixel_write(qts, -30, 4, (uint32_t[]){ 0x00000005 }, 1);
    g_assert_cmphex(front_at(qts, 0, 4), ==, INDEX_GREY);
    g_assert_cmphex(front_at(qts, 1, 4), ==, 0x23);
    g_assert_cmphex(front_at(qts, 2, 4), ==, INDEX_GREY);
    assert_in_step(qts);
}

/*
 * The label's scissor from the run, 0x12d 240, 295, 72, 32, frames as one
 * four-word command both as kona.so stores it and as plain words: its
 * second word, 295, is 0x127, which must not open a command of its own.
 */
static void test_scissor_frames(void)
{
    qtest_system_reset(qts);
    x_command(qts, TOKEN_SCISSOR, (uint32_t[]){ 240, 295, 72, 32 }, 4);
    assert_in_step(qts);
    qtest_writel(qts, REG_COMMAND, TOKEN_SCISSOR);
    data_words(qts, (uint32_t[]){ 240, 0x127, 72, 32 }, 4);
    assert_in_step(qts);

    /* The scissor it left clips a box to (240, 295)-(312, 327). */
    fill_box(qts, INDEX_RED, 230, 290, 320, 330);
    g_assert_cmphex(front_tag_at(qts, 239, 300), ==, 0);
    g_assert_cmphex(front_tag_at(qts, 240, 294), ==, 0);
    g_assert_cmphex(front_at(qts, 240, 295), ==, INDEX_RED);
    g_assert_cmphex(front_at(qts, 311, 326), ==, INDEX_RED);
    g_assert_cmphex(front_tag_at(qts, 312, 326), ==, 0);
    g_assert_cmphex(front_tag_at(qts, 311, 327), ==, 0);
    assert_in_step(qts);
}

/*
 * A scissor narrower than the clip clips one-bit writes and fills to
 * their intersection: under a full-screen clip and a scissor of 250, 300,
 * 10 by 5, a packing-5 row from x = 245 lands in columns 250 to 259 only,
 * a row at y = 305 not at all, and an X box over the area only inside the
 * scissor.
 */
static void test_scissor_clips_writes_and_fills(void)
{
    qtest_system_reset(qts);
    fill_box(qts, INDEX_GREY, 240, 290, 280, 320);
    full_screen_clip(qts);
    x_command(qts, TOKEN_SCISSOR, (uint32_t[]){ 250, 300, 10, 5 }, 4);
    load_bitmap_gc(qts, 0x100, 0, 0x200, 0);
    bitmap_setup(qts, 5, 32, 1);
    pixel_write(qts, 245, 302, (uint32_t[]){ 0xffffffff }, 1);
    pixel_write(qts, 245, 305, (uint32_t[]){ 0xffffffff }, 1);
    g_assert_cmphex(front_at(qts, 249, 302), ==, INDEX_GREY);
    g_assert_cmphex(front_at(qts, 250, 302), ==, INDEX_GREEN);
    g_assert_cmphex(front_at(qts, 259, 302), ==, INDEX_GREEN);
    g_assert_cmphex(front_at(qts, 260, 302), ==, INDEX_GREY);
    g_assert_cmphex(front_at(qts, 250, 305), ==, INDEX_GREY);

    load_fill_gc(qts, INDEX_RED);
    x_command(qts, TOKEN_BOX, (uint32_t[]){ XY(240, 290), XY(280, 320) }, 2);
    g_assert_cmphex(front_at(qts, 250, 300), ==, INDEX_RED);
    g_assert_cmphex(front_at(qts, 259, 304), ==, INDEX_RED);
    g_assert_cmphex(front_at(qts, 249, 300), ==, INDEX_GREY);
    g_assert_cmphex(front_at(qts, 260, 304), ==, INDEX_GREY);
    g_assert_cmphex(front_at(qts, 250, 299), ==, INDEX_GREY);
    g_assert_cmphex(front_at(qts, 250, 305), ==, INDEX_GREY);
    assert_in_step(qts);
}

/*
 * The model's lifetime rule: the next X load clip ends the scissor, so a
 * box after a clip with no new scissor fills the clip's area, and a
 * scissor sent after the clip, in the glyph senders' order, applies.
 */
static void test_scissor_ends_at_clip(void)
{
    qtest_system_reset(qts);
    x_command(qts, TOKEN_SCISSOR, (uint32_t[]){ 10, 10, 2, 2 }, 4);
    full_screen_clip(qts);
    fill_box(qts, INDEX_RED, 0, 0, 20, 20);
    g_assert_cmphex(front_at(qts, 0, 0), ==, INDEX_RED);
    g_assert_cmphex(front_at(qts, 19, 19), ==, INDEX_RED);

    x_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 30, 30, 40, 40 }, 5);
    x_command(qts, TOKEN_SCISSOR, (uint32_t[]){ 30, 30, 5, 5 }, 4);
    fill_box(qts, INDEX_GREY, 30, 30, 40, 40);
    g_assert_cmphex(front_at(qts, 34, 34), ==, INDEX_GREY);
    g_assert_cmphex(front_tag_at(qts, 35, 34), ==, 0);
    g_assert_cmphex(front_tag_at(qts, 34, 35), ==, 0);
    assert_in_step(qts);
}

/*
 * A scissor of zero width clips everything and is logged; so does one of
 * zero height.
 */
static void test_scissor_zero_clips_everything(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, &log);

    x_command(s, TOKEN_SCISSOR, (uint32_t[]){ 0, 0, 0, 1024 }, 4);
    fill_box(s, INDEX_RED, 0, 0, 1280, 1024);
    g_assert_cmphex(front_tag_at(s, 0, 0), ==, 0);
    g_assert_cmphex(front_tag_at(s, 640, 512), ==, 0);
    x_command(s, TOKEN_SCISSOR, (uint32_t[]){ 0, 0, 1280, 0 }, 4);
    fill_box(s, INDEX_RED, 0, 0, 1280, 1024);
    g_assert_cmphex(front_tag_at(s, 640, 512), ==, 0);
    x_command(s, TOKEN_SCISSOR, (uint32_t[]){ 0, 0, 1280, 1024 }, 4);
    fill_box(s, INDEX_RED, 0, 0, 1280, 1024);
    g_assert_cmphex(front_at(s, 640, 512), ==, INDEX_RED);
    assert_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "X scissor of 0 by 1024; clipping "
                                   "everything"));
    g_assert_nonnull(strstr(found, "X scissor of 1280 by 0; clipping "
                                   "everything"));
}

/*
 * A migration carries the scissor: set on the source, it clips a box on
 * the destination.
 */
static void test_migration_carries_scissor(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-glyph-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    x_command(from, TOKEN_SCISSOR, (uint32_t[]){ 10, 10, 2, 2 }, 4);
    sgi_migrate_stopped(from, uri);

    fill_box(to, INDEX_RED, 0, 0, 20, 20);
    g_assert_cmphex(front_at(to, 10, 10), ==, INDEX_RED);
    g_assert_cmphex(front_at(to, 11, 11), ==, INDEX_RED);
    g_assert_cmphex(front_tag_at(to, 9, 10), ==, 0);
    g_assert_cmphex(front_tag_at(to, 12, 11), ==, 0);
    assert_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * A stream from the model before the one-bit drawing state (x-server
 * subsection version 2) still loads: its clip list and GC cross, and the
 * destination starts with no scissor. Skipped unless
 * QTEST_QEMU_BINARY_KONA_X_SERVER_V2 names a qemu-system-mips64 built with
 * that subsection at version 2, such as one built at 61b7e6c4e2.
 */
static void test_migration_from_x_server_v2(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    if (!g_getenv("QTEST_QEMU_BINARY_KONA_X_SERVER_V2")) {
        g_test_skip("QTEST_QEMU_BINARY_KONA_X_SERVER_V2 is not set");
        return;
    }

    tmpfs = g_dir_make_tmp("sn0-kona-x-server-v2-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init_ext("QTEST_QEMU_BINARY_KONA_X_SERVER_V2",
                          MIGRATION_ARGS, NULL, true);
    x_command(from, TOKEN_CLIP, (uint32_t[]){ 1, 10, 10, 20, 20 }, 5);
    load_bitmap_gc(from, 0xfff0, 0x230, 0, 0);

    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    sgi_migrate_stopped(from, uri);

    bitmap_setup(to, 4, 32, 1);
    pixel_write(to, 0, 15, (uint32_t[]){ 0xffffffff }, 1);
    g_assert_cmphex(front_tag_at(to, 9, 15), ==, 0);
    g_assert_cmphex(front_at(to, 10, 15), ==, 0x23);
    g_assert_cmphex(front_at(to, 19, 15), ==, 0x23);
    g_assert_cmphex(front_tag_at(to, 20, 15), ==, 0);
    assert_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * The grammar's stipple row: 0x13d 5, 6, 2, 0xfc000000, 0x30000000;
 * 0x13e 5, 0, 0; 0x13c 5; a full-screen clip; 0x152 (0, 0) by 12 by 4
 * fills columns 0 to 5 and 6 to 11 of rows 0 and 2, and only columns 2,
 * 3, 8 and 9 of rows 1 and 3, leaving the rest as it was; after 0x13c
 * 0xffffffff the same rect fills solid.
 */
static void test_stipple_grammar_row(void)
{
    qtest_system_reset(qts);
    fill_box(qts, INDEX_GREY, 0, 0, 16, 8);
    store_grammar_stipple(qts, 5);
    x_command(qts, TOKEN_ROTATE_STIPPLE, (uint32_t[]){ 5, 0, 0 }, 3);
    x_command(qts, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 5 }, 1);
    full_screen_clip(qts);
    load_fill_gc(qts, INDEX_RED);
    fill_rect(qts, 0, 0, 12, 4);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 12; x++) {
            bool set = y % 2 == 0 || x % 6 == 2 || x % 6 == 3;

            g_assert_cmphex(front_at(qts, x, y), ==,
                            set ? INDEX_RED : INDEX_GREY);
        }
    }
    g_assert_cmphex(front_at(qts, 12, 0), ==, INDEX_GREY);

    x_command(qts, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 0xffffffff }, 1);
    load_fill_gc(qts, INDEX_GREEN);
    fill_rect(qts, 0, 0, 12, 4);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 12; x++) {
            g_assert_cmphex(front_at(qts, x, y), ==, INDEX_GREEN);
        }
    }
    assert_in_step(qts);
}

/*
 * The model reads the rotate origin as drawable-relative, like a rect's:
 * the pattern starts at the X load position plus the origin. That reading
 * is inferred, and rests only on the run's I-beam lining up with its rect;
 * the origin words come from a private record nobody traced. Under a
 * position of 100, 50 and an origin of 1, 0, a 12 by 4 rect at (0, 0)
 * paints (x, y) where the bit at ((x - 101) mod 6, (y - 50) mod 2) is set:
 * rows 51 and 53 in columns 103, 104, 109 and 110 only. An absolute origin
 * would paint columns 100, 105, 106 and 111 there instead. (The grammar's
 * 105, 106, 111 and 112 are for its rect at (1, 0), not this one's.)
 */
static void test_stipple_origin_follows_position(void)
{
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    load_entries(qts);
    fill_box(qts, INDEX_GREY, 90, 40, 130, 70);
    store_grammar_stipple(qts, 7);
    x_command(qts, TOKEN_ROTATE_STIPPLE, (uint32_t[]){ 7, 1, 0 }, 3);
    x_command(qts, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 7 }, 1);
    x_command(qts, TOKEN_POSITION, (uint32_t[]){ 100, 50, 40, 40 }, 4);
    x_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 0, 0, 1280, 1024 }, 5);
    load_fill_gc(qts, INDEX_RED);
    fill_rect(qts, 0, 0, 12, 4);

    frame = kona_screendump(qts);
    for (unsigned y = 48; y < 56; y++) {
        for (unsigned x = 98; x < 114; x++) {
            bool inside = x >= 100 && x < 112 && y >= 50 && y < 54;
            bool set = inside &&
                       grammar_stipple_bit((int64_t)x - 101, (int64_t)y - 50);

            g_assert_cmphex(ppm_pixel(frame, x, y), ==,
                            set ? RGB_RED : RGB_GREY_192);
        }
    }
    for (unsigned x = 100; x < 112; x++) {
        bool painted = x == 103 || x == 104 || x == 109 || x == 110;

        g_assert_cmphex(ppm_pixel(frame, x, 51), ==,
                        painted ? RGB_RED : RGB_GREY_192);
    }
    assert_in_step(qts);
}

/*
 * The stipple masks X boxes too, and bits past the pattern's width are
 * ignored: a 3-wide pattern whose rows carry stray bits past the third
 * repeats every three columns. Its rows are sent as plain words shaped
 * like the sync probe, 1, 9, 0 read as rows, to show a store's rows are
 * consumed as data.
 */
static void test_stipple_masks_boxes_and_ignores_stray_bits(void)
{
    qtest_system_reset(qts);
    qtest_writel(qts, REG_COMMAND, TOKEN_STORE_STIPPLE);
    data_words(qts, (uint32_t[]){ 2, 3, 1, 0xbfffffff }, 4);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);
    x_command(qts, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 2 }, 1);
    fill_box(qts, INDEX_RED, 0, 0, 9, 1);
    for (unsigned x = 0; x < 9; x++) {
        g_assert_cmphex(front_tag_at(qts, x, 0), ==,
                        x % 3 == 1 ? 0 : TAG_X_INDEX);
    }

    x_command(qts, TOKEN_STORE_STIPPLE, (uint32_t[]){ 3, 32, 3 }, 3);
    data_words(qts, (uint32_t[]){ 1, 9, 0 }, 3);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);
    assert_in_step(qts);
}

/*
 * A store that does not fit the model's cache, an entry past it or a
 * pattern over 32 bits wide, is logged and its rows consumed; loading an
 * entry past the cache is logged and unloads the stipple, and loading one
 * that holds no pattern is logged and masks nothing. A token arriving
 * before a store's last row is logged and the store dropped.
 */
static void test_stipple_outside_cache(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, &log);

    x_command(s, TOKEN_STORE_STIPPLE, (uint32_t[]){ 40, 6, 2, 1, 9 }, 5);
    assert_in_step(s);
    x_command(s, TOKEN_STORE_STIPPLE, (uint32_t[]){ 4, 33, 1, 0 }, 4);
    assert_in_step(s);
    x_command(s, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 4 }, 1);
    fill_box(s, INDEX_RED, 0, 0, 4, 1);
    g_assert_cmphex(front_at(s, 1, 0), ==, INDEX_RED);

    store_grammar_stipple(s, 5);
    x_command(s, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 5 }, 1);
    x_command(s, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 40 }, 1);
    fill_box(s, INDEX_GREEN, 0, 1, 4, 3);
    g_assert_cmphex(front_at(s, 0, 1), ==, INDEX_GREEN);

    x_command(s, TOKEN_STORE_STIPPLE, (uint32_t[]){ 6, 4, 4, 0 }, 4);
    x_command(s, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 6 }, 1);
    fill_box(s, INDEX_BLUE, 0, 3, 4, 4);
    g_assert_cmphex(front_at(s, 0, 3), ==, INDEX_BLUE);
    assert_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "X stipple entry 0x28 of 6 by 2 does not"
                                   " fit"));
    g_assert_nonnull(strstr(found, "X stipple entry 0x4 of 33 by 1 does not"
                                   " fit"));
    g_assert_nonnull(strstr(found, "X stipple load of entry 0x4, which holds"
                                   " no pattern"));
    g_assert_nonnull(strstr(found, "X stipple load of entry 0x28 outside"));
    g_assert_nonnull(strstr(found, "while token 0x13d still needs 3 rows"));
}

/*
 * Reset clears the scissor, the stipple cache and the loaded entry: after
 * it, a box fills solid, loading the entry stored before it masks
 * nothing, and a pattern stored afresh masks again.
 */
static void test_reset_clears_scissor_and_stipples(void)
{
    qtest_system_reset(qts);
    store_grammar_stipple(qts, 5);
    x_command(qts, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 5 }, 1);
    x_command(qts, TOKEN_SCISSOR, (uint32_t[]){ 0, 0, 1, 1 }, 4);
    qtest_system_reset(qts);

    fill_box(qts, INDEX_RED, 0, 0, 12, 2);
    for (unsigned x = 0; x < 12; x++) {
        g_assert_cmphex(front_at(qts, x, 1), ==, INDEX_RED);
    }
    x_command(qts, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 5 }, 1);
    fill_box(qts, INDEX_GREEN, 0, 2, 12, 4);
    g_assert_cmphex(front_at(qts, 0, 3), ==, INDEX_GREEN);
    g_assert_cmphex(front_at(qts, 11, 3), ==, INDEX_GREEN);

    store_grammar_stipple(qts, 5);
    fill_box(qts, INDEX_BLUE, 0, 4, 12, 6);
    g_assert_cmphex(front_tag_at(qts, 0, 5), ==, 0);
    g_assert_cmphex(front_at(qts, 2, 5), ==, INDEX_BLUE);
    assert_in_step(qts);
}

/*
 * A stipple store caught mid-pattern by a migration completes on the
 * destination: its first row crosses with the store's entry, size and the
 * rows still to come, and a rotate and load sent before the migration to
 * another entry cross too.
 */
static void test_migration_mid_stipple(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-glyph-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    x_command(from, TOKEN_ROTATE_STIPPLE, (uint32_t[]){ 9, 3, 0 }, 3);
    x_command(from, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 9 }, 1);
    x_command(from, TOKEN_STORE_STIPPLE,
              (uint32_t[]){ 9, 6, 2, 0xfc000000 }, 4);
    sgi_migrate_stopped(from, uri);

    data_words(to, (uint32_t[]){ 0x30000000 }, 1);
    fill_box(to, INDEX_RED, 0, 0, 12, 2);
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned x = 0; x < 12; x++) {
            g_assert_cmphex(front_tag_at(to, x, y), ==,
                            grammar_stipple_bit((int64_t)x - 3, y) ?
                            TAG_X_INDEX : 0);
        }
    }
    assert_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/* Whether exactly [x1, x2) of line y holds index `index`, x0 to x3 read. */
static void assert_span(QTestState *s, int x0, int x1, int x2, int x3, int y,
                        uint32_t index)
{
    for (int x = x0; x < x3; x++) {
        if (x >= x1 && x < x2) {
            g_assert_cmphex(front_at(s, x, y), ==, index);
            g_assert_cmphex(front_tag_at(s, x, y), ==, TAG_X_INDEX);
        } else {
            g_assert_cmphex(front_at(s, x, y), !=, index);
        }
    }
}

/*
 * The grammar's spans row: 0x157 2, 0x00050005, 0x00050006, 3, 1 fills
 * (5, 5)-(8, 6) and (5, 6)-(6, 7); 0x158 and 0x156 frame as 13 and 3
 * words.
 */
static void test_spans_grammar_row(void)
{
    qtest_system_reset(qts);
    load_fill_gc(qts, INDEX_RED);
    x_command(qts, TOKEN_SPANS2,
              (uint32_t[]){ 2, 0x00050005, 0x00050006, 3, 1 }, 5);
    assert_span(qts, 3, 5, 8, 10, 5, INDEX_RED);
    assert_span(qts, 3, 5, 6, 10, 6, INDEX_RED);
    assert_span(qts, 3, 0, 0, 10, 4, INDEX_RED);
    assert_span(qts, 3, 0, 0, 10, 7, INDEX_RED);

    x_command(qts, TOKEN_SPANS6,
              (uint32_t[]){ 6, XY(20, 20), XY(20, 21), XY(20, 22), XY(20, 23),
                            XY(20, 24), XY(20, 25), 1, 2, 3, 4, 5, 6 }, 13);
    for (int i = 0; i < 6; i++) {
        assert_span(qts, 18, 20, 21 + i, 30, 20 + i, INDEX_RED);
    }
    x_command(qts, TOKEN_SPANS, (uint32_t[]){ 1, XY(40, 40), 7 }, 3);
    assert_span(qts, 38, 40, 47, 50, 40, INDEX_RED);
    assert_in_step(qts);
}

/*
 * A triangle in the place of the run's on-screen arrow, sent the way
 * konFillSpansSolid sends spans: six, then two, then one, widths 9 down
 * to 1 from (305, 143), one line each, each span one column in from the
 * last. Sent as plain words, every
 * count word, 1 for the last token among them, must be taken as the
 * token's count and not as the textport's sync word opening a packet.
 */
static void test_spans_arrow_plain_words(void)
{
    static const uint32_t spans6[] = {
        TOKEN_SPANS6, 6, XY(305, 143), XY(306, 144), XY(307, 145),
        XY(308, 146), XY(309, 147), XY(310, 148), 9, 8, 7, 6, 5, 4,
    };
    static const uint32_t spans2[] = {
        TOKEN_SPANS2, 2, XY(311, 149), XY(312, 150), 3, 2,
    };
    static const uint32_t spans1[] = { TOKEN_SPANS, 1, XY(313, 151), 1 };

    qtest_system_reset(qts);
    full_screen_clip(qts);
    load_fill_gc(qts, INDEX_RED);
    for (size_t i = 0; i < ARRAY_SIZE(spans6); i++) {
        qtest_writel(qts, REG_COMMAND, spans6[i]);
    }
    for (size_t i = 0; i < ARRAY_SIZE(spans2); i++) {
        qtest_writel(qts, REG_COMMAND, spans2[i]);
    }
    for (size_t i = 0; i < ARRAY_SIZE(spans1); i++) {
        qtest_writel(qts, REG_COMMAND, spans1[i]);
    }
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);
    for (int i = 0; i < 9; i++) {
        assert_span(qts, 300, 305 + i, 314, 316, 143 + i, INDEX_RED);
    }
    assert_in_step(qts);
}

/*
 * A span with count 1 in the address form, as kona.so stores it, takes
 * its count in the slot; the plain words after it are its point and width
 * and open no textport packet. A count that does not match the token is
 * dropped: a 0x157 whose count is 1 paints nothing and is logged.
 */
static void test_spans_count(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, &log);

    load_fill_gc(s, INDEX_RED);
    x_command(s, TOKEN_SPANS, (uint32_t[]){ 1, XY(1, 9), 3 }, 3);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE), ==, 0);
    assert_span(s, 0, 1, 4, 6, 9, INDEX_RED);
    x_command(s, TOKEN_SPANS2, (uint32_t[]){ 1, XY(1, 10), XY(1, 11), 3, 3 },
              5);
    assert_span(s, 0, 0, 0, 6, 10, INDEX_RED);
    assert_span(s, 0, 0, 0, 6, 11, INDEX_RED);
    assert_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "X spans token 0x157 with count 1, not 2;"
                                   " dropped"));
}

/*
 * Spans take the same clip, scissor and stipple as boxes: under a clip of
 * (0, 0)-(10, 1024), a scissor of 2, 0, 20 by 1024 and the grammar's
 * stipple, a span from 0 of width 20 on line 1 paints columns 2, 3, 8 and
 * 9 only, and one on line 0 columns 2 to 9. A span starting left of the
 * screen keeps its on-screen part.
 */
static void test_spans_clipped_and_stippled(void)
{
    qtest_system_reset(qts);
    x_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 0, 0, 10, 1024 }, 5);
    x_command(qts, TOKEN_SCISSOR, (uint32_t[]){ 2, 0, 20, 1024 }, 4);
    store_grammar_stipple(qts, 1);
    x_command(qts, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 1 }, 1);
    load_fill_gc(qts, INDEX_RED);
    x_command(qts, TOKEN_SPANS2, (uint32_t[]){ 2, XY(0, 0), XY(0, 1), 20, 20 },
              5);
    for (int x = 0; x < 12; x++) {
        bool row0 = x >= 2 && x < 10;
        bool row1 = x == 2 || x == 3 || x == 8 || x == 9;

        g_assert_cmphex(front_tag_at(qts, x, 0), ==, row0 ? TAG_X_INDEX : 0);
        g_assert_cmphex(front_tag_at(qts, x, 1), ==, row1 ? TAG_X_INDEX : 0);
    }

    x_command(qts, TOKEN_LOAD_STIPPLE, (uint32_t[]){ 0xffffffff }, 1);
    full_screen_clip(qts);
    x_command(qts, TOKEN_SPANS, (uint32_t[]){ 1, XY(-3, 5), 5 }, 3);
    assert_span(qts, 0, 0, 2, 4, 5, INDEX_RED);
    assert_in_step(qts);
}

/*
 * Spans take their points as absolute: under an X load position of 40,
 * 30, the spans case still fills (5, 5)-(8, 6) and (5, 6)-(6, 7),
 * and nothing at (45, 35), where the position added would put them. An
 * X rect at (0, 10) under the same position lands at (40, 40), so the
 * position is in force. No traced sequence tells the two readings apart:
 * every traced span follows a position of 0, 0. The model follows the
 * absolute reading; a spans sender that leaves a non-zero
 * position in place would settle it.
 */
static void test_spans_ignore_position(void)
{
    qtest_system_reset(qts);
    full_screen_clip(qts);
    x_command(qts, TOKEN_POSITION, (uint32_t[]){ 40, 30, 200, 200 }, 4);
    load_fill_gc(qts, INDEX_RED);
    x_command(qts, TOKEN_SPANS2,
              (uint32_t[]){ 2, 0x00050005, 0x00050006, 3, 1 }, 5);
    assert_span(qts, 3, 5, 8, 50, 5, INDEX_RED);
    assert_span(qts, 3, 5, 6, 50, 6, INDEX_RED);
    assert_span(qts, 3, 0, 0, 50, 35, INDEX_RED);
    assert_span(qts, 3, 0, 0, 50, 36, INDEX_RED);

    fill_rect(qts, 0, 10, 3, 1);
    assert_span(qts, 38, 40, 43, 46, 40, INDEX_RED);
    assert_in_step(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

    qtest_add_func("sn0/origin2000/kona-glyph/packing4-help-glyph",
                   test_packing4_help_glyph);
    qtest_add_func("sn0/origin2000/kona-glyph/packing4-run-label-is-black",
                   test_packing4_run_label_is_black);
    qtest_add_func("sn0/origin2000/kona-glyph/packing4-grammar-row",
                   test_packing4_grammar_row);
    qtest_add_func("sn0/origin2000/kona-glyph/bitmap-gc-fill-index",
                   test_bitmap_gc_fill_index);
    qtest_add_func("sn0/origin2000/kona-glyph/packing5-paints-both",
                   test_packing5_paints_both);
    qtest_add_func("sn0/origin2000/kona-glyph/bitmap-clipped",
                   test_bitmap_clipped);
    qtest_add_func("sn0/origin2000/kona-glyph/scissor-frames",
                   test_scissor_frames);
    qtest_add_func("sn0/origin2000/kona-glyph/scissor-clips-writes-and-fills",
                   test_scissor_clips_writes_and_fills);
    qtest_add_func("sn0/origin2000/kona-glyph/scissor-ends-at-clip",
                   test_scissor_ends_at_clip);
    qtest_add_func("sn0/origin2000/kona-glyph/scissor-zero-clips-everything",
                   test_scissor_zero_clips_everything);
    qtest_add_func("sn0/origin2000/kona-glyph/migration-carries-scissor",
                   test_migration_carries_scissor);
    qtest_add_func("sn0/origin2000/kona-glyph/migration-from-x-server-v2",
                   test_migration_from_x_server_v2);
    qtest_add_func("sn0/origin2000/kona-glyph/stipple-grammar-row",
                   test_stipple_grammar_row);
    qtest_add_func("sn0/origin2000/kona-glyph/stipple-origin-follows-position",
                   test_stipple_origin_follows_position);
    qtest_add_func("sn0/origin2000/kona-glyph/"
                   "stipple-masks-boxes-and-ignores-stray-bits",
                   test_stipple_masks_boxes_and_ignores_stray_bits);
    qtest_add_func("sn0/origin2000/kona-glyph/stipple-outside-cache",
                   test_stipple_outside_cache);
    qtest_add_func("sn0/origin2000/kona-glyph/"
                   "reset-clears-scissor-and-stipples",
                   test_reset_clears_scissor_and_stipples);
    qtest_add_func("sn0/origin2000/kona-glyph/migration-mid-stipple",
                   test_migration_mid_stipple);
    qtest_add_func("sn0/origin2000/kona-glyph/spans-grammar-row",
                   test_spans_grammar_row);
    qtest_add_func("sn0/origin2000/kona-glyph/spans-arrow-plain-words",
                   test_spans_arrow_plain_words);
    qtest_add_func("sn0/origin2000/kona-glyph/spans-count",
                   test_spans_count);
    qtest_add_func("sn0/origin2000/kona-glyph/spans-clipped-and-stippled",
                   test_spans_clipped_and_stippled);
    qtest_add_func("sn0/origin2000/kona-glyph/spans-ignore-position",
                   test_spans_ignore_position);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
