/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA qtests for the shared graphics layer the pipe is built on: the
 * board memory and planes (hw/display/sgi/board-memory.c) that hold its
 * pixels, the scanout (hw/display/sgi/scanout.c) that presents them,
 * marks the display lines drawing changed and runs the vertical retrace,
 * and the migration subsections that carry both. The drawing commands
 * themselves are owned by sn0-origin2000-kona-test.c and the DMA engine
 * by sn0-origin2000-kona-dma-test.c; here they are only the means of
 * changing pixels.
 *
 * The fixture is sn0-origin2000-kona-dma-test.c's: KONA as a -device at
 * the empty slot IO4, crossbow0's widget 11, with fabric-hub-widget=9 so
 * the DMA engine reaches node 0's memory.
 *
 * What the scanout did is read from its own trace events and from a
 * screendump of the pipe's console, not from the model's inspection
 * properties, so these cases check what the display shows.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

#define SMALL_WINDOW_SIZE UINT64_C(0x1000000)
#define KONA_WIDGET 11
#define KONA_BASE   (SPACE_IO + (uint64_t)KONA_WIDGET * SMALL_WINDOW_SIZE)
#define KONA_REG(offset) (KONA_BASE + (offset))

#define REG_DMA_FIFO_PUSH KONA_REG(0x80007c)
#define REG_TEXTPORT      KONA_REG(0xc10000)

#define KONA_QOM_PATH "/machine/module0/kona0"

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4," \
    "fabric-hub-widget=9"

/* The one-node machine is small enough to migrate quickly as it is. */
#define MIGRATION_ARGS MACHINE_ARGS

/* Repeated as literals: a test that borrowed the model's could not fail. */
#define KONA_FB_WIDTH  1280
#define KONA_FB_HEIGHT 1024
#define KONA_DMA_ADDR_TAG 0x9000u

/* A node 0 guest RAM address the DMA engine reads pixels from. */
#define DMA_SRC UINT64_C(0x9004)

/* Plane ids, in the order the model declares them. */
#define PLANE_FRONT     0
#define PLANE_FRONT_TAG 2

#define TAG_DIRECT      0
#define TAG_COLOR_INDEX 1

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

