/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Selected Origin 300 L1 software status builder; no USB transfer. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

#define L1 "/machine/node0/l1"
#define DEFAULT_STATE "config=1,ep0=2,out1=1,in2=2"
#define DEVICE_HEX "12010001ffff00405e063412000101020301"
#define TREE_HEX "09022000010104600009040000020000ff05" \
                 "0705010240000007058202400000"

static char *test_dir(void)
{
    g_autofree char *cwd = g_get_current_dir();
    g_autofree char *scratch = g_build_filename(cwd, ".cache", NULL);
    char *dir;

    g_assert_cmpint(g_mkdir_with_parents(scratch, 0700), ==, 0);
    dir = g_build_filename(scratch, "sgi-l1-usb-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(dir));
    return dir;
}

static char *empty_prom(const char *dir)
{
    g_autofree uint8_t *bytes = g_malloc0(PROM_SIZE);
    char *path = g_build_filename(dir, "prom.bin", NULL);
    int fd = g_open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, bytes, PROM_SIZE), ==, PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *start(const char *prom, const char *extra)
{
    g_autofree char *quoted = g_shell_quote(prom);

    return qtest_initf("-M origin300 -cpu R14000 -smp 2 -bios %s -S %s",
                       quoted, extra);
}

static QDict *set(QTestState *s, const char *property, const char *value)
{
    return qtest_qmp(s, "{'execute':'qom-set','arguments':"
                       "{'path':%s,'property':%s,'value':%s}}",
                       L1, property, value);
}

static char *get(QTestState *s, const char *property)
{
    g_autoptr(QDict) reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        L1, property);

    g_assert_true(qdict_haskey(reply, "return"));
    return g_strdup(qdict_get_str(reply, "return"));
}

static void expect_set(QTestState *s, const char *property,
                       const char *value, bool success)
{
    g_autoptr(QDict) reply = set(s, property, value);

    g_assert_cmpint(qdict_haskey(reply, "return"), ==, success);
    g_assert_cmpint(qdict_haskey(reply, "error"), !=, success);
}

static void expect_result(QTestState *s, const char *expected)
{
    g_autofree char *actual = get(s, "usb-test-result");

    g_assert_cmpstr(actual, ==, expected);
}

static void expect_state(QTestState *s, const char *expected)
{
    g_autofree char *actual = get(s, "usb-test-cached-state-readback");

    g_assert_cmpstr(actual, ==, expected);
}

static void expect_descriptor(QTestState *s, const char *expected)
{
    g_autofree char *actual = get(s, "usb-test-descriptor-result");

    g_assert_cmpstr(actual, ==, expected);
}

