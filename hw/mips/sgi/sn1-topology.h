/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Origin 300 ("SN1") CPU population-to-Bedrock-slot topology. A pure lookup
 * on the compiled catalogue's Bedrock node record (qapi/sgi-sn.json), with no
 * device state, so the node realizer and its regression test share one
 * implementation.
 */

#ifndef HW_MIPS_SGI_SN1_TOPOLOGY_H
#define HW_MIPS_SGI_SN1_TOPOLOGY_H

#include "hw/mips/sgi/sn-catalogue.h"

/*
 * The physical Bedrock processor-interface slot (0..3: interface *
 * cpus_per_interface + position within that interface, spanning PI0's A/B and
 * PI1's A/B) that the node's "logical"-th realized CPU occupies.
 *
 * A real Origin 300 populates diagonally: the documented two-processor board
 * fits one processor per interface (A on PI0, C on PI1) before a second on
 * either: PI0/A and PI1/A are fitted, PI0/B and PI1/B are empty, and the
 * catalogue's cpu_slots wiring fits every position the largest accepted
 * population uses (this configuration accepts either two or four processors
 * against the same four-slot wiring), shared by every smaller accepted
 * population of the same configuration. So a populated cpu_slots entry alone
 * does not say whether this run realizes it: "logical"-th walks populated
 * slots in that diagonal order (every interface's first position before
 * any interface's second), and the caller bounds "logical" by how many
 * processors this run actually creates.
 */
unsigned sn1_cpu_slot(const SGIModelNode *node, unsigned logical);

/*
 * The bitmask of physical slots this run's first nr_cpus realized processors
 * occupy, in the same diagonal order sn1_cpu_slot walks. It does not cover
 * every cpu_slots entry the catalogue marks populated, which can exceed
 * nr_cpus when the configuration accepts more than one population size.
 * nr_cpus must not exceed the catalogue's populated count.
 */
uint32_t sn1_cpu_population_mask(const SGIModelNode *node, unsigned nr_cpus);

#endif /* HW_MIPS_SGI_SN1_TOPOLOGY_H */
