/* SPDX-License-Identifier: BSD-3-Clause */
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
#include "qapi/util.h"
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

/* The selected IP30 profiles have one CPU; this is not a HEART CPU limit. */
static void check_heart_node_shape(const SGIModelNode *node)
{
    const SGIModelBankLayoutList *layout;
    const uint64List *bank, *default_bank;
    uint64_t total = 0;

    g_assert_nonnull(node->heart);
    g_assert_null(node->identity);
    g_assert_false(node->has_hub_slot_bits);
    g_assert_cmpuint(node->heart->widget_id_64 >> 28, ==,
                     node->heart->widget_revision);
    g_assert_cmpuint(QAPI_LIST_LENGTH(node->cpu_slots), ==, 1);
    g_assert_cmpuint(node->cpu_slots->value->interface, ==, 0);
    g_assert_cmpuint(node->cpu_slots->value->slot, ==, 0);
    g_assert_nonnull(node->cpu_slots->value->assembly);
    g_assert_cmpuint(QAPI_LIST_LENGTH(node->bank_mib), ==, 4);
    for (bank = node->bank_mib; bank; bank = bank->next) {
        total += bank->value;
    }
    g_assert_cmpuint(total, >, 0);
    g_assert_cmpuint(QAPI_LIST_LENGTH(node->accepted_memory_mib), ==, 1);
    g_assert_cmpuint(node->accepted_memory_mib->value, ==, total);
    g_assert_true(node->has_accepted_bank_layouts);
    g_assert_nonnull(node->accepted_bank_layouts);
    for (layout = node->accepted_bank_layouts; layout; layout = layout->next) {
        uint64_t fitted = 0;
        bool empty = false;

        g_assert_cmpuint(QAPI_LIST_LENGTH(layout->value->bank_mib), ==, 4);
        for (bank = layout->value->bank_mib; bank; bank = bank->next) {
            g_assert_true(bank->value == 0 || bank->value == 64 ||
                          bank->value == 128 || bank->value == 256);
            if (bank->value) {
                g_assert_false(empty);
            } else {
                empty = true;
            }
            fitted += bank->value;
        }
        g_assert_cmpuint(fitted, ==, total);
    }
    default_bank = node->accepted_bank_layouts->value->bank_mib;
    for (bank = node->bank_mib; bank; bank = bank->next) {
        g_assert_cmpuint(bank->value, ==, default_bank->value);
        default_bank = default_bank->next;
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

        if (node->asic_kind == SGI_MODEL_ASIC_KIND_HEART) {
            g_assert_cmpint(t->generation, ==, SGI_MODEL_GENERATION_IP30);
            check_heart_node_shape(node);
            continue;
        }

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
                      preset == SGI_MODEL_IO_BOARD_PRESET_IP30 ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO6 ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO6G ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO8 ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_FUEL ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_PCI_SHOEHORN ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_PCI_CARRIER ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_GIGACHANNEL ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_MENET ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_MSCSI ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_KONA ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_MGRAS ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_ODYSSEY ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_VBRICK);
        if (preset != SGI_MODEL_IO_BOARD_PRESET_IO8 &&
            preset != SGI_MODEL_IO_BOARD_PRESET_FUEL) {
            g_assert_nonnull(entry->value->part);
            g_assert_true(entry->value->has_revision);
            g_assert_true(entry->value->has_serial);
        }
    }
}

