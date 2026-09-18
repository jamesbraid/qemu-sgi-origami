/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The compiled descriptor's own structural invariants (hw/mips/sgi/
 * sn-model.c's sgi_model_entries[]), checked once here instead of at
 * every machine launch. hw/mips/sgi/sn-machine.c's builder trusts this
 * descriptor now: Origami's compiler and its goldens are what keep
 * sgi_model_entries[] well-formed, and this test is what proves that for
 * the entries actually shipped. See docs/plans/sn-machine-model.md,
 * "The builder trusts the descriptor", and docs/designs/
 * sn-machine-model-architecture.md's "Validation" section.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/sn-model.h"

/*
 * A node ASIC's CPU-slot and memory-bank shape: hardware facts, not
 * configuration (the deleted sgi_machine_asic_shape()'s own values).
 * The Hub's real numbers live in sn-machine.c's sgi_sn0_hub_hardware,
 * whose header (sn0.h) pulls in target/mips/cpu.h and so cannot link
 * into this host-native test; both kinds resolve to the same
 * SGI_MODEL_MAX_MEMORY_BANKS today (sgi_sn0_hub_hardware.memory.
 * architectural_banks is 8, the array bound every node's bank_mib[]
 * already carries), so restating that literal here duplicates no
 * information sn-model.h does not already fix.
 */
struct AsicShape {
    unsigned int cpu_interfaces;
    unsigned int cpu_slots_per_interface;
    unsigned int architectural_banks;
};

static bool asic_shape(SGIModelAsicKind kind, struct AsicShape *shape)
{
    switch (kind) {
    case SGI_MODEL_ASIC_HUB:
        *shape = (struct AsicShape){
            .cpu_interfaces = 1,
            .cpu_slots_per_interface = 2,
            .architectural_banks = SGI_MODEL_MAX_MEMORY_BANKS,
        };
        return true;
    case SGI_MODEL_ASIC_BEDROCK:
        *shape = (struct AsicShape){
            .cpu_interfaces = 2,
            .cpu_slots_per_interface = 2,
            .architectural_banks = SGI_MODEL_MAX_MEMORY_BANKS,
        };
        return true;
    default:
        return false;
    }
}

/* Every module's nodes[] partitions t->node_count: no gap, no overlap. */
static void check_modules_partition_nodes(const SGIModelTopology *t)
{
    g_autofree bool *claimed = g_new0(bool, t->node_count);
    size_t i, j;

    for (i = 0; i < t->module_count; i++) {
        const SGIModelModule *module = &t->modules[i];

        for (j = 0; j < module->node_count; j++) {
            unsigned int node_index = module->nodes[j];

            g_assert_cmpuint(node_index, <, t->node_count);
            g_assert_false(claimed[node_index]);
            claimed[node_index] = true;
        }
    }
    for (i = 0; i < t->node_count; i++) {
        g_assert_true(claimed[i]);
    }
}

/* Every node names a known ASIC kind; its CPU slots and banks fit it. */
static void check_node_shapes(const SGIModelTopology *t)
{
    size_t i, j;

    for (i = 0; i < t->node_count; i++) {
        const SGIModelNode *node = &t->nodes[i];
        struct AsicShape shape = { };

        /*
         * Zero-initialized rather than left indeterminate: GCC's flow
         * analysis cannot see that g_assert_true() never returns on
         * failure, so it otherwise flags the fields below as possibly
         * read uninitialized.
         */
        g_assert_true(asic_shape(node->asic_kind, &shape));
        for (j = 0; j < node->cpu_slot_count; j++) {
            const SGIModelCpuSlot *slot = &node->cpu_slots[j];

            g_assert_cmpuint(slot->interface, <, shape.cpu_interfaces);
            g_assert_cmpuint(slot->slot, <, shape.cpu_slots_per_interface);
        }
        for (j = 0; j < SGI_MODEL_MAX_MEMORY_BANKS; j++) {
            if (node->bank_mib[j] != 0) {
                g_assert_cmpuint(j, <, shape.architectural_banks);
            }
        }
    }
}

