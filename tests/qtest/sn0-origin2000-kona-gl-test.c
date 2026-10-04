/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA GL drawing qtests.
 *
 * Covers what the GL client's drawing commands put in the colour banks:
 * the window the kernel's 0x1f3 and 0x1ee place each GL context in, the
 * clear and the flat triangle the minimal GLX probe draws, the swap of a
 * double-buffered context and the GL state a migration carries. Each case
 * drives the command page the way the kernel and the GL client do and
 * reads back what the display shows: the probe's cases compare a
 * screendump of the window, byte for byte, with tests/data/kona's
 * gl-probe-*.ppm, the windows the OpenGL 1.1 polygon rule gives, which
 * no capture of a real pipe has confirmed.
 *
 * The fixture is KONA as a -device at the empty slot IO4, crossbow0's
 * widget 11, with fabric-hub-widget=9 so the board's writes reach node
 * 0's memory, on a machine with one node board: the head sees nothing of
 * the other three, and one node starts in a tenth of the time.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

#define SMALL_WINDOW_SIZE UINT64_C(0x1000000)
#define KONA_WIDGET 11
#define KONA_BASE   (SPACE_IO + (uint64_t)KONA_WIDGET * SMALL_WINDOW_SIZE)
#define KONA_REG(offset) (KONA_BASE + (offset))

#define REG_KICK            KONA_REG(0xc00108)
#define REG_COMMAND         KONA_REG(0xc10000)
#define REG_RETRACE_TAG     KONA_REG(0x800248)

/*
 * Virtual time from one retrace to just past the next: the model's 60 Hz
 * field is 16.67 ms. Stepping 20 ms from a point between retraces crosses
 * exactly one when the step starts within 13.3 ms after a retrace.
 */
#define RETRACE_STEP_NS     (20 * 1000 * 1000)

#define KONA_QOM_PATH "/machine/module0/kona0"

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4,fabric-hub-widget=9"

/* Repeated as literals: a test that borrowed the model's could not fail. */
#define FB_WIDTH  1280
#define FB_HEIGHT 1024

/* Node 0 guest RAM: the GL context's save area and return page. */
#define SAVE_AREA   UINT64_C(0x340000)
#define RETURN_PAGE UINT64_C(0x320000)

/* The probe's window: 128 pixels square, its left column at 256. */
#define PROBE_SIZE  128
#define PROBE_X     256

/* IEEE single-precision 0.0 and 1.0, as the client stores them. */
#define F0 0x00000000u
#define F1 0x3f800000u

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

/* The pixel the display shows at plane (x, y), rows counted upward. */
static uint32_t shown_pixel(QTestState *s, uint32_t x, uint32_t y)
{
    kona_qom_set_uint(s, "x-test-pixel-x", x);
    kona_qom_set_uint(s, "x-test-pixel-y", y);
    return kona_qom_get_uint(s, "x-test-pixel") & 0xffffff;
}

/* One command on the +0xc10000 port, as the GL client's doorbell sends. */
static void kona_command(QTestState *s, uint32_t token, const uint32_t *args,
                         unsigned argc)
{
    qtest_writel(s, REG_COMMAND, token);
    for (unsigned i = 0; i < argc; i++) {
        qtest_writel(s, REG_COMMAND, args[i]);
    }
}

/* The kernel's bracket: kick 0x30000, token, kick 0x10000, words, kick 0. */
static void kona_command_kicked(QTestState *s, uint32_t token,
                                const uint32_t *args, unsigned argc)
{
    qtest_writel(s, REG_KICK, 0x30000);
    qtest_writel(s, REG_COMMAND, token);
    qtest_writel(s, REG_KICK, 0x10000);
    for (unsigned i = 0; i < argc; i++) {
        qtest_writel(s, REG_COMMAND, args[i]);
    }
    qtest_writel(s, REG_KICK, 0);
}

/* A 0x1f2 or 0x1fd page word and tagged offset word for a physical page. */
static uint32_t physical_page_word(uint64_t addr)
{
    return 0x10000000 | (uint32_t)(addr >> 12);
}

