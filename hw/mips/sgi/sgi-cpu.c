/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The catalogue's processor part, applied to an SGI machine's CPUs as
 * -cpu properties the user may override.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/mips/sgi/sgi-cpu.h"

/* Set @name on @dev to @value unless -cpu or -global already named it. */
static bool sgi_cpu_default(DeviceState *dev, const char *name,
                            uint64_t value, Error **errp)
{
    if (qdev_find_global_prop(OBJECT(dev), name)) {
        return true;
    }
    return object_property_set_uint(OBJECT(dev), name, value, errp);
}

bool sgi_cpu_set_prid(DeviceState *dev, uint32_t prid, Error **errp)
{
    static const char *const names[] = { "r10000-prid", "r14000-prid" };

    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        if (object_property_find(OBJECT(dev), names[i])) {
            return sgi_cpu_default(dev, names[i], prid, errp);
        }
    }
    return true;
}

/*
 * An R12000 has no default profile, so the catalogue supplies all of it:
 * PRId and FPU identification, the reset-mode word, and the secondary
 * cache that word's SS and SB fields select.
 */
static bool sgi_cpu_apply_r12000(DeviceState *dev,
                                 const SGIModelProcessor *processor,
                                 Error **errp)
{
    uint32_t mode = processor->mode_bits;
    const struct {
        const char *name;
        uint32_t value;
    } fields[] = {
        { "r12000-prid", processor->prid },
        { "r12000-fpu-id", processor->fpu_prid },
        { "r12000-reset-mode", mode },
        { "r12000-scache-bytes", processor->cache_mib * MiB },
        /* Config SB, bit 13, selects 32-word rather than 16-word blocks. */
        { "r12000-scache-block-words", mode & (1u << 13) ? 32 : 16 },
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
    return true;
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
