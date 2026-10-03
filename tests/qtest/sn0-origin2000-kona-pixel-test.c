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

static char *copy_plane_digest(QTestState *s, unsigned plane)
{
    QDict *reply;
    char *digest;

    kona_qom_set_uint(s, "x-test-plane", plane);
    reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'x-test-plane-digest'}}", KONA_QOM_PATH);
    digest = g_strdup(qdict_get_str(reply, "return"));
    qobject_unref(reply);
    return digest;
}

static QTestState *kona_init_logged(const char *base_args, char **log)
{
    int fd;

    *log = g_strdup_printf("%s/sn0-kona-pixel-XXXXXX.log", g_get_tmp_dir());
    fd = g_mkstemp(*log);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return qtest_initf("%s -trace enable=sgi_kona_* -d guest_errors,"
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
 * Mixed source classes refuse before stores. Separate ordinary CI and RGB
 * copies preserve their canonical tags; textport transport stays unsupported.
 */
static void test_copy_refuses_mixed_tags(void)
{
    g_autofree uint8_t *frame = NULL;
    char *before[11];

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

    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        before[plane] = copy_plane_digest(qts, plane);
    }
    copy_rect(qts, 300, 10, 400, 20, 3, 1);
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = copy_plane_digest(qts, plane);

        g_assert_cmpstr(actual, ==, before[plane]);
        g_free(before[plane]);
    }
    copy_rect(qts, 302, 10, 402, 20, 1, 1);
    x_command(qts, TOKEN_GC,
              (uint32_t[]){ 0, 0x1ffe, 0, 0x1ffe, 0xffffff, 0xffffff,
                            0x2804, 0x60001, 1, 6, 8, 0, 0 }, 13);
    x_command(qts, TOKEN_PIXEL_SETUP,
              (uint32_t[]){ 1, 2, 2, 0, 1, 1, 0 }, 7);
    x_command(qts, TOKEN_PIXEL_COPY, (uint32_t[]){ 301, 10, 401, 20 }, 4);
    g_assert_cmphex(front_tag_at(qts, 400, 20), ==, TAG_DIRECT);
    g_assert_cmphex(front_at(qts, 400, 20), ==, 0);
    g_assert_cmphex(front_tag_at(qts, 401, 20), ==, TAG_DIRECT);
    g_assert_cmphex(front_at(qts, 401, 20), ==, 0x00abcdef);
    g_assert_cmphex(front_tag_at(qts, 402, 20), ==, TAG_X_INDEX);
    g_assert_cmphex(front_at(qts, 402, 20), ==, 5);
    frame = kona_screendump(qts);
    g_assert_cmphex(ppm_pixel(frame, 400, 20), ==, 0);
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

#define READ_PAGE UINT64_C(0x320000)
#define READ_SAVE UINT64_C(0x340000)
#define TOKEN_PIXEL_READ 0x138

static uint32_t read_guest_word(QTestState *s, uint64_t addr)
{
    uint8_t bytes[4];

    qtest_memread(s, addr, bytes, sizeof(bytes));
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

static void read_activate(QTestState *s, uint64_t save, uint64_t page)
{
    x_command(s, 0x1fd,
              (uint32_t[]){ 0x10000000 | (save >> 12),
                            0x9000 | (save & 0xfff), 0x10000 }, 3);
    x_command(s, 0x1f2,
              (uint32_t[]){ 0x10000000 | (page >> 12),
                            0x9000 | (page & 0xfff), 0x4000 }, 3);
}

static void read_gc(QTestState *s)
{
    x_command(s, TOKEN_GC,
              (uint32_t[]){ 0, 0, 0, 0, 0, 0,
                            0x180c, 0x40001, 1, 6, 5, 1, 1 }, 13);
}

static void read_setup(QTestState *s, unsigned width, unsigned height)
{
    x_command(s, TOKEN_PIXEL_SETUP,
              (uint32_t[]){ 2, 1, 0, 8, width, height, 0 }, 7);
}

static void read_prepare(QTestState *s, uint64_t page)
{
    qtest_memset(s, page, 0xa5, 0x4000);
    qtest_memset(s, page, 0, 4);
}

static void read_unchanged(QTestState *s, uint64_t page)
{
    uint8_t got[0x4000], expected[0x4000];

    memset(expected, 0xa5, sizeof(expected));
    memset(expected, 0, 4);
    qtest_memread(s, page, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, sizeof(expected));
}

static uint8_t read_pattern(unsigned x, unsigned y)
{
    static const uint8_t first[] = { 0, 1, 0x80, 0xff };

    return x < 4 && y == 0 ? first[x] : 31 * y + 17 * x;
}

/* Write unequal normalized indices through the existing eight-bit path. */
static void read_write_pattern(QTestState *s, int x, int y,
                               unsigned width, unsigned height)
{
    unsigned pitch = (width + 3) & ~3;

    pixel_setup(s, 0, 0, width, height, 0);
    pixel_write(s, x, y);
    for (unsigned r = 0; r < height; r++) {
        for (unsigned c = 0; c < pitch; c += 4) {
            uint32_t word = 0;

            for (unsigned k = 0; k < 4; k++) {
                word |= (uint32_t)read_pattern(c + k, r) << (24 - 8 * k);
            }
            data_words(s, &word, 1);
        }
    }
    pixel_finish(s);
}

static void read_assert_pattern(QTestState *s, uint64_t page,
                                unsigned width, unsigned height)
{
    unsigned pitch = (width + 3) & ~3;
    unsigned len = pitch * height;
    g_autofree uint8_t *got = g_malloc(len);

    g_assert_cmphex(read_guest_word(s, page), ==, 1);
    g_assert_cmphex(read_guest_word(s, page + 4), ==, 0xa5a5a5a5);
    qtest_memread(s, page + 8, got, len);
    for (unsigned r = 0; r < height; r++) {
        for (unsigned c = 0; c < pitch; c++) {
            g_assert_cmphex(got[r * pitch + c], ==,
                            c < width ? read_pattern(c, r) : 0);
        }
    }
    if (len < 0x4000 - 8) {
        g_assert_cmphex(read_guest_word(s, page + 8 + len), ==, 0xa5a5a5a5);
    }
}

static void test_read_indexed_rows(void)
{
    qtest_system_reset(qts);
    read_activate(qts, READ_SAVE, READ_PAGE);
    read_gc(qts);
    read_write_pattern(qts, 454, 700, 6, 20);
    g_assert_cmphex(front_at(qts, 457, 700), ==, 0xff);
    g_assert_cmphex(front_tag_at(qts, 457, 700), ==, TAG_X_INDEX);
    read_setup(qts, 6, 20);
    read_prepare(qts, READ_PAGE);
    read_unchanged(qts, READ_PAGE);
    x_command(qts, TOKEN_PIXEL_READ, (uint32_t[]){ 454, 700 }, 2);
    read_assert_pattern(qts, READ_PAGE, 6, 20);
}

static void read_scene(QTestState *s)
{
    read_activate(s, READ_SAVE, READ_PAGE);
    read_gc(s);
    read_write_pattern(s, 454, 700, 6, 20);
    read_setup(s, 6, 20);
}

static void read_refused(QTestState *s, int32_t x, int32_t y)
{
    read_prepare(s, READ_PAGE);
    x_command(s, TOKEN_PIXEL_READ, (uint32_t[]){ x, y }, 2);
    read_unchanged(s, READ_PAGE);
}

static void read_supported(QTestState *s)
{
    read_prepare(s, READ_PAGE);
    x_command(s, TOKEN_PIXEL_READ, (uint32_t[]){ 454, 700 }, 2);
    read_assert_pattern(s, READ_PAGE, 6, 20);
}

static void test_read_shapes(void)
{
    static const unsigned shapes[][4] = {
        { 0, 0, 1, 3 }, { 1276, 0, 4, 2 }, { 0, 1022, 5, 2 },
        { 1273, 1021, 7, 3 }, { 193, 301, 184, 89 },
    };

    qtest_system_reset(qts);
    read_activate(qts, READ_SAVE, READ_PAGE);
    read_gc(qts);
    for (unsigned k = 0; k < ARRAY_SIZE(shapes); k++) {
        unsigned x = shapes[k][0], y = shapes[k][1];
        unsigned width = shapes[k][2], height = shapes[k][3];

        read_write_pattern(qts, x, y, width, height);
        read_setup(qts, width, height);
        read_prepare(qts, READ_PAGE);
        x_command(qts, TOKEN_PIXEL_READ, (uint32_t[]){ x, y }, 2);
        read_assert_pattern(qts, READ_PAGE, width, height);
    }
    read_setup(qts, 184, 90);
    read_refused(qts, 193, 301);
}

/* Source byte values remain independent of colour maps and write state. */
static void test_read_ignores_write_state(void)
{
    qtest_system_reset(qts);
    read_scene(qts);
    read_supported(qts);
    x_command(qts, TOKEN_COLOR_MAP,
              (uint32_t[]){ X_MAP_WINDOW, 1, 0x3ff00000 }, 3);
    x_command(qts, TOKEN_POSITION, (uint32_t[]){ 91, 37, 7, 9 }, 4);
    x_command(qts, TOKEN_CLIP, (uint32_t[]){ 1, 0, 0, 10, 10 }, 5);
    x_command(qts, 0x12d, (uint32_t[]){ 0, 0, 1, 1 }, 4);
    x_command(qts, 0x13d, (uint32_t[]){ 1, 6, 2, 0, 0 }, 5);
    x_command(qts, 0x13c, (uint32_t[]){ 1 }, 1);
    read_supported(qts);
}

static void test_read_gc_predicates(void)
{
    static const uint32_t forms[][13] = {
        { 0, 0, 0, 0, 0, 0, 0x180c, 0x40001, 1, 6, 5, 1, 1 },
        { 0, 0, 0, 0, 0, 0, 0xc, 1, 1, 6, 0, 1, 1 },
    };
    uint32_t gc[13];

    qtest_system_reset(qts);
    read_scene(qts);
    for (unsigned f = 0; f < ARRAY_SIZE(forms); f++) {
        x_command(qts, TOKEN_GC, forms[f], 13);
        read_supported(qts);
        for (unsigned k = 0; k < 6; k++) {
            memcpy(gc, forms[f], sizeof(gc));
            gc[k] = 0xdeadbeef;
            x_command(qts, TOKEN_GC, gc, 13);
            read_supported(qts);
        }
        for (unsigned k = 6; k < 13; k++) {
            memcpy(gc, forms[f], sizeof(gc));
            gc[k] ^= 2;
            x_command(qts, TOKEN_GC, gc, 13);
            read_refused(qts, 454, 700);
        }
    }
    read_gc(qts);
    read_supported(qts);
}

static void test_read_setup_refused(void)
{
    static const uint32_t changes[][2] = {
        { 0, 0 }, { 0, 0x12 }, { 1, 0 }, { 1, 2 }, { 2, 1 },
        { 3, 0 }, { 3, 16 }, { 4, 0 }, { 5, 0 }, { 6, 1 },
        { 4, UINT32_MAX }, { 5, UINT32_MAX }, { 4, 1280 },
    };
    uint32_t setup[7];

    qtest_system_reset(qts);
    read_scene(qts);
    for (unsigned k = 0; k < ARRAY_SIZE(changes); k++) {
        memcpy(setup, (uint32_t[]){ 2, 1, 0, 8, 6, 20, 0 }, sizeof(setup));
        setup[changes[k][0]] = changes[k][1];
        x_command(qts, TOKEN_PIXEL_SETUP, setup, 7);
        read_refused(qts, 454, 700);
    }
    read_setup(qts, 6, 20);
    read_supported(qts);
}

static void test_read_coordinates_refused(void)
{
    static const int32_t coords[][2] = {
        { -1, 700 }, { 454, -1 }, { 1275, 700 }, { 454, 1005 },
        { INT32_MAX, INT32_MAX }, { INT32_MIN, INT32_MIN },
        { 0x138, 0x129 }, { 0x136, 0x13a },
    };

    qtest_system_reset(qts);
    read_scene(qts);
    for (unsigned k = 0; k < ARRAY_SIZE(coords); k++) {
        read_refused(qts, coords[k][0], coords[k][1]);
        read_supported(qts);
    }
}

/* A different tag at the last pixel cannot cause a partial reply. */
static void test_read_wrong_tag(gconstpointer opaque)
{
    bool textport = GPOINTER_TO_INT(opaque);

    qtest_system_reset(qts);
    read_scene(qts);
    if (textport) {
        qtest_writel(qts, REG_COMMAND, 1);
        qtest_writel(qts, REG_COMMAND, 6);
        qtest_writel(qts, REG_COMMAND, 459);
        qtest_writel(qts, REG_COMMAND, 1023 - 719);
        g_assert_cmphex(front_tag_at(qts, 459, 719), ==, 1);
    } else {
        pixel_setup(qts, 0, 2, 1, 1, 0);
        pixel_write(qts, 459, 719);
        data_words(qts, (uint32_t[]){ 0x12abcdef }, 1);
        pixel_finish(qts);
        g_assert_cmphex(front_tag_at(qts, 459, 719), ==, TAG_DIRECT);
    }
    read_setup(qts, 6, 20);
    read_refused(qts, 454, 700);
}

static void read_deactivate(QTestState *s)
{
    x_command(s, 0x1fc, (uint32_t[]){ 0 }, 1);
}

static void read_restore(QTestState *s, uint64_t save)
{
    x_command(s, 0x1f9,
              (uint32_t[]){ read_guest_word(s, save),
                            0x10000000 | (save >> 12),
                            0x9000 | (save & 0xfff), 0xedededed }, 4);
}

static void test_read_context_pages(void)
{
    const uint64_t save_b = READ_SAVE + 0x20000;
    const uint64_t page_b = READ_PAGE + 0x10000;

    qtest_system_reset(qts);
    read_scene(qts);
    read_deactivate(qts);
    read_refused(qts, 454, 700);
    read_activate(qts, save_b, page_b);
    read_prepare(qts, page_b);
    x_command(qts, TOKEN_PIXEL_READ, (uint32_t[]){ 454, 700 }, 2);
    read_unchanged(qts, page_b);
    read_setup(qts, 6, 20);
    x_command(qts, TOKEN_PIXEL_READ, (uint32_t[]){ 454, 700 }, 2);
    read_unchanged(qts, page_b);
    read_gc(qts);
    x_command(qts, TOKEN_PIXEL_READ, (uint32_t[]){ 454, 700 }, 2);
    read_assert_pattern(qts, page_b, 6, 20);
    read_unchanged(qts, READ_PAGE);
    read_prepare(qts, page_b);
    read_deactivate(qts);
    read_restore(qts, READ_SAVE);
    read_refused(qts, 454, 700);
    read_gc(qts);
    read_refused(qts, 454, 700);
    read_setup(qts, 6, 20);
    read_supported(qts);
    read_unchanged(qts, page_b);
}

static void test_read_context_eviction(void)
{
    qtest_system_reset(qts);
    read_scene(qts);
    read_deactivate(qts);
    for (unsigned k = 1; k < 9; k++) {
        read_activate(qts, READ_SAVE + k * 0x20000,
                       READ_PAGE + k * 0x10000);
        read_deactivate(qts);
    }
    read_restore(qts, READ_SAVE);
    read_gc(qts);
    read_setup(qts, 6, 20);
    read_refused(qts, 454, 700);
}

/* Selector-shaped programmed-I/O words remain pixel data until count zero. */
static void test_read_inside_write(void)
{
    static const uint8_t bytes[] = {
        0, 0, 1, 0x38, 0, 0, 1, 0x29, 0, 0, 1, 0x36,
    };
    uint8_t got[12];

    qtest_system_reset(qts);
    read_activate(qts, READ_SAVE, READ_PAGE);
    read_gc(qts);
    read_prepare(qts, READ_PAGE);
    pixel_setup(qts, 0, 0, 4, 3, 0);
    pixel_write(qts, 454, 700);
    data_words(qts, (uint32_t[]){ 0x138, 0x129, 0x136 }, 3);
    read_unchanged(qts, READ_PAGE);
    pixel_finish(qts);
    read_setup(qts, 4, 3);
    x_command(qts, TOKEN_PIXEL_READ, (uint32_t[]){ 454, 700 }, 2);
    g_assert_cmphex(read_guest_word(qts, READ_PAGE), ==, 1);
    qtest_memread(qts, READ_PAGE + 8, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), bytes, sizeof(bytes));
}

static void test_read_open_dma_write_refused(void)
{
    qtest_system_reset(qts);
    read_scene(qts);
    pixel_setup(qts, 0x10, 0, 4, 2, 0);
    pixel_write(qts, 454, 700);
    read_refused(qts, 454, 700);
    pixel_finish(qts);
    read_gc(qts);
    read_setup(qts, 6, 20);
    read_supported(qts);
}

static void test_read_pending_run_refused(void)
{
    qtest_system_reset(qts);
    read_scene(qts);
    x_command(qts, 0x141, (uint32_t[]){ 0x00010002 }, 1);
    read_refused(qts, 454, 700);
    read_gc(qts);
    read_setup(qts, 6, 20);
    read_supported(qts);
}

static void test_read_overflow_requires_reload(gconstpointer opaque)
{
    bool setup_first = GPOINTER_TO_INT(opaque);

    qtest_system_reset(qts);
    read_scene(qts);
    read_supported(qts);
    for (unsigned i = 0; i < 33; i++) {
        x_command(qts, 0x141, (uint32_t[]){ 0x00010002 }, 1);
    }
    read_refused(qts, 454, 700);
    if (setup_first) {
        read_setup(qts, 6, 20);
    } else {
        read_gc(qts);
    }
    read_refused(qts, 454, 700);
    if (setup_first) {
        read_gc(qts);
    } else {
        read_setup(qts, 6, 20);
    }
    read_supported(qts);
}

static void test_read_bulk_abort_requires_reload(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);

    qtest_system_reset(qts);
    read_scene(qts);
    read_supported(qts);
    if (kind == 0) {
        pixel_setup(qts, 0x10, 0, 4, 2, 0);
        pixel_write(qts, 454, 700);
        /* Plain framing avoids the generic slot-abort invalidation. */
        qtest_writel(qts, REG_COMMAND_DATA, TOKEN_PIXEL_FINISH);
        qtest_writel(qts, REG_COMMAND_DATA, 0);
        read_setup(qts, 6, 20);
    } else if (kind == 1) {
        static const uint32_t header[] = { 65535, 65535, 0, 0, 0, 0 };

        qtest_writel(qts, REG_COMMAND, 1);
        qtest_writel(qts, REG_COMMAND, 0xa);
        for (unsigned i = 0; i < ARRAY_SIZE(header); i++) {
            qtest_writel(qts, REG_COMMAND, header[i]);
        }
    } else if (kind == 2) {
        qtest_writel(qts, REG_COMMAND, 0x1a6);
        qtest_writel(qts, REG_COMMAND, 1);
        for (unsigned i = 0; i < 1024; i++) {
            qtest_writel(qts, REG_COMMAND, 0);
        }
    } else {
        static const uint32_t empty[] = { 0x10, 0, 0, 0, 0, 0, 0 };

        pixel_setup(qts, 0x10, 0, 4, 2, 0);
        pixel_write(qts, 454, 700);
        qtest_writel(qts, REG_COMMAND_DATA, TOKEN_PIXEL_SETUP);
        for (unsigned i = 0; i < ARRAY_SIZE(empty); i++) {
            qtest_writel(qts, REG_COMMAND_DATA, empty[i]);
        }
        qtest_writel(qts, REG_COMMAND_DATA, TOKEN_PIXEL_WRITE);
        qtest_writel(qts, REG_COMMAND_DATA, 0);
        qtest_writel(qts, REG_COMMAND_DATA, 0);
        qtest_writel(qts, REG_COMMAND_DATA, TOKEN_PIXEL_FINISH);
        qtest_writel(qts, REG_COMMAND_DATA, 0);
        read_setup(qts, 6, 20);
    }
    read_refused(qts, 454, 700);
    read_setup(qts, 6, 20);
    read_refused(qts, 454, 700);
    read_gc(qts);
    read_supported(qts);
}

