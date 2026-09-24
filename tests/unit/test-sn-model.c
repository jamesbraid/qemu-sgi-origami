/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The compiled catalogue's own structural invariants (hw/mips/sgi/
 * sn-catalogue.json's topologies), checked once here instead of at every
 * machine launch. hw/mips/sgi/sn-machine.c's builder trusts this
 * catalogue now: Origami's compiler and its goldens are what keep it
 * well-formed, and this test is what proves that for the file actually
 * shipped. See docs/plans/sn-machine-model.md, "The builder trusts the
 * descriptor", and docs/designs/sn-machine-model-architecture.md's
 * "Validation" section.
 *
 * Parses the tracked hw/mips/sgi/sn-catalogue.json directly, the same way
 * hw/mips/sgi/sn-catalogue.c parses the build's embedded copy of the same
 * file (qobject_from_json() and qobject_input_visitor_new(), also
 * hw/uefi/var-service-json.c's pattern) -- reading the file here rather
 * than linking the embedding machinery keeps this a host-native test with
 * no target/mips dependency, the same property it had linking sn-model.c
 * directly before the compiled-catalogue step.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-types-sgi-sn.h"
#include "qapi/qapi-visit-sgi-sn.h"
#include "qapi/qobject-input-visitor.h"
#include "qobject/qjson.h"
#include "qobject/qobject.h"

/*
 * A node ASIC's CPU-slot and memory-bank shape: hardware facts, not
 * configuration (the deleted sgi_machine_asic_shape()'s own values).
 * The Hub's real numbers live in hw/mips/sgi/sn-machine.c's
 * sgi_sn0_hub_hardware, whose header (sn0.h) pulls in target/mips/cpu.h
 * and so cannot link into this host-native test; both kinds resolve to
 * the same architectural bank count today (sgi_sn0_hub_hardware.memory.
 * architectural_banks is 8, the same array bound every node's bank-mib
 * already carries), so restating that literal here duplicates no
 * information the catalogue does not already fix.
 */
#define SN_ARCHITECTURAL_MEMORY_BANKS 8

struct AsicShape {
    unsigned int cpu_interfaces;
    unsigned int cpu_slots_per_interface;
    unsigned int architectural_banks;
};

static bool asic_shape(SGIModelAsicKind kind, struct AsicShape *shape)
{
    switch (kind) {
    case SGI_MODEL_ASIC_KIND_HUB:
        *shape = (struct AsicShape){
            .cpu_interfaces = 1,
            .cpu_slots_per_interface = 2,
            .architectural_banks = SN_ARCHITECTURAL_MEMORY_BANKS,
        };
        return true;
    case SGI_MODEL_ASIC_KIND_BEDROCK:
        *shape = (struct AsicShape){
            .cpu_interfaces = 2,
            .cpu_slots_per_interface = 2,
            .architectural_banks = SN_ARCHITECTURAL_MEMORY_BANKS,
        };
        return true;
    default:
        return false;
    }
}

static size_t node_count(const SGIModelTopology *t)
{
    const SGIModelNodeList *entry;
    size_t count = 0;

    for (entry = t->nodes; entry; entry = entry->next) {
        count++;
    }
    return count;
}

static size_t router_count(const SGIModelTopology *t)
{
    const SGIModelRouterList *entry;
    size_t count = 0;

    for (entry = t->routers; entry; entry = entry->next) {
        count++;
    }
    return count;
}

static size_t io_board_count(const SGIModelTopology *t)
{
    const SGIModelIoBoardList *entry;
    size_t count = 0;

    for (entry = t->io_boards; entry; entry = entry->next) {
        count++;
    }
    return count;
}

static const SGIModelNode *node0(const SGIModelTopology *t)
{
    g_assert(t->nodes);
    return t->nodes->value;
}

/* Every module's nodes list partitions the topology's own nodes: no gap, no
 * overlap.
 */
static void check_modules_partition_nodes(const SGIModelTopology *t)
{
    size_t total_nodes = node_count(t);
    g_autofree bool *claimed = g_new0(bool, total_nodes);
    const SGIModelModuleList *module_entry;
    size_t i;

    for (module_entry = t->modules; module_entry;
         module_entry = module_entry->next) {
        const uint32List *node_entry;

        for (node_entry = module_entry->value->nodes; node_entry;
             node_entry = node_entry->next) {
            unsigned int node_index = node_entry->value;

            g_assert_cmpuint(node_index, <, total_nodes);
            g_assert_false(claimed[node_index]);
            claimed[node_index] = true;
        }
    }
    for (i = 0; i < total_nodes; i++) {
        g_assert_true(claimed[i]);
    }
}

/* Every node names a known ASIC kind; its CPU slots and banks fit it. */
static void check_node_shapes(const SGIModelTopology *t)
{
    const SGIModelNodeList *node_entry;

    for (node_entry = t->nodes; node_entry; node_entry = node_entry->next) {
        const SGIModelNode *node = node_entry->value;
        const SGIModelCpuSlotList *slot_entry;
        const uint64List *bank_entry;
        struct AsicShape shape = { };
        unsigned int bank;

        /*
         * Zero-initialized rather than left indeterminate: GCC's flow
         * analysis cannot see that g_assert_true() never returns on
         * failure, so it otherwise flags the fields below as possibly
         * read uninitialized.
         */
        g_assert_true(asic_shape(node->asic_kind, &shape));
        for (slot_entry = node->cpu_slots; slot_entry;
             slot_entry = slot_entry->next) {
            const SGIModelCpuSlot *slot = slot_entry->value;

            g_assert_cmpuint(slot->interface, <, shape.cpu_interfaces);
            g_assert_cmpuint(slot->slot, <, shape.cpu_slots_per_interface);
        }
        for (bank_entry = node->bank_mib, bank = 0; bank_entry;
             bank_entry = bank_entry->next, bank++) {
            if (bank_entry->value != 0) {
                g_assert_cmpuint(bank, <, shape.architectural_banks);
            }
        }
    }
}

/*
 * Every cable's two ends name a real vertex, at a port at least one (port
 * zero is a crossbar's own local view, never a numbered cable end); no end
 * is shared by two cables. A router's own port count is a device fact
 * (decision D-0026) this catalogue does not carry, so the upper bound on
 * a router end's port is the device's own qtest to check, not this one.
 */
static void check_cables(const SGIModelTopology *t)
{
    size_t total_nodes = node_count(t);
    size_t total_routers = router_count(t);
    const SGIModelCableList *cable_entry;

    for (cable_entry = t->cables; cable_entry;
         cable_entry = cable_entry->next) {
        const SGIModelCable *cable = cable_entry->value;
        const SGIModelLinkEnd *end0 = cable->ends->value;
        const SGIModelLinkEnd *end1 = cable->ends->next->value;
        const SGIModelLinkEnd *ends[2] = { end0, end1 };
        const SGIModelCableList *other_entry;
        unsigned int side;

        g_assert(cable->ends->next);
        g_assert(!cable->ends->next->next);
        for (side = 0; side < 2; side++) {
            const SGIModelLinkEnd *end = ends[side];
            size_t count = end->kind == SGI_MODEL_LINK_END_KIND_ROUTER ?
                total_routers : total_nodes;

            g_assert_cmpuint(end->index, <, count);
            if (end->kind == SGI_MODEL_LINK_END_KIND_ROUTER) {
                g_assert_cmpuint(end->port, >=, 1);
            }
        }
        for (other_entry = t->cables; other_entry != cable_entry;
             other_entry = other_entry->next) {
            const SGIModelCable *other = other_entry->value;
            const SGIModelLinkEnd *other_ends[2] = {
                other->ends->value, other->ends->next->value,
            };
            unsigned int a, b;

            for (a = 0; a < 2; a++) {
                for (b = 0; b < 2; b++) {
                    g_assert_false(
                        ends[a]->kind == other_ends[b]->kind &&
                        ends[a]->index == other_ends[b]->index &&
                        ends[a]->port == other_ends[b]->port);
                }
            }
        }
    }
}

/* Every io-boards entry names a preset the builder knows. */
static void check_io_board_presets(const SGIModelTopology *t)
{
    const SGIModelIoBoardList *entry;

    for (entry = t->io_boards; entry; entry = entry->next) {
        SGIModelIoBoardPreset preset = entry->value->preset;

        g_assert_true(preset == SGI_MODEL_IO_BOARD_PRESET_IP29_IO ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO6 ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO6G ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO8 ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_PCI_SHOEHORN ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_PCI_CARRIER);
    }
}

/*
 * Every node claims exactly one crosstalk attachment as its own, and the
 * remaining attachments own the topology's io-boards entries one for
 * one: the same split sgi_machine_index_xtalk_attachments() computes at
 * realize time. Hub topologies only -- sgi_machine_realize() builds a
 * Bedrock node and returns before that function ever runs, because a
 * Bedrock node's XBridge is not a separate crosstalk-attached board the
 * way a Hub's onboard or Crossbow-attached board is; a Bedrock
 * topology's io-boards entries are checked by
 * check_bedrock_has_populated_io8() below instead.
 */
static void check_xtalk_attachments(const SGIModelTopology *t)
{
    size_t total_nodes = node_count(t);
    size_t total_boards = io_board_count(t);
    g_autofree bool *claimed = g_new0(bool, total_nodes);
    const SGIModelXtalkAttachmentList *entry;
    unsigned int board = 0;
    size_t i;

    if (node0(t)->asic_kind != SGI_MODEL_ASIC_KIND_HUB) {
        return;
    }

    for (entry = t->xtalk_attachments; entry; entry = entry->next) {
        const SGIModelXtalkAttachment *attachment = entry->value;
        unsigned int node_index = attachment->node_index;

        g_assert_cmpuint(node_index, <, total_nodes);
        if (attachment->crossbar_index < 0) {
            g_assert_false(claimed[node_index]);
            claimed[node_index] = true;
            g_assert_cmpuint(board, <, total_boards);
            board++;
        } else if (!claimed[node_index]) {
            claimed[node_index] = true;
        } else {
            g_assert_cmpuint(board, <, total_boards);
            board++;
        }
    }
    for (i = 0; i < total_nodes; i++) {
        g_assert_true(claimed[i]);
    }
    g_assert_cmpuint(board, ==, total_boards);
}

static void check_xtalk_slots(const SGIModelTopology *t)
{
    size_t total_nodes = node_count(t);
    size_t total_crossbars = 0;
    const SGIModelCrossbarList *crossbar;
    const SGIModelXtalkSlotList *entry;

    for (crossbar = t->crossbars; crossbar; crossbar = crossbar->next) {
        total_crossbars++;
    }
    for (entry = t->xtalk_slots; entry; entry = entry->next) {
        const SGIModelXtalkSlot *slot = entry->value;
        const SGIModelXtalkSlotList *other;

        g_assert_cmpuint(slot->crossbar_index, <, total_crossbars);
        g_assert_cmpuint(slot->controlling_node, <, total_nodes);
        for (other = t->xtalk_slots; other != entry; other = other->next) {
            g_assert_cmpstr(slot->id, !=, other->value->id);
            g_assert_false(
                slot->crossbar_index == other->value->crossbar_index &&
                           slot->widget == other->value->widget);
        }
    }
}

static void check_origin2000_io3(const SGIModelTopology *t)
{
    const SGIModelXtalkSlotList *slot_entry;
    const SGIModelXtalkAttachmentList *attachment_entry;
    bool found = false;

    if (!g_str_equal(t->product, "origin2000")) {
        return;
    }
    for (slot_entry = t->xtalk_slots; slot_entry;
         slot_entry = slot_entry->next) {
        const SGIModelXtalkSlot *slot = slot_entry->value;

        if (g_str_equal(slot->id, "xio-slot-3")) {
            g_assert_cmpuint(slot->crossbar_index, ==, 0);
            g_assert_cmpuint(slot->widget, ==, 0xf);
            g_assert_cmpuint(slot->controlling_node, ==, 0);
            found = true;
        }
    }
    g_assert_true(found);
    for (attachment_entry = t->xtalk_attachments; attachment_entry;
         attachment_entry = attachment_entry->next) {
        const SGIModelXtalkAttachment *attachment = attachment_entry->value;

        g_assert_false(attachment->crossbar_index == 0 &&
                       attachment->widget == 0xf);
    }
}

/*
 * A Bedrock topology's one node ships a populated IO-8 board: a preset's
 * fitted IOC3 one-wire serial is the one fact left, after the
 * ownership-rule trim, that distinguishes a populated head from an
 * empty one (Origin 300's second XBridge head carries no population).
 */
static void check_bedrock_has_populated_io8(const SGIModelTopology *t)
{
    bool has_populated_io8 = false;
    const SGIModelIoBoardList *entry;

    if (node0(t)->asic_kind == SGI_MODEL_ASIC_KIND_HUB) {
        return;
    }
    for (entry = t->io_boards; entry; entry = entry->next) {
        if (entry->value->preset == SGI_MODEL_IO_BOARD_PRESET_IO8 &&
            entry->value->has_ioc3_serial) {
            has_populated_io8 = true;
            break;
        }
    }
    g_assert_true(has_populated_io8);
}

static char *catalogue_path(void)
{
    return g_build_filename(g_test_get_dir(G_TEST_DIST), "..", "..", "hw",
                            "mips", "sgi", "sn-catalogue.json", NULL);
}

static SGIModelCatalogue *load_catalogue(void)
{
    g_autofree char *path = catalogue_path();
    g_autofree char *contents = NULL;
    g_autoptr(GError) local_error = NULL;
    QObject *qobj;
    Visitor *v;
    SGIModelCatalogue *catalogue = NULL;

    if (!g_file_get_contents(path, &contents, NULL, &local_error)) {
        g_error("cannot read %s: %s", path, local_error->message);
    }
    qobj = qobject_from_json(contents, &error_abort);
    v = qobject_input_visitor_new(qobj);
    visit_type_SGIModelCatalogue(v, NULL, &catalogue, &error_abort);
    visit_free(v);
    qobject_unref(qobj);
    return catalogue;
}

static void test_every_topology_is_well_formed(void)
{
    g_autoptr(SGIModelCatalogue) catalogue = load_catalogue();
    const SGIModelTopologyList *entry;
    size_t topology_count = 0;

    for (entry = catalogue->topologies; entry; entry = entry->next) {
        const SGIModelTopology *t = entry->value;

        topology_count++;
        g_assert_cmpuint(node_count(t), >, 0);
        check_modules_partition_nodes(t);
        check_node_shapes(t);
        check_cables(t);
        check_io_board_presets(t);
        check_xtalk_attachments(t);
        check_xtalk_slots(t);
        check_origin2000_io3(t);
        check_bedrock_has_populated_io8(t);
    }
    g_assert_cmpuint(topology_count, >, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sn-model/every-topology-is-well-formed",
                    test_every_topology_is_well_formed);
    return g_test_run();
}
