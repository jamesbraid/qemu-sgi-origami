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

#define SPACE_XTALK       0x20000000000ULL
#define SI_BASE           (SPACE_XTALK + (0xfULL << 24))
#define SI_REG(offset)    (SI_BASE + (offset))
#define MACHINE_ARGS      "-M origin2000 -smp 8 -m 256M"
#define SI_VIZ_QOM_PATH   "/machine/si-viz"
#define DEFAULT_REPLAY    "../tests/data/mgras/fill.replay"
#define MINIMAL_REPLAY    "../tests/data/mgras/minimal.replay"
#define SI_MMIO_SIZE      (2 * MiB)

static uint64_t parse_number(const char *path, unsigned int line,
                             const char *word)
{
    char *end;
    uint64_t value;

    errno = 0;
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
        "'property': %s}}", SI_VIZ_QOM_PATH, property);
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
        SI_VIZ_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}",
        SI_VIZ_QOM_PATH, y);
    return qom_get_uint(qts, display ? "x-test-display-pixel" :
                                      "x-test-pixel");
}

static void replay_mmio(QTestState *qts, const char *path, unsigned int line,
                        char **words, bool write)
{
    uint64_t width, offset, value, actual;

    if (!words[1] || !words[2] || !words[3] || words[4]) {
        g_error("%s:%u: %s requires WIDTH OFFSET VALUE", path, line,
                words[0]);
    }
    width = parse_number(path, line, words[1]);
    offset = parse_number(path, line, words[2]);
    value = parse_number(path, line, words[3]);
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        g_error("%s:%u: unsupported MMIO width %" PRIu64, path, line, width);
    }
    if (offset >= SI_MMIO_SIZE || offset + width > SI_MMIO_SIZE) {
        g_error("%s:%u: MMIO access is outside the MGRAS aperture", path,
                line);
    }
    if (width < 8 && value >= (1ULL << (width * 8))) {
        g_error("%s:%u: MMIO value is wider than width %" PRIu64, path,
                line, width);
    }
    if (write) {
        if (width == 1) {
            qtest_writeb(qts, SI_REG(offset), value);
        } else if (width == 2) {
            qtest_writew(qts, SI_REG(offset), value);
        } else if (width == 4) {
            qtest_writel(qts, SI_REG(offset), value);
        } else {
            qtest_writeq(qts, SI_REG(offset), value);
        }
        return;
    }

    if (width == 1) {
        actual = qtest_readb(qts, SI_REG(offset));
    } else if (width == 2) {
        actual = qtest_readw(qts, SI_REG(offset));
    } else if (width == 4) {
        actual = qtest_readl(qts, SI_REG(offset));
    } else {
        actual = qtest_readq(qts, SI_REG(offset));
    }
    if (actual != value) {
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
    if (x > UINT32_MAX || y > UINT32_MAX || expected > UINT32_MAX) {
        g_error("%s:%u: pixel value is wider than 32 bits", path, line);
    }
    actual = pixel(qts, x, y, display);
    if (actual != expected) {
        g_error("%s:%u: pixel (%" PRIu64 ", %" PRIu64 ") returned 0x%"
                PRIx64 ", expected 0x%" PRIx64,
                path, line, x, y, actual, expected);
    }
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
    } else {
        g_error("%s:%u: unknown or malformed operation '%s'", path, line,
                words[0]);
    }
}

static void run_replay(const char *path, bool check_default_trace)
{
    const char *trace = g_getenv("MGRAS_TRACE");
    g_autofree char *contents = NULL;
    g_autofree char *trace_contents = NULL;
    g_autofree char *temporary_trace = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GError) error = NULL;
    bool remove_trace = false;
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
    qts = qtest_initf(MACHINE_ARGS
                      " -trace enable=sgi_mgras_mmio -D %s", trace);
    lines = g_strsplit(contents, "\n", -1);
    for (unsigned int i = 0; lines[i]; i++) {
        replay_line(qts, path, i + 1, lines[i]);
    }
    qtest_quit(qts);
    if (!g_file_get_contents(trace, &trace_contents, NULL, &error)) {
        g_error("cannot read trace %s: %s", trace, error->message);
    }
    if (check_default_trace) {
        g_assert_nonnull(strstr(trace_contents,
            "offset 0x73c08 value 0x55 size 1 write=1"));
        g_assert_nonnull(strstr(trace_contents,
            "offset 0x73c08 value 0x55 size 1 write=0"));
        g_assert_nonnull(strstr(trace_contents,
            "offset 0x720b0 value 0x2468 size 2 write=0"));
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

static void test_minimal_replay(void)
{
    run_replay(MINIMAL_REPLAY, false);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sgi-mgras/replay", test_replay);
    qtest_add_func("sgi-mgras/minimal-replay", test_minimal_replay);
    return g_test_run();
}
