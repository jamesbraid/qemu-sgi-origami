/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 ("SN1") CPU population-to-Bedrock-slot topology. A pure lookup
 * on the format-2 descriptor's Bedrock node record (sn-model.h), with no
 * device state, so the node realizer and its own regression test share one
 * implementation.
 */

#ifndef HW_MIPS_SGI_SN1_TOPOLOGY_H
#define HW_MIPS_SGI_SN1_TOPOLOGY_H

#include "hw/mips/sgi/sn-model.h"

/*
 * The physical Bedrock processor-interface slot (0..3, spanning PI0's A/B
 * and PI1's A/B) that the node's "logical"-th populated CPU occupies, in
 * cpu_slots[] population order. cpu_slots[] enumerates every slot the ASIC
 * has, populated or not, so the slot's array index already is its physical
 * position -- this walks past the unpopulated ones to find the requested
 * populated one.
 */
unsigned sn1_cpu_slot(const SGIModelNode *node, unsigned logical);

/* The bitmask of node->cpu_slots[] indices whose assembly is populated. */
uint32_t sn1_cpu_population_mask(const SGIModelNode *node);

#endif /* HW_MIPS_SGI_SN1_TOPOLOGY_H */
