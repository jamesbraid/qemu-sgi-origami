/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA qtests for the X server's pixel writes: X pixel setup 0x136, X
 * pixel write 0x139 with its row data, X pixel finish 0x13a and X pixel copy
 * 0x137, as the X server sends them. The setup is seven words: mode, source
 * packing, destination packing, shift, width, height and left pad in bits. A
 * write is x, then y, then height rows of ceil((pad + bpp * width) / 32)
 * big-endian words.
 *
 * Bits per pixel from the source-packing word (0, 1, 2 for 8, 16, 32, and
 * 4 for 1-bit glyph rows) is inferred; the model draws 8-bit pixels as X
 * colour-map indices, 32-bit ones as direct colour and keeps a 16-bit
 * pixel's low byte as an index. What 1-bit rows paint is
 * sn0-origin2000-kona-glyph-test.c's subject; here they are checked for
 * framing only. These cases check that rule and the framing, not the
 * hardware. y counts down from the top display line, as for boxes.
 *
 * The fixture is sn0-origin2000-kona-fill-test.c's: KONA as a -device at
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
#define TOKEN_COLOR_MAP     0x12e
#define TOKEN_PIXEL_SETUP   0x136
#define TOKEN_PIXEL_COPY    0x137
#define TOKEN_PIXEL_WRITE   0x139
#define TOKEN_PIXEL_FINISH  0x13a

/* Repeated as literals: a test that borrowed the model's could not fail. */
#define KONA_FB_WIDTH  1280
#define KONA_FB_HEIGHT 1024

#define PLANE_FRONT     0
#define PLANE_FRONT_TAG 2
#define TAG_DIRECT      0
#define TAG_X_INDEX     2

#define X_MAP_WINDOW    0x2d00
#define ENTRY_GREY_192  0x304c1304

#define KONA_QOM_PATH "/machine/module0/kona0"

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4," \
    "fabric-hub-widget=9"

#define MIGRATION_ARGS MACHINE_ARGS

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

static QTestState *kona_init_logged(const char *base_args, char **log)
{
    int fd;

    *log = g_strdup_printf("%s/sn0-kona-pixel-XXXXXX.log", g_get_tmp_dir());
    fd = g_mkstemp(*log);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return qtest_initf("%s -trace enable=sgi_kona_x_pixel* -d guest_errors,"
                       "unimp -D %s", base_args, *log);
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
    g_autofree char *dir = g_dir_make_tmp("sn0-kona-pixel-XXXXXX", &err);
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

static void pixel_setup(QTestState *s, uint32_t mode, uint32_t src_pack,
                        uint32_t width, uint32_t height, uint32_t pad)
{
    x_command(s, TOKEN_PIXEL_SETUP,
              (uint32_t[]){ mode, src_pack, 1, 8, width, height, pad }, 7);
}

static void pixel_write(QTestState *s, int32_t x, int32_t y)
{
    x_command(s, TOKEN_PIXEL_WRITE, (uint32_t[]){ x, y }, 2);
}

static void pixel_finish(QTestState *s)
{
    x_command(s, TOKEN_PIXEL_FINISH, (uint32_t[]){ 0 }, 1);
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

/* The weave's rows: one index-7 pixel per row, at x = 3, 1, 2, 0. */
static const uint32_t weave_words[] = { 0x7, 0x70000, 0x700, 0x7000000 };
static const unsigned weave_column[] = { 3, 1, 2, 0 };

/* The 4 by 4 weave drawn with its top-left corner at (x0, y0). */
static void assert_weave(QTestState *s, unsigned x0, unsigned y0)
{
    for (unsigned r = 0; r < 4; r++) {
        for (unsigned c = 0; c < 4; c++) {
            uint32_t want = c == weave_column[r] ? 7 : 0;

            g_assert_cmphex(front_at(s, x0 + c, y0 + r), ==, want);
        }
        g_assert_cmphex(front_tag_at(s, x0 + weave_column[r], y0 + r), ==,
                        TAG_X_INDEX);
    }
}

/*
 * The X server's image write from a traced guest run, in
 * konDrawImage's order: clip, position, GC, setup 0 with 0, 1, 8, 4, 4, 0,
 * write at (0, 0), the four words 0x7, 0x70000, 0x700, 0x7000000, finish.
 * Big-endian rows put index 7 at x = 3, 1, 2, 0 of rows 0 to 3; a
 * little-endian reading would put it at 0, 2, 1, 3. The pixels are X
 * pixels, so with X colour-map entry 0x2d07 grey they show grey and the
 * rest of the tile, index 0 with entry 0x2d00 zero, black.
 */
static void test_weave(void)
{
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    x_command(qts, TOKEN_COLOR_MAP,
              (uint32_t[]){ X_MAP_WINDOW + 7, 1, ENTRY_GREY_192 }, 3);
    x_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 0, 0, 1280, 1024 }, 5);
    x_command(qts, TOKEN_POSITION, (uint32_t[]){ 0, 0, 1280, 1024 }, 4);
    x_command(qts, TOKEN_GC, (uint32_t[]){ 0, 0, 0, 0, 0xf00fff, 0, 0x180c,
                                           0x40001, 1, 6, 5, 1, 1 }, 13);
    x_command(qts, TOKEN_PIXEL_SETUP, (uint32_t[]){ 0, 0, 1, 8, 4, 4, 0 }, 7);
    pixel_write(qts, 0, 0);
    data_words(qts, weave_words, 4);
    pixel_finish(qts);

    assert_weave(qts, 0, 0);
    frame = kona_screendump(qts);
    for (unsigned r = 0; r < 4; r++) {
        for (unsigned c = 0; c < 4; c++) {
            g_assert_cmphex(ppm_pixel(frame, c, r), ==,
                            c == weave_column[r] ? 0xc0c0c0 : 0);
        }
    }
    assert_in_step(qts);
}

