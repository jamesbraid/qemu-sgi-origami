/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The catalogue's processor part, applied to an SGI machine's CPUs as
 * -cpu properties the user may override.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/main-loop.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/mips/sgi/sgi-cpu.h"
#include "cpu.h"
#include "exec/tb-flush.h"

/* Set @name on @dev to @value unless -cpu or -global already named it. */
static bool sgi_cpu_default(DeviceState *dev, const char *name,
                            uint64_t value, Error **errp)
{
    if (qdev_find_global_prop(OBJECT(dev), name)) {
        return true;
    }
    return object_property_set_uint(OBJECT(dev), name, value, errp);
}

bool sgi_cpu_set_reset_mode(DeviceState *dev, uint32_t mode, Error **errp)
{
    uint64_t requested;

    if (qdev_find_global_prop(OBJECT(dev), "reset-mode")) {
        requested = object_property_get_uint(OBJECT(dev), "reset-mode",
                                             &error_abort);
        if (requested != mode) {
            error_setg(errp, "CPU reset-mode 0x%" PRIx64 " conflicts with "
                       "the board's 0x%x", requested, mode);
            return false;
        }
    }
    return object_property_set_uint(OBJECT(dev), "reset-mode", mode, errp);
}

bool sgi_cpu_set_prid(DeviceState *dev, uint32_t prid, Error **errp)
{
    static const char *const names[] = {
        "r10000-prid", "r14000-prid", "r16000-prid",
    };

    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        if (object_property_find(OBJECT(dev), names[i])) {
            return sgi_cpu_default(dev, names[i], prid, errp);
        }
    }
    return true;
}

/*
 * An R12000 has no default profile, so the catalogue supplies all of it:
 * PRId and FPU identification and the reset-mode word, whose SS and SB
 * fields select the secondary cache. Only the word's bits 24:0 reach
 * Config; an IP27 flash word also carries the cache clock tap above them.
 */
static bool sgi_cpu_apply_r12000(DeviceState *dev,
                                 const SGIModelProcessor *processor,
                                 Error **errp)
{
    uint32_t mode = processor->mode_bits & 0x01ffffff;
    const struct {
        const char *name;
        uint32_t value;
    } fields[] = {
        { "r12000-prid", processor->prid },
        { "r12000-fpu-id", processor->fpu_prid },
    };

    if (!processor->has_prid || !processor->has_fpu_prid ||
        !processor->has_mode_bits) {
        error_setg(errp, "catalogue processor '%s' has no complete R12000 "
                   "profile", processor->id);
        return false;
    }
    for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
        if (!sgi_cpu_default(dev, fields[i].name, fields[i].value, errp)) {
            return false;
        }
    }
    return sgi_cpu_set_reset_mode(dev, mode, errp);
}

bool sgi_cpu_apply_processor(DeviceState *dev,
                             const SGIModelProcessor *processor,
                             Error **errp)
{
    if (object_property_find(OBJECT(dev), "r12000-prid")) {
        return sgi_cpu_apply_r12000(dev, processor, errp);
    }
    return !processor->has_prid ||
           sgi_cpu_set_prid(dev, processor->prid, errp);
}

typedef struct SGICpuRestart {
    bool big;
    uint64_t entry;
    void (*resync)(void *opaque);
    void *opaque;
} SGICpuRestart;

const char *sgi_cpu_byte_order(CPUState *cpu)
{
    return !cpu || extract32(MIPS_CPU(cpu)->env.CP0_Config0, CP0C0_BE, 1) ?
           "big" : "little";
}

bool sgi_cpu_restart_entry_valid(uint64_t entry, Error **errp)
{
    if ((entry & 3) || entry >= 512 * MiB) {
        error_setg(errp, "x-restart-entry must be a word-aligned physical "
                   "address below 512 MiB, or 0 for the reset vector");
        return false;
    }
    return true;
}

/* One exclusive section restarts every CPU and flushes, so none runs mixed. */
static void sgi_cpu_restart_work(CPUState *first, run_on_cpu_data data)
{
    SGICpuRestart *r = data.host_ptr;
    CPUState *cs;

    /* The reset and the machine's lines are the main loop's to touch. */
    bql_lock();
    CPU_FOREACH(cs) {
        MIPSCPU *cpu = MIPS_CPU(cs);

        cpu->is_big_endian = r->big;
        cpu_reset(cs);
        /* A later machine reset comes back big-endian. */
        cpu->is_big_endian = true;
        if (r->entry) {
            cpu_set_pc(cs, (uint64_t)(int32_t)0x80000000 + r->entry);
        }
    }
    if (r->resync) {
        r->resync(r->opaque);
    }
    bql_unlock();
    /* Translated blocks are not keyed on Config0.BE. */
    tb_flush__exclusive_or_serial();
    g_free(r);
}

bool sgi_cpu_restart(CPUState *first, const char *order, uint64_t entry,
                     void (*resync)(void *opaque), void *opaque,
                     Error **errp)
{
    SGICpuRestart *r;
    bool big;

    if (!strcmp(order, "big")) {
        big = true;
    } else if (!strcmp(order, "little")) {
        big = false;
    } else {
        error_setg(errp, "x-cpu-byte-order takes big or little");
        return false;
    }
    if (big == extract32(MIPS_CPU(first)->env.CP0_Config0, CP0C0_BE, 1)) {
        return true;
    }
    r = g_new0(SGICpuRestart, 1);
    r->big = big;
    r->entry = entry;
    r->resync = resync;
    r->opaque = opaque;
    async_safe_run_on_cpu(first, sgi_cpu_restart_work, RUN_ON_CPU_HOST_PTR(r));
    return true;
}
