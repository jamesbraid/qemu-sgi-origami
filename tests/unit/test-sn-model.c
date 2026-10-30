/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Check the shipped hw/mips/sgi/sgi-machines.json topology once; sn-machine.c
 * trusts the compiler and golden data at machine launch. Parse the tracked
 * file with qobject_from_json() and qobject_input_visitor_new(), as
 * sgi-machines.c parses its embedded copy and hw/uefi/var-service-json.c
 * does. Link the host-only runtime projector to test population remapping;
 * it has no target MIPS dependencies.
 */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qapi/clone-visitor.h"
#include "qapi/error.h"
#include "hw/mips/sgi/sgi-machines.h"
#include "qapi/qapi-types-sgi-machines.h"
#include "qapi/qapi-visit-sgi-machines.h"
#include "qapi/qobject-input-visitor.h"
#include "qapi/util.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qobject.h"

/*
 * CPU-slot and memory-bank limits are ASIC facts, formerly in
 * sgi_machine_asic_shape(). The Hub values live in sgi_sn0_hub_hardware, but
 * sn0.h includes target/mips/cpu.h and cannot link into this host test. Both
 * kinds have eight architectural banks; every node bank-mib array already has
 * that bound, so the literal adds no independent catalogue fact.
 */
#define SN_ARCHITECTURAL_MEMORY_BANKS 8

/* The catalogue population-view cache keeps pointers into its source model. */
static SGIModelTopology *cached_projection_fixture;

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

/* IP32 records are standalone boards, not module-owned Scalable Nodes. */
static void check_topology_ownership(const SGIModelTopology *t)
{
    const SGIModelNode *node;

    if (t->generation != SGI_MODEL_GENERATION_IP32) {
        check_modules_partition_nodes(t);
        return;
    }

    g_assert_cmpuint(node_count(t), ==, 1);
    g_assert_null(t->modules);
    g_assert_null(t->controllers);
    g_assert_null(t->io_peers);
    g_assert_null(t->io_assemblies);
    g_assert_null(t->routers);
    g_assert_null(t->cables);
    g_assert_null(t->crossbars);
    g_assert_null(t->xio_attachments);
    g_assert_null(t->xio_slots);
    g_assert_null(t->io_boards);
    node = node0(t);
    g_assert_cmpint(node->asic_kind, ==, SGI_MODEL_ASIC_KIND_CRIME);
    g_assert_false(node->has_module_index);
    g_assert_false(node->has_slot);
    g_assert_false(node->has_hub_slot_bits);
    g_assert_false(node->slave_role);
}

/* The selected O2 records fix eight 128 MiB banks, not CRIME's limits. */
static void check_crime_node_shape(const SGIModelNode *node)
{
    const uint64List *bank;

    g_assert_null(node->identity);
    g_assert_null(node->heart);
    g_assert_null(node->bedrock);
    g_assert_cmpuint(QAPI_LIST_LENGTH(node->cpu_slots), ==, 1);
    g_assert_cmpuint(node->cpu_slots->value->interface, ==, 0);
    g_assert_cmpuint(node->cpu_slots->value->slot, ==, 0);
    g_assert_nonnull(node->cpu_slots->value->assembly);
    g_assert_cmpuint(QAPI_LIST_LENGTH(node->bank_mib), ==, 8);
    for (bank = node->bank_mib; bank; bank = bank->next) {
        g_assert_cmpuint(bank->value, ==, 128);
    }
    g_assert_cmpuint(QAPI_LIST_LENGTH(node->accepted_memory_mib), ==, 1);
    g_assert_cmpuint(node->accepted_memory_mib->value, ==, 1024);
    g_assert_nonnull(node->flash);
    g_assert_cmpint(node->flash->kind, ==, SGI_MODEL_FLASH_KIND_OPERATOR_PROM);
    g_assert_cmpuint(node->flash->size, ==, 524288);
}

