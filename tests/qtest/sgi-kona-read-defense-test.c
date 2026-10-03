/* SPDX-License-Identifier: BSD-3-Clause */
/* Defensive predicates exercised through real KONA MMIO and VMState. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "kona-read-fixture.h"

#define CONTROL UINT64_C(0x10000000)
#define COMMANDS UINT64_C(0x20000000)
#define PAGE UINT64_C(0x30000000)
#define DATA (COMMANDS + 0xc1000c)
#define SLOT(t) (DATA + 0x20 * (t))

static QTestState *start(char **log)
{
    QTestState *s;
    g_autofree char *logging = NULL;

    if (log) {
        int fd;

        *log = g_strdup_printf("%s/kona-read-defense-XXXXXX", g_get_tmp_dir());
        fd = g_mkstemp(*log);
        g_assert_cmpint(fd, >=, 0);
        close(fd);
        logging = g_strdup_printf("-trace enable=sgi_kona_* "
            "-trace enable=vmstate_load_state "
            "-trace enable=vmstate_load_state_fail "
            "-trace enable=vmstate_load_state_success "
            "-trace enable=vmstate_load_field_error "
            "-d guest_errors,unimp -D %s",
            *log);
    }
    s = qtest_initf("-M none -cpu R10000 -S -device "
                    TYPE_KONA_READ_FIXTURE ",id=test %s",
                    logging ? logging : "");
    qtest_sysbus_mmio_map(s, "/machine/peripheral/test", 0, CONTROL);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/test", 1, COMMANDS);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/test", 2, PAGE);
    return s;
}

static uint64_t field(QTestState *s, unsigned reg)
{
    return qtest_readq(s, CONTROL + reg);
}

static void control(QTestState *s, unsigned reg, uint64_t value)
{
    qtest_writeq(s, CONTROL + reg, value);
}

static void command(QTestState *s, uint32_t token, const uint32_t *args,
                      unsigned count)
{
    qtest_writel(s, SLOT(token), args[0]);
    for (unsigned i = 1; i < count; i++) {
        qtest_writel(s, DATA, args[i]);
    }
}

static void gc(QTestState *s)
{
    command(s, 0x129, (uint32_t[]){ 0, 0, 0, 0, 0, 0,
            0x180c, 0x40001, 1, 6, 5, 1, 1 }, 13);
}

static void setup(QTestState *s)
{
    command(s, 0x136, (uint32_t[]){ 2, 1, 0, 8, 6, 2, 0 }, 7);
}

static void scene(QTestState *s)
{
    command(s, 0x1fd, (uint32_t[]){ 0x10000340, 0x9000, 0x10000 }, 3);
    command(s, 0x1f2, (uint32_t[]){ 0x10000320, 0x9000, 0x4000 }, 3);
    command(s, 0x129, (uint32_t[]){ 0, 0, 0, 0, 0xf00fff, 0,
            0xc, 1, 1, 6, 0, 1, 1 }, 13);
    command(s, 0x136, (uint32_t[]){ 0, 0, 1, 8, 6, 2, 0 }, 7);
    command(s, 0x139, (uint32_t[]){ 454, 700 }, 2);
    for (unsigned r = 0; r < 2; r++) {
        qtest_writel(s, DATA, 0x000180ff);
        qtest_writel(s, DATA, 0x357e0000);
    }
    command(s, 0x13a, (uint32_t[]){ 0 }, 1);
    gc(s);
    setup(s);
    control(s, FIXTURE_PREPARE, 0);
    g_assert_cmpuint(field(s, FIXTURE_FLAGS), ==, 3);
}

static void read_pixels(QTestState *s)
{
    command(s, 0x138, (uint32_t[]){ 454, 700 }, 2);
}

static void assert_unchanged(QTestState *s)
{
    uint8_t actual[FIXTURE_PAGE_BYTES], expected[FIXTURE_PAGE_BYTES];

    memset(expected, 0xa5, sizeof(expected));
    memset(expected, 0, 4);
    qtest_memread(s, PAGE, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    g_assert_cmpuint(field(s, FIXTURE_PAYLOAD_BYTES), ==, 0);
    g_assert_cmpuint(field(s, FIXTURE_READY_WRITES), ==, 0);
}

static void assert_payload(QTestState *s, bool ready)
{
    static const uint8_t expected[] = {
        0, 0, 0, 1, 0xa5, 0xa5, 0xa5, 0xa5,
        0, 1, 0x80, 0xff, 0x35, 0x7e, 0, 0,
        0, 1, 0x80, 0xff, 0x35, 0x7e, 0, 0,
    };
    uint8_t actual[sizeof(expected)], header[sizeof(expected)];

    memcpy(header, expected, sizeof(header));
    if (!ready) {
        header[3] = 0;
    }
    qtest_memread(s, PAGE, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), header, sizeof(header));
    g_assert_cmphex(qtest_readl(s, PAGE + sizeof(expected)), ==, 0xa5a5a5a5);
    g_assert_cmpuint(field(s, FIXTURE_PAYLOAD_BYTES), ==, 16);
    g_assert_cmpuint(field(s, FIXTURE_PAYLOAD_WRITES), ==, 4);
    g_assert_cmpuint(field(s, FIXTURE_READY_WRITES), ==, 1);
    g_assert_cmpuint(field(s, FIXTURE_READY_SEQUENCE), ==, 5);
    g_assert_cmpuint(field(s, FIXTURE_LAST_PAYLOAD), ==, 20);
    g_assert_cmphex(field(s, FIXTURE_WIDTHS), ==, 1 << 4);
}

static char *read_log(QTestState *s, const char *path)
{
    char *text = NULL;

    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_test_message("%s", text);
    return text;
}

static void test_bad_source(gconstpointer opaque)
{
    unsigned mutation = GPOINTER_TO_UINT(opaque);
    QTestState *s = start(NULL);

    scene(s);
    control(s, FIXTURE_MUTATE_PIXEL, mutation);
    read_pixels(s);
    assert_unchanged(s);
    control(s, FIXTURE_MUTATE_PIXEL, FIXTURE_PIXEL_VALID);
    read_pixels(s);
    assert_payload(s, true);
    qtest_quit(s);
}

static void test_migration_refused(gconstpointer opaque)
{
    unsigned request = GPOINTER_TO_UINT(opaque);
    g_autofree char *log = NULL, *text = NULL;
    QTestState *s = start(&log);

    scene(s);
    control(s, FIXTURE_MIGRATE, request);
    g_assert_cmpint((int64_t)field(s, FIXTURE_SAVE_RESULT), ==, 0);
    g_assert_cmpint((int64_t)field(s, FIXTURE_RESULT), ==, -EINVAL);
    g_assert_cmpuint(field(s, FIXTURE_ERROR), ==, 1);
    assert_unchanged(s);
    text = read_log(s, log);
    g_assert_nonnull(strstr(text,
        "vmstate_load_state_fail sgi-kona/x-read-state post-load"));
}

static void test_migration_controls(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    g_autofree char *log = NULL, *text = NULL;
    QTestState *s = start(&log);

    scene(s);
    control(s, FIXTURE_MIGRATE, kind | (kind == FIXTURE_VALID_STATE ? 768 : 0));
    g_assert_cmpint((int64_t)field(s, FIXTURE_SAVE_RESULT), ==, 0);
    g_assert_cmpint((int64_t)field(s, FIXTURE_RESULT), ==, 0);
    g_assert_cmpuint(field(s, FIXTURE_ERROR), ==, 0);
    if (kind != FIXTURE_VALID_STATE) {
        g_assert_cmpuint(field(s, FIXTURE_FLAGS), ==, 0);
        read_pixels(s);
        assert_unchanged(s);
        command(s, 0x1f2, (uint32_t[]){ 0x10000320, 0x9000, 0x4000 }, 3);
        read_pixels(s);
        assert_unchanged(s);
        gc(s);
        setup(s);
    }
    read_pixels(s);
    assert_payload(s, true);
    text = read_log(s, log);
    if (kind == FIXTURE_ABSENT_FLAGS) {
        g_assert_null(strstr(text, "vmstate_load_state sgi-kona/x-read-state"));
    } else if (kind == FIXTURE_VALID_STATE) {
        g_assert_nonnull(strstr(text,
            "vmstate_load_state_success sgi-kona/x-read-state"));
    }
}

static void assert_layout_loaded(QTestState *s, bool old)
{
    uint64_t old_size = field(s, FIXTURE_LAYOUT_OLD_SIZE);
    uint64_t new_size = old_size + 1280 * 1024;

    g_assert_cmpint((int64_t)field(s, FIXTURE_SAVE_RESULT), ==, 0);
    g_assert_cmpint((int64_t)field(s, FIXTURE_RESULT), ==, 0);
    g_assert_cmpuint(field(s, FIXTURE_ERROR), ==, 0);
    g_assert_cmpuint(field(s, FIXTURE_LAYOUT_MATCH), ==, 1);
    g_assert_cmpuint(field(s, FIXTURE_LAYOUT_MARKER), ==, 0);
    g_assert_cmpint((int64_t)field(s, FIXTURE_LAYOUT_STREAM_ERROR), ==, 0);
    g_assert_cmpuint(field(s, FIXTURE_LAYOUT_EOF_BYTES), ==, 0);
    g_assert_cmpint((int64_t)field(s, FIXTURE_LAYOUT_EOF_ERROR), ==, -EIO);
    g_assert_cmpuint(field(s, FIXTURE_LAYOUT_SIZE), ==,
                     old ? old_size : new_size);
    g_assert_cmpuint(field(s, FIXTURE_LAYOUT_PLANES), ==, old ? 6 : 7);
    g_assert_cmpuint(field(s, FIXTURE_LAYOUT_SOURCE_SIZE), ==, new_size);
    g_assert_cmpuint(field(s, FIXTURE_LAYOUT_SOURCE_PLANES), ==, 7);
    g_assert_cmpuint(field(s, FIXTURE_LAYOUT_SOURCE_BYTES), ==, new_size);
    g_test_message("%s layout: prefix matches, marker 0, EOF 0 bytes/-EIO, "
                   "size %" PRIu64 ", planes %u, source restored",
                   old ? "older" : "current", old ? old_size : new_size,
                   old ? 6 : 7);
}

static void test_layout(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    bool old = kind != 0;
    g_autofree char *log = NULL, *text = NULL, *mismatch = NULL;
    QTestState *s = start(&log);
    uint64_t old_size, new_size;

    scene(s);
    control(s, FIXTURE_LAYOUT_SAVE, old);
    control(s, FIXTURE_LAYOUT_LOAD, old);
    if ((int64_t)field(s, FIXTURE_RESULT) != 0) {
        int64_t result = field(s, FIXTURE_RESULT);

        text = read_log(s, log);
        g_assert_cmpint(result, ==, 0);
    }
    assert_layout_loaded(s, old);
    if (kind == 2) {
        /* The exact bytes accepted above must refuse an intact new layout. */
        old_size = field(s, FIXTURE_LAYOUT_OLD_SIZE);
        new_size = old_size + 1280 * 1024;
        control(s, FIXTURE_LAYOUT_LOAD, 0);
        g_assert_cmpint((int64_t)field(s, FIXTURE_SAVE_RESULT), ==, 0);
        g_assert_cmpint((int64_t)field(s, FIXTURE_RESULT), ==, -EINVAL);
        g_assert_cmpuint(field(s, FIXTURE_ERROR), ==, 1);
        g_assert_cmpuint(field(s, FIXTURE_LAYOUT_SIZE), ==, new_size);
        g_assert_cmpuint(field(s, FIXTURE_LAYOUT_PLANES), ==, 7);
        mismatch = g_strdup_printf("%" PRIx64 " != %" PRIx64,
                                    new_size, old_size);
        control(s, FIXTURE_LAYOUT_SAVE, 0);
        control(s, FIXTURE_LAYOUT_LOAD, 0);
        assert_layout_loaded(s, false);
    }
    text = read_log(s, log);
    g_assert_nonnull(strstr(text, "vmstate_load_state sgi-board-memory"));
    if (kind == 2) {
        g_assert_nonnull(strstr(text, "Loading VM subsection "
            "'sgi-kona/board-memory' in 'sgi-kona' failed:"));
        g_assert_nonnull(strstr(text, mismatch));
        g_assert_nonnull(strstr(text,
            "vmstate_load_field_error field \"size\" load failed, ret = -22"));
    } else {
        g_assert_null(strstr(text, "vmstate_load_field_error"));
    }
}