static uint32_t tagged_offset_word(uint64_t addr)
{
    return 0x9000 | (uint32_t)(addr & 0xfff);
}

/*
 * The GL context's first make-current: KONApcxswap's 0x1fd and 0x1f2,
 * then the client's buffer configuration for its visual, single- or
 * double-buffered with 12-bit components, and the pack word.
 */
static void gl_make_current(QTestState *s, bool double_buffered)
{
    uint32_t config[10] = { 0, 0xc, 0, double_buffered };

    kona_command_kicked(s, 0x1fd,
                        (uint32_t[]){ physical_page_word(SAVE_AREA),
                                      tagged_offset_word(SAVE_AREA),
                                      0x10000 }, 3);
    kona_command_kicked(s, 0x1f2,
                        (uint32_t[]){ physical_page_word(RETURN_PAGE),
                                      tagged_offset_word(RETURN_PAGE),
                                      0x4000 }, 3);
    kona_command(s, 1, config, 10);
    kona_command(s, 2, (uint32_t[]){ 0 }, 1);
}

/*
 * kona_act_pcx's 0x1f3 and 0x1ee for a window whose lower-left pixel is
 * at plane (x, y), as the traced run's words carry it: the corner in
 * words 0 and 1 and again in 4 and 5, the size in words 6 and 7, the
 * other words as that run sent them.
 */
static void gl_place_window(QTestState *s, uint32_t x, uint32_t y,
                            uint32_t size)
{
    kona_command_kicked(s, 0x1f3,
                        (uint32_t[]){ x, y, 0x05670080, 0x05670080, x, y,
                                      size, size, 0, 0xd, 0 }, 11);
    kona_command_kicked(s, 0x1ee, (uint32_t[]){ x, y, 0 }, 3);
}

/* The probe's state and clear: viewport, scissor, dither off, colour. */
static void gl_clear(QTestState *s, uint32_t r, uint32_t g, uint32_t b)
{
    kona_command(s, 0xe3, (uint32_t[]){ 0, 0, PROBE_SIZE, PROBE_SIZE }, 4);
    kona_command(s, 0xc, (uint32_t[]){ 0, 0, PROBE_SIZE, PROBE_SIZE }, 4);
    kona_command(s, 0x25, (uint32_t[]){ 0xbd0 }, 1);
    kona_command(s, 0x1d, (uint32_t[]){ r, g, b, F1 }, 4);
    kona_command(s, 0x1a, (uint32_t[]){ 0x4000 }, 1);
}

static uint32_t guest_word(QTestState *s, uint64_t addr)
{
    uint8_t bytes[4];

    qtest_memread(s, addr, bytes, sizeof(bytes));
    return ldl_be_p(bytes);
}

/* A single float's bits. */
static uint32_t float_bits(float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

/*
 * The probe's normalized device coordinate for window coordinate v of
 * its 128-pixel window under identity matrices: exact in single
 * precision for its whole-unit vertices.
 */
static uint32_t ndc(int v)
{
    return float_bits((2.0f * v - PROBE_SIZE) / PROBE_SIZE);
}

/* The probe's flat yellow triangle from window vertices v. */
static void gl_triangle(QTestState *s, const int v[3][2])
{
    kona_command(s, 0xc0, (uint32_t[]){ 0x1d00 }, 1);
    kona_command(s, 0x63, (uint32_t[]){ F1, F1, F0 }, 3);
    kona_command(s, 0x5b, (uint32_t[]){ 0 }, 1);
    for (int i = 0; i < 3; i++) {
        kona_command(s, 0x8c, (uint32_t[]){ ndc(v[i][0]), ndc(v[i][1]) }, 2);
    }
    kona_command(s, 0x71, (uint32_t[]){ 0 }, 1);
}

/* glFlush and glGetError, whose answer, no error, is on the return page. */
static void gl_flush_and_check_error(QTestState *s)
{
    kona_command(s, 0xc7, (uint32_t[]){ 0 }, 1);
    kona_command(s, 0x3e, (uint32_t[]){ 0xdbdbdbdb }, 1);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), !=, 0);
    g_assert_cmphex(guest_word(s, RETURN_PAGE + 8), ==, 0);
}

