/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-misc-mips.h"

void qmp_x_mips_debug_breakpoint(int64_t cpu_index, uint64_t pc,
                                 bool has_asid, uint16_t asid,
                                 bool has_exccode, uint8_t exccode,
                                 bool has_remove, bool remove, Error **errp)
{
    error_setg(errp, "MIPS debug breakpoints are not supported on this target");
}

MipsDebugBreakpointList *qmp_x_query_mips_debug_breakpoints(Error **errp)
{
    error_setg(errp, "MIPS debug breakpoints are not supported on this target");
    return NULL;
}

MipsTlbState *qmp_x_query_mips_tlb(int64_t cpu_index, bool has_shadow,
                                   bool shadow, Error **errp)
{
    error_setg(errp, "MIPS TLB queries are not supported on this target");
    return NULL;
}
