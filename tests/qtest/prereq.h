/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Prerequisites a qtest needs from its host, such as /dev/net/tun or a
 * DRM render node.
 *
 * A developer without one gets a skip. CI sets QEMU_TEST_REQUIRE_<NAME>=1
 * for each prerequisite its image and runners provide, and then a missing
 * one fails the case instead of hiding behind a skip.
 */
#ifndef QTEST_PREREQ_H
#define QTEST_PREREQ_H

#include "qemu/osdep.h"

static inline bool qtest_prereq_required(const char *name)
{
    g_autofree char *var = g_strdup_printf("QEMU_TEST_REQUIRE_%s", name);
    const char *value = g_getenv(var);

    return value && *value && strcmp(value, "0") != 0;
}

/*
 * Report a prerequisite that is absent. Fail the running case if CI
 * requires it, otherwise skip. Call from inside a test function.
 */
static inline void qtest_prereq_missing(const char *name, const char *what)
{
    if (qtest_prereq_required(name)) {
        g_test_message("QEMU_TEST_REQUIRE_%s is set but: %s", name, what);
        g_test_fail();
    } else {
        g_test_skip(what);
    }
}

#endif
