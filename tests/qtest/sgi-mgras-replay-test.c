/*
 * Replay a reduced, device-relative MGRAS MMIO stream.  Origin 2000 is only
 * the host fixture; replay files contain no machine addresses or topology.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

#define MGRAS_BASE           (SPACE_XTALK + (0xfULL << 24))
#define MGRAS_REG(offset)    (MGRAS_BASE + (offset))
#define MACHINE_ARGS      "-M origin2000 -smp 8 -m 256M"
#define MGRAS_QOM_PATH   "/machine/mgras"
#define DEFAULT_REPLAY    "../tests/data/mgras/fill.replay"
#define MINIMAL_REPLAY    "../tests/data/mgras/minimal.replay"
#define MASKED_READ       "../tests/data/mgras/masked-read.replay"
#define CMAP_HIGH_BYTE    "../tests/data/mgras/cmap-high-byte.replay"
#define CHAR_MODES        "../tests/data/mgras/char-modes.replay"
#define CMAP_XMAP_WID     "../tests/data/mgras/cmap-xmap-wid.replay"
#define ROOT_ICON         "../tests/data/mgras/root-icon.replay"
#define OVERLAY           "../tests/data/mgras/overlay.replay"
#define INVALID_REPLAY    "../tests/data/mgras/invalid.replay"
#define INVALID_BOUNDS    "../tests/data/mgras/invalid-bounds.replay"
#define INVALID_EXTRA     "../tests/data/mgras/invalid-extra.replay"
#define INVALID_READ_EXTRA "../tests/data/mgras/invalid-read-extra.replay"
#define INVALID_READ_MASK "../tests/data/mgras/invalid-read-mask.replay"
#define INVALID_READ_MISMATCH \
    "../tests/data/mgras/invalid-read-mismatch.replay"
#define INVALID_MASKED_MISMATCH \
    "../tests/data/mgras/invalid-masked-mismatch.replay"
#define INVALID_OVERFLOW  "../tests/data/mgras/invalid-overflow.replay"
#define INVALID_WID_BOUNDS "../tests/data/mgras/invalid-wid-bounds.replay"
#define INVALID_MISMATCH  "../tests/data/mgras/invalid-mismatch.replay"
#define MGRAS_MMIO_SIZE      (2 * MiB)
#define SI_NATIVE_WIDTH   1280
#define SI_NATIVE_HEIGHT  1024

static uint64_t parse_number(const char *path, unsigned int line,
                             const char *word)
{
    char *end;
    uint64_t value;

    errno = 0;
    if (!word || *word == '-' || *word == '+') {
        g_error("%s:%u: invalid number '%s'", path, line,
                word ? word : "(null)");
    }
    value = g_ascii_strtoull(word, &end, 0);
    if (errno || end == word || *end) {
        g_error("%s:%u: invalid number '%s'", path, line, word);
    }
    return value;
}

static uint64_t qom_get_uint(QTestState *qts, const char *property)
{
    QDict *response = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': %s}}", MGRAS_QOM_PATH, property);
    uint64_t value;

    if (!qdict_haskey(response, "return")) {
        g_autoptr(GString) text = qobject_to_json(QOBJECT(response));
        g_error("qom-get %s failed: %s", property, text->str);
    }
    value = qdict_get_int(response, "return");
    qobject_unref(response);
    return value;
}

static uint32_t pixel(QTestState *qts, uint32_t x, uint32_t y,
                      bool display)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}",
        MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}",
        MGRAS_QOM_PATH, y);
    return qom_get_uint(qts, display ? "x-test-display-pixel" :
                                      "x-test-pixel");
}

static uint32_t plane_value(QTestState *qts, uint32_t x, uint32_t y,
                            const char *property)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}",
        MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}",
        MGRAS_QOM_PATH, y);
    return qom_get_uint(qts, property);
}

static void replay_mmio(QTestState *qts, const char *path, unsigned int line,
                        char **words, bool write)
{
    uint64_t width, offset, value, actual, mask;
    bool masked;
    size_t nwords = g_strv_length(words);

    if ((write && nwords != 4) || (!write && nwords != 4 && nwords != 5)) {
        g_error("%s:%u: %s requires WIDTH OFFSET VALUE%s", path, line,
                words[0], write ? "" : " [MASK]");
    }
    width = parse_number(path, line, words[1]);
    offset = parse_number(path, line, words[2]);
    value = parse_number(path, line, words[3]);
    masked = nwords == 5;
    mask = masked ? parse_number(path, line, words[4]) : UINT64_MAX;
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        g_error("%s:%u: unsupported MMIO width %" PRIu64, path, line, width);
    }
    if (offset >= MGRAS_MMIO_SIZE || offset + width > MGRAS_MMIO_SIZE) {
        g_error("%s:%u: MMIO access is outside the MGRAS aperture", path,
                line);
    }
    if (width < 8 && value >= (1ULL << (width * 8))) {
        g_error("%s:%u: MMIO value is wider than width %" PRIu64, path,
                line, width);
    }
    if (masked && width < 8 && mask >= (1ULL << (width * 8))) {
        g_error("%s:%u: MMIO mask is wider than width %" PRIu64, path,
                line, width);
    }
    if (write) {
        if (width == 1) {
            qtest_writeb(qts, MGRAS_REG(offset), value);
        } else if (width == 2) {
            qtest_writew(qts, MGRAS_REG(offset), value);
        } else if (width == 4) {
            qtest_writel(qts, MGRAS_REG(offset), value);
        } else {
            qtest_writeq(qts, MGRAS_REG(offset), value);
        }
        return;
    }

    if (width == 1) {
        actual = qtest_readb(qts, MGRAS_REG(offset));
    } else if (width == 2) {
        actual = qtest_readw(qts, MGRAS_REG(offset));
    } else if (width == 4) {
        actual = qtest_readl(qts, MGRAS_REG(offset));
    } else {
        actual = qtest_readq(qts, MGRAS_REG(offset));
    }
    if ((actual ^ value) & mask) {
        if (masked) {
            g_error("%s:%u: read 0x%" PRIx64 " returned 0x%" PRIx64
                    ", expected 0x%" PRIx64 " with mask 0x%" PRIx64,
                    path, line, offset, actual, value, mask);
        }
        g_error("%s:%u: read 0x%" PRIx64 " returned 0x%" PRIx64
                ", expected 0x%" PRIx64, path, line, offset, actual, value);
    }
}

static void replay_pixel(QTestState *qts, const char *path, unsigned int line,
                         char **words, bool display)
{
    uint64_t x, y, expected, actual;

    if (!words[1] || !words[2] || !words[3] || words[4]) {
        g_error("%s:%u: %s requires X Y VALUE", path, line, words[0]);
    }
    x = parse_number(path, line, words[1]);
    y = parse_number(path, line, words[2]);
    expected = parse_number(path, line, words[3]);
    if (x >= SI_NATIVE_WIDTH || y >= SI_NATIVE_HEIGHT ||
        expected > UINT32_MAX) {
        g_error("%s:%u: pixel is outside the native surface", path, line);
    }
    actual = pixel(qts, x, y, display);
    if (actual != expected) {
        g_error("%s:%u: pixel (%" PRIu64 ", %" PRIu64 ") returned 0x%"
                PRIx64 ", expected 0x%" PRIx64,
                path, line, x, y, actual, expected);
    }
}

static void replay_pixels(QTestState *qts, const char *path, unsigned int line,
                          char **words)
{
    uint64_t x, y, width, height, expected;

    if (!words[1] || !words[2] || !words[3] || !words[4] || !words[5] ||
        words[6]) {
        g_error("%s:%u: pixels requires X Y WIDTH HEIGHT VALUE", path, line);
    }
    x = parse_number(path, line, words[1]);
    y = parse_number(path, line, words[2]);
    width = parse_number(path, line, words[3]);
    height = parse_number(path, line, words[4]);
    expected = parse_number(path, line, words[5]);
    if (x > UINT32_MAX || y > UINT32_MAX || width > UINT32_MAX ||
        height > UINT32_MAX || expected > UINT32_MAX ||
        width > UINT32_MAX - x || height > UINT32_MAX - y || !width ||
        !height || x >= SI_NATIVE_WIDTH || y >= SI_NATIVE_HEIGHT ||
        width > SI_NATIVE_WIDTH - x || height > SI_NATIVE_HEIGHT - y) {
        g_error("%s:%u: pixels rectangle is outside the native surface",
                path, line);
    }
    for (uint32_t row = 0; row < height; row++) {
        for (uint32_t column = 0; column < width; column++) {
            uint32_t actual = pixel(qts, x + column, y + row, false);
            if (actual != expected) {
                g_error("%s:%u: pixels (%u,%u) returned 0x%08x, expected "
                        "0x%08x", path, line, (uint32_t)(x + column),
                        (uint32_t)(y + row),
                        actual, (uint32_t)expected);
            }
        }
    }
}

/* Check one byte-wide plane: the WID tag or the overlay index. */
static void replay_plane(QTestState *qts, const char *path, unsigned int line,
                         char **words, const char *property)
{
    uint64_t x, y, expected, actual;

    if (!words[1] || !words[2] || !words[3] || words[4]) {
        g_error("%s:%u: %s requires X Y VALUE", path, line, words[0]);
    }
    x = parse_number(path, line, words[1]);
    y = parse_number(path, line, words[2]);
    expected = parse_number(path, line, words[3]);
    if (x >= SI_NATIVE_WIDTH || y >= SI_NATIVE_HEIGHT ||
        expected > UINT8_MAX) {
        g_error("%s:%u: %s value is outside the current model range", path,
                line, words[0]);
    }
    actual = plane_value(qts, x, y, property);
    if (actual != expected) {
        g_error("%s:%u: %s (%" PRIu64 ",%" PRIu64 ") returned 0x%02" PRIx64
                ", expected 0x%02" PRIx64, path, line, words[0], x, y, actual,
                expected);
    }
}

