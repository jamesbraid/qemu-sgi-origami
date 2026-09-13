/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200: the one- or two-chassis SN0 product on the IP29 board.
 * The family machinery this parameterizes lives in sn0.c.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "sn0-origin200-model-id.h"
#include "qemu/units.h"
#include "sn0.h"

static const char * const origin200_r10000_cpu_types[] = {
    MIPS_CPU_TYPE_NAME("R10000"),
    NULL,
};

static const char * const *origin200_valid_cpu_types(SN0ModelCpuKind kind)
{
    switch (kind) {
    case SN0_CPU_R10000:
        return origin200_r10000_cpu_types;
    default:
        g_assert_not_reached();
    }
}

static char *origin200_get_model_id(Object *obj, Error **errp)
{
    return g_strdup(ORIGAMI_ORIGIN200_MODEL_ID);
}

static void origin200_set_model_id(Object *obj, const char *value,
                                   Error **errp)
{
    if (strcmp(value, ORIGAMI_ORIGIN200_MODEL_ID) != 0) {
        error_setg(errp, "origin200 model-id mismatch: requested '%s', "
                   "compiled '%s'", value, ORIGAMI_ORIGIN200_MODEL_ID);
    }
}

static void origin200_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SN0MachineClass *smc = SN0_MACHINE_CLASS(oc);
    const SN0ModelProfile *default_profile;
    unsigned int default_cpus = 0;
    size_t node_index;

    smc->model = &sn0_origin200_model;
    default_profile = &smc->model->profiles[0];
    for (node_index = 0; node_index < default_profile->node_count;
         node_index++) {
        const SN0ModelNodeInstance *node =
            &default_profile->nodes[node_index];

        default_cpus += node->cpu_count;
    }
    object_class_property_add_str(oc, "model-id",
                                  origin200_get_model_id,
                                  origin200_set_model_id);
    object_class_property_set_description(oc, "model-id",
                                          "Require the compiled machine model");
    mc->desc = smc->model->description;
    mc->valid_cpu_types = origin200_valid_cpu_types(smc->model->cpu_kind);
    mc->default_ram_size = smc->model->default_ram_mib * MiB;
    mc->default_cpus = default_cpus;
    mc->default_ram_id = "sn0.node0.ram";
    /* -hda and if=default drives belong on the first SCSI adapter. */
    mc->block_default_type = IF_SCSI;
    mc->default_nic = "sgi-ioc3-eth";
    /* Node memory is memdev-backed -numa nodes; bare mem= has no backend. */
    mc->numa_mem_supported = false;

    mc->max_cpus = sn0_model_max_cpus(smc->model);
}

static const TypeInfo origin200_machine_types[] = {
    {
        .name       = MACHINE_TYPE_NAME("origin200"),
        .parent     = TYPE_SN0_MACHINE,
        .class_init = origin200_class_init,
    },
};

DEFINE_TYPES(origin200_machine_types)