static void test_ready_failure(void)
{
    g_autofree char *log = NULL, *text = NULL;
    QTestState *s = start(&log);

    scene(s);
    control(s, FIXTURE_READY_FAULT, 1);
    read_pixels(s);
    assert_payload(s, false);
    g_assert_cmpuint(field(s, FIXTURE_FAILED_READY), ==, 1);
    text = read_log(s, log);
    g_assert_nonnull(strstr(text,
        "4-byte write to 0x9000000320000 failed (result 1)"));
    g_assert_nonnull(strstr(text, "addr 0x320008 len 0x10"));
    g_assert_null(strstr(text, "sgi_kona_return_post"));

    s = start(NULL);
    scene(s);
    read_pixels(s);
    assert_payload(s, true);
    g_assert_cmpuint(field(s, FIXTURE_FAILED_READY), ==, 0);
    qtest_quit(s);
}

/* These inputs call the helper directly; framed packets reject them earlier. */
static void test_dma_bounds(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    g_autofree char *log = NULL;
    g_autofree char *text = NULL;
    QTestState *s = start(&log);
    uint64_t io = UINT64_MAX - 3;
    uint64_t pitch = 8;
    uint32_t lines = 1, bytes = 4;
    bool success = false;

    if (kind == 0) {
        lines = 0;
        success = true;
    } else if (kind == 1) {
        bytes = 0;
        success = true;
    } else if (kind == 2) {
        bytes = 3;
    } else if (kind == 3) {
        io = 0;
        pitch = UINT64_MAX;
        lines = 3;
    }
    control(s, FIXTURE_DMA_IO, io);
    control(s, FIXTURE_DMA_PITCH, pitch);
    control(s, FIXTURE_DMA_RUN, ((uint64_t)lines << 32) | bytes);
    g_assert_cmpuint(field(s, FIXTURE_DMA_RESULT), ==, success);
    g_assert_cmpuint(field(s, FIXTURE_DMA_READS), ==, 0);
    g_assert_cmpuint(field(s, FIXTURE_WRITE_REFUSED), ==, !success);
    text = read_log(s, log);
    g_assert_null(strstr(text, "maps nothing"));
    if (!success) {
        g_assert_nonnull(strstr(text, kind == 2 ? "line is not whole words" :
                               "device address wraps"));
    }
}