/* The resolved pixel at textport wire (x, y). */
static uint32_t kona_test_pixel(QTestState *s, uint32_t x, uint32_t y)
{
    kona_qom_set_uint(s, "x-test-pixel-x", x);
    kona_qom_set_uint(s, "x-test-pixel-y", y);
    return kona_qom_get_uint(s, "x-test-pixel");
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

/* A private temporary directory for one case's log and screendumps. */
static char *kona_tmpdir(void)
{
    GError *err = NULL;
    char *dir = g_dir_make_tmp("sn0-kona-scanout-XXXXXX", &err);

    g_assert_no_error(err);
    return dir;
}

static void kona_textport_push(QTestState *s, uint32_t selector,
                               const uint32_t *args, unsigned argc)
{
    qtest_writel(s, REG_TEXTPORT, 1);
    qtest_writel(s, REG_TEXTPORT, selector);
    for (unsigned i = 0; i < argc; i++) {
        qtest_writel(s, REG_TEXTPORT, args[i]);
    }
}

/* Colour-map entry `index` = 8-bit r, g, b, pre-scaled as the PROM sends. */
static void kona_load_colour(QTestState *s, uint32_t index, uint8_t r,
                             uint8_t g, uint8_t b)
{
    kona_textport_push(s, 0x3,
                       (uint32_t[]){ index, r << 2, g << 2, b << 2 }, 4);
}

/*
 * One line of `count` words through the DMA engine's packet: 0x1eb, the
 * buffer address split over two words, the total and per-line byte
 * counts, no gap, and 0x100 for a completion.
 */
static void kona_dma_packet(QTestState *s, uint32_t addr, unsigned count)
{
    qtest_qom_set_bool(s, KONA_QOM_PATH, "x-test-dma-framebuffer", true);
    qtest_writel(s, REG_DMA_FIFO_PUSH, 0x1eb);
    qtest_writel(s, REG_DMA_FIFO_PUSH, addr >> 12);
    qtest_writel(s, REG_DMA_FIFO_PUSH, (addr & 0xfffu) | KONA_DMA_ADDR_TAG);
    qtest_writel(s, REG_DMA_FIFO_PUSH, count * 4);
    qtest_writel(s, REG_DMA_FIFO_PUSH, count * 4);
    qtest_writel(s, REG_DMA_FIFO_PUSH, 0);
    qtest_writel(s, REG_DMA_FIFO_PUSH, 0x100);
}

/*
 * Move `count` words from guest RAM at DMA_SRC to the framebuffer's
 * first pixels (x-test-dma-fb-offset's default of 0: the top display
 * line's left end), through the DMA engine.
 */
static void kona_dma_pixels(QTestState *s, const uint32_t *words,
                            unsigned count)
{
    g_autofree uint8_t *buf = g_malloc(count * 4);
    uint32_t addr = (uint32_t)DMA_SRC;

    for (unsigned i = 0; i < count; i++) {
        stl_be_p(buf + i * 4, words[i]);
    }
    qtest_memwrite(s, DMA_SRC, buf, count * 4);
    kona_dma_packet(s, addr, count);
}

/* A node 0 guest RAM address the DMA engine writes pixels read back to. */
#define DMA_DST UINT64_C(0xa004)

/*
 * Read `count` pixels from the framebuffer's first pixels back to guest
 * RAM at DMA_DST, through the DMA engine's qtest-only reverse direction,
 * and return the first.
 */
static uint32_t kona_dma_read_back(QTestState *s, unsigned count)
{
    uint32_t addr = (uint32_t)DMA_DST;
    uint8_t bytes[4] = { 0x5a, 0x5a, 0x5a, 0x5a };

    qtest_memwrite(s, DMA_DST, bytes, sizeof(bytes));
    qtest_qom_set_bool(s, KONA_QOM_PATH, "x-test-dma-reverse", true);
    kona_dma_packet(s, addr, count);
    qtest_qom_set_bool(s, KONA_QOM_PATH, "x-test-dma-reverse", false);
    qtest_memread(s, DMA_DST, bytes, sizeof(bytes));
    return ldl_be_p(bytes);
}

/* The pipe's console as a PPM, returned whole; the file is removed. */
static uint8_t *kona_screendump(QTestState *s, const char *dir,
                                const char *name)
{
    g_autofree char *path = g_build_filename(dir, name, NULL);
    static const char header[] = "P6\n1280 1024\n255\n";
    gchar *data = NULL;
    gsize len = 0;
    uint8_t *pixels;

    qtest_qmp_assert_success(s,
        "{'execute': 'screendump', 'arguments': {'filename': %s, "
        "'device': 'kona0'}}", path);
    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    g_assert_cmpint(g_unlink(path), ==, 0);
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

/* The display line a textport wire y lands on: the wire counts upward. */
static unsigned wire_line(unsigned y)
{
    return KONA_FB_HEIGHT - 1 - y;
}

static char *kona_log_read(const char *log)
{
    char *found = NULL;

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_cmpint(g_unlink(log), ==, 0);
    return found;
}

static unsigned count_occurrences(const char *haystack, const char *needle)
{
    unsigned n = 0;

    for (const char *p = strstr(haystack, needle); p;
         p = strstr(p + 1, needle)) {
        n++;
    }
    return n;
}

/*
 * Every "sgi_scanout_update lines A-B" in the log, in order, as "A-B"
 * joined by spaces.
 */
static char *scanout_updates(const char *log)
{
    static const char tag[] = "sgi_scanout_update lines ";
    GString *out = g_string_new(NULL);

    for (const char *p = strstr(log, tag); p; p = strstr(p + 1, tag)) {
        const char *start = p + strlen(tag);
        size_t n = strcspn(start, "\n ");

        if (out->len) {
            g_string_append_c(out, ' ');
        }
        g_string_append_len(out, start, n);
    }
    return g_string_free(out, false);
}

/*
 * The scanout presents 1280x1024 with its retrace at 60 Hz, the model's
 * own choice (SGI_KONA_FIELD_HZ; no timing table for the pipe is held).
 * The mode is traced once at realize. From reset at virtual time 0,
 * retrace n falls at n / 60 s: 30 retraces by 510 ms and 60 by 1010 ms,
 * each bound 10 ms clear of a retrace. A 50 Hz or 72 Hz period, or none,
 * gives other counts.
 */
static void test_mode_and_retrace_period(void)
{
    g_autofree char *dir = kona_tmpdir();
    g_autofree char *log = g_build_filename(dir, "trace.log", NULL);
    g_autofree char *found = NULL;
    QTestState *s;

    s = qtest_initf("%s -trace enable=sgi_scanout_mode "
                    "-trace enable=sgi_scanout_retrace -D %s",
                    MACHINE_ARGS, log);
    qtest_clock_step(s, 510 * 1000 * 1000);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "sgi_scanout_mode 1280x1024 frame 1 "
                                   "clocks at 60 Hz"));
    g_assert_cmpuint(count_occurrences(found, "sgi_scanout_retrace "), ==,
                     30);
    g_assert_nonnull(strstr(found, "sgi_scanout_retrace frame 30 "));
    g_free(found);

    s = qtest_initf("%s -trace enable=sgi_scanout_retrace -D %s",
                    MACHINE_ARGS, log);
    qtest_clock_step(s, 1010 * 1000 * 1000);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_cmpuint(count_occurrences(found, "sgi_scanout_retrace "), ==,
                     60);
    g_assert_null(strstr(found, "sgi_scanout_retrace frame 61 "));
    g_rmdir(dir);
}

