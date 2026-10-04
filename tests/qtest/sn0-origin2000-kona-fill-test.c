/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA qtests for the X server's rectangle fills: the graphics context
 * load 0x129, the box tokens X box 0x154 and X box6 0x155, and the data
 * tokens 0x142, 0x144, 0x147, 0x149 and 0x14b that carry the boxes before a
 * final X box. A box is x1 << 16 | y1 then x2 << 16 | y2, signed 16-bit
 * halves, far corner exclusive.
 *
 * The GC's word 0 is fg_h, which for a colour-index window carries the index
 * as index << 12 at depth 8. The model fills with that index, tagged as an X
 * pixel, and resolves it at scanout through X colour-map entry 0x2d00 +
 * index, read as 10:10:10 with red in bits 29:20. That the window uses the
 * map at 0x2d00 is inferred; these cases check the model's rule, not
 * hardware. The model also takes a box's y as counting down from the top
 * display line, the X server's convention.
 *
 * Pixels are read two ways: the front colour and tag planes through the
 * model's x-test-plane-word property, in plane coordinates (bottom
 * origin), and a screendump of the pipe's console, in display lines.
 *
 * The fixture is sn0-origin2000-kona-video-test.c's: KONA as a -device
 * at the empty slot IO4, crossbow0's widget 11, with fabric-hub-widget=9.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
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
#define TOKEN_COLOR_MAP     0x12e
#define TOKEN_POSITION      0x12a
#define TOKEN_CLIP          0x12b
#define TOKEN_DATA2         0x142
#define TOKEN_DATA4         0x144
#define TOKEN_BDATA4        0x145
#define TOKEN_DATA6         0x147
#define TOKEN_DATA8         0x149
#define TOKEN_DATA10        0x14b
#define TOKEN_DATA1         0x141
#define TOKEN_RECT          0x152
#define TOKEN_RECT6         0x153
#define TOKEN_BOX           0x154
#define TOKEN_BOX6          0x155

/* Repeated as literals: a test that borrowed the model's could not fail. */
#define KONA_FB_WIDTH  1280
#define KONA_FB_HEIGHT 1024

/* Plane ids, in the order the model declares them. */
#define PLANE_FRONT     0
#define PLANE_FRONT_TAG 2
#define TAG_X_INDEX     3

/* The X colour map's window for X pixels, and two grammar entries. */
#define X_MAP_WINDOW    0x2d00
#define ENTRY_GREY_192  0x304c1304
#define ENTRY_RED       0x3fc00000

#define KONA_QOM_PATH "/machine/module0/kona0"

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4," \
    "fabric-hub-widget=9"

/* The one-node machine is small enough to migrate quickly as it is. */
#define MIGRATION_ARGS MACHINE_ARGS

/* A box word: two signed 16-bit halves. */
#define XY(x, y) (((uint32_t)(uint16_t)(x) << 16) | (uint16_t)(y))

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

/* The raw word of plane `plane` at plane (x, y), through the plane load. */
static uint32_t kona_plane_word(QTestState *s, uint32_t plane, uint32_t x,
                                uint32_t y)
{
    /* plane << 48 | y << 24 | x: one command selects the whole point. */
    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-plane-select', 'value': %" PRIu64 "}}",
        KONA_QOM_PATH, ((uint64_t)plane << 48) | ((uint64_t)y << 24) | x);
    return kona_qom_get_uint(s, "x-test-plane-word");
}

/* The front colour word at X coordinates (x, y), y down from the top. */
/* Existing CI8 fixtures compare logical bytes; raw tests read the plane. */
static uint32_t front_at(QTestState *s, uint32_t x, uint32_t y)
{
    uint32_t row = KONA_FB_HEIGHT - 1 - y;
    uint32_t word = kona_plane_word(s, PLANE_FRONT, x, row);
    uint32_t tag = kona_plane_word(s, PLANE_FRONT_TAG, x, row);

    return tag == TAG_X_INDEX ? word >> 8 : word;
}

static uint32_t front_tag_at(QTestState *s, uint32_t x, uint32_t y)
{
    return kona_plane_word(s, PLANE_FRONT_TAG, x, KONA_FB_HEIGHT - 1 - y);
}

/* The front pixel at X coordinates (x, y), resolved by its tag. */
static uint32_t resolved_at(QTestState *s, uint32_t x, uint32_t y)
{
    kona_qom_set_uint(s, "x-test-pixel-x", x);
    kona_qom_set_uint(s, "x-test-pixel-y", KONA_FB_HEIGHT - 1 - y);
    return kona_qom_get_uint(s, "x-test-pixel");
}

/* A machine logging guest errors to a fresh file, whose path is *log. */
static QTestState *kona_init_logged(const char *base_args, char **log)
{
    int fd;

    *log = g_strdup_printf("%s/sn0-kona-fill-XXXXXX.log", g_get_tmp_dir());
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

/* Word `index` of the model's X graphics context. */
static uint32_t kona_gc(QTestState *s, uint32_t index)
{
    kona_qom_set_uint(s, "x-test-gc-index", index);
    return kona_qom_get_uint(s, "x-test-gc");
}

/* The pipe's console as a PPM, pixels only; the file is removed. */
static uint8_t *kona_screendump(QTestState *s)
{
    static const char header[] = "P6\n1280 1024\n255\n";
    GError *err = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn0-kona-fill-XXXXXX", &err);
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

/* One command on the +0xc10000 port, as plain words, token first. */
static void kona_command(QTestState *s, uint32_t token, const uint32_t *args,
                         unsigned argc)
{
    qtest_writel(s, REG_COMMAND, token);
    for (unsigned i = 0; i < argc; i++) {
        qtest_writel(s, REG_COMMAND, args[i]);
    }
}

/*
 * X load GC with colour index `index` packed into fg_h as depth 8 packs
 * it, index << 12, and the rest of the words the stock guest's first load
 * carried ("The initialisation box and the box packing"), sent as kona.so
 * stores it: word 0 in the token's slot.
 */
static void load_gc(QTestState *s, uint32_t index)
{
    static const uint32_t rest[] = {
        0x1ffe, 0, 0, 0xf00fff, 0, 0xc, 1, 1, 6, 0, 1, 1,
    };

    qtest_writel(s, REG_COMMAND_SLOT(TOKEN_GC), index << 12);
    for (size_t i = 0; i < ARRAY_SIZE(rest); i++) {
        qtest_writel(s, REG_COMMAND_DATA, rest[i]);
    }
}

/* X load colour map: `n` entries from address `start`, start in the slot. */
static void load_x_map(QTestState *s, uint32_t start, const uint32_t *entries,
                       uint32_t n)
{
    qtest_writel(s, REG_COMMAND_SLOT(TOKEN_COLOR_MAP), start);
    qtest_writel(s, REG_COMMAND_DATA, n);
    for (uint32_t i = 0; i < n; i++) {
        qtest_writel(s, REG_COMMAND_DATA, entries[i]);
    }
}

/*
 * The PROM's sync probe, 1 9 0: its mailbox reply shows the command
 * page's framing is back in step after whatever came before it.
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

/*
 * The box [x1, x2) by [y1, y2) holds colour index `colour` at its four
 * corner pixels, tagged as an X pixel, and the pixels just past each edge
 * do not.
 */
static void assert_box(QTestState *s, int x1, int y1, int x2, int y2,
                       uint32_t colour)
{
    g_assert_cmphex(front_at(s, x1, y1), ==, colour);
    g_assert_cmphex(front_at(s, x2 - 1, y1), ==, colour);
    g_assert_cmphex(front_at(s, x1, y2 - 1), ==, colour);
    g_assert_cmphex(front_at(s, x2 - 1, y2 - 1), ==, colour);
    g_assert_cmphex(front_tag_at(s, x1, y1), ==, TAG_X_INDEX);
    g_assert_cmphex(front_tag_at(s, x2 - 1, y2 - 1), ==, TAG_X_INDEX);
    if (x1 > 0) {
        g_assert_cmphex(front_at(s, x1 - 1, y1), !=, colour);
    }
    if (y1 > 0) {
        g_assert_cmphex(front_at(s, x1, y1 - 1), !=, colour);
    }
    if (x2 < KONA_FB_WIDTH) {
        g_assert_cmphex(front_at(s, x2, y2 - 1), !=, colour);
    }
    if (y2 < KONA_FB_HEIGHT) {
        g_assert_cmphex(front_at(s, x2 - 1, y2), !=, colour);
    }
}

/*
 * One X box, (10, 20) to (13, 22), after a GC load whose fg_h is 0x10000,
 * index 0x10, with X colour-map entry 0x2d10 the grammar's 0x304c1304:
 * exactly three columns by two lines take index 0x10, tagged as an X
 * pixel, and show 192, 192, 192 grey on the console, with the far corner
 * excluded. The 13 GC words are kept as sent. Reading fg_h as RGB would
 * show 0x010000; resolving through entry 0x10 of the map rather than
 * 0x2d10 would show black.
 */
static void test_single_box(void)
{
    static const uint32_t gc[] = {
        0x10000, 0x1ffe, 0, 0, 0xf00fff, 0, 0xc, 1, 1, 6, 0, 1, 1,
    };
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    load_x_map(qts, 0x10, (uint32_t[]){ ENTRY_RED }, 1);
    load_x_map(qts, X_MAP_WINDOW + 0x10, (uint32_t[]){ ENTRY_GREY_192 }, 1);
    load_gc(qts, 0x10);
    for (uint32_t i = 0; i < ARRAY_SIZE(gc); i++) {
        g_assert_cmphex(kona_gc(qts, i), ==, gc[i]);
    }
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(13, 22) }, 2);
    assert_box(qts, 10, 20, 13, 22, 0x10);
    g_assert_cmphex(front_at(qts, 11, 21), ==, 0x10);

    frame = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(frame, 10, 20), ==, 0xc0c0c0);
    g_assert_cmphex(ppm_pixel(frame, 12, 21), ==, 0xc0c0c0);
    g_assert_cmphex(ppm_pixel(frame, 13, 21), ==, 0);
    g_assert_cmphex(ppm_pixel(frame, 12, 22), ==, 0);
    g_assert_cmphex(ppm_pixel(frame, 9, 20), ==, 0);
    g_assert_cmphex(ppm_pixel(frame, 10, 19), ==, 0);
    assert_in_step(qts);
}

/*
 * The stock guest's first box, 0 then 0x5000400 after a GC whose word 0
 * is 0 (from a traced guest run): the whole screen. A pixel drawn
 * beforehand is overwritten with index 0, tagged as an X pixel.
 */
static void test_first_box_covers_screen(void)
{
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    load_x_map(qts, X_MAP_WINDOW + 0xff, (uint32_t[]){ ENTRY_RED }, 1);
    load_gc(qts, 0x45);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(600, 500), XY(601, 501) },
                 2);
    g_assert_cmphex(front_at(qts, 600, 500), ==, 0x45);

    load_gc(qts, 0);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ 0, 0x5000400 }, 2);
    g_assert_cmphex(front_at(qts, 600, 500), ==, 0);
    g_assert_cmphex(front_tag_at(qts, 600, 500), ==, TAG_X_INDEX);
    frame = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(frame, 600, 500), ==, 0);

    load_gc(qts, 0xff);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ 0, 0x5000400 }, 2);
    assert_box(qts, 0, 0, KONA_FB_WIDTH, KONA_FB_HEIGHT, 0xff);
    g_free(frame);
    frame = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(frame, 0, 0), ==, 0xfe0000);
    g_assert_cmphex(ppm_pixel(frame, 1279, 1023), ==, 0xfe0000);
    g_assert_cmphex(ppm_pixel(frame, 640, 512), ==, 0xfe0000);
}

/*
 * X box6: six boxes of two words, each filled. The boxes sit apart so
 * each edge check sees background.
 */
static void test_box6(void)
{
    uint32_t words[12];

    qtest_system_reset(qts);
    load_gc(qts, 0xa0);
    for (unsigned i = 0; i < 6; i++) {
        words[2 * i] = XY(100 * i + 5, 7 * i + 3);
        words[2 * i + 1] = XY(100 * i + 5 + i + 1, 7 * i + 3 + 2);
    }
    kona_command(qts, TOKEN_BOX6, words, 12);
    for (unsigned i = 0; i < 6; i++) {
        assert_box(qts, 100 * i + 5, 7 * i + 3, 100 * i + 5 + i + 1,
                   7 * i + 3 + 2, 0xa0);
    }
    assert_in_step(qts);
}

/*
 * A run of data tokens ending in an X box draws every box in it, as
 * konSolidFillRects sends the remainder of its boxes: two boxes in X
 * data4, then the last in X box. Each data token is checked the same way
 * with the count the grammar gives: data2 one box, data6 three, data8
 * four and data10 five, before an X box. The data words are sent as
 * kona.so stores them, the first in the token's slot.
 */
static void test_data_run_ending_in_box(void)
{
    static const struct {
        uint32_t token;
        unsigned boxes;
    } runs[] = {
        { TOKEN_DATA2, 1 }, { TOKEN_DATA4, 2 }, { TOKEN_DATA6, 3 },
        { TOKEN_DATA8, 4 }, { TOKEN_DATA10, 5 },
    };

    for (size_t r = 0; r < ARRAY_SIZE(runs); r++) {
        unsigned n = runs[r].boxes;
        uint32_t colour = 0x10 * (r + 1) + 3;

        qtest_system_reset(qts);
        load_gc(qts, colour);
        for (unsigned i = 0; i < n; i++) {
            uint32_t w0 = XY(20 * i + 2, 40);
            uint32_t w1 = XY(20 * i + 4, 43);

            if (i == 0) {
                qtest_writel(qts, REG_COMMAND_SLOT(runs[r].token), w0);
            } else {
                qtest_writel(qts, REG_COMMAND_DATA, w0);
            }
            qtest_writel(qts, REG_COMMAND_DATA, w1);
        }
        kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(300, 50), XY(302, 52) },
                     2);
        for (unsigned i = 0; i < n; i++) {
            assert_box(qts, 20 * i + 2, 40, 20 * i + 4, 43, colour);
        }
        g_assert_cmphex(front_at(qts, 20 * n + 2, 40), ==, 0);
        assert_box(qts, 300, 50, 302, 52, colour);
        assert_in_step(qts);
    }
}

/*
 * A run of data tokens is drawn only by the X box or rect that ends it,
 * which says how its words read. X data2 then an unsupported X data1
 * packet, then an X box: only the X box's own box is drawn. The same
 * applies to X data1 stored in the address form. X data4 then a clip
 * load: the same. And a run with nothing after it has drawn nothing yet.
 */
static void test_data_run_not_ending_in_box(void)
{
    qtest_system_reset(qts);
    load_gc(qts, 0x77);

    kona_command(qts, TOKEN_DATA2, (uint32_t[]){ XY(10, 10), XY(20, 20) }, 2);
    g_assert_cmphex(front_at(qts, 15, 15), ==, 0);
    kona_command(qts, TOKEN_DATA1, (uint32_t[]){ XY(70, 70) }, 1);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(50, 50), XY(52, 52) }, 2);
    g_assert_cmphex(front_at(qts, 15, 15), ==, 0);
    assert_box(qts, 50, 50, 52, 52, 0x77);

    kona_command(qts, TOKEN_DATA2, (uint32_t[]){ XY(10, 10), XY(20, 20) }, 2);
    qtest_writel(qts, REG_COMMAND_SLOT(TOKEN_DATA1), XY(70, 70));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(56, 56), XY(58, 58) }, 2);
    g_assert_cmphex(front_at(qts, 15, 15), ==, 0);
    assert_box(qts, 56, 56, 58, 58, 0x77);

    kona_command(qts, TOKEN_DATA4,
                 (uint32_t[]){ XY(100, 10), XY(110, 20), XY(120, 10),
                               XY(130, 20) }, 4);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 0, 0, 0x500, 0x400 }, 5);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(60, 60), XY(62, 62) }, 2);
    g_assert_cmphex(front_at(qts, 105, 15), ==, 0);
    g_assert_cmphex(front_at(qts, 125, 15), ==, 0);
    assert_box(qts, 60, 60, 62, 62, 0x77);
    assert_in_step(qts);
}

/* An X rect's two words: signed x, y, then unsigned width, height. */
#define WH(w, h) (((uint32_t)(w) << 16) | (uint32_t)(h))

/*
 * X rect words are relative to the drawable the X load position names,
 * and the position is added before clipping ("GC state, position and
 * clip"). The grammar's test: position 100, 50, 200, 200 and a full-screen
 * clip, then X rect (4, 4) by 16 by 16 fills (104, 54)-(120, 70) and
 * leaves (4, 4) alone. The run's text field: position -192, 179 and clip
 * (0, 184)-(28, 212), then a rect (10, 9) by 205 by 20, which meets the
 * clip at (0, 188)-(23, 208) only with the origin added.
 */
