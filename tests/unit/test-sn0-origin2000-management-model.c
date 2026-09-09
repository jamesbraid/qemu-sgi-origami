/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/mips/sn0-origin2000-management.h"

typedef struct Fixture {
    SN0ModelComposition model;
    SN0ModelModule module;
    SN0ModelModuleManagement management;
    SN0ModelPhysicalNode nodes[4];
    SN0ModelComponent hubs[4];
    unsigned int members[4];
    unsigned int adapters[4];
    uint64_t offsets[4];
} Fixture;

static void fixture_init(Fixture *f)
{
    const SN0ModelComposition *model =
        sn0_origin2000_find_composition(sn0_origin2000_default_composition);

    g_assert_nonnull(model);
    f->model = *model;
    f->module = model->modules[0];
    f->management = *f->module.management;
    f->model.modules = &f->module;
    f->model.nodes = f->nodes;
    f->module.management = &f->management;
    f->module.nodes = f->members;
    f->management.adapter_nodes = f->adapters;
    f->management.adapter_offsets = f->offsets;
    for (unsigned int i = 0; i < 4; i++) {
        f->nodes[i] = model->nodes[i];
        f->hubs[i] = *model->nodes[i].hub;
        f->nodes[i].hub = &f->hubs[i];
        f->members[i] = model->modules[0].nodes[i];
        f->adapters[i] = model->modules[0].management->adapter_nodes[i];
        f->offsets[i] = model->modules[0].management->adapter_offsets[i];
    }
}

static void test_generated(void)
{
    Fixture f;

    fixture_init(&f);
    g_assert_true(sn0_origin2000_management_validate(&f.model, &error_abort));
}

static void test_reordered(void)
{
    Fixture f;
    SN0ModelPhysicalNode node;

    fixture_init(&f);
    /* Distinct windows discriminate the node reference from loop order. */
    for (unsigned int i = 0; i < 4; i++) {
        f.offsets[i] += 16 * i;
        f.nodes[i].map.hub_registers_local.size = f.offsets[i] + 16;
        f.nodes[i].map.hub_registers_node.size = f.offsets[i] + 16;
    }
    node = f.nodes[0];
    f.nodes[0] = f.nodes[3];
    f.nodes[3] = node;
    f.members[0] = 3;
    f.members[3] = 0;
    f.adapters[0] = 3;
    f.adapters[3] = 0;
    f.module.initial_module_number = 29;
    g_assert_true(sn0_origin2000_management_validate(&f.model, &error_abort));
    g_assert_cmpuint(f.module.initial_module_number, ==, 29);

    /* A valid address in a different node's window cannot replace this pair. */
    f.offsets[0] = f.offsets[3];
    {
        Error *err = NULL;

        g_assert_false(sn0_origin2000_management_validate(&f.model, &err));
        g_assert_nonnull(err);
        error_free(err);
    }
}

static void test_invalid(gconstpointer opaque)
{
    unsigned int scenario = GPOINTER_TO_UINT(opaque);
    Fixture f;
    Error *err = NULL;

    fixture_init(&f);
    switch (scenario) {
    case 0:
        f.model.module_count = 0;
        break;
    case 1:
        f.model.modules = NULL;
        break;
    case 2:
        f.model.node_count = 3;
        break;
    case 3:
        f.model.nodes = NULL;
        break;
    case 4:
        f.module.initial_module_number = 0;
        break;
    case 5:
        f.module.management = NULL;
        break;
    case 6:
        f.module.node_count = 3;
        break;
    case 7:
        f.members[1] = f.members[0];
        break;
    case 8:
        f.members[0] = 4;
        break;
    case 9:
        f.nodes[2].module_index = 1;
        break;
    case 10:
        f.management.adapter_count = 3;
        break;
    case 11:
        f.management.adapter_nodes = NULL;
        break;
    case 12:
        f.management.adapter_offsets = NULL;
        break;
    case 13:
        f.adapters[1] = f.adapters[0];
        break;
    case 14:
        f.adapters[2] = 4;
        break;
    case 15:
        f.management.msc_address++;
        break;
    case 16:
        f.management.nvram_address++;
        break;
    case 17:
        f.management.nvram_address_count--;
        break;
    case 18:
        f.management.nvram_bytes--;
        break;
    case 19:
        f.offsets[0] |= 1;
        break;
    case 20:
        f.offsets[0] = UINT64_MAX - 7;
        break;
    case 21:
        f.nodes[0].map.hub_registers_local.size = f.offsets[0] + 8;
        break;
    case 22:
        f.nodes[0].map.hub_registers_node.size = f.offsets[0] + 8;
        break;
    case 23:
        f.hubs[1].i2c_controller = true;
        break;
    case 24:
        f.hubs[1].system_controller = true;
        break;
    case 25:
        f.hubs[1].i2c_nvram_pages = 8;
        break;
    case 26:
        f.nodes[0].hub = NULL;
        break;
    case 27:
        f.nodes[1].slot = f.nodes[0].slot;
        break;
    case 28:
        f.nodes[0].slot = 0;
        break;
    case 29:
        f.nodes[0].slot = 5;
        break;
    case 30:
        f.module.nodes = NULL;
        break;
    case 31:
        f.nodes[0].map.hub_registers_local.size = 8;
        break;
    case 32:
        f.nodes[0].map.hub_registers_node.size = 8;
        break;
    default:
        g_assert_not_reached();
    }
    g_assert_false(sn0_origin2000_management_validate(&f.model, &err));
    g_assert_nonnull(err);
    error_free(err);
}

int main(int argc, char **argv)
{
    const char *invalid[] = {
        "module-count", "module-pointer", "node-count", "node-pointer",
        "zero-module-number", "management-pointer", "member-count",
        "duplicate-member", "member-index", "node-module", "adapter-count",
        "adapter-pointer", "offset-pointer", "duplicate-adapter",
        "adapter-index",
        "msc-address", "nvram-address", "nvram-pages", "nvram-size",
        "unaligned-offset", "offset-overflow", "local-window", "remote-window",
        "private-controller", "private-msc", "private-nvram", "hub-pointer",
        "duplicate-slot", "zero-slot", "slot-range", "member-pointer",
        "short-local-window", "short-remote-window",
    };

    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/origin2000/management/generated", test_generated);
    g_test_add_func("/origin2000/management/reordered", test_reordered);
    for (unsigned int i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_autofree char *name =
            g_strdup_printf("/origin2000/management/reject/%s", invalid[i]);

        g_test_add_data_func(name, GUINT_TO_POINTER(i), test_invalid);
    }
    return g_test_run();
}