/*
 * A 32-bit write, source packing 2: two rows of three words at (100, 200)
 * land as direct colour, the top byte kept in the plane and cleared on
 * the console.
 */
static void test_write_32bit(void)
{
    static const uint32_t words[] = {
        0x00112233, 0xff445566, 0x00778899,
        0x00aabbcc, 0x00ddeeff, 0x00010203,
    };
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    pixel_setup(qts, 0, 2, 3, 2, 0);
    pixel_write(qts, 100, 200);
    data_words(qts, words, 6);
    pixel_finish(qts);

    for (unsigned i = 0; i < 6; i++) {
        g_assert_cmphex(front_at(qts, 100 + i % 3, 200 + i / 3), ==,
                        words[i]);
        g_assert_cmphex(front_tag_at(qts, 100 + i % 3, 200 + i / 3), ==,
                        TAG_DIRECT);
    }
    g_assert_cmphex(front_at(qts, 103, 200), ==, 0);
    frame = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(frame, 101, 200), ==, 0x445566);
    g_assert_cmphex(ppm_pixel(frame, 102, 201), ==, 0x010203);
    assert_in_step(qts);
}

/*
 * A 16-bit write, source packing 1: three pixels a row take two words,
 * the second's low half padding. Each pixel keeps its low byte as an X
 * index. Two rows, so a model that took one word a row would read the
 * second row's first word as padding and misplace it.
 */
static void test_write_16bit(void)
{
    static const uint32_t words[] = {
        0xab12cd34, 0xef56dead, 0x01a102a2, 0x03a3beef,
    };
    static const uint32_t want[2][3] = {
        { 0x12, 0x34, 0x56 }, { 0xa1, 0xa2, 0xa3 },
    };

    qtest_system_reset(qts);
    pixel_setup(qts, 0, 1, 3, 2, 0);
    pixel_write(qts, 10, 20);
    data_words(qts, words, 4);
    pixel_finish(qts);

    for (unsigned r = 0; r < 2; r++) {
        for (unsigned c = 0; c < 3; c++) {
            g_assert_cmphex(front_at(qts, 10 + c, 20 + r), ==, want[r][c]);
            g_assert_cmphex(front_tag_at(qts, 10 + c, 20 + r), ==,
                            TAG_X_INDEX);
        }
        g_assert_cmphex(front_at(qts, 13, 20 + r), ==, 0);
    }
    assert_in_step(qts);
}

