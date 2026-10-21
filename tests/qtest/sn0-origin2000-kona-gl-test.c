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
#define XY(x, y) (((uint32_t)(x) << 16) | (uint32_t)(y))

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

static uint32_t bank_word(QTestState *s, unsigned bank, uint32_t x,
                          uint32_t y)
{
    kona_qom_set_uint(s, "x-test-plane", bank);
    kona_qom_set_uint(s, "x-test-pixel-x", x);
    kona_qom_set_uint(s, "x-test-pixel-y", y);
    return kona_qom_get_uint(s, "x-test-plane-word");
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
 * The X server's display-ID fill of the box from (x1, y1) to (x2, y2),
 * rows counted down, ends exclusive: whole-screen position and clip,
 * then a display-ID GC (cfg 8) writing id into bits 7:2, as kon_drawDid
 * sends it.
 */
static void x_paint_display_id(QTestState *s, uint32_t x1, uint32_t y1,
                               uint32_t x2, uint32_t y2, uint32_t id)
{
    const uint32_t value = id << 2;
    const uint32_t gc[13] = {
        ((value & 15) << 3) | ((value & 0xfff0) << 4), 0, 0, 0,
        0xf00fff, 0, 8, 1, 0, 6, 0, 1, 1,
    };

    kona_command(s, 0x12a, (uint32_t[]){ 0, 0, FB_WIDTH, FB_HEIGHT }, 4);
    kona_command(s, 0x12b, (uint32_t[]){ 1, 0, 0, FB_WIDTH, FB_HEIGHT }, 5);
    kona_command(s, 0x129, gc, ARRAY_SIZE(gc));
    kona_command(s, 0x154, (uint32_t[]){ XY(x1, y1), XY(x2, y2) }, 2);
}

/*
 * kona_act_pcx's 0x1f3 and 0x1ee for a window whose lower-left pixel is
 * at plane (x, y), as the traced run's words carry it: the corner in
 * words 0 and 1 and again in 4 and 5, the size in words 6 and 7, the
 * other words as that run sent them. The X server first gives the
 * window's pixels its display ID 0xd, as it does when it maps a window.
 */
static void gl_place_window(QTestState *s, uint32_t x, uint32_t y,
                            uint32_t size)
{
    x_paint_display_id(s, x, FB_HEIGHT - y - size, x + size,
                       FB_HEIGHT - y, 0xd);
    kona_command_kicked(s, 0x1f3,
                        (uint32_t[]){ x, y, 0x05670080, 0x05670080, x, y,
                                      size, size, 0, 0xd, 0 }, 11);
    kona_command_kicked(s, 0x1ee, (uint32_t[]){ x, y, 0 }, 3);
}

/*
 * 0x1f3 and 0x1ee for the probe-sized window at plane (x, y) with
 * display ID id and word 10 partial, as the runs' kernel sent them for
 * a window another one covers in part.
 */
static void gl_place_window_id(QTestState *s, uint32_t x, uint32_t y,
                               uint32_t id, uint32_t partial)
{
    kona_command_kicked(s, 0x1f3,
                        (uint32_t[]){ x, y, 0x05670080, 0x05670080, x, y,
                                      PROBE_SIZE, PROBE_SIZE, 0, id,
                                      partial }, 11);
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

/* Selector 3's bounded RGB geometry policy must cover both colour banks. */
static void test_x_rgb_op3_both_banks(void)
{
    static const uint32_t green_gc[] = {
        0x0000007f, 0xf8001ffe, 0, 0x1ffe, 0xffffff, 0xffffff,
        4, 3, 1, 6, 0, 0, 0,
    };
    static const uint32_t observed_gc[] = {
        0x00056540, 0x42ae9ffe, 0, 0x1ffe, 0xffffff, 0xffffff,
        4, 3, 1, 6, 0, 0, 0,
    };
    static const uint32_t unsupported_gc[] = {
        0x00012000, 0x1ffe, 0, 0x1ffe, 0xf00fff, 0,
        0xc, 3, 1, 6, 0, 1, 1,
    };
    const uint32_t x = PROBE_X + 40;
    const uint32_t line = 320;
    const uint32_t y = FB_HEIGHT - 1 - line;

    gl_reset();
    gl_make_current(qts, true);
    gl_place_window(qts, PROBE_X, 640, PROBE_SIZE);
    gl_clear(qts, F0, F0, F1);
    gl_swap(qts, 1);
    qtest_clock_step(qts, RETRACE_STEP_NS);
    g_assert_cmphex(shown_pixel(qts, x, y), ==, 0x0000ff);
    gl_clear(qts, F1, F0, F0);
    g_assert_cmphex(bank_word(qts, 0, x, y), ==, 0xff0000);
    g_assert_cmphex(bank_word(qts, 1, x, y), ==, 0x0000ff);

    kona_command(qts, 0x12b,
                 (uint32_t[]){ 1, 288, 312, 304, 328 }, 5);
    kona_command(qts, 0x12a,
                 (uint32_t[]){ 0, 0, FB_WIDTH, FB_HEIGHT }, 4);
    kona_command(qts, 0x12d,
                 (uint32_t[]){ 288, 312, 16, 16 }, 4);
    kona_command(qts, 0x129, green_gc, ARRAY_SIZE(green_gc));
    kona_command(qts, 0x154,
                 (uint32_t[]){ XY(288, 312), XY(304, 328) }, 2);
    g_assert_cmphex(bank_word(qts, 0, x, y), ==, 0x00ff00);
    g_assert_cmphex(bank_word(qts, 1, x, y), ==, 0x00ff00);
    g_assert_cmphex(shown_pixel(qts, x, y), ==, 0x00ff00);
    g_assert_cmphex(bank_word(qts, 0, 286, y), ==, 0xff0000);
    g_assert_cmphex(bank_word(qts, 1, 286, y), ==, 0x0000ff);

    gl_swap(qts, 2);
    qtest_clock_step(qts, RETRACE_STEP_NS);
    g_assert_cmphex(shown_pixel(qts, x, y), ==, 0x00ff00);

    kona_command(qts, 0x129, observed_gc, ARRAY_SIZE(observed_gc));
    kona_command(qts, 0x154,
                 (uint32_t[]){ XY(288, 312), XY(304, 328) }, 2);
    g_assert_cmphex(bank_word(qts, 0, x, y), ==, 0x5680ab);
    g_assert_cmphex(bank_word(qts, 1, x, y), ==, 0x5680ab);
    g_assert_cmphex(shown_pixel(qts, x, y), ==, 0x5680ab);

    kona_command(qts, 0x129, unsupported_gc, ARRAY_SIZE(unsupported_gc));
    kona_command(qts, 0x154,
                 (uint32_t[]){ XY(288, 312), XY(304, 328) }, 2);
    g_assert_cmphex(bank_word(qts, 0, x, y), ==, 0x5680ab);
    g_assert_cmphex(bank_word(qts, 1, x, y), ==, 0x5680ab);
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
        0xfef00, 0, 0, 0, 0xffffff, 0xffffff, 4, 1, 1, 6, 0, 0, 0,
    };

    kona_command(s, 0x12a, (uint32_t[]){ 0, 0, FB_WIDTH, FB_HEIGHT }, 4);
    kona_command(s, 0x12b, (uint32_t[]){ 1, 0, 0, FB_WIDTH, FB_HEIGHT }, 5);
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

/*
 * A GL window another window partly covers draws only where the display-
 * ID planes show its own ID. The probe window at (256, 640) has ID 0xd;
 * the X server gives its top-right quadrant, plane columns 320 to 383
 * and rows 704 to 767, a covering window's ID 5. With 0x1f3's word 10
 * set, a clear fills the other three quadrants and leaves the covered
 * one as it was. In the runs the kernel's 0x1f3 came before the X
 * server's fills, so the clear follows the planes as they are when it
 * draws: once the quadrant shows ID 0xd again, the next clear fills it
 * too. With word 10 clear the window is wholly visible and its rectangle
 * is drawn whatever the planes hold. The ownership test is inferred from
 * the words the kernel and the X server sent in the runs; how the pipe
 * performs it is unknown.
 */
static void test_overlap_clipped_to_display_id(void)
{
    const uint32_t y = 640, top = FB_HEIGHT - y - PROBE_SIZE;

    gl_reset();
    gl_make_current(qts, false);
    gl_place_window_id(qts, PROBE_X, y, 0xd, 1);
    x_paint_display_id(qts, PROBE_X, top, PROBE_X + PROBE_SIZE,
                       top + PROBE_SIZE, 0xd);
    x_paint_display_id(qts, PROBE_X + 64, top, PROBE_X + PROBE_SIZE,
                       top + 64, 5);
    gl_clear(qts, F1, F0, F1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, y), ==, 0xff00ff);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + 63, y + PROBE_SIZE - 1),
                    ==, 0xff00ff);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + PROBE_SIZE - 1, y + 63),
                    ==, 0xff00ff);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + 64, y + 64), ==, 0);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + PROBE_SIZE - 1,
                                y + PROBE_SIZE - 1), ==, 0);

    x_paint_display_id(qts, PROBE_X + 64, top, PROBE_X + PROBE_SIZE,
                       top + 64, 0xd);
    gl_clear(qts, F0, F1, F0);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + 64, y + 64), ==, 0x00ff00);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, y), ==, 0x00ff00);

    x_paint_display_id(qts, PROBE_X + 64, top, PROBE_X + PROBE_SIZE,
                       top + 64, 5);
    gl_place_window_id(qts, PROBE_X, y, 0xd, 0);
    gl_clear(qts, F0, F0, F1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + 64, y + 64), ==, 0x0000ff);
}