static void test_rect_origin(void)
{
    qtest_system_reset(qts);
    load_gc(qts, 0x41);
    kona_command(qts, TOKEN_POSITION, (uint32_t[]){ 100, 50, 200, 200 }, 4);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 0, 0, 1280, 1024 }, 5);
    kona_command(qts, TOKEN_RECT, (uint32_t[]){ 0x40004, 0x100010 }, 2);
    assert_box(qts, 104, 54, 120, 70, 0x41);
    g_assert_cmphex(front_at(qts, 4, 4), ==, 0);

    load_gc(qts, 0x42);
    kona_command(qts, TOKEN_POSITION,
                 (uint32_t[]){ (uint32_t)-192, 179, 225, 38 }, 4);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 0, 184, 28, 212 }, 5);
    kona_command(qts, TOKEN_RECT, (uint32_t[]){ XY(10, 9), WH(205, 20) }, 2);
    assert_box(qts, 0, 188, 23, 208, 0x42);
    g_assert_cmphex(front_at(qts, 11, 10), ==, 0);
    assert_in_step(qts);
}

/*
 * The rect path's runs, as konPolyFillRectSolid sends them: X rect6 with
 * six rectangles, and X data4 with two rectangles before a final X rect.
 * Every rectangle takes the position's origin; a box sent later does not.
 */
static void test_rect_runs(void)
{
    uint32_t words[12];

    qtest_system_reset(qts);
    load_gc(qts, 0x51);
    kona_command(qts, TOKEN_POSITION, (uint32_t[]){ 300, 200, 400, 400 }, 4);
    for (unsigned i = 0; i < 6; i++) {
        words[2 * i] = XY(10 * i, i);
        words[2 * i + 1] = WH(3, 2);
    }
    kona_command(qts, TOKEN_RECT6, words, 12);
    for (unsigned i = 0; i < 6; i++) {
        assert_box(qts, 300 + 10 * i, 200 + i, 303 + 10 * i, 202 + i, 0x51);
    }

    load_gc(qts, 0x52);
    kona_command(qts, TOKEN_DATA4,
                 (uint32_t[]){ XY(0, 50), WH(2, 2), XY(10, 50), WH(2, 2) }, 4);
    kona_command(qts, TOKEN_RECT, (uint32_t[]){ XY(20, 50), WH(2, 2) }, 2);
    assert_box(qts, 300, 250, 302, 252, 0x52);
    assert_box(qts, 310, 250, 312, 252, 0x52);
    assert_box(qts, 320, 250, 322, 252, 0x52);
    g_assert_cmphex(front_at(qts, 0, 50), ==, 0);

    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(0, 60), XY(2, 62) }, 2);
    assert_box(qts, 0, 60, 2, 62, 0x52);
    assert_in_step(qts);
}

/*
 * Boxes are clipped to the screen: one reaching past the top-left corner
 * with negative coordinates fills only its on-screen part, one past the
 * bottom-right corner likewise, and neither wraps to the other side. A
 * box whose far corner is not past its near one, or that lies wholly off
 * the screen, draws nothing.
 */
static void test_clipping(void)
{
    qtest_system_reset(qts);
    load_gc(qts, 0x01);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(-5, -5), XY(3, 2) }, 2);
    assert_box(qts, 0, 0, 3, 2, 0x01);
    g_assert_cmphex(front_at(qts, 1279, 1023), ==, 0);
    g_assert_cmphex(front_at(qts, 1279, 0), ==, 0);
    g_assert_cmphex(front_at(qts, 0, 1023), ==, 0);

    load_gc(qts, 0x04);
    kona_command(qts, TOKEN_BOX,
                 (uint32_t[]){ XY(1277, 1022), XY(1290, 1030) }, 2);
    assert_box(qts, 1277, 1022, KONA_FB_WIDTH, KONA_FB_HEIGHT, 0x04);
    g_assert_cmphex(front_at(qts, 0, 0), ==, 0x01);

    load_gc(qts, 0x07);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(40, 40), XY(30, 50) }, 2);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(40, 40), XY(50, 40) }, 2);
    kona_command(qts, TOKEN_BOX,
                 (uint32_t[]){ XY(-100, 10), XY(-1, 20) }, 2);
    kona_command(qts, TOKEN_BOX,
                 (uint32_t[]){ XY(1280, 10), XY(1300, 20) }, 2);
    g_assert_cmphex(front_at(qts, 35, 45), ==, 0);
    g_assert_cmphex(front_at(qts, 45, 40), ==, 0);
    g_assert_cmphex(front_at(qts, 0, 15), ==, 0);
    g_assert_cmphex(front_at(qts, 1279, 15), ==, 0);
    assert_in_step(qts);
}

/* Native packing retains every index bit, including below a CI8 value. */
static void test_gc_index_packing(void)
{
    static const uint32_t fg_h[] = { 0x10000, 0x15000, 0x18000, 0x1d000,
                                     0x2f000, 0x2f078 };
    static const uint32_t index[] = { 0x10, 0x15, 0x18, 0x1d, 0x2f, 0x2f };
    static const uint32_t raw[] = { 0x1000, 0x1500, 0x1800, 0x1d00,
                                   0x2f00, 0x2f0f };

    qtest_system_reset(qts);
    for (size_t i = 0; i < ARRAY_SIZE(fg_h); i++) {
        uint32_t gc[13] = { fg_h[i], 0x1ffe, 0, 0, 0xf00fff, 0,
                             0xc, 1, 1, 6, 0, 1, 1 };

        kona_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
        kona_command(qts, TOKEN_BOX,
                     (uint32_t[]){ XY(10 * i, 0), XY(10 * i + 2, 2) }, 2);
        assert_box(qts, 10 * i, 0, 10 * i + 2, 2, index[i]);
        g_assert_cmphex(kona_plane_word(qts, PLANE_FRONT, 10 * i,
                                        KONA_FB_HEIGHT - 1), ==, raw[i]);
    }
    /* The all-zero synthetic GC is unsupported, with complete framing. */
    kona_command(qts, TOKEN_GC, (uint32_t[13]) { 0 }, 13);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(70, 0), XY(72, 2) }, 2);
    g_assert_cmphex(front_at(qts, 70, 0), ==, 0);
    g_assert_cmphex(front_tag_at(qts, 70, 0), ==, 0);
    load_gc(qts, 0x2f);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(70, 0), XY(72, 2) }, 2);
    assert_box(qts, 70, 0, 72, 2, 0x2f);
    assert_in_step(qts);
}

/*
 * The grammar's greys: entries loaded as one 256-entry load at 0x2d00, as
 * the stock X server loads its default map, resolve an X pixel of index
 * 0x10, 0x15, 0x1d, 0x22 and 0x23 to 192, 213, 153, 235 and 96 grey. A
 * later single-entry load changes a pixel already drawn when it is next
 * shown. A textport pixel of the same index resolves through the
 * textport's own colour map instead, so the two maps do not mix.
 */
static void test_x_map_resolves_at_scanout(void)
{
    static const struct {
        uint32_t index, entry, rgb;
    } greys[] = {
        { 0x10, 0x304c1304, 0xc0c0c0 }, { 0x15, 0x358d6358, 0xd5d5d5 },
        { 0x1d, 0x26499264, 0x999999 }, { 0x22, 0x3b0ec3b0, 0xebebeb },
        { 0x23, 0x18060180, 0x606060 },
    };
    g_autofree uint32_t *map = g_new0(uint32_t, 256);
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    for (size_t i = 0; i < ARRAY_SIZE(greys); i++) {
        map[greys[i].index] = greys[i].entry;
    }
    load_x_map(qts, X_MAP_WINDOW, map, 256);
    for (size_t i = 0; i < ARRAY_SIZE(greys); i++) {
        load_gc(qts, greys[i].index);
        kona_command(qts, TOKEN_BOX,
                     (uint32_t[]){ XY(20 * i, 30), XY(20 * i + 4, 34) }, 2);
        g_assert_cmphex(resolved_at(qts, 20 * i + 1, 31), ==, greys[i].rgb);
    }

    /* Textport colour map entry 0x22 is 0x112233; a point in it at 700, 5. */
    kona_command(qts, 1, (uint32_t[]){ 3, 0x22, 0x11 << 2, 0x22 << 2,
                                       0x33 << 2 }, 5);
    kona_command(qts, 1, (uint32_t[]){ 2, 0x22 }, 2);
    kona_command(qts, 1, (uint32_t[]){ 6, 700, 5 }, 3);

    frame = kona_screendump(qts);
    for (size_t i = 0; i < ARRAY_SIZE(greys); i++) {
        g_assert_cmphex(ppm_pixel(frame, 20 * i + 1, 31), ==, greys[i].rgb);
    }
    g_assert_cmphex(ppm_pixel(frame, 700, KONA_FB_HEIGHT - 1 - 5), ==,
                    0x112233);

    load_x_map(qts, X_MAP_WINDOW + 0x22, (uint32_t[]){ ENTRY_RED }, 1);
    g_free(frame);
    frame = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(frame, 20 * 3 + 1, 31), ==, 0xfe0000);
    g_assert_cmphex(ppm_pixel(frame, 20 * 4 + 1, 31), ==, 0x606060);
    g_assert_cmphex(ppm_pixel(frame, 700, KONA_FB_HEIGHT - 1 - 5), ==,
                    0x112233);
    assert_in_step(qts);
}

/*
 * A colour-map load reaching past the model's table stores the entries
 * inside it, counts the rest as outside, logs them, and stays in step:
 * three entries from 0x7fff put the first in the table's last entry.
 */
static void test_x_map_past_table(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, &log);

    load_x_map(s, 0x7fff, (uint32_t[]){ 1, 9, 0 }, 3);
    assert_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "colour map load at 0x7fff count 3: 2 "
                                   "entries outside the model's table"));
}

/*
 * X load clip with a count of 1 carries one exclusive clip rectangle, and
 * boxes arrive absolute, so the X load position never offsets them. The
 * greeter run's clips:
 * 1, 240, 295, 312, 327 cuts a larger box down to (240, 295)-(312, 327),
 * and 1, 0, 179, 33, 217, sent after position -192, 179, 225, 38, cuts
 * (-192, 179)-(33, 217) at the screen's left edge. A count of 0, which no
 * sender read makes, clips only to the screen, and a box drawn after a
 * position load lands where its own words put it.
 */
static void test_clip_rectangle(void)
{
    qtest_system_reset(qts);
    load_gc(qts, 0x21);
    kona_command(qts, TOKEN_POSITION, (uint32_t[]){ 240, 295, 72, 32 }, 4);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 240, 295, 312, 327 }, 5);
    kona_command(qts, TOKEN_BOX,
                 (uint32_t[]){ XY(230, 285), XY(320, 335) }, 2);
    assert_box(qts, 240, 295, 312, 327, 0x21);

    load_gc(qts, 0x22);
    kona_command(qts, TOKEN_POSITION,
                 (uint32_t[]){ (uint32_t)-192, 179, 225, 38 }, 4);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 0, 179, 33, 217 }, 5);
    kona_command(qts, TOKEN_BOX,
                 (uint32_t[]){ XY(-192, 179), XY(33, 217) }, 2);
    assert_box(qts, 0, 179, 33, 217, 0x22);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(500, 500), XY(502, 502) },
                 2);
    g_assert_cmphex(front_at(qts, 500, 500), ==, 0);

    load_gc(qts, 0x23);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 0, 0, 179, 33, 217 }, 5);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(500, 500), XY(502, 502) },
                 2);
    assert_box(qts, 500, 500, 502, 502, 0x23);
    kona_command(qts, TOKEN_POSITION, (uint32_t[]){ 600, 100, 10, 10 }, 4);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(5, 800), XY(7, 802) }, 2);
    assert_box(qts, 5, 800, 7, 802, 0x23);
    g_assert_cmphex(front_at(qts, 605, 900), ==, 0);
    assert_in_step(qts);
}

/*
 * A clip list of three rectangles, as kon_loadClipRects sends one: X
 * bdata4 with the count and the first rectangle's x1, y1, x2, X bdata4
 * with its y2 and the second's x1, y1, x2, then X load clip with the
 * second's y2 and the third rectangle. A box across all three fills only
 * inside them, and so does a full-screen box under the grammar's two-box
 * list. A list whose length does not match its count clips only to
 * the screen, and a list cut short by another token is dropped, leaving
 * the X load clip after it a one-rectangle list of its own.
 */
static void test_clip_list(void)
{
    qtest_system_reset(qts);
    load_gc(qts, 0x31);
    kona_command(qts, TOKEN_BDATA4, (uint32_t[]){ 3, 10, 10, 20 }, 4);
    kona_command(qts, TOKEN_BDATA4, (uint32_t[]){ 20, 30, 10, 40 }, 4);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 20, 50, 12, 60, 20 }, 5);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(0, 0), XY(100, 30) }, 2);
    assert_box(qts, 10, 10, 20, 20, 0x31);
    assert_box(qts, 30, 10, 40, 20, 0x31);
    assert_box(qts, 50, 12, 60, 20, 0x31);
    g_assert_cmphex(front_at(qts, 25, 15), ==, 0);
    g_assert_cmphex(front_at(qts, 45, 15), ==, 0);
    g_assert_cmphex(front_at(qts, 55, 10), ==, 0);

    /* The grammar's list: 0x145 2, 0, 0, 10 and 0x12b 10, 20, 20, 30, 30. */
    load_gc(qts, 0x34);
    kona_command(qts, TOKEN_BDATA4, (uint32_t[]){ 2, 0, 0, 10 }, 4);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 10, 20, 20, 30, 30 }, 5);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ 0, 0x5000400 }, 2);
    g_assert_cmphex(front_at(qts, 0, 0), ==, 0x34);
    g_assert_cmphex(front_at(qts, 9, 9), ==, 0x34);
    g_assert_cmphex(front_at(qts, 20, 20), ==, 0x34);
    g_assert_cmphex(front_at(qts, 29, 29), ==, 0x34);
    g_assert_cmphex(front_at(qts, 15, 15), ==, 0x31);
    g_assert_cmphex(front_at(qts, 10, 5), ==, 0);
    g_assert_cmphex(front_at(qts, 30, 29), ==, 0);
    g_assert_cmphex(front_at(qts, 40, 40), ==, 0);
    assert_in_step(qts);

    load_gc(qts, 0x32);
    kona_command(qts, TOKEN_BDATA4, (uint32_t[]){ 3, 10, 10, 20 }, 4);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 20, 50, 12, 60, 20 }, 5);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(0, 40), XY(4, 44) }, 2);
    assert_box(qts, 0, 40, 4, 44, 0x32);

    load_gc(qts, 0x33);
    kona_command(qts, TOKEN_BDATA4, (uint32_t[]){ 2, 100, 100, 110 }, 4);
    kona_command(qts, TOKEN_POSITION, (uint32_t[]){ 0, 0, 1280, 1024 }, 4);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 200, 200, 210, 210 }, 5);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(0, 0), XY(300, 300) }, 2);
    assert_box(qts, 200, 200, 210, 210, 0x33);
    g_assert_cmphex(front_at(qts, 105, 105), ==, 0);
    assert_in_step(qts);
}

/*
 * Reset clears the graphics context, the X colour map and the clip and
 * drops a run of data tokens still waiting for its X box: after it, the
 * GC reads zero, an X box draws only its own box, not the run's, outside
 * the old clip rectangle, and its index resolves to black.
 */
static void test_reset_clears_gc_and_run(void)
{
    qtest_system_reset(qts);
    load_x_map(qts, X_MAP_WINDOW + 0x0f, (uint32_t[]){ ENTRY_GREY_192 }, 1);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 0, 0, 1, 1 }, 5);
    load_gc(qts, 0x65);
    kona_command(qts, TOKEN_DATA2, (uint32_t[]){ XY(10, 10), XY(20, 20) }, 2);
    qtest_system_reset(qts);
    for (uint32_t i = 0; i < 13; i++) {
        g_assert_cmphex(kona_gc(qts, i), ==, 0);
    }
    load_gc(qts, 0x0f);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(30, 30), XY(32, 32) }, 2);
    g_assert_cmphex(front_at(qts, 15, 15), ==, 0);
    assert_box(qts, 30, 30, 32, 32, 0x0f);
    g_assert_cmphex(resolved_at(qts, 31, 31), ==, 0);
    assert_in_step(qts);
}

/*
 * Reset clears the X load position and a clip list waiting for the X
 * load clip that ends it: after a position of 100, 50 and an X bdata4
 * opening a two-rectangle list, a reset, then a one-rectangle clip of
 * (30, 30)-(40, 40), an X rect at (30, 30) lands there and not at
 * (130, 80), and boxes outside the clip draw nothing. A waiting list
 * surviving the reset would join the clip's words as a two-rectangle
 * list whose first rectangle is (0, 0)-(10, 1).
 */
