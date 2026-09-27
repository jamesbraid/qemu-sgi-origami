/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check the shipped hw/mips/sgi/sn-catalogue.json topology once; sn-machine.c
 * trusts the compiler and golden data at machine launch. Parse the tracked
 * file with qobject_from_json() and qobject_input_visitor_new(), as
 * sn-catalogue.c parses its embedded copy and hw/uefi/var-service-json.c
 * does. Reading the file avoids target/mips linkage and keeps this test
 * host-native, as direct sn-model.c linkage did before catalogue compilation.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-types-sgi-sn.h"
#include "qapi/qapi-visit-sgi-sn.h"
#include "qapi/qobject-input-visitor.h"
#include "qobject/qjson.h"
#include "qobject/qobject.h"

/*
 * CPU-slot and memory-bank limits are ASIC facts, formerly in
 * sgi_machine_asic_shape(). The Hub values live in sgi_sn0_hub_hardware, but
 * sn0.h includes target/mips/cpu.h and cannot link into this host test. Both
 * kinds have eight architectural banks; every node bank-mib array already has
 * that bound, so the literal adds no independent catalogue fact.
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

/* Whether two cable ends are the same port of the same vertex. */
static bool same_end(const SGIModelLinkEnd *a, const SGIModelLinkEnd *b)
{
    return a->kind == b->kind && a->index == b->index && a->port == b->port;
}

/* The @index'th cable of @t. */
static const SGIModelCable *nth_cable(const SGIModelTopology *t,
                                      uint32_t index)
{
    const SGIModelCableList *entry = t->cables;

    while (index--) {
        g_assert(entry);
        entry = entry->next;
    }
    g_assert(entry);
    return entry->value;
}

/*
 * Every cable's two ends name a real vertex, at a port at least one (port
 * zero is a crossbar's own local view, never a numbered cable end); no end
 * is shared by two cables one node-population row fits. The chassis lists
 * every row's cables, and two rows may wire one Hub differently (a
 * deskside's Null Router with two node boards, a Router port with three),
 * so the rule holds per row, not across the whole list. A router's own
 * port count is a device fact (decision D-0026) this catalogue does not
 * carry, so the upper bound on a router end's port is the device's own
 * qtest to check, not this one.
 */