/*
 * A nonzero left pad skips that many bits at the start of every row. Pad
 * 8, width 4: each row is two words, 0xff then the four pixels. Pad 12,
 * width 3: the third pixel straddles the row's two words, its high
 * nibble in the first word's low nibble.
 */
static void test_write_left_pad(void)
{
    qtest_system_reset(qts);
    pixel_setup(qts, 0, 0, 4, 2, 8);
    pixel_write(qts, 50, 60);
    data_words(qts, (uint32_t[]){ 0xff010203, 0x04eeeeee,
                                  0xff111213, 0x14eeeeee }, 4);
    pixel_finish(qts);
    for (unsigned c = 0; c < 4; c++) {
        g_assert_cmphex(front_at(qts, 50 + c, 60), ==, 1 + c);
        g_assert_cmphex(front_at(qts, 50 + c, 61), ==, 0x11 + c);
    }
    g_assert_cmphex(front_at(qts, 54, 60), ==, 0);

    pixel_setup(qts, 0, 0, 3, 1, 12);
    pixel_write(qts, 70, 80);
    data_words(qts, (uint32_t[]){ 0xfffa1b2c, 0x3fffffff }, 2);
    pixel_finish(qts);
    g_assert_cmphex(front_at(qts, 70, 80), ==, 0xa1);
    g_assert_cmphex(front_at(qts, 71, 80), ==, 0xb2);
    g_assert_cmphex(front_at(qts, 72, 80), ==, 0xc3);
    g_assert_cmphex(front_at(qts, 73, 80), ==, 0);
    assert_in_step(qts);
}

/*
 * Source packing 4, the greeter's glyph writes: 1-bit rows, so the run's
 * 32 by 20 label takes 20 words, and width 40 takes two words a row. The
 * words after the first are shaped like the sync probe to show they are
 * consumed as data. The reset GC's word 1 is 0, so set bits paint index
 * 0, which reads back as the reset plane's 0.
 */
static void test_write_packing4_framed(void)
{
    static const uint32_t probe[] = { 1, 9, 0 };
    uint32_t words[20];

    qtest_system_reset(qts);
    words[0] = 0x80800000;
    for (unsigned i = 1; i < ARRAY_SIZE(words); i++) {
        words[i] = probe[i % 3];
    }
    pixel_setup(qts, 0, 4, 32, 20, 0);
    pixel_write(qts, 260, 304);
    data_words(qts, words, 20);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);
    pixel_finish(qts);
    assert_in_step(qts);

    pixel_setup(qts, 0, 4, 40, 3, 0);
    pixel_write(qts, 30, 30);
    data_words(qts, words, 6);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE), ==, 0);
    pixel_finish(qts);
    g_assert_cmphex(front_at(qts, 30, 30), ==, 0);
    g_assert_cmphex(front_at(qts, 260, 304), ==, 0);
    assert_in_step(qts);
}

/*
 * With the setup's mode bit 0x10 the rows go by ioctl 0x2711, and the
 * write is followed straight by its finish: no word is taken as data and
 * nothing is drawn. The same setup with mode 0 takes the weave's four
 * words and draws it.
 */
static void test_write_dma_mode_has_no_data(void)
{
    qtest_system_reset(qts);
    pixel_setup(qts, 0x10, 0, 4, 4, 0);
    pixel_write(qts, 0, 0);
    pixel_finish(qts);
    assert_in_step(qts);
    g_assert_cmphex(front_at(qts, 3, 0), ==, 0);

    pixel_setup(qts, 0, 0, 4, 4, 0);
    pixel_write(qts, 8, 0);
    data_words(qts, weave_words, 4);
    pixel_finish(qts);
    assert_weave(qts, 8, 0);
    assert_in_step(qts);
}

/*
 * A write is clipped to the screen and to the X clip: one starting left
 * of the screen keeps its on-screen pixels, under a clip of
 * (201, 0)-(203, 1024) only columns 201 and 202 of a four-pixel row land,
 * and under a two-rectangle clip list only the columns in either land.
 */