/*
 * Only the lines drawing changed are resolved again. The first screendump
 * presents every line, since nothing has been shown yet. A textport fill
 * from wire (10, 20) to (12, 22) then dirties display lines 1001 to 1003
 * and nothing else, and a four-pixel DMA transfer at the framebuffer's
 * first pixels dirties line 0 alone. A screendump with nothing drawn
 * since the last one resolves no line. A colour-map load dirties every
 * line, since any textport pixel may use the entry.
 */
static void test_drawing_dirties_its_lines(void)
{
    g_autofree char *dir = kona_tmpdir();
    g_autofree char *log = g_build_filename(dir, "trace.log", NULL);
    g_autofree char *found = NULL;
    g_autofree char *updates = NULL;
    QTestState *s;

    s = qtest_initf("%s -trace enable=sgi_scanout_update -D %s",
                    MACHINE_ARGS, log);
    kona_load_colour(s, 1, 0x10, 0x20, 0x30);
    kona_textport_push(s, 0x2, (uint32_t[]){ 1 }, 1);
    g_free(kona_screendump(s, dir, "a.ppm"));
    kona_textport_push(s, 0x5, (uint32_t[]){ 10, 20, 12, 22 }, 4);
    g_free(kona_screendump(s, dir, "b.ppm"));
    kona_dma_pixels(s, (uint32_t[]){ 1, 2, 3, 4 }, 4);
    g_free(kona_screendump(s, dir, "c.ppm"));
    g_free(kona_screendump(s, dir, "d.ppm"));
    kona_load_colour(s, 1, 0x40, 0x50, 0x60);
    g_free(kona_screendump(s, dir, "e.ppm"));
    qtest_quit(s);

    found = kona_log_read(log);
    updates = scanout_updates(found);
    g_assert_cmpstr(updates, ==, "0-1023 1001-1003 0-0 0-1023");
    g_rmdir(dir);
}

/*
 * The screendump shows what was drawn where it was drawn: a textport
 * point at wire (100, 50) in its colour-map colour on display line 973,
 * and a DMA pixel as its low 24 bits (x8r8g8b8: the top byte of
 * 0x11223344 is not colour) at the top-left corner. Pixels nothing drew
 * stay black.
 */
