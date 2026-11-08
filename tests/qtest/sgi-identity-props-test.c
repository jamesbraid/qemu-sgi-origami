/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * No identity property of any SGI machine type may abort QEMU. For each
 * machine type that has the emu-id property it sets the identity options to
 * boundary values on -M before the build; for the test fixtures, which build
 * with no boot PROM, it also reads every identity property and sets each to
 * boundary values after the build. sgi-synthetic-serial-test, which builds
 * the real machines with stand-in boot images, does the same after the
 * build. QEMU must never die from a signal and must still answer. A refusal is fine; an
 * assertion is not. The generic qom-test walks and sets every property of
 * every machine and once aborted on an unresolved id; this keeps the case
 * covered by an SGI test that names it.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

static const char * const properties[] = {
    "emu-id", "serial", "hostid", "hostids", "mac",
};

/* Values for -M, each a complete option. */
static const char * const options[] = {
    "emu-id=4096", "emu-id=-1", "serial=ABCDEFGHIJK", "serial=AB:CD",
    "hostid=abc", "hostid=ffffffff", "mac=00:00:00:00:00:00",
};

/* Values for qom-set after the build, as JSON. */
static const char * const values[][2] = {
    { "emu-id", "0" }, { "emu-id", "4095" }, { "emu-id", "4096" },
    { "emu-id", "-1" }, { "emu-id", "'x'" },
    { "serial", "''" }, { "serial", "'ABCDEFGHIJK'" }, { "serial", "'K'" },
    { "serial", "5" },
    { "hostid", "''" }, { "hostid", "'00000000'" }, { "hostid", "'ffffffff'" },
    { "hostid", "'abc'" }, { "hostid", "'69'" }, { "hostid", "'0123456789'" },
    { "hostids", "'69e10000'" },
    { "mac", "'00:00:00:00:00:00'" }, { "mac", "'ff:ff:ff:ff:ff:ff'" },
    { "mac", "'08:00:69:00:00:01'" },
};

/* The machine types that carry the identity properties. */
static GPtrArray *identity_machines(void)
{
    QTestState *qts = qtest_init("-M none");
    g_autoptr(QDict) reply = qtest_qmp(qts, "{'execute':'query-machines'}");
    QList *machines = qdict_get_qlist(reply, "return");
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const QListEntry *entry;

    QLIST_FOREACH_ENTRY(machines, entry) {
        QDict *machine = qobject_to(QDict, qlist_entry_obj(entry));
        const char *name = qdict_get_str(machine, "name");
        g_autofree char *type = g_strdup_printf("%s-machine", name);
        g_autoptr(QDict) props = qtest_qmp(qts,
            "{'execute':'qom-list-properties','arguments':{'typename':%s}}",
            type);
        const QListEntry *p;

        if (!qdict_haskey(props, "return")) {
            continue;
        }
        QLIST_FOREACH_ENTRY(qdict_get_qlist(props, "return"), p) {
            QDict *prop = qobject_to(QDict, qlist_entry_obj(p));

            if (!strcmp(qdict_get_str(prop, "name"), "emu-id")) {
                g_ptr_array_add(names, g_strdup(name));
                break;
            }
        }
    }
    qtest_quit(qts);
    return names;
}

/*
 * Starts @machine with @option, which may be empty, and checks no signal
 * killed it and it did not report an assertion. A QEMU that built its machine
 * waits for input after the end of its qtest stream, so one still running
 * after a few seconds is stopped and counts as started. True when it started.
 */
static bool launch_survives(const char *machine, const char *option)
{
    g_autofree char *spec = option[0] ?
        g_strdup_printf("%s,%s", machine, option) : g_strdup(machine);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", spec, "-display", "none", "-S",
        "-qtest", "stdio", "-accel", "qtest", NULL,
    };
    GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD |
                        G_SPAWN_STDOUT_TO_DEV_NULL;
    char text[4096] = "";
    GPid pid;
    int err_fd, status = 0, i;
    ssize_t length;
    bool started = false;

    g_assert_true(g_spawn_async_with_pipes(NULL, (char **)args, NULL, flags,
                                           NULL, NULL, &pid, NULL, NULL,
                                           &err_fd, NULL));
    for (i = 0; i < 60; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            break;
        }
        g_usleep(50 * 1000);
    }
    if (i == 60) {
        kill(pid, SIGTERM);
        g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
        started = true;
    }
    length = read(err_fd, text, sizeof(text) - 1);
    text[length > 0 ? length : 0] = 0;
    close(err_fd);
    g_spawn_close_pid(pid);
    if (WIFSIGNALED(status) && !started) {
        g_test_message("%s: %s", spec, text);
    }
    g_assert_false(WIFSIGNALED(status) && !started);
    g_assert_null(strstr(text, "assertion failed"));
    return started;
}

/* Every machine type: the identity options on -M, before the build. */
static void test_machine_options(const void *data)
{
    const char *machine = data;

    for (size_t i = 0; i < G_N_ELEMENTS(options); i++) {
        launch_survives(machine, options[i]);
    }
}

/*
 * The machine types that build with no boot PROM, which are the test
 * fixtures: every identity property read and set after the build. The real
 * machines need a stand-in boot image of their own size, and
 * sgi-synthetic-serial-test, which builds each of them, reads every property
 * and sets the same boundary values after the build.
 */
static void test_machine_built(const void *data)
{
    const char *machine = data;
    g_autofree char *args = g_strdup_printf("-machine %s -S", machine);
    QTestState *qts = qtest_init(args);
    g_autoptr(QDict) status = NULL;

    for (size_t i = 0; i < G_N_ELEMENTS(properties); i++) {
        g_autoptr(QDict) reply = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{'path':'/machine',"
            "'property':%s}}", properties[i]);

        g_assert_true(qdict_haskey(reply, "return") ||
                      qdict_haskey(reply, "error"));
    }
    for (size_t i = 0; i < G_N_ELEMENTS(values); i++) {
        g_autofree char *request = g_strdup_printf(
            "{'execute':'qom-set','arguments':{'path':'/machine',"
            "'property':'%s','value':%s}}", values[i][0], values[i][1]);
        g_autoptr(QDict) reply = qtest_qmp(qts, "%s", request);

        /* The machine is built, so each is refused; none may kill it. */
        g_assert_true(qdict_haskey(reply, "return") ||
                      qdict_haskey(reply, "error"));
    }
    status = qtest_qmp(qts, "{'execute':'query-status'}");
    g_assert_true(qdict_haskey(status, "return"));
    qtest_quit(qts);
}

/*
 * The generic qom-tests command makes every QOM type's instance outside a
 * machine build, reads each property and sets it back to what it read. A
 * machine instance made that way has resolved no identity, and its hostid
 * and hostids getters once reached an assertion on that state. Machine
 * instances must answer with an error, not abort.
 */
static void test_qom_tests(void)
{
    QTestState *qts = qtest_init("-machine none");

    qtest_qom_tests(qts);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    GPtrArray *machines;

    g_test_init(&argc, &argv, NULL);
    machines = identity_machines();
    for (guint i = 0; i < machines->len; i++) {
        const char *name = g_ptr_array_index(machines, i);
        g_autofree char *path = g_strdup_printf("sgi/identity-props/%s", name);
        g_autofree char *built = g_strdup_printf(
            "sgi/identity-props-built/%s", name);

        qtest_add_data_func(path, name, test_machine_options);
        if (g_str_has_suffix(name, "-test")) {
            qtest_add_data_func(built, name, test_machine_built);
        }
    }
    qtest_add_func("sgi/identity-props/qom-tests", test_qom_tests);
    return g_test_run();
}