/*
 * A double-buffered window showing bank 1 shows it only at the pixels
 * holding its display ID once another window covers part of it: there
 * the display shows bank 0, where the X server draws the covering
 * window. The covered pixel at the window's corner shows the X server's
 * red, and its neighbour still the GL window's magenta, before and after
 * the kernel's 0x1f3 setting word 10.
 */
static void test_overlap_shows_bank0_where_covered(void)
{
    const uint32_t top = FB_HEIGHT - 1 - 640;

    gl_reset();
    gl_show_bank1(qts);
    x_paint_display_id(qts, PROBE_X, top, PROBE_X + 1, top + 1, 5);
    x_repaint(qts);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0xfe0000);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + 1, 640), ==, 0xff00ff);
    gl_place_window_id(qts, PROBE_X, 640, 0xd, 1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0xfe0000);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + 1, 640), ==, 0xff00ff);
}

/*
 * A window whose owned pixels need more clip rectangles than a drawable
 * holds still keeps to them. twilight draws into the root window, which
 * owns the screen less every window above it; drawing its whole
 * rectangle overwrote those windows, and the X server, which thinks them
 * intact, never repainted them. Here 24 one-pixel holes with another
 * window's ID 5, one on each of 24 rows and in a different column, cut
 * the probe window into far more than 16 rectangles. A clear fills the
 * window but leaves every hole as it was.
 */