/* The selected IP30 profiles have one CPU; this is not a HEART CPU limit. */
static void check_heart_node_shape(const SGIModelNode *node)
{
    const uint64List *bank, *size;
    uint64_t total = 0, previous = 0;
    bool empty = false, found = false;

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
    g_assert_nonnull(node->accepted_bank_mib);
    g_assert_true(node->has_banks_descending);
    for (size = node->accepted_bank_mib; size; size = size->next) {
        /* A HEART size field names 32 MiB to 2 GiB; a pair is 64 MiB up. */
        g_assert_true(size->value >= 64 && size->value <= 2048 &&
                      is_power_of_2(size->value));
        g_assert_cmpuint(size->value, >, previous);
        previous = size->value;
    }
    previous = UINT64_MAX;
    for (bank = node->bank_mib; bank; bank = bank->next) {
        if (!bank->value) {
            empty = true;
            continue;
        }
        g_assert_false(empty);
        found = false;
        for (size = node->accepted_bank_mib; size; size = size->next) {
            found |= size->value == bank->value;
        }
        g_assert_true(found);
        g_assert_true(!node->banks_descending || bank->value <= previous);
        previous = bank->value;
        total += bank->value;
    }
    g_assert_cmpuint(total, >, 0);
    found = false;
    for (size = node->accepted_memory_mib; size; size = size->next) {
        found |= size->value == total;
    }
    g_assert_true(found);
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

        if (node->asic_kind == SGI_MODEL_ASIC_KIND_CRIME) {
            g_assert_cmpint(t->generation, ==, SGI_MODEL_GENERATION_IP32);
            check_crime_node_shape(node);
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
 * port count is a device fact this catalogue does not
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
                total_routers : end->kind == SGI_MODEL_LINK_END_KIND_IO_PEER ?
                QAPI_LIST_LENGTH(t->io_peers) : total_nodes;

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

        if (entry->value->carrier !=
            SGI_MODEL_IO_BOARD_CARRIER_MACHINE_PLACED) {
            g_assert_true(entry->value->has_attachment);
        }
        g_assert_true(preset == SGI_MODEL_IO_BOARD_PRESET_IP29_IO ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IP29_IO_CROSSTOWN ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IP30 ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO6 ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO6G ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO8 ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_FUEL ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO9 ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_PCI_SHOEHORN ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_PCI_CARRIER ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_GIGACHANNEL ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_MENET ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_MSCSI ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_KONA ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_MGRAS ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_ODYSSEY ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_VBRICK ||
                      preset == SGI_MODEL_IO_BOARD_PRESET_IO7);
        if (preset != SGI_MODEL_IO_BOARD_PRESET_IO8 &&
            preset != SGI_MODEL_IO_BOARD_PRESET_FUEL &&
            preset != SGI_MODEL_IO_BOARD_PRESET_IO9 &&
            preset != SGI_MODEL_IO_BOARD_PRESET_IO7) {
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
 * Bedrock topologies using IO8 or Fuel have one fitted base I/O board per
 * node. Both XBridge heads are present, including the empty head.
 */
static void check_bedrock_has_populated_io(const SGIModelTopology *t)
{
    size_t nodes = node_count(t);
    g_autofree bool *seen = g_new0(bool, nodes);
    const SGIModelIoBoardList *entry;
    bool checked = false;

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
        checked = true;
        while (index-- && attachment) {
            attachment = attachment->next;
        }
        g_assert_nonnull(attachment);
        g_assert_cmpint(attachment->value->crossbar_index, ==, -1);
        g_assert_cmpuint(attachment->value->node_index, <, nodes);
        g_assert_false(seen[attachment->value->node_index]);
        seen[attachment->value->node_index] = true;
    }
    if (!checked) {
        return;
    }
    for (size_t i = 0; i < nodes; i++) {
        g_assert_true(seen[i]);
    }
}

static char *catalogue_path(void)
{
    return g_build_filename(g_test_get_dir(G_TEST_DIST), "..", "..", "hw",
                            "mips", "sgi", "sgi-machines.json", NULL);
}

static SGIMachines *load_catalogue(void)
{
    g_autofree char *path = catalogue_path();
    g_autofree char *contents = NULL;
    g_autoptr(GError) local_error = NULL;
    QObject *qobj;
    Visitor *v;
    SGIMachines *catalogue = NULL;

    if (!g_file_get_contents(path, &contents, NULL, &local_error)) {
        g_error("cannot read %s: %s", path, local_error->message);
    }
    qobj = qobject_from_json(contents, &error_abort);
    v = qobject_input_visitor_new(qobj);
    visit_type_SGIMachines(v, NULL, &catalogue, &error_abort);
    visit_free(v);
    qobject_unref(qobj);
    return catalogue;
}

static SGIModelTopology *parse_topology(const char *json)
{
    QObject *qobj = qobject_from_json(json, &error_abort);
    Visitor *v = qobject_input_visitor_new(qobj);
    SGIModelTopology *topology = NULL;

    visit_type_SGIModelTopology(v, NULL, &topology, &error_abort);
    visit_free(v);
    qobject_unref(qobj);
    return topology;
}

static void test_optional_pci_board_references_parse(void)
{
    static const char legacy_json[] =
        "{\"name\":\"fixture-legacy\",\"product\":\"synthetic\","
        "\"generation\":\"sn1\",\"description\":\"fixture\","
        "\"nodes\":[],\"modules\":[],\"routers\":[],"
        "\"cables\":[],\"crossbars\":[],\"xio-attachments\":[],"
        "\"xio-slots\":[],\"io-boards\":[{\"id\":\"baseio\","
        "\"module-index\":0,\"preset\":\"ip29-io\","
        "\"carrier\":\"onboard\",\"attachment\":0,"
        "\"populated\":true}],\"controllers\":[],"
        "\"console\":0,\"junk-uart\":-1,\"node-populations\":[]}";
    static const char io9_json[] =
        "{\"name\":\"fixture-io9\",\"product\":\"synthetic\","
        "\"generation\":\"sn1\",\"description\":\"fixture\","
        "\"nodes\":[],\"modules\":[],\"routers\":[],"
        "\"cables\":[],\"crossbars\":[],"
        "\"pci-host-bridges\":[{\"id\":\"pic\",\"kind\":\"pic\","
        "\"xio-attachment-index\":0}],\"xio-attachments\":[],"
        "\"xio-slots\":[],\"io-boards\":[{\"id\":\"io9\","
        "\"module-index\":0,\"preset\":\"io9\","
        "\"carrier\":\"machine-placed\",\"pci-host-index\":0,"
        "\"pci-head\":1,\"populated\":false,"
        "\"pci-functions\":[{\"kind\":\"ioc4\",\"revision\":83,"
        "\"pci-device\":1,\"pci-function\":0,"
        "\"intx-pin\":\"inta\",\"pic-input\":0,"
        "\"clock-hz\":33333333}]}],"
        "\"controllers\":[],\"console\":0,\"junk-uart\":-1,"
        "\"node-populations\":[]}";
    SGIModelTopology *legacy = parse_topology(legacy_json);
    SGIModelTopology *io9 = parse_topology(io9_json);
    const SGIModelIoBoard *legacy_board = legacy->io_boards->value;
    const SGIModelIoBoard *io9_board = io9->io_boards->value;
    const SGIModelPciHostBridge *host;

    g_assert_false(legacy->has_pci_host_bridges);
    g_assert_true(legacy_board->has_attachment);
    g_assert_cmpuint(legacy_board->attachment, ==, 0);
    g_assert_false(legacy_board->has_pci_host_index);
    g_assert_false(legacy_board->has_pci_head);

    g_assert_true(io9->has_pci_host_bridges);
    g_assert_nonnull(io9->pci_host_bridges);
    host = io9->pci_host_bridges->value;
    g_assert_cmpstr(host->id, ==, "pic");
    g_assert_cmpint(host->kind, ==, SGI_MODEL_PCI_HOST_KIND_PIC);
    g_assert_cmpuint(host->xio_attachment_index, ==, 0);
    g_assert_false(io9_board->has_attachment);
    g_assert_true(io9_board->has_pci_host_index);
    g_assert_cmpuint(io9_board->pci_host_index, ==, 0);
    g_assert_true(io9_board->has_pci_head);
    g_assert_cmpuint(io9_board->pci_head, ==, 1);
    g_assert_nonnull(io9_board->pci_functions);
    g_assert_cmpint(io9_board->pci_functions->value->kind, ==,
                    SGI_MODEL_PCI_FUNCTION_KIND_IOC4);
    g_assert_true(io9_board->pci_functions->value->has_pci_device);
    g_assert_cmpuint(io9_board->pci_functions->value->pci_device, ==, 1);
    g_assert_true(io9_board->pci_functions->value->has_pci_function);
    g_assert_cmpuint(io9_board->pci_functions->value->pci_function, ==, 0);
    g_assert_true(io9_board->pci_functions->value->has_intx_pin);
    g_assert_cmpint(io9_board->pci_functions->value->intx_pin, ==,
                    SGI_MODEL_PCI_INTX_PIN_INTA);
    g_assert_true(io9_board->pci_functions->value->has_pic_input);
    g_assert_cmpuint(io9_board->pci_functions->value->pic_input, ==, 0);
    g_assert_true(io9_board->pci_functions->value->has_clock_hz);
    g_assert_cmpuint(io9_board->pci_functions->value->clock_hz, ==,
                     33333333);

    qapi_free_SGIModelTopology(legacy);
    qapi_free_SGIModelTopology(io9);
}

static void append_pci_host(SGIModelTopology *t, const char *id,
                            uint32_t attachment)
{
    SGIModelPciHostBridgeList **tail = &t->pci_host_bridges;
    SGIModelPciHostBridgeList *entry = g_new0(SGIModelPciHostBridgeList, 1);

    while (*tail) {
        tail = &(*tail)->next;
    }
    entry->value = g_new0(SGIModelPciHostBridge, 1);
    entry->value->id = g_strdup(id);
    entry->value->kind = SGI_MODEL_PCI_HOST_KIND_PIC;
    entry->value->xio_attachment_index = attachment;
    *tail = entry;
}

static void append_io9_board(SGIModelTopology *t, const char *id,
                             uint32_t host)
{
    SGIModelIoBoardList **tail = &t->io_boards;
    SGIModelIoBoardList *entry = g_new0(SGIModelIoBoardList, 1);

    while (*tail) {
        tail = &(*tail)->next;
    }
    entry->value = g_new0(SGIModelIoBoard, 1);
    entry->value->id = g_strdup(id);
    entry->value->module_index = 0;
    entry->value->preset = SGI_MODEL_IO_BOARD_PRESET_IO9;
    entry->value->carrier = SGI_MODEL_IO_BOARD_CARRIER_MACHINE_PLACED;
    entry->value->populated = true;
    entry->value->has_pci_host_index = true;
    entry->value->pci_host_index = host;
    entry->value->has_pci_head = true;
    entry->value->pci_head = 1;
    *tail = entry;
}

static void test_io9_hosts_and_boards_follow_projected_attachments(void)
{
    g_autoptr(SGIMachines) catalogue = load_catalogue();
    const SGIModelTopologyList *entry;
    SGIModelTopology *t = NULL;
    const SGIModelNodePopulation *row;
    const SGIModelTopology *view;
    const SGIModelPciHostBridgeList *host;
    const SGIModelXioAttachmentList *attachment;
    const SGIModelPciHostBridgeList *source_host;
    const SGIModelIoBoardList *board;
    const SGIModelIoBoardList *source_board;
    Error *err = NULL;
    size_t count;

    for (entry = catalogue->topologies; entry; entry = entry->next) {
        if (g_str_equal(entry->value->name, "origin2000-rack")) {
            t = QAPI_CLONE(SGIModelTopology, entry->value);
            break;
        }
    }
    g_assert_nonnull(t);
    row = t->node_populations->value;
    g_assert_cmpuint(QAPI_LIST_LENGTH(row->nodes), ==, 1);
    g_assert_cmpuint(row->nodes->value, ==, 0);

    /*
     * Attachment 1 belongs to node 2 and disappears in the one-node view.
     * The surviving hosts deliberately refer to attachments 2 then 0, so
     * their references expose both renumbering and preservation of order.
     */
    t->has_pci_host_bridges = true;
    append_pci_host(t, "removed-host", 1);
    append_pci_host(t, "host-attachment-2", 2);
    append_pci_host(t, "host-attachment-0", 0);
    append_io9_board(t, "removed-io9", 0);
    append_io9_board(t, "remapped-io9-0", 1);
    append_io9_board(t, "remapped-io9-1", 2);

    view = sgi_model_population_view(t, row, &err);
    g_assert_null(err);
    g_assert_nonnull(view);
    g_assert_cmpuint(QAPI_LIST_LENGTH(view->xio_attachments), ==, 2);
    attachment = view->xio_attachments;
    g_assert_cmpuint(attachment->value->widget, ==, 9);
    g_assert_cmpuint(attachment->next->value->widget, ==, 8);

    host = view->pci_host_bridges;
    g_assert_nonnull(host);
    g_assert_cmpstr(host->value->id, ==, "host-attachment-2");
    g_assert_cmpuint(host->value->xio_attachment_index, ==, 1);
    source_host = t->pci_host_bridges->next;
    g_assert_nonnull(source_host);
    g_assert_true(host->value != source_host->value);
    host = host->next;
    g_assert_nonnull(host);
    g_assert_cmpstr(host->value->id, ==, "host-attachment-0");
    g_assert_cmpuint(host->value->xio_attachment_index, ==, 0);
    source_host = source_host->next;
    g_assert_nonnull(source_host);
    g_assert_true(host->value == source_host->value);
    g_assert_null(host->next);

    board = view->io_boards;
    count = 0;
    for (; board; board = board->next) {
        const SGIModelIoBoard *candidate = board->value;

        if (g_str_equal(candidate->id, "removed-io9")) {
            g_assert_not_reached();
        }
        if (g_str_equal(candidate->id, "remapped-io9-0")) {
            g_assert_cmpuint(candidate->pci_host_index, ==, 0);
            for (source_board = t->io_boards; source_board;
                 source_board = source_board->next) {
                if (g_str_equal(source_board->value->id, candidate->id)) {
                    break;
                }
            }
            g_assert_nonnull(source_board);
            g_assert_true(candidate != source_board->value);
            count++;
        } else if (g_str_equal(candidate->id, "remapped-io9-1")) {
            g_assert_cmpuint(candidate->pci_host_index, ==, 1);
            for (source_board = t->io_boards; source_board;
                 source_board = source_board->next) {
                if (g_str_equal(source_board->value->id, candidate->id)) {
                    break;
                }
            }
            g_assert_nonnull(source_board);
            g_assert_true(candidate != source_board->value);
            count++;
        }
    }
    g_assert_cmpuint(count, ==, 2);
    /* Cached views share unchanged records with the source for process life. */
    g_assert_null(cached_projection_fixture);
    cached_projection_fixture = t;
}

static void test_every_topology_is_well_formed(void)
{
    g_autoptr(SGIMachines) catalogue = load_catalogue();
    const SGIModelTopologyList *entry;
    size_t topology_count = 0;

    for (entry = catalogue->topologies; entry; entry = entry->next) {
        const SGIModelTopology *t = entry->value;

        topology_count++;
        g_assert_cmpuint(node_count(t), >, 0);
        check_topology_ownership(t);
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

static void test_ip32_standalone_board(const void *data)
{
    const char *name = data;
    g_autoptr(SGIMachines) catalogue = load_catalogue();
    const SGIModelTopologyList *entry;
    const SGIModelTopology *topology = NULL;

    for (entry = catalogue->topologies; entry; entry = entry->next) {
        if (g_str_equal(entry->value->name, name)) {
            g_assert_null(topology);
            topology = entry->value;
        }
    }
    g_assert_nonnull(topology);
    g_assert_cmpint(topology->generation, ==, SGI_MODEL_GENERATION_IP32);
    check_topology_ownership(topology);
    check_node_shapes(topology);
    g_assert_cmpint(node0(topology)->asic_kind, ==, SGI_MODEL_ASIC_KIND_CRIME);
}

static void test_ip32_rejects_sn_ownership(const void *data)
{
    if (g_test_subprocess()) {
        SGIModelNode node = { .asic_kind = SGI_MODEL_ASIC_KIND_CRIME };
        SGIModelNodeList nodes = { .value = &node };
        SGIModelRouterList routers = { 0 };
        SGIModelTopology topology = {
            .generation = SGI_MODEL_GENERATION_IP32,
            .nodes = &nodes,
        };

        if (GPOINTER_TO_UINT(data)) {
            topology.routers = &routers;
        } else {
            node.has_module_index = true;
        }
        check_topology_ownership(&topology);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*check_topology_ownership*");
}

/* A standalone-board exception must not permit unowned modular nodes. */
static void test_modular_node_requires_owner(const void *data)
{
    if (g_test_subprocess()) {
        SGIModelNode node = { 0 };
        SGIModelNodeList nodes = { .value = &node };
        SGIModelTopology topology = {
            .generation = GPOINTER_TO_UINT(data),
            .nodes = &nodes,
        };

        check_topology_ownership(&topology);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*check_modules_partition_nodes*claimed[i]*");
}

/* These identities are synthetic parser data, not a fitted hardware profile. */
#define IO_PEER_JSON \
    "{ 'id': 'io-bedrock', 'module-index': 1, 'slot': 0," \
    " 'asic-kind': 'bedrock', 'identity': { 'ni-revision': 1," \
    " 'widget-revision': 1, 'serial': 1193046," \
    " 'i2c-controller': false, 'system-controller': false } }"

static void test_io_peer_descriptor(void)
{
    g_autoptr(SGIModelIoPeer) peer = NULL;
    g_autoptr(SGIModelModule) module = NULL;
    g_autoptr(SGIModelLinkEnd) end = NULL;
    QObject *object = qobject_from_json(IO_PEER_JSON, &error_abort);
    Visitor *v = qobject_input_visitor_new(object);

    visit_type_SGIModelIoPeer(v, NULL, &peer, &error_abort);
    visit_free(v);
    g_assert_cmpstr(peer->id, ==, "io-bedrock");
    g_assert_cmpuint(peer->module_index, ==, 1);
    g_assert_cmpuint(peer->slot, ==, 0);
    g_assert_cmpint(peer->asic_kind, ==, SGI_MODEL_ASIC_KIND_BEDROCK);
    g_assert_cmpuint(peer->identity->serial, ==, 1193046);

    qobject_unref(object);
    object = qobject_from_json(
        "{ 'id': 'io-module', 'serial': 'SYNTHETIC',"
        " 'initial-module-number': 2, 'controller-index': -1,"
        " 'nodes': [], 'io-peers': [0] }", &error_abort);
    v = qobject_input_visitor_new(object);
    visit_type_SGIModelModule(v, NULL, &module, &error_abort);
    visit_free(v);
    g_assert_null(module->nodes);
    g_assert_true(module->has_io_peers);
    g_assert_cmpuint(module->io_peers->value, ==, 0);
    g_assert_null(module->io_peers->next);

    qobject_unref(object);
    object = qobject_from_json(
        "{ 'kind': 'io-peer', 'index': 0, 'port': 0 }", &error_abort);
    v = qobject_input_visitor_new(object);
    visit_type_SGIModelLinkEnd(v, NULL, &end, &error_abort);
    visit_free(v);
    g_assert_cmpint(end->kind, ==, SGI_MODEL_LINK_END_KIND_IO_PEER);
    g_assert_cmpuint(end->index, ==, 0);
    g_assert_cmpuint(end->port, ==, 0);
    qobject_unref(object);
}

static void test_io_peer_requires_identity(void)
{
    g_autoptr(SGIModelIoPeer) peer = NULL;
    Error *err = NULL;
    QObject *object = qobject_from_json(
        "{ 'id': 'io-bedrock', 'module-index': 1, 'slot': 0,"
        " 'asic-kind': 'bedrock' }", &error_abort);
    Visitor *v = qobject_input_visitor_new(object);

    visit_type_SGIModelIoPeer(v, NULL, &peer, &err);
    visit_free(v);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "identity"));
    error_free(err);
    qobject_unref(object);
}

static void test_io_peer_rejects_processors(void)
{
    g_autoptr(SGIModelIoPeer) peer = NULL;
    Error *err = NULL;
    QObject *object = qobject_from_json(IO_PEER_JSON, &error_abort);
    QDict *dict = qobject_to(QDict, object);
    Visitor *v;

    qdict_put_obj(dict, "cpu-slots", QOBJECT(qlist_new()));
    v = qobject_input_visitor_new(object);
    visit_type_SGIModelIoPeer(v, NULL, &peer, &err);
    visit_free(v);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "cpu-slots"));
    error_free(err);
    qobject_unref(object);
}

static void test_existing_topologies_have_no_io_peers(void)
{
    g_autoptr(SGIMachines) catalogue = load_catalogue();
    const SGIModelTopologyList *entry;

    for (entry = catalogue->topologies; entry; entry = entry->next) {
        const SGIModelModuleList *module;

        if (g_str_equal(entry->value->name,
                        "origin300-numalink-pci-expansion") ||
            g_str_equal(entry->value->name, "origin300-pci-expansion")) {
            continue;
        }
        g_assert_false(entry->value->has_io_peers);
        g_assert_null(entry->value->io_peers);
        for (module = entry->value->modules; module; module = module->next) {
            g_assert_false(module->value->has_io_peers);
            g_assert_null(module->value->io_peers);
        }
    }
}

static void test_expansion_offering(void)
{
    g_autoptr(SGIMachines) catalogue = load_catalogue();
    const SGIModelTopologyList *entry;
    const SGIModelTopology *t = NULL;
    SGIModelIoAssembly *a;
    const uint8List *port;
    static const uint8_t ports[] = { 8, 9, 12, 13 };
    unsigned int index = 0;
    Error *err = NULL;

    for (entry = catalogue->topologies; entry; entry = entry->next) {
        if (g_str_equal(entry->value->name,
                        "origin300-pci-expansion")) {
            t = entry->value;
            break;
        }
    }
    g_assert_nonnull(t);
    g_assert_cmpuint(node_count(t), ==, 1);
    g_assert_cmpuint(QAPI_LIST_LENGTH(t->io_peers), ==, 1);
    g_assert_cmpuint(QAPI_LIST_LENGTH(t->io_assemblies), ==, 1);
    g_assert_cmpuint(QAPI_LIST_LENGTH(t->controllers), ==, 2);
    a = t->io_assemblies->value;
    g_assert_cmpuint(a->placement, ==, 1);
    g_assert_cmpuint(a->initial_node_number, ==, 0);
    g_assert_cmpuint(a->bedrock_revision, ==, 2);
    g_assert_cmpuint(a->xbridge_revision, ==, 2);
    g_assert_cmphex(a->bic_identity, ==, 0xc000);
    g_assert_true(a->has_main_power);
    g_assert_cmpint(a->main_power, ==,
                    SGI_MODEL_MAIN_POWER_FUNCTIONAL_EXPERIMENT);
    g_assert_true(a->has_initial_main_power);
    g_assert_true(a->initial_main_power);
    g_assert_true(a->has_power_command_issuer);
    g_assert_cmpuint(a->power_command_issuer, ==, 0);
    g_assert_false(a->has_initial_latch);
    g_assert_true(a->has_intx_map);
    g_assert_cmpuint(QAPI_LIST_LENGTH(a->intx_map), ==, 48);
    for (port = a->head_ports; port; port = port->next, index++) {
        g_assert_cmpuint(index, <, G_N_ELEMENTS(ports));
        g_assert_cmpuint(port->value, ==, ports[index]);
    }
    g_assert_cmpuint(index, ==, G_N_ELEMENTS(ports));
    g_assert_nonnull(t->l1_management);
    g_assert_cmpuint(QAPI_LIST_LENGTH(t->l1_management->owners), ==, 2);
    g_assert_cmpuint(QAPI_LIST_LENGTH(t->l1_management->associations), ==, 2);
    g_assert_cmpuint(QAPI_LIST_LENGTH(t->l1_management->attachments), ==, 2);
    g_assert_true(sgi_model_check_runtime_endpoints(t, &err));
    g_assert_null(err);
    a->has_initial_main_power = false;
    g_assert_false(sgi_model_check_runtime_endpoints(t, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    a->has_initial_main_power = true;
    a->has_main_power = false;
    g_assert_false(sgi_model_check_runtime_endpoints(t, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    a->has_main_power = true;
    g_assert_true(sgi_model_check_runtime_endpoints(t, &err));
    g_assert_null(err);
}

static void test_expansion_storage_offering(void)
{
    g_autoptr(SGIMachines) catalogue = load_catalogue();
    SGIModelTopologyList *entry;
    SGIModelTopology *topology = NULL;
    SGIModelIoAssembly *assembly;
    SGIModelIoAssemblyList duplicate;
    Error *error = NULL;
    uint32_t owner;

    for (entry = catalogue->topologies; entry; entry = entry->next) {
        if (g_str_equal(entry->value->name, "origin300-pci-expansion")) {
            topology = entry->value;
            break;
        }
    }
    g_assert_nonnull(topology);
    assembly = topology->io_assemblies->value;
    g_assert_true(assembly->has_configuration_storage);
    g_assert_cmpint(assembly->configuration_storage, ==,
                    SGI_MODEL_CONFIGURATION_STORAGE_BEDROCK_FLASH_EXPERIMENT);
    g_assert_true(sgi_model_check_runtime_endpoints(topology, &error));
    g_assert_null(error);

    assembly->has_main_power = false;
    assembly->has_initial_main_power = false;
    g_assert_false(sgi_model_check_runtime_endpoints(topology, &error));
    g_assert_nonnull(error);
    error_free(error);
    error = NULL;
    assembly->has_main_power = true;
    assembly->has_initial_main_power = true;
    assembly->configuration_storage = SGI_MODEL_CONFIGURATION_STORAGE__MAX;
    g_assert_false(sgi_model_check_runtime_endpoints(topology, &error));
    g_assert_nonnull(error);
    error_free(error);
    error = NULL;
    assembly->has_configuration_storage = false;
    g_assert_true(sgi_model_check_runtime_endpoints(topology, &error));
    g_assert_null(error);
    assembly->has_configuration_storage = true;
    assembly->configuration_storage =
        SGI_MODEL_CONFIGURATION_STORAGE_BEDROCK_FLASH_EXPERIMENT;

    owner = assembly->module_index;
    assembly->module_index = 0;
    g_assert_false(sgi_model_check_runtime_endpoints(topology, &error));
    g_assert_nonnull(error);
    error_free(error);
    error = NULL;
    assembly->module_index = owner;
    owner = assembly->controller_index;
    assembly->controller_index = 0;
    g_assert_false(sgi_model_check_runtime_endpoints(topology, &error));
    g_assert_nonnull(error);
    error_free(error);
    error = NULL;
    assembly->controller_index = owner;
    duplicate = (SGIModelIoAssemblyList) { .value = assembly };
    topology->io_assemblies->next = &duplicate;
    g_assert_false(sgi_model_check_runtime_endpoints(topology, &error));
    g_assert_nonnull(error);
    error_free(error);
    topology->io_assemblies->next = NULL;
}

static void test_io_peer_runtime_refusal(const void *data)
{
    char name[] = "synthetic-io-peer";
    SGIModelTopology topology = { .name = name };
    SGIModelIoPeer peer = { 0 };
    SGIModelIoPeerList peers = { .value = &peer };
    uint32List membership = { .value = 0 };
    SGIModelModule module = { .has_io_peers = true,
                              .io_peers = &membership };
    SGIModelModuleList modules = { .value = &module };
    SGIModelLinkEnd end = { .kind = SGI_MODEL_LINK_END_KIND_IO_PEER };
    SGIModelLinkEndList ends = { .value = &end };
    SGIModelCable cable = { .ends = &ends };
    SGIModelCableList cables = { .value = &cable };
    Error *err = NULL;

    switch (GPOINTER_TO_UINT(data)) {
    case 0:
        topology.has_io_peers = true;
        topology.io_peers = &peers;
        break;
    case 1:
        /* Module membership must not silently survive without its peers. */
        topology.modules = &modules;
        break;
    case 2:
        /* An orphan cable end must not be interpreted as a Router. */
        topology.cables = &cables;
        break;
    default:
        g_assert_not_reached();
    }
    g_assert_false(sgi_model_check_runtime_endpoints(&topology, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), topology.name));
    g_assert_nonnull(strstr(error_get_pretty(err),
                            "unsupported runtime endpoint"));
    error_free(err);
}

typedef struct PeerPopulationFixture {
    SGIModelTopology topology;
    SGIModelNode nodes[2];
    SGIModelNodeList node_list[2];
    SGIModelIoPeer peer;
    SGIModelIoPeerList peers;
    SGIModelModule modules[2];
    SGIModelModuleList module_list[2];
    SGIModelController controller;
    SGIModelControllerList controllers;
    SGIModelCable cable;
    SGIModelCableList cables;
    SGIModelLinkEnd ends[2];
    SGIModelLinkEndList end_list[2];
    SGIModelNodePopulation row;
    uint32List row_nodes[2];
    uint32List row_cables;
    uint32List membership;
} PeerPopulationFixture;

static PeerPopulationFixture *peer_population_fixture(void)
{
    /* Population views retain pointers to their input records. */
    PeerPopulationFixture *f = g_new0(PeerPopulationFixture, 1);

    f->node_list[0] = (SGIModelNodeList) {
        .value = &f->nodes[0], .next = &f->node_list[1],
    };
    f->node_list[1].value = &f->nodes[1];
    f->peer.module_index = 1;
    f->peers.value = &f->peer;
    f->membership.value = 0;
    f->modules[0].controller_index = -1;
    f->modules[1] = (SGIModelModule) {
        .has_io_peers = true, .io_peers = &f->membership,
        .controller_index = 0,
    };
    f->module_list[0] = (SGIModelModuleList) {
        .value = &f->modules[0], .next = &f->module_list[1],
    };
    f->module_list[1].value = &f->modules[1];
    f->controller.module_index = 1;
    f->controllers.value = &f->controller;
    f->ends[0].kind = SGI_MODEL_LINK_END_KIND_NODE;
    f->ends[1].kind = SGI_MODEL_LINK_END_KIND_IO_PEER;
    f->end_list[0] = (SGIModelLinkEndList) {
        .value = &f->ends[0], .next = &f->end_list[1],
    };
    f->end_list[1].value = &f->ends[1];
    f->cable.ends = &f->end_list[0];
    f->cables.value = &f->cable;
    f->row_nodes[0] = (uint32List) {
        .value = 0, .next = &f->row_nodes[1],
    };
    f->row_nodes[1].value = 1;
    f->row.nodes = &f->row_nodes[0];
    f->row.cables = &f->row_cables;
    f->topology = (SGIModelTopology) {
        .name = (char *)"peer-population", .nodes = &f->node_list[0],
        .has_io_peers = true, .io_peers = &f->peers,
        .modules = &f->module_list[0], .controllers = &f->controllers,
        .cables = &f->cables,
    };
    return f;
}

/*
 * A synthetic chain, with no product claim: n0 cabled to crossbar 1, n1
 * to crossbar 2, crossbar 2 to crossbar 3 and crossbar 3 to the one
 * board. Crossbar 0 is reached by nothing.
 */
#define XIO_CABLE_NODE(id, module, role) \
    "{\"id\":\"" id "\",\"module-index\":" module ",\"slot\":0," \
    "\"asic-kind\":\"bedrock\",\"board-kind\":\"ip35\"," role \
    "\"cpu-slots\":[],\"bank-mib\":[512,0,0,0,0,0,0,0]," \
    "\"accepted-memory-mib\":[],\"slave-role\":false}"
#define XIO_CABLE_CROSSBAR(id) \
    "{\"id\":\"" id "\",\"module-index\":2,\"kind\":\"xbridge\"," \
    "\"widget-revision\":1}"
#define XIO_CABLE_END(kind, index, port) \
    "{\"kind\":\"" kind "\",\"index\":" index port "}"
#define XIO_CABLE_ROW(nodes) \
    "{\"nodes\":[" nodes "],\"routers\":[],\"cables\":[]," \
    "\"xio-slots\":[],\"supported-cpu-populations\":[]," \
    "\"grade\":\"established\"}"

static const char xio_cable_view_json[] =
    "{\"name\":\"xio-cable-view\",\"product\":\"synthetic\","
    "\"generation\":\"sn1\",\"description\":\"XIO cable view fixture\","
    "\"nodes\":["
    XIO_CABLE_NODE("n0", "0", "") ","
    XIO_CABLE_NODE("n1", "1", "\"role\":\"memory\",") "],"
    "\"modules\":["
    "{\"id\":\"m0\",\"serial\":\"\",\"initial-module-number\":0,"
    "\"controller-index\":0,\"brick-type\":\"C\",\"nodes\":[0]},"
    "{\"id\":\"m1\",\"serial\":\"\",\"initial-module-number\":0,"
    "\"controller-index\":-1,\"brick-type\":\"C\",\"nodes\":[1]},"
    "{\"id\":\"m2\",\"serial\":\"\",\"initial-module-number\":0,"
    "\"controller-index\":-1,\"controller\":\"absent\",\"nodes\":[]}],"
    "\"routers\":[],\"cables\":[],"
    "\"crossbars\":["
    XIO_CABLE_CROSSBAR("x0") "," XIO_CABLE_CROSSBAR("x1") ","
    XIO_CABLE_CROSSBAR("x2") "," XIO_CABLE_CROSSBAR("x3") "],"
    "\"xio-attachments\":[],"
    "\"xio-cables\":["
    "{\"name\":\"n0-x1\",\"ends\":[" XIO_CABLE_END("node", "0", "") ","
    XIO_CABLE_END("crossbar", "1", ",\"port\":10") "]},"
    "{\"name\":\"n1-x2\",\"ends\":[" XIO_CABLE_END("node", "1", "") ","
    XIO_CABLE_END("crossbar", "2", ",\"port\":10") "]},"
    "{\"name\":\"x2-x3\",\"ends\":["
    XIO_CABLE_END("crossbar", "2", ",\"port\":8") ","
    XIO_CABLE_END("crossbar", "3", ",\"port\":8") "]},"
    "{\"name\":\"x3-board\",\"ends\":["
    XIO_CABLE_END("crossbar", "3", ",\"port\":9") ","
    XIO_CABLE_END("io-board", "0", "") "]}],"
    "\"xio-slots\":[],"
    "\"io-boards\":[{\"id\":\"board\",\"module-index\":2,"
    "\"preset\":\"io7\",\"carrier\":\"machine-placed\","
    "\"populated\":true}],"
    "\"controllers\":[{\"id\":\"l1\",\"module-index\":0,\"kind\":\"l1\","
    "\"owner\":\"module\"}],"
    "\"console\":1,\"junk-uart\":-1,"
    "\"node-populations\":[" XIO_CABLE_ROW("0,1") ","
    XIO_CABLE_ROW("1") "]}";

static void assert_xio_end(const SGIModelXioEnd *end, SGIModelXioEndKind kind,
                           uint32_t index, int port)
{
    g_assert_cmpint(end->kind, ==, kind);
    g_assert_cmpuint(end->index, ==, index);
    g_assert_cmpint(end->has_port, ==, port >= 0);
    if (port >= 0) {
        g_assert_cmpuint(end->port, ==, port);
    }
}

static const SGIModelXioEnd *xio_cable_end(const SGIModelXioCableList *cables,
                                           size_t cable, size_t side)
{
    const SGIModelXioEndList *ends;

    while (cable--) {
        cables = cables->next;
    }
    ends = cables->value->ends;
    return side ? ends->next->value : ends->value;
}

static void test_xio_cable_view_copies_new_records(void)
{
    /* Population views keep pointers into this topology for process life. */
    SGIModelTopology *t = parse_topology(xio_cable_view_json);
    const SGIModelTopology *view;
    const SGIModelXioCableList *cables;
    Error *err = NULL;

    view = sgi_model_population_view(t, t->node_populations->value, &err);
    g_assert_null(err);
    g_assert_true(view->has_xio_cables);
    cables = view->xio_cables;
    g_assert_cmpuint(QAPI_LIST_LENGTH(cables), ==, 4);
    g_assert_cmpstr(cables->value->name, ==, "n0-x1");
    /* Crossbar 0 has no cable to a kept node, so it is not in the view. */
    g_assert_cmpuint(QAPI_LIST_LENGTH(view->crossbars), ==, 3);
    assert_xio_end(xio_cable_end(cables, 0, 0), SGI_MODEL_XIO_END_KIND_NODE,
                   0, -1);
    assert_xio_end(xio_cable_end(cables, 0, 1),
                   SGI_MODEL_XIO_END_KIND_CROSSBAR, 0, 10);
    assert_xio_end(xio_cable_end(cables, 1, 1),
                   SGI_MODEL_XIO_END_KIND_CROSSBAR, 1, 10);
    assert_xio_end(xio_cable_end(cables, 2, 1),
                   SGI_MODEL_XIO_END_KIND_CROSSBAR, 2, 8);
    assert_xio_end(xio_cable_end(cables, 3, 1),
                   SGI_MODEL_XIO_END_KIND_IO_BOARD, 0, -1);
    g_assert_cmpstr(view->modules->value->brick_type, ==, "C");
    g_assert_true(view->modules->next->next->value->has_controller);
    g_assert_cmpint(view->modules->next->next->value->controller, ==,
                    SGI_MODEL_MODULE_CONTROLLER_ABSENT);
    g_assert_true(view->nodes->next->value->has_role);
    g_assert_cmpint(view->nodes->next->value->role, ==,
                    SGI_MODEL_NODE_ROLE_MEMORY);
    g_assert_cmpint(view->nodes->value->board_kind, ==,
                    SGI_MODEL_BOARD_KIND_IP35);
    g_assert_null(view->nodes->value->flash);
    g_assert_cmpint(view->controllers->value->owner, ==,
                    SGI_MODEL_CONTROLLER_OWNER_MODULE);
    g_assert_cmpint(view->io_boards->value->preset, ==,
                    SGI_MODEL_IO_BOARD_PRESET_IO7);

    /* Without n0 its cable and crossbar go; the rest renumber. */
    view = sgi_model_population_view(t, t->node_populations->next->value,
                                     &err);
    g_assert_null(err);
    cables = view->xio_cables;
    g_assert_cmpuint(QAPI_LIST_LENGTH(cables), ==, 3);
    g_assert_cmpuint(QAPI_LIST_LENGTH(view->crossbars), ==, 2);
    g_assert_cmpstr(cables->value->name, ==, "n1-x2");
    assert_xio_end(xio_cable_end(cables, 0, 0), SGI_MODEL_XIO_END_KIND_NODE,
                   0, -1);
    assert_xio_end(xio_cable_end(cables, 0, 1),
                   SGI_MODEL_XIO_END_KIND_CROSSBAR, 0, 10);
    assert_xio_end(xio_cable_end(cables, 1, 0),
                   SGI_MODEL_XIO_END_KIND_CROSSBAR, 0, 8);
    assert_xio_end(xio_cable_end(cables, 1, 1),
                   SGI_MODEL_XIO_END_KIND_CROSSBAR, 1, 8);
    assert_xio_end(xio_cable_end(cables, 2, 0),
                   SGI_MODEL_XIO_END_KIND_CROSSBAR, 1, 9);
    g_assert_cmpstr(view->modules->next->value->brick_type, ==, "C");
}

/*
 * A synthetic R-brick shape, with no product claim: n0 and n1 cabled to
 * ports 2 and 3 of one router whose record names its node ports.
 */
#define ROUTER_VIEW_MODULE(id, controller, nodes) \
    "{\"id\":\"" id "\",\"serial\":\"\",\"initial-module-number\":0," \
    "\"controller-index\":" controller ",\"nodes\":[" nodes "]}"
#define ROUTER_VIEW_CABLE(name, node, port) \
    "{\"name\":\"" name "\",\"kind\":\"cable\",\"ends\":[" \
    "{\"kind\":\"node\",\"index\":" node ",\"port\":0}," \
    "{\"kind\":\"router\",\"index\":0,\"port\":" port "}]}"
#define ROUTER_VIEW_L1(id, module) \
    "{\"id\":\"" id "\",\"module-index\":" module ",\"kind\":\"l1\"}"

static const char router_view_json[] =
    "{\"name\":\"router-view\",\"product\":\"synthetic\","
    "\"generation\":\"sn1\",\"description\":\"router view fixture\","
    "\"nodes\":["
    XIO_CABLE_NODE("n0", "0", "") "," XIO_CABLE_NODE("n1", "1", "") "],"
    "\"modules\":["
    ROUTER_VIEW_MODULE("m0", "0", "0") ","
    ROUTER_VIEW_MODULE("m1", "1", "1") ","
    ROUTER_VIEW_MODULE("router", "2", "") "],"
    "\"routers\":[{\"id\":\"r\",\"module-index\":2,\"slot\":0,"
    "\"part\":12311,\"revision\":1,\"chip-inputs\":0,\"serial\":1,"
    "\"node-ports\":[2,3,4,5]}],"
    "\"cables\":[" ROUTER_VIEW_CABLE("n0-r", "0", "2") ","
    ROUTER_VIEW_CABLE("n1-r", "1", "3") "],"
    "\"crossbars\":[],\"xio-attachments\":[],\"xio-slots\":[],"
    "\"io-boards\":[],"
    "\"controllers\":[" ROUTER_VIEW_L1("l1-0", "0") ","
    ROUTER_VIEW_L1("l1-1", "1") "," ROUTER_VIEW_L1("l1-r", "2") "],"
    "\"console\":0,\"junk-uart\":-1,"
    "\"node-populations\":[{\"nodes\":[0,1],\"routers\":[0],"
    "\"cables\":[0,1],\"xio-slots\":[],\"supported-cpu-populations\":[],"
    "\"grade\":\"established\"}]}";

static void test_router_view_keeps_node_ports(void)
{
    /* Population views keep pointers into this topology for process life. */
    SGIModelTopology *t = parse_topology(router_view_json);
    const SGIModelTopology *view;
    const uint8List *port;
    unsigned int expected = 2;
    Error *err = NULL;

    view = sgi_model_population_view(t, t->node_populations->value, &err);
    g_assert_null(err);
    g_assert_cmpuint(QAPI_LIST_LENGTH(view->routers), ==, 1);
    g_assert_cmpuint(QAPI_LIST_LENGTH(view->routers->value->node_ports), ==,
                     4);
    for (port = view->routers->value->node_ports; port; port = port->next) {
        g_assert_cmpuint(port->value, ==, expected++);
    }
    g_assert_cmpuint(QAPI_LIST_LENGTH(view->cables), ==, 2);
}

static void test_xio_cable_view_refuses_bad_ends(void)
{
    static const char *const bad[] = { "\"port\":10", "\"index\":7" };

    for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
        g_autofree char *json = NULL;
        g_autoptr(SGIModelTopology) t = NULL;
        const char *needle = "{\"kind\":\"node\",\"index\":0}";
        const char *at = strstr(xio_cable_view_json, needle);
        Error *err = NULL;

        g_assert_nonnull(at);
        json = g_strdup_printf("%.*s{\"kind\":\"node\",%s%s}%s",
                               (int)(at - xio_cable_view_json),
                               xio_cable_view_json, bad[i],
                               i ? "" : ",\"index\":0",
                               at + strlen(needle));
        t = parse_topology(json);
        g_assert_null(sgi_model_population_view(t, t->node_populations->value,
                                                &err));
        g_assert_nonnull(err);
        g_assert_nonnull(strstr(error_get_pretty(err), "XIO cable 'n0-x1'"));
        error_free(err);
    }
}

static void test_l1_peer_population_preserved(void)
{
    /* The population cache borrows these records for process life. */
    static SGIMachines *catalogue;
    const SGIModelTopologyList *entry;

    catalogue = load_catalogue();

    for (entry = catalogue->topologies; entry; entry = entry->next) {
        const SGIModelTopology *t = entry->value;
        const SGIModelNodePopulationList *row;

        if (!g_str_equal(t->name, "origin300-two-chassis")) {
            continue;
        }
        g_assert_true(t->has_l1_peer_links);
        g_assert_cmpuint(QAPI_LIST_LENGTH(t->l1_peer_links), ==, 2);
        for (row = t->node_populations; row; row = row->next) {
            Error *err = NULL;
            const SGIModelTopology *view =
                sgi_model_population_view(t, row->value, &err);

            g_assert_null(err);
            g_assert_nonnull(view);
            g_assert_true(view->has_l1_peer_links);
            g_assert_true(view->l1_peer_links == t->l1_peer_links);
            g_assert_true(view->controllers == t->controllers);
        }
        return;
    }
    g_assert_not_reached();
}

static void test_io_peer_population_preserved(void)
{
    PeerPopulationFixture *f = peer_population_fixture();
    const SGIModelTopology *view;
    Error *err = NULL;

    view = sgi_model_population_view(&f->topology, &f->row, &err);
    g_assert_null(err);
    g_assert_nonnull(view);
    g_assert_true(view->has_io_peers);
    g_assert_true(view->io_peers == &f->peers);
    g_assert_true(view->controllers == &f->controllers);
    g_assert_true(view->modules->next->value->has_io_peers);
    g_assert_cmpuint(view->modules->next->value->io_peers->value, ==, 0);
    g_assert_cmpint(view->modules->next->value->controller_index, ==, 0);
    g_assert_cmpint(view->cables->value->ends->next->value->kind, ==,
                    SGI_MODEL_LINK_END_KIND_IO_PEER);
    g_assert_cmpuint(view->cables->value->ends->next->value->index, ==, 0);
}

static void test_io_peer_population_rejects_reduced(void)
{
    PeerPopulationFixture *f = peer_population_fixture();
    Error *err = NULL;

    f->row_nodes[0].next = NULL;
    g_assert_null(sgi_model_population_view(&f->topology, &f->row, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "complete population"));
    error_free(err);
}

static void test_io_peer_population_rejects_missing_cable(void)
{
    PeerPopulationFixture *f = peer_population_fixture();
    Error *err = NULL;

    f->row.cables = NULL;
    g_assert_null(sgi_model_population_view(&f->topology, &f->row, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "complete population"));
    error_free(err);
}

static void test_tezro_requires_explicit_topology(void)
{
    g_autoptr(SGIMachines) catalogue = load_catalogue();
    const SGIModelTopologyList *topology;
    const SGIModelOfferingList *offering;
    bool tower_found = false;
    bool tezro_found = false;

    for (topology = catalogue->topologies; topology;
         topology = topology->next) {
        if (!strcmp(topology->value->name, "tezro-tower-multi")) {
            tower_found = true;
        }
    }
    g_assert_true(tower_found);
    for (offering = catalogue->offerings; offering;
         offering = offering->next) {
        if (!strcmp(offering->value->product, "onyx350")) {
            g_assert_null(offering->value->default_topology);
        }
        if (strcmp(offering->value->product, "tezro")) {
            continue;
        }
        if (!strcmp(offering->value->topology, "tezro-tower-multi")) {
            tezro_found = true;
        }
        g_assert_null(offering->value->default_topology);
    }
    g_assert_true(tezro_found);
}

static void test_existing_population_views(void)
{
    /* Like the embedded catalogue, inputs outlive all cached projections. */
    SGIMachines *catalogue = load_catalogue();
    const SGIModelTopologyList *topology;

    for (topology = catalogue->topologies; topology;
         topology = topology->next) {
        const SGIModelNodePopulationList *row;

        for (row = topology->value->node_populations; row; row = row->next) {
            const SGIModelTopology *view;
            Error *err = NULL;

            if (row->value->grade == SGI_MODEL_EVIDENCE_GRADE_UNKNOWN) {
                continue;
            }
            view = sgi_model_population_view(topology->value, row->value, &err);
            g_assert_null(err);
            g_assert_nonnull(view);
            g_assert_cmpuint(QAPI_LIST_LENGTH(view->nodes), ==,
                             QAPI_LIST_LENGTH(row->value->nodes));
            g_assert_cmpuint(QAPI_LIST_LENGTH(view->io_peers), ==,
                             QAPI_LIST_LENGTH(topology->value->io_peers));
            g_assert(view->io_assemblies == topology->value->io_assemblies);
            g_assert(view->l1_management == topology->value->l1_management);
        }
    }
}

static void test_io_peer_runtime_accepts_absent_and_empty(void)
{
    char name[] = "no-io-peers";
    SGIModelTopology topology = { .name = name };
    SGIModelModule module = { .has_io_peers = true };
    SGIModelModuleList modules = { .value = &module };
    SGIModelLinkEnd end = { .kind = SGI_MODEL_LINK_END_KIND_NODE };
    SGIModelLinkEndList ends = { .value = &end };
    SGIModelCable cable = { .ends = &ends };
    SGIModelCableList cables = { .value = &cable };
    Error *err = NULL;
    g_autoptr(SGIMachines) catalogue = load_catalogue();
    const SGIModelTopologyList *entry;

    g_assert_true(sgi_model_check_runtime_endpoints(&topology, &err));
    g_assert_null(err);
    topology.has_io_peers = true;
    topology.modules = &modules;
    topology.cables = &cables;
    g_assert_true(sgi_model_check_runtime_endpoints(&topology, &err));
    g_assert_null(err);
    for (entry = catalogue->topologies; entry; entry = entry->next) {
        g_assert_true(sgi_model_check_runtime_endpoints(entry->value, &err));
        g_assert_null(err);
    }
}

static void test_sn1_runtime_rejects_shared_node_cable(void)
{
    SGIModelNode nodes[3] = { 0 };
    SGIModelNodeList node_list[3] = {
        { .value = &nodes[0], .next = &node_list[1] },
        { .value = &nodes[1], .next = &node_list[2] },
        { .value = &nodes[2] },
    };
    SGIModelLinkEnd ends[4] = {
        { .kind = SGI_MODEL_LINK_END_KIND_NODE, .index = 0 },
        { .kind = SGI_MODEL_LINK_END_KIND_NODE, .index = 1 },
        { .kind = SGI_MODEL_LINK_END_KIND_NODE, .index = 0 },
        { .kind = SGI_MODEL_LINK_END_KIND_NODE, .index = 2 },
    };
    SGIModelLinkEndList end_list[4] = {
        { .value = &ends[0], .next = &end_list[1] },
        { .value = &ends[1] },
        { .value = &ends[2], .next = &end_list[3] },
        { .value = &ends[3] },
    };
    SGIModelCable cables[2] = {
        { .ends = &end_list[0] },
        { .ends = &end_list[2] },
    };
    SGIModelCableList cable_list[2] = {
        { .value = &cables[0], .next = &cable_list[1] },
        { .value = &cables[1] },
    };
    SGIModelTopology topology = {
        .name = (char *)"shared-node-cable",
        .generation = SGI_MODEL_GENERATION_SN1,
        .nodes = &node_list[0],
        .cables = &cable_list[0],
    };
    Error *err = NULL;

    g_assert_false(sgi_model_check_runtime_endpoints(&topology, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err),
                            "unsupported runtime endpoint"));
    error_free(err);
}

/*
 * sgi_model_population_view() caches each view by its row's address for
 * the process lifetime, so every resolution test shares one catalogue that
 * is never freed: a freed row's address could be reused by a later load.
 */
static const SGIMachines *resolution_catalogue(void)
{
    static SGIMachines *catalogue;

    if (!catalogue) {
        catalogue = load_catalogue();
    }
    return catalogue;
}

static const SGIModelOffering *resolve(const char *options, Error **errp)
{
    return sgi_model_resolve_options(resolution_catalogue(), options, errp);
}

static void assert_refused(const char *options, const char *message)
{
    Error *err = NULL;

    g_assert_null(resolve(options, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==, message);
    error_free(err);
}

/* Every offering's own -M value selects an offering with that value. */
static void test_resolve_every_offering(void)
{
    const SGIModelOfferingList *entry;

    for (entry = resolution_catalogue()->offerings; entry;
         entry = entry->next) {
        const SGIModelOffering *offering = entry->value;
        const SGIModelOffering *resolved =
            resolve(offering->machine_options, &error_abort);

        g_assert_cmpstr(resolved->machine_options, ==,
                        offering->machine_options);
    }
}

static void test_resolve_selection(void)
{
    const SGIModelOffering *offering;

    offering = resolve("origin2000,topology=origin2000-deskside,nodes=2",
                       &error_abort);
    g_assert_cmpstr(offering->topology, ==, "origin2000-deskside");
    g_assert_cmpuint(offering->nodes, ==, 2);
    g_assert_cmpuint(QAPI_LIST_LENGTH(offering->storage), ==, 6);
    g_assert_cmpstr(offering->storage->value->name, ==, "node0-flash");

    /* No topology= takes the product's default, no nodes= its largest. */
    offering = resolve("origin2000", &error_abort);
    g_assert_cmpstr(offering->machine_options, ==,
                    "origin2000,topology=origin2000-rack,nodes=4");
    offering = resolve("origin200,nodes=1", &error_abort);
    g_assert_cmpstr(offering->machine_options, ==,
                    "origin200,topology=origin200,nodes=1");
    offering = resolve("origin300,topology=origin300-two-chassis",
                       &error_abort);
    g_assert_cmpuint(offering->nodes, ==, 2);

    /* Octane names its one node board like every other machine. */
    offering = resolve("octane2", &error_abort);
    g_assert_cmpstr(offering->machine_options, ==,
                    "octane2,topology=octane2,nodes=1");
    offering = resolve("octane", &error_abort);
    g_assert_cmpstr(offering->machine_options, ==,
                    "octane,topology=octane,nodes=1");
}

static void test_resolve_ip32_variants(void)
{
    const SGIModelOffering *offering;

    offering = resolve("o2", &error_abort);
    g_assert_cmpstr(offering->topology, ==, "o2");
    offering = resolve("o2,variant=o2", &error_abort);
    g_assert_cmpstr(offering->machine_options, ==, "o2");
    offering = resolve("o2,variant=o2plus", &error_abort);
    g_assert_cmpstr(offering->topology, ==, "o2plus");
    g_assert_cmpstr(offering->machine_options, ==, "o2,variant=o2plus");
    offering = resolve("o2,topology=o2plus,variant=o2plus,nodes=1",
                       &error_abort);
    g_assert_cmpstr(offering->topology, ==, "o2plus");

    assert_refused("o2,variant=unknown", "o2 has no variant 'unknown'");
    assert_refused("o2,variant=origin200", "o2 has no variant 'origin200'");
    assert_refused("o2,variant=", "o2 has no variant ''");
    assert_refused("origin200,variant=o2plus",
                   "origin200 does not offer variant selection");
    assert_refused("octane2,variant=o2plus",
                   "octane2 does not offer variant selection");
    assert_refused("o2,variant=o2plus,variant=o2plus",
                   "variant must be specified only once");
    assert_refused("o2,variant=o2,variant=o2plus",
                   "variant must be specified only once");
    assert_refused("o2,variant=o2plus,topology=o2",
                   "variant 'o2plus' conflicts with topology 'o2'");
    assert_refused("o2,variant=o2,topology=o2plus",
                   "variant 'o2' conflicts with topology 'o2plus'");
    assert_refused("o2,variant.0=o2plus", "variant must be a string");
    assert_refused("o2,variant=o2plus,topology.0=o2plus",
                   "topology must be a string");
    assert_refused("o2,variant=o2plus,,variant=o2",
                   "o2 has no variant 'o2plus,variant=o2'");
}

/* Refusals carry the machine's own wording for the same selection. */
static void test_resolve_refusals(void)
{
    assert_refused("origin2000,nodes=5",
                   "topology 'origin2000-rack' fits 1, 2, 3 or 4 node "
                   "boards, not 5");
    assert_refused("origin200,topology=origin2000-rack",
                   "origin200 has no topology 'origin2000-rack'");
    assert_refused("origin2000,topology=origin200",
                   "origin2000 has no topology 'origin200'");
    assert_refused("pc", "unknown machine 'pc'");
    assert_refused("origin2000,nodes=0",
                   "nodes must be a node board count, not '0'");
    assert_refused("origin2000,nodes=two",
                   "nodes must be a node board count, not 'two'");
    assert_refused("origin2000,baseio-preset=io6g",
                   "machine option 'baseio-preset' is not part of a "
                   "catalogue selection; give only the machine, "
                   "topology=, nodes=, processor= or an offered variant=");
    assert_refused("topology=origin200",
                   "no machine named in 'topology=origin200'");
}

/* topology= names only the named product's own topologies. */
static void test_resolve_product_scope(void)
{
    const SGIMachines *catalogue = resolution_catalogue();
    const SGIModelTopologyList *entry;

    for (entry = catalogue->topologies; entry; entry = entry->next) {
        const SGIModelTopology *t = entry->value;

        g_assert_true(sgi_model_topology_matches_product(t, t->product));
        g_assert_true(sgi_model_product_topology(catalogue, t->product,
                                                 t->name) == t);
    }
    g_assert_null(sgi_model_product_topology(catalogue, "origin200",
                                             "origin2000-rack"));
    g_assert_null(sgi_model_product_topology(catalogue, "origin2000",
                                             "no-such-topology"));
}

/*
 * The machine numbers a board's io<N>-flash and nvram<N> stores and a
 * module's module<N>-nvram store by the runtime's own rules, and names an
 * IO9 board's DS1742 pair and an expansion module's configuration storage
 * for their owners. Every
 * Scalable Node offering's storage list, which the generator writes, must
 * name exactly the stores those rules number, in the same order.
 */
static void test_storage_board_rules(void)
{
    const SGIMachines *catalogue = resolution_catalogue();
    const SGIModelOfferingList *entry;

    for (entry = catalogue->offerings; entry; entry = entry->next) {
        const SGIModelOffering *offering = entry->value;
        const SGIModelTopology *t = sgi_model_product_topology(
            catalogue, offering->product, offering->topology);
        g_autoptr(GString) expected = g_string_new(NULL);
        g_autoptr(GString) actual = g_string_new(NULL);
        const SGIModelIoBoardList *board;
        const SGIModelStorageItemList *item;
        const SGIModelModuleList *module;
        const SGIModelIoAssemblyList *assembly;
        const SGIModelTopology *fitted;

        g_assert_nonnull(t);
        if (t->nodes->value->asic_kind == SGI_MODEL_ASIC_KIND_HEART) {
            continue;
        }
        for (board = t->io_boards; board; board = board->next) {
            if (sgi_model_io_board_has_io_flash(board->value)) {
                g_string_append_printf(expected, "io%u-flash ",
                    sgi_model_io_board_index(
                        t, board->value, sgi_model_io_board_has_io_flash));
            }
        }
        for (board = t->io_boards; board; board = board->next) {
            if (sgi_model_io_board_has_nvram(board->value)) {
                unsigned int index = sgi_model_io_board_index(
                    t, board->value, sgi_model_io_board_has_nvram);

                g_string_append_printf(expected, "nvram%u nvram%u-clock ",
                                       index, index);
            }
        }
        /* Module stores follow the nodes this offering fits. */
        fitted = sgi_model_resolve_population(t, offering->nodes,
                                              &error_abort);
        /* An IO9 board's DS1742 pair is named for the board. */
        for (board = fitted->io_boards; board; board = board->next) {
            if (board->value->has_ds1742 && board->value->ds1742) {
                g_string_append_printf(expected,
                                       "ds1742-%s-data ds1742-%s-clock ",
                                       board->value->id, board->value->id);
            }
        }
        /* An expansion module's configuration storage, for its module. */
        for (assembly = fitted->io_assemblies; assembly;
             assembly = assembly->next) {
            const SGIModelModuleList *owner = fitted->modules;

            if (!assembly->value->has_configuration_storage) {
                continue;
            }
            for (size_t i = 0; i < assembly->value->module_index; i++) {
                owner = owner->next;
            }
            g_assert_nonnull(owner);
            g_string_append_printf(expected, "%s-flash ", owner->value->id);
        }
        for (module = fitted->modules; module; module = module->next) {
            if (sgi_model_module_has_nvram(fitted, module->value)) {
                g_string_append_printf(expected, "module%u-nvram ",
                    sgi_model_module_nvram_index(fitted, module->value));
            }
        }
        /* Each graphics slot an InfiniteReality pipe fits, its flash. */
        for (SGIModelGraphicsSlotList *slot = t->graphics ?
             t->graphics->slots : NULL; slot; slot = slot->next) {
            for (SGIModelGraphicsFamilyList *f = slot->value->families; f;
                 f = f->next) {
                if (f->value->family == SGI_MODEL_GRAPHICS_FAMILY_KIND_KONA) {
                    g_string_append_printf(expected, "%s-kona-flash ",
                                           slot->value->name);
                }
            }
        }
        for (item = offering->storage; item; item = item->next) {
            if (!g_str_has_prefix(item->value->name, "node")) {
                g_string_append_printf(actual, "%s ", item->value->name);
            }
        }
        g_assert_cmpstr(actual->str, ==, expected->str);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sn-model/optional-pci-board-references-parse",
                    test_optional_pci_board_references_parse);
    g_test_add_func("/sn-model/io9-host-projection-remaps-references",
                    test_io9_hosts_and_boards_follow_projected_attachments);
    g_test_add_func("/sn-model/every-topology-is-well-formed",
                    test_every_topology_is_well_formed);
    g_test_add_data_func("/sn-model/ip32/o2-standalone", "o2",
                         test_ip32_standalone_board);
    g_test_add_data_func("/sn-model/ip32/o2plus-standalone", "o2plus",
                         test_ip32_standalone_board);
    g_test_add_data_func("/sn-model/ip32/rejects-module-owner", NULL,
                         test_ip32_rejects_sn_ownership);
    g_test_add_data_func("/sn-model/ip32/rejects-router", GUINT_TO_POINTER(1),
                         test_ip32_rejects_sn_ownership);
    g_test_add_data_func("/sn-model/ownership/sn0-requires-module",
                         GUINT_TO_POINTER(SGI_MODEL_GENERATION_SN0),
                         test_modular_node_requires_owner);
    g_test_add_data_func("/sn-model/ownership/sn1-requires-module",
                         GUINT_TO_POINTER(SGI_MODEL_GENERATION_SN1),
                         test_modular_node_requires_owner);
    g_test_add_data_func("/sn-model/ownership/ip30-requires-module",
                         GUINT_TO_POINTER(SGI_MODEL_GENERATION_IP30),
                         test_modular_node_requires_owner);
    g_test_add_func("/sn-model/tezro-requires-explicit-topology",
                    test_tezro_requires_explicit_topology);
    g_test_add_func("/sn-model/existing-population-views",
                    test_existing_population_views);
    g_test_add_func("/sn-model/l1-peer/population-preserved",
                    test_l1_peer_population_preserved);
    g_test_add_func("/sn-model/xio-cable/view-copies-new-records",
                    test_xio_cable_view_copies_new_records);
    g_test_add_func("/sn-model/router/view-keeps-node-ports",
                    test_router_view_keeps_node_ports);
    g_test_add_func("/sn-model/xio-cable/view-refuses-bad-ends",
                    test_xio_cable_view_refuses_bad_ends);
    g_test_add_func("/sn-model/io-peer/population-preserved",
                    test_io_peer_population_preserved);
    g_test_add_func("/sn-model/io-peer/population-rejects-reduced",
                    test_io_peer_population_rejects_reduced);
    g_test_add_func("/sn-model/io-peer/population-rejects-missing-cable",
                    test_io_peer_population_rejects_missing_cable);
    g_test_add_func("/sn-model/io-peer/descriptor", test_io_peer_descriptor);
    g_test_add_func("/sn-model/io-peer/requires-identity",
                    test_io_peer_requires_identity);
    g_test_add_func("/sn-model/io-peer/rejects-processors",
                    test_io_peer_rejects_processors);
    g_test_add_func("/sn-model/io-peer/existing-topologies",
                    test_existing_topologies_have_no_io_peers);
    g_test_add_func("/sn-model/io-peer/expansion-offering",
                    test_expansion_offering);
    g_test_add_func("/sn-model/io-peer/expansion-storage-offering",
                    test_expansion_storage_offering);
    g_test_add_data_func("/sn-model/io-peer/runtime-refuses-peer", NULL,
                         test_io_peer_runtime_refusal);
    g_test_add_data_func("/sn-model/io-peer/runtime-refuses-module-only",
                         GUINT_TO_POINTER(1), test_io_peer_runtime_refusal);
    g_test_add_data_func("/sn-model/io-peer/runtime-refuses-orphan-cable",
                         GUINT_TO_POINTER(2), test_io_peer_runtime_refusal);
    g_test_add_func("/sn-model/io-peer/runtime-accepts-absent-and-empty",
                    test_io_peer_runtime_accepts_absent_and_empty);
    g_test_add_func("/sn-model/runtime-rejects-shared-node-cable",
                    test_sn1_runtime_rejects_shared_node_cable);
    g_test_add_func("/sn-model/resolve/every-offering",
                    test_resolve_every_offering);
    g_test_add_func("/sn-model/resolve/selection", test_resolve_selection);
    g_test_add_func("/sn-model/resolve/ip32-variants",
                    test_resolve_ip32_variants);
    g_test_add_func("/sn-model/resolve/refusals", test_resolve_refusals);
    g_test_add_func("/sn-model/resolve/product-scope",
                    test_resolve_product_scope);
    g_test_add_func("/sn-model/storage-board-rules",
                    test_storage_board_rules);
    return g_test_run();
}
