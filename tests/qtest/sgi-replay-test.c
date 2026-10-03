/*
 * Replay a reduced, device-relative MMIO stream against an SGI graphics
 * head.  A fixture names its head and nothing else about the machine: the
 * head's entry in replay_heads[] chooses the host machine, where the head's
 * registers sit and how to reach its test properties.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

/*
 * The fixture format.  Blank lines and lines whose first word starts with
 * '#' are ignored wherever they appear.  The first record is the version
 * line, "sgi-replay 2", and the second names the graphics head the fixture
 * drives, "head CODENAME".  Every later record is a verb.
 *
 * Version 1 is the MGRAS replay format from before fixtures carried a
 * header: the same verbs, no version line and no head.  The harness refuses
 * it rather than guess the head, and scripts/sgi-replay-convert.py rewrites
 * it; a version 1 file can only be an MGRAS one.
 */
#define REPLAY_FORMAT_VERSION 2

/*
 * What a head's codename selects.  A family joins the harness by adding one
 * entry here; nothing else in this file names a family.
 *
 * @machine is the whole -M/-smp/-m (and any -device) argument string.
 * @mmio_base is where the machine maps offset 0 of the head's register
 * window, @mmio_size that window's size; write and read offsets are checked
 * against it.  @qom_path is the head's device, which must provide the
 * x-test-pixel-x/-y selectors and the x-test-pixel, x-test-display-pixel,
 * x-test-wid and x-test-overlay values the pixel verbs read.  @width and
 * @height bound those verbs' coordinates.  @trace is the -trace arguments
 * that record the head's register accesses into SGI_REPLAY_TRACE.
 * @console, when set, is the qdev id of the device whose console the
 * screendump verb reads; NULL reads console 0, which is the head only
 * when the machine builds no other display first.
 */
typedef struct ReplayHead {
    const char *codename;
    const char *machine;
    uint64_t mmio_base;
    uint64_t mmio_size;
    const char *qom_path;
    uint32_t width;
    uint32_t height;
    const char *trace;
    const char *console;
} ReplayHead;

static const ReplayHead replay_heads[] = {
    {
        /*
         * The SI captures come from a one-node Origin 2000 with 256 MiB
         * across node 0's first two banks; the deskside is that one-node
         * module, the SI board is fitted in its IO3, and it answers as
         * widget 0xf.
         */
        .codename = "sgi-mgras",
        .machine = "-M origin2000,topology=origin2000-deskside,nodes=1 "
                   "-smp 2 -m 256M -device sgi-mgras,slot=io3",
        .mmio_base = SPACE_IO + (0xfULL << 24),
        .mmio_size = 2 * MiB,
        .qom_path = "/machine/module0/io3",
        .width = 1280,
        .height = 1024,
        .trace = "-trace enable=sgi_mgras_mmio "
                 "-trace enable=sgi_mgras_control_mmio",
    },
    {
        /*
         * InfiniteReality as the KONA qtests build it: one pipe fitted at
         * the empty slot IO4, crossbow0's widget 11, whose small window
         * is 16 MiB.
         */
        .codename = "sgi-kona",
        .machine = "-M origin2000 -smp 8 -m 256M "
                   "-device sgi-kona,id=kona0,slot=io4",
        .mmio_base = SPACE_IO + (11ULL << 24),
        .mmio_size = 16 * MiB,
        .qom_path = "/machine/module0/kona0",
        .width = 1280,
        .height = 1024,
        .trace = "-trace enable=sgi_kona_pio_read "
                 "-trace enable=sgi_kona_pio_write",
        /*
         * Named, so the screendump reads this head whatever display the
         * machine builds first.
         */
        .console = "kona0",
    },
};

static const ReplayHead *find_head(const char *codename)
{
    for (size_t i = 0; i < ARRAY_SIZE(replay_heads); i++) {
        if (!strcmp(replay_heads[i].codename, codename)) {
            return &replay_heads[i];
        }
    }
    return NULL;
}

#define DEFAULT_REPLAY    "fill.replay"
#define MINIMAL_REPLAY    "minimal.replay"
#define MASKED_READ       "masked-read.replay"
#define CMAP_HIGH_BYTE    "cmap-high-byte.replay"
#define CHAR_MODES        "char-modes.replay"
#define CMAP_XMAP_WID     "cmap-xmap-wid.replay"
#define ROOT_ICON         "root-icon.replay"
#define OVERLAY           "overlay.replay"
#define DMA_IMAGE_TRANSFER "dma-image-transfer.replay"
#define INVALID_REPLAY    "invalid.replay"
#define INVALID_BOUNDS    "invalid-bounds.replay"
#define INVALID_EXTRA     "invalid-extra.replay"
#define INVALID_READ_EXTRA "invalid-read-extra.replay"
#define INVALID_READ_MASK "invalid-read-mask.replay"
#define INVALID_READ_MISMATCH \
    "invalid-read-mismatch.replay"
#define INVALID_MASKED_MISMATCH \
    "invalid-masked-mismatch.replay"
#define INVALID_OVERFLOW  "invalid-overflow.replay"
#define INVALID_WID_BOUNDS "invalid-wid-bounds.replay"
#define INVALID_MISMATCH  "invalid-mismatch.replay"
#define INVALID_DMA_EXTRA "invalid-dma-extra.replay"
#define INVALID_DMA_HEX_MISMATCH \
    "invalid-dma-hex-mismatch.replay"
#define KONA_MAINTENANCE_MENU "maintenance-menu.replay"

static char *replay_family_fixture_path(const char *family, const char *name)
{
    return g_test_build_filename(G_TEST_DIST, "..", "data", family, name,
                                 NULL);
}

static char *replay_fixture_path(const char *name)
{
    return replay_family_fixture_path("mgras", name);
}

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

