/*
 * QTest for the SGI GPU back end: a plane written through the C interface
 * reads back identical. Skips when the host has no adapter.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

/* The first lavapipe use can take minutes on a loaded host. */
#define TIMEOUT_S 300

static char *get_prop(QTestState *qts, const char *prop)
{
    QDict *r = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments':"
                         "{'path': '/machine/peripheral/g', 'property': %s}}",
                         prop);
    char *v;

    g_assert(qdict_haskey(r, "return"));
    v = g_strdup(qdict_get_str(r, "return"));
    qobject_unref(r);
    return v;
}

/* Polls @prop until it is not @pending; returns the final value. */
static char *wait_until_not(QTestState *qts, const char *prop,
                            const char *pending)
{
    char *v = get_prop(qts, prop);

    for (int i = 0; i < TIMEOUT_S * 10 && !strcmp(v, pending); i++) {
        g_usleep(100 * 1000);
        g_free(v);
        v = get_prop(qts, prop);
    }
    return v;
}

static void test_plane_roundtrip(void)
{
    QTestState *qts = qtest_init("-M none -S -device x-sgi-gpu-test,id=g");
    char *state = wait_until_not(qts, "state", "opening");
    char *result;
    char *adapter;

    if (!strcmp(state, "unavailable")) {
        char *why = get_prop(qts, "lost");

        g_test_skip(why);
        g_free(why);
        goto out;
    }
    g_assert_cmpstr(state, ==, "ready");

    adapter = get_prop(qts, "adapter");

    g_assert_cmpuint(strlen(adapter), >, 0);
    g_test_message("adapter %s", adapter);
    g_free(adapter);

    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments':"
                             "{'path': '/machine/peripheral/g',"
                             " 'property': 'x-run', 'value': true}}");
    result = wait_until_not(qts, "result", "running");
    g_assert_cmpstr(result, ==, "ok");
    g_free(result);
out:
    g_free(state);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi-gpu/plane-roundtrip", test_plane_roundtrip);
    return g_test_run();
}