static void test_matrix(void)
{
    static const struct {
        const char *state;
        const char *setup;
        const char *result;
    } cases[] = {
        { DEFAULT_STATE, "8000000000000200", "prepared:0100" },
        { DEFAULT_STATE, "8100000000000200", "prepared:0000" },
        { DEFAULT_STATE, "8200000000000200", "prepared:0000" },
        { "config=1,ep0=1,out1=1,in2=2", "8200000080000200", "prepared:0100" },
        { DEFAULT_STATE, "8200000001000200", "prepared:0100" },
        { "config=0,ep0=2,out1=1,in2=2", "8200000001000200", "prepared:0100" },
        { "config=1,ep0=2,out1=2,in2=2", "8200000001000200", "prepared:0000" },
        { DEFAULT_STATE, "8200000082000200", "prepared:0000" },
        { "config=1,ep0=2,out1=1,in2=1", "8200000082000200", "prepared:0100" },
        { DEFAULT_STATE, "8200000081000200", "none" },
        { DEFAULT_STATE, "8200000002000200", "none" },
        { DEFAULT_STATE, "8200000003000200", "none" },
        { DEFAULT_STATE, "8000000000000000", "prepared:" },
        { DEFAULT_STATE, "8000000000000100", "prepared:01" },
        { DEFAULT_STATE, "8100000000000100", "prepared:00" },
        { DEFAULT_STATE, "8000000000000300", "prepared:0100" },
    };
    g_autofree char *dir = test_dir();
    g_autofree char *prom = empty_prom(dir);
    QTestState *s = start(prom, "");
    g_autoptr(QDict) listing = qtest_qmp(s,
        "{'execute':'qom-list','arguments':{'path':%s}}", L1);
    QList *entries = qdict_get_qlist(listing, "return");
    unsigned found = 0;

    for (const QListEntry *entry = qlist_first(entries); entry;
         entry = qlist_next(entry)) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));
        const char *name = qdict_get_str(item, "name");

        if (g_str_has_prefix(name, "usb-test-")) {
            found++;
        }
    }
    g_assert_cmpuint(found, ==, 6);
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        expect_set(s, "usb-test-cached-state", cases[i].state, true);
        expect_state(s, cases[i].state);
        expect_set(s, "usb-test-setup", cases[i].setup, true);
        expect_result(s, cases[i].result);
    }
    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_descriptor_matrix(void)
{
    static const struct {
        const char *setup;
        const char *result;
    } cases[] = {
        { "8006000100000000", "prepared:" },
        { "8006000100000100", "prepared:12" },
        { "8006000100001200", "prepared:" DEVICE_HEX },
        { "8006000100001300", "prepared:" DEVICE_HEX },
        { "8006000200000000", "prepared:" },
        { "8006000200000100", "prepared:09" },
        { "8006000200000900", "prepared:090220000101046000" },
        { "8006000200000a00", "prepared:09022000010104600009" },
        { "8006000200001200", "prepared:09022000010104600009040000020000ff05" },
        { "8006000200001300", "prepared:090220000101046000"
                              "09040000020000ff0507" },
        { "8006000200001900", "prepared:090220000101046000"
                              "09040000020000ff0507050102400000" },
        { "8006000200001a00", "prepared:090220000101046000"
                              "09040000020000ff050705010240000007" },
        { "8006000200002000", "prepared:" TREE_HEX },
        { "8006000200002100", "prepared:" TREE_HEX },
        { "8006000200000A00", "prepared:09022000010104600009" },
    };
    g_autofree char *dir = test_dir();
    g_autofree char *prom = empty_prom(dir);
    QTestState *s = start(prom, "");

    expect_descriptor(s, "none");
    expect_set(s, "usb-test-cached-state", DEFAULT_STATE, true);
    expect_set(s, "usb-test-setup", "8000000000000200", true);
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        expect_set(s, "usb-test-descriptor-setup", cases[i].setup, true);
        expect_descriptor(s, cases[i].result);
        expect_state(s, DEFAULT_STATE);
        expect_result(s, "prepared:0100");
    }
    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_descriptor_errors_reset(void)
{
    static const char *invalid[] = {
        "80060002000020", "800600020000200000", "80060002000020gg",
        "0006000200002000", "8106000200002000", "8005000200002000",
        "8006000300002000", "8006000900002000",
        "8006010100002000", "8006010200002000",
        "8006000201002000", "8006000200012000",
        "8006000309002000", "8006000300002000",
        "8006000309042000",
    };
    g_autofree char *dir = test_dir();
    g_autofree char *prom = empty_prom(dir);
    QTestState *s = start(prom, "");

    expect_set(s, "usb-test-cached-state", DEFAULT_STATE, true);
    expect_set(s, "usb-test-setup", "8000000000000200", true);
    expect_set(s, "usb-test-descriptor-setup", "8006000200002000", true);
    for (unsigned i = 0; i < G_N_ELEMENTS(invalid); i++) {
        expect_set(s, "usb-test-descriptor-setup", invalid[i], false);
        expect_descriptor(s, "prepared:" TREE_HEX);
        expect_state(s, DEFAULT_STATE);
        expect_result(s, "prepared:0100");
    }
    qtest_system_reset(s);
    expect_descriptor(s, "none");
    expect_state(s, "unset");
    expect_result(s, "none");
    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_errors_reset(void)
{
    static const char *bad_setup[] = {
        "80000000000002", "800000000000020000", "80000000000002gg",
        "8001000000000200", "8000000100000200", "8000000000010200",
        "8200000001010200", "0000000000000200",
    };
    static const char *bad_state[] = {
        "config=2,ep0=2,out1=1,in2=2", "config=1,ep0=0,out1=1,in2=2",
        "config=1,ep0=2,out1=1,in2=3", "config=1, ep0=2,out1=1,in2=2",
        "config=1,ep0=2,out1=1,in2=2,extra=0",
    };
    g_autofree char *dir = test_dir();
    g_autofree char *prom = empty_prom(dir);
    QTestState *s = start(prom, "");

    expect_state(s, "unset");
    expect_result(s, "none");
    expect_set(s, "usb-test-setup", "8000000000000200", false);
    expect_set(s, "usb-test-cached-state", DEFAULT_STATE, true);
    expect_set(s, "usb-test-setup", "8000000000000200", true);
    for (unsigned i = 0; i < G_N_ELEMENTS(bad_setup); i++) {
        expect_set(s, "usb-test-setup", bad_setup[i], false);
        expect_result(s, "prepared:0100");
        expect_state(s, DEFAULT_STATE);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(bad_state); i++) {
        expect_set(s, "usb-test-cached-state", bad_state[i], false);
        expect_result(s, "prepared:0100");
        expect_state(s, DEFAULT_STATE);
    }
    qtest_system_reset(s);
    expect_state(s, "unset");
    expect_result(s, "none");
    expect_set(s, "usb-test-setup", "8000000000000200", false);
    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_migration(void)
{
    g_autofree char *dir = test_dir();
    g_autofree char *prom = empty_prom(dir);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;

    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start(prom, "");
    to = start(prom, "-incoming defer");
    expect_set(from, "usb-test-cached-state", DEFAULT_STATE, true);
    expect_set(from, "usb-test-setup", "8000000000000200", true);
    expect_set(from, "usb-test-descriptor-setup", "8006000200002000", true);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    expect_state(to, "unset");
    expect_result(to, "none");
    expect_descriptor(to, "none");
    expect_set(to, "usb-test-setup", "8000000000000200", false);
    expect_set(to, "usb-test-descriptor-setup", "8006000100001200", true);
    expect_descriptor(to, "prepared:" DEVICE_HEX);
    qtest_quit(from);
    qtest_quit(to);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("l1/usb-software/matrix", test_matrix);
    qtest_add_func("l1/usb-software/descriptor-matrix", test_descriptor_matrix);
    qtest_add_func("l1/usb-software/descriptor-errors-reset",
                   test_descriptor_errors_reset);
    qtest_add_func("l1/usb-software/errors-reset", test_errors_reset);
    qtest_add_func("l1/usb-software/migration", test_migration);
    return g_test_run();
}