static uint64_t qom_get_uint(QTestState *qts, const ReplayHead *head,
                             const char *property)
{
    QDict *response = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': %s}}", head->qom_path, property);
    uint64_t value;

    if (!qdict_haskey(response, "return")) {
        g_autoptr(GString) text = qobject_to_json(QOBJECT(response));
        g_error("qom-get %s failed: %s", property, text->str);
    }
    value = qdict_get_int(response, "return");
    qobject_unref(response);
    return value;
}

/* Select pixel (@x, @y) on the head and read one of its values there. */
static uint32_t plane_value(QTestState *qts, const ReplayHead *head,
                            uint32_t x, uint32_t y, const char *property)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}",
        head->qom_path, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}",
        head->qom_path, y);
    return qom_get_uint(qts, head, property);
}

static uint32_t pixel(QTestState *qts, const ReplayHead *head, uint32_t x,
                      uint32_t y, bool display)
{
    return plane_value(qts, head, x, y,
                       display ? "x-test-display-pixel" : "x-test-pixel");
}

static void replay_mmio(QTestState *qts, const ReplayHead *head,
                        const char *path, unsigned int line, char **words,
                        bool write)
{
    uint64_t width, offset, value, actual, mask, addr;
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
    if (offset >= head->mmio_size || width > head->mmio_size - offset) {
        g_error("%s:%u: MMIO access is outside the %s aperture", path,
                line, head->codename);
    }
    addr = head->mmio_base + offset;
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
            qtest_writeb(qts, addr, value);
        } else if (width == 2) {
            qtest_writew(qts, addr, value);
        } else if (width == 4) {
            qtest_writel(qts, addr, value);
        } else {
            qtest_writeq(qts, addr, value);
        }
        return;
    }

    if (width == 1) {
        actual = qtest_readb(qts, addr);
    } else if (width == 2) {
        actual = qtest_readw(qts, addr);
    } else if (width == 4) {
        actual = qtest_readl(qts, addr);
    } else {
        actual = qtest_readq(qts, addr);
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

/*
 * A replay has no guest memory behind it, so a captured sgi_mgras_dma_chunk
 * event is the only way a host-to-screen DMA draw's source bytes reach a
 * replayed session. ADDR is the plain node-offset address the capturing
 * guest wrote to, exactly as sgi_mgras_dma_chunk recorded it -- not the
 * fully composed fabric address sgi_mgras_xio_compose() derives from it
 * (the widget shifted into the high bits) -- because qtest_memwrite only
 * reaches ordinary guest RAM at the plain address; the fabric address
 * exists solely to route the model's own dma_memory_read() through
 * s->fabric_as and preloading there leaves the plain address, and so the
 * model's read, untouched. HEX is the same lowercase, unseparated hex-pair
 * encoding qemu_hexdump_to_buffer() writes into the trace;
 * MGRAS_DMA_CHUNK_MAX_BYTES in hw/misc/sgi-mgras-raster.c bounds a single
 * captured chunk the same way.
 */
#define REPLAY_DMA_MAX_BYTES 4096

static void replay_dma(QTestState *qts, const char *path, unsigned int line,
                       char **words)
{
    uint64_t addr, len;
    size_t hexlen;
    g_autofree uint8_t *bytes = NULL;

    if (!words[1] || !words[2] || !words[3] || words[4]) {
        g_error("%s:%u: dma requires ADDR LEN HEX", path, line);
    }
    addr = parse_number(path, line, words[1]);
    len = parse_number(path, line, words[2]);
    if (!len || len > REPLAY_DMA_MAX_BYTES) {
        g_error("%s:%u: dma length %" PRIu64 " is out of range", path, line,
                len);
    }
    hexlen = strlen(words[3]);
    if (hexlen != len * 2) {
        g_error("%s:%u: dma hex length does not match LEN %" PRIu64, path,
                line, len);
    }
    bytes = g_malloc(len);
    for (uint64_t i = 0; i < len; i++) {
        int hi = g_ascii_xdigit_value(words[3][i * 2]);
        int lo = g_ascii_xdigit_value(words[3][i * 2 + 1]);

        if (hi < 0 || lo < 0) {
            g_error("%s:%u: dma hex is not valid hex", path, line);
        }
        bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    qtest_memwrite(qts, addr, bytes, len);
}

static void replay_pixel(QTestState *qts, const ReplayHead *head,
                         const char *path, unsigned int line, char **words,
                         bool display)
{
    uint64_t x, y, expected, actual;

    if (!words[1] || !words[2] || !words[3] || words[4]) {
        g_error("%s:%u: %s requires X Y VALUE", path, line, words[0]);
    }
    x = parse_number(path, line, words[1]);
    y = parse_number(path, line, words[2]);
    expected = parse_number(path, line, words[3]);
    if (x >= head->width || y >= head->height || expected > UINT32_MAX) {
        g_error("%s:%u: pixel is outside the native surface", path, line);
    }
    actual = pixel(qts, head, x, y, display);
    if (actual != expected) {
        g_error("%s:%u: pixel (%" PRIu64 ", %" PRIu64 ") returned 0x%"
                PRIx64 ", expected 0x%" PRIx64,
                path, line, x, y, actual, expected);
    }
}

static void replay_pixels(QTestState *qts, const ReplayHead *head,
                          const char *path, unsigned int line, char **words)
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
        !height || x >= head->width || y >= head->height ||
        width > head->width - x || height > head->height - y) {
        g_error("%s:%u: pixels rectangle is outside the native surface",
                path, line);
    }
    for (uint32_t row = 0; row < height; row++) {
        for (uint32_t column = 0; column < width; column++) {
            uint32_t actual = pixel(qts, head, x + column, y + row, false);
            if (actual != expected) {
                g_error("%s:%u: pixels (%u,%u) returned 0x%08x, expected "
                        "0x%08x", path, line, (uint32_t)(x + column),
                        (uint32_t)(y + row),
                        actual, (uint32_t)expected);
            }
        }
    }
}