/*
 * A line number given through MGRAS_START_LINE/MGRAS_END_LINE, not one
 * read out of a replay file -- parse_number()'s own messages assume the
 * latter, so this reports the offending environment variable instead.
 */
static unsigned int parse_env_line(const char *name, const char *value)
{
    char *end;
    uint64_t parsed;

    errno = 0;
    parsed = g_ascii_strtoull(value, &end, 0);
    if (errno || end == value || *end || !parsed || parsed > UINT_MAX) {
        g_error("%s: invalid line number '%s'", name, value);
    }
    return (unsigned int)parsed;
}

static void replay_line(QTestState *qts, const char *path, unsigned int line,
                        char *text)
{
    g_auto(GStrv) raw = g_strsplit_set(text, " \t\r", -1);
    g_autoptr(GPtrArray) compact = g_ptr_array_new();
    char **words;

    for (size_t i = 0; raw[i]; i++) {
        if (*raw[i]) {
            g_ptr_array_add(compact, raw[i]);
        }
    }
    g_ptr_array_add(compact, NULL);
    words = (char **)compact->pdata;
    if (!words[0] || words[0][0] == '#') {
        return;
    }
    if (!strcmp(words[0], "reset") && !words[1]) {
        qtest_system_reset(qts);
    } else if (!strcmp(words[0], "write")) {
        replay_mmio(qts, path, line, words, true);
    } else if (!strcmp(words[0], "read")) {
        replay_mmio(qts, path, line, words, false);
    } else if (!strcmp(words[0], "pixel")) {
        replay_pixel(qts, path, line, words, false);
    } else if (!strcmp(words[0], "display-pixel")) {
        replay_pixel(qts, path, line, words, true);
    } else if (!strcmp(words[0], "pixels")) {
        replay_pixels(qts, path, line, words);
    } else if (!strcmp(words[0], "wid")) {
        replay_plane(qts, path, line, words, "x-test-wid");
    } else if (!strcmp(words[0], "overlay")) {
        replay_plane(qts, path, line, words, "x-test-overlay");
    } else {
        g_error("%s:%u: unknown or malformed operation '%s'", path, line,
                words[0]);
    }
}

