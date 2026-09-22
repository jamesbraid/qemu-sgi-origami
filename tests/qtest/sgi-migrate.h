/*
 * Shared stopped-VM migration helper for SGI qtest device tests.
 *
 * qtest never executes guest code, so these tests migrate a paused
 * machine and check device state, not a running guest's behavior --
 * the same reasoning sn0-migration-test.c's own docstring gives for
 * standing outside tests/qtest/migration/.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SGI_MIGRATE_H
#define SGI_MIGRATE_H

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

/*
 * Block until @s reports @status, polling query-migrate against a
 * wall-clock deadline. Every duplicate this header replaces polled
 * query-migrate this way; none enabled the "events" migration capability,
 * so waiting on the MIGRATION QMP event instead would block forever --
 * migrate_generate_event() in migration.c only sends it when that
 * capability is on, and turning it on after "migrate" is issued can still
 * race a transfer that completes before the capability change lands.
 * 60 seconds is the widest deadline any duplicate used; most used 30.
 * Fails the test immediately if migration reports "failed" before
 * reaching @status.
 */
static inline void sgi_wait_migration_status(QTestState *s,
                                              const char *status)
{
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(s,
            "{ 'execute': 'query-migrate' }");
        g_autofree char *seen = g_strdup(qdict_get_str(ret, "status"));

        qobject_unref(ret);
        g_test_message("migration: %s", seen);
        g_assert_cmpstr(seen, !=, "failed");
        if (g_str_equal(seen, status)) {
            return;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

/*
 * Migrate @from over @uri and block until it reports the transfer
 * completed. The destination must already be listening for the incoming
 * stream, either via "-incoming <uri>" on its command line or a prior
 * "migrate-incoming" QMP command -- this only drives @from's "migrate"
 * command and its own completion wait, matching every existing caller,
 * none of which keeps a handle to the destination QTestState here.
 */
static inline void sgi_migrate_stopped(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);

    sgi_wait_migration_status(from, "completed");
}

/*
 * Start a source machine with @machine_args, run @prepare on it, migrate
 * it to a fresh destination started with the same @machine_args plus
 * "-incoming", and run @check on the destination. Either callback may be
 * NULL. Both QTestStates are torn down before returning.
 *
 * This is the plain case: one shared command line for both ends, no
 * setup needed on the destination before it starts listening, and
 * nothing @check needs from the source. A caller whose destination needs
 * different arguments, or needs a step before "-incoming defer" starts
 * listening, or whose check needs to read the source too, does not fit
 * this shape and keeps its own test_migration body instead.
 */
static inline void sgi_migrate_roundtrip(const char *machine_args,
                                          void (*prepare)(QTestState *),
                                          void (*check)(QTestState *))
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-migrate-XXXXXX", &error);
    g_autofree char *uri = NULL;
    g_autofree char *to_args = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);

    from = qtest_init(machine_args);
    if (prepare) {
        prepare(from);
    }

    to_args = g_strdup_printf("%s -incoming %s", machine_args, uri);
    to = qtest_init(to_args);

    sgi_migrate_stopped(from, uri);
    /* The source completes when its state is sent, not when it is loaded. */
    sgi_wait_migration_status(to, "completed");

    if (check) {
        check(to);
    }

    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(dir);
}

#endif /* SGI_MIGRATE_H */
