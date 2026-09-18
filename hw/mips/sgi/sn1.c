/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Origin 300 product class over the abstract family builder
 * (sn-machine.c). Everything specific to the Bedrock node -- the CPUs,
 * L1, XBridge, PROM policy and PCI population -- lives in sgi-sn1-node
 * (sn1-node.c); this file only registers the machine type and its
 * fixed facts (CPU part, default population, description).
 */

#include "qemu/osdep.h"
#include "hw/core/boards.h"
#include "hw/mips/sgi/sn-machine.h"
#include "cpu.h"

static const char * const sgi_sn1_r14000_cpu_types[] = {
    MIPS_CPU_TYPE_NAME("R14000"),
    NULL,
};

static void origin300_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelTopology *t = sgi_model_find_topology("origin300");

    g_assert(t);
    smc->product = "origin300";
    smc->default_topology = "origin300";
    mc->desc = t->description;
    mc->valid_cpu_types = sgi_sn1_r14000_cpu_types;
    mc->default_cpus = sgi_machine_topology_default_cpus(t);
    /* Bedrock's one accepted population is fixed at two CPUs. */
    mc->min_cpus = mc->default_cpus;
    mc->max_cpus = sgi_machine_product_max_cpus("origin300");
    mc->default_ram_size = sgi_machine_topology_default_ram(t);
    mc->default_ram_id = "sn1.ram";
}

/*
 * A bare -M origin300 needs sockets and cores preset before QEMU's own
 * -smp completion runs, the same reason origin2000_instance_init exists
 * in sn-machine.c: nothing else sets machine->smp.sockets before a
 * launch with no -smp at all reaches sgi_machine_realize's "topology is
 * one node, one socket" check, and leaving that to QEMU's own default
 * completion is an accident this makes explicit instead. The shipped
 * Origin 300 is one chassis, one node, carrying its default population
 * as that node's cores.
 */
static void origin300_instance_init(Object *obj)
{
    MachineState *machine = MACHINE(obj);
    const SGIModelTopology *t = sgi_model_find_topology("origin300");

    g_assert(t);
    g_assert(!t->nodes->next);
    machine->smp.sockets = 1;
    machine->smp.cores = sgi_machine_topology_default_cpus(t);
    machine->smp.threads = 1;
}

static const TypeInfo origin300_machine_type = {
    .name          = MACHINE_TYPE_NAME("origin300"),
    .parent        = TYPE_SGI_SN_MACHINE,
    .instance_init = origin300_instance_init,
    .class_init    = origin300_class_init,
};

static void origin300_machine_register_types(void)
{
    type_register_static(&origin300_machine_type);
}

type_init(origin300_machine_register_types)
