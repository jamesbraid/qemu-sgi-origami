/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Bedrock machine classes: CPU part, population, and description.
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

static const char * const sgi_sn1_r16000_cpu_types[] = {
    MIPS_CPU_TYPE_NAME("R16000"),
    NULL,
};

/*
 * The Bedrock products differ only in catalogue data, so one class init
 * serves each, keyed by its product and selected default topology.
 */
static void sn1_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const char *product = data;
    bool ip53 = g_str_equal(product, "origin350");
    const char *topology = product;
    const SGIModelTopology *t = sgi_model_find_topology(topology);

    g_assert(t);
    smc->product = product;
    smc->default_topology = topology;
    mc->desc = t->description;
    mc->valid_cpu_types = ip53 ? sgi_sn1_r16000_cpu_types :
                                 sgi_sn1_r14000_cpu_types;
    mc->default_cpus = sgi_machine_topology_default_cpus(t);
    /* A product's default population is also its smallest accepted one. */
    mc->min_cpus = mc->default_cpus;
    mc->max_cpus = sgi_machine_product_max_cpus(product);
    mc->default_ram_size = sgi_machine_topology_default_ram(t);
    mc->default_ram_id = "sn1.ram";
    sgi_machine_class_add_storage(oc);
}

static const TypeInfo sn1_machine_types[] = {
    {
        .name          = MACHINE_TYPE_NAME("origin300"),
        .parent        = TYPE_SGI_SN_MACHINE,
        .class_init    = sn1_machine_class_init,
        .class_data    = "origin300",
    },
    {
        .name          = MACHINE_TYPE_NAME("origin350"),
        .parent        = TYPE_SGI_SN_MACHINE,
        .class_init    = sn1_machine_class_init,
        .class_data    = "origin350",
    },
    {
        .name          = MACHINE_TYPE_NAME("fuel"),
        .parent        = TYPE_SGI_SN_MACHINE,
        .class_init    = sn1_machine_class_init,
        .class_data    = "fuel",
    },
};

DEFINE_TYPES(sn1_machine_types)