/*
 * The pixels of a binary PPM, its header's comment lines skipped; its
 * size must be width by height with a maximum of 255.
 */
static uint8_t *ppm_pixels(const char *data, gsize len, unsigned width,
                           unsigned height)
{
    const char *p = data, *end = data + len;
    unsigned v[3];

    g_assert_true(len > 2 && !memcmp(p, "P6", 2));
    p += 2;
    for (int i = 0; i < 3; i++) {
        const char *next;
        unsigned long value;

        while (p < end && (g_ascii_isspace(*p) || *p == '#')) {
            if (*p == '#') {
                while (p < end && *p != '\n') {
                    p++;
                }
            } else {
                p++;
            }
        }
        g_assert_cmpint(qemu_strtoul(p, &next, 10, &value), ==, 0);
        g_assert_cmpuint(value, <=, UINT_MAX);
        v[i] = value;
        p = next;
    }
    g_assert_cmpuint(v[0], ==, width);
    g_assert_cmpuint(v[1], ==, height);
    g_assert_cmpuint(v[2], ==, 255);
    g_assert_true(p < end && g_ascii_isspace(*p));
    p++;
    g_assert_cmpuint(end - p, ==, (gsize)width * height * 3);
    return g_memdup2(p, end - p);
}

/* The console as a screendump's pixels, FB_WIDTH by FB_HEIGHT. */
static uint8_t *kona_screendump(QTestState *s)
{
    g_autofree char *dir = NULL;
    g_autofree char *path = NULL;
    g_autofree char *data = NULL;
    GError *err = NULL;
    gsize len = 0;

    dir = g_dir_make_tmp("sn0-kona-gl-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(dir, "frame.ppm", NULL);
    qtest_qmp_assert_success(s,
        "{'execute': 'screendump', 'arguments': {'filename': %s, "
        "'device': 'kona0'}}", path);
    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    g_unlink(path);
    g_rmdir(dir);
    return ppm_pixels(data, len, FB_WIDTH, FB_HEIGHT);
}

/*
 * The probe-sized window whose lower-left pixel is at plane (x, y), as
 * the display shows it, compared byte for byte with expected image
 * `name`; the pixels just outside the window's four sides stay black.
 */
static void assert_window_shows(QTestState *s, uint32_t x, uint32_t y,
                                const char *name)
{
    g_autofree char *path = g_test_build_filename(G_TEST_DIST, "..", "data",
                                                  "kona", name, NULL);
    g_autofree char *data = NULL;
    g_autofree uint8_t *expected = NULL;
    g_autofree uint8_t *frame = kona_screendump(s);
    g_autofree uint8_t *window = g_malloc(PROBE_SIZE * PROBE_SIZE * 3);
    const unsigned top = FB_HEIGHT - 1 - (y + PROBE_SIZE - 1);
    const size_t row = FB_WIDTH * 3;
    gsize len = 0;

    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    expected = ppm_pixels(data, len, PROBE_SIZE, PROBE_SIZE);
    for (unsigned r = 0; r < PROBE_SIZE; r++) {
        memcpy(window + r * PROBE_SIZE * 3,
               frame + (top + r) * row + x * 3, PROBE_SIZE * 3);
    }
    g_assert_cmpmem(window, PROBE_SIZE * PROBE_SIZE * 3, expected,
                    PROBE_SIZE * PROBE_SIZE * 3);
    for (unsigned r = 0; r < PROBE_SIZE; r++) {
        static const uint8_t black[3];

        g_assert_cmpmem(frame + (top + r) * row + (x - 1) * 3, 3, black, 3);
        g_assert_cmpmem(frame + (top + r) * row + (x + PROBE_SIZE) * 3, 3,
                        black, 3);
        g_assert_cmpmem(frame + (top - 1) * row + (x + r) * 3, 3, black, 3);
        g_assert_cmpmem(frame + (top + PROBE_SIZE) * row + (x + r) * 3, 3,
                        black, 3);
    }
}

/* A clean machine for each case. */
static void gl_reset(void)
{
    qtest_system_reset(qts);
    qtest_memset(qts, RETURN_PAGE, 0, 0x4000);
}

/*
 * The window lands where the 0x1f3 corner says, read as the left column
 * and the lower edge counted up from the bottom line: a window placed at
 * column 200, 596 rows up, whose X position is 200, 300, clears exactly
 * the 128 by 128 pixels from there. The two coordinates differ, so a
 * model that took the second word as a row counted down, or swapped the
 * two, clears somewhere else. Which reading the pipe uses is inferred
 * from one run whose window had both coordinates 256; a capture of this
 * placement would confirm it.
 */
static void test_window_placement_asymmetric(void)
{
    const uint32_t x = 200, y = FB_HEIGHT - 300 - PROBE_SIZE;

    gl_reset();
    gl_make_current(qts, false);
    gl_place_window(qts, x, y, PROBE_SIZE);
    gl_clear(qts, F1, F0, F1);

    g_assert_cmphex(shown_pixel(qts, x, y), ==, 0xff00ff);
    g_assert_cmphex(shown_pixel(qts, x + PROBE_SIZE - 1,
                                y + PROBE_SIZE - 1), ==, 0xff00ff);
    g_assert_cmphex(shown_pixel(qts, x - 1, y), ==, 0);
    g_assert_cmphex(shown_pixel(qts, x, y - 1), ==, 0);
    g_assert_cmphex(shown_pixel(qts, x + PROBE_SIZE, y), ==, 0);
    g_assert_cmphex(shown_pixel(qts, x, y + PROBE_SIZE), ==, 0);
}

/*
 * A context with no window yet draws nothing: the clear before 0x1f3
 * leaves the screen as it was, and the same clear after it fills the
 * window. A 0x1ee alone moves the corner and keeps the size.
 */
static void test_window_needed_and_moved(void)
{
    gl_reset();
    gl_make_current(qts, false);
    gl_clear(qts, F1, F0, F1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0);

    gl_place_window(qts, PROBE_X, 640, PROBE_SIZE);
    kona_command_kicked(qts, 0x1ee, (uint32_t[]){ 16, 32, 0 }, 3);
    gl_clear(qts, F0, F1, F0);
    g_assert_cmphex(shown_pixel(qts, 16, 32), ==, 0x00ff00);
    g_assert_cmphex(shown_pixel(qts, 16 + PROBE_SIZE - 1,
                                32 + PROBE_SIZE - 1), ==, 0x00ff00);
    g_assert_cmphex(shown_pixel(qts, 16 + PROBE_SIZE, 32), ==, 0);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0);
}

