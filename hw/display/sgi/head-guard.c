/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The per-thread re-entry guard SGI graphics heads share. See the header
 * for the contract.
 */
#include "qemu/osdep.h"
#include "hw/display/sgi/head-guard.h"

/* Register accesses and slices this thread is inside, across all heads. */
static __thread unsigned int sgi_head_work_depth;

bool sgi_head_work_enter(void)
{
    if (sgi_head_work_depth) {
        return false;
    }
    sgi_head_work_depth++;
    return true;
}

void sgi_head_work_enter_work(void)
{
    sgi_head_work_depth++;
}

void sgi_head_work_leave(void)
{
    g_assert(sgi_head_work_depth);
    sgi_head_work_depth--;
}