static void test_reset_clears_position_and_clip_list(void)
{
    qtest_system_reset(qts);
    kona_command(qts, TOKEN_POSITION, (uint32_t[]){ 100, 50, 64, 64 }, 4);
    kona_command(qts, TOKEN_BDATA4, (uint32_t[]){ 2, 0, 0, 10 }, 4);
    qtest_system_reset(qts);

    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 30, 30, 40, 40 }, 5);
    load_gc(qts, 0x21);
    kona_command(qts, TOKEN_RECT, (uint32_t[]){ XY(30, 30), 0x20002 }, 2);
    assert_box(qts, 30, 30, 32, 32, 0x21);
    g_assert_cmphex(front_tag_at(qts, 130, 80), ==, 0);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(50, 50), XY(52, 52) }, 2);
    g_assert_cmphex(front_tag_at(qts, 50, 50), ==, 0);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(0, 0), XY(10, 1) }, 2);
    g_assert_cmphex(front_tag_at(qts, 5, 0), ==, 0);
    assert_in_step(qts);
}

/*
 * A store at an offset the command page does not decode ends a waiting run
 * of data tokens: the run's box is not drawn by the X box after it, which
 * draws only its own.
 */
static void test_unexpected_offset_drops_box_run(void)
{
    qtest_system_reset(qts);
    load_gc(qts, 0x22);
    kona_command(qts, TOKEN_DATA2, (uint32_t[]){ XY(10, 10), XY(12, 12) }, 2);
    qtest_writel(qts, KONA_REG(0xc10004), 0);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(30, 30), XY(32, 32) }, 2);
    g_assert_cmphex(front_tag_at(qts, 10, 10), ==, 0);
    g_assert_cmphex(front_tag_at(qts, 11, 11), ==, 0);
    assert_box(qts, 30, 30, 32, 32, 0x22);
    assert_in_step(qts);
}

/*
 * The leading words of a two-rectangle clip list, then a store that ends
 * them, then the X load clip that would have ended the list: the clip
 * alone fails its count, so a box over (0, 0)-(40, 40) fills all of it,
 * where the complete list would have kept (15, 15) clear.
 */
static void assert_clip_list_dropped_by(uint64_t addr)
{
    qtest_system_reset(qts);
    load_gc(qts, 0x23);
    kona_command(qts, TOKEN_BDATA4, (uint32_t[]){ 2, 0, 0, 10 }, 4);
    qtest_writel(qts, addr, 0);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 10, 20, 20, 30, 30 }, 5);
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(0, 0), XY(40, 40) }, 2);
    g_assert_cmphex(front_at(qts, 15, 15), ==, 0x23);
    g_assert_cmphex(front_at(qts, 35, 35), ==, 0x23);
    assert_in_step(qts);
}

/* A store at an offset the command page does not decode. */
static void test_unexpected_offset_drops_clip_list(void)
{
    assert_clip_list_dropped_by(KONA_REG(0xc10004));
}

/* An unrecognized slot token discards pending clip-list data. */
static void test_unknown_token_drops_clip_list(void)
{
    assert_clip_list_dropped_by(REG_COMMAND_SLOT(0x140));
}

/*
 * The graphics context, the X colour map, the clip and a run of data
 * tokens waiting for its X box cross a migration: the destination's X box
 * draws the run's two boxes and its own in the source's colour index, the
 * pixels drawn before the migration are still there and resolve through
 * the source's colour map, and a later box is clipped by the source's
 * clip.
 */
static void test_migration_carries_gc_and_run(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-fill-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    load_x_map(from, X_MAP_WINDOW + 0x13, (uint32_t[]){ ENTRY_GREY_192 }, 1);
    load_gc(from, 0x13);
    kona_command(from, TOKEN_BOX, (uint32_t[]){ XY(5, 5), XY(7, 7) }, 2);
    kona_command(from, TOKEN_CLIP, (uint32_t[]){ 1, 0, 0, 40, 40 }, 5);
    kona_command(from, TOKEN_DATA4,
                 (uint32_t[]){ XY(10, 10), XY(12, 12), XY(20, 10),
                               XY(22, 12) }, 4);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(kona_gc(to, 0), ==, 0x13000);
    g_assert_cmphex(kona_gc(to, 12), ==, 1);
    kona_command(to, TOKEN_BOX, (uint32_t[]){ XY(30, 10), XY(32, 12) }, 2);
    assert_box(to, 5, 5, 7, 7, 0x13);
    assert_box(to, 10, 10, 12, 12, 0x13);
    assert_box(to, 20, 10, 22, 12, 0x13);
    assert_box(to, 30, 10, 32, 12, 0x13);
    g_assert_cmphex(resolved_at(to, 6, 6), ==, 0xc0c0c0);
    kona_command(to, TOKEN_BOX, (uint32_t[]){ XY(38, 38), XY(42, 42) }, 2);
    assert_box(to, 38, 38, 40, 40, 0x13);
    assert_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

#define CURSOR_WORDS 260

static void cursor_register(QTestState *s, uint32_t selector, uint32_t value)
{
    qtest_writel(s, KONA_REG(0x8003c0), selector);
    qtest_writel(s, KONA_REG(0x8003d0), value);
}

static uint32_t cursor_position(int x, int y)
{
    return ((x + 32) & 0x1fff) | (((y + 32) & 0x1fff) << 13);
}

static void cursor_image(uint32_t *image)
{
    memset(image, 0, CURSOR_WORDS * sizeof(*image));
    image[0] = 0xa0000000;
    image[2] = 0x60000000;
    image[63 * 4 + 1] = 1;
    image[256] = 0x000003ff;
    image[257] = 0x3ff00000;
    image[258] = 0x000ffc00;
    image[259] = 0x3fffffff;
}

static void cursor_load(QTestState *s, uint32_t offset, const uint32_t *image)
{
    qtest_writel(s, REG_COMMAND_SLOT(0x130), offset);
    for (unsigned i = 0; i < CURSOR_WORDS; i++) {
        qtest_writel(s, REG_COMMAND_DATA, image[i]);
    }
}

static void test_loaded_cursor(void)
{
    uint32_t image[CURSOR_WORDS];
    g_autofree uint8_t *pixels = NULL;

    qtest_system_reset(qts);
    load_gc(qts, 0x77);
    load_x_map(qts, X_MAP_WINDOW + 0x77, (uint32_t[]){ ENTRY_GREY_192 }, 1);
    kona_command(qts, TOKEN_BOX,
                 (uint32_t[]){ XY(100, 50), XY(164, 114) }, 2);
    cursor_image(image);
    cursor_load(qts, 0, image);
    cursor_register(qts, 0x100, 0);
    cursor_register(qts, 0x101, 0);
    cursor_register(qts, 0x102, cursor_position(100, 50));
    cursor_register(qts, 0x103, 2);
    pixels = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0xff0000);
    g_assert_cmphex(ppm_pixel(pixels, 101, 50), ==, 0x00ff00);
    g_assert_cmphex(ppm_pixel(pixels, 102, 50), ==, 0x0000ff);
    g_assert_cmphex(ppm_pixel(pixels, 103, 50), ==, 0xc0c0c0);
    g_assert_cmphex(front_at(qts, 100, 50), ==, 0x77);
    cursor_register(qts, 0x103, 0);
    g_clear_pointer(&pixels, g_free);
    pixels = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0xc0c0c0);
}

static void test_cursor_edges(void)
{
    static const int positions[][2] = {
        { -63, 0 }, { -32, 0 }, { -31, 0 }, { 0, 0 },
        { 0, -63 }, { 0, -32 }, { 0, -31 },
        { 1279, 0 }, { 0, 1023 }, { 1279, 1023 },
        { -63, -63 }, { -63, 1023 }, { 1279, -63 },
    };
    uint32_t image[CURSOR_WORDS] = { 0 };

    qtest_system_reset(qts);
    for (unsigned row = 0; row < 64; row++) {
        image[4 * row] = image[4 * row + 1] = 0xffffffff;
    }
    image[252] &= ~0x80000000;
    image[254] = 0x80000000;
    image[255] = 1;
    image[256] = 0x000003ff;
    image[257] = 0x3ff00000;
    image[258] = 0x000ffc00;
    cursor_load(qts, 0, image);
    cursor_register(qts, 0x103, 2);
    for (unsigned i = 0; i < ARRAY_SIZE(positions); i++) {
        int x = positions[i][0], y = positions[i][1];
        unsigned px = MAX(x, 0), py = MAX(y, 0);
        uint32_t expected = (x == -63 && y == -63) ? 0xff :
                            (x >= 0 && y == -63) ? 0xff00 : 0xff0000;
        g_autofree uint8_t *pixels = NULL;

        cursor_register(qts, 0x102, cursor_position(x, y));
        pixels = kona_screendump(qts);
        g_assert_cmphex(ppm_pixel(pixels, px, py), ==, expected);
        g_assert_cmphex(front_at(qts, px, py), ==, 0);
        cursor_register(qts, 0x103, 0);
        g_clear_pointer(&pixels, g_free);
        pixels = kona_screendump(qts);
        g_assert_cmphex(ppm_pixel(pixels, px, py), ==, 0);
        cursor_register(qts, 0x103, 2);
    }
}

static void test_cursor_selection_recolor(void)
{
    uint32_t image[CURSOR_WORDS];
    g_autofree uint8_t *pixels = NULL;

    qtest_system_reset(qts);
    cursor_image(image);
    cursor_load(qts, 0, image);
    image[257] = 0x000003ff;
    cursor_load(qts, 260, image);
    cursor_register(qts, 0x102, cursor_position(100, 50));
    cursor_register(qts, 0x103, 2);
    pixels = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0xff0000);
    cursor_register(qts, 0x101, 260);
    g_clear_pointer(&pixels, g_free);
    pixels = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0xff);
    kona_command(qts, 0x131, (uint32_t[]){ 260, 0x3ff00000, 0x000ffc00,
                  0x000003ff, 0x3fffffff }, 5);
    g_clear_pointer(&pixels, g_free);
    pixels = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0xff00);
    g_assert_cmphex(ppm_pixel(pixels, 101, 50), ==, 0xff);
    g_assert_cmphex(ppm_pixel(pixels, 102, 50), ==, 0xff0000);
    cursor_register(qts, 0x101, 0);
    cursor_register(qts, 0x102, cursor_position(200, 100));
    g_clear_pointer(&pixels, g_free);
    pixels = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0);
    g_assert_cmphex(ppm_pixel(pixels, 200, 100), ==, 0xff0000);
    g_assert_cmphex(front_at(qts, 200, 100), ==, 0);
}

static void test_cursor_refusal_abort(void)
{
    static const struct { uint32_t selector, value; } refused[] = {
        { 0x100, 1 }, { 0x101, 1 }, { 0x101, 64 * 260 },
        { 0x101, 260 }, { 0x103, 1 }, { 0x103, 4 },
        { 0x102, 0 },
    };
    uint32_t image[CURSOR_WORDS];
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, &log);

    cursor_image(image);
    cursor_load(s, 0, image);
    for (unsigned i = 0; i < ARRAY_SIZE(refused) + 4; i++) {
        g_autofree uint8_t *pixels = NULL;

        cursor_register(s, 0x100, 0);
        cursor_register(s, 0x101, 0);
        cursor_register(s, 0x102, cursor_position(100, 50));
        cursor_register(s, 0x103, 2);
        if (i < ARRAY_SIZE(refused)) {
            cursor_register(s, refused[i].selector,
                            i == 6 ? cursor_position(-64, 0) :
                                     refused[i].value);
        } else {
            static const int out[][2] = {
                { 0, -64 }, { 1280, 0 }, { 0, 1024 }, { 4000, 4000 },
            };
            unsigned n = i - ARRAY_SIZE(refused);

            cursor_register(s, 0x102, cursor_position(out[n][0], out[n][1]));
        }
        pixels = kona_screendump(s);
        g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0);
        g_assert_cmphex(ppm_pixel(pixels, 0, 0), ==, 0);
    }
    cursor_register(s, 0x100, 0);
    cursor_register(s, 0x101, 0);
    cursor_register(s, 0x102, cursor_position(100, 50));
    cursor_register(s, 0x103, 2);
    cursor_register(s, 0x80, 1);
    cursor_register(s, 0xb00, 1);
    cursor_register(s, 0xb02, 1);
    cursor_register(s, 0xc00, 1);
    cursor_register(s, 0xc02, 1);
    kona_command(s, 0x131, (uint32_t[]){ 0x269c, 0, 0, 0, 0 }, 5);
    cursor_load(s, 1, image);
    qtest_writel(s, REG_COMMAND_SLOT(0x130), 0);
    for (unsigned i = 0; i < 130; i++) {
        qtest_writel(s, REG_COMMAND_DATA, 0);
    }
    qtest_writel(s, REG_COMMAND_SLOT(TOKEN_BOX), XY(500, 500));
    qtest_writel(s, REG_COMMAND_DATA, XY(502, 502));
    {
        g_autofree uint8_t *pixels = kona_screendump(s);

        g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0xff0000);
    }
    qtest_writel(s, REG_COMMAND_SLOT(0x131), 0);
    qtest_writel(s, REG_COMMAND_DATA, 0);
    qtest_writel(s, REG_COMMAND_DATA, 0);
    qtest_writel(s, REG_COMMAND_DATA, 0);
    qtest_system_reset(s);
    cursor_register(s, 0x102, cursor_position(100, 50));
    cursor_register(s, 0x103, 2);
    {
        g_autofree uint8_t *pixels = kona_screendump(s);

        g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0);
    }
    cursor_load(s, 0, image);
    {
        g_autofree uint8_t *pixels = kona_screendump(s);

        g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0xff0000);
    }
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "unsupported cursor channel"));
    g_assert_nonnull(strstr(found, "invalid or unloaded cursor offset"));
    g_assert_nonnull(strstr(found, "unsupported cursor position"));
    g_assert_nonnull(strstr(found, "unsupported cursor mode"));
    g_assert_nonnull(strstr(found, "unsupported cursor selector"));
    g_assert_nonnull(strstr(found, "unsupported crosshair recolouring"));
    g_assert_nonnull(strstr(found, "invalid cursor load offset"));
    g_assert_nonnull(strstr(found, "partial command dropped"));
}

static void test_cursor_reset_partial_image(void)
{
    uint32_t image[CURSOR_WORDS];
    g_autofree uint8_t *pixels = NULL;

    qtest_system_reset(qts);
    cursor_image(image);
    qtest_writel(qts, REG_COMMAND_SLOT(0x130), 260);
    for (unsigned i = 0; i < 130; i++) {
        qtest_writel(qts, REG_COMMAND_DATA, image[i]);
    }
    qtest_system_reset(qts);
    cursor_register(qts, 0x101, 260);
    cursor_register(qts, 0x102, cursor_position(100, 50));
    cursor_register(qts, 0x103, 2);
    pixels = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0);

    /* A surviving partial load would become valid from this orphaned tail. */
    for (unsigned i = 130; i < CURSOR_WORDS; i++) {
        qtest_writel(qts, REG_COMMAND_DATA, image[i]);
    }
    g_clear_pointer(&pixels, g_free);
    pixels = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0);
    g_assert_cmphex(front_at(qts, 100, 50), ==, 0);
    cursor_load(qts, 260, image);
    g_clear_pointer(&pixels, g_free);
    pixels = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0xff0000);
    assert_in_step(qts);
}

static void test_cursor_migration(void)
{
    for (unsigned partial = 0; partial < 3; partial++) {
        g_autofree char *dir = NULL, *uri = NULL, *dest = NULL;
        g_autofree uint8_t *before = NULL, *after = NULL;
        GError *err = NULL;
        QTestState *from, *to;
        uint32_t image[CURSOR_WORDS];

        dir = g_dir_make_tmp("sn0-kona-loaded-cursor-XXXXXX", &err);
        g_assert_no_error(err);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        from = qtest_init(MIGRATION_ARGS);
        dest = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
        to = qtest_init(dest);
        cursor_image(image);
        cursor_load(from, 0, image);
        cursor_register(from, 0x102, cursor_position(1279, 1023));
        cursor_register(from, 0x103, 2);
        if (partial == 1) {
            qtest_writel(from, REG_COMMAND_SLOT(0x130), 260);
            for (unsigned i = 0; i < 130; i++) {
                qtest_writel(from, REG_COMMAND_DATA, image[i]);
            }
        } else if (partial == 2) {
            qtest_writel(from, REG_COMMAND_SLOT(0x131), 0);
            qtest_writel(from, REG_COMMAND_DATA, 0x3ff00000);
            qtest_writel(from, REG_COMMAND_DATA, 0x000003ff);
            qtest_writel(from, REG_COMMAND_DATA, 0x000ffc00);
        }
        before = kona_screendump(from);
        sgi_migrate_stopped(from, uri);
        after = kona_screendump(to);
        g_assert_cmpmem(before, KONA_FB_WIDTH * KONA_FB_HEIGHT * 3,
                        after, KONA_FB_WIDTH * KONA_FB_HEIGHT * 3);
        if (partial == 1) {
            for (unsigned i = 130; i < CURSOR_WORDS; i++) {
                qtest_writel(to, REG_COMMAND_DATA, image[i]);
            }
            cursor_register(to, 0x101, 260);
            cursor_register(to, 0x102, cursor_position(100, 50));
        } else if (partial == 2) {
            qtest_writel(to, REG_COMMAND_DATA, 0x3fffffff);
        }
        g_clear_pointer(&after, g_free);
        after = kona_screendump(to);
        g_assert_cmphex(ppm_pixel(after, partial == 1 ? 100 : 1279,
                                  partial == 1 ? 50 : 1023), ==,
                        partial == 2 ? 0xff : 0xff0000);
        if (partial == 1) {
            cursor_register(to, 0x103, 0);
        } else {
            qtest_writel(to, KONA_REG(0x8003d0), 0);
        }
        g_clear_pointer(&after, g_free);
        after = kona_screendump(to);
        g_assert_cmphex(ppm_pixel(after, partial == 1 ? 100 : 1279,
                                  partial == 1 ? 50 : 1023), ==, 0);
        cursor_register(to, 0x103, 2);
        assert_in_step(to);
        qtest_system_reset(to);
        cursor_register(to, 0x102, cursor_position(100, 50));
        cursor_register(to, 0x103, 2);
        g_clear_pointer(&after, g_free);
        after = kona_screendump(to);
        g_assert_cmphex(ppm_pixel(after, 100, 50), ==, 0);
        qtest_quit(to);
        qtest_quit(from);
        g_rmdir(dir);
    }
}