static void test_screendump_shows_drawn_pixels(void)
{
    g_autofree char *dir = kona_tmpdir();
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    kona_load_colour(qts, 9, 0xff, 0x80, 0x40);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 9 }, 1);
    kona_textport_push(qts, 0x6, (uint32_t[]){ 100, 50 }, 2);
    kona_dma_pixels(qts, (uint32_t[]){ 0x11223344 }, 1);

    frame = kona_screendump(qts, dir, "frame.ppm");
    g_assert_cmphex(ppm_pixel(frame, 100, wire_line(50)), ==, 0xff8040);
    g_assert_cmphex(ppm_pixel(frame, 100, wire_line(51)), ==, 0);
    g_assert_cmphex(ppm_pixel(frame, 101, wire_line(50)), ==, 0);
    g_assert_cmphex(ppm_pixel(frame, 0, 0), ==, 0x223344);
    g_assert_cmphex(ppm_pixel(frame, 1, 0), ==, 0);
    g_rmdir(dir);
}

/*
 * Reset clears the framebuffer and the console shows it cleared: a pixel
 * drawn and presented before the reset is black in the screendump after.
 */
static void test_reset_repaints(void)
{
    g_autofree char *dir = kona_tmpdir();
    g_autofree uint8_t *before = NULL;
    g_autofree uint8_t *after = NULL;

    qtest_system_reset(qts);
    kona_load_colour(qts, 2, 0x12, 0x34, 0x56);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 2 }, 1);
    kona_textport_push(qts, 0x6, (uint32_t[]){ 700, 700 }, 2);
    before = kona_screendump(qts, dir, "before.ppm");
    g_assert_cmphex(ppm_pixel(before, 700, wire_line(700)), ==, 0x123456);
    qtest_system_reset(qts);
    after = kona_screendump(qts, dir, "after.ppm");
    g_assert_cmphex(ppm_pixel(after, 700, wire_line(700)), ==, 0);
    g_rmdir(dir);
}

/*
 * The scanout subsection carries the retrace phase: 510 ms into the
 * source's run, 30 retraces have passed, so the destination's next
 * retrace is frame 31, at 516.7 ms from the source's epoch. A
 * destination that kept its own phase would trace frame 1 first. The
 * destination is stepped 520 ms, which reaches frame 31 whether or not
 * its virtual clock resumed from the source's. The drawn pixel crosses
 * in the framebuffer and the destination's console shows it.
 */
static void test_migration_carries_scanout(void)
{
    g_autofree char *dir = kona_tmpdir();
    g_autofree char *log = g_build_filename(dir, "trace.log", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    g_autofree char *dest_args = NULL;
    g_autofree char *found = NULL;
    g_autofree uint8_t *frame = NULL;
    QTestState *from, *to;

    from = qtest_init(MIGRATION_ARGS);
    kona_load_colour(from, 4, 0xab, 0xcd, 0xef);
    kona_textport_push(from, 0x2, (uint32_t[]){ 4 }, 1);
    kona_textport_push(from, 0x6, (uint32_t[]){ 300, 400 }, 2);
    qtest_clock_step(from, 510 * 1000 * 1000);

    dest_args = g_strdup_printf("%s -incoming %s -trace "
                                "enable=sgi_scanout_retrace -D %s",
                                MIGRATION_ARGS, uri, log);
    to = qtest_init(dest_args);
    sgi_migrate_stopped(from, uri);

    qtest_clock_step(to, 520 * 1000 * 1000);
    frame = kona_screendump(to, dir, "frame.ppm");
    g_assert_cmphex(ppm_pixel(frame, 300, wire_line(400)), ==, 0xabcdef);

    qtest_quit(to);
    qtest_quit(from);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "sgi_scanout_retrace frame 31 "));
    g_assert_null(strstr(found, "sgi_scanout_retrace frame 1 "));
    g_assert_null(strstr(found, "sgi_scanout_retrace frame 30 "));
    g_rmdir(dir);
}