static void test_write_clipped(void)
{
    qtest_system_reset(qts);
    pixel_setup(qts, 0, 0, 4, 1, 0);
    pixel_write(qts, -2, 5);
    data_words(qts, (uint32_t[]){ 0x01020304 }, 1);
    pixel_finish(qts);
    g_assert_cmphex(front_at(qts, 0, 5), ==, 3);
    g_assert_cmphex(front_at(qts, 1, 5), ==, 4);
    g_assert_cmphex(front_at(qts, 2, 5), ==, 0);
    g_assert_cmphex(front_at(qts, 1279, 5), ==, 0);

    x_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 201, 0, 203, 1024 }, 5);
    pixel_write(qts, 200, 6);
    data_words(qts, (uint32_t[]){ 0x05060708 }, 1);
    pixel_finish(qts);
    g_assert_cmphex(front_at(qts, 200, 6), ==, 0);
    g_assert_cmphex(front_at(qts, 201, 6), ==, 6);
    g_assert_cmphex(front_at(qts, 202, 6), ==, 7);
    g_assert_cmphex(front_at(qts, 203, 6), ==, 0);

    x_command(qts, 0x145, (uint32_t[]){ 2, 300, 0, 301 }, 4);
    x_command(qts, TOKEN_CLIP, (uint32_t[]){ 1024, 303, 0, 304, 1024 }, 5);
    pixel_write(qts, 300, 7);
    data_words(qts, (uint32_t[]){ 0x090a0b0c }, 1);
    pixel_finish(qts);
    g_assert_cmphex(front_at(qts, 300, 7), ==, 9);
    g_assert_cmphex(front_at(qts, 301, 7), ==, 0);
    g_assert_cmphex(front_at(qts, 302, 7), ==, 0);
    g_assert_cmphex(front_at(qts, 303, 7), ==, 0xc);
    assert_in_step(qts);
}

/*
 * konCopyRect's group: position and clip over the screen, the GC the run
 * sent, setup 1 with 1, 1, 0 and the size, then the copy.
 */
static void copy_rect(QTestState *s, int32_t sx, int32_t sy, int32_t dx,
                      int32_t dy, uint32_t w, uint32_t h)
{
    x_command(s, TOKEN_POSITION, (uint32_t[]){ 0, 0, 1280, 1024 }, 4);
    x_command(s, TOKEN_CLIP, (uint32_t[]){ 1, 0, 0, 1280, 1024 }, 5);
    x_command(s, TOKEN_GC, (uint32_t[]){ 0, 0, 0, 0, 0xf00fff, 0, 0x180c,
                                         0x40001, 1, 6, 5, 1, 1 }, 13);
    x_command(s, TOKEN_PIXEL_SETUP, (uint32_t[]){ 1, 1, 1, 0, w, h, 0 }, 7);
    x_command(s, TOKEN_PIXEL_COPY, (uint32_t[]){ sx, sy, dx, dy }, 4);
}

/*
 * The run's drawing: the weave at (0, 0), then 17 copies. Nine double it
 * along the top four rows, widths 4 to 512 from (0, 0) to (w, 0) and 256
 * more to (1024, 0); eight double those rows down the screen, heights 4
 * to 512 at width 1280 from (0, 0) to (0, h). The weave then repeats
 * every four pixels over the whole screen. Tiles are spot-checked across
 * the screen, including the last columns and rows, through the plane and
 * the resolved frame. Reading the wire's first pair as the destination
 * would copy blank pixels over the weave.
 */