/*
 * Replay @path against a fresh machine, or, with MGRAS_CHECKPOINT_LOAD set,
 * against one restored from an earlier run's MGRAS_CHECKPOINT_SAVE. Either
 * way MGRAS_START_LINE/MGRAS_END_LINE (1-based, inclusive, default the
 * whole file) select which lines of @path this process actually replays --
 * a checkpoint stands in for the lines before it, so a save at line N and a
 * load replaying N+1..M skip re-executing 1..N through the model.
 *
 * The checkpoint file itself is plain QEMU migration state (RAM and every
 * device's VMState, not just sgi-mgras' own), written and read the same way
 * sn0-migration-test.c's file-URI case does: "-incoming defer" plus a
 * migrate-incoming QMP command on the load side, sgi_migrate_stopped() on
 * the save side, both through sgi-migrate.h. qtest's dummy CPUs and frozen
 * QEMU_CLOCK_VIRTUAL mean the two sides never need an explicit stop.
 */
static void run_replay(const char *path, bool check_default_trace)
{
    const char *trace = g_getenv("MGRAS_TRACE");
    const char *screendump = g_getenv("MGRAS_SCREENDUMP");
    const char *checkpoint_load = g_getenv("MGRAS_CHECKPOINT_LOAD");
    const char *checkpoint_save = g_getenv("MGRAS_CHECKPOINT_SAVE");
    const char *start_line_env = g_getenv("MGRAS_START_LINE");
    const char *end_line_env = g_getenv("MGRAS_END_LINE");
    g_autofree char *contents = NULL;
    g_autofree char *trace_contents = NULL;
    g_autofree char *temporary_trace = NULL;
    g_autofree char *args = NULL;
    g_autofree char *load_uri = NULL;
    g_autofree char *save_uri = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GError) error = NULL;
    bool remove_trace = false;
    unsigned int line_count, start_line, end_line;
    QTestState *qts;
    int fd;

    if (!g_file_get_contents(path, &contents, NULL, &error)) {
        g_error("cannot read replay %s: %s", path, error->message);
    }
    if (!trace || !*trace) {
        fd = g_file_open_tmp("sgi-mgras-replay-XXXXXX.trace",
                             &temporary_trace, &error);
        if (fd < 0) {
            g_error("cannot create trace file: %s", error->message);
        }
        close(fd);
        trace = temporary_trace;
        remove_trace = true;
    }
    args = g_strdup_printf(MACHINE_ARGS
                           " -trace enable=sgi_mgras_mmio"
                           " -trace enable=sgi_mgras_control_mmio"
                           " -D %s%s", trace,
                           checkpoint_load && *checkpoint_load ?
                           " -incoming defer" : "");
    qts = qtest_init(args);
    if (checkpoint_load && *checkpoint_load) {
        load_uri = g_strdup_printf("file:%s", checkpoint_load);
        qtest_qmp_assert_success(qts,
            "{'execute': 'migrate-incoming', 'arguments': {'uri': %s}}",
            load_uri);
        sgi_wait_migration_status(qts, "completed", 30);
    }

    lines = g_strsplit(contents, "\n", -1);
    line_count = g_strv_length(lines);
    start_line = start_line_env && *start_line_env ?
                 parse_env_line("MGRAS_START_LINE", start_line_env) : 1;
    end_line = end_line_env && *end_line_env ?
               parse_env_line("MGRAS_END_LINE", end_line_env) : line_count;
    if (start_line < 1 || end_line < start_line || end_line > line_count) {
        g_error("%s: line range %u..%u is outside 1..%u", path,
                start_line, end_line, line_count);
    }
    for (unsigned int i = start_line; i <= end_line; i++) {
        replay_line(qts, path, i, lines[i - 1]);
    }
    if (checkpoint_save && *checkpoint_save) {
        save_uri = g_strdup_printf("file:%s", checkpoint_save);
        sgi_migrate_stopped(qts, save_uri);
    }
    if (screendump && *screendump) {
        qtest_qmp_assert_success(qts,
            "{'execute': 'screendump', 'arguments': {'filename': %s}}",
            screendump);
    }
    qtest_quit(qts);
    if (!g_file_get_contents(trace, &trace_contents, NULL, &error)) {
        g_error("cannot read trace %s: %s", trace, error->message);
    }
    if (check_default_trace) {
        g_auto(GStrv) trace_lines = g_strsplit(trace_contents, "\n", -1);
        bool saw_control_read = false;
        bool saw_control_write = false;

        g_assert_nonnull(strstr(trace_contents,
            "offset 0x73c08 value 0x55 size 1 write=1"));
        g_assert_nonnull(strstr(trace_contents,
            "offset 0x73c08 value 0x55 size 1 write=0"));
        g_assert_nonnull(strstr(trace_contents,
            "offset 0x720b0 value 0x2468 size 2 write=0"));
        g_assert_nonnull(strstr(trace_contents, "offset 0x120400"));
        g_assert_nonnull(strstr(trace_contents, "offset 0x130400"));
        for (unsigned int i = 0; trace_lines[i]; i++) {
            if (!strstr(trace_lines[i], "sgi_mgras_control_mmio")) {
                continue;
            }
            g_assert_null(strstr(trace_lines[i], "offset 0x120400"));
            g_assert_null(strstr(trace_lines[i], "offset 0x130400"));
            saw_control_read |= strstr(trace_lines[i],
                                       "offset 0x73c08 value 0x55 size 1 "
                                       "write=0") != NULL;
            saw_control_write |= strstr(trace_lines[i],
                                        "offset 0x73c08 value 0x55 size 1 "
                                        "write=1") != NULL;
        }
        g_assert_true(saw_control_read);
        g_assert_true(saw_control_write);
    }
    if (remove_trace) {
        unlink(trace);
    }
}