static void test_overlap_beyond_clip_list(void)
{
    const uint32_t y = 640, top = FB_HEIGHT - y - PROBE_SIZE;

    gl_reset();
    gl_make_current(qts, false);
    gl_place_window_id(qts, PROBE_X, y, 0xd, 1);
    x_paint_display_id(qts, PROBE_X, top, PROBE_X + PROBE_SIZE,
                       top + PROBE_SIZE, 0xd);
    for (uint32_t k = 0; k < 24; k++) {
        x_paint_display_id(qts, PROBE_X + 4 + 5 * k, top + 2 + 5 * k,
                           PROBE_X + 5 + 5 * k, top + 3 + 5 * k, 5);
    }
    gl_clear(qts, F1, F0, F1);
    for (uint32_t k = 0; k < 24; k++) {
        uint32_t hole_y = y + PROBE_SIZE - 3 - 5 * k;

        g_assert_cmphex(shown_pixel(qts, PROBE_X + 4 + 5 * k, hole_y), ==, 0);
        g_assert_cmphex(shown_pixel(qts, PROBE_X + 5 + 5 * k, hole_y), ==,
                        0xff00ff);
    }
    g_assert_cmphex(shown_pixel(qts, PROBE_X, y), ==, 0xff00ff);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + PROBE_SIZE - 1,
                                y + PROBE_SIZE - 1), ==, 0xff00ff);
}

/*
 * A window the X server has not given an ID yet comes in 0x1f3 with word
 * 9 all ones and word 10 set, as mag's first placement did while its
 * window was still unmapped at the screen's corner. It owns no pixel: a
 * clear then leaves the pixels beneath, here another window's ID 1 and
 * black. Once the kernel sends the window's ID 0xd, which the X server
 * has painted, the clear fills it.
 */
static void test_unassigned_id_owns_nothing(void)
{
    const uint32_t y = 640, top = FB_HEIGHT - y - PROBE_SIZE;

    gl_reset();
    gl_make_current(qts, false);
    x_paint_display_id(qts, PROBE_X, top, PROBE_X + PROBE_SIZE,
                       top + PROBE_SIZE, 1);
    gl_place_window_id(qts, PROBE_X, y, UINT32_MAX, 1);
    gl_clear(qts, F1, F0, F1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, y), ==, 0);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + PROBE_SIZE - 1,
                                y + PROBE_SIZE - 1), ==, 0);

    x_paint_display_id(qts, PROBE_X, top, PROBE_X + PROBE_SIZE,
                       top + PROBE_SIZE, 0xd);
    gl_place_window_id(qts, PROBE_X, y, 0xd, 0);
    gl_clear(qts, F1, F0, F1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, y), ==, 0xff00ff);
}

