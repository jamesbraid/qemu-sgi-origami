/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The SGI graphics heads' per-thread re-entry guard, without a device:
 * an access enters once and is refused inside another access or a slice
 * of work on the same thread, a slice may nest inside an access, and
 * another thread has its own guard.
 */
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "hw/display/sgi/head-guard.h"

/* An access enters; a second access on the same thread is refused. */
static void test_access_refuses_reentry(void)
{
    g_assert_true(sgi_head_work_enter());
    g_assert_false(sgi_head_work_enter());
    g_assert_false(sgi_head_work_enter());
    sgi_head_work_leave();
    g_assert_true(sgi_head_work_enter());
    sgi_head_work_leave();
}

/* Inside a slice of work, as on a head's own thread, accesses are refused. */
static void test_work_refuses_access(void)
{
    sgi_head_work_enter_work();
    g_assert_false(sgi_head_work_enter());
    sgi_head_work_leave();
    g_assert_true(sgi_head_work_enter());
    sgi_head_work_leave();
}

/*
 * A drain inside an access (a vCPU's access that waits for the work)
 * brackets itself again; leaving it leaves the access still entered.
 */
static void test_work_nests_in_access(void)
{
    g_assert_true(sgi_head_work_enter());
    sgi_head_work_enter_work();
    g_assert_false(sgi_head_work_enter());
    sgi_head_work_leave();
    g_assert_false(sgi_head_work_enter());
    sgi_head_work_leave();
    g_assert_true(sgi_head_work_enter());
    sgi_head_work_leave();
}

static void *other_thread_enters(void *opaque)
{
    bool *entered = opaque;

    *entered = sgi_head_work_enter();
    if (*entered) {
        sgi_head_work_leave();
    }
    return NULL;
}

/* A processor's access on another thread is not this thread's re-entry. */
static void test_threads_are_separate(void)
{
    QemuThread thread;
    bool entered = false;

    g_assert_true(sgi_head_work_enter());
    qemu_thread_create(&thread, "head-guard-test", other_thread_enters,
                       &entered, QEMU_THREAD_JOINABLE);
    qemu_thread_join(&thread);
    g_assert_true(entered);
    sgi_head_work_leave();
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi/head-guard/access-refuses-reentry",
                    test_access_refuses_reentry);
    g_test_add_func("/sgi/head-guard/work-refuses-access",
                    test_work_refuses_access);
    g_test_add_func("/sgi/head-guard/work-nests-in-access",
                    test_work_nests_in_access);
    g_test_add_func("/sgi/head-guard/threads-are-separate",
                    test_threads_are_separate);
    return g_test_run();
}
