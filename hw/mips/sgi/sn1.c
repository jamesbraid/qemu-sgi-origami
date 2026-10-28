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

/*
 * The Origin 3000 takes the C-brick's R12000, and the Origin 3900's Cx-brick
 * node boards fit an R16000.
 */
static const char * const sgi_sn1_origin3000_cpu_types[] = {
    MIPS_CPU_TYPE_NAME("R12000"),
    MIPS_CPU_TYPE_NAME("R16000"),
    NULL,
};

static const char * const sgi_sn1_r16000_cpu_types[] = {
    MIPS_CPU_TYPE_NAME("R16000"),
    NULL,
};

/*
 * The Bedrock products differ only in catalogue data, so one class init
 * serves each, described by the topology the catalogue names as its
 * machine class's own.
 */
static void sn1_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const char *product = data;
    bool ip53 = g_str_equal(product, "origin350") ||
                g_str_equal(product, "onyx4-host") ||
                g_str_equal(product, "onyx350") ||
                g_str_equal(product, "tezro");
    const SGIModelTopology *t = sgi_model_machine_topology(product);

    g_assert(t);
    smc->product = product;
    /* Preflight still requires topology= for products without a default. */
    smc->default_topology = t->name;
    mc->desc = t->description;
    mc->valid_cpu_types = ip53 ? sgi_sn1_r16000_cpu_types :
                          g_str_equal(product, "origin3000") ?
                          sgi_sn1_origin3000_cpu_types :
                          sgi_sn1_r14000_cpu_types;
    if (g_str_equal(product, "origin3000")) {
        mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R12000");
    }
    mc->default_cpus = sgi_machine_topology_default_cpus(t);
    mc->min_cpus = sgi_machine_product_min_cpus(product);
    mc->max_cpus = sgi_machine_product_max_cpus(product);
    sgi_machine_class_set_default_ram(
        mc, sgi_machine_topology_default_ram(t));
    mc->default_ram_id = "sn1.ram";
    sgi_machine_class_add_storage(oc);
}

static const TypeInfo sn1_machine_types[] = {
    {
        .name = MACHINE_TYPE_NAME("origin300"),
        .parent = TYPE_SGI_SN_MACHINE,
        .class_init = sn1_machine_class_init,
        .class_data = "origin300",
    },
    {
        .name = MACHINE_TYPE_NAME("origin3000"),
        .parent = TYPE_SGI_SN_MACHINE,
        .class_init = sn1_machine_class_init,
        .class_data = "origin3000",
    },
    {
        .name = MACHINE_TYPE_NAME("origin350"),
        .parent = TYPE_SGI_SN_MACHINE,
        .class_init = sn1_machine_class_init,
        .class_data = "origin350",
    },
    {
        .name = MACHINE_TYPE_NAME("onyx4-host"),
        .parent = TYPE_SGI_SN_MACHINE,
        .class_init = sn1_machine_class_init,
        .class_data = "onyx4-host",
    },
    {
        .name = MACHINE_TYPE_NAME("onyx350"),
        .parent = TYPE_SGI_SN_MACHINE,
        .class_init = sn1_machine_class_init,
        .class_data = "onyx350",
    },
    {
        .name = MACHINE_TYPE_NAME("tezro"),
        .parent = TYPE_SGI_SN_MACHINE,
        .class_init = sn1_machine_class_init,
        .class_data = "tezro",
    },
    {
        .name = MACHINE_TYPE_NAME("fuel"),
        .parent = TYPE_SGI_SN_MACHINE,
        .class_init = sn1_machine_class_init,
        .class_data = "fuel",
    },
};

DEFINE_TYPES(sn1_machine_types)