/*
 * The board memory holds six planes, declared at realize in this order:
 * front and back colour banks of 1280x1024 32-bit words (format RGB24,
 * 2), a byte-per-pixel tag plane for each (format TAG, 6), 1 MiB of list
 * words in 4096-byte rows (format RAW, 9) and a 1280x1024 depth plane of
 * 32-bit words (format Z24, 4). The list size follows the
 * "list-memory-size" property, and a size that is not a whole number of
 * rows is refused at realize.
 */
static void test_planes_declared(void)
{
    g_autofree char *dir = kona_tmpdir();
    g_autofree char *log = g_build_filename(dir, "trace.log", NULL);
    g_autofree char *found = NULL;
    QTestState *s;
    static const char *const planes[] = {
        "plane 0 base 0x0 len 0x500000 1280x1024 stride 5120 format 2",
        "plane 1 base 0x500000 len 0x500000 1280x1024 stride 5120 format 2",
        "plane 2 base 0xa00000 len 0x140000 1280x1024 stride 1280 format 6",
        "plane 3 base 0xb40000 len 0x140000 1280x1024 stride 1280 format 6",
        "plane 4 base 0xc80000 len 0x100000 1024x256 stride 4096 format 9",
        "plane 5 base 0xd80000 len 0x500000 1280x1024 stride 5120 format 4",
    };

    s = qtest_initf("%s -trace enable=sgi_plane_add -D %s", MACHINE_ARGS,
                    log);
    qtest_quit(s);
    found = kona_log_read(log);
    for (size_t i = 0; i < G_N_ELEMENTS(planes); i++) {
        g_assert_nonnull(strstr(found, planes[i]));
    }
    g_assert_cmpuint(count_occurrences(found, "sgi_plane_add "), ==, 6);
    g_free(found);

    s = qtest_initf("%s,list-memory-size=0x200000 -trace enable=sgi_plane_add"
                    " -D %s", MACHINE_ARGS, log);
    qtest_quit(s);
    found = kona_log_read(log);
    g_assert_nonnull(strstr(found, "plane 4 base 0xc80000 len 0x200000 "
                                   "1024x512 stride 4096 format 9"));
    g_assert_nonnull(strstr(found, "plane 5 base 0xe80000 len 0x500000 "
                                   "1280x1024 stride 5120 format 4"));
    g_rmdir(dir);
}

static void test_list_size_refused(void)
{
    if (g_test_subprocess()) {
        qtest_quit(qtest_init(MACHINE_ARGS ",list-memory-size=0x1800"));
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*list-memory-size 6144 is not a nonzero "
                              "multiple of 4096*");
}

/*
 * The property's other two refusals: a zero size, and one past 64 MiB
 * that is still a whole number of rows.
 */
static void test_list_size_zero_refused(void)
{
    if (g_test_subprocess()) {
        qtest_quit(qtest_init(MACHINE_ARGS ",list-memory-size=0"));
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*list-memory-size 0 is not a nonzero "
                              "multiple of 4096*");
}

static void test_list_size_too_large_refused(void)
{
    if (g_test_subprocess()) {
        qtest_quit(qtest_init(MACHINE_ARGS ",list-memory-size=0x4001000"));
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*list-memory-size 67112960 is not a nonzero "
                              "multiple of 4096 no larger than 67108864*");
}

/*
 * A stream from a board with another list size is refused: the board
 * memory subsection's length differs, so the destination's load fails
 * and it exits, as the property's comment promises.
 */