static void test_read_empty_bitmap_retains_state(void)
{
    qtest_system_reset(qts);
    read_scene(qts);
    qtest_writel(qts, REG_COMMAND, 1);
    qtest_writel(qts, REG_COMMAND, 0xa);
    for (unsigned i = 0; i < 6; i++) {
        qtest_writel(qts, REG_COMMAND, 0);
    }
    read_supported(qts);
}

static void test_read_state_migrates(gconstpointer opaque)
{
    unsigned state = GPOINTER_TO_UINT(opaque);
    g_autofree char *tmpfs = NULL, *uri = NULL, *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-read-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MACHINE_ARGS);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    read_scene(from);
    read_prepare(from, READ_PAGE);
    if (state == 1) {
        qtest_writel(from, REG_COMMAND_SLOT(TOKEN_PIXEL_READ), 454);
    } else if (state == 2) {
        read_deactivate(from);
        read_restore(from, READ_SAVE);
    }
    sgi_migrate_stopped(from, uri);
    if (state == 1) {
        qtest_writel(to, REG_COMMAND_DATA, 700);
        read_assert_pattern(to, READ_PAGE, 6, 20);
    } else if (state == 2) {
        x_command(to, TOKEN_PIXEL_READ, (uint32_t[]){ 454, 700 }, 2);
        read_unchanged(to, READ_PAGE);
        read_gc(to);
        read_setup(to, 6, 20);
        read_supported(to);
    } else {
        read_supported(to);
    }
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/* GL uses the ordinary command port; kernel transitions use kick framing. */
static void read_gl_command(QTestState *s, uint32_t token,
                            const uint32_t *args, unsigned count, bool kicked)
{
    if (kicked) {
        qtest_writel(s, KONA_REG(0xc00108), 0x30000);
    }
    qtest_writel(s, REG_COMMAND, token);
    if (kicked) {
        qtest_writel(s, KONA_REG(0xc00108), 0x10000);
    }
    for (unsigned i = 0; i < count; i++) {
        qtest_writel(s, REG_COMMAND, args[i]);
    }
    if (kicked) {
        qtest_writel(s, KONA_REG(0xc00108), 0);
    }
}

