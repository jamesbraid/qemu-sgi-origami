/*
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/*
 * IA-64 migration state.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "migration/vmstate.h"

/* The CPU's TLB, RSE, ALAT and cache-array state is not in any stream. */
const VMStateDescription vmstate_ia64_cpu = {
    .name = "cpu",
    .unmigratable = 1,
};
