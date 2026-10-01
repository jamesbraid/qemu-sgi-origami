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
 * Block until @s reports @status, polling query-migrate against a wall-clock
 * deadline of @deadline_s seconds. Every duplicate this header replaces
 * polled query-migrate this way; none enabled the "events" migration
 * capability, so waiting on the MIGRATION QMP event instead would block
 * forever. migrate_generate_event() in migration.c only sends it when that
 * capability is on, and turning it on after "migrate" is issued can still
 * race a transfer that completes before the capability change lands. Fails
 * the test immediately if migration reports "failed" before reaching
 * @status. Only logs the states it polled through when the wait is about to
 * fail; at a 1 ms poll interval, logging every poll drowned the test log
 * without telling a passing run anything new.
 */
static inline void sgi_wait_migration_status(QTestState *s,
                                              const char *status,
                                              int deadline_s)
{
    int64_t deadline = g_get_monotonic_time() +
                        deadline_s * G_TIME_SPAN_SECOND;

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
        }
        g_assert_cmpstr(seen, !=, "failed");
        g_assert_cmpint(now, <, deadline);
        g_usleep(1000);
    }
}

/*
 * Migrate @from over @uri and block until it reports the transfer completed.
 * The destination must already be listening for the incoming stream, either
 * via "-incoming <uri>" on its command line or a prior "migrate-incoming"
 * QMP command. This only drives @from's "migrate" command and its own
 * completion wait, matching every existing caller, none of which keeps a
 * handle to the destination QTestState here. Every existing caller used a 30
 * second deadline (or none at all), so this passes 30 through rather than
 * taking a parameter of its own.
 */
static inline void sgi_migrate_stopped(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);

    sgi_wait_migration_status(from, "completed", 30);
}

#endif /* SGI_MIGRATE_H */