/*
 * Compare the head's screendump with FILE byte for byte.  A relative FILE
 * is taken from the fixture's own directory, so a family keeps its
 * reference images beside its fixtures.  The dump is always PPM: a PNG's
 * bytes depend on the zlib the build links, and a PPM's depend only on the
 * pixels.
 */
static void replay_screendump(QTestState *qts, const ReplayHead *head,
                              const char *path, unsigned int line,
                              char **words)
{
    g_autofree char *dir = NULL;
    g_autofree char *reference = NULL;
    g_autofree char *dump = NULL;
    g_autofree char *expected = NULL;
    g_autofree char *actual = NULL;
    g_autoptr(GError) error = NULL;
    gsize expected_len, actual_len, same = 0;
    int fd;

    if (!words[1] || words[2]) {
        g_error("%s:%u: screendump requires FILE", path, line);
    }
    if (g_path_is_absolute(words[1])) {
        reference = g_strdup(words[1]);
    } else {
        dir = g_path_get_dirname(path);
        reference = g_build_filename(dir, words[1], NULL);
    }
    if (!g_file_get_contents(reference, &expected, &expected_len, &error)) {
        g_error("%s:%u: cannot read screendump reference: %s", path, line,
                error->message);
    }

    fd = g_file_open_tmp("sgi-replay-XXXXXX.ppm", &dump, &error);
    if (fd < 0) {
        g_error("cannot create screendump file: %s", error->message);
    }
    close(fd);
    if (head->console) {
        qtest_qmp_assert_success(qts,
            "{'execute': 'screendump', 'arguments': {'filename': %s, "
            "'format': 'ppm', 'device': %s}}", dump, head->console);
    } else {
        qtest_qmp_assert_success(qts,
            "{'execute': 'screendump', 'arguments': {'filename': %s, "
            "'format': 'ppm'}}", dump);
    }
    if (!g_file_get_contents(dump, &actual, &actual_len, &error)) {
        g_error("cannot read screendump %s: %s", dump, error->message);
    }
    unlink(dump);

    while (same < actual_len && same < expected_len &&
           actual[same] == expected[same]) {
        same++;
    }
    if (same != actual_len || same != expected_len) {
        g_error("%s:%u: screendump differs from %s: %" G_GSIZE_FORMAT
                " bytes against %" G_GSIZE_FORMAT ", first difference at "
                "byte %" G_GSIZE_FORMAT, path, line, reference, actual_len,
                expected_len, same);
    }
}

/* Check one byte-wide plane: the WID tag or the overlay index. */
static void replay_plane(QTestState *qts, const ReplayHead *head,
                         const char *path, unsigned int line, char **words,
                         const char *property)
{
    uint64_t x, y, expected, actual;

    if (!words[1] || !words[2] || !words[3] || words[4]) {
        g_error("%s:%u: %s requires X Y VALUE", path, line, words[0]);
    }
    x = parse_number(path, line, words[1]);
    y = parse_number(path, line, words[2]);
    expected = parse_number(path, line, words[3]);
    if (x >= head->width || y >= head->height || expected > UINT8_MAX) {
        g_error("%s:%u: %s value is outside the current model range", path,
                line, words[0]);
    }
    actual = plane_value(qts, head, x, y, property);
    if (actual != expected) {
        g_error("%s:%u: %s (%" PRIu64 ",%" PRIu64 ") returned 0x%02" PRIx64
                ", expected 0x%02" PRIx64, path, line, words[0], x, y, actual,
                expected);
    }
}

/*
 * A line number given through SGI_REPLAY_START_LINE/SGI_REPLAY_END_LINE,
 * not one read out of a replay file -- parse_number()'s own messages assume
 * the latter, so this reports the offending environment variable instead.
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

/* Split one fixture line into its words, dropping empty ones. */
static char **split_words(const char *text)
{
    g_auto(GStrv) raw = g_strsplit_set(text, " \t\r", -1);
    GPtrArray *words = g_ptr_array_new();

    for (size_t i = 0; raw[i]; i++) {
        if (*raw[i]) {
            g_ptr_array_add(words, g_strdup(raw[i]));
        }
    }
    g_ptr_array_add(words, NULL);
    return (char **)g_ptr_array_free(words, false);
}

/*
 * Read the version line and the head record from @lines.  Returns the head
 * and sets *@header_end to the head record's line number, so the replay
 * proper starts after it.
 */
static const ReplayHead *parse_header(const char *path, char **lines,
                                      unsigned int *header_end)
{
    unsigned int records = 0;

    for (unsigned int i = 0; lines[i]; i++) {
        unsigned int line = i + 1;
        g_auto(GStrv) words = split_words(lines[i]);
        const ReplayHead *head;
        uint64_t version;

        if (!words[0] || words[0][0] == '#') {
            continue;
        }
        if (records++ == 0) {
            if (strcmp(words[0], "sgi-replay")) {
                g_error("%s:%u: a replay starts with the version line "
                        "'sgi-replay %d'; a version 1 MGRAS fixture converts "
                        "with scripts/sgi-replay-convert.py", path, line,
                        REPLAY_FORMAT_VERSION);
            }
            if (!words[1] || words[2]) {
                g_error("%s:%u: sgi-replay requires VERSION", path, line);
            }
            version = parse_number(path, line, words[1]);
            if (version != REPLAY_FORMAT_VERSION) {
                g_error("%s:%u: replay format version %" PRIu64 " is not "
                        "supported; this harness reads version %d", path,
                        line, version, REPLAY_FORMAT_VERSION);
            }
            continue;
        }
        if (strcmp(words[0], "head") || !words[1] || words[2]) {
            g_error("%s:%u: the version line must be followed by "
                    "'head CODENAME'", path, line);
        }
        head = find_head(words[1]);
        if (!head) {
            g_error("%s:%u: unknown head '%s'", path, line, words[1]);
        }
        *header_end = line;
        return head;
    }
    g_error("%s: no %s", path, records ? "head record" : "version line");
}

