/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/* IA-64 debugger and state-dump integration. */

#ifndef TARGET_IA64_DEBUG_H
#define TARGET_IA64_DEBUG_H

#include "cpu.h"

#define IA64_GDB_NUM_CORE_REGS 462
#define IA64_STATE_SCHEMA_VERSION 1

void ia64_cpu_dump_state(CPUState *cs, FILE *f, int flags);
int ia64_cpu_gdb_read_register(CPUState *cs, GByteArray *buf, int reg);
int ia64_cpu_gdb_write_register(CPUState *cs, uint8_t *buf, int reg);

#endif /* TARGET_IA64_DEBUG_H */