static void test_migration_refuses_other_list_size(void)
{
    g_autofree char *dir = kona_tmpdir();
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    g_autofree char *dest_args = NULL;
    QTestState *from, *to;

    from = qtest_init(MIGRATION_ARGS);
    dest_args = g_strdup_printf("%s,list-memory-size=0x200000 -incoming %s",
                                MACHINE_ARGS, uri);
    to = qtest_init(dest_args);
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    qtest_wait_qemu(to);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

/*
 * Drawing lands in the front planes, read back through the shared plane
 * load. A textport point keeps its colour-map index, 5, with the
 * colour-index tag, at plane (40, 60), the wire coordinate: the plane is
 * bottom-origin. A DMA pixel keeps its whole word with the direct tag,
 * at plane (0, 1023), the top display line's left end.
 */
static void test_drawing_lands_in_planes(void)
{
    qtest_system_reset(qts);
    kona_load_colour(qts, 5, 0x55, 0x66, 0x77);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 5 }, 1);
    kona_textport_push(qts, 0x6, (uint32_t[]){ 40, 60 }, 2);
    kona_dma_pixels(qts, (uint32_t[]){ 0x11223344 }, 1);

    g_assert_cmphex(kona_plane_word(qts, PLANE_FRONT, 40, 60), ==, 5);
    g_assert_cmphex(kona_plane_word(qts, PLANE_FRONT_TAG, 40, 60), ==,
                    TAG_COLOR_INDEX);
    g_assert_cmphex(kona_plane_word(qts, PLANE_FRONT, 40, 61), ==, 0);
    g_assert_cmphex(kona_plane_word(qts, PLANE_FRONT, 0, 1023), ==,
                    0x11223344);
    g_assert_cmphex(kona_plane_word(qts, PLANE_FRONT_TAG, 0, 1023), ==,
                    TAG_DIRECT);
    g_assert_cmphex(kona_test_pixel(qts, 40, 60), ==, 0x556677);
}

/*
 * A textport pixel resolves through the colour map when it is shown, not
 * when it is drawn: reloading its entry after the draw changes the
 * screendump and the resolved pixel, and a DMA pixel beside it, direct
 * colour, is untouched by any colour-map entry. A model that resolved at
 * draw time would still show the first colour.
 */
static void test_colour_map_resolves_at_scanout(void)
{
    g_autofree char *dir = kona_tmpdir();
    g_autofree uint8_t *first = NULL;
    g_autofree uint8_t *second = NULL;

    qtest_system_reset(qts);
    kona_load_colour(qts, 7, 0xff, 0, 0);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 7 }, 1);
    kona_textport_push(qts, 0x6, (uint32_t[]){ 500, 500 }, 2);
    kona_dma_pixels(qts, (uint32_t[]){ 0x00000007 }, 1);
    first = kona_screendump(qts, dir, "first.ppm");
    g_assert_cmphex(ppm_pixel(first, 500, wire_line(500)), ==, 0xff0000);
    g_assert_cmphex(ppm_pixel(first, 0, 0), ==, 0x000007);

    kona_load_colour(qts, 7, 0, 0xff, 0);
    second = kona_screendump(qts, dir, "second.ppm");
    g_assert_cmphex(ppm_pixel(second, 500, wire_line(500)), ==, 0x00ff00);
    g_assert_cmphex(ppm_pixel(second, 0, 0), ==, 0x000007);
    g_assert_cmphex(kona_test_pixel(qts, 500, 500), ==, 0x00ff00);
    g_rmdir(dir);
}

/*
 * The board memory subsection carries the planes as stored: the
 * destination holds the textport pixel's index and tag, not its resolved
 * colour, so reloading the entry there recolours it, and the DMA pixel's
 * whole word.
 */