/*
 * A swap as the GL client and the kernel send it: the client's 0xc6 0,
 * then KONAschedswapbuf's packets, the retrace request 0x1e7 in a
 * bracket of its own, as the traced run sent them.
 */
static void gl_swap(QTestState *s, uint32_t tag)
{
    kona_command(s, 0xc6, (uint32_t[]){ 0 }, 1);
    kona_command_kicked(s, 0x1f4, (uint32_t[]){ 1, 1 }, 2);
    kona_command_kicked(s, 0x172, (uint32_t[]){ 0 }, 1);
    kona_command_kicked(s, 0x1e6, (uint32_t[]){ 0x800003, 0xd, 5 }, 3);
    kona_command_kicked(s, 0x1e7, (uint32_t[]){ tag }, 1);
}

/*
 * A double-buffered context draws into the bank its window does not
 * show: its clear is invisible until the retrace that honours the swap's
 * 0x1e7, which shows it. The next clear goes to the bank shown before,
 * now the back buffer, and appears only at the next swap. A model that
 * drew into the shown bank shows the first clear at once; one that never
 * exchanged the banks never shows it.
 */
static void test_swap_exchanges_banks(void)
{
    const uint32_t y = 640;

    gl_reset();
    gl_make_current(qts, true);
    gl_place_window(qts, PROBE_X, y, PROBE_SIZE);
    gl_clear(qts, F1, F0, F1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, y), ==, 0);

    gl_swap(qts, 1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, y), ==, 0);
    qtest_clock_step(qts, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(qts, REG_RETRACE_TAG), ==, 1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, y), ==, 0xff00ff);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + PROBE_SIZE - 1,
                                y + PROBE_SIZE - 1), ==, 0xff00ff);
    g_assert_cmphex(shown_pixel(qts, PROBE_X - 1, y), ==, 0);

    gl_clear(qts, F0, F1, F0);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, y), ==, 0xff00ff);
    gl_swap(qts, 2);
    qtest_clock_step(qts, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(qts, REG_RETRACE_TAG), ==, 2);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, y), ==, 0x00ff00);
}