static void test_cursor_migration_refuses_bounds(void)
{
    static const char section[] = "sgi-kona/loaded-cursor";
    g_autofree char *dir = NULL, *path = NULL, *quoted = NULL;
    g_autofree char *outgoing = NULL, *incoming = NULL;
    g_autofree char *data = NULL;
    GError *err = NULL;
    gsize size, fields = 0;
    QTestState *from;

    dir = g_dir_make_tmp("sn0-kona-cursor-bounds-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(dir, "state", NULL);
    quoted = g_shell_quote(path);
    outgoing = g_strdup_printf("exec:cat > %s", quoted);
    incoming = g_strdup_printf("exec:cat %s", quoted);
    from = qtest_init(MIGRATION_ARGS);
    {
        uint32_t image[CURSOR_WORDS];

        cursor_image(image);
        cursor_load(from, 0, image);
        cursor_register(from, 0x102, cursor_position(100, 50));
        cursor_register(from, 0x103, 2);
    }
    sgi_migrate_stopped(from, outgoing);
    qtest_quit(from);
    g_assert_true(g_file_get_contents(path, &data, &size, NULL));
    for (gsize i = 0; i + sizeof(section) + 4 < size; i++) {
        if (!memcmp(data + i, section, sizeof(section) - 1)) {
            fields = i + sizeof(section) - 1 + 4;
            break;
        }
    }
    g_assert_cmpuint(fields, >, 0);
    for (unsigned bad = 0; bad < 5; bad++) {
        g_autofree char *corrupt = g_memdup2(data, size);
        QTestState *to;
        gsize length = size;

        if (bad == 0) {
            stl_be_p(corrupt + fields, 261);
        } else if (bad == 1) {
            stl_be_p(corrupt + fields + 4, 65);
        } else if (bad == 2) {
            corrupt[fields + 8 + 260 * 4 + 64 * 260 * 4] = 2;
        } else if (bad == 3) {
            stl_be_p(corrupt + fields, 260);
        } else {
            gsize start = fields - 4 - (sizeof(section) - 1) - 2;
            gsize end = fields + 8 + 260 * 4 + 64 * 260 * 4 + 64 + 5 * 4;

            g_assert_cmpuint(end, <, size);
            memmove(corrupt + start, corrupt + end, size - end);
            length -= end - start;
        }
        g_assert_true(g_file_set_contents(path, corrupt, length, NULL));
        to = qtest_initf(MIGRATION_ARGS " -incoming defer");
        if (bad < 4) {
            qtest_set_expected_status(to, 1);
        }
        qtest_qmp_assert_success(to,
            "{'execute': 'migrate-incoming', 'arguments': {'uri': %s}}",
            incoming);
        if (bad < 4) {
            qtest_wait_qemu(to);
        } else {
            g_autofree uint8_t *pixels = NULL;

            sgi_wait_migration(to, "completed");
            pixels = kona_screendump(to);
            g_assert_cmphex(ppm_pixel(pixels, 100, 50), ==, 0);
        }
        qtest_quit(to);
    }
    g_unlink(path);
    g_rmdir(dir);
}

static void load_point_gc(QTestState *s, uint32_t index)
{
    uint32_t gc[] = {
        index << 12, 0x1ffe, 0, 0, 0xf00fff, 0,
        0xc, 1, 1, 6, 0, 1, 1,
    };

    kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
}

static void test_absolute_points(void)
{
    qtest_system_reset(qts);
    load_point_gc(qts, 0x3c);
    kona_command(qts, TOKEN_POSITION, (uint32_t[]){ 100, 50, 200, 200 }, 4);
    kona_command(qts, 0x141, (uint32_t[]){ XY(10, 20) }, 1);
    g_assert_cmphex(front_at(qts, 10, 20), ==, 0);
    kona_command(qts, 0x14c, (uint32_t[]){ XY(30, 40) }, 1);
    g_assert_cmphex(front_at(qts, 10, 20), ==, 0x3c);
    g_assert_cmphex(front_at(qts, 30, 40), ==, 0x3c);
    g_assert_cmphex(front_at(qts, 110, 70), ==, 0);
    kona_command(qts, 0x143,
                 (uint32_t[]){ XY(50, 60), XY(70, 80), XY(90, 100) }, 3);
    g_assert_cmphex(front_at(qts, 50, 60), ==, 0);
    kona_command(qts, 0x14d, (uint32_t[]){ XY(110, 120), XY(130, 140),
                  XY(150, 160), XY(170, 180), XY(190, 200), XY(210, 220) }, 6);
    for (unsigned i = 0; i < 9; i++) {
        g_assert_cmphex(front_at(qts, 50 + 20 * i, 60 + 20 * i), ==, 0x3c);
    }
    kona_command(qts, 0x14d, (uint32_t[]){ XY(-1, 0), XY(0, -1),
                  XY(1280, 0), XY(0, 1024), XY(-100, -100),
                  XY(2000, 2000) }, 6);
    g_assert_cmphex(front_at(qts, 0, 0), ==, 0);
    assert_in_step(qts);
}

static void test_point_clipping(void)
{
    qtest_system_reset(qts);
    load_point_gc(qts, 0x3c);
    kona_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 10, 10, 20, 20 }, 5);
    kona_command(qts, 0x12d, (uint32_t[]){ 12, 12, 6, 6 }, 4);
    kona_command(qts, 0x13d, (uint32_t[]){ 5, 2, 1 }, 3);
    qtest_writel(qts, REG_COMMAND, 0x80000000);
    kona_command(qts, 0x13c, (uint32_t[]){ 5 }, 1);
    kona_command(qts, 0x14d, (uint32_t[]){ XY(9, 14), XY(10, 14),
                  XY(12, 14), XY(13, 14), XY(18, 14), XY(20, 14) }, 6);
    g_assert_cmphex(front_at(qts, 9, 14), ==, 0);
    g_assert_cmphex(front_at(qts, 10, 14), ==, 0);
    g_assert_cmphex(front_at(qts, 12, 14), ==, 0x3c);
    g_assert_cmphex(front_at(qts, 13, 14), ==, 0);
    g_assert_cmphex(front_at(qts, 18, 14), ==, 0);
    g_assert_cmphex(front_at(qts, 20, 14), ==, 0);
}

static void test_point_gc_refusal(void)
{
    static const struct {
        unsigned word;
        uint32_t value;
    } rejected[] = {
        { 9, 13 }, { 4, 0xf0000e }, { 6, 8 }, { 6, 0x8c },
        { 6, 0x20c }, { 0, 0x100003c }, { 1, 0x1000 }, { 5, 1 },
        { 7, 2 }, { 8, 2 }, { 10, 1 }, { 11, 2 }, { 12, 2 },
    };
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, &log);

    for (unsigned i = 0; i < ARRAY_SIZE(rejected); i++) {
        uint32_t gc[] = {
            0x3c000, 0x1ffe, 0, 0, 0xf00fff, 0,
            0xc, 1, 1, 6, 0, 1, 1,
        };

        qtest_system_reset(s);
        load_gc(s, 0xa5);
        kona_command(s, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(11, 21) }, 2);
        gc[rejected[i].word] = rejected[i].value;
        kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
        kona_command(s, 0x141, (uint32_t[]){ XY(30, 40) }, 1);
        kona_command(s, 0x14c, (uint32_t[]){ XY(10, 20) }, 1);
        g_assert_cmphex(front_at(s, 10, 20), ==, 0xa5);
        g_assert_cmphex(front_at(s, 30, 40), ==, 0);
        load_point_gc(s, 0x3c);
        kona_command(s, 0x14c, (uint32_t[]){ XY(10, 20) }, 1);
        g_assert_cmphex(front_at(s, 10, 20), ==, 0x3c);
        g_assert_cmphex(front_at(s, 30, 40), ==, 0);
    }
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "unsupported X GC status "));
}

static void test_point_run_discard(void)
{
    static const struct { uint32_t token; unsigned count; } unsupported[] = {
        { 0x159, 2 },
        { 0x15b, 1 }, { 0x15c, 7 }, { 0x15d, 2 }, { 0x15e, 7 },
    };
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, &log);
    uint32_t words[24];

    load_point_gc(s, 0x3c);
    for (unsigned i = 0; i < ARRAY_SIZE(unsupported); i++) {
        for (unsigned j = 0; j < ARRAY_SIZE(words); j++) {
            words[j] = 0x14c;
        }
        kona_command(s, 0x143,
                     (uint32_t[]){ XY(10, 20), XY(30, 40), XY(50, 60) }, 3);
        kona_command(s, unsupported[i].token, words, unsupported[i].count);
        kona_command(s, TOKEN_BOX, (uint32_t[]){ XY(70, 80), XY(72, 82) }, 2);
        assert_box(s, 70, 80, 72, 82, 0x3c);
        g_assert_cmphex(front_at(s, 10, 20), ==, 0);
        g_assert_cmphex(front_at(s, 0, 332), ==, 0);
        assert_in_step(s);
    }
    kona_command(s, 0x143,
                 (uint32_t[]){ XY(10, 20), XY(30, 40), XY(50, 60) }, 3);
    kona_command(s, TOKEN_BOX, (uint32_t[]){ XY(90, 100), XY(92, 102) }, 2);
    g_assert_cmphex(front_at(s, 15, 25), ==, 0);
    assert_box(s, 90, 100, 92, 102, 0x3c);
    for (unsigned i = 0; i < 11; i++) {
        kona_command(s, 0x143,
                     (uint32_t[]){ XY(10, 20), XY(30, 40), XY(50, 60) }, 3);
    }
    kona_command(s, 0x14c, (uint32_t[]){ XY(110, 120) }, 1);
    g_assert_cmphex(front_at(s, 10, 20), ==, 0);
    g_assert_cmphex(front_at(s, 110, 120), ==, 0x3c);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "unsupported X drawing token"));
    g_assert_nonnull(strstr(found, "X data run of 3 words dropped"));
    g_assert_nonnull(strstr(found, "more than 32 box words"));
}

typedef struct XFramingCase {
    const char *name;
    uint32_t token;
    unsigned words;
} XFramingCase;