static void test_replay(void)
{
    const char *path = g_getenv("MGRAS_REPLAY");
    bool custom = path && *path;

    run_replay(custom ? path : DEFAULT_REPLAY, !custom);
}

static void test_replay_screendump(void)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd;

    fd = g_file_open_tmp("sgi-mgras-replay-XXXXXX.ppm", &path, &error);
    g_assert_no_error(error);
    close(fd);
    unlink(path);
    g_setenv("MGRAS_SCREENDUMP", path, true);
    run_replay(MINIMAL_REPLAY, false);
    g_unsetenv("MGRAS_SCREENDUMP");
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(g_str_has_prefix(contents, "P6\n1280 1024\n255\n"));
    unlink(path);
}

static void test_minimal_replay(void)
{
    run_replay(MINIMAL_REPLAY, false);
}

static void test_masked_read(void)
{
    run_replay(MASKED_READ, false);
}

/*
 * MGRAS_START_LINE/MGRAS_END_LINE are plain line numbers with no
 * cross-check against an unrelated checkpoint file's own boundary --
 * resuming at the line a checkpoint was saved from, instead of the line
 * after it, would silently replay that line's write a second time. Prove
 * the documented N/N+1 contract instead: save after line two of a tiny
 * fixture, resume at line three, and confirm line two's write appears
 * exactly once across the combined trace of both runs.
 */