/* Every link's two ends name a real vertex; no end is shared by two links. */
static void check_links(const SGIModelTopology *t)
{
    size_t i, j;

    for (i = 0; i < t->link_count; i++) {
        const SGIModelFabricLink *link = &t->links[i];
        unsigned int side;

        for (side = 0; side < 2; side++) {
            const SGIModelFabricVertex *end = &link->ends[side];
            unsigned int count;

            switch (end->kind) {
            case SGI_MODEL_FABRIC_NODE:
                count = t->node_count;
                break;
            case SGI_MODEL_FABRIC_ROUTER:
                count = t->router_count;
                break;
            default:
                g_assert_not_reached();
            }
            g_assert_cmpuint(end->index, <, count);
            if (end->kind == SGI_MODEL_FABRIC_ROUTER) {
                g_assert_cmpuint(end->port, >=, 1);
                g_assert_cmpuint(end->port, <=,
                                 t->routers[end->index].port_count);
            }
        }
        for (j = 0; j < i; j++) {
            const SGIModelFabricLink *other = &t->links[j];
            unsigned int a, b;

            for (a = 0; a < 2; a++) {
                for (b = 0; b < 2; b++) {
                    g_assert_false(
                        link->ends[a].kind == other->ends[b].kind &&
                        link->ends[a].index == other->ends[b].index &&
                        link->ends[a].port == other->ends[b].port);
                }
            }
        }
    }
}

/* Every io_boards[] entry names a board kind the builder knows. */
static void check_io_board_kinds(const SGIModelTopology *t)
{
    size_t i;

    for (i = 0; i < t->io_board_count; i++) {
        SGIModelIoBoardKind kind = t->io_boards[i].board_kind;

        g_assert_true(kind == SGI_MODEL_IO_IP29_IO ||
                      kind == SGI_MODEL_IO_BASEIO ||
                      kind == SGI_MODEL_IO_IO8);
    }
}

/*
 * Every node claims exactly one crosstalk attachment as its own, and the
 * remaining attachments own the topology's io_boards[] entries one for
 * one: the same split sgi_machine_index_xtalk_attachments() computes at
 * realize time. Hub topologies only -- sgi_machine_realize() builds a
 * Bedrock node and returns before that function ever runs, because a
 * Bedrock node's XBridge is not a separate crosstalk-attached board the
 * way a Hub's onboard or Crossbow-attached board is; a Bedrock
 * topology's io_boards[] entries are checked by
 * check_bedrock_has_populated_io8() below instead.
 */
static void check_xtalk_attachments(const SGIModelTopology *t)
{
    g_autofree bool *claimed = g_new0(bool, t->node_count);
    unsigned int board = 0;
    size_t i;

    if (t->nodes[0].asic_kind != SGI_MODEL_ASIC_HUB) {
        return;
    }

    for (i = 0; i < t->xtalk_attachment_count; i++) {
        const SGIModelXtalkAttachment *attachment = &t->xtalk_attachments[i];
        unsigned int node_index = attachment->node_index;

        g_assert_cmpuint(node_index, <, t->node_count);
        if (attachment->crossbar_index < 0) {
            g_assert_false(claimed[node_index]);
            claimed[node_index] = true;
            g_assert_cmpuint(board, <, t->io_board_count);
            board++;
        } else if (!claimed[node_index]) {
            claimed[node_index] = true;
        } else {
            g_assert_cmpuint(board, <, t->io_board_count);
            board++;
        }
    }
    for (i = 0; i < t->node_count; i++) {
        g_assert_true(claimed[i]);
    }
    g_assert_cmpuint(board, ==, t->io_board_count);
}

/* A Bedrock topology's one node ships a populated IO-8 board. */
static void check_bedrock_has_populated_io8(const SGIModelTopology *t)
{
    bool has_populated_io8 = false;
    size_t i;

    if (t->nodes[0].asic_kind == SGI_MODEL_ASIC_HUB) {
        return;
    }
    for (i = 0; i < t->io_board_count; i++) {
        if (t->io_boards[i].board_kind == SGI_MODEL_IO_IO8 &&
            t->io_boards[i].pci_count) {
            has_populated_io8 = true;
            break;
        }
    }
    g_assert_true(has_populated_io8);
}

static void test_every_topology_is_well_formed(void)
{
    size_t i;

    g_assert_cmpuint(sgi_model_entry_count, >, 0);
    for (i = 0; i < sgi_model_entry_count; i++) {
        const SGIModelTopology *t = &sgi_model_entries[i];

        g_assert_cmpuint(t->node_count, >, 0);
        check_modules_partition_nodes(t);
        check_node_shapes(t);
        check_links(t);
        check_io_board_kinds(t);
        check_xtalk_attachments(t);
        check_bedrock_has_populated_io8(t);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sn-model/every-topology-is-well-formed",
                    test_every_topology_is_well_formed);
    return g_test_run();
}