/* Selector-shaped coordinates must never become GL commands. */
static void test_point_data_framing(gconstpointer opaque)
{
    const XFramingCase *c = opaque;

    for (unsigned slot = 0; slot < 2; slot++) {
        g_autofree char *log = NULL, *found = NULL, *packet = NULL;
        QTestState *s = kona_init_logged(MACHINE_ARGS
                                         " -trace enable=sgi_kona_*", &log);
        uint32_t args[24] = { 0x1a };

        args[c->words - 1] = 0x1a;
        kona_command(s, 0x1fd,
                     (uint32_t[]){ 0x10000340, 0x9000, 0x10000 }, 3);
        load_gc(s, 0x55);
        if (slot) {
            qtest_writel(s, REG_COMMAND_SLOT(c->token), args[0]);
            for (unsigned i = 1; i < c->words; i++) {
                qtest_writel(s, REG_COMMAND_DATA, args[i]);
            }
        } else {
            kona_command(s, c->token, args, c->words);
        }
        kona_command(s, TOKEN_BOX,
                     (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
        assert_box(s, 10, 20, 12, 22, 0x55);
        g_assert_cmphex(front_at(s, 0, 26), ==,
                        c->token == 0x14c || c->token == 0x14d ||
                        c->token == 0x14e || c->token == 0x14f ||
                        c->token == 0x150 || c->token == 0x159 ||
                        c->token == 0x15c || c->token == 0x15d ||
                        c->token == 0x15e ? 0x55 : 0);
        assert_in_step(s);
        qtest_quit(s);
        found = kona_log_read(log);
        packet = g_strdup_printf("sgi_kona_kernel_command token 0x%x",
                                  c->token);
        g_assert_nonnull(strstr(found, packet));
    }
}

/* The partial point packet migrates, and reset discards a later one. */
static void test_point_framing_migrates(void)
{
    g_autofree char *dir = NULL, *uri = NULL, *dest = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    dir = g_dir_make_tmp("sn0-kona-point-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    from = qtest_init(MIGRATION_ARGS);
    dest = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest);
    load_point_gc(from, 0x55);
    kona_command(from, 0x143,
                 (uint32_t[]){ XY(50, 60), XY(70, 80), XY(90, 100) }, 3);
    g_assert_cmphex(front_at(from, 50, 60), ==, 0);
    qtest_writel(from, REG_COMMAND, 0x14d);
    qtest_writel(from, REG_COMMAND, 0x1a);
    qtest_writel(from, REG_COMMAND, 0);
    qtest_writel(from, REG_COMMAND, 0);
    g_assert_cmphex(qtest_readl(from, KONA_REG(0x800208)), ==, 8);
    sgi_migrate_stopped(from, uri);
    g_assert_cmphex(qtest_readl(to, KONA_REG(0x800208)), ==, 8);
    qtest_writel(to, REG_COMMAND, 0);
    qtest_writel(to, REG_COMMAND, 0);
    qtest_writel(to, REG_COMMAND, 0x1a);
    g_assert_cmphex(qtest_readl(to, KONA_REG(0x800208)), ==, 0);
    g_assert_cmphex(front_at(to, 50, 60), ==, 0x55);
    g_assert_cmphex(front_at(to, 70, 80), ==, 0x55);
    g_assert_cmphex(front_at(to, 90, 100), ==, 0x55);
    g_assert_cmphex(front_at(to, 0, 26), ==, 0x55);
    kona_command(to, TOKEN_BOX,
                 (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    assert_box(to, 10, 20, 12, 22, 0x55);

    kona_command(to, 0x143,
                 (uint32_t[]){ XY(50, 60), XY(70, 80), XY(90, 100) }, 3);
    qtest_writel(to, REG_COMMAND, 0x14d);
    qtest_writel(to, REG_COMMAND, 0x1a);
    qtest_system_reset(to);
    g_assert_cmphex(qtest_readl(to, KONA_REG(0x800208)), ==, 0);
    load_point_gc(to, 0x55);
    kona_command(to, 0x14c, (uint32_t[]){ XY(110, 120) }, 1);
    g_assert_cmphex(front_at(to, 50, 60), ==, 0);
    g_assert_cmphex(front_at(to, 110, 120), ==, 0x55);
    kona_command(to, TOKEN_BOX,
                 (uint32_t[]){ XY(30, 40), XY(32, 42) }, 2);
    assert_box(to, 30, 40, 32, 42, 0x55);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

/* Independent X GX truth tables, indexed by (source bit, destination bit). */
static uint8_t semantic_ci_result(unsigned gx, uint8_t source, uint8_t dest,
                                   uint8_t mask)
{
    static const uint8_t truth[] = {
        0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15,
    };
    uint8_t result = 0;

    for (unsigned bit = 0; bit < 8; bit++) {
        unsigned input = (((source >> bit) & 1) << 1) |
                         ((dest >> bit) & 1);

        result |= ((truth[gx] >> input) & 1) << bit;
    }
    return (result & mask) | (dest & ~mask);
}

/*
 * The digests of `count` planes from `first` on, sixty-four hex digits a
 * plane, in one command; digest_at() picks one out.
 */
static char *sem_digests(QTestState *s, unsigned first, unsigned count)
{
    QDict *reply;
    char *all;

    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-plane', 'value': %u}}", KONA_QOM_PATH, first);
    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-plane-count', 'value': %u}}", KONA_QOM_PATH,
        count);
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'x-test-plane-digests'}}", KONA_QOM_PATH);
    all = g_strdup(qdict_get_str(reply, "return"));
    qobject_unref(reply);
    g_assert_cmpuint(strlen(all), ==, count * 64);
    return all;
}

/* Plane `i` of a sem_digests() result; dashes mean no storage. */
static char *digest_at(const char *all, unsigned i)
{
    g_assert_cmpint(all[i * 64], !=, '-');
    return g_strndup(all + i * 64, 64);
}

static void semantic_rgb_gc(uint32_t *gc, uint32_t rgb, uint32_t mask,
                              unsigned gx)
{
    uint64_t color = 0x1ffe;
    uint32_t fields[3];

    for (unsigned i = 0; i < 3; i++) {
        unsigned byte = (rgb >> (8 * i)) & 0xff;
        unsigned mask_byte = (mask >> (8 * i)) & 0xff;

        color |= (uint64_t)((byte << 4) | (byte >> 4)) << (14 + 13 * i);
        fields[i] = (mask_byte << 4) | (mask_byte >> 4);
    }
    memset(gc, 0, 13 * sizeof(*gc));
    gc[0] = color >> 32;
    gc[1] = color;
    gc[3] = 0x1ffe;
    gc[4] = fields[2] | (fields[1] << 12);
    gc[5] = 0xfff000 | fields[0];
    gc[6] = 4;
    gc[7] = 1;
    gc[8] = 1;
    gc[9] = gx * 2;
}

static void test_point_batch(gconstpointer opaque)
{
    unsigned value = GPOINTER_TO_UINT(opaque), shape = value / 16;
    unsigned mode = value % 16;
    static const unsigned tokens[] = { 0x14e, 0x15f, 0x160, 0x161 };
    static const unsigned counts[] = { 24, 1, 6, 24 };
    bool background = shape != 0, rgb = mode == 2 || mode == 3;
    unsigned gx = mode == 1 || mode == 3 || mode == 5 ? 6 :
                  mode == 8 ? 5 : 3;
    unsigned count = counts[shape];
    uint32_t mask = mode == 7 ? 0 : rgb ?
                    (mode == 3 ? 0x53a56c : 0xffffff) :
                    mode == 1 ? 0x53 : 0xff;
    uint32_t source = rgb ? (background ? 0x1379bd : 0x962ca5) :
                           (background ? 0x96 : 0x3c);
    uint32_t dest = rgb ? 0x5a81c3 : 0xa5;
    uint32_t gc[13] = { 0xa5000, 0x1ffe, 0, 0x1ffe, 0xf00fff, 0,
                        0xc, 1, 1, 6, 0, 1, 1 };
    uint32_t args[24], result = 0;
    uint32_t pending[] = { XY(90, 40), XY(92, 40), XY(94, 40) };
    g_autofree char *dir = NULL, *uri = NULL;
    GError *error = NULL;
    QTestState *from = qtest_init(MACHINE_ARGS), *to = NULL, *s = from;

    if (rgb) {
        semantic_rgb_gc(gc, dest, 0xffffff, 3);
    }
    kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(s, TOKEN_BOX, (uint32_t[]) { XY(8, 18), XY(70, 22) }, 2);
    kona_command(s, TOKEN_BOX, (uint32_t[]) { XY(88, 38), XY(96, 42) }, 2);
    if (mode == 5) {
        semantic_rgb_gc(gc, 0x123456, 0xffffff, 3);
        kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
        kona_command(s, TOKEN_BOX,
                     (uint32_t[]) { XY(10 + 2 * (count - 1), 20),
                                    XY(11 + 2 * (count - 1), 21) }, 2);
    }
    if (rgb) {
        uint32_t bg[13];

        semantic_rgb_gc(gc, 0x962ca5, mask, gx);
        semantic_rgb_gc(bg, 0x1379bd, mask, gx);
        gc[2] = bg[0];
        gc[3] = bg[1];
    } else {
        memcpy(gc, (uint32_t[]) { 0x3c000, 0x1ffe, 0x96000, 0x1ffe,
                                 0xf0000f | (mask << 4), 0,
                                 0xc, 1, 1, gx * 2, 0, 1, 1 }, sizeof(gc));
    }
    kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
    if (mode == 4) {
        kona_command(s, TOKEN_POSITION, (uint32_t[]) { 100, 50, 200, 200 }, 4);
        kona_command(s, TOKEN_CLIP, (uint32_t[]) { 1, 10, 20, 70, 21 }, 5);
        kona_command(s, 0x12d, (uint32_t[]) { 20, 20, 16, 1 }, 4);
        kona_command(s, 0x13d, (uint32_t[]) { 5, 4, 1 }, 3);
        qtest_writel(s, REG_COMMAND, 0x80000000);
        kona_command(s, 0x13c, (uint32_t[]) { 5 }, 1);
    }
    for (unsigned i = 0; i < count; i++) {
        args[i] = XY(10 + 2 * i, 20);
    }
    for (unsigned byte = 0; byte < (rgb ? 3 : 1); byte++) {
        result |= (uint32_t)semantic_ci_result(gx, source >> (8 * byte),
                                                dest >> (8 * byte),
                                                mask >> (8 * byte)) <<
                  (8 * byte);
    }
    kona_command(s, 0x143, pending, ARRAY_SIZE(pending));
    g_assert_cmphex(front_at(s, 90, 40), ==, dest);
    if (mode == 6) {
        unsigned partial = count / 2;

        kona_command(s, tokens[shape], args, partial);
        g_assert_cmphex(front_at(s, 90, 40), ==, dest);
        g_assert_cmphex(front_at(s, 10, 20), ==, dest);
        dir = g_dir_make_tmp("kona-point-batch-XXXXXX", &error);
        g_assert_no_error(error);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MIGRATION_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
        for (unsigned i = partial; i < count; i++) {
            qtest_writel(s, REG_COMMAND, args[i]);
        }
    } else {
        kona_command(s, tokens[shape], args, count);
    }
    for (unsigned i = 0; i < count; i++) {
        unsigned x = 10 + 2 * i;
        bool drawn = mode != 5 &&
                     (mode != 4 || (x >= 20 && x < 36 && x % 4 == 0));
        uint32_t expected = mode == 5 && i == count - 1 ? 0x123456 :
                            drawn ? result : dest;

        g_assert_cmphex(front_at(s, x, 20), ==, expected);
        g_assert_cmphex(front_at(s, x + 1, 20), ==, dest);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(pending); i++) {
        g_assert_cmphex(front_at(s, 90 + 2 * i, 40), ==,
                        mode == 4 || mode == 5 ? dest : result);
    }
    if (to) {
        qtest_quit(to);
        g_rmdir(dir);
    }
    qtest_quit(from);
}

static void test_solid_line_batch(gconstpointer opaque)
{
    unsigned value = GPOINTER_TO_UINT(opaque), shape = value / 16;
    unsigned mode = value % 16;
    bool six = shape == 2 || shape == 4, relative = shape >= 3;
    unsigned count = six ? 7 : relative ? 2 : shape ? 1 : 2;
    bool rgb = mode == 2 || mode == 3;
    unsigned gx = mode == 1 || mode == 3 || mode == 5 ? 6 :
                  mode == 8 ? 5 : 3;
    uint32_t mask = mode == 7 ? 0 : rgb ?
                    (mode == 3 ? 0x53a56c : 0xffffff) :
                    mode == 1 ? 0x53 : 0xff;
    uint32_t source = rgb ? 0x962ca5 : 0x3c;
    uint32_t dest = rgb ? 0x5a81c3 : 0xa5;
    uint32_t result = (dest & ~mask) |
                      ((gx == 6 ? source ^ dest : gx == 5 ? dest : source) &
                       mask);
    uint32_t gc[13] = { 0xa5000, 0x1ffe, 0, 0x1ffe, 0xf00fff, 0,
                        0xc, 1, 1, 6, 0, 1, 1 };
    const uint32_t absolute_pending[] = { XY(10, 20), XY(14, 20) };
    const uint32_t relative_pending[] = { XY(10, 20), XY(4, 0) };
    const uint32_t *pending = relative ? relative_pending : absolute_pending;
    const uint32_t segment[] = { XY(18, 22), XY(22, 22) };
    const uint32_t line[] = { XY(14, 24) };
    const uint32_t line6[] = {
        XY(14, 24), XY(18, 24), XY(18, 28), XY(22, 28),
        XY(22, 32), XY(26, 32), XY(26, 36),
    };
    const uint32_t relative_line[] = { XY(0, 2), XY(0, 2) };
    const uint32_t relative_line6[] = {
        XY(0, 4), XY(4, 0), XY(0, 4), XY(4, 0),
        XY(0, 4), XY(4, 0), XY(0, 4),
    };
    const uint32_t *args = relative ?
        (six ? relative_line6 : relative_line) :
        (six ? line6 : shape ? line : segment);
    unsigned token = relative ? (six ? 0x15e : 0x15d) :
                     six ? 0x15c : shape ? 0x15b : 0x159;
    unsigned bad_x = six ? 26 : shape ? 14 : 22;
    unsigned bad_y = six ? 35 : shape ? 24 : 22;
    g_autofree char *dir = NULL, *uri = NULL;
    GError *error = NULL;
    QTestState *from = qtest_init(MACHINE_ARGS), *to = NULL, *s = from;

    if (rgb) {
        semantic_rgb_gc(gc, dest, 0xffffff, 3);
    }
    kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(s, TOKEN_BOX, (uint32_t[]) { XY(8, 18), XY(36, 40) }, 2);
    if (mode == 5) {
        semantic_rgb_gc(gc, 0x123456, 0xffffff, 3);
        kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
        kona_command(s, TOKEN_BOX,
                     (uint32_t[]) { XY(bad_x, bad_y),
                                    XY(bad_x + 1, bad_y + 1) }, 2);
    }
    if (rgb) {
        semantic_rgb_gc(gc, source, mask, gx);
    } else {
        memcpy(gc, (uint32_t[]) { 0x3c000, 0x1ffe, 0, 0x1ffe,
                                 0xf0000f | (mask << 4), 0,
                                 0xc, 1, 1, gx * 2, 0, 1, 1 }, sizeof(gc));
    }
    kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
    if (mode == 4) {
        kona_command(s, TOKEN_POSITION, (uint32_t[]) { 100, 50, 200, 200 }, 4);
        kona_command(s, TOKEN_CLIP, (uint32_t[]) { 1, 11, 19, 26, 34 }, 5);
        kona_command(s, 0x12d, (uint32_t[]) { 12, 20, 12, 12 }, 4);
        kona_command(s, 0x13d, (uint32_t[]) { 5, 4, 1 }, 3);
        qtest_writel(s, REG_COMMAND, 0x80000000);
        kona_command(s, 0x13c, (uint32_t[]) { 5 }, 1);
    }
    kona_command(s, 0x142, pending, 2);
    g_assert_cmphex(front_at(s, 10, 20), ==, dest);
    if (mode == 6) {
        unsigned partial = count / 2;

        kona_command(s, token, args, partial);
        g_assert_cmphex(front_at(s, 10, 20), ==, dest);
        dir = g_dir_make_tmp("kona-solid-line-XXXXXX", &error);
        g_assert_no_error(error);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MIGRATION_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
        for (unsigned i = partial; i < count; i++) {
            qtest_writel(s, REG_COMMAND, args[i]);
        }
    } else {
        kona_command(s, token, args, count);
    }
    for (unsigned y = 18; y < 40; y++) {
        for (unsigned x = 8; x < 36; x++) {
            bool covered = y == 20 && x >= 10 && x < 14;
            bool drawn;

            if (!shape) {
                covered |= (y == 20 && x == 14) ||
                           (y == 22 && x >= 18 && x <= 22);
            } else {
                covered |= x == 14 && y >= 20 && y <= 24;
                if (six) {
                    covered |= (y == 24 && x >= 14 && x <= 18) ||
                               (x == 18 && y >= 24 && y <= 28) ||
                               (y == 28 && x >= 18 && x <= 22) ||
                               (x == 22 && y >= 28 && y <= 32) ||
                               (y == 32 && x >= 22 && x <= 26) ||
                               (x == 26 && y >= 32 && y < 36);
                }
            }
            drawn = covered && mode != 5 &&
                    (mode != 4 || (x >= 12 && x < 24 && y >= 20 &&
                                   y < 32 && x % 4 == 0));
            g_assert_cmphex(front_at(s, x, y), ==,
                            mode == 5 && x == bad_x && y == bad_y ?
                            0x123456 : drawn ? result : dest);
        }
    }
    if (six && mode != 5) {
        kona_command(s, 0x141, (uint32_t[]) { XY(26, 36) }, 1);
        if (relative) {
            kona_command(s, 0x15d, (uint32_t[]) { XY(2, 0), XY(2, 0) }, 2);
        } else {
            kona_command(s, 0x15b, (uint32_t[]) { XY(30, 36) }, 1);
        }
        for (unsigned x = 26; x <= 30; x++) {
            g_assert_cmphex(front_at(s, x, 36), ==,
                            mode == 4 ? dest : result);
        }
    }
    if (to) {
        qtest_quit(to);
        g_rmdir(dir);
    }
    qtest_quit(from);
}

static void test_solid_line_octant(gconstpointer opaque)
{
    unsigned octant = GPOINTER_TO_UINT(opaque);
    const unsigned coverage[][2] = {
        { 0, 0 }, { 1, 0 }, { 2, 1 }, { 3, 1 }, { 4, 2 }, { 5, 2 },
    };
    int dx = octant & 1 ? -1 : 1, dy = octant & 2 ? -1 : 1;
    bool transpose = octant & 4;
    QTestState *s = qtest_init(MACHINE_ARGS);
    int end_x = 20 + dx * (transpose ? 2 : 5);
    int end_y = 20 + dy * (transpose ? 5 : 2);

    load_gc(s, 0x3c);
    kona_command(s, 0x159,
                 (uint32_t[]) { XY(20, 20), XY(end_x, end_y) }, 2);
    for (int y = 14; y <= 26; y++) {
        for (int x = 14; x <= 26; x++) {
            bool covered = false;

            for (unsigned i = 0; i < ARRAY_SIZE(coverage); i++) {
                covered |= x == 20 + dx * coverage[i][transpose ? 1 : 0] &&
                           y == 20 + dy * coverage[i][transpose ? 0 : 1];
            }
            g_assert_cmphex(front_at(s, x, y), ==, covered ? 0x3c : 0);
        }
    }
    qtest_quit(s);
}

static void test_solid_line_control(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    QTestState *s = qtest_init(MACHINE_ARGS);
    uint32_t gc[] = { 0x3c000, 0x1ffe, 0, 0x1ffe, 0xf00fff, 0,
                      0xc, 1, 1, 12, 0, 1, 1 };

    gc[0] = 0;
    gc[9] = 6;
    kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(s, TOKEN_BOX, (uint32_t[]) { XY(0, 0), XY(40, 40) }, 2);
    gc[0] = 0x3c000;
    gc[9] = 12;
    kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
    if (mode == 0) {
        const uint32_t vertices[] = {
            XY(10, 20), XY(14, 20), XY(14, 24), XY(10, 24),
        };

        kona_command(s, 0x144, vertices, ARRAY_SIZE(vertices));
        kona_command(s, 0x15b, (uint32_t[]) { XY(10, 20) }, 1);
        for (unsigned y = 19; y <= 25; y++) {
            for (unsigned x = 9; x <= 15; x++) {
                bool border = x >= 10 && x <= 14 && y >= 20 && y <= 24 &&
                              (x == 10 || x == 14 || y == 20 || y == 24);

                g_assert_cmphex(front_at(s, x, y), ==, border ? 0x3c : 0);
            }
        }
    } else if (mode == 1) {
        kona_command(s, 0x143,
                     (uint32_t[]) { XY(10, 20), XY(10, 20), XY(10, 20) }, 3);
        kona_command(s, 0x15b, (uint32_t[]) { XY(10, 20) }, 1);
        g_assert_cmphex(front_at(s, 10, 20), ==, 0x3c);
        kona_command(s, 0x159,
                     (uint32_t[]) { XY(12, 20), XY(12, 20) }, 2);
        g_assert_cmphex(front_at(s, 12, 20), ==, 0x3c);
        kona_command(s, 0x15c, (uint32_t[]) {
            XY(14, 20), XY(14, 20), XY(14, 20), XY(14, 20),
            XY(14, 20), XY(14, 20), XY(14, 20),
        }, 7);
        g_assert_cmphex(front_at(s, 14, 20), ==, 0);
    } else if (mode == 7) {
        kona_command(s, TOKEN_CLIP, (uint32_t[]) { 1, 0, 0, 4, 4 }, 5);
        kona_command(s, 0x159, (uint32_t[]) { XY(-3, -1), XY(2, 1) }, 2);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                bool covered = (x == 0 && y == 0) ||
                               (x == 1 && y == 1) || (x == 2 && y == 1);

                g_assert_cmphex(front_at(s, x, y), ==, covered ? 0x3c : 0);
            }
        }
    } else if (mode == 8) {
        kona_command(s, 0x159, (uint32_t[]) { XY(10, 20), XY(14, 22) }, 2);
        for (unsigned y = 20; y <= 22; y++) {
            for (unsigned x = 10; x <= 14; x++) {
                bool covered = y == 20 + (x - 10 + 1) / 2;

                g_assert_cmphex(front_at(s, x, y), ==, covered ? 0x3c : 0);
            }
        }
    } else {
        if (mode != 3) {
            kona_command(s, 0x141, (uint32_t[]) { XY(10, 20) }, 1);
        }
        if (mode == 4) {
            gc[7] = 5;
            kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
        } else if (mode == 5) {
            qtest_system_reset(s);
            gc[9] = 6;
            kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
        } else if (mode == 6) {
            kona_command(s, TOKEN_POSITION, (uint32_t[]) { 0, 0, 40, 40 }, 4);
        }
        if (mode == 2 || mode == 4) {
            kona_command(s, 0x159,
                         (uint32_t[]) { XY(12, 20), XY(14, 20) }, 2);
        } else {
            kona_command(s, 0x15b, (uint32_t[]) { XY(14, 20) }, 1);
        }
        for (unsigned x = 10; x <= 14; x++) {
            g_assert_cmphex(front_at(s, x, 20), ==, 0);
        }
        gc[7] = 1;
        kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
        kona_command(s, 0x15b, (uint32_t[]) { XY(18, 20) }, 1);
        g_assert_cmphex(front_at(s, 18, 20), ==, 0);
        kona_command(s, 0x159,
                     (uint32_t[]) { XY(20, 20), XY(24, 20) }, 2);
        for (unsigned x = 20; x <= 24; x++) {
            g_assert_cmphex(front_at(s, x, 20), ==, 0x3c);
        }
    }
    assert_in_step(s);
    qtest_quit(s);
}

