/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "sn1-topology.h"

uint32_t sn1_cpu_population_mask(const SGIModelNode *node)
{
    const SGIModelCpuSlotList *entry;
    uint32_t mask = 0;
    unsigned physical;

    for (entry = node->cpu_slots, physical = 0; entry;
         entry = entry->next, physical++) {
        if (entry->value->assembly) {
            mask |= 1U << physical;
        }
    }
    return mask;
}

unsigned sn1_cpu_slot(const SGIModelNode *node, unsigned logical)
{
    const SGIModelCpuSlotList *entry;
    unsigned physical;

    for (entry = node->cpu_slots, physical = 0; entry;
         entry = entry->next, physical++) {
        if (entry->value->assembly) {
            if (logical == 0) {
                return physical;
            }
            logical--;
        }
    }
    g_assert_not_reached();
}
