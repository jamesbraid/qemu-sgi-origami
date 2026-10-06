/*
 * Migrate paused SGI qtest machines and check device state; qtest runs no
 * guest code. See sn0-migration-test.c for the same migration-test scope.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SGI_MIGRATE_H
#define SGI_MIGRATE_H

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

/*
 * The shortest host-time bound any wait below uses, as upstream's migration
 * helpers do with MIGRATION_STATUS_WAIT_TIMEOUT. A migration's two QEMUs run
 * CPU-bound on a shared host; at heavy load a source waiting on a slow
 * destination overran the 30 s its callers asked for while still moving.
 */
#define SGI_MIGRATION_WAIT_MIN_S 120

/* How often a wait still in progress logs the state it is waiting in. */
#define SGI_MIGRATION_WAIT_LOG_S 15

/*
 * Block until @s reports @status, polling query-migrate against a wall-clock
 * deadline of SGI_MIGRATION_WAIT_MIN_S seconds. Every duplicate this header
 * replaces polled query-migrate this way; none enabled the "events" migration
 * capability, so waiting on the MIGRATION QMP event instead would block
 * forever. migrate_generate_event() in migration.c only sends it when that
 * capability is on, and turning it on after "migrate" is issued can still
 * race a transfer that completes before the capability change lands. Fails
 * the test immediately if migration reports "failed" before reaching
 * @status. Logs the state it is waiting in every SGI_MIGRATION_WAIT_LOG_S
 * seconds and when the wait is about to fail; at a 1 ms poll interval,
 * logging every poll drowned the test log without telling a passing run
 * anything new. The periodic line matters where meson's own timeout for the
 * suite is shorter than this wait: a hang it kills still leaves its last
 * migration state in the test log.
 */
static inline void sgi_wait_migration(QTestState *s, const char *status)
{
    int64_t deadline = g_get_monotonic_time() +
                       SGI_MIGRATION_WAIT_MIN_S * G_TIME_SPAN_SECOND;
    int64_t next_log = g_get_monotonic_time() +
                       SGI_MIGRATION_WAIT_LOG_S * G_TIME_SPAN_SECOND;

    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(s,
            "{ 'execute': 'query-migrate' }");
        g_autofree char *seen = g_strdup(qdict_get_str(ret, "status"));
        int64_t now = g_get_monotonic_time();

        qobject_unref(ret);
        if (g_str_equal(seen, status)) {
            return;
        }
        if (g_str_equal(seen, "failed") || now >= deadline) {
            g_test_message("migration: %s", seen);
        } else if (now >= next_log) {
            g_test_message("migration: still %s, waiting for %s", seen,
                           status);
            next_log = now + SGI_MIGRATION_WAIT_LOG_S * G_TIME_SPAN_SECOND;
        }
        g_assert_cmpstr(seen, !=, "failed");
        g_assert_cmpint(now, <, deadline);
        g_usleep(1000);
    }
}

/*
 * The old spelling, whose deadline argument stopped mattering once every
 * wait took SGI_MIGRATION_WAIT_MIN_S. sn-builder-test.c still calls it;
 * remove it when that file calls sgi_wait_migration() instead.
 */
static inline void sgi_wait_migration_status(QTestState *s,
                                              const char *status,
                                              int deadline_s)
{
    sgi_wait_migration(s, status);
}

/*
 * Migrate @from over @uri and block until it reports the transfer completed.
 * The destination must already be listening for the incoming stream, either
 * via "-incoming <uri>" on its command line or a prior "migrate-incoming"
 * QMP command. This only drives @from's "migrate" command and its own
 * completion wait, matching every existing caller, none of which keeps a
 * handle to the destination QTestState here.
 */
static inline void sgi_migrate_stopped(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);

    sgi_wait_migration(from, "completed");
}

/*
 * Start the destination @to once it has loaded the incoming stream, and
 * check that it runs. The source reporting "completed" only means it has
 * sent the stream. A "cont" that reaches the destination while it is still
 * loading just asks for an autostart, and when the source was stopped the
 * load then restores its paused runstate, so the guest never runs. Upstream's
 * migration test framework waits on the destination for the same reason.
 */
static inline void sgi_migrate_resume(QTestState *to)
{
    QDict *status;

    sgi_wait_migration(to, "completed");
    qtest_qmp_assert_success(to, "{ 'execute': 'cont' }");
    status = qtest_qmp_assert_success_ref(to, "{ 'execute': 'query-status' }");
    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "running");
    qobject_unref(status);
}

#endif /* SGI_MIGRATE_H */