static void test_relative_line_coordinates(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    uint32_t xor_gc[] = { 0x3c000, 0x1ffe, 0, 0x1ffe, 0xf00fff, 0,
                          0xc, 1, 1, 12, 0, 1, 1 };
    g_autofree char *dir = NULL, *uri = NULL;
    GError *error = NULL;
    QTestState *from = qtest_init(MACHINE_ARGS), *to = NULL, *s = from;

    load_gc(s, 0);
    kona_command(s, TOKEN_BOX, (uint32_t[]) { XY(0, 0), XY(64, 64) }, 2);
    load_gc(s, 0x3c);
    if (mode == 0) {
        kona_command(s, 0x15d, (uint32_t[]) { XY(20, 20), XY(4, 0) }, 2);
        kona_command(s, 0x15d, (uint32_t[]) { XY(10, 20), XY(4, 0) }, 2);
        for (unsigned x = 10; x <= 30; x++) {
            bool covered = x <= 14 || (x >= 20 && x <= 24);

            g_assert_cmphex(front_at(s, x, 20), ==, covered ? 0x3c : 0);
        }
    } else if (mode == 1) {
        kona_command(s, TOKEN_GC, xor_gc, ARRAY_SIZE(xor_gc));
        kona_command(s, 0x144,
                     (uint32_t[]) { XY(10, 20), XY(4, 0),
                                    XY(0, 4), XY(-4, 0) }, 4);
        kona_command(s, 0x15d, (uint32_t[]) { XY(0, -2), XY(0, -2) }, 2);
        for (unsigned y = 19; y <= 25; y++) {
            for (unsigned x = 9; x <= 15; x++) {
                bool border = x >= 10 && x <= 14 && y >= 20 && y <= 24 &&
                              (x == 10 || x == 14 || y == 20 || y == 24);

                g_assert_cmphex(front_at(s, x, y), ==, border ? 0x3c : 0);
            }
        }
    } else if (mode == 2) {
        kona_command(s, TOKEN_GC, xor_gc, ARRAY_SIZE(xor_gc));
        kona_command(s, 0x143, (uint32_t[]) { XY(10, 20), 0, 0 }, 3);
        kona_command(s, 0x15d, (uint32_t[]) { 0, 0 }, 2);
        g_assert_cmphex(front_at(s, 10, 20), ==, 0x3c);
        kona_command(s, 0x15e,
                     (uint32_t[]) { XY(12, 20), 0, 0, 0, 0, 0, 0 }, 7);
        g_assert_cmphex(front_at(s, 12, 20), ==, 0);
    } else if (mode == 3) {
        kona_command(s, TOKEN_CLIP, (uint32_t[]) { 1, 0, 0, 4, 4 }, 5);
        kona_command(s, 0x15d, (uint32_t[]) { XY(-3, -1), XY(5, 2) }, 2);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                bool covered = (x == 0 && y == 0) ||
                               (x == 1 && y == 1) || (x == 2 && y == 1);

                g_assert_cmphex(front_at(s, x, y), ==, covered ? 0x3c : 0);
            }
        }
    } else if (mode >= 4 && mode <= 7) {
        bool vertical = mode & 1, lower = mode >= 6;
        int delta = lower ? INT16_MIN : INT16_MAX;
        uint32_t args[] = {
            vertical ? XY(0, lower ? delta : 4) : XY(lower ? delta : 4, 0),
            vertical ? XY(0, delta) : XY(delta, 0),
        };

        kona_command(s, 0x141, (uint32_t[]) { XY(10, 20) }, 1);
        kona_command(s, 0x15d, args, ARRAY_SIZE(args));
        for (unsigned y = 20; y <= 24; y++) {
            for (unsigned x = 10; x <= 14; x++) {
                g_assert_cmphex(front_at(s, x, y), ==, 0);
            }
        }
        kona_command(s, 0x15d, (uint32_t[]) { XY(20, 20), XY(4, 0) }, 2);
        g_assert_cmphex(front_at(s, 20, 20), ==, 0x3c);
        g_assert_cmphex(front_at(s, 24, 20), ==, 0x3c);
    } else if (mode == 8 || mode == 11) {
        uint32_t pending[32], args[7];

        pending[0] = XY(10, 20);
        for (unsigned i = 1; i < ARRAY_SIZE(pending); i++) {
            pending[i] = XY(1, 0);
        }
        for (unsigned i = 0; i < ARRAY_SIZE(args); i++) {
            args[i] = XY(1, 0);
        }
        for (unsigned i = 0; i < 30; i += 10) {
            kona_command(s, 0x14b, pending + i, 10);
        }
        kona_command(s, 0x142, pending + 30, 2);
        if (mode == 11) {
            kona_command(s, 0x15e, args, 3);
            dir = g_dir_make_tmp("kona-relative-line-XXXXXX", &error);
            g_assert_no_error(error);
            uri = g_strdup_printf("unix:%s/migrate.sock", dir);
            to = qtest_initf("%s -incoming %s", MIGRATION_ARGS, uri);
            sgi_migrate_stopped(from, uri);
            s = to;
            for (unsigned i = 3; i < ARRAY_SIZE(args); i++) {
                qtest_writel(s, REG_COMMAND, args[i]);
            }
        } else {
            kona_command(s, 0x15e, args, ARRAY_SIZE(args));
        }
        for (unsigned x = 9; x <= 49; x++) {
            g_assert_cmphex(front_at(s, x, 20), ==,
                            x >= 10 && x < 48 ? 0x3c : 0);
        }
    } else {
        kona_command(s, 0x141, (uint32_t[]) { XY(10, 20) }, 1);
        if (mode == 9) {
            qtest_system_reset(s);
            load_gc(s, 0x3c);
        } else {
            kona_command(s, TOKEN_POSITION,
                         (uint32_t[]) { 100, 50, 200, 200 }, 4);
        }
        kona_command(s, 0x15d, (uint32_t[]) { XY(20, 20), XY(4, 0) }, 2);
        g_assert_cmphex(front_at(s, 10, 20), ==, 0);
        for (unsigned x = 20; x <= 24; x++) {
            g_assert_cmphex(front_at(s, x, 20), ==, 0x3c);
        }
        g_assert_cmphex(front_at(s, 30, 40), ==, 0);
    }
    assert_in_step(s);
    if (to) {
        qtest_quit(to);
        g_rmdir(dir);
    }
    qtest_quit(from);
}

static void test_relative_point_batch(gconstpointer opaque)
{
    unsigned value = GPOINTER_TO_UINT(opaque), shape = value / 16;
    unsigned mode = value % 16, count = shape ? 6 : 1;
    bool rgb = mode == 2 || mode == 3;
    unsigned gx = mode == 1 || mode == 3 || mode == 5 ? 6 :
                  mode == 8 ? 5 : 3;
    uint32_t mask = mode == 7 ? 0 : rgb ?
                    (mode == 3 ? 0x53a56c : 0xffffff) :
                    mode == 1 ? 0x53 : 0xff;
    uint32_t source = rgb ? 0x962ca5 : 0x3c;
    uint32_t dest = rgb ? 0x5a81c3 : 0xa5;
    uint32_t result = (dest & ~mask) |
                      ((gx == 6 ? source ^ dest : gx == 5 ? dest : source) &
                       mask);
    const uint32_t pending[] = { XY(12, 24), XY(5, -2), XY(-3, 4) };
    const uint32_t args[] = {
        XY(2, -3), XY(4, 0), XY(-1, 2), XY(0, -4), XY(3, 1), XY(-2, 2),
    };
    const unsigned points[][2] = {
        { 12, 24 }, { 17, 22 }, { 14, 26 }, { 16, 23 }, { 20, 23 },
        { 19, 25 }, { 19, 21 }, { 22, 22 }, { 20, 24 },
    };
    uint32_t gc[13] = { 0xa5000, 0x1ffe, 0, 0x1ffe, 0xf00fff, 0,
                        0xc, 1, 1, 6, 0, 1, 1 };
    g_autofree char *dir = NULL, *uri = NULL;
    GError *error = NULL;
    QTestState *from = qtest_init(MACHINE_ARGS), *to = NULL, *s = from;

    if (rgb) {
        semantic_rgb_gc(gc, dest, 0xffffff, 3);
    }
    kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(s, TOKEN_BOX, (uint32_t[]) { XY(8, 18), XY(32, 30) }, 2);
    if (mode == 5) {
        unsigned x = points[2 + count][0], y = points[2 + count][1];

        semantic_rgb_gc(gc, 0x123456, 0xffffff, 3);
        kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
        kona_command(s, TOKEN_BOX,
                     (uint32_t[]) { XY(x, y), XY(x + 1, y + 1) }, 2);
    }
    if (rgb) {
        semantic_rgb_gc(gc, source, mask, gx);
    } else {
        memcpy(gc, (uint32_t[]) { 0x3c000, 0x1ffe, 0, 0x1ffe,
                                 0xf0000f | (mask << 4), 0,
                                 0xc, 1, 1, gx * 2, 0, 1, 1 }, sizeof(gc));
    }
    kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
    if (mode == 4) {
        kona_command(s, TOKEN_CLIP, (uint32_t[]) { 1, 13, 22, 24, 27 }, 5);
        kona_command(s, 0x12d, (uint32_t[]) { 16, 21, 5, 5 }, 4);
        kona_command(s, 0x13d, (uint32_t[]) { 5, 4, 1 }, 3);
        qtest_writel(s, REG_COMMAND, 0x80000000);
        kona_command(s, 0x13c, (uint32_t[]) { 5 }, 1);
    }
    kona_command(s, 0x143, pending, ARRAY_SIZE(pending));
    g_assert_cmphex(front_at(s, 12, 24), ==, dest);
    if (mode == 6) {
        unsigned partial = count / 2;

        kona_command(s, shape ? 0x150 : 0x14f, args, partial);
        dir = g_dir_make_tmp("kona-relative-point-XXXXXX", &error);
        g_assert_no_error(error);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MIGRATION_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
        for (unsigned i = partial; i < count; i++) {
            qtest_writel(s, REG_COMMAND, args[i]);
        }
    } else {
        kona_command(s, shape ? 0x150 : 0x14f, args, count);
    }
    for (unsigned i = 0; i < 3 + count; i++) {
        unsigned x = points[i][0], y = points[i][1];
        bool drawn = mode != 5 &&
                     (mode != 4 || (x >= 16 && x < 21 && y >= 22 &&
                                    y < 26 && x % 4 == 0));
        uint32_t expected = mode == 5 && i == 2 + count ? 0x123456 :
                            drawn ? result : dest;

        g_assert_cmphex(front_at(s, x, y), ==, expected);
    }
    g_assert_cmphex(front_at(s, 8, 18), ==, dest);
    g_assert_cmphex(front_at(s, 2, 0), ==, 0);
    if (to) {
        qtest_quit(to);
        g_rmdir(dir);
    }
    qtest_quit(from);
}

static void test_data5_points(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    const uint32_t source = mode == 2 ? 0x96 : 0x3c;
    uint32_t gc[] = { 0x3c000, 0x1ffe, 0x96000, 0x1ffe, 0xf00fff, 0,
                      0xc, 1, 1, 6, 0, 1, 1 };
    uint32_t data[5];
    g_autofree char *dir = NULL, *uri = NULL;
    GError *error = NULL;
    QTestState *from = qtest_init(MACHINE_ARGS), *to = NULL, *s = from;

    kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
    for (unsigned k = 0; k < ARRAY_SIZE(data); k++) {
        data[k] = mode == 1 && k ? XY(2, 0) : XY(10 + 2 * k, 20);
    }
    kona_command(s, 0x146, data, mode == 3 ? 2 : ARRAY_SIZE(data));
    g_assert_cmphex(front_at(s, 10, 20), ==, 0);
    if (mode == 3) {
        dir = g_dir_make_tmp("kona-data5-XXXXXX", &error);
        g_assert_no_error(error);
        uri = g_strdup_printf("unix:%s/migrate.sock", dir);
        to = qtest_initf("%s -incoming %s", MIGRATION_ARGS, uri);
        sgi_migrate_stopped(from, uri);
        s = to;
        for (unsigned k = 2; k < ARRAY_SIZE(data); k++) {
            qtest_writel(s, REG_COMMAND, data[k]);
        }
    } else if (mode == 5) {
        kona_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
    } else if (mode == 6) {
        for (unsigned k = 0; k < 6; k++) {
            kona_command(s, 0x146, data, ARRAY_SIZE(data));
        }
    }
    if (mode == 4) {
        kona_command(s, TOKEN_BOX,
                     (uint32_t[]) { XY(30, 30), XY(32, 32) }, 2);
    } else {
        kona_command(s, mode == 1 ? 0x14f : mode == 2 ? 0x15f : 0x14c,
                     (uint32_t[]) { mode == 1 ? XY(2, 0) : XY(20, 20) }, 1);
    }
    for (unsigned k = 0; k < 5; k++) {
        g_assert_cmphex(front_at(s, 10 + 2 * k, 20), ==,
                        mode >= 4 ? 0 : source);
    }
    g_assert_cmphex(front_at(s, 20, 20), ==, mode == 4 ? 0 : source);
    g_assert_cmphex(front_at(s, 21, 20), ==, 0);
    kona_command(s, TOKEN_BOX, (uint32_t[]) { XY(30, 30), XY(32, 32) }, 2);
    assert_box(s, 30, 30, 32, 32, 0x3c);
    if (to) {
        qtest_quit(to);
        g_rmdir(dir);
    }
    qtest_quit(from);
}

static void test_relative_point_coordinates(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    QTestState *s = qtest_init(MACHINE_ARGS);
    const uint32_t points[] = {
        XY(-4, 3), XY(6, -1), XY(3, 4), XY(-7, -7), XY(10, 10), XY(-3, -1),
    };

    load_point_gc(s, 0x3c);
    if (mode == 0) {
        kona_command(s, 0x150, points, ARRAY_SIZE(points));
        g_assert_cmphex(front_at(s, 2, 2), ==, 0x3c);
        g_assert_cmphex(front_at(s, 5, 6), ==, 0x3c);
        g_assert_cmphex(front_at(s, 8, 9), ==, 0x3c);
        g_assert_cmphex(front_at(s, 5, 8), ==, 0x3c);
    } else if (mode == 1 || mode == 2) {
        uint32_t delta = mode == 1 ? XY(32767, 0) : XY(0, 32767);

        kona_command(s, 0x143, (uint32_t[]) { XY(12, 24), delta, 0 }, 3);
        kona_command(s, 0x14f, (uint32_t[]) { XY(1, 1) }, 1);
        g_assert_cmphex(front_at(s, 12, 24), ==, 0);
    } else if (mode == 3) {
        kona_command(s, 0x143,
                     (uint32_t[]) { XY(12, 24), XY(1, 1), XY(1, 1) }, 3);
        load_point_gc(s, 0x3c);
    } else if (mode == 4) {
        kona_command(s, 0x143,
                     (uint32_t[]) { XY(12, 24), XY(1, 1), XY(1, 1) }, 3);
        kona_command(s, 0x150, points, 3);
        qtest_system_reset(s);
        load_point_gc(s, 0x3c);
    } else if (mode == 5) {
        kona_command(s, 0x150,
                     (uint32_t[]) { XY(12, 24), 0, 0, 0, 0, 0 }, 6);
        g_assert_cmphex(front_at(s, 12, 24), ==, 0x3c);
    } else {
        for (unsigned i = 0; i < 11; i++) {
            kona_command(s, 0x143,
                         (uint32_t[]) { XY(12, 24), XY(1, 1), XY(1, 1) }, 3);
        }
        g_assert_cmphex(front_at(s, 12, 24), ==, 0);
    }
    kona_command(s, 0x14f, (uint32_t[]) { XY(42, 44) }, 1);
    g_assert_cmphex(front_at(s, 42, 44), ==, 0x3c);
    g_assert_cmphex(front_at(s, 43, 45), ==, 0);
    kona_command(s, TOKEN_BOX, (uint32_t[]) { XY(60, 60), XY(62, 62) }, 2);
    assert_box(s, 60, 60, 62, 62, 0x3c);
    qtest_quit(s);
}

