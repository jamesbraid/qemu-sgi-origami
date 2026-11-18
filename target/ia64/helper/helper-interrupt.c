/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/* IA-64 TCG helper ABI adapters for interrupt and timer state. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "accel/tcg/cpu-loop.h"
#include "arch/arch.h"

uint64_t HELPER(itc_read)(CPUIA64State *env)
{
    return ia64_itc_read(env);
}