static void test_read_bank1_window(void)
{
    qtest_system_reset(qts);
    read_scene(qts);
    read_gl_command(qts, 1, (uint32_t[]){ 0, 12, 0, 1, 0, 0, 0, 0, 0, 0 },
                    10, false);
    read_gl_command(qts, 0x1f3,
                    (uint32_t[]){ 454, 304, 0x05670080, 0x05670080,
                                  454, 304, 6, 20, 0, 0xd, 0 }, 11, true);
    read_gl_command(qts, 0x1ee, (uint32_t[]){ 454, 304, 0 }, 3, true);
    read_gl_command(qts, 0x1d,
                    (uint32_t[]){ 0, 0x3f800000, 0, 0x3f800000 }, 4, false);
    read_gl_command(qts, 0x1a, (uint32_t[]){ 0x4000 }, 1, false);
    g_assert_cmphex(kona_plane_word(qts, 1, 454, 323), ==, 0x00ff00);
    g_assert_cmphex(front_at(qts, 454, 700), ==, 0);
    read_supported(qts);

    read_gl_command(qts, 0xc6, (uint32_t[]){ 0 }, 1, false);
    read_gl_command(qts, 0x1f4, (uint32_t[]){ 1, 1 }, 2, true);
    read_gl_command(qts, 0x172, (uint32_t[]){ 0 }, 1, true);
    read_gl_command(qts, 0x1e6, (uint32_t[]){ 0x800003, 0xd, 5 }, 3, true);
    read_gl_command(qts, 0x1e7, (uint32_t[]){ 1 }, 1, true);
    qtest_clock_step(qts, 20000000);
    read_refused(qts, 454, 700);

    /* A one-pixel intersection refuses the whole otherwise-valid rectangle. */
    read_gl_command(qts, 0x1f3,
                    (uint32_t[]){ 459, 304, 0x05670080, 0x05670080,
                                  459, 304, 6, 20, 0, 0xd, 0 }, 11, true);
    read_refused(qts, 454, 700);
    read_gl_command(qts, 0x1f3,
                    (uint32_t[]){ 460, 304, 0x05670080, 0x05670080,
                                  460, 304, 6, 20, 0, 0xd, 0 }, 11, true);
    read_supported(qts);
}

