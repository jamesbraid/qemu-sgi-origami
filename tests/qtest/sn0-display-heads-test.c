/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Graphics-head qdev id and QMP screendump-by-device qtest (issue #895).
 *
 * sgi_machine_instantiate_mgras() and sgi_machine_instantiate_kona()
 * (hw/mips/sgi/sn-machine.c) used to parent each graphics board with only
 * object_property_add_child(), leaving dev->id unset. QMP's screendump
 * matches its device= argument against dev->id through
 * qdev_find_recursive() (ui/console.c's qemu_console_lookup_by_device_name()),
 * a lookup entirely separate from the QOM composition tree those add_child()
 * calls populate, so with no id set neither head could ever be named --
 * screendump always fell back to console index 0. The fix gives each board
 * a stable id, its IRIX slot name: io3 for an MGRAS fitted at IO3. This
 * file boots a two-head composition and
 * asserts both ids resolve, that a screendump can target each head by its
 * id, and that naming an unknown device fails cleanly instead of silently
 * falling back to console 0.
 *
 * The "kona" preset's catalogue records belong to the onyx2 topologies,
 * which fit no MGRAS, so no shipped topology carries both heads. This file
 * attaches KONA to an origin2000 the same way tests/qtest/sn0-origin2000-
 * kona-test.c does: a `-device sgi-kona,id=kona0,slot=io4` at an empty XIO
 * slot. That id comes from the CLI's own
 * qdev_set_id() (system/qdev-monitor.c), unaffected by this fix -- it is
 * included here only so the composition is genuinely two-head and the
 * screendump-by-device contract is proven across both heads together. Of
 * the two ids this file checks, only MGRAS's demonstrates the sn-machine.c
 * fix itself: MGRAS has no `-device` of its own to carry an id=, since
 * sgi_machine_instantiate_mgras() runs during machine construction, not in
 * response to a command-line device.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-test.h"

#define MACHINE_ARGS \
    "-M origin2000 -device sgi-mgras,slot=io3 " \
    "-device sgi-kona,id=kona0,slot=io4"

#define MGRAS_MODULE_PATH "/machine/module0"
#define MGRAS_LEAF        "io3"
#define KONA_MODULE_PATH  "/machine/module0"
#define KONA_LEAF         "kona0"

#define MGRAS_DEVICE_ID   "io3"
#define KONA_DEVICE_ID    "kona0"
#define UNKNOWN_DEVICE_ID "no-such-display-head"

static QTestState *qts;

/* Assert a qom-list of parent_path names a child called leaf. */
static void assert_qom_child(const char *parent_path, const char *leaf)
{
    QDict *response = qtest_qmp(qts,
        "{'execute': 'qom-list', 'arguments': {'path': %s}}", parent_path);
    QList *children;
    const QListEntry *entry;
    bool found = false;

    if (!qdict_haskey(response, "return")) {
        g_autoptr(GString) text = qobject_to_json(QOBJECT(response));
        g_error("qom-list %s failed: %s", parent_path, text->str);
    }
    children = qdict_get_qlist(response, "return");
    for (entry = qlist_first(children); entry; entry = qlist_next(entry)) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(child, "name"), leaf)) {
            found = true;
            break;
        }
    }
    qobject_unref(response);
    g_assert_true(found);
}

static void test_ids_resolve_in_qom_tree(void)
{
    assert_qom_child(MGRAS_MODULE_PATH, MGRAS_LEAF);
    assert_qom_child(KONA_MODULE_PATH, KONA_LEAF);
}

/*
 * Take a screendump by device id to a fresh temp file; assert it succeeds
 * and that the file it wrote is a non-empty PPM.
 */
static void assert_screendump_by_device(const char *device_id)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    QDict *response;
    gsize length;
    int fd;

    fd = g_file_open_tmp("sn0-display-heads-XXXXXX.ppm", &path, &error);
    g_assert_no_error(error);
    close(fd);

    response = qtest_qmp(qts,
        "{'execute': 'screendump', "
        "'arguments': {'filename': %s, 'device': %s}}", path, device_id);
    if (!qdict_haskey(response, "return")) {
        g_autoptr(GString) text = qobject_to_json(QOBJECT(response));
        g_error("screendump device=%s failed: %s", device_id, text->str);
    }
    qobject_unref(response);

    g_assert_true(g_file_get_contents(path, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(length, >, 0);
    g_assert_true(g_str_has_prefix(contents, "P6\n"));
    unlink(path);
}

static void test_screendump_targets_mgras(void)
{
    assert_screendump_by_device(MGRAS_DEVICE_ID);
}

static void test_screendump_targets_kona(void)
{
    assert_screendump_by_device(KONA_DEVICE_ID);
}

static void test_screendump_unknown_device_fails_cleanly(void)
{
    g_autofree char *path = NULL;
    g_autoptr(GError) error = NULL;
    QDict *response;
    int fd;

    fd = g_file_open_tmp("sn0-display-heads-XXXXXX.ppm", &path, &error);
    g_assert_no_error(error);
    close(fd);

    response = qtest_qmp(qts,
        "{'execute': 'screendump', "
        "'arguments': {'filename': %s, 'device': %s}}",
        path, UNKNOWN_DEVICE_ID);
    qmp_expect_error_and_unref(response, "DeviceNotFound");
    unlink(path);

    /* The failed lookup left the machine itself alone. */
    assert_qom_child(MGRAS_MODULE_PATH, MGRAS_LEAF);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

    qtest_add_func("sn0/display-heads/ids-resolve-in-qom-tree",
                   test_ids_resolve_in_qom_tree);
    qtest_add_func("sn0/display-heads/screendump-targets-mgras",
                   test_screendump_targets_mgras);
    qtest_add_func("sn0/display-heads/screendump-targets-kona",
                   test_screendump_targets_kona);
    qtest_add_func("sn0/display-heads/screendump-unknown-device-fails-cleanly",
                   test_screendump_unknown_device_fails_cleanly);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
