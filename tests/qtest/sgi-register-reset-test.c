/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * Reset values the SGI manual pages state, read back from the model.
 *
 * A page marks a register table with an ".. sgi-registers:" comment
 * (docs/devel/sgi-manual-pages.rst). scripts/ci/sgi-register-tables.py
 * reads those tables and prints each as JSON: the arguments to start QEMU
 * with, and for every register its address, access width and documented
 * reset value. This test starts QEMU as asked, reads each register and
 * fails with the page and line of the row when the model reads another
 * value. The values come from the pages and nothing in the model's own
 * sources, so a page and the model that drift apart fail here.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-test.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"

#define EXTRACTOR "scripts/ci/sgi-register-tables.py"

static uint64_t hex_of(const QDict *d, const char *key)
{
    const char *text = qdict_get_str(d, key);
    char *end;
    uint64_t value = g_ascii_strtoull(text, &end, 16);

    g_assert_true(end != text && *end == '\0');
    return value;
}

static uint64_t read_at(QTestState *qts, uint64_t address, int width)
{
    switch (width) {
    case 8:
        return qtest_readb(qts, address);
    case 16:
        return qtest_readw(qts, address);
    case 32:
        return qtest_readl(qts, address);
    case 64:
        return qtest_readq(qts, address);
    }
    g_assert_not_reached();
}

static void write_at(QTestState *qts, uint64_t address, int width,
                     uint64_t value)
{
    switch (width) {
    case 8:
        qtest_writeb(qts, address, value);
        return;
    case 16:
        qtest_writew(qts, address, value);
        return;
    case 32:
        qtest_writel(qts, address, value);
        return;
    case 64:
        qtest_writeq(qts, address, value);
        return;
    }
    g_assert_not_reached();
}

static void write_steps(QTestState *qts, const QList *steps)
{
    const QListEntry *entry;

    QLIST_FOREACH_ENTRY(steps, entry) {
        const QDict *step = qobject_to(QDict, qlist_entry_obj(entry));

        write_at(qts, hex_of(step, "address"), qdict_get_int(step, "width"),
                 hex_of(step, "value"));
    }
}

/*
 * A board that refuses to build without a boot PROM, and runs nothing from
 * it, takes an erased image of the size a real one has.
 */