static void test_copy_doubling_tiles_screen(void)
{
    static const unsigned tiles[][2] = {
        { 0, 0 }, { 4, 0 }, { 508, 0 }, { 1024, 0 }, { 1276, 0 },
        { 0, 4 }, { 640, 512 }, { 1276, 1020 }, { 0, 1020 }, { 100, 700 },
    };
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    x_command(qts, TOKEN_COLOR_MAP,
              (uint32_t[]){ X_MAP_WINDOW + 7, 1, ENTRY_GREY_192 }, 3);
    pixel_setup(qts, 0, 0, 4, 4, 0);
    pixel_write(qts, 0, 0);
    data_words(qts, weave_words, 4);
    pixel_finish(qts);
    for (uint32_t w = 4; w <= 512; w *= 2) {
        copy_rect(qts, 0, 0, w, 0, w, 4);
    }
    copy_rect(qts, 0, 0, 1024, 0, 256, 4);
    for (uint32_t h = 4; h <= 512; h *= 2) {
        copy_rect(qts, 0, 0, 0, h, 1280, h);
    }

    for (size_t t = 0; t < ARRAY_SIZE(tiles); t++) {
        assert_weave(qts, tiles[t][0], tiles[t][1]);
    }
    frame = kona_screendump(qts);
    for (size_t t = 0; t < ARRAY_SIZE(tiles); t++) {
        for (unsigned r = 0; r < 4; r++) {
            for (unsigned c = 0; c < 4; c++) {
                g_assert_cmphex(ppm_pixel(frame, tiles[t][0] + c,
                                          tiles[t][1] + r), ==,
                                c == weave_column[r] ? 0xc0c0c0 : 0);
            }
        }
    }
    assert_in_step(qts);
}

/*
 * Overlapping copies read every source pixel before overwriting it: a
 * row 1, 2, 3, 4 copied one pixel right becomes 1, 1, 2, 3, 4, and one
 * pixel left 2, 3, 4, 4; a column copied one pixel down and up does the
 * same. A copy walking the wrong way would smear the first pixel along.
 */
static void test_copy_overlapping(void)
{
    static const uint32_t column[] = {
        0x01000000, 0x02000000, 0x03000000, 0x04000000,
    };

    qtest_system_reset(qts);
    pixel_setup(qts, 0, 0, 4, 1, 0);
    pixel_write(qts, 100, 100);
    data_words(qts, (uint32_t[]){ 0x01020304 }, 1);
    pixel_finish(qts);
    copy_rect(qts, 100, 100, 101, 100, 4, 1);
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmphex(front_at(qts, 100 + i, 100), ==, i ? i : 1);
    }
    copy_rect(qts, 101, 100, 100, 100, 4, 1);
    g_assert_cmphex(front_at(qts, 100, 100), ==, 1);
    g_assert_cmphex(front_at(qts, 101, 100), ==, 2);
    g_assert_cmphex(front_at(qts, 102, 100), ==, 3);
    g_assert_cmphex(front_at(qts, 103, 100), ==, 4);
    g_assert_cmphex(front_at(qts, 104, 100), ==, 4);

    pixel_setup(qts, 0, 0, 1, 4, 0);
    pixel_write(qts, 200, 200);
    data_words(qts, column, 4);
    pixel_finish(qts);
    copy_rect(qts, 200, 200, 200, 201, 1, 4);
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmphex(front_at(qts, 200, 200 + i), ==, i ? i : 1);
    }
    copy_rect(qts, 200, 201, 200, 200, 1, 4);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(front_at(qts, 200, 200 + i), ==, i + 1);
    }
    g_assert_cmphex(front_at(qts, 200, 204), ==, 4);
    assert_in_step(qts);
}

/*
 * A copy keeps each pixel's tag with its word: a textport pixel still
 * resolves through the textport colour map, a direct pixel stays direct
 * and an X pixel stays an X colour-map index.
 */