static void test_checkpoint_boundary_runs_once(void)
{
    static const char fixture[] =
        "write 1 0x073c08 0xaa\n"
        "write 1 0x073c08 0xbb\n"
        "read 1 0x073c08 0xbb\n";
    static const char needle[] = "offset 0x73c08 value 0xbb size 1 write=1";
    g_autofree char *fixture_path = NULL;
    g_autofree char *checkpoint_path = NULL;
    g_autofree char *trace_save_path = NULL;
    g_autofree char *trace_load_path = NULL;
    g_autofree char *trace_save = NULL;
    g_autofree char *trace_load = NULL;
    g_autofree char *combined = NULL;
    g_auto(GStrv) combined_lines = NULL;
    g_autoptr(GError) error = NULL;
    unsigned int seen = 0;
    int fd;

    fd = g_file_open_tmp("sgi-mgras-boundary-XXXXXX.replay", &fixture_path,
                         &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(fixture_path, fixture,
                                      strlen(fixture), &error));
    g_assert_no_error(error);

    fd = g_file_open_tmp("sgi-mgras-boundary-XXXXXX.state", &checkpoint_path,
                         &error);
    g_assert_no_error(error);
    close(fd);
    unlink(checkpoint_path);
    fd = g_file_open_tmp("sgi-mgras-boundary-save-XXXXXX.trace",
                         &trace_save_path, &error);
    g_assert_no_error(error);
    close(fd);
    fd = g_file_open_tmp("sgi-mgras-boundary-load-XXXXXX.trace",
                         &trace_load_path, &error);
    g_assert_no_error(error);
    close(fd);

    g_setenv("MGRAS_END_LINE", "2", true);
    g_setenv("MGRAS_CHECKPOINT_SAVE", checkpoint_path, true);
    g_setenv("MGRAS_TRACE", trace_save_path, true);
    run_replay(fixture_path, false);
    g_unsetenv("MGRAS_CHECKPOINT_SAVE");

    g_setenv("MGRAS_CHECKPOINT_LOAD", checkpoint_path, true);
    g_setenv("MGRAS_START_LINE", "3", true);
    g_setenv("MGRAS_END_LINE", "3", true);
    g_setenv("MGRAS_TRACE", trace_load_path, true);
    run_replay(fixture_path, false);
    g_unsetenv("MGRAS_CHECKPOINT_LOAD");
    g_unsetenv("MGRAS_START_LINE");
    g_unsetenv("MGRAS_END_LINE");
    g_unsetenv("MGRAS_TRACE");

    g_assert_true(g_file_get_contents(trace_save_path, &trace_save, NULL,
                                      &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(trace_load_path, &trace_load, NULL,
                                      &error));
    g_assert_no_error(error);
    combined = g_strconcat(trace_save, trace_load, NULL);

    /*
     * Every MMIO access fires both sgi_mgras_mmio and, for the control
     * window this offset falls in, sgi_mgras_control_mmio -- one real
     * access, two trace lines. Count only the plain event, the same way
     * check_default_trace's own scan above filters to one event name.
     */
    combined_lines = g_strsplit(combined, "\n", -1);
    for (unsigned int i = 0; combined_lines[i]; i++) {
        if (g_str_has_prefix(combined_lines[i], "sgi_mgras_mmio ") &&
            strstr(combined_lines[i], needle)) {
            seen++;
        }
    }
    g_assert_cmpuint(seen, ==, 1);

    unlink(fixture_path);
    unlink(checkpoint_path);
    unlink(trace_save_path);
    unlink(trace_load_path);
}

static void test_cmap_high_byte(void)
{
    run_replay(CMAP_HIGH_BYTE, false);
}

static void test_char_modes(void)
{
    run_replay(CHAR_MODES, false);
}

static void test_cmap_xmap_wid(void)
{
    run_replay(CMAP_XMAP_WID, false);
}

static void test_root_icon(void)
{
    run_replay(ROOT_ICON, false);
}

static void test_overlay(void)
{
    run_replay(OVERLAY, false);
}

/*
 * Save the machine at the fixture's "# checkpoint" line and replay the rest
 * against the restored copy, whose checks need the overlay plane and the
 * per-WID overlay modes to have crossed the migration stream.
 */
static void test_overlay_migration(void)
{
    g_autofree char *contents = NULL;
    g_autofree char *checkpoint_path = NULL;
    g_autofree char *end_line = NULL;
    g_autofree char *start_line = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GError) error = NULL;
    unsigned int marker = 0;
    int fd;

    g_assert_true(g_file_get_contents(OVERLAY, &contents, NULL, &error));
    g_assert_no_error(error);
    lines = g_strsplit(contents, "\n", -1);
    for (unsigned int i = 0; lines[i]; i++) {
        if (!strcmp(lines[i], "# checkpoint")) {
            marker = i + 1;
            break;
        }
    }
    g_assert_cmpuint(marker, >, 0);

    fd = g_file_open_tmp("sgi-mgras-overlay-XXXXXX.state", &checkpoint_path,
                         &error);
    g_assert_no_error(error);
    close(fd);
    unlink(checkpoint_path);
    end_line = g_strdup_printf("%u", marker);
    start_line = g_strdup_printf("%u", marker + 1);

    g_setenv("MGRAS_END_LINE", end_line, true);
    g_setenv("MGRAS_CHECKPOINT_SAVE", checkpoint_path, true);
    run_replay(OVERLAY, false);
    g_unsetenv("MGRAS_CHECKPOINT_SAVE");
    g_unsetenv("MGRAS_END_LINE");

    g_setenv("MGRAS_CHECKPOINT_LOAD", checkpoint_path, true);
    g_setenv("MGRAS_START_LINE", start_line, true);
    run_replay(OVERLAY, false);
    g_unsetenv("MGRAS_CHECKPOINT_LOAD");
    g_unsetenv("MGRAS_START_LINE");
    unlink(checkpoint_path);
}