static char *blank_prom(void)
{
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    g_autoptr(GError) error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("sgi-register-prom-XXXXXX.bin", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void check_table(gconstpointer data)
{
    const QDict *table = data;
    const QList *maps = qdict_get_qlist(table, "maps");
    const QList *registers = qdict_get_qlist(table, "registers");
    const QList *skipped = qdict_get_qlist(table, "skipped");
    const QListEntry *entry;
    const char *page = qdict_get_str(table, "page");
    g_autofree char *prom = NULL;
    QTestState *qts;
    unsigned wrong = 0;

    if (qdict_get_try_str(table, "bios")) {
        prom = blank_prom();
        qts = qtest_initf("%s -bios %s", qdict_get_str(table, "qemu"), prom);
    } else {
        qts = qtest_init(qdict_get_str(table, "qemu"));
    }

    QLIST_FOREACH_ENTRY(maps, entry) {
        const QDict *map = qobject_to(QDict, qlist_entry_obj(entry));

        qtest_sysbus_mmio_map(qts, qdict_get_str(map, "path"),
                              qdict_get_int(map, "index"),
                              hex_of(map, "address"));
    }

    /* The writes a guest makes before the device answers. */
    write_steps(qts, qdict_get_qlist(table, "setup"));

    QLIST_FOREACH_ENTRY(registers, entry) {
        const QDict *reg = qobject_to(QDict, qlist_entry_obj(entry));
        int width = qdict_get_int(reg, "width");
        uint64_t want = hex_of(reg, "expect");
        uint64_t got;
        const char *note = qdict_get_try_str(reg, "note");

        /* A register behind a selector is chosen just before it is read. */
        if (qdict_haskey(reg, "select")) {
            write_steps(qts, qdict_get_qlist(reg, "select"));
        }
        if (qdict_haskey(reg, "split")) {
            /* A little-endian 64-bit window: low half first, then high. */
            g_assert_cmpint(qdict_get_int(reg, "split"), ==, 32);
            got = read_at(qts, hex_of(reg, "address"), 32);
            got |= read_at(qts, hex_of(reg, "address") + 4, 32) << 32;
        } else {
            got = read_at(qts, hex_of(reg, "address"), width);
        }
        if (note) {
            g_test_message("%s:%" PRId64 ": %s at %s: %s", page,
                           qdict_get_int(reg, "line"),
                           qdict_get_str(reg, "name"),
                           qdict_get_str(reg, "offset"), note);
        }

        if (got != want) {
            fprintf(stderr, "%s:%" PRId64 ": %s at %s: page says 0x%" PRIx64
                    ", model reads 0x%" PRIx64 " (%d-bit)\n", page,
                    qdict_get_int(reg, "line"), qdict_get_str(reg, "name"),
                    qdict_get_str(reg, "offset"), want, got, width);
            wrong++;
        }
    }
    /* Say what was left out or pinned rather than pass silently. */
    QLIST_FOREACH_ENTRY(skipped, entry) {
        const QDict *skip = qobject_to(QDict, qlist_entry_obj(entry));

        g_test_message("%s:%" PRId64 ": %s at %s skipped: %s", page,
                       qdict_get_int(skip, "line"),
                       qdict_get_str(skip, "name"),
                       qdict_get_str(skip, "offset"),
                       qdict_get_str(skip, "reason"));
    }
    g_test_message("%s: %zu registers read, %zu skipped",
                   qdict_get_str(table, "id"), (size_t)qlist_size(registers),
                   (size_t)qlist_size(skipped));
    qtest_quit(qts);
    if (prom) {
        unlink(prom);
    }
    if (wrong) {
        g_test_fail();
    }
}

static QList *load_tables(void)
{
    const char *python = g_getenv("PYTHON");
    const char *srcdir = g_getenv("G_TEST_SRCDIR");
    g_autofree char *script = NULL;
    g_autofree char *out = NULL;
    g_autoptr(GError) error = NULL;
    int status;
    QObject *parsed;
    char *argv[5];

    g_assert_nonnull(python);
    g_assert_nonnull(srcdir);
    script = g_build_filename(srcdir, "..", "..", EXTRACTOR, NULL);
    argv[0] = (char *)python;
    argv[1] = (char *)"-B";
    argv[2] = script;
    argv[3] = (char *)"--json";
    argv[4] = NULL;
    g_assert_true(g_spawn_sync(NULL, argv, NULL, 0, NULL, NULL, &out, NULL,
                               &status, &error));
    g_assert_no_error(error);
    g_spawn_check_exit_status(status, &error);
    g_assert_no_error(error);
    parsed = qobject_from_json(out, &error_abort);
    return qobject_to(QList, parsed);
}

int main(int argc, char **argv)
{
    QList *tables;
    const QListEntry *entry;

    g_test_init(&argc, &argv, NULL);
    tables = load_tables();
    /* The extractor fails when it drops a marker; none at all is no test. */
    g_assert_cmpuint(qlist_size(tables), >, 0);

    QLIST_FOREACH_ENTRY(tables, entry) {
        QDict *table = qobject_to(QDict, qlist_entry_obj(entry));
        const char *variant = qdict_get_try_str(table, "variant");
        const char *label = qdict_get_try_str(table, "label");
        g_autofree char *path = g_strdup_printf(
            "/sgi/register-reset/%s@%" PRId64 "%s%s%s%s",
            qdict_get_str(table, "page"), qdict_get_int(table, "line"),
            variant ? "/" : "", variant ? variant : "",
            label ? "/" : "", label ? label : "");

        g_strdelimit(path, " ", '-');
        qtest_add_data_func(path, table, check_table);
    }
    return g_test_run();
}
