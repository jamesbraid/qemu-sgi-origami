/* SPDX-License-Identifier: BSD-3-Clause */
/* SGI R14000 revision 2.3 identity under the reviewed R14000 policy. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "cpu-qom.h"

#define TYPE_SGI_R14000_REV23 MIPS_CPU_TYPE_NAME("sgi-r14000-rev23")

/*
 * The revision is the r14000-prid default, so reset installs it and
 * migration checks it as it does any R14000's.
 */
static void sgi_r14000_instance_init(Object *obj)
{
    MIPS_CPU(obj)->r10k_profile->prid = 0x0f23;
}

static const TypeInfo sgi_r14000_types[] = {
    {
        .name = TYPE_SGI_R14000_REV23,
        .parent = MIPS_CPU_TYPE_NAME("R14000"),
        .instance_init = sgi_r14000_instance_init,
    },
};

DEFINE_TYPES(sgi_r14000_types)