/*
 * Every node claims exactly one crosstalk attachment as its own, and the
 * remaining attachments own the topology's io-boards entries one for
 * one: the same split sgi_machine_index_xio_attachments() computes at
 * realize time. Bedrock topologies are checked separately because their
 * XBridge heads are node parts. Both empty and fitted heads have io-boards
 * records, distinguished by populated.
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

/* IP30 routes HEART and its onboard Bridge through one Crossbow. */
static void check_heart_xio_attachments(const SGIModelTopology *t)
{
    const SGIModelXioAttachmentList *entry;
    unsigned int widgets = 0;

    if (node0(t)->asic_kind != SGI_MODEL_ASIC_KIND_HEART) {
        return;
    }
    g_assert_cmpuint(node_count(t), ==, 1);
    g_assert_null(t->routers);
    g_assert_null(t->cables);
    g_assert_null(t->xio_slots);
    g_assert_cmpuint(QAPI_LIST_LENGTH(t->crossbars), ==, 1);
    g_assert_cmpint(t->crossbars->value->kind, ==,
                    SGI_MODEL_CROSSBAR_KIND_XBOW);
    g_assert_cmpuint(io_board_count(t), ==, 1);
    g_assert_cmpint(t->io_boards->value->preset, ==,
                    SGI_MODEL_IO_BOARD_PRESET_IP30);
    g_assert_cmpint(t->io_boards->value->carrier, ==,
                    SGI_MODEL_IO_BOARD_CARRIER_ONBOARD);
    g_assert_true(t->io_boards->value->has_ioc3_serial);
    g_assert_cmpuint(QAPI_LIST_LENGTH(t->xio_attachments), ==, 2);
    for (entry = t->xio_attachments; entry; entry = entry->next) {
        const SGIModelXioAttachment *attachment = entry->value;

        g_assert_cmpint(attachment->crossbar_index, ==, 0);
        g_assert_cmpuint(attachment->node_index, ==, 0);
        g_assert_true(attachment->widget == 8 || attachment->widget == 15);
        g_assert_false(widgets & (1u << attachment->widget));
        widgets |= 1u << attachment->widget;
    }
    g_assert_cmpuint(widgets, ==, (1u << 8) | (1u << 15));
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
 * A Bedrock topology has one fitted base I/O board per node. Both XBridge
 * heads are present in xio-attachments, including the empty head.
 */
static void check_bedrock_has_populated_io(const SGIModelTopology *t)
{
    size_t nodes = node_count(t);
    g_autofree bool *seen = g_new0(bool, nodes);
    const SGIModelIoBoardList *entry;

    if (node0(t)->asic_kind != SGI_MODEL_ASIC_KIND_BEDROCK) {
        return;
    }
    for (entry = t->io_boards; entry; entry = entry->next) {
        const SGIModelXioAttachmentList *attachment = t->xio_attachments;
        unsigned int index = entry->value->attachment;

        if (!entry->value->populated ||
            (entry->value->preset != SGI_MODEL_IO_BOARD_PRESET_IO8 &&
             entry->value->preset != SGI_MODEL_IO_BOARD_PRESET_FUEL)) {
            continue;
        }
        while (index-- && attachment) {
            attachment = attachment->next;
        }
        g_assert_nonnull(attachment);
        g_assert_cmpint(attachment->value->crossbar_index, ==, -1);
        g_assert_cmpuint(attachment->value->node_index, <, nodes);
        g_assert_false(seen[attachment->value->node_index]);
        seen[attachment->value->node_index] = true;
    }
    for (size_t i = 0; i < nodes; i++) {
        g_assert_true(seen[i]);
    }
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
        check_heart_xio_attachments(t);
        check_xio_slots(t);
        check_origin2000_node_slots(t);
        check_slots_are_unoccupied(t);
        check_bedrock_has_populated_io(t);
    }
    g_assert_cmpuint(topology_count, >, 0);
}

static void test_origin300_graphics_attachment(gconstpointer data)
{
    const char *name = data;
    SGIModelIoBoardPreset preset = g_str_equal(name, "origin300-v12-direct") ?
        SGI_MODEL_IO_BOARD_PRESET_ODYSSEY : SGI_MODEL_IO_BOARD_PRESET_VBRICK;
    g_autoptr(SGIModelCatalogue) catalogue = load_catalogue();
    const SGIModelTopologyList *entry;

    for (entry = catalogue->topologies; entry; entry = entry->next) {
        const SGIModelTopology *t = entry->value;
        const SGIModelIoBoardList *board;
        const SGIModelXioAttachmentList *attachment;
        const unsigned int widgets[] = { 14, 15, 11 };
        unsigned int index = 0;
        unsigned int io8_count = 0, empty_count = 0, graphics_count = 0;

        if (!g_str_equal(t->name, name)) {
            continue;
        }
        g_assert_null(t->xio_slots);
        g_assert_cmpuint(io_board_count(t), ==, 3);
        for (attachment = t->xio_attachments; attachment;
             attachment = attachment->next, index++) {
            g_assert_cmpuint(index, <, G_N_ELEMENTS(widgets));
            g_assert_cmpint(attachment->value->crossbar_index, ==, -1);
            g_assert_cmpuint(attachment->value->node_index, ==, 0);
            g_assert_cmpuint(attachment->value->widget, ==, widgets[index]);
        }
        g_assert_cmpuint(index, ==, G_N_ELEMENTS(widgets));
        for (board = t->io_boards; board; board = board->next) {
            if (board->value->preset == SGI_MODEL_IO_BOARD_PRESET_IO8) {
                if (board->value->populated) {
                    g_assert_cmpuint(board->value->attachment, ==, 1);
                    g_assert_nonnull(board->value->pci_functions);
                    io8_count++;
                } else {
                    g_assert_cmpuint(board->value->attachment, ==, 0);
                    g_assert_null(board->value->pci_functions);
                    g_assert_null(board->value->system_timekeeper);
                    empty_count++;
                }
                continue;
            }
            g_assert_cmpint(board->value->preset, ==, preset);
            g_assert_true(board->value->populated);
            g_assert_cmpuint(board->value->attachment, ==, 2);
            graphics_count++;
        }
        g_assert_cmpuint(io8_count, ==, 1);
        g_assert_cmpuint(empty_count, ==, 1);
        g_assert_cmpuint(graphics_count, ==, 1);
        return;
    }
    g_error("%s is missing", name);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sn-model/every-topology-is-well-formed",
                    test_every_topology_is_well_formed);
    g_test_add_data_func("/sn-model/origin300-direct-v12-attachment",
                         "origin300-v12-direct",
                         test_origin300_graphics_attachment);
    g_test_add_data_func("/sn-model/origin300-vbrick-attachment",
                         "origin300-vbrick",
                         test_origin300_graphics_attachment);
    return g_test_run();
}