/*
 * A client killed with its window showing bank 1 sends no teardown; the
 * X server repaints the window's pixels with the IDs beneath, here the
 * root window's ID 1, and paints bank 0. The display then shows bank 0
 * there although the dead context still holds its window and its swap,
 * and the window's word 10 never changed.
 */
static void test_unmapped_window_shows_bank0(void)
{
    const uint32_t top = FB_HEIGHT - 640 - PROBE_SIZE;

    gl_reset();
    gl_show_bank1(qts);
    x_paint_display_id(qts, PROBE_X, top, PROBE_X + PROBE_SIZE,
                       top + PROBE_SIZE, 1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + PROBE_SIZE - 1,
                                640 + PROBE_SIZE - 1), ==, 0);
    x_repaint(qts);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0xfe0000);
}

/*
 * A double-buffered window showing bank 1 that the kernel places again
 * with word 9 all ones and word 10 set, the words for a window with no
 * display ID, shows nothing: no pixel holds that ID, as it owns none
 * (unassigned-id-owns-nothing). powerflip's context was left so after
 * it was killed, and its last frame stayed over the root and the file
 * manager, though the X server repainted them in bank 0.
 */
static void test_unassigned_id_shows_nothing(void)
{
    gl_reset();
    gl_show_bank1(qts);
    gl_place_window_id(qts, PROBE_X, 640, UINT32_MAX, 1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + PROBE_SIZE - 1,
                                640 + PROBE_SIZE - 1), ==, 0);
}

/*
 * The X server hands a dead client's display ID to the next window. hist
 * on brick.rgb, traced: ivview was killed showing bank 1 in a window
 * with ID 0xe and sent no teardown; the X server repainted the IDs
 * beneath, then mapped hist's single-buffered window over part of that
 * rectangle with the same ID 0xe. hist cleared bank 0, but its window
 * showed ivview's last frame from bank 1. The window placed last with an
 * ID is the one that ID shows: here a second, single-buffered context
 * placed with ID 0xd over a dead double-buffered one shows bank 0.
 */
#define SECOND_SAVE_AREA UINT64_C(0x360000)

/*
 * A second, single-buffered context, fresh from its own save area, placed
 * over the probe window with the same display ID 0xd and cleared green.
 */
static void gl_second_window_takes_id(QTestState *s)
{
    uint32_t config[10] = { 0, 0xc, 0, 0 };

    kona_command_kicked(s, 0x1fd,
                        (uint32_t[]){ physical_page_word(SECOND_SAVE_AREA),
                                      tagged_offset_word(SECOND_SAVE_AREA),
                                      0x10000 }, 3);
    kona_command(s, 1, config, 10);
    kona_command(s, 2, (uint32_t[]){ 0 }, 1);
    gl_place_window(s, PROBE_X, 640, PROBE_SIZE);
    gl_clear(s, F0, F1, F0);
}

static void test_reused_display_id_shows_new_window(void)
{
    const uint32_t top = FB_HEIGHT - 640 - PROBE_SIZE;

    gl_reset();
    gl_show_bank1(qts);
    x_paint_display_id(qts, PROBE_X, top, PROBE_X + PROBE_SIZE,
                       top + PROBE_SIZE, 1);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0);

    gl_second_window_takes_id(qts);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0x00ff00);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + PROBE_SIZE - 1,
                                640 + PROBE_SIZE - 1), ==, 0x00ff00);
}

/*
 * A display ID given to another window and then back. The double-buffered
 * window showing bank 1 is switched out, a single-buffered window takes its
 * ID 0xd and shows bank 0, and then the first context is restored and its
 * 0x1f3 carries 0xd again: the window placed last owns the ID, so the first
 * window's bank 1 shows once more.
 */
static void test_display_id_given_back_shows_bank1(void)
{
    gl_reset();
    gl_show_bank1(qts);
    kona_command_kicked(qts, 0x1fc, (uint32_t[]){ 1 }, 1);

    gl_second_window_takes_id(qts);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0x00ff00);
    kona_command_kicked(qts, 0x1fc, (uint32_t[]){ 1 }, 1);

    kona_command_kicked(qts, 0x1f9,
                        (uint32_t[]){ guest_word(qts, SAVE_AREA),
                                      physical_page_word(SAVE_AREA),
                                      tagged_offset_word(SAVE_AREA),
                                      0xedededed }, 4);
    gl_place_window(qts, PROBE_X, 640, PROBE_SIZE);
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0xff00ff);
    g_assert_cmphex(shown_pixel(qts, PROBE_X + PROBE_SIZE - 1,
                                640 + PROBE_SIZE - 1), ==, 0xff00ff);
}

