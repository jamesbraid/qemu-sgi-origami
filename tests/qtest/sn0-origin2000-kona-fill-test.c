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
#define TAG_X_INDEX     2

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
    return qtest_initf("%s -d guest_errors -D %s", base_args, *log);
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
        0, 0, 0, 0, 0xfc000, 8, 1, 0, 6, 0, 1, 1,
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
        0x10000, 0, 0, 0, 0, 0xfc000, 8, 1, 0, 6, 0, 1, 1,
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

/*
 * fg_h carries the colour index in the colour-index packing: the greeter
 * run's fg_h values 0x10000, 0x15000, 0x18000, 0x1d000 and 0x2f000 fill
 * with indices 0x10, 0x15, 0x18, 0x1d and 0x2f. Bits 6:3 of fg_h, where
 * the packing puts the low nibble of index << 8, are always zero at depth
 * 8; setting them must not change the index.
 */
static void test_gc_index_packing(void)
{
    static const uint32_t fg_h[] = { 0x10000, 0x15000, 0x18000, 0x1d000,
                                     0x2f000, 0x2f078 };
    static const uint32_t index[] = { 0x10, 0x15, 0x18, 0x1d, 0x2f, 0x2f };

    qtest_system_reset(qts);
    for (size_t i = 0; i < ARRAY_SIZE(fg_h); i++) {
        qtest_writel(qts, REG_COMMAND_SLOT(TOKEN_GC), fg_h[i]);
        for (unsigned w = 1; w < 13; w++) {
            qtest_writel(qts, REG_COMMAND_DATA, 0);
        }
        kona_command(qts, TOKEN_BOX,
                     (uint32_t[]){ XY(10 * i, 0), XY(10 * i + 2, 2) }, 2);
        assert_box(qts, 10 * i, 0, 10 * i + 2, 2, index[i]);
    }
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
 * three entries from 0x3fff put the first in the table's last entry.
 */
static void test_x_map_past_table(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, &log);

    load_x_map(s, 0x3fff, (uint32_t[]){ 1, 9, 0 }, 3);
    assert_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "colour map load at 0x3fff count 3: 2 "
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

/*
 * A stream from before the X colour map (x-server subsection version 1):
 * the GC and a waiting run cross, and the colour map starts empty. The
 * source's fg_h 0x13000 fills with index 0x13 on the destination.
 * Skipped unless QTEST_QEMU_BINARY_KONA_X_SERVER_V1 names a
 * qemu-system-mips64 built with that subsection at version 1.
 */
static void test_migration_from_x_server_v1(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    if (!g_getenv("QTEST_QEMU_BINARY_KONA_X_SERVER_V1")) {
        g_test_skip("QTEST_QEMU_BINARY_KONA_X_SERVER_V1 is not set");
        return;
    }

    tmpfs = g_dir_make_tmp("sn0-kona-x-server-v1-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init_ext("QTEST_QEMU_BINARY_KONA_X_SERVER_V1",
                          MIGRATION_ARGS, NULL, true);
    load_gc(from, 0x13);
    kona_command(from, TOKEN_DATA2, (uint32_t[]){ XY(10, 10), XY(12, 12) },
                 2);

    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(kona_gc(to, 0), ==, 0x13000);
    kona_command(to, TOKEN_BOX, (uint32_t[]){ XY(30, 10), XY(32, 12) }, 2);
    assert_box(to, 10, 10, 12, 12, 0x13);
    assert_box(to, 30, 10, 32, 12, 0x13);
    g_assert_cmphex(resolved_at(to, 11, 11), ==, 0);
    assert_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
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
        uint32_t args[6] = { 0x1a };

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
        g_assert_cmphex(front_at(s, 0, 26), ==, 0);
        assert_in_step(s);
        qtest_quit(s);
        found = kona_log_read(log);
        packet = g_strdup_printf("sgi_kona_command_unmodelled token 0x%x"
                                  " argc %u consumed", c->token, c->words);
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
    load_gc(from, 0x55);
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
    kona_command(to, TOKEN_BOX,
                 (uint32_t[]){ XY(10, 20), XY(12, 22) }, 2);
    assert_box(to, 10, 20, 12, 22, 0x55);

    qtest_writel(to, REG_COMMAND, 0x14d);
    qtest_writel(to, REG_COMMAND, 0x1a);
    qtest_system_reset(to);
    g_assert_cmphex(qtest_readl(to, KONA_REG(0x800208)), ==, 0);
    load_gc(to, 0x55);
    kona_command(to, TOKEN_BOX,
                 (uint32_t[]){ XY(30, 40), XY(32, 42) }, 2);
    assert_box(to, 30, 40, 32, 42, 0x55);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    static const XFramingCase cases[] = {
        { "point", 0x14c, 1 }, { "point6", 0x14d, 6 },
        { "data1", 0x141, 1 }, { "data3", 0x143, 3 },
    };
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

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
    qtest_add_func("sn0/origin2000/kona-fill/migration-from-x-server-v1",
                   test_migration_from_x_server_v1);

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