static void test_invalid_replay(const char *path, const char *message)
{
    if (g_test_subprocess()) {
        run_replay(g_getenv("MGRAS_INVALID_REPLAY"), false);
        return;
    }
    g_setenv("MGRAS_INVALID_REPLAY", path, true);
    /*
     * The child boots a real 8-CPU origin2000 fixture before it ever
     * reaches the invalid line, and that boot alone has been measured at
     * 10.5-11s under load -- past the old 10s trap timeout with nothing
     * left over for the parse itself. Give it the same order of headroom
     * the rest of this suite's budget uses.
     */
    g_test_trap_subprocess(NULL, 60 * G_TIME_SPAN_SECOND, 0);
    g_unsetenv("MGRAS_INVALID_REPLAY");
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(message);
}

static void test_invalid_number(void)
{
    test_invalid_replay(INVALID_REPLAY, "*invalid number*");
}

static void test_invalid_bounds(void)
{
    test_invalid_replay(INVALID_BOUNDS, "*outside the native surface*");
}

static void test_invalid_extra(void)
{
    test_invalid_replay(INVALID_EXTRA, "*requires X Y WIDTH HEIGHT VALUE*");
}

static void test_invalid_read_extra(void)
{
    test_invalid_replay(INVALID_READ_EXTRA,
                        "*read requires WIDTH OFFSET VALUE [MASK]*");
}

