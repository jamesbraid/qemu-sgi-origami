/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT */
/*
 * Host tests link the real coordinator, adapters, endpoints and bus. Stubs
 * supply the unused address space, monitor, network, block, audio and
 * processor services. Timer vmstate below is real: it copies the MIT
 * implementation from migration/savevm.c; its notice follows.
 *
 *   QEMU System Emulator
 *
 *   Copyright (c) 2003-2008 Fabrice Bellard
 *   Copyright (c) 2009-2015 Red Hat Inc
 *   Authors: Juan Quintela <quintela@redhat.com>
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "migration/vmstate.h"
#include "monitor/hmp.h"
#include "monitor/monitor.h"
#include "net/net.h"
#include "qemu/audio.h"
#include "qemu/timer.h"
#include "../migration/qemu-file.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/memory.h"
#include "system/runstate.h"

/*
 * An adapter's register window. Nothing dispatches through it: the tests
 * reach registers with pcf8584_read_register()/pcf8584_write_register(), and
 * the coordinator's outputs are named qdev GPIOs rather than sysbus IRQ
 * lines. The region stays the zeroed field it already is inside the device
 * state, which sysbus_init_mmio() then records unread.
 */
void memory_region_init_io(MemoryRegion *mr, Object *owner,
                           const MemoryRegionOps *ops, void *opaque,
                           const char *name, uint64_t size)
{
}

uint64_t memory_region_size(const MemoryRegion *mr)
{
    return 0;
}

/*
 * Mapping a region is the one thing that would quietly do nothing here, so it
 * fails loudly instead. A test that starts driving an adapter through its
 * window needs a real address space, not this file.
 */
MemoryRegion *get_system_memory(void)
{
    g_assert_not_reached();
}

void memory_region_add_subregion(MemoryRegion *mr, hwaddr offset,
                                 MemoryRegion *subregion)
{
    g_assert_not_reached();
}

void memory_region_add_subregion_overlap(MemoryRegion *mr, hwaddr offset,
                                         MemoryRegion *subregion,
                                         int priority)
{
    g_assert_not_reached();
}

void memory_region_del_subregion(MemoryRegion *mr, MemoryRegion *subregion)
{
    g_assert_not_reached();
}

/* Reached only by "info qtree", which no test binary has. */
int monitor_hmp_printf(MonitorHMP *mon, const char *fmt, ...)
{
    return 0;
}

/*
 * The endpoint registers "info elsc" from its type initialiser, so this one
 * is called once before any test runs, with nothing to record it in.
 */
void monitor_register_hmp_info_hrt(const char *name,
                                   HumanReadableText *(*handler)(Error **errp))
{
}

/*
 * The processor an endpoint attributes a response to. NULL is the documented
 * "no processor" case and is the case both tests exercise, so this is the
 * real definition rather than a stand-in.
 */
__thread CPUState *current_cpu;

/*
 * An endpoint asks the machine to reset or power down only when no module
 * owner is installed to take the request instead. The coordinator installs
 * itself as that owner over every endpoint it creates, and both tests build
 * their endpoints through the coordinator, so neither of these is reached.
 */
void qemu_system_reset_request(ShutdownCause reason)
{
}

void qemu_system_shutdown_request(ShutdownCause reason)
{
}

/*
 * Queues a committed action on the processor driving it, reached only when
 * current_cpu is set. Both tests exercise only the "no processor" case
 * above, so this is never called; it exists to link hw/misc/sgi-elsc.c,
 * which takes its address to hand to async_run_on_cpu().
 */
void cpu_pause(CPUState *cpu)
{
    g_assert_not_reached();
}

/*
 * Backends for qdev property kinds no device in this link declares: no
 * audiodev, no netdev, no drive. They exist because
 * hw/core/qdev-properties-system.c defines all of its property kinds in one
 * object file, not because anything here can call them.
 */
AudioBackend *audio_be_by_name(const char *name, Error **errp)
{
    g_assert_not_reached();
}

const char *audio_be_get_id(AudioBackend *be)
{
    g_assert_not_reached();
}

int qemu_find_net_clients_except(const char *id, NetClientState **ncs,
                                 NetClientDriver type, int max)
{
    return 0;
}

void blockdev_auto_del(BlockBackend *blk)
{
    g_assert_not_reached();
}

/*
 * Endpoint migration needs the real arbitration timer vmstate. Host tests
 * cannot link migration/savevm.c, where timer_put()/timer_get() implement
 * the qemu/timer.h declarations. Replicate its stream format with linked
 * timer_expire_time_ns(), timer_mod_ns(), timer_del() (util/qemu-timer.c),
 * qemu_put_be64() and qemu_get_be64() (migration/qemu-file.c): one big-endian
 * expiry, all-ones when disarmed.
 */
void timer_put(QEMUFile *f, QEMUTimer *ts)
{
    uint64_t expire_time = timer_expire_time_ns(ts);

    qemu_put_be64(f, expire_time);
}

void timer_get(QEMUFile *f, QEMUTimer *ts)
{
    uint64_t expire_time = qemu_get_be64(f);

    if (expire_time != -1) {
        timer_mod_ns(ts, expire_time);
    } else {
        timer_del(ts);
    }
}

static bool load_timer(QEMUFile *f, void *pv, size_t size,
                       const VMStateField *field, Error **errp)
{
    timer_get(f, pv);
    return true;
}

static bool save_timer(QEMUFile *f, void *pv, size_t size,
                       const VMStateField *field, JSONWriter *vmdesc,
                       Error **errp)
{
    timer_put(f, pv);
    return true;
}

const VMStateInfo vmstate_info_timer = {
    .name = "timer",
    .load = load_timer,
    .save = save_timer,
};
