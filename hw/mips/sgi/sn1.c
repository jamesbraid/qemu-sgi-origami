/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 machine class: CPU part, default population, and description.
 * sn1-node.c owns Bedrock CPUs, L1, XBridge, PROM policy, and PCI population.
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

static const TypeInfo origin300_machine_type = {
    .name          = MACHINE_TYPE_NAME("origin300"),
    .parent        = TYPE_SGI_SN_MACHINE,
    .class_init    = origin300_class_init,
};

static void origin300_machine_register_types(void)
{
    type_register_static(&origin300_machine_type);
}

type_init(origin300_machine_register_types)
