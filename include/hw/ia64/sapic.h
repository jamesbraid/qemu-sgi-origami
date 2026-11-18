/* SPDX-License-Identifier: BSD-3-Clause */
/* The IA-64 CPU operations platform code may call. */
#ifndef HW_IA64_SAPIC_H
#define HW_IA64_SAPIC_H

#include "hw/core/cpu.h"

/*
 * A processor's cr.lid bits 31:16 (id and eid), which the platform assigns.
 * Set it before the processor's first reset; reset keeps it.
 */
void ia64_cpu_set_sapic_id(CPUState *cs, uint16_t lid_hi);
uint16_t ia64_cpu_sapic_id(CPUState *cs);

/* Latch an external interrupt vector in a processor's SAPIC IRR. */
void ia64_sapic_set_irq(CPUState *cs, uint8_t vector);

#endif
