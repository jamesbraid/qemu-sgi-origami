/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"

static void test_owner(const void *opaque)
{
    bool cycle = GPOINTER_TO_INT(opaque);
    QTestState *qts = qtest_initf("-M origin2000-xtalk-owner-test%s "
                                  "-m 256M",
                                  cycle ? ",cycle=on" : "");
    g_assert_true(qtest_qom_get_bool(qts, "/machine", "verified"));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("sn0/origin2000/xtalk-owner/prepare",
                        GINT_TO_POINTER(false), test_owner);
    qtest_add_data_func("sn0/origin2000/xtalk-owner/destroy-reprepare",
                        GINT_TO_POINTER(true), test_owner);
    return g_test_run();
}