/* A depth-only clear must preserve every colour sample. */
static void test_depth_clear_keeps_colour(void)
{
    gl_reset();
    gl_make_current(qts, false);
    gl_place_window(qts, PROBE_X, 640, PROBE_SIZE);
    gl_clear(qts, F1, F0, F1);
    kona_command(qts, 0x1d, (uint32_t[]){ F0, F1, F0, F1 }, 4);
    kona_command(qts, 0x1a, (uint32_t[]){ 0x100 }, 1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0xff00ff);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + 64, 704), ==, 0xff00ff);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + PROBE_SIZE - 1,
                                640 + PROBE_SIZE - 1), ==, 0xff00ff);
}

/* Paint one X pixel red in bank 0 under the GL window. */
static void x_repaint(QTestState *s)
{
    const uint32_t top = FB_HEIGHT - 1 - 640;
    const uint32_t gc[] = {
        0xff000, 0, 0, 0, 0, 0xfc000, 8, 1, 0, 6, 0, 1, 1,
    };

    kona_command(s, 0x12a, (uint32_t[]){ 0, 0, FB_WIDTH, FB_HEIGHT }, 4);
    kona_command(s, 0x12b, (uint32_t[]){ 1, 0, 0, FB_WIDTH, FB_HEIGHT }, 5);
    qtest_writel(s, KONA_REG(0xc1000c + 0x20 * 0x12e), 0x2dff);
    qtest_writel(s, KONA_REG(0xc1000c), 1);
    qtest_writel(s, KONA_REG(0xc1000c), 0x3fc00000);
    kona_command(s, 0x129, gc, ARRAY_SIZE(gc));
    kona_command(s, 0x154, (uint32_t[]){ (PROBE_X << 16) | top,
                    ((PROBE_X + 1) << 16) | (top + 1) }, 2);
}

static void gl_show_bank1(QTestState *s)
{
    gl_make_current(s, true);
    gl_place_window(s, PROBE_X, 640, PROBE_SIZE);
    gl_clear(s, F1, F0, F1);
    gl_swap(s, 1);
    qtest_clock_step(s, RETRACE_STEP_NS);
    g_assert_cmphex(shown_pixel(s, PROBE_X, 640), ==, 0xff00ff);
}

/* Only the destroy's exact reason releases the current window. */
static void test_destroy_releases_window(void)
{
    gl_reset();
    gl_show_bank1(qts);
    x_repaint(qts);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0xff00ff);
    kona_command_kicked(qts, 0x1f8, (uint32_t[]){ 0x102 }, 1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0xff00ff);
    kona_command_kicked(qts, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    x_repaint(qts);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0xfe0000);

    gl_reset();
    gl_show_bank1(qts);
    qtest_system_reset(qts);
    x_repaint(qts);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0xfe0000);
}