/*
 * Two windows sharing display ID 0xd across a migration: the newer,
 * single-buffered one still owns the ID after the load, so the older
 * double-buffered window's bank 1 stays hidden. The order lives in the
 * windows' placement serials, which must migrate.
 */
static void test_shared_display_id_migrates(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-shared-id-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MACHINE_ARGS);
    dest_args = g_strdup_printf(MACHINE_ARGS " -incoming %s", uri);
    to = qtest_init(dest_args);
    gl_show_bank1(from);
    gl_second_window_takes_id(from);
    g_assert_cmphex(shown_pixel(from, PROBE_X, 640), ==, 0x00ff00);
    sgi_migrate_stopped(from, uri);
    g_assert_cmphex(shown_pixel(to, PROBE_X, 640), ==, 0x00ff00);
    g_assert_cmphex(shown_pixel(to, PROBE_X + PROBE_SIZE - 1,
                                640 + PROBE_SIZE - 1), ==, 0x00ff00);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * The X server's gamma load passes every displayed pixel through its
 * table. With an inverting red table (entry i is 0xfff - i) and no green
 * or blue table, a magenta clear shows as blue on the display: red 0xff
 * indexes entry 0xfff, which is 0. The stored colour stays magenta.
 */
static void test_gamma_applied_at_scanout(void)
{
    const unsigned top = FB_HEIGHT - 1 - 640;
    g_autofree uint8_t *frame = NULL;
    const uint8_t *px;

    gl_reset();
    gl_make_current(qts, false);
    gl_place_window(qts, PROBE_X, 640, PROBE_SIZE);
    gl_clear(qts, F1, F0, F1);
    qtest_writel(qts, REG_COMMAND, 0x12f);
    qtest_writel(qts, REG_COMMAND, 0);
    qtest_writel(qts, REG_COMMAND, 0x1000);
    for (unsigned i = 0; i < 0x1000; i++) {
        qtest_writel(qts, REG_COMMAND, 0xfff - i);
    }
    g_assert_cmphex(shown_pixel(qts, PROBE_X, 640), ==, 0xff00ff);
    frame = kona_screendump(qts);
    px = frame + ((size_t)top * FB_WIDTH + PROBE_X) * 3;
    g_assert_cmpuint(px[0], ==, 0);
    g_assert_cmpuint(px[1], ==, 0);
    g_assert_cmpuint(px[2], ==, 0xff);
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
    qtest_add_func("sn0/origin2000/kona-gl/x-rgb-op3-both-banks",
                   test_x_rgb_op3_both_banks);
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
    qtest_add_func("sn0/origin2000/kona-gl/overlap-clipped-to-display-id",
                   test_overlap_clipped_to_display_id);
    qtest_add_func("sn0/origin2000/kona-gl/overlap-beyond-clip-list",
                   test_overlap_beyond_clip_list);
    qtest_add_func("sn0/origin2000/kona-gl/unassigned-id-owns-nothing",
                   test_unassigned_id_owns_nothing);
    qtest_add_func("sn0/origin2000/kona-gl/"
                   "overlap-shows-bank0-where-covered",
                   test_overlap_shows_bank0_where_covered);
    qtest_add_func("sn0/origin2000/kona-gl/unassigned-id-shows-nothing",
                   test_unassigned_id_shows_nothing);
    qtest_add_func("sn0/origin2000/kona-gl/unmapped-window-shows-bank0",
                   test_unmapped_window_shows_bank0);
    qtest_add_func("sn0/origin2000/kona-gl/reused-display-id-shows-new-window",
                   test_reused_display_id_shows_new_window);
    qtest_add_func("sn0/origin2000/kona-gl/display-id-given-back-shows-bank1",
                   test_display_id_given_back_shows_bank1);
    qtest_add_func("sn0/origin2000/kona-gl/shared-display-id-migrates",
                   test_shared_display_id_migrates);
    qtest_add_func("sn0/origin2000/kona-gl/gamma-applied-at-scanout",
                   test_gamma_applied_at_scanout);

    qts = qtest_init(MACHINE_ARGS);
    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