static void test_semantic_ci_logic(gconstpointer opaque)
{
    unsigned test = GPOINTER_TO_UINT(opaque);
    unsigned gx = (test >> 8) & 15;
    bool rgb = test & 0x10000;
    uint8_t mask = test & 0xff;
    const size_t pixels = (size_t)KONA_FB_WIDTH * KONA_FB_HEIGHT;
    g_autofree uint8_t *expected_words = g_malloc0(pixels * 4);
    g_autofree uint8_t *expected_tags = g_malloc0(pixels);
    g_autofree char *word_digest = NULL, *tag_digest = NULL;
    uint32_t gc[13] = {
        0xa5000, 0x1ffe, 0, 0x1ffe, 0xf00fff, 0,
        0xc, 1, 1, 6, 0, 1, 1,
    };
    char *before[7] = { NULL };
    uint32_t source = rgb ? 0x963ca5 : 0x96;
    uint32_t dest = rgb ? 0x5a81c3 : 0xa5;
    uint32_t logical_mask = rgb ? (mask == 0xff ? 0xffffff :
                                  mask ? 0x53a56c : 0) : mask;
    uint32_t result = 0;

    for (unsigned byte = 0; byte < (rgb ? 3 : 1); byte++) {
        result |= (uint32_t)semantic_ci_result(gx, source >> (8 * byte),
                                                dest >> (8 * byte),
                                                logical_mask >> (8 * byte)) <<
                  (8 * byte);
    }

    qtest_system_reset(qts);
    g_autofree char *dg1680 = sem_digests(qts, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        if (plane != PLANE_FRONT && plane != PLANE_FRONT_TAG) {
            before[plane] = digest_at(dg1680, plane);
        }
    }
    if (rgb) {
        semantic_rgb_gc(gc, dest, 0xffffff, 3);
    }
    kona_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    gc[0] = 0x96000;
    gc[4] = 0xf0000f | ((uint32_t)mask << 4);
    gc[9] = gx * 2;
    if (rgb) {
        semantic_rgb_gc(gc, source, logical_mask, gx);
    }
    kona_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(11, 21) }, 2);

    for (unsigned y = 20; y < 22; y++) {
        for (unsigned x = 10; x < 12; x++) {
            size_t pixel = (KONA_FB_HEIGHT - 1 - y) * KONA_FB_WIDTH + x;

            uint32_t value = x == 10 && y == 20 ? result : dest;

            if (!rgb) {
                value <<= 8;
                if (x == 10 && y == 20) {
                    value |= semantic_ci_result(gx, 0, 0, 0xff);
                }
            }
            for (unsigned byte = 0; byte < 4; byte++) {
                expected_words[pixel * 4 + byte] = value >> (8 * byte);
            }
            expected_tags[pixel] = rgb ? 0 : TAG_X_INDEX;
        }
    }
    word_digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                               expected_words, pixels * 4);
    tag_digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                              expected_tags, pixels);
    g_autofree char *dg1721 = sem_digests(qts, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = digest_at(dg1721, plane);
        const char *expected = plane == PLANE_FRONT ? word_digest :
                              plane == PLANE_FRONT_TAG ? tag_digest :
                              before[plane];

        g_assert_cmpstr(actual, ==, expected);
        g_free(before[plane]);
    }
    assert_in_step(qts);
}

static void test_semantic_ci_refusal(void)
{
    uint32_t gc[13] = {
        0xa5000, 0x1ffe, 0, 0x1ffe, 0xf00fff, 0,
        0xc, 1, 1, 6, 0, 1, 1,
    };
    char *before[7];

    qtest_system_reset(qts);
    kona_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(11, 21) }, 2);
    g_autofree char *dg1744 = sem_digests(qts, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        before[plane] = digest_at(dg1744, plane);
    }
    /* The later destination has reset's incompatible direct tag. */
    gc[0] = 0x96000;
    gc[4] = 0xf0053f;
    gc[9] = 12; /* GXxor. */
    kona_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(12, 21) }, 2);
    g_autofree char *dg1753 = sem_digests(qts, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = digest_at(dg1753, plane);

        g_assert_cmpstr(actual, ==, before[plane]);
        g_free(before[plane]);
    }
    /* Refusal leaves framing ready for an ordinary full overwrite. */
    gc[4] = 0xf00fff;
    gc[9] = 6;
    kona_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(12, 21) }, 2);
    g_assert_cmphex(front_at(qts, 10, 20), ==, 0x96);
    g_assert_cmphex(front_at(qts, 11, 20), ==, 0x96);
    g_assert_cmphex(front_tag_at(qts, 11, 20), ==, TAG_X_INDEX);
    assert_in_step(qts);
}

static void test_semantic_shape_refusal(gconstpointer opaque)
{
    unsigned shape = GPOINTER_TO_UINT(opaque);
    uint32_t gc[13] = { 0xa5000, 0, 0, 0, 0xf00fff, 0,
                         0xc, 1, 1, 6, 0, 1, 1 };
    char *before[11];

    qtest_system_reset(qts);
    kona_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(11, 21) }, 2);
    g_autofree char *dg1780 = sem_digests(qts, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        before[plane] = digest_at(dg1780, plane);
    }
    gc[0] = 0x96000;
    gc[9] = 12;
    kona_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    /* A later direct-tagged destination refuses the entire shape packet. */
    if (shape == 0) {
        kona_command(qts, TOKEN_BOX6, (uint32_t[]){
            XY(10, 20), XY(11, 21), XY(30, 40), XY(31, 41),
            XY(-2, -2), XY(-1, -1), XY(-2, -2), XY(-1, -1),
            XY(-2, -2), XY(-1, -1), XY(-2, -2), XY(-1, -1),
        }, 12);
    } else if (shape == 1) {
        kona_command(qts, 0x14d, (uint32_t[]){
            XY(10, 20), XY(30, 40), XY(-2, -2),
            XY(-2, -2), XY(-2, -2), XY(-2, -2),
        }, 6);
    } else if (shape == 2) {
        kona_command(qts, 0x157,
                     (uint32_t[]){ 2, XY(10, 20), XY(30, 40), 1, 1 }, 5);
    } else {
        kona_command(qts, TOKEN_DATA2,
                     (uint32_t[]){ XY(10, 20), XY(11, 21) }, 2);
        kona_command(qts, TOKEN_BOX,
                     (uint32_t[]){ XY(30, 40), XY(31, 41) }, 2);
    }
    g_autofree char *dg1807 = sem_digests(qts, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = digest_at(dg1807, plane);

        g_assert_cmpstr(actual, ==, before[plane]);
        g_free(before[plane]);
    }
    gc[9] = 6;
    kona_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(30, 40), XY(31, 41) }, 2);
    g_assert_cmphex(kona_plane_word(qts, 0, 30, KONA_FB_HEIGHT - 1 - 40),
                    ==, 0x9600);
    assert_in_step(qts);
}

static void test_semantic_did(gconstpointer opaque)
{
    unsigned low = GPOINTER_TO_UINT(opaque);
    uint32_t gc[13] = {
        0xa5000, 0x1ffe, 0, 0x1ffe, 0xf00fff, 0,
        0xc, 1, 1, 6, 0, 1, 1,
    };
    uint32_t did[13] = { 0, 0, 0, 0, 0, 0, 8, 1, 0, 6, 0, 1, 1 };
    const uint32_t value = 0x5a96, mask = 0xa5c3;
    char *before[7];

    qtest_system_reset(qts);
    kona_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    g_autofree char *dg1835 = sem_digests(qts, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        before[plane] = digest_at(dg1835, plane);
    }
    if (low) {
        did[1] = ((value & 15) << 9) | ((value & 0xfff0) << 10);
        did[5] = 0xfff00f;
    } else {
        did[0] = ((value & 15) << 3) | ((value & 0xfff0) << 4);
        did[4] = 0xf00fff;
    }
    kona_command(qts, TOKEN_GC, did, ARRAY_SIZE(did));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    g_assert_cmphex(kona_plane_word(qts, 7, 10, KONA_FB_HEIGHT - 1 - 20), ==,
                    value);
    /* Clearing under an asymmetric raw mask is valid in either packing form. */
    did[0] = did[1] = 0;
    if (low) {
        did[5] = ((mask & 0xfff) << 12) | (mask >> 12);
    } else {
        did[4] = ((mask >> 4) & 0xfff) | ((mask & 15) << 20);
    }
    kona_command(qts, TOKEN_GC, did, ARRAY_SIZE(did));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(11, 21) }, 2);
    g_assert_cmphex(kona_plane_word(qts, 7, 10, KONA_FB_HEIGHT - 1 - 20), ==,
                    value & ~mask);
    g_assert_cmphex(kona_plane_word(qts, 7, 11, KONA_FB_HEIGHT - 1 - 20), ==,
                    value);
    g_autofree char *dg1862 = sem_digests(qts, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = digest_at(dg1862, plane);

        g_assert_cmpstr(actual, ==, before[plane]);
        g_free(before[plane]);
    }
    for (unsigned invalid = 0; invalid < 2; invalid++) {
        char *unchanged[11];
        uint32_t refused[13] = { 8, 0x200, 0, 0, 0xf00fff, 0xfff00f,
                                 8, 1, 0, 6, 0, 1, 1 };

        if (invalid) {
            refused[0] = 0x80;
            refused[1] = refused[5] = 0;
        }
        g_autofree char *dg1877 = sem_digests(qts, 0, ARRAY_SIZE(unchanged));
        for (unsigned plane = 0; plane < ARRAY_SIZE(unchanged); plane++) {
            unchanged[plane] = digest_at(dg1877, plane);
        }
        kona_command(qts, TOKEN_GC, refused, ARRAY_SIZE(refused));
        kona_command(qts, TOKEN_BOX,
                     (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
        g_autofree char *dg1883 = sem_digests(qts, 0, ARRAY_SIZE(unchanged));
        for (unsigned plane = 0; plane < ARRAY_SIZE(unchanged); plane++) {
            g_autofree char *actual = digest_at(dg1883, plane);

            g_assert_cmpstr(actual, ==, unchanged[plane]);
            g_free(unchanged[plane]);
        }
    }
    load_gc(qts, 0x2f);
    kona_command(qts, TOKEN_BOX,
                 (uint32_t[]){ XY(30, 40), XY(31, 41) }, 2);
    g_assert_cmphex(front_at(qts, 30, 40), ==, 0x2f);
    assert_in_step(qts);
}

static void test_semantic_did_map(void)
{
    uint32_t gc[13] = {
        0x5a000, 0x1ffe, 0, 0x1ffe, 0xf00fff, 0,
        0xc, 1, 1, 6, 0, 1, 1,
    };
    uint32_t did[13] = { 0x20, 0, 0, 0, 0xf00fff, 0,
                         8, 1, 0, 6, 0, 1, 1 };
    char *before[7];

    qtest_system_reset(qts);
    load_x_map(qts, X_MAP_WINDOW + 0x5a, (uint32_t[]){ ENTRY_RED }, 1);
    load_x_map(qts, 0x2e00 + 0x5a, (uint32_t[]){ ENTRY_GREY_192 }, 1);
    kona_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    g_autofree char *dg1912 = sem_digests(qts, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        before[plane] = digest_at(dg1912, plane);
    }
    kona_command(qts, TOKEN_GC, did, ARRAY_SIZE(did));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0xfe0000);
    kona_command(qts, 0x132,
                 (uint32_t[]){ 1, (0x2e00 << 7) | (7 << 3) | 4 }, 2);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0xc0c0c0);
    load_x_map(qts, 0x2e00 + 0x5a, (uint32_t[]){ 0x000003ff }, 1);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0x0000ff);
    kona_command(qts, 0x132,
                 (uint32_t[]){ 1, (0x2e00 << 7) | (11 << 3) }, 2);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0);
    kona_command(qts, 0x132,
                 (uint32_t[]){ 1, (0x7fff << 7) | (7 << 3) }, 2);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0);
    g_autofree char *dg1929 = sem_digests(qts, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = digest_at(dg1929, plane);

        g_assert_cmpstr(actual, ==, before[plane]);
        g_free(before[plane]);
    }
    assert_in_step(qts);
}

static void test_semantic_ancillary(void)
{
    uint32_t main_gc[13] = { 0x5a000, 0, 0, 0, 0xf00fff, 0,
                              0xc, 1, 1, 6, 0, 1, 1 };
    uint32_t overlay[13] = { 0xa5500 | (0xa << 3), 0, 0, 0,
                              0xf00fff, 0, 0x2c, 1, 0, 6, 0, 1, 1 };
    uint32_t popup[13] = { 3 << 3, 0, 0, 0, 0x300000, 0,
                            0x20, 1, 0, 6, 0, 1, 1 };
    char *before[7];
    unsigned row = KONA_FB_HEIGHT - 1 - 20;

    qtest_system_reset(qts);
    load_x_map(qts, X_MAP_WINDOW + 0x5a, (uint32_t[]){ ENTRY_RED }, 1);
    load_x_map(qts, 0x2e00 + 0x5a, (uint32_t[]){ ENTRY_GREY_192 }, 1);
    load_x_map(qts, 0x2f00 + 3, (uint32_t[]){ 0x3ff }, 1);
    kona_command(qts, TOKEN_GC, main_gc, ARRAY_SIZE(main_gc));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    g_autofree char *dg1955 = sem_digests(qts, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        before[plane] = digest_at(dg1955, plane);
    }
    kona_command(qts, 0x132, (uint32_t[]){ 0x40,
                 (0x2e00 << 7) | (7 << 3) }, 2);
    kona_command(qts, 0x132, (uint32_t[]){ 0x3f,
                 (0x2f00 << 7) | (1 << 3) }, 2);
    kona_command(qts, TOKEN_GC, overlay, ARRAY_SIZE(overlay));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    g_assert_cmphex(kona_plane_word(qts, 8, 10, row), ==, 0xa55a);
    g_assert_cmphex(kona_plane_word(qts, 9, 10, row), ==, 0);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0xc0c0c0);
    overlay[6] = 0x1830;
    overlay[7] = 2;
    overlay[10] = 5;
    overlay[0] = 0x12300 | (4 << 3);
    kona_command(qts, TOKEN_GC, overlay, ARRAY_SIZE(overlay));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    g_assert_cmphex(kona_plane_word(qts, 9, 10, row), ==, 0x1234);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0xc0c0c0);
    kona_command(qts, TOKEN_GC, popup, ARRAY_SIZE(popup));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(11, 21) }, 2);
    g_assert_cmphex(kona_plane_word(qts, 10, 10, row), ==, 3);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0xff);
    kona_command(qts, 0x132,
                 (uint32_t[]){ 0x3f, (0x2f00 << 7) | (7 << 3) }, 2);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0);
    kona_command(qts, 0x132,
                 (uint32_t[]){ 0x3f, (0x7fff << 7) | (1 << 3) }, 2);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0);
    kona_command(qts, 0x132,
                 (uint32_t[]){ 0x3f, (0x2f00 << 7) | (1 << 3) }, 2);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0xff);
    popup[0] = 0;
    kona_command(qts, TOKEN_GC, popup, ARRAY_SIZE(popup));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(11, 21) }, 2);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0xc0c0c0);
    kona_command(qts, 0x132,
                 (uint32_t[]){ 0x40, (0x2e00 << 7) | (1 << 3) }, 2);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0);
    kona_command(qts, 0x132,
                 (uint32_t[]){ 0x40, (0x7fff << 7) | (7 << 3) }, 2);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0);
    kona_command(qts, 0x132,
                 (uint32_t[]){ 0x40, (0x2e00 << 7) | (7 << 3) }, 2);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0xc0c0c0);
    overlay[6] = 0x2c;
    overlay[7] = 1;
    overlay[10] = 0;
    overlay[0] = 0;
    kona_command(qts, TOKEN_GC, overlay, ARRAY_SIZE(overlay));
    kona_command(qts, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(11, 21) }, 2);
    g_assert_cmphex(resolved_at(qts, 10, 20), ==, 0xfe0000);
    g_autofree char *dg2008 = sem_digests(qts, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = digest_at(dg2008, plane);

        g_assert_cmpstr(actual, ==, before[plane]);
        g_free(before[plane]);
    }
    assert_in_step(qts);
}