static void test_read_partial_replacement(void)
{
    static const uint32_t gc[] = {
        0, 0, 0, 0, 0, 0, 0xc, 1, 1, 6, 0, 1, 1,
    };
    static const uint32_t setup[] = { 2, 1, 0, 8, 6, 20, 0 };

    qtest_system_reset(qts);
    read_scene(qts);
    read_supported(qts);
    read_supported(qts);
    for (unsigned field = 0; field < 2; field++) {
        const uint32_t *args = field ? setup : gc;
        unsigned count = field ? ARRAY_SIZE(setup) : ARRAY_SIZE(gc);
        uint32_t token = field ? TOKEN_PIXEL_SETUP : TOKEN_GC;

        for (unsigned n = 1; n < count; n++) {
            x_command(qts, token, args, n);
            read_refused(qts, 454, 700);
            /* Aborted replacement invalidates both read records. */
            read_gc(qts);
            read_refused(qts, 454, 700);
            read_setup(qts, 6, 20);
            read_supported(qts);
        }
    }
}

static void test_read_reply_order(void)
{
    g_autofree char *log = NULL, *found = NULL;
    const char *payload, *ready;
    QTestState *s = kona_init_logged(MACHINE_ARGS, &log);

    read_scene(s);
    read_supported(s);
    g_assert_cmphex(qtest_readl(s, REG_MAILBOX), ==, 0);
    g_assert_cmphex(qtest_readl(s, REG_INT_CAUSE) & INT_CAUSE_MAILBOX, ==, 0);
    qtest_quit(s);
    found = kona_log_read(log);
    payload = strstr(found, "addr 0x320008 len 0xa0\n");
    ready = strstr(found, "addr 0x320000 len 0x4\n");
    g_assert_nonnull(payload);
    g_assert_nonnull(ready);
    g_assert_true(payload < ready);
}

static void test_read_payload_failure(void)
{
    const uint64_t page = 64 * 1024 * 1024 - 8;
    g_autofree char *log = NULL, *found = NULL;
    QTestState *s = kona_init_logged(MACHINE_ARGS, &log);

    read_activate(s, READ_SAVE, page);
    read_gc(s);
    read_write_pattern(s, 454, 700, 6, 20);
    read_setup(s, 6, 20);
    qtest_memset(s, page, 0, 4);
    qtest_memset(s, page + 4, 0xa5, 4);
    x_command(s, TOKEN_PIXEL_READ, (uint32_t[]){ 454, 700 }, 2);
    g_assert_cmphex(read_guest_word(s, page), ==, 0);
    g_assert_cmphex(read_guest_word(s, page + 4), ==, 0xa5a5a5a5);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found,
                    "160-byte write to 0x9000004000000 failed (result 2)"));
    g_assert_null(strstr(found, "addr 0x3fffff8 len 0x4\n"));
}

#define READ_OTHER_PAGE UINT64_C(0x360000)

static void read_register(QTestState *s, uint64_t page)
{
    x_command(s, 0x1f2,
              (uint32_t[]){ 0x10000000 | (page >> 12),
                            0x9000 | (page & 0xfff), 0x4000 }, 3);
}

static void read_page_reload(QTestState *s, bool setup_first)
{
    read_register(s, READ_PAGE);
    read_refused(s, 454, 700);
    if (setup_first) {
        read_setup(s, 6, 20);
    } else {
        read_gc(s);
    }
    read_refused(s, 454, 700);
    if (setup_first) {
        read_gc(s);
    } else {
        read_setup(s, 6, 20);
    }
    read_supported(s);
    read_unchanged(s, READ_OTHER_PAGE);
}

