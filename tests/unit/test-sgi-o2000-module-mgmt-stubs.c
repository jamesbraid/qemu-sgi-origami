/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The running machine that the module-management tests do not have.
 *
 * Both tests link the real coordinator, the real adapters, the real
 * management endpoints and the real system bus into a host binary, so the
 * device behaviour under test is the shipping behaviour.  What is missing
 * around them is the rest of a machine: an address space, a monitor, a
 * network and block layer, an audio backend, and a processor.  Each of those
 * gets a definition below that stands in for it, and none of them sits on a
 * path either test takes.
 *
 * The timer vmstate at the end of this file is the exception, and is a real
 * implementation rather than a stand-in.  Its own comment says why.
 */
#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "migration/vmstate.h"
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
 * An adapter's register window.  Nothing dispatches through it: the tests
 * reach registers with pcf8584_read_register()/pcf8584_write_register(),
 * and the coordinator's outputs are named qdev GPIOs rather than sysbus
 * IRQ lines.  The region stays the zeroed field it already is inside the
 * device state, which sysbus_init_mmio() then records unread.
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
 * Mapping a region is the one thing that would quietly do nothing here, so
 * it fails loudly instead.  A test that starts driving an adapter through
 * its window needs a real address space, not this file.
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
int monitor_printf(Monitor *mon, const char *fmt, ...)
{
    return 0;
}

/*
 * The endpoint registers "info elsc" from its type initialiser, so this one
 * really is called -- once, before any test runs, with nothing to record it
 * in.
 */
void monitor_register_hmp_info_hrt(const char *name,
                                   HumanReadableText *(*handler)(Error **errp))
{
}

/*
 * The processor an endpoint attributes a response to.  NULL is the
 * documented "no processor" case and is the case both tests exercise, so
 * this is the real definition rather than a stand-in.
 */
__thread CPUState *current_cpu;

/*
 * An endpoint asks the machine to reset or power down only when no module
 * owner is installed to take the request instead.  The coordinator installs
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
 * Backends for qdev property kinds no device in this link declares: no
 * audiodev, no netdev, no drive.  They exist because
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
 * Not a stub. The migration test moves each endpoint's own device vmstate,
 * and that names vmstate_info_timer for the arbitration timer, so this has
 * to carry a real expiry across the stream rather than refuse.
 *
 * QEMU keeps the timer's stream format in migration/savevm.c, which a host
 * unit test cannot link, and timer_put()/timer_get() are declared in
 * qemu/timer.h but defined only there. Everything the format needs is
 * already linked here: timer_expire_time_ns(), timer_mod_ns() and
 * timer_del() from util/qemu-timer.c, qemu_put_be64() and qemu_get_be64()
 * from migration/qemu-file.c. So the two are defined below exactly as
 * savevm.c defines them, and the bytes written here are the bytes QEMU
 * writes: one big-endian expiry, with all-ones meaning no timer armed.
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