static void test_copy_keeps_tags(void)
{
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    /* Textport colour map entry 5 is 0x102030; a point in it at (300, 10). */
    qtest_writel(qts, REG_COMMAND, 1);
    data_words(qts, (uint32_t[]){ 3, 5, 0x10 << 2, 0x20 << 2, 0x30 << 2 }, 5);
    qtest_writel(qts, REG_COMMAND, 1);
    data_words(qts, (uint32_t[]){ 2, 5 }, 2);
    qtest_writel(qts, REG_COMMAND, 1);
    data_words(qts, (uint32_t[]){ 6, 300, KONA_FB_HEIGHT - 1 - 10 }, 3);
    pixel_setup(qts, 0, 2, 1, 1, 0);
    pixel_write(qts, 301, 10);
    data_words(qts, (uint32_t[]){ 0x00abcdef }, 1);
    pixel_finish(qts);
    x_command(qts, TOKEN_COLOR_MAP,
              (uint32_t[]){ X_MAP_WINDOW + 5, 1, ENTRY_GREY_192 }, 3);
    pixel_setup(qts, 0, 0, 1, 1, 0);
    pixel_write(qts, 302, 10);
    data_words(qts, (uint32_t[]){ 0x05000000 }, 1);
    pixel_finish(qts);

    copy_rect(qts, 300, 10, 400, 20, 3, 1);
    g_assert_cmphex(front_tag_at(qts, 400, 20), ==, 1);
    g_assert_cmphex(front_at(qts, 400, 20), ==, 5);
    g_assert_cmphex(front_tag_at(qts, 401, 20), ==, TAG_DIRECT);
    g_assert_cmphex(front_at(qts, 401, 20), ==, 0x00abcdef);
    g_assert_cmphex(front_tag_at(qts, 402, 20), ==, TAG_X_INDEX);
    g_assert_cmphex(front_at(qts, 402, 20), ==, 5);
    frame = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(frame, 400, 20), ==, 0x102030);
    g_assert_cmphex(ppm_pixel(frame, 401, 20), ==, 0xabcdef);
    g_assert_cmphex(ppm_pixel(frame, 402, 20), ==, 0xc0c0c0);
    assert_in_step(qts);
}

/*
 * A copy is clipped: to the screen at the destination, so one running
 * off the right edge keeps its on-screen part and does not wrap; to the
 * source, so pixels from past the screen's edge are not copied; and to
 * the clip list at the destination.
 */
static void test_copy_clipped(void)
{
    qtest_system_reset(qts);
    pixel_setup(qts, 0, 0, 4, 1, 0);
    pixel_write(qts, 10, 10);
    data_words(qts, (uint32_t[]){ 0x01020304 }, 1);
    pixel_finish(qts);

    copy_rect(qts, 10, 10, 1278, 10, 4, 1);
    g_assert_cmphex(front_at(qts, 1278, 10), ==, 1);
    g_assert_cmphex(front_at(qts, 1279, 10), ==, 2);
    g_assert_cmphex(front_at(qts, 0, 11), ==, 0);

    pixel_setup(qts, 0, 0, 4, 1, 0);
    pixel_write(qts, 0, 12);
    data_words(qts, (uint32_t[]){ 0x05060708 }, 1);
    pixel_finish(qts);
    pixel_write(qts, 20, 20);
    data_words(qts, (uint32_t[]){ 0x09090909 }, 1);
    pixel_finish(qts);
    copy_rect(qts, -2, 12, 20, 20, 4, 1);
    g_assert_cmphex(front_at(qts, 20, 20), ==, 9);
    g_assert_cmphex(front_at(qts, 21, 20), ==, 9);
    g_assert_cmphex(front_at(qts, 22, 20), ==, 5);
    g_assert_cmphex(front_at(qts, 23, 20), ==, 6);

    x_command(qts, TOKEN_POSITION, (uint32_t[]){ 0, 0, 1280, 1024 }, 4);
    x_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 31, 30, 33, 31 }, 5);
    x_command(qts, TOKEN_PIXEL_SETUP, (uint32_t[]){ 1, 1, 1, 0, 4, 1, 0 }, 7);
    x_command(qts, TOKEN_PIXEL_COPY, (uint32_t[]){ 10, 10, 30, 30 }, 4);
    g_assert_cmphex(front_at(qts, 30, 30), ==, 0);
    g_assert_cmphex(front_at(qts, 31, 30), ==, 2);
    g_assert_cmphex(front_at(qts, 32, 30), ==, 3);
    g_assert_cmphex(front_at(qts, 33, 30), ==, 0);
    assert_in_step(qts);
}

/*
 * X pixel finish with no write open is traced and logged and changes
 * nothing; after a write it is traced as closing it.
 */