static void test_read_page_transitions(void)
{
    qtest_system_reset(qts);
    read_scene(qts);
    read_supported(qts);
    /* Same resolved page through either registration path retains validity. */
    read_register(qts, READ_PAGE);
    read_supported(qts);
    qtest_writel(qts, KONA_REG(0x100000 + 4 * 5),
                 0x20000000 | (READ_PAGE >> 12));
    x_command(qts, 0x1f2, (uint32_t[]){ 0x14, 0x9000, 0x4000 }, 3);
    read_supported(qts);
    /* Unmapped registration preserves the eligible active page. */
    x_command(qts, 0x1f2, (uint32_t[]){ 0x18, 0x9000, 0x4000 }, 3);
    read_supported(qts);

    read_prepare(qts, READ_OTHER_PAGE);
    read_register(qts, READ_OTHER_PAGE);
    read_refused(qts, 454, 700);
    read_unchanged(qts, READ_OTHER_PAGE);
    read_page_reload(qts, false);

    read_register(qts, READ_OTHER_PAGE);
    /* Loads while mismatched update write state but cannot arm reads. */
    read_gc(qts);
    read_write_pattern(qts, 454, 700, 6, 20);
    g_assert_cmphex(front_at(qts, 457, 700), ==, 0xff);
    read_setup(qts, 6, 20);
    read_refused(qts, 454, 700);
    read_unchanged(qts, READ_OTHER_PAGE);
    read_page_reload(qts, true);
}

static void test_read_mismatched_page_migrates(gconstpointer opaque)
{
    bool load_mismatched = GPOINTER_TO_INT(opaque);
    g_autofree char *tmpfs = NULL, *uri = NULL, *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-read-page-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MACHINE_ARGS);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    read_scene(from);
    read_prepare(from, READ_OTHER_PAGE);
    read_register(from, READ_OTHER_PAGE);
    if (load_mismatched) {
        read_gc(from);
        read_setup(from, 6, 20);
    }
    read_refused(from, 454, 700);
    read_unchanged(from, READ_OTHER_PAGE);
    sgi_migrate_stopped(from, uri);
    read_refused(to, 454, 700);
    read_unchanged(to, READ_OTHER_PAGE);
    read_page_reload(to, load_mismatched);
    /* The uninterrupted control follows the same reload/refusal sequence. */
    read_page_reload(from, load_mismatched);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_read_reset_partial(void)
{
    qtest_system_reset(qts);
    read_scene(qts);
    qtest_writel(qts, REG_COMMAND_SLOT(TOKEN_PIXEL_READ), 454);
    qtest_system_reset(qts);
    read_refused(qts, 454, 700);
    read_scene(qts);
    read_supported(qts);
}

#define COPY_XY(x, y) (((uint32_t)(uint16_t)(x) << 16) | (uint16_t)(y))

/* Independently pack the reviewed RGB8 colour and component masks. */
static void copy_rgb_gc(uint32_t *gc, uint32_t rgb, uint32_t mask,
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
    gc[7] = gc[8] = 1;
    gc[9] = gx * 2;
}

static uint32_t copy_expected(unsigned gx, uint32_t source, uint32_t dest,
                               uint32_t mask, unsigned bits)
{
    static const uint8_t truth[] = {
        0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15,
    };
    uint32_t result = 0;

    for (unsigned bit = 0; bit < bits; bit++) {
        unsigned input = (((source >> bit) & 1) << 1) |
                         ((dest >> bit) & 1);

        result |= ((truth[gx] >> input) & 1) << bit;
    }
    return (result & mask) | (dest & ~mask);
}

/* Existing public GL fixtures seed nonzero back, depth and stencil planes. */
static void copy_canaries(QTestState *s)
{
    uint32_t gc[13] = { 0x20, 0, 0, 0, 0xf00fff, 0,
                         8, 1, 0, 6, 0, 1, 1 };
    static const uint32_t config[] = { 0, 12, 0, 1, 0, 0, 24, 8, 0, 0 };

    read_gl_command(s, 0x1fd,
                    (uint32_t[]){ 0x10000340, 0x9000, 0x10000 }, 3, true);
    read_gl_command(s, 0x1f2,
                    (uint32_t[]){ 0x10000380, 0x9000, 0x4000 }, 3, true);
    read_gl_command(s, 1, config, ARRAY_SIZE(config), false);
    read_gl_command(s, 2, (uint32_t[]){ 0 }, 1, false);
    read_gl_command(s, 0x1f3,
                    (uint32_t[]){ 450, 300, 0x05670080, 0x05670080,
                                  450, 300, 128, 128, 0, 0xd, 0 }, 11, true);
    read_gl_command(s, 0x1ee, (uint32_t[]){ 450, 300, 0 }, 3, true);
    read_gl_command(s, 0xc4, (uint32_t[]){ 0x405 }, 1, false);
    read_gl_command(s, 0x1d,
                    (uint32_t[]){ 0x3f800000, 0x3f000000, 0x3e800000,
                                  0x3f800000 }, 4, false);
    read_gl_command(s, 0x1a, (uint32_t[]){ 0x4000 }, 1, false);
    read_gl_command(s, 0x1f, (uint32_t[]){ 0x3fe40000, 0 }, 2, false);
    read_gl_command(s, 0x20, (uint32_t[]){ 255 }, 1, false);
    read_gl_command(s, 0x1e, (uint32_t[]){ 0x5a }, 1, false);
    read_gl_command(s, 0x1a, (uint32_t[]){ 0x500 }, 1, false);
    g_assert_cmphex(kona_plane_word(s, 1, 450, 300), !=, 0);
    g_assert_cmphex(kona_plane_word(s, 5, 450, 300), !=, 0);
    g_assert_cmphex(kona_plane_word(s, 6, 450, 300), ==, 0x5a);
    for (unsigned plane = 7; plane < 11; plane++) {
        gc[0] = plane == 10 ? 8 : 0x20;
        gc[6] = plane == 7 ? 8 : plane == 8 ? 0x2c :
                plane == 9 ? 0x1830 : 0x20;
        gc[7] = plane == 9 ? 2 : 1;
        gc[10] = plane == 9 ? 5 : 0;
        gc[4] = plane == 10 ? 0x300000 : 0xf00fff;
        x_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
        x_command(s, 0x154,
                  (uint32_t[]){ COPY_XY(700, 800), COPY_XY(701, 801) }, 2);
        g_assert_cmphex(kona_plane_word(s, plane, 700,
                                        KONA_FB_HEIGHT - 1 - 800), !=, 0);
    }
}

static void copy_semantic_gc(QTestState *s, bool rgb, uint32_t mask,
                              unsigned gx)
{
    uint32_t gc[13] = { 0, 0, 0, 0, 0xf0000f | (mask << 4), 0,
                         0x180c, 0x40001, 1, gx * 2, 5, 1, 1 };

    if (rgb) {
        copy_rgb_gc(gc, 0, mask, gx);
        gc[6] = 0x2804;
        gc[7] = 0x60001;
        gc[10] = 8;
    }
    x_command(s, TOKEN_GC, gc, ARRAY_SIZE(gc));
}