static void test_invalid_read_mask(void)
{
    test_invalid_replay(INVALID_READ_MASK,
                        "*MMIO mask is wider than width 1*");
}

static void test_invalid_read_mismatch(void)
{
    test_invalid_replay(INVALID_READ_MISMATCH,
                        "*invalid-read-mismatch.replay:3: read 0x73c08*"
                        "*expected 0x54*");
}

static void test_invalid_masked_mismatch(void)
{
    test_invalid_replay(INVALID_MASKED_MISMATCH,
                        "*invalid-masked-mismatch.replay:2: read 0x8000*"
                        "*expected 0x2 with mask 0x2*");
}

static void test_invalid_overflow(void)
{
    test_invalid_replay(INVALID_OVERFLOW, "*invalid number*");
}

static void test_invalid_wid_bounds(void)
{
    test_invalid_replay(INVALID_WID_BOUNDS,
                        "*outside the current model range*");
}

static void test_invalid_mismatch(void)
{
    test_invalid_replay(INVALID_MISMATCH,
                        "*invalid-mismatch.replay:2: pixel (0, 0)*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sgi-mgras/replay", test_replay);
    qtest_add_func("sgi-mgras/replay-screendump", test_replay_screendump);
    qtest_add_func("sgi-mgras/minimal-replay", test_minimal_replay);
    qtest_add_func("sgi-mgras/masked-read", test_masked_read);
    qtest_add_func("sgi-mgras/checkpoint-boundary-once",
                   test_checkpoint_boundary_runs_once);
    qtest_add_func("sgi-mgras/cmap-high-byte", test_cmap_high_byte);
    qtest_add_func("sgi-mgras/char-modes", test_char_modes);
    qtest_add_func("sgi-mgras/cmap-xmap-wid", test_cmap_xmap_wid);
    qtest_add_func("sgi-mgras/root-icon", test_root_icon);
    qtest_add_func("sgi-mgras/overlay", test_overlay);
    qtest_add_func("sgi-mgras/overlay-migration", test_overlay_migration);
    qtest_add_func("sgi-mgras/invalid-number", test_invalid_number);
    qtest_add_func("sgi-mgras/invalid-bounds", test_invalid_bounds);
    qtest_add_func("sgi-mgras/invalid-extra", test_invalid_extra);
    qtest_add_func("sgi-mgras/invalid-read-extra", test_invalid_read_extra);
    qtest_add_func("sgi-mgras/invalid-read-mask", test_invalid_read_mask);
    qtest_add_func("sgi-mgras/invalid-read-mismatch",
                   test_invalid_read_mismatch);
    qtest_add_func("sgi-mgras/invalid-masked-mismatch",
                   test_invalid_masked_mismatch);
    qtest_add_func("sgi-mgras/invalid-overflow", test_invalid_overflow);
    qtest_add_func("sgi-mgras/invalid-wid-bounds", test_invalid_wid_bounds);
    qtest_add_func("sgi-mgras/invalid-mismatch", test_invalid_mismatch);
    return g_test_run();
}