static void test_migration_carries_planes(void)
{
    g_autofree char *dir = kona_tmpdir();
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    g_autofree char *dest_args = NULL;
    QTestState *from, *to;

    from = qtest_init(MIGRATION_ARGS);
    kona_load_colour(from, 6, 0x10, 0x20, 0x30);
    kona_textport_push(from, 0x2, (uint32_t[]){ 6 }, 1);
    kona_textport_push(from, 0x6, (uint32_t[]){ 123, 456 }, 2);
    kona_dma_pixels(from, (uint32_t[]){ 0xa1b2c3d4 }, 1);

    dest_args = g_strdup_printf("%s -incoming %s", MIGRATION_ARGS, uri);
    to = qtest_init(dest_args);
    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(kona_plane_word(to, PLANE_FRONT, 123, 456), ==, 6);
    g_assert_cmphex(kona_plane_word(to, PLANE_FRONT_TAG, 123, 456), ==,
                    TAG_COLOR_INDEX);
    g_assert_cmphex(kona_test_pixel(to, 123, 456), ==, 0x102030);
    g_assert_cmphex(kona_plane_word(to, PLANE_FRONT, 0, 1023), ==,
                    0xa1b2c3d4);
    kona_load_colour(to, 6, 0x40, 0x50, 0x60);
    g_assert_cmphex(kona_test_pixel(to, 123, 456), ==, 0x405060);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

/*
 * A version 6 stream saved before the board memory carries the resolved
 * framebuffer in the main section and no board memory subsection. It
 * loads into the front plane as direct colour, so the destination shows
 * what the source showed: the textport pixel in its colour, now tagged
 * direct, and the DMA pixel's whole word. Skipped unless
 * QTEST_QEMU_BINARY_KONA_V6 names a qemu-system-mips64 built before the
 * board memory, the pattern the version 4 and 5 cases in
 * sn0-origin2000-kona-video-test.c use.
 */
static void test_migration_from_v6(void)
{
    g_autofree char *dir = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    g_autofree uint8_t *frame = NULL;
    QTestState *from, *to;

    if (!g_getenv("QTEST_QEMU_BINARY_KONA_V6")) {
        g_test_skip("QTEST_QEMU_BINARY_KONA_V6 is not set");
        return;
    }
    dir = kona_tmpdir();
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);

    from = qtest_init_ext("QTEST_QEMU_BINARY_KONA_V6", MIGRATION_ARGS, NULL,
                          true);
    kona_load_colour(from, 3, 0x0a, 0x0b, 0x0c);
    kona_textport_push(from, 0x2, (uint32_t[]){ 3 }, 1);
    kona_textport_push(from, 0x6, (uint32_t[]){ 77, 88 }, 2);
    kona_dma_pixels(from, (uint32_t[]){ 0x99887766 }, 1);

    dest_args = g_strdup_printf("%s -incoming %s", MIGRATION_ARGS, uri);
    to = qtest_init(dest_args);
    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(kona_test_pixel(to, 77, 88), ==, 0x0a0b0c);
    g_assert_cmphex(kona_plane_word(to, PLANE_FRONT, 77, 88), ==, 0x0a0b0c);
    g_assert_cmphex(kona_plane_word(to, PLANE_FRONT_TAG, 77, 88), ==,
                    TAG_DIRECT);
    g_assert_cmphex(kona_plane_word(to, PLANE_FRONT, 0, 1023), ==,
                    0x99887766);
    frame = kona_screendump(to, dir, "frame.ppm");
    g_assert_cmphex(ppm_pixel(frame, 77, wire_line(88)), ==, 0x0a0b0c);
    g_assert_cmphex(ppm_pixel(frame, 0, 0), ==, 0x887766);
    g_assert_cmphex(ppm_pixel(frame, 78, wire_line(88)), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

/*
 * A DMA readback resolves like the scanout: a textport pixel read back to
 * guest memory returns its colour-map entry as it stands, so reloading
 * the entry changes what the next readback returns. The pixel is the
 * framebuffer's first, the top display line's left end, which the DMA
 * engine's flat offset 0 names.
 */
static void test_dma_readback_follows_colour_map(void)
{
    qtest_system_reset(qts);
    kona_load_colour(qts, 9, 0x11, 0x22, 0x33);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 9 }, 1);
    kona_textport_push(qts, 0x6, (uint32_t[]){ 0, KONA_FB_HEIGHT - 1 }, 2);
    g_assert_cmphex(kona_dma_read_back(qts, 1), ==, 0x112233);
    kona_load_colour(qts, 9, 0x44, 0x55, 0x66);
    g_assert_cmphex(kona_dma_read_back(qts, 1), ==, 0x445566);
}