static void test_copy_semantics(gconstpointer opaque)
{
    unsigned test = GPOINTER_TO_UINT(opaque);
    unsigned gx = (test >> 8) & 15;
    unsigned mask_byte = test & 0xff;
    bool rgb = test & 0x10000;
    uint32_t source = rgb ? 0x963ca5 : 0x96;
    uint32_t dest = rgb ? 0x5a81c3 : 0xa5;
    uint32_t mask = rgb ? (mask_byte == 0xff ? 0xffffff :
                           mask_byte ? 0x53a56c : 0) : mask_byte;
    uint32_t result = copy_expected(gx, source, dest, mask, rgb ? 24 : 8);
    uint32_t gc[13] = { source << 12, 0, 0, 0, 0xf00fff, 0,
                         0xc, 1, 1, 6, 0, 1, 1 };
    size_t pixels = (size_t)KONA_FB_WIDTH * KONA_FB_HEIGHT;
    g_autofree uint8_t *words = g_malloc0(pixels * 4);
    g_autofree uint8_t *tags = g_malloc0(pixels);
    char *before[11] = { NULL };

    qtest_system_reset(qts);
    copy_canaries(qts);
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        if (plane != 0 && plane != 2) {
            before[plane] = copy_plane_digest(qts, plane);
        }
    }
    for (unsigned region = 0; region < 2; region++) {
        unsigned x = region ? 30 : 10, y = region ? 40 : 20;
        uint32_t color = region ? dest : source;

        if (rgb) {
            copy_rgb_gc(gc, color, 0xffffff, 3);
        } else {
            gc[0] = color << 12;
        }
        x_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
        x_command(qts, 0x154,
                  (uint32_t[]){ COPY_XY(x, y), COPY_XY(x + 2, y + 2) }, 2);
        for (unsigned row = 0; row < 2; row++) {
            for (unsigned col = 0; col < 2; col++) {
                size_t pixel = (KONA_FB_HEIGHT - 1 - y - row) *
                               KONA_FB_WIDTH + x + col;
                uint32_t value = region && row == 0 && col == 0 ?
                                 result : color;

                for (unsigned byte = 0; byte < 4; byte++) {
                    words[pixel * 4 + byte] = value >> (8 * byte);
                }
                tags[pixel] = rgb ? 0 : TAG_X_INDEX;
            }
        }
    }
    copy_semantic_gc(qts, rgb, mask, gx);
    x_command(qts, TOKEN_PIXEL_SETUP,
              (uint32_t[]){ 1, rgb ? 2 : 1, rgb ? 2 : 1, 0, 1, 1, 0 }, 7);
    x_command(qts, TOKEN_PIXEL_COPY, (uint32_t[]){ 10, 20, 30, 40 }, 4);
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = copy_plane_digest(qts, plane);

        if (plane == 0 || plane == 2) {
            g_autofree char *expected = g_compute_checksum_for_data(
                G_CHECKSUM_SHA256, plane == 0 ? words : tags,
                pixels * (plane == 0 ? 4 : 1));

            g_assert_cmpstr(actual, ==, expected);
        } else {
            g_assert_cmpstr(actual, ==, before[plane]);
            g_free(before[plane]);
        }
    }
    assert_in_step(qts);
}

static void test_copy_late_refusal(gconstpointer opaque)
{
    unsigned test = GPOINTER_TO_UINT(opaque);
    bool rgb = test & 1, source_bad = test & 2;
    uint32_t gc[13] = { 0x96000, 0, 0, 0, 0xf00fff, 0,
                         0xc, 1, 1, 6, 0, 1, 1 };
    uint32_t source = rgb ? 0x963ca5 : 0x96;
    char *before[11];

    qtest_system_reset(qts);
    copy_canaries(qts);
    if (rgb) {
        copy_rgb_gc(gc, source, 0xffffff, 3);
    }
    x_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    x_command(qts, 0x154,
              (uint32_t[]){ COPY_XY(10, 20), COPY_XY(12, 21) }, 2);
    if (rgb) {
        copy_rgb_gc(gc, 0x5a81c3, 0xffffff, 3);
    } else {
        gc[0] = 0xa5000;
    }
    x_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    x_command(qts, 0x154,
              (uint32_t[]){ COPY_XY(30, 40), COPY_XY(32, 41) }, 2);
    if (rgb) {
        memcpy(gc, (uint32_t[]){ 0x7b000, 0, 0, 0, 0xf00fff, 0,
                                 0xc, 1, 1, 6, 0, 1, 1 }, sizeof(gc));
    } else {
        copy_rgb_gc(gc, 0x123456, 0xffffff, 3);
    }
    x_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    x_command(qts, 0x154, (uint32_t[]){
        COPY_XY(source_bad ? 11 : 31, source_bad ? 20 : 40),
        COPY_XY(source_bad ? 12 : 32, source_bad ? 21 : 41),
    }, 2);
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        before[plane] = copy_plane_digest(qts, plane);
    }
    copy_semantic_gc(qts, rgb, rgb ? 0x53a56c : 0x53, 6);
    x_command(qts, TOKEN_PIXEL_SETUP,
              (uint32_t[]){ 1, rgb ? 2 : 1, rgb ? 2 : 1, 0, 2, 1, 0 }, 7);
    x_command(qts, TOKEN_PIXEL_COPY, (uint32_t[]){ 10, 20, 30, 40 }, 4);
    /* Even an incompatible accessed set is untouched by a zero mask. */
    copy_semantic_gc(qts, rgb, 0, 3);
    x_command(qts, TOKEN_PIXEL_COPY, (uint32_t[]){ 10, 20, 30, 40 }, 4);
    copy_semantic_gc(qts, rgb, rgb ? 0xffffff : 0xff, 5);
    x_command(qts, TOKEN_PIXEL_COPY, (uint32_t[]){ 10, 20, 30, 40 }, 4);
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = copy_plane_digest(qts, plane);

        g_assert_cmpstr(actual, ==, before[plane]);
        g_free(before[plane]);
    }
    copy_semantic_gc(qts, rgb, rgb ? 0xffffff : 0xff, 3);
    x_command(qts, TOKEN_PIXEL_SETUP,
              (uint32_t[]){ 1, rgb ? 2 : 1, rgb ? 2 : 1, 0, 1, 1, 0 }, 7);
    x_command(qts, TOKEN_PIXEL_COPY, (uint32_t[]){ 10, 20, 30, 40 }, 4);
    g_assert_cmphex(front_at(qts, 30, 40), ==, source);
    g_assert_cmphex(front_tag_at(qts, 30, 40), ==, rgb ? 0 : TAG_X_INDEX);
    assert_in_step(qts);
}

