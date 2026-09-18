/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "sn1-topology.h"

/*
 * Headroom for the scratch array below, not a hardware constant: no
 * Bedrock-family node comes close to this many processor-interface
 * slots. Keeps this file free of a compile-time dependency on
 * sn1-node.h's own SGI_SN1_NODE_MAX_CPUS, since this lookup and its
 * regression test (tests/unit/test-sn1-topology.c) share nothing else
 * with the node device.
 */
#define SN1_TOPOLOGY_MAX_SLOTS 64

/*
 * Every populated slot, in diagonal population order: round 0 visits
 * each interface's own position 0 before round 1 visits each
 * interface's position 1, and so on. This is the order a real Origin
 * 300 board populates in -- the documented two-processor board fits
 * one processor per interface (PI0/A and PI1/A; see sn1_cpu_slot's own
 * comment) rather than filling one interface first -- so cpu_slots'
 * own list order (which groups every position of one interface before
 * the next) is not itself the population order and has to be walked
 * this way instead of straight through. Returns the count written into
 * physical[].
 */
static unsigned sn1_cpu_population_order(
    const SGIModelNode *node, unsigned physical[SN1_TOPOLOGY_MAX_SLOTS])
{
    const SGIModelCpuSlotList *entry;
    unsigned per_interface = 0;
    unsigned count = 0;
    unsigned round;

    for (entry = node->cpu_slots; entry; entry = entry->next) {
        per_interface = MAX(per_interface, entry->value->slot + 1);
    }
    for (round = 0; round < per_interface; round++) {
        for (entry = node->cpu_slots; entry; entry = entry->next) {
            const SGIModelCpuSlot *slot = entry->value;

            if (slot->slot != round || !slot->assembly) {
                continue;
            }
            g_assert(count < SN1_TOPOLOGY_MAX_SLOTS);
            /*
             * The hardware bit/register layout sgi-bedrock.c's
             * processor-mask and cpu-enable-initial-mask split into
             * per-interface halves (its own (mask >> (2 * i)) & 3
             * unpacking): interface * cpus-per-interface + position.
             */
            physical[count++] = slot->interface * per_interface + slot->slot;
        }
    }
    return count;
}

unsigned sn1_cpu_slot(const SGIModelNode *node, unsigned logical)
{
    unsigned physical[SN1_TOPOLOGY_MAX_SLOTS];
    unsigned count = sn1_cpu_population_order(node, physical);

    g_assert(logical < count);
    return physical[logical];
}

uint32_t sn1_cpu_population_mask(const SGIModelNode *node, unsigned nr_cpus)
{
    unsigned physical[SN1_TOPOLOGY_MAX_SLOTS];
    unsigned count = sn1_cpu_population_order(node, physical);
    uint32_t mask = 0;
    unsigned i;

    g_assert(nr_cpus <= count);
    for (i = 0; i < nr_cpus; i++) {
        mask |= 1U << physical[i];
    }
    return mask;
}
