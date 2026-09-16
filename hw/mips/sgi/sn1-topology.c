/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "sn1-topology.h"

uint32_t sn1_cpu_population_mask(const SGIModelNode *node)
{
    uint32_t mask = 0;
    size_t physical;

    for (physical = 0; physical < node->cpu_slot_count; physical++) {
        if (node->cpu_slots[physical].assembly) {
            mask |= 1U << physical;
        }
    }
    return mask;
}

unsigned sn1_cpu_slot(const SGIModelNode *node, unsigned logical)
{
    size_t physical;

    for (physical = 0; physical < node->cpu_slot_count; physical++) {
        if (node->cpu_slots[physical].assembly) {
            if (logical == 0) {
                return physical;
            }
            logical--;
        }
    }
    g_assert_not_reached();
}