static void test_write_state_validation(gconstpointer opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    QTestState *s = start(NULL);

    command(s, 0x129, (uint32_t[]){ 0, 0, 0, 0, 0xf00fff, 0,
            0xc, 1, 1, 6, 0, 1, 1 }, 13);
    command(s, 0x136, (uint32_t[]){ 0, 0, 1, 8, 8, 1, 0 }, 7);
    command(s, 0x139, (uint32_t[]){ 70, 80 }, 2);
    qtest_writel(s, DATA, 0x11223344);
    control(s, FIXTURE_WRITE_MIGRATE, kind);
    g_assert_cmpint((int64_t)field(s, FIXTURE_SAVE_RESULT), ==, 0);
    g_assert_cmpint((int64_t)field(s, FIXTURE_RESULT), ==,
                   kind ? -EINVAL : 0);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    static const char *const ownership[] = {
        "no-current", "invalid-current", "invalid-context", "stored-page",
        "active-page", "page-mismatch",
    };

    g_test_init(&argc, &argv, NULL);
    for (unsigned kind = 0; kind < 5; kind++) {
        static const char *const names[] = {
            "zero-lines", "zero-bytes", "unaligned", "pitch-wrap", "end-wrap",
        };
        g_autofree char *path = g_strdup_printf("kona-write-defense/dma/%s",
                                                names[kind]);

        qtest_add_data_func(path, GUINT_TO_POINTER(kind), test_dma_bounds);
    }
    for (unsigned kind = 0; kind < 8; kind++) {
        static const char *const names[] = {
            "valid", "packing", "stride", "count", "remaining",
            "missing-validity", "unarrived-validity", "source-width",
        };
        g_autofree char *path = g_strdup_printf(
            "kona-write-defense/migration/%s", names[kind]);

        qtest_add_data_func(path, GUINT_TO_POINTER(kind),
                            test_write_state_validation);
    }

    qtest_add_data_func("kona-read-defense/unknown-tag",
        GUINT_TO_POINTER(FIXTURE_PIXEL_UNKNOWN_TAG), test_bad_source);
    qtest_add_data_func("kona-read-defense/large-index",
        GUINT_TO_POINTER(FIXTURE_PIXEL_LARGE_INDEX), test_bad_source);
    for (unsigned flag = 1; flag <= 2; flag++) {
        for (unsigned kind = 0; kind < ARRAY_SIZE(ownership); kind++) {
            g_autofree char *path = g_strdup_printf(
                "kona-read-defense/migration/%s/%s", flag == 1 ? "gc" : "setup",
                ownership[kind]);

            qtest_add_data_func(path, GUINT_TO_POINTER(kind | (flag << 8)),
                                test_migration_refused);
        }
    }
    qtest_add_data_func("kona-read-defense/migration/false-mismatch",
        GUINT_TO_POINTER(FIXTURE_FALSE_MISMATCH), test_migration_controls);
    qtest_add_data_func("kona-read-defense/migration/absent-subsection",
        GUINT_TO_POINTER(FIXTURE_ABSENT_FLAGS), test_migration_controls);
    qtest_add_data_func("kona-read-defense/migration/valid",
        GUINT_TO_POINTER(FIXTURE_VALID_STATE), test_migration_controls);
    qtest_add_data_func("kona-read-defense/layout/current", NULL, test_layout);
    qtest_add_data_func("kona-read-defense/layout/older-control",
        GUINT_TO_POINTER(1), test_layout);
    qtest_add_data_func("kona-read-defense/layout/older-refused",
        GUINT_TO_POINTER(2), test_layout);
    qtest_add_func("kona-read-defense/ready-failure", test_ready_failure);
    return g_test_run();
}