static void replay_line(QTestState *qts, const ReplayHead *head,
                        const char *path, unsigned int line, char *text)
{
    g_auto(GStrv) words = split_words(text);

    if (!words[0] || words[0][0] == '#') {
        return;
    }
    if (!strcmp(words[0], "sgi-replay") || !strcmp(words[0], "head")) {
        g_error("%s:%u: %s must lead the fixture", path, line, words[0]);
    }
    if (!strcmp(words[0], "reset") && !words[1]) {
        qtest_system_reset(qts);
    } else if (!strcmp(words[0], "write")) {
        replay_mmio(qts, head, path, line, words, true);
    } else if (!strcmp(words[0], "read")) {
        replay_mmio(qts, head, path, line, words, false);
    } else if (!strcmp(words[0], "dma")) {
        replay_dma(qts, path, line, words);
    } else if (!strcmp(words[0], "pixel")) {
        replay_pixel(qts, head, path, line, words, false);
    } else if (!strcmp(words[0], "display-pixel")) {
        replay_pixel(qts, head, path, line, words, true);
    } else if (!strcmp(words[0], "pixels")) {
        replay_pixels(qts, head, path, line, words);
    } else if (!strcmp(words[0], "wid")) {
        replay_plane(qts, head, path, line, words, "x-test-wid");
    } else if (!strcmp(words[0], "overlay")) {
        replay_plane(qts, head, path, line, words, "x-test-overlay");
    } else if (!strcmp(words[0], "screendump")) {
        replay_screendump(qts, head, path, line, words);
    } else {
        g_error("%s:%u: unknown or malformed operation '%s'", path, line,
                words[0]);
    }
}

/*
 * Replay @path against a fresh machine, or, with SGI_REPLAY_CHECKPOINT_LOAD
 * set, against one restored from an earlier run's SGI_REPLAY_CHECKPOINT_SAVE.
 * Either way SGI_REPLAY_START_LINE/SGI_REPLAY_END_LINE (1-based, inclusive,
 * default the whole file) select which lines of @path this process replays --
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
static void run_replay(const char *path,
                       void (*check_trace)(const char *trace_contents))
{
    const char *trace = g_getenv("SGI_REPLAY_TRACE");
    const char *screendump = g_getenv("SGI_REPLAY_SCREENDUMP");
    const char *checkpoint_load = g_getenv("SGI_REPLAY_CHECKPOINT_LOAD");
    const char *checkpoint_save = g_getenv("SGI_REPLAY_CHECKPOINT_SAVE");
    const char *start_line_env = g_getenv("SGI_REPLAY_START_LINE");
    const char *end_line_env = g_getenv("SGI_REPLAY_END_LINE");
    g_autofree char *contents = NULL;
    g_autofree char *trace_contents = NULL;
    g_autofree char *temporary_trace = NULL;
    g_autofree char *args = NULL;
    g_autofree char *load_uri = NULL;
    g_autofree char *save_uri = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GError) error = NULL;
    bool remove_trace = false;
    unsigned int line_count, start_line, end_line, header_end;
    const ReplayHead *head;
    QTestState *qts;
    int fd;

    if (!g_file_get_contents(path, &contents, NULL, &error)) {
        g_error("cannot read replay %s: %s", path, error->message);
    }
    lines = g_strsplit(contents, "\n", -1);
    head = parse_header(path, lines, &header_end);
    if (!trace || !*trace) {
        fd = g_file_open_tmp("sgi-replay-XXXXXX.trace",
                             &temporary_trace, &error);
        if (fd < 0) {
            g_error("cannot create trace file: %s", error->message);
        }
        close(fd);
        trace = temporary_trace;
        remove_trace = true;
    }
    args = g_strdup_printf("%s %s -D %s%s", head->machine, head->trace,
                           trace, checkpoint_load && *checkpoint_load ?
                           " -incoming defer" : "");
    qts = qtest_init(args);
    if (checkpoint_load && *checkpoint_load) {
        load_uri = g_strdup_printf("file:%s", checkpoint_load);
        qtest_qmp_assert_success(qts,
            "{'execute': 'migrate-incoming', 'arguments': {'uri': %s}}",
            load_uri);
        sgi_wait_migration_status(qts, "completed", 30);
    }

    line_count = g_strv_length(lines);
    start_line = start_line_env && *start_line_env ?
                 parse_env_line("SGI_REPLAY_START_LINE", start_line_env) : 1;
    end_line = end_line_env && *end_line_env ?
               parse_env_line("SGI_REPLAY_END_LINE", end_line_env) : line_count;
    if (start_line < 1 || end_line < start_line || end_line > line_count) {
        g_error("%s: line range %u..%u is outside 1..%u", path,
                start_line, end_line, line_count);
    }
    /* The header was read before the machine started. */
    for (unsigned int i = MAX(start_line, header_end + 1); i <= end_line;
         i++) {
        replay_line(qts, head, path, i, lines[i - 1]);
    }
    if (checkpoint_save && *checkpoint_save) {
        save_uri = g_strdup_printf("file:%s", checkpoint_save);
        sgi_migrate_stopped(qts, save_uri);
    }
    if (screendump && *screendump && head->console) {
        qtest_qmp_assert_success(qts,
            "{'execute': 'screendump', 'arguments': {'filename': %s, "
            "'device': %s}}", screendump, head->console);
    } else if (screendump && *screendump) {
        qtest_qmp_assert_success(qts,
            "{'execute': 'screendump', 'arguments': {'filename': %s}}",
            screendump);
    }
    qtest_quit(qts);
    if (!g_file_get_contents(trace, &trace_contents, NULL, &error)) {
        g_error("cannot read trace %s: %s", trace, error->message);
    }
    if (check_trace) {
        check_trace(trace_contents);
    }
    if (remove_trace) {
        unlink(trace);
    }
}