/* A switched-out context still owns its displayed window. */
static void test_deactivate_keeps_window(void)
{
    gl_reset();
    gl_show_bank1(qts);
    kona_command_kicked(qts, 0x1fc, (uint32_t[]){ 1 }, 1);
    x_repaint(qts);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0xff00ff);

    /* With no context current, mop-up cannot identify this old window. */
    kona_command_kicked(qts, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0xff00ff);
}

/* A migration keeps the window's lifetime on either side of destroy. */
static void test_destroy_window_migrates(gconstpointer opaque)
{
    bool destroy_first = GPOINTER_TO_INT(opaque);
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-destroy-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MACHINE_ARGS);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    gl_show_bank1(from);
    if (destroy_first) {
        kona_command_kicked(from, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    }
    sgi_migrate_stopped(from, uri);
    if (!destroy_first) {
        g_assert_cmphex(shown_pixel(to, PROBE_X, 640), ==, 0xff00ff);
        kona_command_kicked(to, 0x1f8, (uint32_t[]){ 0x103 }, 1);
    }
    x_repaint(to);
    g_assert_cmphex(shown_pixel(to, PROBE_X, 640), ==, 0xfe0000);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * The probe's clear case: the 128-pixel window at X position 256, 256,
 * 640 rows up from the bottom line, cleared magenta.
 */
static void test_probe_clear(void)
{
    gl_reset();
    gl_make_current(qts, false);
    gl_place_window(qts, PROBE_X, 640, PROBE_SIZE);
    gl_clear(qts, F1, F0, F1);
    gl_flush_and_check_error(qts);
    assert_window_shows(qts, PROBE_X, 640, "gl-probe-clear.ppm");
}

/* The probe's triangle cases, each over a blue clear. */
static void probe_triangle(const int v[3][2], const char *name)
{
    gl_reset();
    gl_make_current(qts, false);
    gl_place_window(qts, PROBE_X, 640, PROBE_SIZE);
    gl_clear(qts, F0, F0, F1);
    gl_triangle(qts, v);
    gl_flush_and_check_error(qts);
    assert_window_shows(qts, PROBE_X, 640, name);
}

static void test_probe_triangle(void)
{
    static const int v[3][2] = { { 16, 16 }, { 112, 24 }, { 56, 111 } };

    probe_triangle(v, "gl-probe-triangle.ppm");
}

/* The third vertex moved: 3,888 pixels where the first drew 4,400. */
static void test_probe_triangle_moved(void)
{
    static const int v[3][2] = { { 16, 16 }, { 112, 24 }, { 88, 103 } };

    probe_triangle(v, "gl-probe-triangle-moved.ppm");
}

/* The vertices in the other order: the same pixels, no culling. */
static void test_probe_triangle_reversed(void)
{
    static const int v[3][2] = { { 16, 16 }, { 56, 111 }, { 112, 24 } };

    probe_triangle(v, "gl-probe-triangle-reversed.ppm");
}

/*
 * The clear case with a double-buffered visual: the window stays black
 * until the swap's 0x1e7 has been honoured at a retrace, and then shows
 * the clear.
 */
static void test_probe_clear_double_buffered(void)
{
    g_autofree uint8_t *frame = NULL;
    static const uint8_t black[PROBE_SIZE * 3];
    const unsigned top = FB_HEIGHT - 1 - (640 + PROBE_SIZE - 1);

    gl_reset();
    gl_make_current(qts, true);
    gl_place_window(qts, PROBE_X, 640, PROBE_SIZE);
    gl_clear(qts, F1, F0, F1);
    gl_flush_and_check_error(qts);
    gl_swap(qts, 7);
    frame = kona_screendump(qts);
    for (unsigned r = 0; r < PROBE_SIZE; r++) {
        g_assert_cmpmem(frame + ((top + r) * FB_WIDTH + PROBE_X) * 3,
                        PROBE_SIZE * 3, black, PROBE_SIZE * 3);
    }
    qtest_clock_step(qts, RETRACE_STEP_NS);
    g_assert_cmphex(qtest_readl(qts, REG_RETRACE_TAG), ==, 7);
    assert_window_shows(qts, PROBE_X, 640, "gl-probe-clear.ppm");
}

/*
 * The clear case with the window at X position 200, 300: the same image,
 * moved to where the model's reading of 0x1f3 puts it.
 */
static void test_probe_clear_asymmetric(void)
{
    const uint32_t y = FB_HEIGHT - 300 - PROBE_SIZE;

    gl_reset();
    gl_make_current(qts, false);
    gl_place_window(qts, 200, y, PROBE_SIZE);
    gl_clear(qts, F1, F0, F1);
    gl_flush_and_check_error(qts);
    assert_window_shows(qts, 200, y, "gl-probe-clear.ppm");
}

/*
 * A migration carries each context's GL state and window: the clear
 * colour and viewport the source set, and the window it placed, so a
 * clear sent only to the destination fills the probe's window in the
 * source's colour. A destination that lost any of them clears nothing,
 * or in black.
 */
static void test_migration_of_gl_state(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-gl-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_ARGS);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);

    gl_make_current(from, false);
    gl_place_window(from, PROBE_X, 640, PROBE_SIZE);
    kona_command(from, 0xe3, (uint32_t[]){ 0, 0, PROBE_SIZE, PROBE_SIZE },
                 4);
    kona_command(from, 0x1d, (uint32_t[]){ F0, F1, F1, F1 }, 4);

    sgi_migrate_stopped(from, uri);

    kona_command(to, 0x1a, (uint32_t[]){ 0x4000 }, 1);
    g_assert_cmphex(shown_pixel(to, PROBE_X, 640), ==, 0x00ffff);
    g_assert_cmphex(shown_pixel(to, PROBE_X + PROBE_SIZE - 1,
                                640 + PROBE_SIZE - 1), ==, 0x00ffff);
    g_assert_cmphex(shown_pixel(to, PROBE_X - 1, 640), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sn0/origin2000/kona-gl/window-placement-asymmetric",
                   test_window_placement_asymmetric);
    qtest_add_func("sn0/origin2000/kona-gl/window-needed-and-moved",
                   test_window_needed_and_moved);
    qtest_add_func("sn0/origin2000/kona-gl/swap-exchanges-banks",
                   test_swap_exchanges_banks);
    qtest_add_func("sn0/origin2000/kona-gl/probe-clear", test_probe_clear);
    qtest_add_func("sn0/origin2000/kona-gl/probe-triangle",
                   test_probe_triangle);
    qtest_add_func("sn0/origin2000/kona-gl/probe-triangle-moved",
                   test_probe_triangle_moved);
    qtest_add_func("sn0/origin2000/kona-gl/probe-triangle-reversed",
                   test_probe_triangle_reversed);
    qtest_add_func("sn0/origin2000/kona-gl/probe-clear-double-buffered",
                   test_probe_clear_double_buffered);
    qtest_add_func("sn0/origin2000/kona-gl/probe-clear-asymmetric",
                   test_probe_clear_asymmetric);
    qtest_add_func("sn0/origin2000/kona-gl/migration-of-gl-state",
                   test_migration_of_gl_state);

    qtest_add_func("sn0/origin2000/kona-gl/destroy-releases-window",
                   test_destroy_releases_window);
    qtest_add_func("sn0/origin2000/kona-gl/deactivate-keeps-window",
                   test_deactivate_keeps_window);
    qtest_add_data_func("sn0/origin2000/kona-gl/destroy-after-migration",
                        GINT_TO_POINTER(false), test_destroy_window_migrates);
    qtest_add_data_func("sn0/origin2000/kona-gl/destroy-before-migration",
                        GINT_TO_POINTER(true), test_destroy_window_migrates);

    qtest_add_func("sn0/origin2000/kona-gl/depth-clear-keeps-colour",
                   test_depth_clear_keeps_colour);

    qts = qtest_init(MACHINE_ARGS);
    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