static void check_cables(const SGIModelTopology *t)
{
    size_t total_nodes = node_count(t);
    size_t total_routers = router_count(t);
    size_t total_cables = 0;
    const SGIModelCableList *cable_entry;
    const SGIModelNodePopulationList *row;

    for (cable_entry = t->cables; cable_entry;
         cable_entry = cable_entry->next) {
        const SGIModelCable *cable = cable_entry->value;
        unsigned int side;

        g_assert(cable->ends->next);
        g_assert(!cable->ends->next->next);
        for (side = 0; side < 2; side++) {
            const SGIModelLinkEnd *end = side ? cable->ends->next->value :
                                                cable->ends->value;
            size_t count = end->kind == SGI_MODEL_LINK_END_KIND_ROUTER ?
                total_routers : total_nodes;

            g_assert_cmpuint(end->index, <, count);
            if (end->kind == SGI_MODEL_LINK_END_KIND_ROUTER) {
                g_assert_cmpuint(end->port, >=, 1);
            }
        }
        total_cables++;
    }

    g_assert(t->node_populations);
    for (row = t->node_populations; row; row = row->next) {
        const uint32List *fitted, *other;

        for (fitted = row->value->cables; fitted; fitted = fitted->next) {
            const SGIModelCable *cable;

            g_assert_cmpuint(fitted->value, <, total_cables);
            cable = nth_cable(t, fitted->value);
            for (other = row->value->cables; other != fitted;
                 other = other->next) {
                const SGIModelCable *peer = nth_cable(t, other->value);
                const SGIModelLinkEndList *a, *b;

                g_assert_cmpuint(other->value, !=, fitted->value);
                for (a = cable->ends; a; a = a->next) {
                    for (b = peer->ends; b; b = b->next) {
                        g_assert_false(same_end(a->value, b->value));
                    }
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
                      preset == SGI_MODEL_IO_BOARD_PRESET_IP29_IO_CROSSTOWN ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO6 ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO6G ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO8 ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_PCI_SHOEHORN ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_PCI_CARRIER ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_GIGACHANNEL ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_MENET ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_MSCSI ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_KONA ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_MGRAS);
    }
}

/*
 * Every node claims exactly one crosstalk attachment as its own, and the
 * remaining attachments own the topology's io-boards entries one for
 * one: the same split sgi_machine_index_xio_attachments() computes at
 * realize time. Hub topologies only -- sgi_machine_realize() builds a
 * Bedrock node and returns before that function ever runs, because a
 * Bedrock node's XBridge is not a separate crosstalk-attached board the
 * way a Hub's onboard or Crossbow-attached board is; a Bedrock
 * topology's io-boards entries are checked by
 * check_bedrock_has_populated_io8() below instead.
 */
static void check_xio_attachments(const SGIModelTopology *t)
{
    size_t total_nodes = node_count(t);
    size_t total_boards = io_board_count(t);
    g_autofree bool *claimed = g_new0(bool, total_nodes);
    const SGIModelXioAttachmentList *entry;
    unsigned int board = 0;
    size_t i;

    if (node0(t)->asic_kind != SGI_MODEL_ASIC_KIND_HUB) {
        return;
    }

    for (entry = t->xio_attachments; entry; entry = entry->next) {
        const SGIModelXioAttachment *attachment = entry->value;
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

static void check_xio_slots(const SGIModelTopology *t)
{
    size_t total_nodes = node_count(t);
    size_t total_crossbars = 0;
    const SGIModelCrossbarList *crossbar;
    const SGIModelXioSlotList *entry;

    for (crossbar = t->crossbars; crossbar; crossbar = crossbar->next) {
        total_crossbars++;
    }
    for (entry = t->xio_slots; entry; entry = entry->next) {
        const SGIModelXioSlot *slot = entry->value;
        const SGIModelXioSlotList *other;

        g_assert_cmpuint(slot->crossbar_index, <, total_crossbars);
        g_assert_cmpuint(slot->controlling_node, <, total_nodes);
        for (other = t->xio_slots; other != entry; other = other->next) {
            g_assert_cmpstr(slot->id, !=, other->value->id);
            g_assert_false(
                slot->crossbar_index == other->value->crossbar_index &&
                           slot->widget == other->value->widget);
        }
    }
}

/*
 * An Origin 2000 or Onyx2 node board sits in node slot 1 through 4. The
 * Hub reports what its midplane drives for that slot in a three-bit
 * field, and the builder takes slot 0 to mean the Origin 200's chassis,
 * which drives nothing; the stock PROM decodes that as node slot 3 and so
 * aims I/O at the wrong Crossbow port. A midplane that names its own
 * slot bits (the Onyx2 deskside) must fit that field.
 */
static void check_origin2000_node_slots(const SGIModelTopology *t)
{
    const SGIModelNodeList *entry;

    if (!g_str_equal(t->product, "origin2000") &&
        !g_str_equal(t->product, "onyx2")) {
        return;
    }
    for (entry = t->nodes; entry; entry = entry->next) {
        g_assert_cmpuint(entry->value->slot, >=, 1);
        g_assert_cmpuint(entry->value->slot, <=, 4);
        if (entry->value->has_hub_slot_bits) {
            g_assert_cmpuint(entry->value->hub_slot_bits, <=, 7);
        }
    }
}

/*
 * An xio-slots entry is an empty connector: no attachment may already
 * occupy its Crossbow widget, or a fitted board and an empty slot would
 * claim the same port.
 */
static void check_slots_are_unoccupied(const SGIModelTopology *t)
{
    const SGIModelXioSlotList *slot_entry;
    const SGIModelXioAttachmentList *attachment_entry;

    for (slot_entry = t->xio_slots; slot_entry;
         slot_entry = slot_entry->next) {
        const SGIModelXioSlot *slot = slot_entry->value;

        for (attachment_entry = t->xio_attachments; attachment_entry;
             attachment_entry = attachment_entry->next) {
            const SGIModelXioAttachment *attachment =
                attachment_entry->value;

            g_assert_false(attachment->crossbar_index ==
                           (int64_t)slot->crossbar_index &&
                           attachment->widget == slot->widget);
        }
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
        check_xio_attachments(t);
        check_xio_slots(t);
        check_origin2000_node_slots(t);
        check_slots_are_unoccupied(t);
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