static void test_copy_overlap_semantics(gconstpointer opaque)
{
    unsigned test = GPOINTER_TO_UINT(opaque);
    bool rgb = test & 1, logic = test & 2;
    unsigned direction = test >> 2;
    unsigned gx = logic ? 6 : 3;
    uint32_t mask = logic ? (rgb ? 0x53a56c : 0x53) :
                            (rgb ? 0xffffff : 0xff);
    uint32_t original[4][4], final[4][4];
    uint32_t gc[13] = { 0, 0, 0, 0, 0xf00fff, 0, 0xc, 1, 1, 6, 0, 1, 1 };
    unsigned sx = direction & 1, sy = (direction >> 1) & 1;
    unsigned dx = !sx, dy = !sy;
    size_t pixels = (size_t)KONA_FB_WIDTH * KONA_FB_HEIGHT;
    g_autofree uint8_t *words = g_malloc0(pixels * 4);
    g_autofree uint8_t *tags = g_malloc0(pixels);
    char *before[11] = { NULL };

    qtest_system_reset(qts);
    copy_canaries(qts);
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        if (plane != 0 && plane != 2) {
            before[plane] = copy_plane_digest(qts, plane);
        }
    }
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            uint32_t value = ((x + 1) * 0x31 + (y + 1) * 0x17) & 0xff;

            if (rgb) {
                value |= ((value ^ 0x5a) << 8) | ((value ^ 0xa5) << 16);
                copy_rgb_gc(gc, value, 0xffffff, 3);
            } else {
                gc[0] = value << 12;
            }
            original[y][x] = final[y][x] = value;
            x_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
            x_command(qts, 0x154, (uint32_t[]){ COPY_XY(10 + x, 20 + y),
                                               COPY_XY(11 + x, 21 + y) }, 2);
        }
    }
    for (unsigned y = 0; y < 3; y++) {
        for (unsigned x = 0; x < 3; x++) {
            final[dy + y][dx + x] = copy_expected(gx,
                original[sy + y][sx + x], original[dy + y][dx + x],
                mask, rgb ? 24 : 8);
        }
    }
    copy_semantic_gc(qts, rgb, mask, gx);
    x_command(qts, TOKEN_PIXEL_SETUP,
              (uint32_t[]){ 1, rgb ? 2 : 1, rgb ? 2 : 1, 0, 3, 3, 0 }, 7);
    x_command(qts, TOKEN_PIXEL_COPY,
              (uint32_t[]){ 10 + sx, 20 + sy, 10 + dx, 20 + dy }, 4);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            size_t pixel = (KONA_FB_HEIGHT - 1 - 20 - y) *
                           KONA_FB_WIDTH + 10 + x;

            for (unsigned byte = 0; byte < 4; byte++) {
                words[pixel * 4 + byte] = final[y][x] >> (8 * byte);
            }
            tags[pixel] = rgb ? 0 : TAG_X_INDEX;
        }
    }
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = copy_plane_digest(qts, plane);

        if (plane == 0 || plane == 2) {
            g_autofree char *expected = g_compute_checksum_for_data(
                G_CHECKSUM_SHA256, plane == 0 ? words : tags,
                pixels * (plane == 0 ? 4 : 1));

            g_assert_cmpstr(actual, ==, expected);
        } else {
            g_assert_cmpstr(actual, ==, before[plane]);
            g_free(before[plane]);
        }
    }
    assert_in_step(qts);
}

static void test_copy_migration(gconstpointer opaque)
{
    bool rgb = GPOINTER_TO_UINT(opaque);
    g_autofree char *dir = NULL, *uri = NULL, *args = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint32_t gc[13] = { 0x96000, 0, 0, 0, 0xf00fff, 0,
                         0xc, 1, 1, 6, 0, 1, 1 };
    uint32_t source = rgb ? 0x963ca5 : 0x96;
    char *before[11];

    dir = g_dir_make_tmp("sn0-kona-copy-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    from = qtest_init(MIGRATION_ARGS);
    args = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(args);
    copy_canaries(from);
    if (rgb) {
        copy_rgb_gc(gc, source, 0xffffff, 3);
    }
    x_command(from, TOKEN_GC, gc, ARRAY_SIZE(gc));
    x_command(from, 0x154,
              (uint32_t[]){ COPY_XY(10, 20), COPY_XY(12, 22) }, 2);
    copy_semantic_gc(from, rgb, rgb ? 0xffffff : 0xff, 3);
    x_command(from, TOKEN_PIXEL_SETUP,
              (uint32_t[]){ 1, rgb ? 2 : 1, rgb ? 2 : 1, 0, 2, 2, 0 }, 7);
    x_command(from, TOKEN_PIXEL_COPY, (uint32_t[]){ 10, 20, 30, 40 }, 4);
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        before[plane] = copy_plane_digest(from, plane);
    }
    sgi_migrate_stopped(from, uri);
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = copy_plane_digest(to, plane);

        g_assert_cmpstr(actual, ==, before[plane]);
        g_free(before[plane]);
    }
    /* The migrated GC and setup perform another same-class copy. */
    x_command(to, TOKEN_PIXEL_COPY, (uint32_t[]){ 30, 40, 50, 60 }, 4);
    g_assert_cmphex(front_at(to, 50, 60), ==, source);
    g_assert_cmphex(front_tag_at(to, 50, 60), ==, rgb ? 0 : TAG_X_INDEX);
    assert_in_step(to);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

