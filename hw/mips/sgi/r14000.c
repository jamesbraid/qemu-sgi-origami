/* SPDX-License-Identifier: BSD-3-Clause */
/* SGI R14000 revision 2.3 identity under the reviewed R14000 policy. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "cpu-qom.h"
#include "hw/core/resettable.h"

#define TYPE_SGI_R14000_REV23 MIPS_CPU_TYPE_NAME("sgi-r14000-rev23")

typedef struct SGIR14000Class {
    MIPSCPUClass parent_class;
    ResettablePhases parent_reset_phases;
} SGIR14000Class;

DECLARE_CLASS_CHECKERS(SGIR14000Class, SGI_R14000_REV23, TYPE_SGI_R14000_REV23)

static void sgi_r14000_reset_hold(Object *obj, ResetType type)
{
    SGIR14000Class *sc = SGI_R14000_REV23_GET_CLASS(obj);

    if (sc->parent_reset_phases.hold) {
        sc->parent_reset_phases.hold(obj, type);
    }
    MIPS_CPU(obj)->env.CP0_PRid = 0x0f23;
}

static void sgi_r14000_class_init(ObjectClass *oc, const void *data)
{
    SGIR14000Class *sc = SGI_R14000_REV23_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    resettable_class_set_parent_phases(rc, NULL, sgi_r14000_reset_hold, NULL,
                                       &sc->parent_reset_phases);
}

static const TypeInfo sgi_r14000_types[] = {
    {
        .name = TYPE_SGI_R14000_REV23,
        .parent = MIPS_CPU_TYPE_NAME("R14000"),
        .class_size = sizeof(SGIR14000Class),
        .class_init = sgi_r14000_class_init,
    },
};

DEFINE_TYPES(sgi_r14000_types)
