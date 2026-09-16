/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 ("SN1") CPU population-to-Bedrock-slot topology. A pure lookup
 * on SN1MachineModel, with no device state, so the node realizer and its
 * own regression test share one implementation.
 */

#ifndef HW_MIPS_SGI_SN1_TOPOLOGY_H
#define HW_MIPS_SGI_SN1_TOPOLOGY_H

#include "sn1-model.h"

/*
 * The physical Bedrock processor-interface slot (0..3, spanning PI0's A/B
 * and PI1's A/B) that the model's "logical"-th populated CPU occupies, in
 * population order. A model that leaves cpu_population_mask unset (0) gets
 * the legacy contiguous mask implied by cpu_count -- slots 0..cpu_count-1 --
 * so a model predating the mask field still resolves instead of aborting.
 */
unsigned sn1_cpu_slot(const SN1MachineModel *model, unsigned logical);

#endif /* HW_MIPS_SGI_SN1_TOPOLOGY_H */