/*
 * fill.replay's register accesses as its head's trace records them: the
 * command-channel handshake reaches the control window, and the FIFO words
 * at 0x120400 and 0x130400 do not.
 */
static void check_fill_trace(const char *trace_contents)
{
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

static void run_fixture(const char *name,
                        void (*check_trace)(const char *))
{
    g_autofree char *path = replay_fixture_path(name);

    run_replay(path, check_trace);
}

static void test_replay(void)
{
    const char *path = g_getenv("SGI_REPLAY_FILE");
    bool custom = path && *path;

    if (custom) {
        run_replay(path, NULL);
    } else {
        run_fixture(DEFAULT_REPLAY, check_fill_trace);
    }
}

static void test_replay_screendump(void)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    int fd;

    fd = g_file_open_tmp("sgi-replay-XXXXXX.ppm", &path, &error);
    g_assert_no_error(error);
    close(fd);
    unlink(path);
    g_setenv("SGI_REPLAY_SCREENDUMP", path, true);
    run_fixture(MINIMAL_REPLAY, NULL);
    g_unsetenv("SGI_REPLAY_SCREENDUMP");
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(g_str_has_prefix(contents, "P6\n1280 1024\n255\n"));
    unlink(path);
}

static void test_minimal_replay(void)
{
    run_fixture(MINIMAL_REPLAY, NULL);
}

static void test_masked_read(void)
{
    run_fixture(MASKED_READ, NULL);
}

/*
 * SGI_REPLAY_START_LINE/SGI_REPLAY_END_LINE are plain line numbers with no
 * cross-check against an unrelated checkpoint file's own boundary --
 * resuming at the line a checkpoint was saved from, instead of the line
 * after it, would silently replay that line's write a second time. Prove
 * the documented N/N+1 contract instead: save after the second write of a
 * tiny fixture, resume at the read after it, and confirm that write appears
 * exactly once across the combined trace of both runs.
 */