static void test_semantic_desktop_diagnostics(void)
{
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, &log);
    uint32_t did[13] = { 0x20, 0, 0, 0, 0xf00fff, 0,
                         8, 1, 0, 6, 0, 1, 1 };
    unsigned reports = 0;
    const char *cursor;

    load_gc(s, 0x5a);
    kona_command(s, TOKEN_BOX,
                 (uint32_t[]){ XY(0, 0), XY(1280, 1024) }, 2);
    kona_command(s, TOKEN_GC, did, ARRAY_SIZE(did));
    kona_command(s, TOKEN_BOX,
                 (uint32_t[]){ XY(0, 0), XY(1280, 1024) }, 2);
    kona_command(s, 0x132,
                 (uint32_t[]){ 1, (0x2e00 << 7) | (10 << 3) }, 2);
    for (unsigned pass = 0; pass < 2; pass++) {
        g_autofree uint8_t *pixels = kona_screendump(s);

        for (size_t i = 0; i < (size_t)1280 * 1024 * 3; i++) {
            g_assert_cmpuint(pixels[i], ==, 0);
        }
        for (unsigned x = 0; x < 64; x++) {
            g_assert_cmphex(resolved_at(s, x, 20), ==, 0);
        }
    }
    /* A changed raw entry permits one fresh diagnostic. */
    kona_command(s, 0x132,
                 (uint32_t[]){ 1, (0x2e00 << 7) | (12 << 3) }, 2);
    for (unsigned x = 0; x < 64; x++) {
        g_assert_cmphex(resolved_at(s, x, 20), ==, 0);
    }
    qtest_quit(s);
    found = kona_log_read(log);
    cursor = found;
    while ((cursor = strstr(cursor, "unsupported desktop map 1 raw="))) {
        reports++;
        cursor++;
    }
    g_assert_cmpuint(reports, ==, 2);
}

static void test_semantic_desktop_migration(gconstpointer opaque)
{
    unsigned variant = GPOINTER_TO_UINT(opaque);
    bool present = variant != 0;
    uint32_t map = variant == 4 ? 0 :
                   variant == 2 ? (0x2e00 << 7) | (11 << 3) :
                   variant == 3 ? (0x7fff << 7) | (7 << 3) :
                                  (0x2e00 << 7) | (7 << 3);
    g_autofree char *dir = NULL, *uri = NULL, *args = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint32_t did[13] = {
        0x20, 0, 0, 0, 0xf00fff, 0, 8, 1, 0, 6, 0, 1, 1,
    };
    uint32_t main_gc[13] = { 0x5a000, 0, 0, 0, 0xf00fff, 0,
                              0xc, 1, 1, 6, 0, 1, 1 };
    char *before[11];

    dir = g_dir_make_tmp("sn0-kona-desktop-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    from = qtest_init(MIGRATION_ARGS);
    args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(args);
    load_x_map(from, X_MAP_WINDOW + 0x5a, (uint32_t[]){ ENTRY_RED }, 1);
    load_x_map(from, 0x2e00 + 0x5a, (uint32_t[]){ ENTRY_GREY_192 }, 1);
    kona_command(from, TOKEN_GC, main_gc, ARRAY_SIZE(main_gc));
    kona_command(from, TOKEN_BOX,
                 (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    if (present) {
        kona_command(from, 0x132,
                     (uint32_t[]){ 1, map }, 2);
        kona_command(from, TOKEN_GC, did, ARRAY_SIZE(did));
        kona_command(from, TOKEN_BOX,
                     (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    }
    /* A reused destination has different nondefault ancillary pixels. */
    kona_command(to, 0x132,
                 (uint32_t[]){ 0, (0x7fff << 7) | (11 << 3) }, 2);
    kona_command(to, 0x132,
                 (uint32_t[]){ 1, (0x7fff << 7) | (11 << 3) }, 2);
    did[0] = 0x12300;
    kona_command(to, TOKEN_GC, did, ARRAY_SIZE(did));
    kona_command(to, TOKEN_BOX, (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    g_autofree char *dg2104 = sem_digests(from, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        before[plane] = digest_at(dg2104, plane);
    }
    sgi_migrate_stopped(from, uri);
    g_autofree char *dg2108 = sem_digests(to, 0, ARRAY_SIZE(before));
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = digest_at(dg2108, plane);

        g_assert_cmpstr(actual, ==, before[plane]);
        g_free(before[plane]);
    }
    g_assert_cmphex(resolved_at(to, 10, 20), ==,
                    variant == 1 ? 0xc0c0c0 : present ? 0 : 0xfe0000);
    qtest_system_reset(to);
    g_assert_cmphex(kona_plane_word(to, 7, 10, KONA_FB_HEIGHT - 1 - 20), ==, 0);
    assert_in_step(to);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    static const XFramingCase cases[] = {
        { "point", 0x14c, 1 }, { "point6", 0x14d, 6 },
        { "data1", 0x141, 1 }, { "data3", 0x143, 3 },
        { "point24", 0x14e, 24 }, { "relative-point", 0x14f, 1 },
        { "relative-point6", 0x150, 6 }, { "segment", 0x159, 2 },
        { "line", 0x15b, 1 }, { "line6", 0x15c, 7 },
        { "relative-line", 0x15d, 2 }, { "relative-line6", 0x15e, 7 },
        { "background-point", 0x15f, 1 },
        { "background-point6", 0x160, 6 },
        { "background-point24", 0x161, 24 },
    };
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

    qtest_add_func("sn0/origin2000/kona-fill/semantic-ancillary/composition",
                   test_semantic_ancillary);
    qtest_add_func("sn0/origin2000/kona-fill/semantic-did/map",
                   test_semantic_did_map);
    qtest_add_func("sn0/origin2000/kona-fill/semantic-desktop/diagnostics",
                   test_semantic_desktop_diagnostics);
    qtest_add_data_func("sn0/origin2000/kona-fill/semantic-desktop/"
                        "migration-installed-zero", GUINT_TO_POINTER(4),
                        test_semantic_desktop_migration);
    qtest_add_data_func("sn0/origin2000/kona-fill/semantic-desktop/migration",
                        GUINT_TO_POINTER(1), test_semantic_desktop_migration);
    qtest_add_data_func("sn0/origin2000/kona-fill/semantic-desktop/"
                        "migration-omitted",
                        GUINT_TO_POINTER(0), test_semantic_desktop_migration);
    qtest_add_data_func("sn0/origin2000/kona-fill/semantic-desktop/"
                        "migration-mode-unsupported", GUINT_TO_POINTER(2),
                        test_semantic_desktop_migration);
    qtest_add_data_func("sn0/origin2000/kona-fill/semantic-desktop/"
                        "migration-range-unsupported", GUINT_TO_POINTER(3),
                        test_semantic_desktop_migration);
    for (unsigned shape = 0; shape < 4; shape++) {
        static const char *names[] = { "boxes", "points", "spans", "run" };
        g_autofree char *name = g_strdup_printf(
            "sn0/origin2000/kona-fill/semantic-ci/refusal-%s", names[shape]);

        qtest_add_data_func(name, GUINT_TO_POINTER(shape),
                            test_semantic_shape_refusal);
    }
    qtest_add_func("sn0/origin2000/kona-fill/semantic-ci/refusal",
                   test_semantic_ci_refusal);
    qtest_add_data_func("sn0/origin2000/kona-fill/semantic-did/high",
                        GUINT_TO_POINTER(0), test_semantic_did);
    qtest_add_data_func("sn0/origin2000/kona-fill/semantic-did/low",
                        GUINT_TO_POINTER(1), test_semantic_did);
    for (unsigned gx = 0; gx < 16; gx++) {
        static const uint8_t masks[] = { 0xff, 0x53, 0 };

        for (unsigned i = 0; i < ARRAY_SIZE(masks); i++) {
            g_autofree char *name = g_strdup_printf(
                "sn0/origin2000/kona-fill/semantic-ci/gx-%u-mask-%02x",
                gx, masks[i]);

            qtest_add_data_func(name, GUINT_TO_POINTER((gx << 8) | masks[i]),
                                test_semantic_ci_logic);
            g_clear_pointer(&name, g_free);
            name = g_strdup_printf(
                "sn0/origin2000/kona-fill/semantic-rgb/gx-%u-mask-%06x",
                gx, masks[i] == 0xff ? 0xffffff : masks[i] ? 0x53a56c : 0);
            qtest_add_data_func(name,
                GUINT_TO_POINTER(0x10000 | (gx << 8) | masks[i]),
                test_semantic_ci_logic);
        }
    }
    qtest_add_func("sn0/origin2000/kona-fill/cursor-reset-partial-image",
                   test_cursor_reset_partial_image);
    qtest_add_func("sn0/origin2000/kona-fill/cursor-migration",
                   test_cursor_migration);
    qtest_add_func("sn0/origin2000/kona-fill/cursor-migration-refuses-bounds",
                   test_cursor_migration_refuses_bounds);
    qtest_add_func("sn0/origin2000/kona-fill/cursor-edges", test_cursor_edges);
    qtest_add_func("sn0/origin2000/kona-fill/cursor-selection-recolor",
                   test_cursor_selection_recolor);
    qtest_add_func("sn0/origin2000/kona-fill/cursor-refusal-abort",
                   test_cursor_refusal_abort);
    qtest_add_func("sn0/origin2000/kona-fill/loaded-cursor",
                   test_loaded_cursor);
    static const char *point_batch_shapes[] = {
        "foreground24", "background1", "background6", "background24",
    };
    static const char *point_batch_cases[] = {
        "copy", "masked-xor", "rgb-copy", "rgb-masked-xor", "clip",
        "late-incompatible", "partial-migrates", "zero-mask", "noop",
    };
    for (unsigned shape = 0; shape < ARRAY_SIZE(point_batch_shapes); shape++) {
        for (unsigned mode = 0; mode < ARRAY_SIZE(point_batch_cases); mode++) {
            g_autofree char *name = g_strdup_printf(
                "sn0/origin2000/kona-fill/point-batch/%s/%s",
                point_batch_shapes[shape], point_batch_cases[mode]);

            qtest_add_data_func(name, GUINT_TO_POINTER(shape * 16 + mode),
                                test_point_batch);
        }
    }
    for (unsigned shape = 0; shape < 2; shape++) {
        for (unsigned mode = 0; mode < ARRAY_SIZE(point_batch_cases); mode++) {
            g_autofree char *name = g_strdup_printf(
                "sn0/origin2000/kona-fill/relative-point/%s/%s",
                shape ? "six" : "one", point_batch_cases[mode]);

            qtest_add_data_func(name, GUINT_TO_POINTER(shape * 16 + mode),
                                test_relative_point_batch);
        }
    }
    for (unsigned shape = 0; shape < 5; shape++) {
        static const char *names[] = {
            "segment", "line", "line6", "relative", "relative6",
        };

        for (unsigned mode = 0; mode < ARRAY_SIZE(point_batch_cases); mode++) {
            g_autofree char *name = g_strdup_printf(
                "sn0/origin2000/kona-fill/solid-line/%s/%s",
                names[shape], point_batch_cases[mode]);

            qtest_add_data_func(name, GUINT_TO_POINTER(shape * 16 + mode),
                                test_solid_line_batch);
        }
    }
    for (unsigned octant = 0; octant < 8; octant++) {
        g_autofree char *name = g_strdup_printf(
            "sn0/origin2000/kona-fill/solid-line/octant-%u", octant);

        qtest_add_data_func(name, GUINT_TO_POINTER(octant),
                            test_solid_line_octant);
    }
    static const char *line_control_cases[] = {
        "closed-xor", "degenerate", "odd-segment", "no-anchor", "bad-gc",
        "reset", "interrupted", "signed-clip", "ties",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(line_control_cases); mode++) {
        g_autofree char *name = g_strdup_printf(
            "sn0/origin2000/kona-fill/solid-line/control/%s",
            line_control_cases[mode]);

        qtest_add_data_func(name, GUINT_TO_POINTER(mode),
                            test_solid_line_control);
    }
    static const char *relative_line_cases[] = {
        "fresh-anchor", "closed-xor", "degenerate", "signed-clip",
        "x-upper-overflow", "y-upper-overflow", "x-lower-overflow",
        "y-lower-overflow", "maximum-run", "reset", "interrupted",
        "maximum-run-migrates",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(relative_line_cases); mode++) {
        g_autofree char *name = g_strdup_printf(
            "sn0/origin2000/kona-fill/relative-line-coordinates/%s",
            relative_line_cases[mode]);

        qtest_add_data_func(name, GUINT_TO_POINTER(mode),
                            test_relative_line_coordinates);
    }
    static const char *data5_cases[] = {
        "absolute", "relative", "background", "partial-migrates",
        "odd-box", "interrupted", "overflow",
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(data5_cases); mode++) {
        g_autofree char *name = g_strdup_printf(
            "sn0/origin2000/kona-fill/data5/%s", data5_cases[mode]);

        qtest_add_data_func(name, GUINT_TO_POINTER(mode), test_data5_points);
    }
    static const char *relative_coordinate_cases[] = {
        "signed", "x-overflow", "y-overflow", "interrupted", "reset",
        "fresh-anchor", "run-overflow",
    };
    for (unsigned mode = 0;
         mode < ARRAY_SIZE(relative_coordinate_cases); mode++) {
        g_autofree char *name = g_strdup_printf(
            "sn0/origin2000/kona-fill/relative-coordinates/%s",
            relative_coordinate_cases[mode]);

        qtest_add_data_func(name, GUINT_TO_POINTER(mode),
                            test_relative_point_coordinates);
    }
    qtest_add_func("sn0/origin2000/kona-fill/absolute-points",
                   test_absolute_points);
    qtest_add_func("sn0/origin2000/kona-fill/point-clipping",
                   test_point_clipping);
    qtest_add_func("sn0/origin2000/kona-fill/point-gc-refusal",
                   test_point_gc_refusal);
    qtest_add_func("sn0/origin2000/kona-fill/point-run-discard",
                   test_point_run_discard);
    qtest_add_func("sn0/origin2000/kona-fill/single-box", test_single_box);
    qtest_add_func("sn0/origin2000/kona-fill/first-box-covers-screen",
                   test_first_box_covers_screen);
    qtest_add_func("sn0/origin2000/kona-fill/box6", test_box6);
    qtest_add_func("sn0/origin2000/kona-fill/data-run-ending-in-box",
                   test_data_run_ending_in_box);
    qtest_add_func("sn0/origin2000/kona-fill/data-run-not-ending-in-box",
                   test_data_run_not_ending_in_box);
    qtest_add_func("sn0/origin2000/kona-fill/clipping", test_clipping);
    qtest_add_func("sn0/origin2000/kona-fill/gc-index-packing",
                   test_gc_index_packing);
    qtest_add_func("sn0/origin2000/kona-fill/x-map-resolves-at-scanout",
                   test_x_map_resolves_at_scanout);
    qtest_add_func("sn0/origin2000/kona-fill/x-map-past-table",
                   test_x_map_past_table);
    qtest_add_func("sn0/origin2000/kona-fill/clip-rectangle",
                   test_clip_rectangle);
    qtest_add_func("sn0/origin2000/kona-fill/clip-list", test_clip_list);
    qtest_add_func("sn0/origin2000/kona-fill/rect-origin", test_rect_origin);
    qtest_add_func("sn0/origin2000/kona-fill/rect-runs", test_rect_runs);
    qtest_add_func("sn0/origin2000/kona-fill/reset-clears-gc-and-run",
                   test_reset_clears_gc_and_run);
    qtest_add_func("sn0/origin2000/kona-fill/"
                   "reset-clears-position-and-clip-list",
                   test_reset_clears_position_and_clip_list);
    qtest_add_func("sn0/origin2000/kona-fill/unexpected-offset-drops-box-run",
                   test_unexpected_offset_drops_box_run);
    qtest_add_func("sn0/origin2000/kona-fill/"
                   "unexpected-offset-drops-clip-list",
                   test_unexpected_offset_drops_clip_list);
    qtest_add_func("sn0/origin2000/kona-fill/unknown-token-drops-clip-list",
                   test_unknown_token_drops_clip_list);
    qtest_add_func("sn0/origin2000/kona-fill/migration-carries-gc-and-run",
                   test_migration_carries_gc_and_run);

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *name = g_strdup_printf(
            "sn0/origin2000/kona-fill/frame-%s", cases[i].name);

        qtest_add_data_func(name, &cases[i], test_point_data_framing);
    }
    qtest_add_func("sn0/origin2000/kona-fill/point-framing-migrates",
                   test_point_framing_migrates);
    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