static void test_finish_without_write(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, &log);

    pixel_finish(s);
    assert_in_step(s);
    pixel_setup(s, 0, 0, 0, 0, 0);
    pixel_write(s, 0, 0);
    pixel_finish(s);
    assert_in_step(s);
    qtest_quit(s);

    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "sgi_kona_x_pixel_finish open=0"));
    g_assert_nonnull(strstr(found, "sgi_kona_x_pixel_finish open=1"));
    g_assert_nonnull(strstr(found, "X pixel finish with no pixel write "
                                   "open"));
}

/*
 * Reset drops a write still taking its data: the sync probe after it
 * frames at once, and the setup is cleared, so a write with no new setup
 * takes no data. A write after a fresh setup draws.
 */
static void test_reset_clears_write(void)
{
    qtest_system_reset(qts);
    pixel_setup(qts, 0, 0, 4, 4, 0);
    pixel_write(qts, 0, 0);
    data_words(qts, weave_words, 1);
    qtest_system_reset(qts);
    assert_in_step(qts);
    pixel_write(qts, 0, 0);
    assert_in_step(qts);
    g_assert_cmphex(front_at(qts, 3, 0), ==, 0);

    pixel_setup(qts, 0, 0, 4, 1, 0);
    pixel_write(qts, 0, 0);
    data_words(qts, (uint32_t[]){ 0x01020304 }, 1);
    pixel_finish(qts);
    g_assert_cmphex(front_at(qts, 0, 0), ==, 1);
    g_assert_cmphex(front_at(qts, 3, 0), ==, 4);
    assert_in_step(qts);
}

/*
 * A write caught mid-row by a migration completes on the destination:
 * with pad 12 and width 3 the third pixel straddles the row's two words,
 * so the first word must cross with the rest of the write.
 */
static void test_migration_mid_write(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-pixel-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    pixel_setup(from, 0, 0, 3, 2, 12);
    pixel_write(from, 70, 80);
    data_words(from, (uint32_t[]){ 0xfffa1b2c }, 1);

    sgi_migrate_stopped(from, uri);

    data_words(to, (uint32_t[]){ 0x3fffffff, 0xfff0a0b0, 0xc0ffffff }, 3);
    pixel_finish(to);
    g_assert_cmphex(front_at(to, 70, 80), ==, 0xa1);
    g_assert_cmphex(front_at(to, 71, 80), ==, 0xb2);
    g_assert_cmphex(front_at(to, 72, 80), ==, 0xc3);
    g_assert_cmphex(front_at(to, 70, 81), ==, 0x0a);
    g_assert_cmphex(front_at(to, 71, 81), ==, 0x0b);
    g_assert_cmphex(front_at(to, 72, 81), ==, 0x0c);
    assert_in_step(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

    qtest_add_func("sn0/origin2000/kona-pixel/weave", test_weave);
    qtest_add_func("sn0/origin2000/kona-pixel/write-32bit", test_write_32bit);
    qtest_add_func("sn0/origin2000/kona-pixel/write-16bit", test_write_16bit);
    qtest_add_func("sn0/origin2000/kona-pixel/write-left-pad",
                   test_write_left_pad);
    qtest_add_func("sn0/origin2000/kona-pixel/write-packing4-framed",
                   test_write_packing4_framed);
    qtest_add_func("sn0/origin2000/kona-pixel/write-dma-mode-has-no-data",
                   test_write_dma_mode_has_no_data);
    qtest_add_func("sn0/origin2000/kona-pixel/write-clipped",
                   test_write_clipped);
    qtest_add_func("sn0/origin2000/kona-pixel/copy-doubling-tiles-screen",
                   test_copy_doubling_tiles_screen);
    qtest_add_func("sn0/origin2000/kona-pixel/copy-overlapping",
                   test_copy_overlapping);
    qtest_add_func("sn0/origin2000/kona-pixel/copy-keeps-tags",
                   test_copy_keeps_tags);
    qtest_add_func("sn0/origin2000/kona-pixel/copy-clipped",
                   test_copy_clipped);
    qtest_add_func("sn0/origin2000/kona-pixel/finish-without-write",
                   test_finish_without_write);
    qtest_add_func("sn0/origin2000/kona-pixel/reset-clears-write",
                   test_reset_clears_write);
    qtest_add_func("sn0/origin2000/kona-pixel/migration-mid-write",
                   test_migration_mid_write);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