static void test_checkpoint_boundary_runs_once(void)
{
    static const char fixture[] =
        "sgi-replay 2\n"
        "head sgi-mgras\n"
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

    g_setenv("SGI_REPLAY_END_LINE", "4", true);
    g_setenv("SGI_REPLAY_CHECKPOINT_SAVE", checkpoint_path, true);
    g_setenv("SGI_REPLAY_TRACE", trace_save_path, true);
    run_replay(fixture_path, NULL);
    g_unsetenv("SGI_REPLAY_CHECKPOINT_SAVE");

    g_setenv("SGI_REPLAY_CHECKPOINT_LOAD", checkpoint_path, true);
    g_setenv("SGI_REPLAY_START_LINE", "5", true);
    g_setenv("SGI_REPLAY_END_LINE", "5", true);
    g_setenv("SGI_REPLAY_TRACE", trace_load_path, true);
    run_replay(fixture_path, NULL);
    g_unsetenv("SGI_REPLAY_CHECKPOINT_LOAD");
    g_unsetenv("SGI_REPLAY_START_LINE");
    g_unsetenv("SGI_REPLAY_END_LINE");
    g_unsetenv("SGI_REPLAY_TRACE");

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
     * check_fill_trace()'s own scan above filters to one event name.
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

/* The 1-based number of the first line of @path that starts with @prefix. */
static unsigned int first_line_with_prefix(const char *path,
                                           const char *prefix)
{
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    lines = g_strsplit(contents, "\n", -1);
    for (unsigned int i = 0; lines[i]; i++) {
        if (g_str_has_prefix(lines[i], prefix)) {
            return i + 1;
        }
    }
    g_error("%s: no line starts with '%s'", path, prefix);
}

/*
 * checkpoint-boundary-once proves the MMIO-count contract at a line
 * boundary; neither it nor replay-screendump ever look at what a
 * screendump taken after SGI_REPLAY_CHECKPOINT_LOAD actually contains. Split
 * fill.replay before its display-enable write (the two display-pixel
 * assertions straddling it show display_enabled flips there) so the loaded
 * process both restores painted-but-not-yet-shown state through post_load
 * and then enables the display itself, and compare its screendump byte for
 * byte against one from an uninterrupted run of the same fixture.  The
 * split is found by content, the first display-pixel check, because a
 * pinned line number went stale as the fixture grew.
 */
static void test_checkpoint_screendump_matches_uninterrupted_replay(void)
{
    g_autofree char *fill_path = replay_fixture_path(DEFAULT_REPLAY);
    unsigned int split = first_line_with_prefix(fill_path,
                                                "display-pixel ");
    g_autofree char *end_line = g_strdup_printf("%u", split);
    g_autofree char *start_line = g_strdup_printf("%u", split + 1);
    g_autofree char *full_path = NULL;
    g_autofree char *split_path = NULL;
    g_autofree char *checkpoint_path = NULL;
    g_autofree char *full_contents = NULL;
    g_autofree char *split_contents = NULL;
    gsize full_len, split_len;
    g_autoptr(GError) error = NULL;
    int fd;

    fd = g_file_open_tmp("checkpoint-full-XXXXXX.ppm", &full_path, &error);
    g_assert_no_error(error);
    close(fd);
    unlink(full_path);
    fd = g_file_open_tmp("checkpoint-split-XXXXXX.ppm", &split_path, &error);
    g_assert_no_error(error);
    close(fd);
    unlink(split_path);
    fd = g_file_open_tmp("checkpoint-XXXXXX.state", &checkpoint_path,
                         &error);
    g_assert_no_error(error);
    close(fd);
    unlink(checkpoint_path);

    g_setenv("SGI_REPLAY_SCREENDUMP", full_path, true);
    run_fixture(DEFAULT_REPLAY, NULL);
    g_unsetenv("SGI_REPLAY_SCREENDUMP");

    g_setenv("SGI_REPLAY_END_LINE", end_line, true);
    g_setenv("SGI_REPLAY_CHECKPOINT_SAVE", checkpoint_path, true);
    run_fixture(DEFAULT_REPLAY, NULL);
    g_unsetenv("SGI_REPLAY_CHECKPOINT_SAVE");
    g_unsetenv("SGI_REPLAY_END_LINE");

    g_setenv("SGI_REPLAY_CHECKPOINT_LOAD", checkpoint_path, true);
    g_setenv("SGI_REPLAY_START_LINE", start_line, true);
    g_setenv("SGI_REPLAY_SCREENDUMP", split_path, true);
    run_fixture(DEFAULT_REPLAY, NULL);
    g_unsetenv("SGI_REPLAY_CHECKPOINT_LOAD");
    g_unsetenv("SGI_REPLAY_START_LINE");
    g_unsetenv("SGI_REPLAY_SCREENDUMP");

    g_assert_true(g_file_get_contents(full_path, &full_contents, &full_len,
                                      &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(split_path, &split_contents,
                                      &split_len, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(full_len, ==, split_len);
    if (memcmp(full_contents, split_contents, full_len) != 0) {
        g_error("checkpoint-load screendump differs from the uninterrupted "
                "replay's screendump (%zu bytes)", full_len);
    }

    unlink(full_path);
    unlink(split_path);
    unlink(checkpoint_path);
}

static void test_cmap_high_byte(void)
{
    run_fixture(CMAP_HIGH_BYTE, NULL);
}

static void test_char_modes(void)
{
    run_fixture(CHAR_MODES, NULL);
}

static void test_cmap_xmap_wid(void)
{
    run_fixture(CMAP_XMAP_WID, NULL);
}

static void test_root_icon(void)
{
    run_fixture(ROOT_ICON, NULL);
}

static void test_overlay(void)
{
    run_fixture(OVERLAY, NULL);
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

    g_autofree char *overlay_path = replay_fixture_path(OVERLAY);

    g_assert_true(g_file_get_contents(overlay_path, &contents, NULL, &error));
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

    g_setenv("SGI_REPLAY_END_LINE", end_line, true);
    g_setenv("SGI_REPLAY_CHECKPOINT_SAVE", checkpoint_path, true);
    run_fixture(OVERLAY, NULL);
    g_unsetenv("SGI_REPLAY_CHECKPOINT_SAVE");
    g_unsetenv("SGI_REPLAY_END_LINE");

    g_setenv("SGI_REPLAY_CHECKPOINT_LOAD", checkpoint_path, true);
    g_setenv("SGI_REPLAY_START_LINE", start_line, true);
    run_fixture(OVERLAY, NULL);
    g_unsetenv("SGI_REPLAY_CHECKPOINT_LOAD");
    g_unsetenv("SGI_REPLAY_START_LINE");
    unlink(checkpoint_path);
}

/*
 * A captured sgi_mgras_dma_chunk pair is the only way a replayed host-to-
 * screen DMA draw's source bytes reach the model: DMA_IMAGE_TRANSFER's two
 * 'dma' lines preload the page-list entry and the pixel row at their plain
 * node addresses, exactly matching what
 * sn0-origin2000-mgras-test.c's test_dma_image_transfer_traces_source_chunks
 * proved a real capture of this same register sequence records, before the
 * kick that makes the model read them back. The fixture also carries its
 * own command-channel handshake (copied from fill.replay), since unlike a
 * real capture -- which already contains the guest's own context entry
 * earlier in the session -- a synthetic fixture built straight from a qtest
 * register sequence has to supply it explicitly, or REG_CONTEXT_COMMAND
 * ignores every paint command that follows.
 */
static void test_dma_image_transfer(void)
{
    run_fixture(DMA_IMAGE_TRANSFER, NULL);
}

static void test_invalid_replay(const char *path, const char *message)
{
    if (g_test_subprocess()) {
        run_replay(g_getenv("SGI_REPLAY_INVALID_FILE"), NULL);
        return;
    }
    g_setenv("SGI_REPLAY_INVALID_FILE", path, true);
    /*
     * The child boots a real origin2000 fixture before it ever reaches
     * the invalid line. On the eight-CPU machine this harness used before
     * it replayed on the one-node deskside, that boot alone was measured at
     * 10.5-11s under load -- past the old 10s trap timeout with nothing
     * left over for the parse itself. Give it the same order of headroom
     * the rest of this suite's budget uses.
     */
    g_test_trap_subprocess(NULL, 60 * G_TIME_SPAN_SECOND, 0);
    g_unsetenv("SGI_REPLAY_INVALID_FILE");
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(message);
}

static void test_invalid_fixture(const char *name, const char *message)
{
    g_autofree char *path = replay_fixture_path(name);

    test_invalid_replay(path, message);
}

static void test_invalid_number(void)
{
    test_invalid_fixture(INVALID_REPLAY, "*invalid number*");
}

static void test_invalid_bounds(void)
{
    test_invalid_fixture(INVALID_BOUNDS, "*outside the native surface*");
}

static void test_invalid_extra(void)
{
    test_invalid_fixture(INVALID_EXTRA, "*requires X Y WIDTH HEIGHT VALUE*");
}

static void test_invalid_read_extra(void)
{
    test_invalid_fixture(INVALID_READ_EXTRA,
                        "*read requires WIDTH OFFSET VALUE [MASK]*");
}

static void test_invalid_read_mask(void)
{
    test_invalid_fixture(INVALID_READ_MASK,
                        "*MMIO mask is wider than width 1*");
}

static void test_invalid_read_mismatch(void)
{
    test_invalid_fixture(INVALID_READ_MISMATCH,
                        "*invalid-read-mismatch.replay:5: read 0x73c08*"
                        "*expected 0x54*");
}

static void test_invalid_masked_mismatch(void)
{
    test_invalid_fixture(INVALID_MASKED_MISMATCH,
                        "*invalid-masked-mismatch.replay:4: read 0x8000*"
                        "*expected 0x2 with mask 0x2*");
}

static void test_invalid_overflow(void)
{
    test_invalid_fixture(INVALID_OVERFLOW, "*invalid number*");
}

static void test_invalid_wid_bounds(void)
{
    test_invalid_fixture(INVALID_WID_BOUNDS,
                        "*outside the current model range*");
}

static void test_invalid_mismatch(void)
{
    test_invalid_fixture(INVALID_MISMATCH,
                        "*invalid-mismatch.replay:4: pixel (0, 0)*");
}

static void test_invalid_dma_extra(void)
{
    test_invalid_fixture(INVALID_DMA_EXTRA, "*dma requires ADDR LEN HEX*");
}

static void test_invalid_dma_hex_mismatch(void)
{
    test_invalid_fixture(INVALID_DMA_HEX_MISMATCH,
                        "*dma hex length does not match LEN*");
}

/*
 * Take a reference screendump of fill.replay, then replay a copy of the
 * fixture that ends in "screendump REFERENCE" from a directory of its own,
 * so the reference is found relative to the fixture.  With @corrupt the
 * reference has one byte changed and the record must refuse it.
 */
static void run_screendump_record(bool corrupt)
{
    g_autofree char *dir = NULL;
    g_autofree char *reference = NULL;
    g_autofree char *fixture = NULL;
    g_autofree char *fill = NULL;
    g_autofree char *image = NULL;
    g_autofree char *text = NULL;
    gsize image_len;
    g_autoptr(GError) error = NULL;

    if (g_test_subprocess()) {
        run_replay(g_getenv("SGI_REPLAY_INVALID_FILE"), NULL);
        return;
    }
    dir = g_dir_make_tmp("sgi-replay-XXXXXX", &error);
    g_assert_no_error(error);
    reference = g_build_filename(dir, "fill.ppm", NULL);
    fixture = g_build_filename(dir, "fill.replay", NULL);

    g_setenv("SGI_REPLAY_SCREENDUMP", reference, true);
    run_fixture(DEFAULT_REPLAY, NULL);
    g_unsetenv("SGI_REPLAY_SCREENDUMP");

    g_autofree char *fill_path = replay_fixture_path(DEFAULT_REPLAY);

    g_assert_true(g_file_get_contents(fill_path, &fill, NULL, &error));
    g_assert_no_error(error);
    text = g_strconcat(fill, "screendump fill.ppm\n", NULL);
    g_assert_true(g_file_set_contents(fixture, text, -1, &error));
    g_assert_no_error(error);

    if (corrupt) {
        g_assert_true(g_file_get_contents(reference, &image, &image_len,
                                          &error));
        g_assert_no_error(error);
        g_assert_cmpuint(image_len, >, 100);
        image[image_len - 100] ^= 1;
        g_assert_true(g_file_set_contents(reference, image, image_len,
                                          &error));
        g_assert_no_error(error);
        test_invalid_replay(fixture, "*fill.replay:*: screendump differs "
                            "from *fill.ppm*first difference at byte *");
    } else {
        run_replay(fixture, NULL);
    }

    unlink(fixture);
    unlink(reference);
    rmdir(dir);
}

static void test_screendump_record(void)
{
    run_screendump_record(false);
}

static void test_screendump_record_mismatch(void)
{
    run_screendump_record(true);
}

/*
 * A header fault is refused before any machine starts, so these write their
 * fixture to a temporary file instead of carrying one per case.
 */
static void test_invalid_header(const char *fixture, const char *message)
{
    g_autofree char *path = NULL;
    g_autoptr(GError) error = NULL;
    int fd;

    if (g_test_subprocess()) {
        run_replay(g_getenv("SGI_REPLAY_INVALID_FILE"), NULL);
        return;
    }
    fd = g_file_open_tmp("sgi-replay-header-XXXXXX.replay", &path, &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(path, fixture, -1, &error));
    g_assert_no_error(error);
    test_invalid_replay(path, message);
    unlink(path);
}

static void test_invalid_no_version(void)
{
    test_invalid_header("# a version 1 fixture\n"
                        "read 4 0x008000 0x0\n",
                        "*:2: a replay starts with the version line "
                        "'sgi-replay 2'*");
}

static void test_invalid_version(void)
{
    test_invalid_header("sgi-replay 3\nhead sgi-mgras\n",
                        "*:1: replay format version 3 is not supported*");
}

static void test_invalid_no_head(void)
{
    test_invalid_header("sgi-replay 2\nread 4 0x008000 0x0\n",
                        "*:2: the version line must be followed by "
                        "'head CODENAME'*");
}

/*
 * The IO6 PROM drawing its graphical System Maintenance Menu on
 * InfiniteReality: a captured boot's register accesses, its screendump
 * compared byte for byte at the end. See the fixture's own comments.
 */
static void test_kona_trace_ordering(void)
{
    const ReplayHead *head = find_head("sgi-kona");
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    unsigned count = 0;
    uint64_t previous = 0;
    int fd = g_file_open_tmp("sgi-kona-order-XXXXXX.trace", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    args = g_strdup_printf("%s %s -D %s", head->machine, head->trace, path);
    qts = qtest_init(args);
    /* One original access per event, including internally split doublewords. */
    qtest_writeq(qts, head->mmio_base + 0x800328, 0x1122334455667788);
    qtest_readq(qts, head->mmio_base + 0x800328);
    qtest_writel(qts, head->mmio_base + 0x800328, 0x99aabbcc);
    qtest_readl(qts, head->mmio_base + 0x800328);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    lines = g_strsplit(contents, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        const char *event = strstr(lines[i], "sgi_kona_pio_");
        const char *seq;
        uint64_t sequence;

        if (!event) {
            continue;
        }
        seq = strstr(event, "seq ");
        g_assert_nonnull(seq);
        g_assert_cmpint(sscanf(seq, "seq %" SCNu64, &sequence), ==, 1);
        if (count) {
            g_assert_cmpuint(sequence, ==, previous + 1);
        }
        g_assert_nonnull(strstr(event, count < 2 ? "size 8" : "size 4"));
        g_assert_nonnull(strstr(event, count % 2 ? "pio_read" : "pio_write"));
        previous = sequence;
        count++;
    }
    g_assert_cmpuint(count, ==, 4);
    unlink(path);
}

static void test_kona_maintenance_menu(void)
{
    g_autofree char *path = replay_family_fixture_path(
        "kona", KONA_MAINTENANCE_MENU);

    run_replay(path, NULL);
}

static void test_invalid_unknown_head(void)
{
    test_invalid_header("sgi-replay 2\nhead sgi-nonesuch\n",
                        "*:2: unknown head 'sgi-nonesuch'*");
}

static void test_invalid_late_head(void)
{
    test_invalid_header("sgi-replay 2\nhead sgi-mgras\n"
                        "read 4 0x008000 0x0\nhead sgi-mgras\n",
                        "*:4: head must lead the fixture*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sgi-replay/mgras/replay", test_replay);
    qtest_add_func("sgi-replay/mgras/replay-screendump",
                   test_replay_screendump);
    qtest_add_func("sgi-replay/mgras/minimal-replay", test_minimal_replay);
    qtest_add_func("sgi-replay/mgras/masked-read", test_masked_read);
    qtest_add_func("sgi-replay/mgras/checkpoint-boundary-once",
                   test_checkpoint_boundary_runs_once);
    qtest_add_func("sgi-replay/mgras/checkpoint-screendump-matches-replay",
                   test_checkpoint_screendump_matches_uninterrupted_replay);
    qtest_add_func("sgi-replay/mgras/cmap-high-byte", test_cmap_high_byte);
    qtest_add_func("sgi-replay/mgras/char-modes", test_char_modes);
    qtest_add_func("sgi-replay/mgras/cmap-xmap-wid", test_cmap_xmap_wid);
    qtest_add_func("sgi-replay/mgras/root-icon", test_root_icon);
    qtest_add_func("sgi-replay/mgras/overlay", test_overlay);
    qtest_add_func("sgi-replay/mgras/overlay-migration",
                   test_overlay_migration);
    qtest_add_func("sgi-replay/mgras/dma-image-transfer",
                   test_dma_image_transfer);
    qtest_add_func("sgi-replay/mgras/invalid-number", test_invalid_number);
    qtest_add_func("sgi-replay/mgras/invalid-bounds", test_invalid_bounds);
    qtest_add_func("sgi-replay/mgras/invalid-extra", test_invalid_extra);
    qtest_add_func("sgi-replay/mgras/invalid-read-extra",
                   test_invalid_read_extra);
    qtest_add_func("sgi-replay/mgras/invalid-read-mask",
                   test_invalid_read_mask);
    qtest_add_func("sgi-replay/mgras/invalid-read-mismatch",
                   test_invalid_read_mismatch);
    qtest_add_func("sgi-replay/mgras/invalid-masked-mismatch",
                   test_invalid_masked_mismatch);
    qtest_add_func("sgi-replay/mgras/invalid-overflow", test_invalid_overflow);
    qtest_add_func("sgi-replay/mgras/invalid-wid-bounds",
                   test_invalid_wid_bounds);
    qtest_add_func("sgi-replay/mgras/invalid-mismatch", test_invalid_mismatch);
    qtest_add_func("sgi-replay/mgras/invalid-dma-extra",
                   test_invalid_dma_extra);
    qtest_add_func("sgi-replay/mgras/invalid-dma-hex-mismatch",
                   test_invalid_dma_hex_mismatch);
    qtest_add_func("sgi-replay/kona/trace-ordering", test_kona_trace_ordering);
    qtest_add_func("sgi-replay/kona/maintenance-menu",
                   test_kona_maintenance_menu);
    qtest_add_func("sgi-replay/screendump-record", test_screendump_record);
    qtest_add_func("sgi-replay/screendump-record-mismatch",
                   test_screendump_record_mismatch);
    qtest_add_func("sgi-replay/header/no-version", test_invalid_no_version);
    qtest_add_func("sgi-replay/header/version", test_invalid_version);
    qtest_add_func("sgi-replay/header/no-head", test_invalid_no_head);
    qtest_add_func("sgi-replay/header/unknown-head",
                   test_invalid_unknown_head);
    qtest_add_func("sgi-replay/header/late-head", test_invalid_late_head);
    return g_test_run();
}
