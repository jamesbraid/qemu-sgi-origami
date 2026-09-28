/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "sn1-topology.h"

/*
 * A Bedrock has exactly two processor interfaces, two processors each; the
 * same fact SGI_SN1_NODE_MAX_CPUS (4) records in sn1-node.h and
 * sgi-bedrock.c's (mask >> (2 * i)) & 3 unpacking assumes. This is a hardware
 * constant, not something to infer from how many of a descriptor's cpu_slots
 * entries happen to be listed: the compiler always emits the full
 * interfaces-times-positions square today, but a lookup that only works
 * because of that convention, with no check that it holds, is a silent
 * miscount waiting for a descriptor shape it does not expect. Kept as a plain
 * constant here rather than an include of sgi-bedrock.h, which would pull in
 * device state this file has none of (its header comment: "no device state").
 */
#define SN1_BEDROCK_PROCESSOR_INTERFACES 2
#define SN1_BEDROCK_PROCESSORS_PER_INTERFACE 2
#define SN1_BEDROCK_PROCESSORS \
    (SN1_BEDROCK_PROCESSOR_INTERFACES * SN1_BEDROCK_PROCESSORS_PER_INTERFACE)

/*
 * Every populated slot, in diagonal population order: round 0 visits each
 * interface's position 0 before round 1 visits each interface's position 1,
 * and so on. This is the order a real Origin 300 board populates in: the
 * documented two-processor board fits one processor per interface (PI0/A and
 * PI1/A; see sn1_cpu_slot's comment) rather than filling one interface
 * first, so cpu_slots' own list order (which groups every position of one
 * interface before the next) is not itself the population order and has to be
 * walked this way instead of straight through. Returns the count written into
 * physical[]. Aborts if a slot names an interface or position outside the
 * Bedrock's two-by-two shape, rather than silently computing a physical bit
 * that does not match the hardware register layout.
 */
static unsigned sn1_cpu_population_order(
    const SGIModelNode *node,
    unsigned physical[SN1_BEDROCK_PROCESSORS])
{
    const SGIModelCpuSlotList *entry;
    unsigned count = 0;
    unsigned round;

    for (entry = node->cpu_slots; entry; entry = entry->next) {
        const SGIModelCpuSlot *slot = entry->value;

        g_assert(slot->interface < SN1_BEDROCK_PROCESSOR_INTERFACES);
        g_assert(slot->slot < SN1_BEDROCK_PROCESSORS_PER_INTERFACE);
    }
    for (round = 0; round < SN1_BEDROCK_PROCESSORS_PER_INTERFACE; round++) {
        for (entry = node->cpu_slots; entry; entry = entry->next) {
            const SGIModelCpuSlot *slot = entry->value;

            if (slot->slot != round || !slot->assembly) {
                continue;
            }
            g_assert(count < SN1_BEDROCK_PROCESSORS);
            /*
             * The hardware bit/register layout sgi-bedrock.c's processor-mask
             * and cpu-enable-initial-mask split into per-interface halves
             * (its (mask >> (2 * i)) & 3 unpacking): interface *
             * processors-per-interface + position.
             */
            physical[count++] =
                slot->interface * SN1_BEDROCK_PROCESSORS_PER_INTERFACE +
                slot->slot;
        }
    }
    return count;
}

unsigned sn1_cpu_slot(const SGIModelNode *node, unsigned logical)
{
    unsigned physical[SN1_BEDROCK_PROCESSORS];
    unsigned count = sn1_cpu_population_order(node, physical);

    g_assert(logical < count);
    return physical[logical];
}

uint32_t sn1_cpu_population_mask(const SGIModelNode *node, unsigned nr_cpus)
{
    unsigned physical[SN1_BEDROCK_PROCESSORS];
    unsigned count = sn1_cpu_population_order(node, physical);
    uint32_t mask = 0;
    unsigned i;

    g_assert(nr_cpus <= count);
    for (i = 0; i < nr_cpus; i++) {
        mask |= 1U << physical[i];
    }
    return mask;
}