static void test_copy_setup_refusal(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    uint32_t gc[13] = { 0x96000, 0, 0, 0, 0xf00fff, 0,
                         0xc, 1, 1, 6, 0, 1, 1 };
    uint32_t setup[7] = { 1, 1, 1, 0, 2, 2, 0 };
    char *before[11];

    qtest_system_reset(qts);
    copy_canaries(qts);
    x_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    x_command(qts, 0x154,
              (uint32_t[]){ COPY_XY(10, 20), COPY_XY(12, 22) }, 2);
    gc[0] = 0xa5000;
    x_command(qts, TOKEN_GC, gc, ARRAY_SIZE(gc));
    x_command(qts, 0x154,
              (uint32_t[]){ COPY_XY(30, 40), COPY_XY(32, 42) }, 2);
    copy_semantic_gc(qts, false, 0xff, 3);
    if (kind == 0) {
        setup[0] = 0;
    } else if (kind == 1) {
        setup[0] = 0x11;
    } else if (kind == 2) {
        setup[1] = 2;
    } else if (kind == 3) {
        setup[2] = 2;
    } else if (kind == 4) {
        setup[3] = 8;
    } else if (kind == 5) {
        setup[6] = 1;
    } else if (kind == 6) {
        setup[1] = setup[2] = 3;
    } else {
        /* Huge dimensions clip to screen bounds, then refuse a late tag. */
        setup[4] = setup[5] = UINT32_MAX;
    }
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        before[plane] = copy_plane_digest(qts, plane);
    }
    x_command(qts, TOKEN_PIXEL_SETUP, setup, ARRAY_SIZE(setup));
    x_command(qts, TOKEN_PIXEL_COPY, (uint32_t[]){ 10, 20, 30, 40 }, 4);
    for (unsigned plane = 0; plane < ARRAY_SIZE(before); plane++) {
        g_autofree char *actual = copy_plane_digest(qts, plane);

        g_assert_cmpstr(actual, ==, before[plane]);
        g_free(before[plane]);
    }
    x_command(qts, TOKEN_PIXEL_SETUP, (uint32_t[]){ 1, 1, 1, 0, 1, 1, 0 }, 7);
    x_command(qts, TOKEN_PIXEL_COPY, (uint32_t[]){ 10, 20, 30, 40 }, 4);
    g_assert_cmphex(front_at(qts, 30, 40), ==, 0x96);
    assert_in_step(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

    for (unsigned rgb = 0; rgb < 2; rgb++) {
        for (unsigned gx = 0; gx < 16; gx++) {
            static const uint8_t masks[] = { 0xff, 0x53, 0 };

            for (unsigned i = 0; i < ARRAY_SIZE(masks); i++) {
                g_autofree char *name = g_strdup_printf(
                    "sn0/origin2000/kona-pixel/semantic-copy/%s/gx-%u-mask-%02x",
                    rgb ? "rgb" : "ci", gx, masks[i]);

                qtest_add_data_func(name, GUINT_TO_POINTER(
                    (rgb << 16) | (gx << 8) | masks[i]), test_copy_semantics);
            }
        }
    }

    for (unsigned test = 0; test < 4; test++) {
        g_autofree char *name = g_strdup_printf(
            "sn0/origin2000/kona-pixel/semantic-copy/refusal/%s-%s",
            test & 1 ? "rgb" : "ci", test & 2 ? "source" : "destination");

        qtest_add_data_func(name, GUINT_TO_POINTER(test),
                            test_copy_late_refusal);
    }

    for (unsigned test = 0; test < 16; test++) {
        g_autofree char *name = g_strdup_printf(
            "sn0/origin2000/kona-pixel/semantic-copy/overlap/%s-%s-%u",
            test & 1 ? "rgb" : "ci", test & 2 ? "xor" : "copy", test >> 2);

        qtest_add_data_func(name, GUINT_TO_POINTER(test),
                            test_copy_overlap_semantics);
    }
    qtest_add_data_func("sn0/origin2000/kona-pixel/semantic-copy/migration/ci",
                        GUINT_TO_POINTER(0), test_copy_migration);
    qtest_add_data_func("sn0/origin2000/kona-pixel/semantic-copy/migration/rgb",
                        GUINT_TO_POINTER(1), test_copy_migration);

    for (unsigned test = 0; test < 8; test++) {
        g_autofree char *name = g_strdup_printf(
            "sn0/origin2000/kona-pixel/semantic-copy/setup-refusal/%u", test);

        qtest_add_data_func(name, GUINT_TO_POINTER(test),
                            test_copy_setup_refusal);
    }

    qtest_add_func("sn0/origin2000/kona-pixel/read-indexed-rows",
                   test_read_indexed_rows);
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
    qtest_add_func("sn0/origin2000/kona-pixel/copy-refuses-mixed-tags",
                   test_copy_refuses_mixed_tags);
    qtest_add_func("sn0/origin2000/kona-pixel/copy-clipped",
                   test_copy_clipped);
    qtest_add_func("sn0/origin2000/kona-pixel/finish-without-write",
                   test_finish_without_write);
    qtest_add_func("sn0/origin2000/kona-pixel/reset-clears-write",
                   test_reset_clears_write);
    qtest_add_func("sn0/origin2000/kona-pixel/migration-mid-write",
                   test_migration_mid_write);

    qtest_add_func("sn0/origin2000/kona-pixel/read-shapes", test_read_shapes);
    qtest_add_func("sn0/origin2000/kona-pixel/read-write-state",
                   test_read_ignores_write_state);
    qtest_add_func("sn0/origin2000/kona-pixel/read-gc-predicates",
                   test_read_gc_predicates);
    qtest_add_func("sn0/origin2000/kona-pixel/read-setup-refused",
                   test_read_setup_refused);
    qtest_add_func("sn0/origin2000/kona-pixel/read-coordinates-refused",
                   test_read_coordinates_refused);
    qtest_add_data_func("sn0/origin2000/kona-pixel/read-direct-tag-refused",
                        GINT_TO_POINTER(false), test_read_wrong_tag);
    qtest_add_data_func("sn0/origin2000/kona-pixel/read-textport-tag-refused",
                        GINT_TO_POINTER(true), test_read_wrong_tag);
    qtest_add_func("sn0/origin2000/kona-pixel/read-context-pages",
                   test_read_context_pages);
    qtest_add_func("sn0/origin2000/kona-pixel/read-context-eviction",
                   test_read_context_eviction);
    qtest_add_func("sn0/origin2000/kona-pixel/read-inside-write",
                   test_read_inside_write);
    qtest_add_func("sn0/origin2000/kona-pixel/read-dma-write-refused",
                   test_read_open_dma_write_refused);
    qtest_add_func("sn0/origin2000/kona-pixel/read-pending-run-refused",
                   test_read_pending_run_refused);
    qtest_add_data_func("sn0/origin2000/kona-pixel/read-state-migrates",
                        GUINT_TO_POINTER(0), test_read_state_migrates);
    qtest_add_data_func("sn0/origin2000/kona-pixel/read-partial-migrates",
                        GUINT_TO_POINTER(1), test_read_state_migrates);
    qtest_add_data_func("sn0/origin2000/kona-pixel/read-empty-flags-migrate",
                        GUINT_TO_POINTER(2), test_read_state_migrates);
    qtest_add_func("sn0/origin2000/kona-pixel/read-reset-partial",
                   test_read_reset_partial);

    qtest_add_func("sn0/origin2000/kona-pixel/read-bank1-window",
                   test_read_bank1_window);
    qtest_add_func("sn0/origin2000/kona-pixel/read-partial-replacement",
                   test_read_partial_replacement);
    qtest_add_func("sn0/origin2000/kona-pixel/read-reply-order",
                   test_read_reply_order);
    qtest_add_func("sn0/origin2000/kona-pixel/read-payload-failure",
                   test_read_payload_failure);

    qtest_add_func("sn0/origin2000/kona-pixel/read-page-transitions",
                   test_read_page_transitions);
    qtest_add_data_func(
                        "sn0/origin2000/kona-pixel/read-mismatched-page-migrates",
                        GINT_TO_POINTER(false),
                        test_read_mismatched_page_migrates);
    qtest_add_data_func(
                        "sn0/origin2000/kona-pixel/read-mismatched-loads-migrate",
                        GINT_TO_POINTER(true),
                        test_read_mismatched_page_migrates);

    qtest_add_data_func("sn0/origin2000/kona-pixel/read-overflow-gc-reload",
                        GINT_TO_POINTER(false),
                        test_read_overflow_requires_reload);
    qtest_add_data_func("sn0/origin2000/kona-pixel/read-overflow-setup-reload",
                        GINT_TO_POINTER(true),
                        test_read_overflow_requires_reload);

    qtest_add_data_func(
        "sn0/origin2000/kona-pixel/read-framing-abort/dma-finish",
        GUINT_TO_POINTER(0), test_read_bulk_abort_requires_reload);
    qtest_add_data_func(
        "sn0/origin2000/kona-pixel/read-framing-abort/bitmap-bound",
        GUINT_TO_POINTER(1), test_read_bulk_abort_requires_reload);
    qtest_add_data_func(
        "sn0/origin2000/kona-pixel/read-framing-abort/video-bound",
        GUINT_TO_POINTER(2), test_read_bulk_abort_requires_reload);
    qtest_add_data_func(
        "sn0/origin2000/kona-pixel/read-framing-abort/dma-replace",
        GUINT_TO_POINTER(3), test_read_bulk_abort_requires_reload);
    qtest_add_func("sn0/origin2000/kona-pixel/read-empty-bitmap-retains-state",
                   test_read_empty_bitmap_retains_state);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