/*
 * The textport's colour-map load carries each component scaled by four,
 * as ten bits. The model keeps eight bits, dropping the low two: 0x3ff,
 * 0x201 and 0x002 resolve as 0xff, 0x80 and 0x00, and 0x300 as 0xc0. A
 * model that kept ten bits would show another colour, and one that
 * rounded 0x300 * 255 / 1023 would show 0xbf, 191.4 being no tie.
 */
static void test_colour_map_low_bits_dropped(void)
{
    g_autofree char *dir = kona_tmpdir();
    g_autofree uint8_t *frame = NULL;

    qtest_system_reset(qts);
    kona_textport_push(qts, 0x3, (uint32_t[]){ 4, 0x3ff, 0x201, 0x002 }, 4);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 4 }, 1);
    kona_textport_push(qts, 0x6, (uint32_t[]){ 10, 20 }, 2);
    kona_textport_push(qts, 0x3, (uint32_t[]){ 5, 0x300, 0x300, 0x300 }, 4);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 5 }, 1);
    kona_textport_push(qts, 0x6, (uint32_t[]){ 11, 20 }, 2);
    g_assert_cmphex(kona_test_pixel(qts, 10, 20), ==, 0xff8000);
    g_assert_cmphex(kona_test_pixel(qts, 11, 20), ==, 0xc0c0c0);
    frame = kona_screendump(qts, dir, "frame.ppm");
    g_assert_cmphex(ppm_pixel(frame, 10, wire_line(20)), ==, 0xff8000);
    g_assert_cmphex(ppm_pixel(frame, 11, wire_line(20)), ==, 0xc0c0c0);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

    qtest_add_func("sn0/origin2000/kona-scanout/mode-and-retrace-period",
                   test_mode_and_retrace_period);
    qtest_add_func("sn0/origin2000/kona-scanout/drawing-dirties-its-lines",
                   test_drawing_dirties_its_lines);
    qtest_add_func("sn0/origin2000/kona-scanout/screendump-shows-drawn-pixels",
                   test_screendump_shows_drawn_pixels);
    qtest_add_func("sn0/origin2000/kona-scanout/reset-repaints",
                   test_reset_repaints);
    qtest_add_func("sn0/origin2000/kona-scanout/migration-carries-scanout",
                   test_migration_carries_scanout);
    qtest_add_func("sn0/origin2000/kona-scanout/planes-declared",
                   test_planes_declared);
    qtest_add_func("sn0/origin2000/kona-scanout/list-size-refused",
                   test_list_size_refused);
    qtest_add_func("sn0/origin2000/kona-scanout/drawing-lands-in-planes",
                   test_drawing_lands_in_planes);
    qtest_add_func("sn0/origin2000/kona-scanout/"
                   "colour-map-resolves-at-scanout",
                   test_colour_map_resolves_at_scanout);
    qtest_add_func("sn0/origin2000/kona-scanout/migration-carries-planes",
                   test_migration_carries_planes);
    qtest_add_func("sn0/origin2000/kona-scanout/migration-from-v6",
                   test_migration_from_v6);
    qtest_add_func("sn0/origin2000/kona-scanout/list-size-zero-refused",
                   test_list_size_zero_refused);
    qtest_add_func("sn0/origin2000/kona-scanout/list-size-too-large-refused",
                   test_list_size_too_large_refused);
    qtest_add_func("sn0/origin2000/kona-scanout/"
                   "migration-refuses-other-list-size",
                   test_migration_refuses_other_list_size);
    qtest_add_func("sn0/origin2000/kona-scanout/"
                   "dma-readback-follows-colour-map",
                   test_dma_readback_follows_colour_map);
    qtest_add_func("sn0/origin2000/kona-scanout/colour-map-low-bits-dropped",
                   test_colour_map_low_bits_dropped);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
