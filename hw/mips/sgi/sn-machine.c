/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The abstract SGI Scalable Node family machine and its builder over an
 * SGIModelTopology from the compiled catalogue (qapi/sgi-sn.json,
 * embedded and parsed by hw/mips/sgi/sn-catalogue.c). Origin 200 is the
 * first product on this base; see docs/specs/sgi-sn.rst.
 *
 * Fixed here rather than carried in the descriptor, because they are Hub
 * hardware facts rather than per-configuration data: the Hub's physical
 * address map, its memory-bank geometry and its three hardwired CPU
 * interrupt inputs.
 *
 * Origin 200's onboard base I/O sits on its one node's XIO port: sn0.c
 * owns it (sn0_node_build_baseio(), called from this file's own
 * io_boards[] loop below, once every node has realized) since it is a
 * fitted part of the node's own System Board, not a removable card. The
 * Origin 2000 module's BaseIO card (sgi-xio-baseio) sits behind a Crossbow
 * this builder owns, and is a sgi-xio-board instance built and realized
 * here, before the nodes' Crossbows realize. Every XIO attachment is one
 * XIO link (xio-link.h) between two ports. An empty XIO slot the
 * topology names keeps a link of its own, to its Crossbow port, for a
 * `-device` board that names the slot (slot=io5) to claim
 * (TYPE_SGI_XIO_SLOTS); see sgi_machine_done() below for how that board
 * comes up.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "qemu/host-utils.h"
#include "qemu/datadir.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "qapi/qapi-builtin-types.h"
#include "qapi/qapi-visit-common.h"
#include "qapi/visitor.h"
#include "qobject/qlist.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/mips/sgi/baseio.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/misc/sgi-bic-board-interface.h"
#include "hw/misc/sgi-l1.h"
#include "chardev/char.h"
#include "migration/vmstate.h"
#include "hw/misc/sgi-hub.h"
#include "hw/misc/sgi-kona.h"
#include "hw/misc/sgi-mgras.h"
#include "hw/misc/sgi-router.h"
#include "hw/misc/sgi-sn0-module-management.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "hw/pci/pci.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/system.h"
#include "trace/control.h"
#include "cpu.h"
#include "hw/mips/sgi/sn-catalogue.h"
#include "hw/mips/sgi/sn-machine.h"
#include "hw/mips/sgi/sn1-node.h"
#include "hw/mips/sgi/net-cable.h"
#include "sn0.h"
#include "origin300-pci-expansion.h"

/*
 * The compiled catalogue's QAPI-generated types (qapi/sgi-sn.json) hold
 * every repeated field as a NULL-terminated linked list, where the old
 * hand-written sn-model.h used a flat array and a count; the builder
 * below still wants positional indexing (a node's own position in
 * t->nodes, a module's node by position in its own nodes list), so each
 * helper walks a list once into a freshly allocated array of pointers --
 * an index table built once per caller -- rather
 * than threading list-walk logic through every call site below. Callers
 * hold the result no longer than their own scope (g_autofree).
 */
#define SGI_MODEL_DEFINE_LIST_ARRAY(Type, name)                             \
static const Type **sgi_model_##name##_array(const Type##List *list,        \
                                             size_t *count)                  \
{                                                                            \
    const Type##List *entry;                                                \
    Type **array;                                                           \
    size_t i = 0, n = 0;                                                    \
                                                                              \
    for (entry = list; entry; entry = entry->next) {                        \
        n++;                                                                \
    }                                                                       \
    array = g_new(Type *, n ? n : 1);                                       \
    for (entry = list; entry; entry = entry->next) {                        \
        array[i++] = entry->value;                                          \
    }                                                                       \
    *count = n;                                                             \
    return (const Type **)array;                                           \
}

SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelNode, node)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelModule, module)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelRouter, router)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelCable, cable)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelCrossbar, crossbar)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelXioAttachment, xio_attachment)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelXioSlot, xio_slot)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelIoBoard, io_board)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelController, controller)

static unsigned int *sgi_model_array_uint32(const uint32List *list, size_t *count)
{
    const uint32List *entry;
    unsigned int *array;
    size_t i = 0, n = 0;

    for (entry = list; entry; entry = entry->next) {
        n++;
    }
    array = g_new(unsigned int, n ? n : 1);
    for (entry = list; entry; entry = entry->next) {
        array[i++] = entry->value;
    }
    *count = n;
    return array;
}

static uint64_t *sgi_model_array_uint64(const uint64List *list, size_t *count)
{
    const uint64List *entry;
    uint64_t *array;
    size_t i = 0, n = 0;

    for (entry = list; entry; entry = entry->next) {
        n++;
    }
    array = g_new(uint64_t, n ? n : 1);
    for (entry = list; entry; entry = entry->next) {
        array[i++] = entry->value;
    }
    *count = n;
    return array;
}

#define TYPE_SGI_XIO_BASEIO "sgi-xio-baseio"
#define TYPE_SGI_PCI_EXPANSION "sgi-pci-expansion"
#define TYPE_SGI_PCI_CARRIER "sgi-pci-carrier"
#define TYPE_SGI_PCI_SHOEHORN "sgi-pci-shoehorn"
#define TYPE_SGI_GIGACHANNEL "sgi-gigachannel"

typedef enum SGIMachineInput {
    SGI_INPUT_BOARD_ID,
    SGI_INPUT_IOC3_SUBSYSTEM,
    SGI_INPUT_L1_TYPE,
    SGI_INPUT_L1_REVISION,
    SGI_INPUT_BEDROCK_REVISION,
    SGI_INPUT_MAC_EEPROM,
    SGI_INPUT_COUNT,
} SGIMachineInput;

static const char *const sgi_machine_input_names[] = {
    [SGI_INPUT_BOARD_ID] = "fuel-board-id-word",
    [SGI_INPUT_IOC3_SUBSYSTEM] = "fuel-ioc3-subsystem-id",
    [SGI_INPUT_L1_TYPE] = "fuel-l1-type-code",
    [SGI_INPUT_L1_REVISION] = "fuel-l1-revision",
    [SGI_INPUT_BEDROCK_REVISION] = "fuel-bedrock-revision",
    [SGI_INPUT_MAC_EEPROM] = "fuel-mac-eeprom",
};

typedef struct SGIMachineNodeConfig {
    uint64_t board_id_word;
    uint8_t revision;
    uint8_t local_region;
    uint8_t initiator_region;
    uint8_t installed_bank_mask;
    unsigned int controller_index;
} SGIMachineNodeConfig;

typedef struct SGIMachineL1Config {
    uint8_t reported_type;
    int32_t revision[3];
    const char *mac_eeprom;
} SGIMachineL1Config;

typedef struct SGIMachineIoConfig {
    uint16_t ioc3_subsystem_id;
    SGIModelMacSource mac_source;
    uint8_t pci_revision[SGI_MODEL_PCI_FUNCTION_KIND__MAX];
} SGIMachineIoConfig;

typedef struct SGIMachinePeerNotifier {
    Notifier notifier;
    SN1FabricEndpoint *endpoint;
} SGIMachinePeerNotifier;

typedef struct SGIMachineAssemblyOwnership {
    uint32_t peer;
    uint32_t module;
    uint32_t controller;
} SGIMachineAssemblyOwnership;

struct SGIMachineState {
    MachineState parent_obj;

    char *topology_name;
    /*
     * population=: the per-node processor counts, colon-separated in node
     * order, naming one of the topology's accepted populations when -smp
     * alone matches more than one (a two-chassis Origin 300 fits six as
     * 4:2 or 2:4). NULL leaves -smp to choose.
     */
    char *population;
    /*
     * nodes=: how many node boards the chassis fits, in SGI's fill
     * order; 0 until set, which fits the topology's largest row.
     */
    uint32_t nodes_requested;
    /* The topology this machine builds: one row of its chassis. */
    const SGIModelTopology *view;
    char *baseio_preset;

    unsigned int num_nodes;
    SN0NodeState **nodes;
    MemoryRegion *big_window_route;

    /* This machine's Routers, in descriptor order, so a cable's own
     * SGIModelLinkEnd index addresses them. */
    SGIRouterState **routers;
    unsigned int router_count;
    Clock *cpu_refclk;
    Clock *rt_refclk;

    /* Parented and realized as soon as every port attached to it is. */
    SGIXbowState **crossbars;

    /*
     * One sgi-xio-board per Crossbow-attached io_boards[] entry, built
     * before its own Crossbow. An entry with no board wrapper leaves its
     * own slot NULL: Origin 200's onboard I/O, built by
     * sn0_node_build_baseio() instead.
     */
    SGIXioBoardState **io_boards;

    /*
     * The XIO links: one per node on a Crossbow (its Hub's I/O interface
     * to its Crossbow port), and one per io_boards[] entry (the board's
     * widget to its node's Hub or its Crossbow port). Unused entries stay
     * zeroed.
     */
    SGIXioLink *node_links;
    SGIXioLink *board_links;
    SGIXioLink vbrick_link;

    /*
     * One link per empty XIO slot the topology names (t->xio_slots, in
     * order), from the slot's Crossbow port to the `-device` board that
     * claims it. slot_claimed marks each slot a board has taken, so a
     * second board naming it is refused.
     */
    SGIXioLink *slot_links;
    bool *slot_claimed;
    Notifier machine_done;

    bool debug_leds;
    bool junk_uart_present;
    uint8_t elsc_dip_switches;

    /*
     * The Bedrock nodes, for a topology whose node kind is Bedrock: one
     * per t->nodes entry, the way s->nodes is for a Hub topology. The
     * shipped Origin 300 carries one; a second is descriptor data the
     * builder accepts without exercising it (see sn-builder-test.c's
     * synthetic two-node fixture).
     */
    DeviceState **bedrock_nodes;
    SGIMachineNodeConfig *node_config;
    SGIMachineL1Config *l1_config;
    SGIMachineIoConfig *io_config;
    DeviceState **io_peers;
    unsigned int io_peer_count;
    SGIMachinePeerNotifier *io_peer_notifiers;
    SGIMachineAssemblyOwnership *assembly_ownership;
    DeviceState **controllers;
    bool *controller_reset_registered;
    DeviceState **controller_bics;
    uint32_t controller_count;
    uint32_t *owner_controllers;
    SGIL1QueryOwner *query_owners;
    SGIL1QueryAssociation *query_associations;
    SGIL1QueryAttachment *query_attachments;
    uint32_t association_count;
    uint32_t attachment_count;
    uint8_t *initial_reachability;
    SGIL1QueryRouter query_router;
    bool management_registered;
    /* Each Bedrock node's share of machine->ram, in node order. */
    MemoryRegion *bedrock_ram;

    /*
     * The Origin 300/350 L1's identity-record file lists. One machine
     * property set regardless of product (per-kind properties split
     * across classes is exactly what the family base replaces); refused
     * at init if the topology carries no L1 controller to hand them to.
     */
    strList *chassis_eeprom;
    strList *board_eeprom;
    strList *spd_eeprom;
    uint64_t fuel_board_id_word;
    bool fuel_board_id_word_set;
    uint16_t fuel_ioc3_subsystem_id;
    bool fuel_ioc3_subsystem_id_set;
    uint8_t fuel_l1_type_code;
    bool fuel_l1_type_code_set;
    int32_t fuel_l1_revision[3];
    bool fuel_l1_revision_set;
    uint8_t fuel_bedrock_revision;
    bool fuel_bedrock_revision_set;
    char *fuel_mac_eeprom;
    strList *controller_board_eeprom;
    strList *controller_chardev;
};

/*
 * The controller holds one identification record per file, in order. The
 * machine names the files and claims nothing about what is in them: no
 * default record, and no record implied by the memory size.
 */
typedef struct SGIMachineIdentityRecords {
    const char *name;
    ptrdiff_t offset;
    const char *description;
    bool targeted;
} SGIMachineIdentityRecords;

static const SGIMachineIdentityRecords sgi_machine_identity_records[] = {
    {
        "chassis-eeprom", offsetof(SGIMachineState, chassis_eeprom),
        "Files holding the chassis identification records, in order",
    },
    {
        "board-eeprom", offsetof(SGIMachineState, board_eeprom),
        "Files holding the board identification records, in order",
    },
    {
        "spd-eeprom", offsetof(SGIMachineState, spd_eeprom),
        "Files holding the memory module presence-detect records, in order",
    },
    {
        "controller-board-eeprom",
        offsetof(SGIMachineState, controller_board_eeprom),
        "Expansion board records as controller-index/component-index=path",
        true,
    },
    {
        "controller-chardev", offsetof(SGIMachineState, controller_chardev),
        "Expansion host console as controller-index=chardev-id",
        true,
    },
};

static strList **sgi_machine_identity_slot(Object *obj, void *opaque)
{
    const SGIMachineIdentityRecords *records = opaque;

    return (strList **)((char *)SGI_MACHINE(obj) + records->offset);
}

static void sgi_machine_get_identity_records(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    visit_type_strList(v, name, sgi_machine_identity_slot(obj, opaque), errp);
}

static void sgi_machine_set_identity_records(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    strList **slot = sgi_machine_identity_slot(obj, opaque);
    strList *replacement = NULL;

    if (!visit_type_strList(v, name, &replacement, errp)) {
        return;
    }
    qapi_free_strList(*slot);
    *slot = replacement;
}

static bool sgi_machine_has_identity_records(SGIMachineState *s)
{
    return s->chassis_eeprom || s->board_eeprom || s->spd_eeprom;
}

/*
 * Copies the machine's own identity-record file lists onto an L1 device
 * before it is realized. Takes the L1 explicitly because it lives on the
 * node (sgi-sn1-node), while the file lists it reads are machine
 * properties. With several nodes each list splits evenly, node_index
 * taking its own share in order; a list that does not divide evenly is
 * refused, since no share of it names one chassis's parts.
 */
bool sgi_machine_set_l1_identity_records(MachineState *machine,
                                         DeviceState *l1,
                                         unsigned int node_index,
                                         Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    unsigned int nodes = MAX(s->num_nodes, 1);
    size_t i;

    for (i = 0; i < ARRAY_SIZE(sgi_machine_identity_records); i++) {
        const SGIMachineIdentityRecords *records =
            &sgi_machine_identity_records[i];
        strList *head = *sgi_machine_identity_slot(OBJECT(s), (void *)records);
        strList *file;
        size_t count = 0, share, position = 0;
        QList *files;

        if (!head || records->targeted) {
            continue;
        }
        for (file = head; file; file = file->next) {
            count++;
        }
        if (count % nodes) {
            error_setg(errp, "%s: %s lists %zu files, which do not divide "
                       "evenly across the %u chassis L1s",
                       MACHINE_GET_CLASS(machine)->name, records->name,
                       count, nodes);
            return false;
        }
        share = count / nodes;
        files = qlist_new();
        for (file = head; file; file = file->next, position++) {
            if (position / share == node_index) {
                qlist_append_str(files, file->value);
            }
        }
        qdev_prop_set_array(l1, records->name, files);
    }
    return true;
}

static bool sgi_machine_controller_input(const char *text, bool component,
    unsigned *owner, unsigned *slot, const char **payload, Error **errp)
{
    const char *end;

    if (!g_ascii_isdigit(text[0]) || qemu_strtoui(text, &end, 10, owner)) {
        goto invalid;
    }
    if (component) {
        if (*end != '/' || !g_ascii_isdigit(end[1]) ||
            qemu_strtoui(end + 1, &end, 10, slot) || *slot >= 4) {
            goto invalid;
        }
    }
    if (*end != '=' || !end[1]) {
        goto invalid;
    }
    *payload = end + 1;
    return true;

invalid:
    error_setg(errp, "invalid controller input '%s'; expected %s", text,
               component ? "owner/component=path" : "owner=chardev-id");
    return false;
}

static bool sgi_machine_expansion_controller(const SGIModelTopology *t,
                                             unsigned owner)
{
    const SGIModelIoAssemblyList *entry;

    for (entry = t->io_assemblies; entry; entry = entry->next) {
        if (entry->value->controller_index == owner) {
            return true;
        }
    }
    return false;
}

static bool sgi_machine_validate_controller_inputs(SGIMachineState *s,
    const SGIModelTopology *t, Error **errp)
{
    size_t count = QAPI_LIST_LENGTH(t->controllers);
    g_autofree bool *boards = g_new0(bool, MAX(count * 4, 1));
    g_autofree bool *consoles = g_new0(bool, MAX(count, 1));
    const strList *entry, *prior;

    for (unsigned kind = 0; kind < 2; kind++) {
        const strList *head = kind ? s->controller_chardev :
                                   s->controller_board_eeprom;

        for (entry = head; entry; entry = entry->next) {
            unsigned owner, slot = 0;
            const char *payload;
            bool *used;

            if (!sgi_machine_controller_input(entry->value, !kind, &owner,
                                               &slot, &payload, errp)) {
                return false;
            }
            if (owner >= count || !sgi_machine_expansion_controller(t, owner)) {
                error_setg(errp, "controller %u is not an expansion L1 owner",
                           owner);
                return false;
            }
            used = kind ? &consoles[owner] : &boards[owner * 4 + slot];
            if (*used) {
                error_setg(errp, "duplicate controller input for owner %u",
                           owner);
                return false;
            }
            *used = true;
            for (prior = head; prior != entry; prior = prior->next) {
                if (!strcmp(strchr(prior->value, '=') + 1, payload)) {
                    error_setg(errp, "controller inputs reuse '%s'", payload);
                    return false;
                }
            }
            if (kind) {
                if (!qemu_chr_find(payload)) {
                    error_setg(errp, "controller chardev '%s' does not exist",
                               payload);
                    return false;
                }
            } else {
                for (size_t i = 0; i < ARRAY_SIZE(sgi_machine_identity_records);
                     i++) {
                    const SGIMachineIdentityRecords *records =
                        &sgi_machine_identity_records[i];
                    const strList *raw;

                    if (records->targeted) {
                        continue;
                    }
                    raw = *sgi_machine_identity_slot(OBJECT(s),
                                                      (void *)records);
                    for (; raw; raw = raw->next) {
                        if (!strcmp(raw->value, payload)) {
                            error_setg(errp, "expansion record reuses compute "
                                       "input '%s'", payload);
                            return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

static bool sgi_machine_set_expansion_inputs(SGIMachineState *s,
    unsigned owner, DeviceState *l1, Error **errp)
{
    const strList *entry;
    const char *paths[4] = { NULL };
    bool supplied = false;

    for (entry = s->controller_board_eeprom; entry; entry = entry->next) {
        unsigned selected, slot;
        const char *payload;

        if (!sgi_machine_controller_input(entry->value, true, &selected,
                                           &slot, &payload, errp)) {
            return false;
        }
        if (selected == owner) {
            paths[slot] = payload;
            supplied = true;
        }
    }
    if (supplied) {
        QList *files = qlist_new();

        for (unsigned i = 0; i < ARRAY_SIZE(paths); i++) {
            qlist_append_str(files, paths[i] ? paths[i] : "");
        }
        qdev_prop_set_array(l1, "board-record-eeprom", files);
    }
    for (entry = s->controller_chardev; entry; entry = entry->next) {
        unsigned selected, slot;
        const char *payload;

        if (!sgi_machine_controller_input(entry->value, false, &selected,
                                           &slot, &payload, errp)) {
            return false;
        }
        if (selected == owner &&
            !object_property_set_str(OBJECT(l1), "chardev", payload, errp)) {
            return false;
        }
    }
    return true;
}

/* ---- Topology resolution and the CPU-population contract ---- */

static bool sgi_machine_topology_matches_product(const SGIModelTopology *t,
                                                  const char *product)
{
    size_t len = strlen(product);
    size_t name_len = strlen(t->name);

    return name_len >= len && !strncmp(t->name, product, len) &&
           (t->name[len] == '\0' || t->name[len] == '-');
}

/*
 * The compiled catalogue holds every product's topologies in one list,
 * so a name match alone is not enough: topology= is product-scoped
 * (origin200 never builds origin2000's wiring).
 */
static const SGIModelTopology *sgi_machine_chassis(MachineState *machine)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(machine);
    const char *name;
    const SGIModelTopology *t;

    if (smc->test_topology) {
        return smc->test_topology;
    }

    name = s->topology_name ? s->topology_name : smc->default_topology;
    if (!name || !smc->product) {
        return NULL;
    }
    t = sgi_model_find_topology(name);

    if (t && !sgi_machine_topology_matches_product(t, smc->product)) {
        return NULL;
    }
    return t;
}

/* "1, 2, 3 or 4": a chassis's accepted node counts, for a refusal. */
static char *sgi_machine_accepted_node_counts(const SGIModelTopology *t)
{
    GString *out = g_string_new(NULL);
    const SGIModelNodePopulationList *entry;

    for (entry = t->node_populations; entry; entry = entry->next) {
        if (out->len) {
            g_string_append(out, entry->next ? ", " : " or ");
        }
        g_string_append_printf(out, "%zu",
                               QAPI_LIST_LENGTH(entry->value->nodes));
    }
    return g_string_free(out, false);
}

/*
 * The topology this machine builds: the chassis topology= names, with
 * the node-population row nodes= selects, or its largest by default. A
 * row resting on unsettled evidence is refused, not built on a guess.
 */
static const SGIModelTopology *sgi_machine_resolve(MachineState *machine,
                                                   Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    const SGIModelTopology *chassis = sgi_machine_chassis(machine);
    const SGIModelNodePopulation *row;

    if (s->view) {
        return s->view;
    }
    if (!chassis) {
        error_setg(errp, "unknown topology");
        return NULL;
    }
    if (!sgi_model_check_runtime_endpoints(chassis, errp)) {
        return NULL;
    }
    row = s->nodes_requested ?
        sgi_model_find_population(chassis, s->nodes_requested) :
        sgi_model_default_population(chassis);
    if (!row) {
        g_autofree char *counts = sgi_machine_accepted_node_counts(chassis);

        error_setg(errp, "topology '%s' fits %s node board%s, not %u",
                   chassis->name, counts,
                   g_str_equal(counts, "1") ? "" : "s", s->nodes_requested);
        return NULL;
    }
    if (row->grade == SGI_MODEL_EVIDENCE_GRADE_UNKNOWN) {
        size_t count = QAPI_LIST_LENGTH(row->nodes);

        error_setg(errp, "topology '%s' with %zu node board%s rests on "
                   "evidence that does not settle its wiring; it is not "
                   "built", chassis->name, count, count == 1 ? "" : "s");
        return NULL;
    }
    s->view = sgi_model_population_view(chassis, row, errp);
    return s->view;
}

static const SGIModelTopology *sgi_machine_topology(MachineState *machine)
{
    return sgi_machine_resolve(machine, NULL);
}

/*
 * -smp and -m default to the product's default topology and its most
 * node boards, whatever nodes= says: the machine cannot tell a default
 * from an explicit value. A refusal says so when nodes= asked for fewer.
 */
static const char *sgi_machine_nodes_hint(MachineState *machine)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    const SGIModelTopology *chassis = sgi_machine_chassis(machine);
    const SGIModelNodePopulation *largest =
        chassis ? sgi_model_default_population(chassis) : NULL;

    if (!s->nodes_requested || !largest ||
        s->nodes_requested == QAPI_LIST_LENGTH(largest->nodes)) {
        return "";
    }
    return "; -smp and -m default to the most node boards, so give both "
           "with nodes=";
}

/* The one row a machine's own built topology holds. */
static const SGIModelNodePopulation *sgi_machine_view_row(
    const SGIModelTopology *t)
{
    g_assert(t->node_populations);
    return t->node_populations->value;
}

static unsigned int sgi_machine_population_total(
    const SGIModelCpuPopulation *population)
{
    unsigned int total = 0;
    const uint32List *entry;

    for (entry = population->cpus_per_node; entry; entry = entry->next) {
        total += entry->value;
    }
    return total;
}

unsigned int sgi_machine_product_max_cpus(const char *product)
{
    unsigned int max_cpus = 0;
    const SGIModelTopologyList *entry;

    for (entry = sgi_model_catalogue()->topologies; entry; entry = entry->next) {
        const SGIModelTopology *t = entry->value;
        const SGIModelNodePopulationList *row;

        if (!sgi_machine_topology_matches_product(t, product)) {
            continue;
        }
        for (row = t->node_populations; row; row = row->next) {
            const SGIModelCpuPopulationList *population;

            for (population = row->value->supported_cpu_populations;
                 population; population = population->next) {
                unsigned int total =
                    sgi_machine_population_total(population->value);

                max_cpus = MAX(max_cpus, total);
            }
        }
    }
    return max_cpus;
}

/*
 * The NASID a node seeds with: its slot, relative to the topology's
 * lowest slot, wherever every node has a distinct slot. Origin 200's
 * slots all read zero, so a topology whose slots collide falls back to
 * node array order instead of colliding every node on NASID zero.
 */
static unsigned int sgi_machine_topology_nasid(const SGIModelTopology *t,
                                               unsigned int node_index)
{
    g_autofree const SGIModelNode **nodes = NULL;
    unsigned int slot_min;
    size_t node_count, i, j;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    for (i = 0; i < node_count; i++) {
        for (j = 0; j < i; j++) {
            if (nodes[i]->slot == nodes[j]->slot) {
                return node_index;
            }
        }
    }
    slot_min = nodes[0]->slot;
    for (i = 1; i < node_count; i++) {
        slot_min = MIN(slot_min, nodes[i]->slot);
    }
    return nodes[node_index]->slot - slot_min;
}

/* The module whose node list names this node, or NULL. */
static const SGIModelModule *sgi_machine_node_module(const SGIModelTopology *t,
                                                     unsigned int node_index)
{
    g_autofree const SGIModelModule **modules = NULL;
    size_t module_count, i, j;

    modules = sgi_model_module_array(t->modules, &module_count);
    for (i = 0; i < module_count; i++) {
        g_autofree unsigned int *nodes = NULL;
        size_t node_count;

        nodes = sgi_model_array_uint32(modules[i]->nodes, &node_count);
        for (j = 0; j < node_count; j++) {
            if (nodes[j] == node_index) {
                return modules[i];
            }
        }
    }
    return NULL;
}

uint64_t sgi_machine_topology_default_ram(const SGIModelTopology *chassis)
{
    const SGIModelTopology *t = sgi_model_population_view(
        chassis, sgi_model_default_population(chassis), &error_abort);
    g_autofree const SGIModelNode **nodes = NULL;
    uint64_t total_mib = 0;
    size_t node_count, i;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    for (i = 0; i < node_count; i++) {
        g_autofree uint64_t *bank_mib = NULL;
        size_t bank_count, b;

        bank_mib = sgi_model_array_uint64(nodes[i]->bank_mib, &bank_count);
        for (b = 0; b < bank_count; b++) {
            total_mib += bank_mib[b];
        }
    }
    return total_mib * MiB;
}

unsigned int sgi_machine_topology_default_cpus(
    const SGIModelTopology *chassis)
{
    const SGIModelNodePopulation *row = sgi_model_default_population(chassis);

    g_assert(row && row->supported_cpu_populations);
    return sgi_machine_population_total(
        row->supported_cpu_populations->value);
}

/* A topology's own first node: every product measured has at least one. */
static const SGIModelNode *sgi_machine_topology_node0(const SGIModelTopology *t)
{
    g_assert(t->nodes);
    return t->nodes->value;
}

/* Every topology's node_count, without an index table for one integer. */
static size_t sgi_machine_topology_node_count(const SGIModelTopology *t)
{
    const SGIModelNodeList *entry;
    size_t count = 0;

    for (entry = t->nodes; entry; entry = entry->next) {
        count++;
    }
    return count;
}

/* Likewise for io_boards, without allocating an index table for one count. */
static size_t sgi_machine_topology_io_board_count(const SGIModelTopology *t)
{
    const SGIModelIoBoardList *entry;
    size_t count = 0;

    for (entry = t->io_boards; entry; entry = entry->next) {
        count++;
    }
    return count;
}

/* Likewise for xio_slots. */
static size_t sgi_machine_topology_xio_slot_count(const SGIModelTopology *t)
{
    const SGIModelXioSlotList *entry;
    size_t count = 0;

    for (entry = t->xio_slots; entry; entry = entry->next) {
        count++;
    }
    return count;
}

/* Likewise for crossbars. */
static size_t sgi_machine_topology_crossbar_count(const SGIModelTopology *t)
{
    const SGIModelCrossbarList *entry;
    size_t count = 0;

    for (entry = t->crossbars; entry; entry = entry->next) {
        count++;
    }
    return count;
}

/* Whether @population's per-node counts are exactly @text's (e.g. "4:2"). */
static bool sgi_machine_population_named(
    const SGIModelCpuPopulation *population, const char *text)
{
    g_auto(GStrv) counts = g_strsplit(text, ":", -1);
    g_autofree unsigned int *cpus_per_node = NULL;
    size_t n, i;

    cpus_per_node = sgi_model_array_uint32(population->cpus_per_node, &n);
    if (g_strv_length(counts) != n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        unsigned long value;

        if (qemu_strtoul(counts[i], NULL, 10, &value) ||
            value != cpus_per_node[i]) {
            return false;
        }
    }
    return true;
}

/*
 * Every R10000 and R14000 is a single-core socket, so -smp N names a
 * total, never a shape: which node each of the N sockets sits on comes
 * from the topology's own accepted population, the way the population
 * table already records it. Find the population whose total is
 * total_cpus, and whose per-node counts are @named's when @named is
 * given; *second names a second match so the caller can refuse an
 * ambiguous total instead of guessing between them.
 */
static const SGIModelCpuPopulation *sgi_machine_find_population_by_total(
    const SGIModelTopology *t, unsigned int total_cpus,
    const char *named, const SGIModelCpuPopulation **second)
{
    size_t node_count = sgi_machine_topology_node_count(t);
    const SGIModelCpuPopulation *match = NULL;
    const SGIModelCpuPopulationList *entry;

    *second = NULL;
    for (entry = sgi_machine_view_row(t)->supported_cpu_populations; entry;
         entry = entry->next) {
        const SGIModelCpuPopulation *population = entry->value;
        g_autofree unsigned int *cpus_per_node = NULL;
        size_t population_node_count;

        cpus_per_node = sgi_model_array_uint32(population->cpus_per_node,
                                               &population_node_count);
        if (population_node_count != node_count) {
            continue;
        }
        if (sgi_machine_population_total(population) != total_cpus) {
            continue;
        }
        if (named && !sgi_machine_population_named(population, named)) {
            continue;
        }
        if (!match) {
            match = population;
        } else {
            *second = population;
            break;
        }
    }
    return match;
}

/* "1, 2 or 4": the topology's accepted totals, for a refusal message. */
static char *sgi_machine_accepted_totals(const SGIModelTopology *t)
{
    GString *out = g_string_new(NULL);
    const SGIModelCpuPopulationList *entry;
    unsigned int last = 0;

    /* Each pass appends the next larger total, so each appears once. */
    for (;;) {
        unsigned int next = UINT_MAX;

        for (entry = sgi_machine_view_row(t)->supported_cpu_populations;
             entry; entry = entry->next) {
            unsigned int total = sgi_machine_population_total(entry->value);

            if (total > last && total < next) {
                next = total;
            }
        }
        if (next == UINT_MAX) {
            break;
        }
        g_string_append_printf(out, "%s%u", out->len ? ", " : "", next);
        last = next;
    }
    /* The last separator reads "or": "1, 2 or 4". */
    {
        char *comma = strrchr(out->str, ',');

        if (comma) {
            g_string_erase(out, comma - out->str, 1);
            g_string_insert(out, comma - out->str, " or");
        }
    }
    return g_string_free(out, false);
}

/*
 * Validate that the per-node memory size (-m split evenly across every
 * node) matches this node's accepted population, per
 * SGIModelNode.accepted-memory-mib: a present list is the DIMM kit
 * sizes the node accepts, and an absent one means the node's own
 * shipped bank-mib total is the only accepted size, not an open
 * fitting rule.
 */
static bool sgi_machine_validate_memory_population(
    const SGIModelTopology *t, uint64_t per_node_mib)
{
    g_autofree const SGIModelNode **nodes = NULL;
    size_t node_count, node_index;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    for (node_index = 0; node_index < node_count; node_index++) {
        const SGIModelNode *node = nodes[node_index];

        if (!node->accepted_memory_mib) {
            g_autofree uint64_t *bank_mib = NULL;
            uint64_t shipped_mib = 0;
            size_t bank_count, bank;

            bank_mib = sgi_model_array_uint64(node->bank_mib, &bank_count);
            for (bank = 0; bank < bank_count; bank++) {
                shipped_mib += bank_mib[bank];
            }
            if (per_node_mib != shipped_mib) {
                return false;
            }
        } else {
            g_autofree uint64_t *accepted = NULL;
            size_t accepted_count, i;
            bool found = false;

            accepted = sgi_model_array_uint64(node->accepted_memory_mib,
                                              &accepted_count);
            for (i = 0; i < accepted_count; i++) {
                if (accepted[i] == per_node_mib) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
    }
    return true;
}

/* ---- possible_cpu_arch_ids and friends ---- */

static const CPUArchIdList *sgi_machine_possible_cpu_arch_ids(
    MachineState *machine)
{
    const SGIModelTopology *t = sgi_machine_topology(machine);
    unsigned int max_cpus = machine->smp.max_cpus;
    unsigned int node_count = t ? sgi_machine_topology_node_count(t) : 1;
    const SGIModelCpuPopulation *population = NULL;
    g_autofree unsigned int *cpus_per_node = NULL;
    g_autofree unsigned int *node_of = NULL;
    size_t population_node_count = 0;
    unsigned int node_index, slice, index;

    if (machine->possible_cpus) {
        assert(machine->possible_cpus->len == max_cpus);
        return machine->possible_cpus;
    }
    machine->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                       sizeof(CPUArchId) * max_cpus);
    machine->possible_cpus->len = max_cpus;

    if (t) {
        const SGIModelCpuPopulation *second;

        population = sgi_machine_find_population_by_total(
            t, max_cpus, SGI_MACHINE(machine)->population, &second);
    }
    if (population) {
        cpus_per_node = sgi_model_array_uint32(population->cpus_per_node,
                                               &population_node_count);
    }

    /*
     * One node per slot in the selected population's order, then node 0
     * for whatever -smp overshoots it (an unrecognized total or a
     * second matching population): sgi_machine_realize reports the
     * mismatch clearly, so this table just needs to be complete for
     * NUMA parsing to reject from meanwhile. Every socket is its own
     * arch-id slot; core and thread are always 0.
     */
    node_of = g_new0(unsigned int, max_cpus);
    index = 0;
    if (population) {
        for (node_index = 0; node_index < node_count &&
             node_index < population_node_count && index < max_cpus;
             node_index++) {
            for (slice = 0; slice < cpus_per_node[node_index]
                 && index < max_cpus; slice++, index++) {
                node_of[index] = node_index;
            }
        }
    }
    for (; index < max_cpus; index++) {
        node_of[index] = 0;
    }

    for (index = 0; index < max_cpus; index++) {
        CPUArchId *slot = &machine->possible_cpus->cpus[index];

        slot->type = machine->cpu_type;
        slot->vcpus_count = 1;
        slot->arch_id = index;
        slot->props.has_socket_id = true;
        slot->props.socket_id = index;
        slot->props.has_core_id = true;
        slot->props.core_id = 0;
        slot->props.has_thread_id = true;
        slot->props.thread_id = 0;
        slot->props.has_node_id = true;
        slot->props.node_id = node_of[index];
    }
    return machine->possible_cpus;
}

static CpuInstanceProperties sgi_machine_cpu_index_to_props(
    MachineState *machine, unsigned int cpu_index)
{
    const CPUArchIdList *possible_cpus =
        sgi_machine_possible_cpu_arch_ids(machine);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}

static int64_t sgi_machine_get_default_cpu_node_id(
    const MachineState *machine, int index)
{
    const CPUArchIdList *possible_cpus =
        sgi_machine_possible_cpu_arch_ids((MachineState *)machine);

    assert(index >= 0 && (unsigned int)index < possible_cpus->len);
    return possible_cpus->cpus[index].props.socket_id;
}

/* ---- Onboard and Crossbow-attached I/O boards ---- */

/*
 * Split t->xio_attachments[] into each node's own entry and each
 * io_boards[] entry's placement. A direct board (crossbar_index < 0)
 * shares its one entry with its node. A Crossbow-attached node's first
 * entry in array order is always its own host entry, so a later entry
 * repeating a node index already seen is that node's board.
 */
static bool sgi_machine_index_xio_attachments(
    const SGIModelTopology *t, unsigned int *node_attachment,
    unsigned int *board_attachment, Error **errp)
{
    size_t node_count = sgi_machine_topology_node_count(t);
    size_t io_board_count = sgi_machine_topology_io_board_count(t);
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    size_t attachment_count;
    g_autofree bool *claimed = g_new0(bool, node_count);
    unsigned int board = 0;
    size_t i;

    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                 &attachment_count);

    for (i = 0; i < attachment_count; i++) {
        const SGIModelXioAttachment *attachment = attachments[i];
        unsigned int node_index = attachment->node_index;

        if (node_index >= node_count) {
            error_setg(errp, "topology '%s' XIO attachment %zu names "
                       "node %u, past its %zu nodes", t->name, i, node_index,
                       node_count);
            return false;
        }
        if (attachment->crossbar_index < 0) {
            node_attachment[node_index] = i;
            claimed[node_index] = true;
            if (board >= io_board_count) {
                error_setg(errp, "topology '%s' names more direct-attach or "
                          "board-owning XIO attachments than its %zu "
                          "io board(s)", t->name, io_board_count);
                return false;
            }
            board_attachment[board++] = i;
            continue;
        }
        if (!claimed[node_index]) {
            claimed[node_index] = true;
            node_attachment[node_index] = i;
        } else {
            if (board >= io_board_count) {
                error_setg(errp, "topology '%s' names more direct-attach or "
                          "board-owning XIO attachments than its %zu "
                          "io board(s)", t->name, io_board_count);
                return false;
            }
            board_attachment[board++] = i;
        }
    }
    /*
     * A node no attachment entry ever claimed is a topology fault. It is
     * caught here because this function resolves each node's own
     * attachment before the board path builds anything.
     */
    for (i = 0; i < node_count; i++) {
        if (!claimed[i]) {
            error_setg(errp, "topology '%s' node %zu names no direct-attach "
                      "I/O board or XIO attachment", t->name, i);
            return false;
        }
    }
    if (board != io_board_count) {
        error_setg(errp, "topology '%s' names %zu io board(s) but only %u "
                  "XIO attachment(s) own one", t->name,
                  io_board_count, board);
        return false;
    }
    return true;
}

/*
 * The console node's IOC3 port A is serial line 0; other nodes follow
 * in index order from line 6.
 */
static unsigned int sgi_machine_compute_serial_line(
    const SGIModelTopology *t, unsigned int node_index, unsigned int offset)
{
    unsigned int position;

    if (node_index == t->console) {
        position = 0;
    } else if (node_index < t->console) {
        position = node_index + 1;
    } else {
        position = node_index;
    }
    return position * SN0_SERIAL_LINES_PER_NODE + offset;
}

/*
 * A crossbar's manufacturer id and part number are fixed by which chip
 * it is (docs/designs/sn-machine-model-architecture.md's own
 * SGIModelCrossbarKind split: a standalone Crossbow versus an XBridge's
 * internal crossbar section), the same on every machine fitting either,
 * so the compiled catalogue no longer carries them. The manufacturer id
 * is zero for both kinds measured so far; the part number is what every
 * measured configuration's descriptor named before the ownership-rule
 * trim.
 */
#define SGI_MACHINE_CROSSBAR_MANUFACTURER 0

static uint32_t sgi_machine_crossbar_part(SGIModelCrossbarKind kind)
{
    switch (kind) {
    case SGI_MODEL_CROSSBAR_KIND_XBOW:
        return 0;
    case SGI_MODEL_CROSSBAR_KIND_XBRIDGE:
        return 0xd000;
    default:
        g_assert_not_reached();
    }
}

/* The module an XIO slot's Crossbow sits in. */
static uint32_t sgi_machine_xio_slot_module(const SGIModelTopology *t,
                                            const SGIModelXioSlot *slot)
{
    g_autofree const SGIModelCrossbar **crossbars = NULL;
    size_t crossbar_count;

    crossbars = sgi_model_crossbar_array(t->crossbars, &crossbar_count);
    g_assert(slot->crossbar_index < crossbar_count);
    return crossbars[slot->crossbar_index]->module_index;
}

/*
 * An XIO slot's name, unique within the machine: its IRIX label (io5),
 * or m<module>-io5 where another module has a slot of the same label.
 */
static char *sgi_machine_xio_slot_name(const SGIModelTopology *t,
                                       const SGIModelXioSlot *slot)
{
    const SGIModelXioSlotList *entry;
    uint32_t module = sgi_machine_xio_slot_module(t, slot);

    for (entry = t->xio_slots; entry; entry = entry->next) {
        if (entry->value != slot &&
            g_str_equal(entry->value->label, slot->label) &&
            sgi_machine_xio_slot_module(t, entry->value) != module) {
            return g_strdup_printf("m%u-%s", module, slot->label);
        }
    }
    return g_strdup(slot->label);
}

static void sgi_machine_build_crossbars(SGIMachineState *s,
                                        const SGIModelTopology *t)
{
    g_autofree const SGIModelCrossbar **crossbars = NULL;
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    size_t crossbar_count, attachment_count, i;

    crossbars = sgi_model_crossbar_array(t->crossbars, &crossbar_count);
    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                 &attachment_count);
    for (i = 0; i < crossbar_count; i++) {
        const SGIModelCrossbar *descriptor = crossbars[i];
        DeviceState *crossbow = qdev_new(TYPE_SGI_XBOW);
        g_autofree char *name = g_strdup_printf("crossbow%zu", i);
        uint16_t startup_alive = 0;
        size_t attachment_index;

        /*
         * The widgets this topology attaches: the only ports with a
         * widget physically present, and each reported alive at startup.
         * An empty XIO slot has no widget, so its port reads presence
         * clear (link auxiliary status bit 5);
         * reporting it present would make the PROM find a widget whose
         * link never came up and diagnose it as a bad IO link. An empty
         * slot becomes present only when a board fitted at launch claims
         * it (sgi_machine_done()).
         */
        for (attachment_index = 0; attachment_index < attachment_count;
             attachment_index++) {
            const SGIModelXioAttachment *attachment =
                attachments[attachment_index];

            if ((unsigned int)attachment->crossbar_index == i) {
                startup_alive |= 1u << attachment->widget;
            }
        }

        s->crossbars[i] = SGI_XBOW(crossbow);
        object_property_add_child(OBJECT(s), name, OBJECT(crossbow));
        object_unref(OBJECT(crossbow));
        qdev_prop_set_uint64(crossbow, "span", SN0_XIO_WIDGET_SIZE);
        qdev_prop_set_bit(crossbow,
                          "complete-posted-error-address-writes", true);
        qdev_prop_set_uint32(crossbow, "revision",
                             descriptor->widget_revision);
        qdev_prop_set_uint32(crossbow, "part",
                             sgi_machine_crossbar_part(descriptor->kind));
        qdev_prop_set_uint32(crossbow, "manufacturer",
                             SGI_MACHINE_CROSSBAR_MANUFACTURER);
        qdev_prop_set_uint16(crossbow, "links-present", startup_alive);
        qdev_prop_set_uint16(crossbow, "startup-links-alive", startup_alive);
        /*
         * Stock PROM resets populated links and expects one alive before
         * the reset to return to service after the pulse.
         */
        qdev_prop_set_uint16(crossbow, "link-reset-restore-alive",
                             startup_alive);

    }
}

/*
 * A catalogue-built XIO board's own placement: the widget its attachment
 * names, which the builder wires through that board's own crossbow link.
 */
static void sgi_machine_set_board_widget(DeviceState *dev, uint32_t widget)
{
    g_autofree char *slot = g_strdup_printf("%u", widget);

    qdev_prop_set_string(dev, "slot", slot);
}

/*
 * A catalogue-built XIO board's QOM name and qdev id: its IRIX slot name
 * (io1, io5), or m<module>-io5 where a board or empty slot in another
 * module shares the label, so screendump device= and a card's own
 * -device bus=io5.0 name it the way IRIX's /hw/module/1/slot/io5 does.
 * A board the catalogue gives no label keeps io-board<N>.
 */
static char *sgi_machine_xio_board_name(const SGIModelTopology *t,
                                        const SGIModelIoBoard *record,
                                        unsigned int board_index)
{
    const SGIModelIoBoardList *board;
    const SGIModelXioSlotList *slot;

    if (!record->label) {
        return g_strdup_printf("io-board%u", board_index);
    }
    for (board = t->io_boards; board; board = board->next) {
        if (board->value != record && board->value->label &&
            g_str_equal(board->value->label, record->label) &&
            board->value->module_index != record->module_index) {
            return g_strdelimit(g_strdup_printf("m%u-%s",
                                               record->module_index,
                                               record->label), " ", '-');
        }
    }
    for (slot = t->xio_slots; slot; slot = slot->next) {
        if (g_str_equal(slot->value->label, record->label) &&
            sgi_machine_xio_slot_module(t, slot->value) !=
            record->module_index) {
            return g_strdelimit(g_strdup_printf("m%u-%s",
                                               record->module_index,
                                               record->label), " ", '-');
        }
    }
    return g_strdelimit(g_strdup(record->label), " ", '-');
}

/*
 * Parent a catalogue-built XIO board under the machine by its slot name,
 * give it that name as its qdev id, and place it at its module and
 * widget. The id also names the board's PCI bus, <id>.0.
 */
static void sgi_machine_add_xio_board(SGIMachineState *s, DeviceState *dev,
                                      const SGIModelIoBoard *record,
                                      unsigned int board_index,
                                      const SGIModelXioAttachment *attachment)
{
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));
    g_autofree char *name = sgi_machine_xio_board_name(t, record,
                                                       board_index);

    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    object_unref(OBJECT(dev));
    /* Set directly: qdev_set_id() would reparent under /machine/peripheral. */
    dev->id = g_strdup(name);
    qdev_prop_set_uint32(dev, "module", record->module_index);
    sgi_machine_set_board_widget(dev, attachment->widget);
}

/*
 * The onboard IOC3's own separate one-wire identity part (the carrier's
 * chip identity is a different part); NULL when this preset's IOC3 head
 * carries no population, leaving sgi-baseio's own class default.
 */
static const uint64_t *sgi_machine_io_board_ioc3_serial(
    const SGIModelIoBoard *record)
{
    return record->has_ioc3_serial ? &record->ioc3_serial : NULL;
}

/*
 * The GIGAchannel's Crosstown extension board, fitted in place of the
 * XTalk jumper, and its own board record: part 030-1264-001 rev C, named
 * XBOX_EXT, which real inventories list beside the IP29 record under
 * io1/MotherBoard. That it answers on the IP29 Bridge's own one-wire line
 * is inferred from that grouping, the way the BaseIO-G's MIO record shares
 * its Bridge's line. The upgrade guide's one loopback example prints a rev
 * B board as 2G_MPLN; this model fits the rev C board whichever way the
 * module is cabled. The barcode is a placeholder, and the serial only has
 * to read back distinct from the IP29 record's; it continues the
 * descending-byte placeholder idiom past the MIO's and the Cad Duo's.
 */
static const SGIBridgeBoardRecord sgi_machine_crosstown_record = {
    .barcode = "OXBOXEXT01",
    .part = "030-1264-001",
    .revision = "C",
    .name = "XBOX_EXT",
};
#define SGI_MACHINE_CROSSTOWN_ONEWIRE_SERIAL 0x0e0d0c0b0a09ULL

/*
 * IP29's onboard I/O, one per Origin 200 node: linked to its node's own
 * Hub, or to a GIGAchannel cabinet's Crossbow port. sn0.c owns the
 * Bridge, the sgi-baseio behind it and the link (see
 * sn0_node_build_baseio()'s own comment for why); this function resolves
 * every fitted-part value and backend id the descriptor and command line
 * carry, the same policy the Crossbow-attached BaseIO card below applies,
 * and names the port the Bridge links to.
 */
static bool sgi_machine_instantiate_ip29_io(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXioAttachment *attachment, Error **errp)
{
    SN0NodeState *node = s->nodes[attachment->node_index];
    Object *upstream = OBJECT(&node->hub);
    unsigned int upstream_index = 0;
    DeviceState *bridge;
    const uint64_t *ioc3_serial = sgi_machine_io_board_ioc3_serial(record);
    /*
     * "pci" for the first onboard board, "pci.<N>" for a second: a
     * two-chassis Origin 200's two onboard Bridges would otherwise share
     * a migration-visible PCI bus name.
     */
    g_autofree char *bus_name = board_index == 0 ? g_strdup("pci") :
        g_strdup_printf("pci.%u", board_index);
    g_autofree char *nvram_id = g_strdup_printf("sgi-nvram%u", board_index);
    DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, board_index);
    BlockBackend *nvram_blk = nvram_dinfo ?
        blk_by_legacy_dinfo(nvram_dinfo) : blk_by_name(nvram_id);
    Chardev *chardev_a = serial_hd(sgi_machine_compute_serial_line(
        t, attachment->node_index, SN0_IOC3_SERIAL_A_CHARDEV));
    Chardev *chardev_b = serial_hd(sgi_machine_compute_serial_line(
        t, attachment->node_index, SN0_IOC3_SERIAL_B_CHARDEV));
    Chardev *record_a = serial_hd(sgi_machine_compute_serial_line(
        t, attachment->node_index, SN0_IOC3_RECORD_A_CHARDEV));
    Chardev *record_b = serial_hd(sgi_machine_compute_serial_line(
        t, attachment->node_index, SN0_IOC3_RECORD_B_CHARDEV));
    const char *parallel_id = board_index < MAX_PARALLEL_PORTS &&
        parallel_hds[board_index] ? parallel_hds[board_index]->label : NULL;

    if (attachment->crossbar_index >= 0) {
        upstream = OBJECT(s->crossbars[attachment->crossbar_index]);
        upstream_index = attachment->widget;
    }
    if (!sn0_node_build_baseio(
            node, OBJECT(s), &s->board_links[board_index], upstream,
            upstream_index, record->serial, ioc3_serial, bus_name,
            nvram_blk ? blk_name(nvram_blk) : NULL,
            chardev_a ? chardev_a->label : NULL,
            chardev_b ? chardev_b->label : NULL,
            record_a ? record_a->label : NULL,
            record_b ? record_b->label : NULL, parallel_id, &bridge, errp)) {
        return false;
    }
    if (record->preset == SGI_MODEL_IO_BOARD_PRESET_IP29_IO_CROSSTOWN &&
        !sgi_bridge_add_onewire_board_part(
            bridge, &sgi_machine_crosstown_record,
            SGI_MACHINE_CROSSTOWN_ONEWIRE_SERIAL, errp)) {
        return false;
    }
    return true;
}

/*
 * A graphics head returns the credits its port takes to its master node,
 * the node whose I/O interface its Crossbow port reaches: that node's
 * graphics node-widget map routes them on to the processor that paid.
 * Every board with a "pi" link gets that node's Hub, or its Bedrock; a
 * board given one outright keeps it.
 */
static void sgi_machine_link_board_pi(SGIMachineState *s, Object *board,
                                      unsigned int node_index)
{
    Object *node;

    if (!object_property_find(board, "pi") ||
        object_property_get_link(board, "pi", &error_abort)) {
        return;
    }
    node = s->nodes ? OBJECT(&s->nodes[node_index]->hub) :
        OBJECT(SN1_NODE(s->bedrock_nodes[node_index])->bedrock);
    object_property_set_link(board, "pi", node, &error_abort);
}

/*
 * Link a Crossbow-attached XIO card @dev to its Crossbow port at
 * @attachment's widget and realize it: the card takes its fabric from the
 * link before it builds its widget, and the Crossbow takes the widget's
 * faces once it has. The Crossbow itself realizes later, once every board
 * and node that attaches to it has (see sgi_machine_realize_crossbars()).
 */
static bool sgi_machine_attach_xio_card(
    SGIMachineState *s, DeviceState *dev, unsigned int board_index,
    const SGIModelXioAttachment *attachment, Error **errp)
{
    SGIXioLink *link = &s->board_links[board_index];
    g_autofree char *name = g_strdup_printf("io-board%u-link", board_index);

    sgi_xio_link_init(link, OBJECT(s), name,
                      OBJECT(s->crossbars[attachment->crossbar_index]),
                      attachment->widget, OBJECT(dev), 0);
    sgi_machine_link_board_pi(s, OBJECT(dev), attachment->node_index);
    if (!sgi_xio_link_connect(link, errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(dev), errp) ||
        !sgi_xio_link_connect(link, errp)) {
        return false;
    }
    s->io_boards[board_index] = SGI_XIO_BOARD(dev);
    return true;
}

static bool sgi_machine_instantiate_baseio_card(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXioAttachment *attachment, const char *preset,
    Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_SGI_XIO_BASEIO);
    g_autofree char *nvram_id = g_strdup_printf("sgi-nvram%u", board_index);
    DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, board_index);
    BlockBackend *nvram_blk = nvram_dinfo ?
        blk_by_legacy_dinfo(nvram_dinfo) : blk_by_name(nvram_id);
    /*
     * The card's BASEIO PROM flash, by name only: -drive if=none,
     * id=sgi-baseio-flash<N>. The pflash units are the node PROMs.
     */
    g_autofree char *flash_id =
        g_strdup_printf("sgi-baseio-flash%u", board_index);
    BlockBackend *flash_blk = blk_by_name(flash_id);
    static const struct {
        unsigned int offset;
        const char *prop;
    } serial_bindings[] = {
        { SN0_IOC3_SERIAL_A_CHARDEV, "chardev-a" },
        { SN0_IOC3_SERIAL_B_CHARDEV, "chardev-b" },
        { SN0_IOC3_RECORD_A_CHARDEV, "record-a" },
        { SN0_IOC3_RECORD_B_CHARDEV, "record-b" },
    };
    size_t i;

    sgi_machine_add_xio_board(s, dev, record, board_index, attachment);
    qdev_prop_set_uint64(dev, "onewire-serial", record->serial);
    if (record->has_ioc3_serial) {
        qdev_prop_set_uint64(dev, "ioc3-onewire-serial", record->ioc3_serial);
    }
    qdev_prop_set_string(dev, "preset", preset);
    if (nvram_blk) {
        qdev_prop_set_string(dev, "nvram-drive", blk_name(nvram_blk));
    }
    if (flash_blk) {
        qdev_prop_set_string(dev, "flash-drive", blk_name(flash_blk));
    }
    for (i = 0; i < ARRAY_SIZE(serial_bindings); i++) {
        Chardev *chr = serial_hd(sgi_machine_compute_serial_line(
            t, attachment->node_index, serial_bindings[i].offset));

        if (chr) {
            qdev_prop_set_string(dev, serial_bindings[i].prop, chr->label);
        }
    }
    /*
     * The MediaIO daughtercard's parallel port, by board index the way
     * sgi_machine_instantiate_ip29_io() binds the Origin 200's. The plain
     * card has no parallel port, and QEMU's default "-parallel" backend
     * exists whether or not one was asked for, so only io6g takes it.
     */
    if (g_str_equal(preset, "io6g") && board_index < MAX_PARALLEL_PORTS &&
        parallel_hds[board_index]) {
        qdev_prop_set_string(dev, "parallel-chardev",
                             parallel_hds[board_index]->label);
    }
    return sgi_machine_attach_xio_card(s, dev, board_index, attachment, errp);
}

/*
 * The Origin 2000 PCI shoehorn: a removable XIO card like the BaseIO,
 * carrying one PCI card position, which this preset fills with an SGI Cad
 * Duo. The preset fits the card at PCI device 1, where real Origin 2000
 * hardware graphs show cards behind this adapter: a Cad Duo's keyboard at
 * /hw/module/1/slot/io5/xtalk_pci/pci/1/pckb (nekonomicon thread 5749) and
 * a SCSI card at .../io5/xtalk_pci/pci/1/scsi_ctlr/0 (thread 16727716).
 * docs/machines/layers/modules/pci-shoehorn.yaml records the same. The
 * board's own identity and the card's one-wire part come from the record.
 */
#define SGI_MACHINE_PCI_SHOEHORN_CAD_DUO_SLOT 1

/*
 * The Origin 2000 PCI Carrier Assembly: three PCI slots for third-party
 * cards (007-3456-003 printed page 34), empty until a -device fills one on
 * the carrier's bus, io2.0 in IO2. sgi-pci-carrier (board-pci-expansion.c)
 * carries the carrier's slots and board identity itself.
 */
static bool sgi_machine_instantiate_pci_carrier(
    SGIMachineState *s, const SGIModelIoBoard *record,
    unsigned int board_index, const SGIModelXioAttachment *attachment,
    Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_SGI_PCI_CARRIER);

    sgi_machine_add_xio_board(s, dev, record, board_index, attachment);
    qdev_prop_set_uint64(dev, "onewire-serial", record->serial);
    return sgi_machine_attach_xio_card(s, dev, board_index, attachment, errp);
}

/*
 * The GIGAchannel cabinet's own I/O on Crossbow link 8, software position
 * io7: an sgi-gigachannel (board-gigachannel.c), which owns the cabinet
 * Bridge, its board record, the cabinet's status word and its BASEIO PROM
 * flash. The cabinet's own Bridge sits in no XIO slot the catalogue
 * labels, but the PROM names it io7 by that fixed position, so it takes
 * io7 as its id and bus io7.0.
 */
#define SGI_MACHINE_GIGACHANNEL_NAME "io7"

static bool sgi_machine_instantiate_gigachannel(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXioAttachment *attachment, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_SGI_GIGACHANNEL);
    const SGIModelIoBoardList *entry;
    unsigned int cabinet = 0;
    g_autofree char *flash_id = NULL;
    BlockBackend *flash_blk;

    /*
     * The cabinet's BASEIO PROM flash, by name only: -drive if=none,
     * id=sgi-gigachannel-flash<N>, N counting cabinets rather than
     * io_boards entries, so the first cabinet's is 0 whatever else the
     * topology fits ahead of it.
     */
    for (entry = t->io_boards; entry && entry->value != record;
         entry = entry->next) {
        if (entry->value->preset == SGI_MODEL_IO_BOARD_PRESET_GIGACHANNEL) {
            cabinet++;
        }
    }
    flash_id = g_strdup_printf("sgi-gigachannel-flash%u", cabinet);
    flash_blk = blk_by_name(flash_id);

    object_property_add_child(OBJECT(s), SGI_MACHINE_GIGACHANNEL_NAME,
                              OBJECT(dev));
    object_unref(OBJECT(dev));
    /* Set directly: qdev_set_id() would reparent under /machine/peripheral. */
    dev->id = g_strdup(SGI_MACHINE_GIGACHANNEL_NAME);
    qdev_prop_set_uint32(dev, "module", record->module_index);
    sgi_machine_set_board_widget(dev, attachment->widget);
    qdev_prop_set_uint64(dev, "onewire-serial", record->serial);
    if (flash_blk) {
        qdev_prop_set_string(dev, "flash-drive", blk_name(flash_blk));
    }
    return sgi_machine_attach_xio_card(s, dev, board_index, attachment, errp);
}

static bool sgi_machine_instantiate_pci_shoehorn(
    SGIMachineState *s, const SGIModelIoBoard *record,
    unsigned int board_index, const SGIModelXioAttachment *attachment,
    Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_SGI_PCI_SHOEHORN);

    sgi_machine_add_xio_board(s, dev, record, board_index, attachment);
    qdev_prop_set_uint64(dev, "onewire-serial", record->serial);
    qdev_prop_set_uint32(dev, "cad-duo-slot",
                         SGI_MACHINE_PCI_SHOEHORN_CAD_DUO_SLOT);
    if (record->has_ioc3_serial) {
        qdev_prop_set_uint64(dev, "cad-duo-onewire-serial",
                             record->ioc3_serial);
    }
    return sgi_machine_attach_xio_card(s, dev, board_index, attachment, errp);
}

/*
 * An XIO card the catalogue places in any slot: an sgi-xio-baseio with the
 * "menet" or "mscsi" preset, which owns the card's Bridge, board record
 * and chips, on its own PCI bus <slot>.0 (io5.0). The MENET's catalogue
 * IOC3 serial is device 0's one-wire part; the preset numbers the other
 * three from it. The chips' serial ports take no host backend: the
 * console lines belong to the module's BaseIO. The MSCSI has no IOC3.
 */
static bool sgi_machine_instantiate_xio_card(
    SGIMachineState *s, const SGIModelIoBoard *record,
    unsigned int board_index, const SGIModelXioAttachment *attachment,
    const char *preset, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_SGI_XIO_BASEIO);

    sgi_machine_add_xio_board(s, dev, record, board_index, attachment);
    qdev_prop_set_uint64(dev, "onewire-serial", record->serial);
    if (record->has_ioc3_serial) {
        qdev_prop_set_uint64(dev, "ioc3-onewire-serial", record->ioc3_serial);
    }
    qdev_prop_set_string(dev, "preset", preset);
    return sgi_machine_attach_xio_card(s, dev, board_index, attachment, errp);
}

/*
 * The Origin 2000/Onyx2 InfiniteReality graphics pipe's KONA widget:
 * Crossbow-attached like the BaseIO card above, but an sgi-kona instance
 * carries no PCI function, IOC3 or onewire-serial property of its own --
 * increment 1 is identity only, so this function sets only what
 * sgi-xio-board's base class and sgi-kona's own "fabric" property
 * require. The onyx2 topologies fit one, the rack at IO3 and the deskside
 * at IO4; a `-device sgi-kona` reaches the same board without a catalogue
 * record.
 */
static bool sgi_machine_instantiate_kona(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXioAttachment *attachment, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_SGI_KONA);

    /*
     * The board's slot name is its qdev id, so QMP's screendump names
     * this head device=io3 on the rack and device=io4 on the deskside,
     * the slot IRIX names it by (issue #895).
     */
    sgi_machine_add_xio_board(s, dev, record, board_index, attachment);
    /*
     * The pipe's one-wire records: the geometry board's own part, the
     * rack's GE16-4 or the deskside's GE14-4, then the records after it,
     * the rack's Ktown or none on the deskside, whose pipe has no Ktown.
     */
    qdev_prop_set_string(dev, "board-part", record->part);
    if (record->has_other_identity_parts) {
        QList *parts = qlist_new();
        const strList *part;

        for (part = record->other_identity_parts; part; part = part->next) {
            qlist_append_str(parts, part->value);
        }
        qdev_prop_set_array(dev, "other-identity-parts", parts);
    }
    return sgi_machine_attach_xio_card(s, dev, board_index, attachment, errp);
}

/*
 * The SI/MXI (MGRAS) graphics board: an XIO board whose one MMIO region
 * answers both the CPU's widget accesses and packets routed to its
 * widget, with no Bridge. It attaches like any other XIO card, and like
 * them it is named by its slot: screendump device=io3 on an Origin 2000
 * that fits one in IO3.
 */
static bool sgi_machine_instantiate_mgras(
    SGIMachineState *s, const SGIModelIoBoard *record,
    unsigned int board_index, const SGIModelXioAttachment *attachment,
    Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_SGI_MGRAS);

    sgi_machine_add_xio_board(s, dev, record, board_index, attachment);
    return sgi_machine_attach_xio_card(s, dev, board_index, attachment, errp);
}

/* The direct V12 profile occupies the Origin 300 XBridge's external port. */
static bool sgi_machine_instantiate_odyssey(
    SGIMachineState *s, const SGIModelIoBoard *record,
    unsigned int board_index, const SGIModelXioAttachment *attachment,
    Error **errp)
{
    SN1NodeState *node = SN1_NODE(s->bedrock_nodes[attachment->node_index]);
    SGIXioLink *link = &s->board_links[board_index];
    g_autofree char *name = g_strdup_printf("io-board%u-link", board_index);
    DeviceState *dev;

    if (attachment->crossbar_index >= 0 ||
        attachment->widget !=
        sgi_xbridge_xio_port(SGI_XBRIDGE(node->xbridge))) {
        error_setg(errp, "the direct V12 board must use the node "
                   "XBridge external port");
        return false;
    }
    dev = qdev_try_new("sgi-odyssey");
    if (!dev) {
        error_setg(errp, "the direct V12 profile requires the "
                   "sgi-odyssey device");
        return false;
    }
    sgi_machine_add_xio_board(s, dev, record, board_index, attachment);
    sgi_xio_link_init(link, OBJECT(s), name,
                      OBJECT(node->xbridge), attachment->widget,
                      OBJECT(dev), 0);
    sgi_machine_link_board_pi(s, OBJECT(dev), attachment->node_index);
    if (!sgi_xio_link_connect(link, errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(dev), errp) ||
        !sgi_xio_link_connect(link, errp)) {
        return false;
    }
    s->io_boards[board_index] = SGI_XIO_BOARD(dev);
    return true;
}

/*
 * Experimental Origin 300-to-V-brick route. The published enclosure has
 * its own XBridge, L1 and V12; the cable ports and nested processor address
 * are not established. The 8 MiB split is a machine-local QEMU projection:
 * V12 at the cable widget base, then the enclosure XBridge's crossbar.
 */
static bool sgi_machine_instantiate_vbrick(
    SGIMachineState *s, const SGIModelIoBoard *record,
    unsigned int board_index, const SGIModelXioAttachment *attachment,
    Error **errp)
{
    SN1NodeState *node;
    Object *module;
    DeviceState *bridge, *v12;
    Object *l1;
    SGIXioLink *board_link = &s->board_links[board_index];
    g_autofree char *module_name = NULL;
    g_autofree char *board_link_name = NULL;

    if (attachment->node_index != 0 || attachment->crossbar_index >= 0 ||
        record->module_index != 1 || attachment->widget != 0xb) {
        error_setg(errp, "the experimental V-brick requires node 0, "
                   "module 1 and XBridge external port 0xb");
        return false;
    }
    node = SN1_NODE(s->bedrock_nodes[0]);
    if (attachment->widget !=
        sgi_xbridge_xio_port(SGI_XBRIDGE(node->xbridge))) {
        error_setg(errp, "the experimental V-brick needs the node "
                   "XBridge external port at 0xb");
        return false;
    }
    module_name = g_strdup_printf("module%u", record->module_index);
    module = object_resolve_path_component(OBJECT(s), module_name);
    g_assert(module);

    bridge = qdev_new(TYPE_SGI_XBRIDGE);
    object_property_add_child(module, "vbrick-xbridge", OBJECT(bridge));
    object_unref(OBJECT(bridge));
    qdev_prop_set_uint8(bridge, "host-port", 0xa);
    qdev_prop_set_uint8(bridge, "xio-port", 8);
    qdev_prop_set_string(bridge, "bus-prefix", "vbrick-");
    sgi_xio_link_init(&s->vbrick_link, OBJECT(s), "vbrick-cable",
                      OBJECT(node->xbridge), 0xb, OBJECT(bridge), 0xa);
    if (!sgi_xio_link_connect(&s->vbrick_link, errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(bridge), errp) ||
        !sgi_xio_link_connect(&s->vbrick_link, errp)) {
        return false;
    }
    g_assert(sgi_xio_link_complete(&s->vbrick_link));

    /* Preserve the physical L1 position without an invented IP45 identity. */
    l1 = object_new(TYPE_CONTAINER);
    object_property_add_child(module, "vbrick-l1", l1);
    object_unref(l1);

    v12 = qdev_try_new("sgi-odyssey");
    if (!v12) {
        error_setg(errp, "the experimental V-brick requires the "
                   "sgi-odyssey device");
        return false;
    }
    object_property_add_child(module, "vbrick-v12", OBJECT(v12));
    object_unref(OBJECT(v12));
    v12->id = g_strdup("vbrick-v12");
    qdev_prop_set_uint32(v12, "module", record->module_index);
    sgi_machine_set_board_widget(v12, 8);
    board_link_name = g_strdup_printf("io-board%u-link", board_index);
    sgi_xio_link_init(board_link, OBJECT(s), board_link_name,
                      OBJECT(bridge), 8, OBJECT(v12), 0);
    if (!sgi_xio_link_connect(board_link, errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(v12), errp) ||
        !sgi_xio_link_connect(board_link, errp)) {
        return false;
    }
    g_assert(sgi_xio_link_complete(board_link));
    sgi_xbridge_project_vbrick_pio(
        SGI_XBRIDGE(node->xbridge),
        sgi_xbridge_widget_view(SGI_XBRIDGE(bridge), 8),
        sgi_xbridge_widget_view(SGI_XBRIDGE(bridge), 0), 8 * MiB);
    s->io_boards[board_index] = SGI_XIO_BOARD(v12);
    return true;
}

/*
 * Bedrock-board I/O: an sgi-baseio preset on the node's XBridge head F,
 * reached through
 * sn1_node_xbridge_pci_bus() (sn1-node.c) with this attachment's own
 * widget number -- the same attachment record every other product's
 * direct-attach board reads its widget from (Origin 200's onboard
 * Bridge in sgi_machine_instantiate_ip29_io(); an attachment with
 * crossbar_index -1, so sgi_machine_index_xio_attachments() already
 * buckets it as a board-owning entry rather than a Crossbow one). The
 * XBridge always answers both heads. The descriptor marks empty heads
 * separately from fitted boards whose identity is unknown. Resolves every
 * backend id from this record and the command line, the policy
 * sn1_node_build_io8() applied before this step moved it here.
 */
static bool sgi_machine_instantiate_bedrock_io(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXioAttachment *attachment, Error **errp)
{
    SN1NodeState *node = SN1_NODE(s->bedrock_nodes[attachment->node_index]);
    DeviceState *baseio;
    PCIBus *bus;
    const SGIMachineIoConfig *config = &s->io_config[board_index];
    const uint8_t *revision = config->pci_revision;

    if (!record->populated) {
        return true;
    }

    bus = sn1_node_xbridge_pci_bus(node, attachment->widget);
    baseio = qdev_new(TYPE_SGI_BASEIO);
    object_property_add_child(OBJECT(node), "baseio", OBJECT(baseio));
    qdev_prop_set_string(baseio, "preset",
                         SGIModelIoBoardPreset_str(record->preset));
    if (record->system_timekeeper) {
        SGIModelSystemTimekeeper *clock = record->system_timekeeper;

        if (clock->kind != SGI_MODEL_SYSTEM_TIMEKEEPER_KIND_M48T35_COMPATIBLE ||
            clock->year_base != 1968 || clock->initial_ip27_fixture) {
            error_setg(errp, "unsupported fitted system Timekeeper policy");
            return false;
        }
        qdev_prop_set_uint8(baseio, "system-timekeeper-kind",
                            SGI_IOC3_TIMEKEEPER_M48T35);
        qdev_prop_set_uint32(baseio, "system-timekeeper-year-base",
                             clock->year_base);
    }
    qdev_prop_set_uint32(baseio, "ioc3-subsystem-id",
                         config->ioc3_subsystem_id);
    qdev_prop_set_uint32(baseio, "scsi-revision",
                         revision[SGI_MODEL_PCI_FUNCTION_KIND_ISP12160]);
    qdev_prop_set_uint32(baseio, "ioc3-revision",
                         revision[SGI_MODEL_PCI_FUNCTION_KIND_IOC3]);
    qdev_prop_set_uint32(baseio, "usb-revision",
                         revision[SGI_MODEL_PCI_FUNCTION_KIND_LUCENT_OHCI]);
    if (config->mac_source == SGI_MODEL_MAC_SOURCE_MODULE_L1) {
        uint8_t mac[6];

        if (sgi_l1_board_mac(node->l1, mac)) {
            g_autofree char *address = g_strdup_printf(
                "%02x:%02x:%02x:%02x:%02x:%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

            qdev_prop_set_string(baseio, "ioc3-mac", address);
        }
    }
    object_property_set_link(OBJECT(baseio), "pci-bus", OBJECT(bus),
                             &error_abort);
    if (record->has_ioc3_serial) {
        qdev_prop_set_uint64(baseio, "onewire-serial", record->ioc3_serial);
    }
    /*
     * The onboard IOC3's Timekeeper storage: an indexed `-drive if=mtd`
     * first, else a bare `-drive if=none,id=sgi-nvramN` by name, N the
     * IO-8's own node (each chassis carries one IO-8). Passed as the
     * resolved backend's own name,
     * not the backend itself, for the claim-timing reason sgi-baseio's
     * own header comment gives.
     */
    {
        unsigned int node_index = attachment->node_index;
        DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, node_index);
        g_autofree char *nvram_name = g_strdup_printf("sgi-nvram%u",
                                                      node_index);
        BlockBackend *nvram_blk = nvram_dinfo ?
            blk_by_legacy_dinfo(nvram_dinfo) : blk_by_name(nvram_name);

        if (nvram_blk) {
            qdev_prop_set_string(baseio, "nvram-drive", blk_name(nvram_blk));
        }
    }
    /*
     * The onboard IOC3's serial and parallel backends, named by chardev
     * id for the same claim-timing reason. Each node's block of lines
     * starts with its L1 -- the node's own controller, not this board's --
     * so the onboard IOC3 takes the block's next four
     * (sgi_sn1_serial_line()). The one parallel port is node 0's.
     */
    {
        unsigned int node_index = attachment->node_index;
        Chardev *chr;

        chr = serial_hd(sgi_sn1_serial_line(node_index, 1));
        if (chr) {
            qdev_prop_set_string(baseio, "chardev-a", chr->label);
        }
        chr = serial_hd(sgi_sn1_serial_line(node_index, 2));
        if (chr) {
            qdev_prop_set_string(baseio, "chardev-b", chr->label);
        }
        chr = serial_hd(sgi_sn1_serial_line(node_index, 3));
        if (chr) {
            qdev_prop_set_string(baseio, "record-a", chr->label);
        }
        chr = serial_hd(sgi_sn1_serial_line(node_index, 4));
        if (chr) {
            qdev_prop_set_string(baseio, "record-b", chr->label);
        }
        if (node_index == 0 && parallel_hds[0]) {
            qdev_prop_set_string(baseio, "parallel-chardev",
                                 parallel_hds[0]->label);
        }
    }
    return qdev_realize(DEVICE(baseio), NULL, errp);
}

/*
 * Build one io_boards entry, wire it to its own XIO position and
 * realize it: IP29's onboard I/O (direct-attach), the Origin 2000
 * BaseIO card (Crossbow-attach) and the Origin 300 IO-8 (a Bedrock
 * node's own XBridge head) are different enough carriers now (sn0.c
 * owns one outright, sn1-node.c exposes the second as a link property,
 * the third is an sgi-xio-board instance this file builds) that preset
 * alone -- not attachment kind -- decides which function runs;
 * tests/unit/test-sn-model.c checks the compiled descriptor names no
 * other preset once, so this dispatch trusts it and only reports the
 * case it does not recognize.
 */
static bool sgi_machine_instantiate_io_board(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXioAttachment *attachment, Error **errp)
{
    if (!record->populated) {
        return true;
    }
    switch (record->preset) {
    case SGI_MODEL_IO_BOARD_PRESET_IP29_IO:
    case SGI_MODEL_IO_BOARD_PRESET_IP29_IO_CROSSTOWN:
        return sgi_machine_instantiate_ip29_io(s, t, record, board_index,
                                               attachment, errp);
    case SGI_MODEL_IO_BOARD_PRESET_IO6:
        return sgi_machine_instantiate_baseio_card(
            s, t, record, board_index, attachment,
            s->baseio_preset ? s->baseio_preset : "io6", errp);
    case SGI_MODEL_IO_BOARD_PRESET_IO6G:
        return sgi_machine_instantiate_baseio_card(s, t, record, board_index,
                                                   attachment, "io6g",
                                                   errp);
    case SGI_MODEL_IO_BOARD_PRESET_IO8:
    case SGI_MODEL_IO_BOARD_PRESET_FUEL:
        return sgi_machine_instantiate_bedrock_io(s, t, record, board_index,
                                                  attachment, errp);
    case SGI_MODEL_IO_BOARD_PRESET_PCI_SHOEHORN:
        return sgi_machine_instantiate_pci_shoehorn(s, record, board_index,
                                                    attachment, errp);
    case SGI_MODEL_IO_BOARD_PRESET_PCI_CARRIER:
        return sgi_machine_instantiate_pci_carrier(s, record, board_index,
                                                   attachment, errp);
    case SGI_MODEL_IO_BOARD_PRESET_GIGACHANNEL:
        return sgi_machine_instantiate_gigachannel(s, t, record, board_index,
                                                   attachment, errp);
    case SGI_MODEL_IO_BOARD_PRESET_MENET:
        return sgi_machine_instantiate_xio_card(s, record, board_index,
                                                attachment, "menet", errp);
    case SGI_MODEL_IO_BOARD_PRESET_MSCSI:
        return sgi_machine_instantiate_xio_card(s, record, board_index,
                                                attachment, "mscsi", errp);
    case SGI_MODEL_IO_BOARD_PRESET_KONA:
        return sgi_machine_instantiate_kona(s, t, record, board_index,
                                            attachment, errp);
    case SGI_MODEL_IO_BOARD_PRESET_MGRAS:
        return sgi_machine_instantiate_mgras(s, record, board_index,
                                             attachment, errp);
    case SGI_MODEL_IO_BOARD_PRESET_ODYSSEY:
        return sgi_machine_instantiate_odyssey(s, record, board_index,
                                                attachment, errp);
    case SGI_MODEL_IO_BOARD_PRESET_VBRICK:
        return sgi_machine_instantiate_vbrick(s, record, board_index,
                                              attachment, errp);
    default:
        error_setg(errp, "topology '%s' io board %u names an unsupported "
                  "preset for this family", t->name, board_index);
        return false;
    }
}

/*
 * Realizes every Crossbow this topology built, once every node and board
 * linked to one has (sgi_xbow_attach_endpoint() refuses on an
 * already-realized Crossbow, so this can run no earlier). A board fitted
 * later by slot name attaches to its realized Crossbow live
 * (sgi_machine_done() below).
 */
static bool sgi_machine_realize_crossbars(SGIMachineState *s,
                                          const SGIModelTopology *t,
                                          Error **errp)
{
    size_t crossbar_count = sgi_machine_topology_crossbar_count(t);
    size_t i;

    for (i = 0; i < crossbar_count; i++) {
        /*
         * Builder invariant: "links-present" and every endpoint the
         * builders above attached both come from the topology's XIO
         * attachments, so nothing a user supplies can make them differ.
         * An endpoint on a port reported empty would carry traffic to a
         * widget firmware is told is not there.
         */
        uint16_t present = object_property_get_uint(
            OBJECT(s->crossbars[i]), "links-present", &error_abort);

        g_assert(!(sgi_xbow_endpoint_widgets(s->crossbars[i]) & ~present));
        if (!sysbus_realize(SYS_BUS_DEVICE(s->crossbars[i]), errp)) {
            return false;
        }
    }
    return true;
}

/* ---- Module identity, shared controllers and routers ---- */

/*
 * A module naming a shared system controller shares one MicroLAN
 * module-identity responder across every node it carries. A module
 * whose nodes each carry their own controller (Origin 200) has none.
 */
static void sgi_machine_attach_module_identities(SGIMachineState *s,
                                                  const SGIModelTopology *t)
{
    g_autofree const SGIModelModule **modules = NULL;
    g_autofree const SGIModelNode **nodes = NULL;
    size_t module_count, node_count, module_index;

    modules = sgi_model_module_array(t->modules, &module_count);
    nodes = sgi_model_node_array(t->nodes, &node_count);
    for (module_index = 0; module_index < module_count; module_index++) {
        const SGIModelModule *module = modules[module_index];
        g_autofree unsigned int *module_nodes = NULL;
        size_t module_node_count, i;
        SGIHubModuleIdentity identity;

        if (module->controller_index < 0) {
            continue;
        }
        if (!sgi_sn0_module_management_identity_pages(module->serial,
                                                      identity.page)) {
            error_report("module '%s' has an invalid identity serial",
                        module->id);
            exit(EXIT_FAILURE);
        }
        module_nodes = sgi_model_array_uint32(module->nodes,
                                              &module_node_count);
        for (i = 0; i < module_node_count; i++) {
            unsigned int node_index = module_nodes[i];

            sgi_sn0_module_management_identity_rom(
                nodes[node_index]->slot, module->initial_module_number,
                identity.rom);
            if (!sgi_hub_attach_module_identity(&s->nodes[node_index]->hub,
                                                &identity, &error_fatal)) {
                exit(EXIT_FAILURE);
            }
        }
    }
}

/*
 * One shared controller per module that names one: an MSC with one
 * PCF8584 adapter reflected into each of the module's nodes. The
 * adapter array is capacity SGI_O2000_ADAPTER_COUNT (every module
 * measured ships four nodes); asserted here rather than truncated.
 */
static bool sgi_machine_build_controllers(SGIMachineState *s,
                                          const SGIModelTopology *t,
                                          Error **errp)
{
    g_autofree const SGIModelController **controllers = NULL;
    g_autofree const SGIModelModule **modules = NULL;
    size_t controller_count, module_count, controller_index;

    controllers = sgi_model_controller_array(t->controllers, &controller_count);
    modules = sgi_model_module_array(t->modules, &module_count);
    for (controller_index = 0; controller_index < controller_count;
         controller_index++) {
        const SGIModelController *controller = controllers[controller_index];
        const SGIModelModule *module = NULL;
        g_autofree unsigned int *module_nodes = NULL;
        size_t module_node_count, i;
        DeviceState *dev;
        Chardev *aux;

        for (i = 0; i < module_count; i++) {
            if (modules[i]->controller_index == (int64_t)controller_index) {
                module = modules[i];
                break;
            }
        }
        if (!module) {
            error_setg(errp, "topology '%s' controller %zu names no module",
                      t->name, controller_index);
            return false;
        }
        if (controller->kind != SGI_MODEL_CONTROLLER_KIND_MSC) {
            error_setg(errp, "topology '%s' controller %zu names an unknown "
                      "controller kind", t->name, controller_index);
            return false;
        }
        module_nodes = sgi_model_array_uint32(module->nodes,
                                              &module_node_count);
        if (module_node_count > SGI_O2000_ADAPTER_COUNT) {
            error_setg(errp, "topology '%s' module '%s' has %zu nodes, "
                      "beyond its controller's %u adapters", t->name,
                      module->id, module_node_count,
                      SGI_O2000_ADAPTER_COUNT);
            return false;
        }

        dev = qdev_new(TYPE_SGI_SN0_MODULE_MANAGEMENT);
        object_property_add_child(OBJECT(s), "module-management",
                                  OBJECT(dev));
        qdev_prop_set_uint8(dev, "module-number",
                            module->initial_module_number);
        qdev_prop_set_string(dev, "model-serial", module->serial);
        qdev_prop_set_uint8(dev, "dip-switches", s->elsc_dip_switches);
        aux = serial_hd(sgi_machine_compute_serial_line(
            t, module_nodes[0], SN0_ELSC_CHARDEV));
        if (aux) {
            qdev_prop_set_chr(dev, "chardev", aux);
        }
        qdev_realize_and_unref(dev, NULL, &error_fatal);

        for (i = 0; i < module_node_count; i++) {
            unsigned int node_index = module_nodes[i];
            SGIHubState *hub = &s->nodes[node_index]->hub;
            SysBusDevice *adapter = SYS_BUS_DEVICE(
                sgi_sn0_module_management_get_adapter(
                    SGI_SN0_MODULE_MANAGEMENT(dev), i));

            memory_region_add_subregion_overlap(
                &hub->regs, SN0_HUB_MODULE_ADAPTER_OFFSET,
                sysbus_mmio_get_region(adapter, 0), 1);
        }
    }
    return true;
}

/*
 * Create every Router in descriptor order, matching the topology's own
 * cable list's router index, and set its own chip properties -- not its
 * port cables, which do not exist until the link-wiring loop below
 * creates them, nor its realize, which waits until every port property
 * is set.
 */
static void sgi_machine_build_routers(SGIMachineState *s,
                                      const SGIModelTopology *t)
{
    g_autofree const SGIModelRouter **routers = NULL;
    size_t router_count, i;

    routers = sgi_model_router_array(t->routers, &router_count);
    s->router_count = router_count;
    s->routers = g_new0(SGIRouterState *, router_count);
    for (i = 0; i < router_count; i++) {
        const SGIModelRouter *router = routers[i];
        g_autofree char *name = g_strdup_printf("router%zu", i);
        DeviceState *dev = qdev_new(TYPE_SGI_ROUTER);

        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        qdev_prop_set_uint8(dev, "chip-inputs", router->chip_inputs);
        qdev_prop_set_uint32(dev, "slot", router->slot);
        qdev_prop_set_uint64(dev, "nic-clock-hz",
                            SN0_HUB_CLOCK_HZ);
        qdev_prop_set_uint64(dev, "nic-serial", router->serial);
        s->routers[i] = SGI_ROUTER(dev);
    }
}

/*
 * Point one endpoint's own cable-reference property at cable: a Hub's
 * one "net-cable", or a Router's numbered "portN-cable".
 */
static void sgi_machine_wire_fabric_vertex(SGIMachineState *s,
                                           const SGIModelLinkEnd *end,
                                           SGINetCableState *cable)
{
    if (end->kind == SGI_MODEL_LINK_END_KIND_ROUTER) {
        g_autofree char *prop =
            g_strdup_printf("port%u-cable", end->port);

        object_property_set_link(OBJECT(s->routers[end->index]), prop,
                                 OBJECT(cable), &error_fatal);
    } else {
        object_property_set_link(OBJECT(&s->nodes[end->index]->hub),
                                 "net-cable", OBJECT(cable), &error_fatal);
    }
}

/*
 * Build one sgi-net-cable per descriptor cable record, named and typed
 * from it, and wire both ends' own cable-reference properties. Routers
 * must already exist (sgi_machine_build_routers()); neither a Router nor
 * a Hub may be realized yet.
 */
static void sgi_machine_build_fabric_links(SGIMachineState *s,
                                           const SGIModelTopology *t)
{
    g_autofree const SGIModelCable **cables = NULL;
    size_t cable_count, i;

    cables = sgi_model_cable_array(t->cables, &cable_count);
    for (i = 0; i < cable_count; i++) {
        const SGIModelCable *cable = cables[i];
        const SGIModelLinkEnd *end0 = cable->ends->value;
        const SGIModelLinkEnd *end1 = cable->ends->next->value;
        DeviceState *dev = qdev_new(TYPE_SGI_NET_CABLE);
        SGINetCableState *net_cable = SGI_NET_CABLE(dev);

        object_property_add_child(OBJECT(s), cable->name, OBJECT(dev));
        qdev_prop_set_uint32(dev, "kind", cable->kind);
        sgi_net_cable_set_end(net_cable, 0,
            end0->kind == SGI_MODEL_LINK_END_KIND_ROUTER ?
                OBJECT(s->routers[end0->index]) :
                OBJECT(&s->nodes[end0->index]->hub),
            end0->port);
        sgi_net_cable_set_end(net_cable, 1,
            end1->kind == SGI_MODEL_LINK_END_KIND_ROUTER ?
                OBJECT(s->routers[end1->index]) :
                OBJECT(&s->nodes[end1->index]->hub),
            end1->port);
        sgi_machine_wire_fabric_vertex(s, end0, net_cable);
        sgi_machine_wire_fabric_vertex(s, end1, net_cable);
        qdev_realize_and_unref(dev, NULL, &error_fatal);
    }
}

/* Resolve the declared endpoint kind independently of module ownership. */
static SN1FabricEndpoint *sgi_machine_bedrock_endpoint(
    SGIMachineState *s, const SGIModelLinkEnd *end, Error **errp)
{
    DeviceState *dev;

    switch (end->kind) {
    case SGI_MODEL_LINK_END_KIND_NODE:
        if (end->index >= s->num_nodes) {
            error_setg(errp, "Bedrock cable names node %u out of range",
                       end->index);
            return NULL;
        }
        dev = s->bedrock_nodes[end->index];
        break;
    case SGI_MODEL_LINK_END_KIND_IO_PEER:
        if (end->index >= s->io_peer_count) {
            error_setg(errp, "Bedrock cable names I/O peer %u without a "
                       "constructed assembly", end->index);
            return NULL;
        }
        dev = s->io_peers[end->index];
        break;
    default:
        error_setg(errp, "Bedrock cable endpoint kind is unsupported");
        return NULL;
    }
    if (!dev || !object_dynamic_cast(OBJECT(dev), TYPE_SGI_SN1_FABRIC)) {
        error_setg(errp, "Bedrock cable endpoint has no fabric interface");
        return NULL;
    }
    return SGI_SN1_FABRIC_GET_CLASS(dev)->endpoint(OBJECT(dev));
}

/* Existing compute pairs retain their separately modeled L1 peer relation. */
static bool sgi_machine_build_bedrock_links(SGIMachineState *s,
                                           const SGIModelTopology *t,
                                           Error **errp)
{
    g_autofree const SGIModelCable **cables = NULL;
    size_t cable_count, i;

    cables = sgi_model_cable_array(t->cables, &cable_count);
    for (i = 0; i < cable_count; i++) {
        const SGIModelCable *cable = cables[i];
        const SGIModelLinkEnd *ends[2];
        SN1FabricEndpoint *endpoints[2];
        DeviceState *dev;
        unsigned int side;

        if (QAPI_LIST_LENGTH(cable->ends) != 2) {
            error_setg(errp, "Bedrock cable '%s' needs exactly two endpoints",
                       cable->name);
            return false;
        }
        ends[0] = cable->ends->value;
        ends[1] = cable->ends->next->value;
        for (side = 0; side < 2; side++) {
            endpoints[side] = sgi_machine_bedrock_endpoint(s, ends[side], errp);
            if (!endpoints[side]) {
                return false;
            }
        }
        dev = qdev_new(TYPE_SGI_NET_CABLE);
        object_property_add_child(OBJECT(s), cable->name, OBJECT(dev));
        qdev_prop_set_uint32(dev, "kind", cable->kind);
        for (side = 0; side < 2; side++) {
            sgi_net_cable_set_end(SGI_NET_CABLE(dev), side,
                                 OBJECT(endpoints[side]->bedrock),
                                 ends[side]->port);
        }
        if (!qdev_realize(dev, NULL, errp)) {
            object_unref(OBJECT(dev));
            return false;
        }
        for (side = 0; side < 2; side++) {
            if (!object_property_set_link(OBJECT(endpoints[side]->bedrock),
                                           "net-cable", OBJECT(dev), errp)) {
                object_unref(OBJECT(dev));
                return false;
            }
        }
        object_unref(OBJECT(dev));
        if (ends[0]->kind == SGI_MODEL_LINK_END_KIND_NODE &&
            ends[1]->kind == SGI_MODEL_LINK_END_KIND_NODE) {
            for (side = 0; side < 2; side++) {
                SN1NodeState *node =
                    SN1_NODE(s->bedrock_nodes[ends[side]->index]);
                SN1NodeState *other =
                    SN1_NODE(s->bedrock_nodes[ends[side ^ 1]->index]);

                /* The reviewed compute pair carries its L1 management path. */
                object_property_set_link(OBJECT(node->l1), "peer",
                                         OBJECT(other->l1), &error_fatal);
            }
        }
    }
    return true;
}

static void sgi_machine_io_peer_number_changed(Notifier *notifier, void *data)
{
    SGIMachinePeerNotifier *peer =
        container_of(notifier, SGIMachinePeerNotifier, notifier);

    sn1_fabric_update_claims(peer->endpoint);
}

static const VMStateDescription vmstate_sgi_machine_owner = {
    .name = "sgi-machine/l1-owner",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(identity, SGIL1QueryOwner),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_machine_association = {
    .name = "sgi-machine/l1-association",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(from, SGIL1QueryAssociation),
        VMSTATE_UINT32_EQUAL(to, SGIL1QueryAssociation),
        VMSTATE_BOOL(reachable, SGIL1QueryAssociation),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_machine_attachment = {
    .name = "sgi-machine/l1-attachment",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(issuer, SGIL1QueryAttachment),
        VMSTATE_UINT32_EQUAL(owner, SGIL1QueryAttachment),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_machine_assembly_owner = {
    .name = "sgi-machine/assembly-owner",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(peer, SGIMachineAssemblyOwnership),
        VMSTATE_UINT32_EQUAL(module, SGIMachineAssemblyOwnership),
        VMSTATE_UINT32_EQUAL(controller, SGIMachineAssemblyOwnership),
        VMSTATE_END_OF_LIST()
    },
};

static int sgi_machine_management_pre_save(void *opaque)
{
    SGIMachineState *s = opaque;

    return s->query_router.active ? -EBUSY : 0;
}

static const VMStateDescription vmstate_sgi_machine_management = {
    .name = "sgi-machine/l1-management",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = sgi_machine_management_pre_save,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(controller_count, SGIMachineState),
        VMSTATE_UINT32_EQUAL(io_peer_count, SGIMachineState),
        VMSTATE_UINT32_EQUAL(association_count, SGIMachineState),
        VMSTATE_UINT32_EQUAL(attachment_count, SGIMachineState),
        VMSTATE_VARRAY_UINT32(owner_controllers, SGIMachineState,
                             controller_count, 0, vmstate_info_uint32_equal,
                             uint32_t),
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(query_owners, SGIMachineState,
            controller_count, vmstate_sgi_machine_owner, SGIL1QueryOwner),
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(assembly_ownership,
            SGIMachineState, io_peer_count, vmstate_sgi_machine_assembly_owner,
            SGIMachineAssemblyOwnership),
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(query_associations,
            SGIMachineState, association_count, vmstate_sgi_machine_association,
            SGIL1QueryAssociation),
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(query_attachments, SGIMachineState,
            attachment_count, vmstate_sgi_machine_attachment,
            SGIL1QueryAttachment),
        VMSTATE_VARRAY_UINT32(initial_reachability, SGIMachineState,
                             association_count, 0, vmstate_info_uint8_equal,
                             uint8_t),
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_machine_get_association(Object *obj, Visitor *v,
    const char *name, void *opaque, Error **errp)
{
    SGIL1QueryAssociation *edge = opaque;
    bool reachable = edge->reachable;

    visit_type_bool(v, name, &reachable, errp);
}

static void sgi_machine_set_association(Object *obj, Visitor *v,
    const char *name, void *opaque, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);
    SGIL1QueryAssociation *edge = opaque;
    bool reachable;

    if (s->query_router.active) {
        error_setg(errp, "cannot change L1 association during a query");
        return;
    }
    if (visit_type_bool(v, name, &reachable, errp)) {
        edge->reachable = reachable;
    }
}

static bool sgi_machine_build_io_assemblies(SGIMachineState *s,
    const SGIModelTopology *t, Error **errp)
{
    g_autofree const SGIModelModule **modules = NULL;
    const SGIModelIoAssemblyList *entry;
    const SGIModelNodeList *node_entry;
    const SGIModelL1OwnerList *owner_entry;
    const SGIModelL1AssociationList *association;
    const SGIModelL1AttachmentList *attachment;
    size_t module_count;
    unsigned index;
    Error *local_error = NULL;
    static const char *const port_names[] = {
        "chip1-e-port", "chip1-f-port", "chip2-e-port", "chip2-f-port",
    };

    if (!t->io_assemblies) {
        return true;
    }
    modules = sgi_model_module_array(t->modules, &module_count);
    s->controller_count = QAPI_LIST_LENGTH(t->controllers);
    s->controllers = g_new0(DeviceState *, s->controller_count);
    s->controller_reset_registered = g_new0(bool, s->controller_count);
    s->controller_bics = g_new0(DeviceState *, s->controller_count);
    s->io_peer_count = QAPI_LIST_LENGTH(t->io_peers);
    s->io_peers = g_new0(DeviceState *, s->io_peer_count);
    s->io_peer_notifiers = g_new0(SGIMachinePeerNotifier, s->io_peer_count);
    s->assembly_ownership =
        g_new0(SGIMachineAssemblyOwnership, s->io_peer_count);
    for (node_entry = t->nodes, index = 0; node_entry;
         node_entry = node_entry->next, index++) {
        const SGIModelNode *node = node_entry->value;
        int64_t owner;

        if (node->module_index >= module_count) {
            error_setg(errp, "compute L1 names an absent module");
            return false;
        }
        owner = modules[node->module_index]->controller_index;
        if (owner < 0 || owner >= s->controller_count ||
            s->controllers[owner]) {
            error_setg(errp, "compute L1 needs unique module-owned controller");
            return false;
        }
        s->controllers[owner] = SN1_NODE(s->bedrock_nodes[index])->l1;
        object_ref(OBJECT(s->controllers[owner]));
    }
    for (entry = t->io_assemblies; entry; entry = entry->next) {
        const SGIModelIoAssembly *a = entry->value;
        g_autofree char *module_name =
            g_strdup_printf("module%u", a->module_index);
        g_autofree char *prefix =
            g_strdup_printf("expansion%u-", a->peer_index);
        Object *module = object_resolve_path_component(OBJECT(s), module_name);
        DeviceState *l1 = qdev_new(TYPE_SGI_L1);
        DeviceState *interface = qdev_new(TYPE_SGI_BIC_BOARD_INTERFACE);
        DeviceState *dev = qdev_new(TYPE_SGI_ORIGIN300_PCI_EXPANSION);
        SGIOrigin300PciExpansionState *assembly =
            SGI_ORIGIN300_PCI_EXPANSION(dev);
        const uint8List *port;
        SGIMachinePeerNotifier *notifier = &s->io_peer_notifiers[a->peer_index];

        object_property_add_child(module, "l1", OBJECT(l1));
        s->controllers[a->controller_index] = l1;
        qdev_prop_set_string(l1, "brick-type", a->controller_profile);
        object_property_add_child(module, "board-interface", OBJECT(interface));
        qdev_prop_set_uint64(interface, "identity", a->bic_identity);
        qdev_prop_set_bit(interface, "initial-latch-set", a->has_initial_latch);
        qdev_prop_set_uint16(interface, "initial-latch", a->initial_latch);
        if (!qdev_realize(interface, NULL, errp)) {
            object_unref(OBJECT(interface));
            object_unref(OBJECT(dev));
            return false;
        }
        device_cold_reset(interface);
        if (a->has_controller_startup) {
            SGIL1BICEndpoint endpoints[2] = {
                { .receiver = OBJECT(interface),
                  .identity = (uint64_t)a->peer_index + 1 },
            };

            if (a->controller_startup !=
                SGI_MODEL_CONTROLLER_STARTUP_BIC_FUNCTIONAL_EXPERIMENT ||
                !sgi_l1_bind_bic_startup(SGI_L1(l1),
                    SGI_L1_BIC_STARTUP_EXPERIMENT, endpoints,
                    sgi_bic_board_controller_operation, errp)) {
                object_unref(OBJECT(interface));
                object_unref(OBJECT(dev));
                return false;
            }
            s->controller_bics[a->controller_index] = interface;
            object_ref(OBJECT(interface));
            qemu_register_resettable(OBJECT(interface));
            qemu_register_resettable(OBJECT(l1));
            s->controller_reset_registered[a->controller_index] = true;
        }
        if (!sgi_machine_set_expansion_inputs(s, a->controller_index,
                                              l1, errp) ||
            !qdev_realize(l1, NULL, errp)) {
            if (s->controller_reset_registered[a->controller_index]) {
                qemu_unregister_resettable(OBJECT(l1));
                qemu_unregister_resettable(OBJECT(interface));
                s->controller_reset_registered[a->controller_index] = false;
            }
            object_unref(OBJECT(interface));
            object_unref(OBJECT(dev));
            return false;
        }
        object_property_add_child(module, "assembly", OBJECT(dev));
        s->io_peers[a->peer_index] = dev;
        qdev_prop_set_uint32(dev, "placement", a->placement);
        qdev_prop_set_uint8(dev, "node-number", a->initial_node_number);
        qdev_prop_set_uint8(dev, "bedrock-revision", a->bedrock_revision);
        qdev_prop_set_uint8(dev, "xbridge-revision", a->xbridge_revision);
        qdev_prop_set_string(dev, "bus-prefix", prefix);
        for (port = a->head_ports, index = 0; port;
             port = port->next, index++) {
            qdev_prop_set_uint8(dev, port_names[index], port->value);
        }
        if (a->has_intx_map) {
            SGIOrigin300PciIntxMap map;
            uint8_t *pins = &map.pin[0][0][0];
            const uint8List *pin;

            for (pin = a->intx_map, index = 0; pin; pin = pin->next, index++) {
                pins[index] = pin->value;
            }
            if (!sgi_origin300_pci_expansion_set_intx_map(assembly, &map,
                                                         errp)) {
                object_unref(OBJECT(interface));
                return false;
            }
        }
        if (!sgi_origin300_pci_expansion_enable_ni_route(assembly, errp) ||
            !sgi_origin300_pci_expansion_attach_board_interface(assembly,
                SGI_BIC_BOARD_INTERFACE(interface), errp)) {
            object_unref(OBJECT(interface));
            return false;
        }
        object_unref(OBJECT(interface));
        if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
            return false;
        }
        notifier->endpoint =
            SGI_SN1_FABRIC_GET_CLASS(dev)->endpoint(OBJECT(dev));
        notifier->notifier.notify = sgi_machine_io_peer_number_changed;
        sgi_bedrock_lb_add_node_notifier(&notifier->endpoint->bedrock->lb,
                                         &notifier->notifier);
        s->assembly_ownership[a->peer_index] = (SGIMachineAssemblyOwnership) {
            .peer = a->peer_index, .module = a->module_index,
            .controller = a->controller_index,
        };
    }
    for (index = 0; index < s->controller_count; index++) {
        if (!s->controllers[index]) {
            error_setg(errp, "controller %u has no constructed L1 owner",
                       index);
            return false;
        }
    }
    s->query_owners = g_new0(SGIL1QueryOwner, s->controller_count);
    s->owner_controllers = g_new0(uint32_t, s->controller_count);
    for (owner_entry = t->l1_management->owners, index = 0; owner_entry;
         owner_entry = owner_entry->next, index++) {
        const SGIModelL1Owner *owner = owner_entry->value;

        if (owner->controller_index >= s->controller_count) {
            error_setg(errp, "L1 management names controller out of range");
            return false;
        }
        s->owner_controllers[index] = owner->controller_index;
        s->query_owners[index] = (SGIL1QueryOwner) {
            .controller = SGI_L1(s->controllers[owner->controller_index]),
            .identity = owner->identity,
        };
    }
    s->association_count = QAPI_LIST_LENGTH(t->l1_management->associations);
    s->attachment_count = QAPI_LIST_LENGTH(t->l1_management->attachments);
    s->query_associations = g_new0(SGIL1QueryAssociation, s->association_count);
    s->initial_reachability = g_new0(uint8_t, s->association_count);
    s->query_attachments = g_new0(SGIL1QueryAttachment, s->attachment_count);
    for (association = t->l1_management->associations, index = 0; association;
         association = association->next, index++) {
        const SGIModelL1Association *edge = association->value;

        s->query_associations[index] = (SGIL1QueryAssociation) {
            .from = edge->from, .to = edge->to, .reachable = edge->reachable,
        };
        s->initial_reachability[index] = edge->reachable;
    }
    for (attachment = t->l1_management->attachments, index = 0; attachment;
         attachment = attachment->next, index++) {
        s->query_attachments[index] = (SGIL1QueryAttachment) {
            .issuer = attachment->value->issuer,
            .owner = attachment->value->owner,
        };
    }
    sgi_l1_query_router_init(&s->query_router, s->query_owners,
                             s->controller_count, s->query_associations,
                             s->association_count, &local_error);
    if (local_error) {
        error_propagate(errp, local_error);
        return false;
    }
    if (!sgi_l1_query_router_set_attached(&s->query_router,
            s->query_attachments, s->attachment_count, errp)) {
        return false;
    }
    for (index = 0; index < s->association_count; index++) {
        g_autofree char *name =
            g_strdup_printf("l1-association%u-reachable", index);

        object_property_add(OBJECT(s), name, "bool",
                            sgi_machine_get_association,
                            sgi_machine_set_association, NULL,
                            &s->query_associations[index]);
    }
    vmstate_register_any(NULL, &vmstate_sgi_machine_management, s);
    s->management_registered = true;
    return true;
}

/* ---- The builder ---- */

/*
 * The IP27 PROM jump table at flash offset zero: ten entries, real
 * hardware's own layout.
 */
#define SGI_MACHINE_FLASH_ENTRIES       10
#define SGI_MACHINE_FLASH_ENTRY_STRIDE  8

/*
 * The firmware configuration block, at flash offset 0x60 -- real
 * hardware's own layout.
 */
#define SGI_MACHINE_CONFIG_OFFSET              0x60
/* "ip27conf" */
#define SGI_MACHINE_CONFIG_MAGIC               0x69703237636f6e66ULL
#define SGI_MACHINE_CONFIG_SIZE                0x50
#define SGI_MACHINE_CONFIG_FPROM_CYC_OFFSET    0x2c
#define SGI_MACHINE_CONFIG_MACHINE_TYPE_OFFSET 0x30
#define SGI_MACHINE_CONFIG_CHECKSUM_OFFSET     0x34
#define SGI_MACHINE_CONFIG_FPROM_WR_OFFSET     0x3c

/* Where the factory-fresh environment log sits in the flash image. */
#define SGI_MACHINE_PROMLOG_SECTOR    14
#define SGI_MACHINE_PROMLOG_MAGIC     0x504c4f47
#define SGI_MACHINE_PROMLOG_VERSION   1

/*
 * The default flash template: entry zero spins (an unprogrammed reset
 * vector), the rest return, plus a configuration block and factory-
 * fresh environment log in the real IP27 firmware's own layout. A
 * -bios image shorter than the flash overlays only its own bytes, so a
 * short image still finds these pre-formatted underneath.
 */
static void sgi_machine_build_flash(const SGIModelTopology *t,
                                    const SGIMachineClass *smc,
                                    uint8_t *flash, uint64_t flash_size)
{
    static const uint32_t jr_ra = 0x03e00008;      /* jr $31 */
    static const uint32_t branch_to_self = 0x1000ffff; /* b . */
    const SGIModelProcessor *cpu =
        sgi_machine_topology_node0(t)->cpu_slots->value->assembly;
    uint8_t *config = flash + SGI_MACHINE_CONFIG_OFFSET;
    int i;

    g_assert(flash_size >=
             (SGI_MACHINE_PROMLOG_SECTOR + 1) * sn0_flash.sector_size);
    memset(flash, 0xff, flash_size);

    for (i = 0; i < SGI_MACHINE_FLASH_ENTRIES; i++) {
        uint8_t *entry = flash + i * SGI_MACHINE_FLASH_ENTRY_STRIDE;

        stl_be_p(entry, i == 0 ? branch_to_self : jr_ra);
        stl_be_p(entry + 4, 0);                    /* the delay slot */
    }

    /* The field order and widths are the IP27 firmware's. */
    stl_be_p(config + 0x00, 0xb);                  /* timing constant */
    stl_be_p(config + 0x04, cpu->mode_bits);
    stq_be_p(config + 0x08, SGI_MACHINE_CONFIG_MAGIC);
    stq_be_p(config + 0x10, cpu->clock_hz);
    stq_be_p(config + 0x18, SN0_HUB_CLOCK_HZ);
    stq_be_p(config + 0x20, SN0_HUB_RTC_CLOCK_HZ);
    stl_be_p(config + 0x28, 1);                    /* error correction on */
    stl_be_p(config + SGI_MACHINE_CONFIG_FPROM_CYC_OFFSET,
             smc->flash_read_cycles);
    stl_be_p(config + SGI_MACHINE_CONFIG_MACHINE_TYPE_OFFSET,
             smc->firmware_machine_type);
    /* checksum adjustment */
    stl_be_p(config + SGI_MACHINE_CONFIG_CHECKSUM_OFFSET, 0);
    stl_be_p(config + 0x38, 0);                    /* flash program count */
    stl_be_p(config + SGI_MACHINE_CONFIG_FPROM_WR_OFFSET,
             smc->flash_write_cycles);
    stl_be_p(config + 0x40, 6);                    /* firmware version */
    stl_be_p(config + 0x44, 11);                   /* firmware revision */
    stl_be_p(config + 0x48, 0);                    /* configuration flags */

    /* The factory-fresh environment log; an erased first entry ends it. */
    config = flash + SGI_MACHINE_PROMLOG_SECTOR * sn0_flash.sector_size;
    stl_be_p(config + 0x10, SGI_MACHINE_PROMLOG_MAGIC);
    stl_be_p(config + 0x14, SGI_MACHINE_PROMLOG_VERSION);
    stl_be_p(config + 0x18, 1);                    /* initial sequence */
}

static uint8_t sgi_machine_config_byte_sum(const uint8_t *config)
{
    uint8_t sum = 0;
    unsigned int i;

    for (i = 0; i < SGI_MACHINE_CONFIG_SIZE; i++) {
        sum += config[i];
    }
    return sum;
}

/*
 * The flasher's contract: the three product values in, every other field
 * and the old block sum preserved; no IP27 signature, no touch. Runs
 * whether the configuration block present is this file's own default
 * template or a real PROM image's -- every stock PROM image carries one
 * at this same offset with this same magic.
 */
static void sgi_machine_configure_flash(const SGIMachineClass *smc,
                                        uint8_t *flash)
{
    uint8_t *config = flash + SGI_MACHINE_CONFIG_OFFSET;
    uint8_t old_sum;
    uint8_t new_sum;

    if (ldq_be_p(config + 0x08) != SGI_MACHINE_CONFIG_MAGIC) {
        return;
    }

    old_sum = sgi_machine_config_byte_sum(config);
    stl_be_p(config + SGI_MACHINE_CONFIG_CHECKSUM_OFFSET, 0);
    stl_be_p(config + SGI_MACHINE_CONFIG_FPROM_CYC_OFFSET,
             smc->flash_read_cycles);
    stl_be_p(config + SGI_MACHINE_CONFIG_MACHINE_TYPE_OFFSET,
             smc->firmware_machine_type);
    stl_be_p(config + SGI_MACHINE_CONFIG_FPROM_WR_OFFSET,
             smc->flash_write_cycles);
    new_sum = sgi_machine_config_byte_sum(config);
    stl_be_p(config + SGI_MACHINE_CONFIG_CHECKSUM_OFFSET,
             (uint8_t)(old_sum - new_sum));
}

/*
 * Bedrock's own PROM policy: -bios names the required stock image and a
 * per-node pflash drive (unit node_index) overrides it, the same two
 * inputs the Hub path below resolves, but with no synthesized default --
 * every Bedrock node this family has built still requires a real image,
 * the contract sn1_node_attach_prom's own removed prefix used to enforce
 * on its own. One image, read once, attaches to every node this
 * topology names (today, at most one: a topology with more is refused
 * before sgi_machine_realize ever reaches this function).
 */
static bool sgi_machine_load_bedrock_flash(MachineState *machine,
                                           SGIMachineState *s, Error **errp)
{
    g_autoptr(GError) local_error = NULL;
    g_autofree char *contents = NULL;
    g_autofree char *path = NULL;
    gsize length;
    unsigned int node_index;

    if (!machine->firmware) {
        error_setg(errp,
                   "%s requires -bios with a raw stock PROM payload",
                   MACHINE_GET_CLASS(machine)->name);
        return false;
    }
    path = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    if (!path) {
        error_setg(errp, "%s: cannot find PROM image '%s'",
                  MACHINE_GET_CLASS(machine)->name, machine->firmware);
        return false;
    }
    if (!g_file_get_contents(path, &contents, &length, &local_error)) {
        error_setg(errp, "%s: cannot read PROM image '%s': %s",
                  MACHINE_GET_CLASS(machine)->name, machine->firmware,
                  local_error->message);
        return false;
    }

    for (node_index = 0; node_index < s->num_nodes; node_index++) {
        SN1NodeState *node = SN1_NODE(s->bedrock_nodes[node_index]);

        if (!sn1_node_attach_prom(node, node_index, (const uint8_t *)contents,
                                  length, errp)) {
            return false;
        }
    }
    return true;
}

/*
 * -bios fills every node's boot flash unless a pflash drive already backs
 * it (set on the node before it realized); a short -bios image leaves the
 * default template's own bytes behind it, matching a factory-formatted
 * chip rather than a merely erased one. Bedrock's own policy has no
 * synthesized default and lives in sgi_machine_load_bedrock_flash above;
 * this function's own body stays the Hub's.
 */
static bool sgi_machine_load_flash(MachineState *machine,
                                   const SGIModelTopology *t,
                                   SGIMachineState *s, Error **errp)
{
    const SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(machine);
    uint64_t flash_size;

    if (sgi_machine_topology_node0(t)->asic_kind != SGI_MODEL_ASIC_KIND_HUB) {
        return sgi_machine_load_bedrock_flash(machine, s, errp);
    }

    flash_size = sn0_hub_address_map.flash_reset.size;
    g_autofree uint8_t *flash = NULL;
    unsigned int node_index;

    flash = g_malloc(flash_size);
    sgi_machine_build_flash(t, smc, flash, flash_size);
    if (machine->firmware) {
        g_autofree char *path =
            qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        int64_t image_size = path ? get_image_size(path, NULL) : -1;
        ssize_t size;

        if (image_size < 0) {
            error_setg(errp, "%s: cannot read the firmware image '%s'",
                      MACHINE_GET_CLASS(machine)->name, machine->firmware);
            return false;
        }
        if ((uint64_t)image_size > flash_size) {
            error_setg(errp, "%s: firmware image '%s' exceeds flash "
                      "capacity", MACHINE_GET_CLASS(machine)->name,
                      machine->firmware);
            return false;
        }
        size = load_image_size(path, flash, flash_size);
        if (size != image_size) {
            error_setg(errp, "%s: cannot load firmware image '%s'",
                      MACHINE_GET_CLASS(machine)->name, machine->firmware);
            return false;
        }
    }
    sgi_machine_configure_flash(smc, flash);
    for (node_index = 0; node_index < s->num_nodes; node_index++) {
        SN0NodeState *node = s->nodes[node_index];
        MemoryRegion *chip;

        if (node->flash_drive) {
            continue;
        }
        chip = sysbus_mmio_get_region(SYS_BUS_DEVICE(node->flash), 0);
        memcpy(memory_region_get_ram_ptr(chip), flash, flash_size);
        memory_region_set_dirty(chip, 0, flash_size);
    }
    return true;
}

static void sgi_machine_build_module_containers(MachineState *machine,
                                                const SGIModelTopology *t)
{
    const SGIModelModuleList *entry;
    unsigned int index = 0;

    for (entry = t->modules; entry; entry = entry->next, index++) {
        g_autofree char *name = g_strdup_printf("module%u", index);
        Object *container = object_new(TYPE_CONTAINER);

        object_property_add_child(OBJECT(machine), name, container);
        object_unref(container);
    }
}

static bool sgi_machine_input_present(SGIMachineState *s,
                                      SGIMachineInput input)
{
    switch (input) {
    case SGI_INPUT_BOARD_ID:
        return s->fuel_board_id_word_set;
    case SGI_INPUT_IOC3_SUBSYSTEM:
        return s->fuel_ioc3_subsystem_id_set;
    case SGI_INPUT_L1_TYPE:
        return s->fuel_l1_type_code_set;
    case SGI_INPUT_L1_REVISION:
        return s->fuel_l1_revision_set;
    case SGI_INPUT_BEDROCK_REVISION:
        return s->fuel_bedrock_revision_set;
    case SGI_INPUT_MAC_EEPROM:
        return s->fuel_mac_eeprom != NULL;
    default:
        g_assert_not_reached();
    }
}

static bool sgi_machine_bind_input(SGIMachineState *s,
                                  const SGIModelTopology *t,
                                  const SGIModelInputBinding *binding,
                                  SGIMachineInput input, unsigned int *used,
                                  bool *present, Error **errp)
{
    *present = false;
    if (!binding) {
        return true;
    }
    if (strcmp(binding->option, sgi_machine_input_names[input])) {
        error_setg(errp, "topology '%s' binds unsupported input '%s' to '%s'",
                   t->name, binding->option, sgi_machine_input_names[input]);
        return false;
    }
    if (*used & (1U << input)) {
        error_setg(errp, "topology '%s' binds input '%s' more than once",
                   t->name, binding->option);
        return false;
    }
    *used |= 1U << input;
    *present = sgi_machine_input_present(s, input);
    if (binding->required && !*present) {
        error_setg(errp, "topology '%s' requires an explicit %s launch input",
                   t->name, binding->option);
        return false;
    }
    return true;
}

static bool sgi_machine_resolve_uint(SGIMachineState *s,
                                    const SGIModelTopology *t,
                                    const SGIModelInputBinding *binding,
                                    SGIMachineInput input, unsigned int *used,
                                    bool has_default, uint64_t default_value,
                                    uint64_t *value, Error **errp)
{
    bool present;

    if (!sgi_machine_bind_input(s, t, binding, input, used, &present, errp)) {
        return false;
    }
    if (!present) {
        if (!has_default) {
            error_setg(errp, "topology '%s' has no resolved value for '%s'",
                       t->name, sgi_machine_input_names[input]);
            return false;
        }
        *value = default_value;
        return true;
    }
    switch (input) {
    case SGI_INPUT_BOARD_ID:
        *value = s->fuel_board_id_word;
        break;
    case SGI_INPUT_IOC3_SUBSYSTEM:
        *value = s->fuel_ioc3_subsystem_id;
        break;
    case SGI_INPUT_L1_TYPE:
        *value = s->fuel_l1_type_code;
        break;
    case SGI_INPUT_BEDROCK_REVISION:
        *value = s->fuel_bedrock_revision;
        break;
    default:
        g_assert_not_reached();
    }
    return true;
}

/* Resolve once, before assembly, without changing the shared catalogue. */
static bool sgi_machine_resolve_config(SGIMachineState *s,
                                     const SGIModelTopology *t, Error **errp)
{
    g_autofree const SGIModelNode **nodes = NULL;
    g_autofree const SGIModelController **controllers = NULL;
    g_autofree const SGIModelIoBoard **boards = NULL;
    size_t node_count, controller_count, board_count, i;
    unsigned int used = 0;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    controllers = sgi_model_controller_array(t->controllers, &controller_count);
    boards = sgi_model_io_board_array(t->io_boards, &board_count);
    s->node_config = g_new0(SGIMachineNodeConfig, node_count);
    s->l1_config = g_new0(SGIMachineL1Config, controller_count);
    s->io_config = g_new0(SGIMachineIoConfig, board_count);

    for (i = 0; i < node_count; i++) {
        const SGIModelNode *node = nodes[i];
        const SGIModelBedrockConfig *config = node->bedrock;
        SGIMachineNodeConfig *resolved = &s->node_config[i];
        const SGIModelModule *module;
        uint64_t value;

        if (node->asic_kind != SGI_MODEL_ASIC_KIND_BEDROCK) {
            continue;
        }
        module = sgi_machine_node_module(t, i);
        if (!config || !module || module->controller_index < 0 ||
            (uint64_t)module->controller_index >= controller_count ||
            !controllers[module->controller_index]->l1) {
            error_setg(errp, "topology '%s' node '%s' has incomplete "
                       "Bedrock or L1 configuration", t->name, node->id);
            return false;
        }
        resolved->controller_index = module->controller_index;
        if (!sgi_machine_resolve_uint(s, t, config->board_id_input,
                SGI_INPUT_BOARD_ID, &used, config->has_board_id_word,
                config->board_id_word, &resolved->board_id_word, errp)) {
            return false;
        }
        if (config->has_board_id_mask != config->has_board_id_required_bits ||
            (config->has_board_id_mask &&
             ((config->board_id_required_bits & ~config->board_id_mask) ||
              (resolved->board_id_word & config->board_id_mask) !=
              config->board_id_required_bits))) {
            error_setg(errp, "topology '%s' node '%s' board ID fails its "
                       "configured mask/required-bits constraint",
                       t->name, node->id);
            return false;
        }
        if (!sgi_machine_resolve_uint(s, t, config->revision_input,
                SGI_INPUT_BEDROCK_REVISION, &used,
                node->identity->has_widget_revision,
                node->identity->widget_revision, &value, errp)) {
            return false;
        }
        if (value > 15) {
            error_setg(errp, "%s must fit four bits",
                       config->revision_input ?
                       config->revision_input->option : "Bedrock revision");
            return false;
        }
        if (config->local_region >= 64 || config->initiator_region >= 64) {
            error_setg(errp, "topology '%s' node '%s' Bedrock regions must "
                       "be below 64", t->name, node->id);
            return false;
        }
        resolved->revision = value;
        resolved->local_region = config->local_region;
        resolved->initiator_region = config->initiator_region;
        resolved->installed_bank_mask = config->installed_bank_mask;
    }
    for (i = 0; i < controller_count; i++) {
        const SGIModelL1Config *config = controllers[i]->l1;
        SGIMachineL1Config *resolved = &s->l1_config[i];
        uint64_t value;
        bool present;

        bool owns_node = false;

        if (!config) {
            continue;
        }
        for (size_t n = 0; n < node_count; n++) {
            if (nodes[n]->asic_kind == SGI_MODEL_ASIC_KIND_BEDROCK &&
                s->node_config[n].controller_index == i) {
                owns_node = true;
                break;
            }
        }
        if (!owns_node) {
            error_setg(errp, "topology '%s' controller '%s' has L1 "
                       "configuration without a modeled Bedrock node",
                       t->name, controllers[i]->id);
            return false;
        }
        if (!sgi_machine_resolve_uint(s, t, config->reported_type_input,
                SGI_INPUT_L1_TYPE, &used, config->has_reported_type,
                config->reported_type, &value, errp)) {
            return false;
        }
        if (!value || value > UINT8_MAX) {
            error_setg(errp, "%s must be nonzero and fit eight bits",
                       config->reported_type_input ?
                       config->reported_type_input->option :
                       "L1 reported type");
            return false;
        }
        resolved->reported_type = value;
        if (!sgi_machine_bind_input(s, t, config->revision_input,
                SGI_INPUT_L1_REVISION, &used, &present, errp)) {
            return false;
        }
        if (present) {
            memcpy(resolved->revision, s->fuel_l1_revision,
                   sizeof(resolved->revision));
        } else if (config->revision) {
            if (config->revision->major > INT32_MAX ||
                config->revision->minor > INT32_MAX ||
                config->revision->patch > INT32_MAX) {
                error_setg(errp, "topology '%s' controller '%s' L1 revision "
                           "components must fit signed 32-bit values",
                           t->name, controllers[i]->id);
                return false;
            }
            resolved->revision[0] = config->revision->major;
            resolved->revision[1] = config->revision->minor;
            resolved->revision[2] = config->revision->patch;
        } else {
            for (unsigned int j = 0; j < ARRAY_SIZE(resolved->revision); j++) {
                resolved->revision[j] = -1;
            }
        }
        if (!sgi_machine_bind_input(s, t, config->mac_eeprom_input,
                SGI_INPUT_MAC_EEPROM, &used, &present, errp)) {
            return false;
        }
        if (present) {
            resolved->mac_eeprom = s->fuel_mac_eeprom;
        }
    }
    for (i = 0; i < board_count; i++) {
        const SGIModelIoBoard *board = boards[i];
        SGIMachineIoConfig *resolved = &s->io_config[i];
        const SGIModelPciFunctionList *entry;
        unsigned int found = 0;
        uint64_t value;

        if (!board->populated ||
            (board->preset != SGI_MODEL_IO_BOARD_PRESET_IO8 &&
             board->preset != SGI_MODEL_IO_BOARD_PRESET_FUEL)) {
            continue;
        }
        if (!sgi_machine_resolve_uint(s, t, board->ioc3_subsystem_input,
                SGI_INPUT_IOC3_SUBSYSTEM, &used, board->has_ioc3_subsystem_id,
                board->ioc3_subsystem_id, &value, errp)) {
            return false;
        }
        if (value > UINT16_MAX || !board->has_mac_source) {
            error_setg(errp, "topology '%s' board '%s' has invalid IOC3 "
                       "identity configuration", t->name, board->id);
            return false;
        }
        resolved->ioc3_subsystem_id = value;
        resolved->mac_source = board->mac_source;
        if (resolved->mac_source != SGI_MODEL_MAC_SOURCE_NONE &&
            resolved->mac_source != SGI_MODEL_MAC_SOURCE_MODULE_L1) {
            error_setg(errp, "topology '%s' board '%s' has unsupported "
                       "MAC source", t->name, board->id);
            return false;
        }
        for (entry = board->pci_functions; entry; entry = entry->next) {
            const SGIModelPciFunction *function = entry->value;
            unsigned int revision;

            switch (function->kind) {
            case SGI_MODEL_PCI_FUNCTION_KIND_ISP12160:
                revision = 6;
                break;
            case SGI_MODEL_PCI_FUNCTION_KIND_IOC3:
                revision = SGI_IOC3_REVISION;
                break;
            case SGI_MODEL_PCI_FUNCTION_KIND_LUCENT_OHCI:
                revision = 16;
                break;
            default:
                error_setg(errp, "topology '%s' board '%s' has unsupported "
                           "PCI function", t->name, board->id);
                return false;
            }
            if ((found & (1U << function->kind)) ||
                function->revision != revision) {
                error_setg(errp, "topology '%s' board '%s' has duplicate "
                           "or unsupported PCI function revision",
                           t->name, board->id);
                return false;
            }
            found |= 1U << function->kind;
            resolved->pci_revision[function->kind] = function->revision;
        }
        if (found != (1U << SGI_MODEL_PCI_FUNCTION_KIND__MAX) - 1) {
            error_setg(errp, "topology '%s' board '%s' has incomplete PCI "
                       "function configuration", t->name, board->id);
            return false;
        }
    }
    for (i = 0; i < SGI_INPUT_COUNT; i++) {
        if (sgi_machine_input_present(s, i) && !(used & (1U << i))) {
            error_setg(errp, "input '%s' is not bound by topology '%s'",
                       sgi_machine_input_names[i], t->name);
            return false;
        }
    }
    return true;
}

static bool sgi_machine_realize(MachineState *machine, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    Error *local_err = NULL;
    const SGIModelTopology *t = sgi_machine_resolve(machine, &local_err);
    const SN0ModelMemory *node_memory =
        smc->node_memory ? smc->node_memory : &sn0_hub_memory;
    g_autofree unsigned int *requested = NULL;
    g_autofree unsigned int *node_attachment = NULL;
    g_autofree unsigned int *board_attachment = NULL;
    g_autofree BlockBackend **flash_drives = NULL;
    g_autofree const SGIModelNode **nodes = NULL;
    unsigned int flash_count = 0;
    unsigned int node_count, node_index, board_index;
    unsigned int cpu_index = 0, cpu_base = 0;
    size_t io_board_count = 0, crossbar_count = 0, node_count_check;
    uint64_t ram_offset = 0;
    SGIModelAsicKind kind;

    if (!t && sgi_machine_chassis(machine)) {
        error_propagate_prepend(errp, local_err, "%s: ", mc->name);
        return false;
    }
    if (!t) {
        GString *names = g_string_new(NULL);
        const SGIModelTopologyList *entry;

        error_free(local_err);

        for (entry = sgi_model_catalogue()->topologies; entry;
             entry = entry->next) {
            if (sgi_machine_topology_matches_product(entry->value,
                                                      smc->product)) {
                if (names->len) {
                    g_string_append(names, ", ");
                }
                g_string_append(names, entry->value->name);
            }
        }
        error_setg(errp, "%s: unknown topology '%s'; known topologies: %s",
                  mc->name, s->topology_name ? s->topology_name : "(none)",
                  names->str);
        g_string_free(names, true);
        return false;
    }
    if (!sgi_machine_resolve_config(s, t, errp)) {
        return false;
    }
    if (!sgi_machine_validate_controller_inputs(s, t, errp)) {
        return false;
    }
    if (sgi_machine_has_identity_records(s)) {
        g_autofree const SGIModelController **controllers = NULL;
        size_t controller_count;
        bool has_l1 = false;
        size_t i;

        controllers = sgi_model_controller_array(t->controllers,
                                                  &controller_count);
        for (i = 0; i < controller_count; i++) {
            if (controllers[i]->kind == SGI_MODEL_CONTROLLER_KIND_L1) {
                has_l1 = true;
                break;
            }
        }
        if (!has_l1) {
            error_report("%s: chassis-eeprom/board-eeprom/spd-eeprom name "
                        "an L1 identity controller, which topology '%s' "
                        "does not have", mc->name, t->name);
            exit(EXIT_FAILURE);
        }
    }
    if (machine->kernel_filename) {
        error_setg(errp, "%s: stock-PROM boot only; boot a kernel through "
                  "the PROM's own TFTP netboot or a disk image, not -kernel",
                  mc->name);
        return false;
    }
    if (machine->numa_state && machine->numa_state->num_nodes) {
        error_setg(errp, "%s: -numa is refused; topology= selects the "
                  "wiring, -smp and -m select the population within it",
                  mc->name);
        return false;
    }
    /*
     * Every R10000 and R14000 is a single-core socket: -smp N means N
     * sockets of one core each, and which node each socket sits on
     * comes from the topology's accepted population whose total is N,
     * never from sockets= or cores= on the command line (QEMU's own
     * smp_props.prefer_sockets, set below, completes a bare -smp N to
     * exactly this shape already).
     */
    if (machine->smp.cores != 1 || machine->smp.threads != 1) {
        error_setg(errp, "%s: every processor is its own single-core "
                  "socket; pass -smp N alone, not cores= or threads=",
                  mc->name);
        return false;
    }
    /*
     * With one core and one thread per socket, QEMU derives sockets from
     * maxcpus, so a sockets= that disagrees can only come from an explicit
     * count; -smp N,maxcpus=M (M sockets fitted, N running) stays legal.
     */
    if (machine->smp.sockets != machine->smp.max_cpus) {
        error_setg(errp, "%s: -smp N means N sockets; pass -smp %u, not "
                  "sockets=%u", mc->name, machine->smp.max_cpus,
                  machine->smp.sockets);
        return false;
    }

    node_count = sgi_machine_topology_node_count(t);
    requested = g_new0(unsigned int, node_count);
    {
        const SGIModelCpuPopulation *population;
        const SGIModelCpuPopulation *second;
        g_autofree unsigned int *cpus_per_node = NULL;
        size_t population_node_count;

        population = sgi_machine_find_population_by_total(
            t, machine->smp.cpus, s->population, &second);
        if (second) {
            error_setg(errp, "%s: topology '%s' accepts %u CPUs as more "
                      "than one population; name one with population=, "
                      "per-node counts in node order such as 4:2",
                      mc->name, t->name, machine->smp.cpus);
            return false;
        }
        if (!population && s->population) {
            error_setg(errp, "%s: topology '%s' accepts no population %s "
                      "of %u CPUs", mc->name, t->name, s->population,
                      machine->smp.cpus);
            return false;
        }
        if (!population) {
            g_autofree char *totals = sgi_machine_accepted_totals(t);

            error_setg(errp, "%s: topology '%s' accepts %s CPUs, not %u%s",
                      mc->name, t->name, totals, machine->smp.cpus,
                      sgi_machine_nodes_hint(machine));
            return false;
        }
        cpus_per_node = sgi_model_array_uint32(population->cpus_per_node,
                                               &population_node_count);
        for (node_index = 0; node_index < node_count &&
             node_index < population_node_count; node_index++) {
            requested[node_index] = cpus_per_node[node_index];
        }
    }

    if (node_count > 1 && machine->ram_size % ((uint64_t)node_count * MiB)) {
        error_setg(errp, "%s: topology '%s' splits -m evenly across %u "
                  "nodes", mc->name, t->name, node_count);
        return false;
    }

    if (!sgi_machine_validate_memory_population(
            t, machine->ram_size / node_count / MiB)) {
        error_setg(errp, "%s: topology '%s' does not accept %" PRIu64
                  " MiB per node%s", mc->name, t->name,
                  machine->ram_size / node_count / MiB,
                  sgi_machine_nodes_hint(machine));
        return false;
    }

    /*
     * The boot-progress indicator is a Hub trace event; Bedrock has no
     * counterpart yet, so debug-leds (on by default) stays inert for it
     * rather than refusing every origin300 launch that does not turn it
     * off explicitly.
     */
    if (sgi_machine_topology_node0(t)->asic_kind == SGI_MODEL_ASIC_KIND_HUB &&
        s->debug_leds) {
        TraceEvent *ev = trace_event_name("sgi_hub_led");

        if (!ev) {
            error_setg(errp, "the sgi_hub_led trace event is missing, so "
                      "the firmware's boot-progress indicator cannot be "
                      "shown; build with it or pass debug-leds=off");
            return false;
        }
        trace_event_set_state_dynamic(ev, true);
    }

    /* Cache machine->possible_cpus before the node loop fills in cpu. */
    sgi_machine_possible_cpu_arch_ids(machine);

    /* One clock per processor entry; both node kinds share this rate rule. */
    s->cpu_refclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(s->cpu_refclk,
                sgi_machine_topology_node0(t)->cpu_slots->value->assembly->clock_hz);

    sgi_machine_build_module_containers(machine, t);

    kind = sgi_machine_topology_node0(t)->asic_kind;
    nodes = sgi_model_node_array(t->nodes, &node_count_check);
    g_assert(node_count_check == node_count);

    /*
     * One loop creates every node, the ASIC kind selecting the device
     * type from this two-entry table. The two kinds share no fields, so
     * what each configures below still differs; they no longer reach
     * that configuration through two separate paths, one of them
     * reachable only through an early return.
     */
    s->num_nodes = node_count;
    if (kind == SGI_MODEL_ASIC_KIND_HUB) {
        s->nodes = g_new0(SN0NodeState *, node_count);
    } else {
        s->bedrock_nodes = g_new0(DeviceState *, node_count);
        s->bedrock_ram = g_new0(MemoryRegion, node_count);
    }
    for (node_index = 0; node_index < node_count; node_index++) {
        g_autofree char *name = g_strdup_printf("node%u", node_index);
        Object *node = object_new(kind == SGI_MODEL_ASIC_KIND_HUB ?
                                  TYPE_SGI_SN0_NODE : TYPE_SGI_SN1_NODE);

        object_property_add_child(OBJECT(machine), name, node);
        object_unref(node);
        if (kind == SGI_MODEL_ASIC_KIND_HUB) {
            s->nodes[node_index] = SN0_NODE(node);
        } else {
            s->bedrock_nodes[node_index] = DEVICE(node);
        }
    }

    if (kind != SGI_MODEL_ASIC_KIND_HUB) {
        g_autofree const SGIModelXioAttachment **attachments = NULL;
        size_t attachment_count;

        uint64_t node_ram = machine->ram_size / node_count;
        size_t bedrock_board_capacity = sgi_machine_topology_io_board_count(t);

        s->io_boards = g_new0(SGIXioBoardState *, bedrock_board_capacity);
        s->board_links = g_new0(SGIXioLink, bedrock_board_capacity);

        attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                     &attachment_count);
        for (node_index = 0; node_index < node_count; node_index++) {
            SN1NodeState *node = SN1_NODE(s->bedrock_nodes[node_index]);
            g_autofree unsigned int *widgets = NULL;
            size_t widget_count = 0;
            size_t i;
            const SGIModelIoBoardList *board_entry;
            bool found_board = false;

            /*
             * Widget zero is the crossbar's own local view; the rest come
             * one per this node's own XIO attachment. The empty
             * head still needs a widget; the board loop selects the fitted
             * population after the node realizes.
             */
            widgets = g_new0(unsigned int, attachment_count + 1);
            widgets[widget_count++] = 0;
            for (i = 0; i < attachment_count; i++) {
                if (attachments[i]->node_index != node_index) {
                    continue;
                }
                widgets[widget_count++] = attachments[i]->widget;
            }

            qdev_prop_set_uint32(DEVICE(node), "nasid",
                                 sgi_machine_topology_nasid(t, node_index));
            node->model_node = nodes[node_index];
            for (board_entry = t->io_boards; board_entry;
                 board_entry = board_entry->next) {
                const SGIModelIoBoard *board = board_entry->value;
                const SGIModelXioAttachment *placement;

                if (board->preset != SGI_MODEL_IO_BOARD_PRESET_IO8 &&
                    board->preset != SGI_MODEL_IO_BOARD_PRESET_FUEL) {
                    continue;
                }
                if (!board->populated) {
                    continue;
                }
                if (board->attachment >= attachment_count) {
                    error_setg(errp, "topology '%s' io board '%s' names "
                               "attachment %u, past its %zu attachment(s)",
                               t->name, board->id, board->attachment,
                               attachment_count);
                    return false;
                }
                placement = attachments[board->attachment];
                if (placement->node_index != node_index) {
                    continue;
                }
                if (found_board || placement->crossbar_index >= 0 ||
                    (placement->widget != 14 && placement->widget != 15)) {
                    error_setg(errp, "topology '%s' node %u has invalid "
                               "XBridge I/O board placement", t->name,
                               node_index);
                    return false;
                }
                found_board = true;
                qdev_prop_set_uint8(DEVICE(node), "io-board-widget",
                                    placement->widget);
                if (board->part) {
                    qdev_prop_set_string(DEVICE(node), "io-board-part",
                                         board->part);
                }
            }
            if (!found_board) {
                error_setg(errp, "topology '%s' node %u has no XBridge "
                           "I/O board", t->name, node_index);
                return false;
            }
            node->xbridge_widgets = g_steal_pointer(&widgets);
            node->xbridge_widget_count = widget_count;
            /*
             * Each node's memory is its own share of machine->ram: one
             * region cannot sit at two node placements, and the claim
             * layer aliases each node's window by number anyway.
             */
            if (node_count == 1) {
                object_property_set_link(OBJECT(node), "ram",
                                         OBJECT(machine->ram), &error_abort);
            } else {
                memory_region_init_alias(&s->bedrock_ram[node_index],
                                         OBJECT(node), "sn1.node.ram",
                                         machine->ram,
                                         (uint64_t)node_index * node_ram,
                                         node_ram);
                object_property_set_link(OBJECT(node), "ram",
                                         OBJECT(&s->bedrock_ram[node_index]),
                                         &error_abort);
            }
            qdev_prop_set_uint32(DEVICE(node), "nr-cpus",
                                 requested[node_index]);
            {
                const SGIMachineNodeConfig *config =
                    &s->node_config[node_index];
                const SGIMachineL1Config *l1 =
                    &s->l1_config[config->controller_index];

                qdev_prop_set_uint64(DEVICE(node), "board-id-word",
                                     config->board_id_word);
                qdev_prop_set_uint8(DEVICE(node), "bedrock-revision",
                                    config->revision);
                qdev_prop_set_uint8(DEVICE(node), "local-region",
                                    config->local_region);
                qdev_prop_set_uint8(DEVICE(node), "initiator-region",
                                    config->initiator_region);
                qdev_prop_set_uint8(DEVICE(node), "installed-bank-mask",
                                    config->installed_bank_mask);
                qdev_prop_set_uint32(DEVICE(node), "l1-type-code",
                                     l1->reported_type);
                qdev_prop_set_int32(DEVICE(node), "l1-revision-major",
                                    l1->revision[0]);
                qdev_prop_set_int32(DEVICE(node), "l1-revision-minor",
                                    l1->revision[1]);
                qdev_prop_set_int32(DEVICE(node), "l1-revision-patch",
                                    l1->revision[2]);
                if (l1->mac_eeprom) {
                    qdev_prop_set_string(DEVICE(node), "l1-mac-eeprom",
                                         l1->mac_eeprom);
                }
            }
            qdev_prop_set_bit(DEVICE(node), "last-node",
                              node_index == node_count - 1);
            qdev_prop_set_uint32(DEVICE(node), "l1-bay",
                                 sgi_machine_node_module(t, node_index) ?
                                 sgi_machine_node_module(t, node_index)->
                                 initial_module_number : 0);
            qdev_connect_clock_in(DEVICE(node), "cpu-refclk", s->cpu_refclk);
        }

        for (node_index = 0; node_index < node_count; node_index++) {
            SN1NodeState *node = SN1_NODE(s->bedrock_nodes[node_index]);
            size_t i;

            sysbus_realize(SYS_BUS_DEVICE(node), &error_fatal);
            for (i = 0; i < requested[node_index]; i++) {
                machine->possible_cpus->cpus[cpu_index++].cpu =
                    CPU(node->cpu[i]);
            }
        }
        g_assert(cpu_index == machine->smp.cpus);
        if (!sgi_machine_build_io_assemblies(s, t, errp)) {
            return false;
        }
        /*
         * The NUMAlink cables join realized Bedrocks: each node creates its
         * Bedrock while it realizes, and a Bedrock's cable reference may be
         * set afterwards. Then every node derives which numbers it reaches.
         */
        if (!sgi_machine_build_bedrock_links(s, t, errp)) {
            return false;
        }
        for (node_index = 0; node_index < node_count; node_index++) {
            sn1_node_update_claims(SN1_NODE(s->bedrock_nodes[node_index]));
        }

        /*
         * Each fitted I/O board names the attachment of its XBridge head.
         * The node already created both heads from xio_attachments.
         */
        {
            g_autofree const SGIModelIoBoard **io_boards = NULL;
            size_t bedrock_io_board_count;

            io_boards = sgi_model_io_board_array(t->io_boards,
                                                 &bedrock_io_board_count);
            for (board_index = 0; board_index < bedrock_io_board_count;
                 board_index++) {
                const SGIModelIoBoard *record = io_boards[board_index];

                if (record->attachment >= attachment_count) {
                    error_setg(errp, "topology '%s' io board %u names "
                              "attachment %u, past its %zu XIO "
                              "attachment(s)", t->name, board_index,
                              record->attachment, attachment_count);
                    return false;
                }
                if (attachments[record->attachment]->node_index >= node_count ||
                    attachments[record->attachment]->crossbar_index >= 0) {
                    error_setg(errp, "topology '%s' io board %u has no direct "
                               "Bedrock attachment", t->name, board_index);
                    return false;
                }
                if (!sgi_machine_instantiate_io_board(
                        s, t, record, board_index,
                        attachments[record->attachment], errp)) {
                    return false;
                }
            }
        }
        if (!sgi_machine_load_flash(machine, t, s, errp)) {
            return false;
        }
        return true;
    }

    flash_drives = g_new0(BlockBackend *, node_count);
    for (node_index = 0; node_index < node_count; node_index++) {
        DriveInfo *drive = drive_get_by_index(IF_PFLASH, node_index);

        if (drive) {
            flash_drives[node_index] = blk_by_legacy_dinfo(drive);
            flash_count++;
        }
    }
    if (!machine->firmware && !flash_count && !qtest_enabled()) {
        error_setg(errp, "%s: supports stock-PROM boot only: pass -bios or "
                  "a pflash drive per node", mc->name);
        return false;
    }

    io_board_count = sgi_machine_topology_io_board_count(t);
    crossbar_count = sgi_machine_topology_crossbar_count(t);

    /* Indexed by io_boards position, one entry per board, not by node. */
    s->io_boards = g_new0(SGIXioBoardState *, io_board_count);
    s->crossbars = g_new0(SGIXbowState *, crossbar_count);
    s->node_links = g_new0(SGIXioLink, node_count);
    s->board_links = g_new0(SGIXioLink, io_board_count);
    s->slot_links = g_new0(SGIXioLink, sgi_machine_topology_xio_slot_count(t));
    s->slot_claimed = g_new0(bool, sgi_machine_topology_xio_slot_count(t));

    node_attachment = g_new0(unsigned int, node_count);
    board_attachment = g_new0(unsigned int, io_board_count);

    sgi_machine_attach_module_identities(s, t);

    s->rt_refclk = clock_new(OBJECT(machine), "rt-refclk");
    clock_set_hz(s->rt_refclk, SN0_HUB_RTC_CLOCK_HZ);

    sgi_machine_build_routers(s, t);
    sgi_machine_build_fabric_links(s, t);
    for (size_t router_index = 0; router_index < s->router_count;
        router_index++) {
        qdev_realize_and_unref(DEVICE(s->routers[router_index]), NULL,
                               &error_fatal);
    }

    /*
     * Every Hub in the machine, in canonical stride order: each Hub's own
     * "stride" property is its index here, and every Hub gets the same
     * array, itself included, so its own claim-alias walk can size and
     * aim one alias per peer.
     */
    g_autofree SGIHubState **net_peers = g_new(SGIHubState *, node_count);
    for (node_index = 0; node_index < node_count; node_index++) {
        net_peers[node_index] = &s->nodes[node_index]->hub;
    }

    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node = s->nodes[node_index];
        const SGIModelNode *model_node = nodes[node_index];
        uint64_t ram_size = machine->ram_size / node_count;
        uint16_t bank_mb[SN0_MAX_MEMORY_BANKS];
        unsigned int populated_banks;
        QList *banks_mb;
        unsigned int bank;
        Chardev *aux_chr;

        if (!sn0_model_memory_layout(node_memory, ram_size,
                                     bank_mb, &populated_banks)) {
            error_setg(errp, "%s: node %u's %" PRIu64 " MiB cannot be "
                      "populated from %s memory banks", mc->name,
                      node_index, ram_size / MiB, node_memory->board);
            return false;
        }
        banks_mb = qlist_new();
        for (bank = 0; bank < populated_banks; bank++) {
            qlist_append_int(banks_mb, bank_mb[bank]);
        }

        qdev_prop_set_uint32(DEVICE(node),
                             "nasid",
                             sgi_machine_topology_nasid(t, node_index));
        node->flash_drive = flash_drives[node_index];
        node->model_node = model_node;
        node->memory = node_memory;
        node->slave_role = model_node->slave_role;
        qdev_prop_set_uint32(DEVICE(node), "cpu-base", cpu_base);
        qdev_prop_set_uint32(DEVICE(node), "nr-cpus", requested[node_index]);
        qdev_prop_set_string(DEVICE(node), "cpu-type", machine->cpu_type);
        object_property_set_link(OBJECT(node), "ram", OBJECT(machine->ram),
                                 &error_abort);
        qdev_prop_set_uint64(DEVICE(node), "ram-start", ram_offset);
        qdev_prop_set_array(DEVICE(node), "banks-mb", banks_mb);
        qdev_connect_clock_in(DEVICE(node), "cpu-refclk", s->cpu_refclk);
        qdev_connect_clock_in(DEVICE(node), "rt-refclk", s->rt_refclk);

        qdev_prop_set_uint32(DEVICE(&node->hub), "nasid", 0);
        qdev_prop_set_uint8(DEVICE(&node->hub), "elsc-dip-switches",
                            s->elsc_dip_switches);
        if (!model_node->slave_role) {
            int line = sgi_machine_compute_serial_line(t, node_index,
                                                        SN0_ELSC_CHARDEV);

            aux_chr = serial_hd(line);
            if (aux_chr) {
                object_property_set_link(OBJECT(&node->hub), "elsc-chardev",
                                         OBJECT(aux_chr), &error_fatal);
            }
        }
        qdev_prop_set_uint32(DEVICE(&node->hub), "stride", node_index);
        sgi_hub_set_net_peers(&node->hub, net_peers, node_count);

        {
            bool has_junk_uart = (int)node_index == t->junk_uart &&
                                 s->junk_uart_present;

            qdev_prop_set_bit(DEVICE(&node->hub), "junk-uart", has_junk_uart);
            if (has_junk_uart) {
                Chardev *junk_chr = serial_hd(
                    sgi_machine_compute_serial_line(t, node_index,
                                                    SN0_JUNK_UART_CHARDEV));

                if (junk_chr) {
                    object_property_set_link(OBJECT(&node->hub),
                                             "junk-uart-chardev",
                                             OBJECT(junk_chr), &error_fatal);
                }
            }
        }

        cpu_base += requested[node_index];
        ram_offset += ram_size;
    }
    g_assert(ram_offset == machine->ram_size);

    if (!sgi_machine_index_xio_attachments(t, node_attachment,
                                           board_attachment, errp)) {
        return false;
    }

    if (crossbar_count) {
        sgi_machine_build_crossbars(s, t);
    }

    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node = s->nodes[node_index];
        unsigned int slice;

        sysbus_realize(SYS_BUS_DEVICE(node), &error_fatal);
        for (slice = 0; slice < requested[node_index]; slice++) {
            machine->possible_cpus->cpus[cpu_index++].cpu =
                CPU(node->cpu[slice]);
        }
    }
    g_assert(cpu_index == machine->smp.cpus);

    sn0_map_cpu_big_windows(OBJECT(s), s->nodes, s->num_nodes,
                            &s->big_window_route);

    /*
     * The two-chassis Origin 200's two Hub-embedded ELSCs share
     * arbitration; a shared-controller topology (Origin 2000, and the
     * two-node Onyx2 deskside) has no per-node ELSC here
     * (sgi_machine_build_controllers builds that).
     */
    if (node_count == 2 && s->nodes[0]->hub.elsc && s->nodes[1]->hub.elsc) {
        SGIElscState *master = NULL;
        SGIElscState *slave = NULL;

        for (node_index = 0; node_index < node_count; node_index++) {
            if (nodes[node_index]->slave_role) {
                slave = s->nodes[node_index]->hub.elsc;
            } else {
                master = s->nodes[node_index]->hub.elsc;
            }
        }
        g_assert(master);
        g_assert(slave);
        sgi_elsc_share_arbitration(master, slave, 5);
    }

    /*
     * Every Hub and Router now has its port cables wired and is
     * realized; the machine's own cold reset that follows board
     * construction derives every Hub's initial reachability and link
     * state through the ordinary reset-exit path
     * (sgi_hub_reset_exit(), sgi_router_reset_exit()), the same path a
     * guest-triggered reset uses later. No separate build step is
     * needed.
     */

    /*
     * Every node is realized now, so a direct board's claim view exists
     * and a Crossbow-attached node's physical map can be read.
     */
    {
        g_autofree const SGIModelIoBoard **io_boards = NULL;
        g_autofree const SGIModelXioAttachment **attachments = NULL;
        size_t io_board_count_check, attachment_count;

        io_boards = sgi_model_io_board_array(t->io_boards, &io_board_count_check);
        g_assert(io_board_count_check == io_board_count);
        attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                     &attachment_count);
        for (board_index = 0; board_index < io_board_count; board_index++) {
            const SGIModelXioAttachment *attachment =
                attachments[board_attachment[board_index]];

            if (!sgi_machine_instantiate_io_board(
                    s, t, io_boards[board_index], board_index, attachment,
                    errp)) {
                return false;
            }
        }
        for (node_index = 0; node_index < node_count; node_index++) {
            const SGIModelXioAttachment *attachment =
                attachments[node_attachment[node_index]];

            if (attachment->crossbar_index >= 0) {
                SGIXioLink *link = &s->node_links[node_index];
                g_autofree char *name =
                    g_strdup_printf("node%u-link", node_index);

                sgi_xio_link_init(
                    link, OBJECT(s), name,
                    OBJECT(&s->nodes[node_index]->hub), 0,
                    OBJECT(s->crossbars[attachment->crossbar_index]),
                    attachment->widget);
                if (!sgi_xio_link_connect(link, errp)) {
                    return false;
                }
            }
        }
    }
    if (!sgi_machine_build_controllers(s, t, errp)) {
        return false;
    }

    if (!sgi_machine_load_flash(machine, t, s, errp)) {
        return false;
    }

    /*
     * Every node and board this run builds is linked now, so every
     * Crossbow can realize in one pass, here. A `-device` board realized
     * later links to the empty slot it names and attaches to its
     * realized Crossbow live.
     */
    if (!sgi_machine_realize_crossbars(s, t, errp)) {
        return false;
    }
    return true;
}

/*
 * Brings up every empty XIO slot a `-device` board claimed. The board's
 * own realize, which runs once machine init has returned and every
 * Crossbow this machine builds has realized, linked it to the slot's
 * Crossbow port: its Bridge or widget took the port as its fabric, and
 * the Crossbow attached the widget's faces live, which also turned the
 * port from an empty slot into one reporting a widget present. What is
 * left is the widget's own link state, which this notifier sets alive,
 * with the same pre-reset health restore every topology link gets
 * (sgi_machine_build_crossbars()'s "link-reset-restore-alive"): the
 * stock PROM resets each link it finds attached and expects it back
 * alive. It runs before the machine's cold reset, which reapplies that
 * health from the Crossbow's startup mask.
 */
static void sgi_machine_done(Notifier *notifier, void *data)
{
    SGIMachineState *s = container_of(notifier, SGIMachineState,
                                      machine_done);
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));
    g_autofree const SGIModelXioSlot **slots = NULL;
    size_t slot_count, i;

    slots = sgi_model_xio_slot_array(t->xio_slots, &slot_count);
    for (i = 0; i < slot_count; i++) {
        if (!sgi_xio_link_complete(&s->slot_links[i])) {
            continue;
        }
        sgi_xbow_mark_widget_alive(s->crossbars[slots[i]->crossbar_index],
                                   slots[i]->widget, true, &error_fatal);
    }
}

/*
 * Why this machine cannot fit a board in XIO slot @name when its chassis
 * has that slot but the fitted node-population row does not use it:
 * the node board that controls it in the smallest row that does. NULL
 * when the chassis has no such slot either.
 */
static char *sgi_machine_slot_needs_node(MachineState *machine,
                                         const char *name)
{
    const SGIModelTopology *chassis = sgi_machine_chassis(machine);
    const SGIModelTopology *t = sgi_machine_topology(machine);
    const SGIModelNodePopulationList *row;
    const SGIModelNodePopulation *smallest = NULL;
    const SGIModelXioSlotList *slot;
    uint32_t position = 0, controller = 0;
    g_autofree const SGIModelNode **nodes = NULL;
    size_t node_count;

    for (slot = chassis->xio_slots; slot; slot = slot->next, position++) {
        if (g_str_equal(slot->value->label, name)) {
            break;
        }
    }
    if (!slot) {
        return NULL;
    }
    for (row = chassis->node_populations; row; row = row->next) {
        const SGIModelPopulationSlotList *usable;

        for (usable = row->value->xio_slots; usable; usable = usable->next) {
            if (usable->value->slot == position &&
                (!smallest || QAPI_LIST_LENGTH(row->value->nodes) <
                              QAPI_LIST_LENGTH(smallest->nodes))) {
                smallest = row->value;
                controller = usable->value->controlling_node;
            }
        }
    }
    if (!smallest) {
        return NULL;
    }
    nodes = sgi_model_node_array(chassis->nodes, &node_count);
    return g_strdup_printf("XIO slot %s of topology '%s' needs node board "
                           "n%u, which nodes=%zu does not fit; nodes=%zu "
                           "and up fit it", name, chassis->name,
                           nodes[controller]->slot, QAPI_LIST_LENGTH(t->nodes),
                           QAPI_LIST_LENGTH(smallest->nodes));
}

/*
 * The machine's side of a board fitted by slot name (TYPE_SGI_XIO_SLOTS,
 * xio-board.h): the named empty slot's module and widget, the link
 * from its Crossbow port to the board, and the board's link to the
 * graphics credits of the node that controls the slot.
 */
static void sgi_machine_link_slot(SGIMachineState *s,
                                  const SGIModelTopology *t,
                                  const SGIModelXioSlot *slot, size_t index,
                                  Object *board)
{
    g_autofree char *slot_name = sgi_machine_xio_slot_name(t, slot);
    g_autofree char *name = g_strdup_printf("%s-link", slot_name);

    s->slot_claimed[index] = true;
    sgi_xio_link_init(&s->slot_links[index], OBJECT(s), name,
                      OBJECT(s->crossbars[slot->crossbar_index]),
                      slot->widget, board, 0);
    sgi_machine_link_board_pi(s, board, slot->controlling_node);
}

static SGIXioLink *sgi_machine_claim_xio_slot(Object *obj, Object *board,
                                              const char *name,
                                              SGIXioSlotPlace *place,
                                              Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));
    g_autofree const SGIModelXioSlot **slots = NULL;
    const SGIModelIoBoardList *io_board;
    size_t slot_count, i;
    ssize_t found = -1;

    slots = sgi_model_xio_slot_array(t->xio_slots, &slot_count);
    for (i = 0; i < slot_count; i++) {
        g_autofree char *slot_name = sgi_machine_xio_slot_name(t, slots[i]);
        uint32_t slot_module = sgi_machine_xio_slot_module(t, slots[i]);

        if (!g_str_equal(slot_name, name) &&
            !g_str_equal(slots[i]->label, name)) {
            continue;
        }
        if (place->module != SGI_XIO_BOARD_MODULE_UNSET &&
            place->module != slot_module) {
            continue;
        }
        if (found >= 0) {
            error_setg(errp, "XIO slot %s is in more than one module of "
                       "topology '%s'; name the module with module=",
                       name, t->name);
            return NULL;
        }
        found = i;
    }
    if (found < 0) {
        for (i = 0; i < slot_count; i++) {
            if (g_str_equal(slots[i]->label, name)) {
                error_setg(errp, "XIO slot %s of topology '%s' is in module "
                           "%u, not module %u", name, t->name,
                           sgi_machine_xio_slot_module(t, slots[i]),
                           place->module);
                return NULL;
            }
        }
        for (io_board = t->io_boards; io_board; io_board = io_board->next) {
            if (io_board->value->label &&
                g_str_equal(io_board->value->label, name)) {
                error_setg(errp, "XIO slot %s of topology '%s' already holds "
                           "the topology's own board", name, t->name);
                return NULL;
            }
        }
        {
            g_autofree char *needs =
                sgi_machine_slot_needs_node(MACHINE(s), name);

            if (needs) {
                error_setg(errp, "%s", needs);
                return NULL;
            }
        }
        /*
         * An Origin 300 has no XIO slots at all. Its board's XIO connector
         * is an external cable port for an expansion brick, which the model
         * does not provide, and no XIO board attaches to it directly.
         */
        if (s->bedrock_nodes && !t->xio_slots) {
            error_setg(errp, "topology '%s' has no XIO slots: the XIO "
                       "connector is an external cable port for an expansion "
                       "brick, and no XIO board attaches to it", t->name);
            return NULL;
        }
        error_setg(errp, "topology '%s' has no empty XIO slot %s", t->name,
                   name);
        return NULL;
    }
    if (s->slot_claimed[found]) {
        error_setg(errp, "XIO slot %s already holds a board", name);
        return NULL;
    }
    sgi_machine_link_slot(s, t, slots[found], found, board);
    place->module = sgi_machine_xio_slot_module(t, slots[found]);
    place->widget = slots[found]->widget;
    place->position = found;
    place->name = sgi_machine_xio_slot_name(t, slots[found]);
    place->label = g_strdup(slots[found]->label);
    return &s->slot_links[found];
}

/*
 * A board given its fabric outright: when that fabric is one of this
 * machine's empty slots' Crossbow ports, the board must name that slot's
 * own module and widget, and be the first to take it; it is then known
 * by the slot's machine-wide name.
 */
static bool sgi_machine_claim_xio_fabric(Object *obj, Object *board,
                                         MemoryRegion *fabric,
                                         uint32_t module, uint32_t widget,
                                         char **name, SGIXioLink **link,
                                         Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));
    g_autofree const SGIModelXioSlot **slots = NULL;
    size_t slot_count, i;

    *link = NULL;
    slots = sgi_model_xio_slot_array(t->xio_slots, &slot_count);
    for (i = 0; i < slot_count; i++) {
        uint32_t slot_module = sgi_machine_xio_slot_module(t, slots[i]);
        SGIXbowState *crossbow = s->crossbars[slots[i]->crossbar_index];

        if (fabric != sgi_xbow_packet_ingress(crossbow, slots[i]->widget)) {
            continue;
        }
        if (module != slot_module || widget != slots[i]->widget) {
            error_setg(errp, "fabric of XIO slot %s is module %u widget %u, "
                       "not the module %u widget %u the board names",
                       slots[i]->label, slot_module, slots[i]->widget,
                       module, widget);
            return false;
        }
        if (s->slot_claimed[i]) {
            error_setg(errp, "XIO slot %s already holds a board",
                       slots[i]->label);
            return false;
        }
        sgi_machine_link_slot(s, t, slots[i], i, board);
        *link = &s->slot_links[i];
        *name = sgi_machine_xio_slot_name(t, slots[i]);
        return true;
    }
    return true;
}

void sgi_machine_init(MachineState *machine)
{
    SGIMachineState *s = SGI_MACHINE(machine);

    sgi_machine_realize(machine, &error_fatal);
    s->machine_done.notify = sgi_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
}

static void sgi_machine_reset(MachineState *machine, ResetType type)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    const SGIModelTopology *t = sgi_machine_topology(machine);
    uint32_t mode_bits;
    CPUState *cs;
    unsigned int node, slice;

    for (unsigned int i = 0; i < s->association_count; i++) {
        s->query_associations[i].reachable = s->initial_reachability[i];
    }

    /*
     * Every Bedrock node resets itself: "start-powered-off" needs its
     * cpu_reset() to run before qemu_devices_reset(), the reverse of
     * the Hub's own order below, so it cannot share this prefix.
     */
    if (t && sgi_machine_topology_node0(t)->asic_kind != SGI_MODEL_ASIC_KIND_HUB) {
        unsigned int i;

        if (s->num_nodes == 1) {
            sn1_node_full_reset(SN1_NODE(s->bedrock_nodes[0]), type);
            return;
        }
        for (i = 0; i < s->num_nodes; i++) {
            sn1_node_reset_cpus(SN1_NODE(s->bedrock_nodes[i]));
        }
        qemu_devices_reset(type);
        for (i = 0; i < s->num_nodes; i++) {
            sn1_node_release_cpus(SN1_NODE(s->bedrock_nodes[i]));
        }
        return;
    }

    mode_bits = t ?
        sgi_machine_topology_node0(t)->cpu_slots->value->assembly->mode_bits : 0;
    qemu_devices_reset(type);
    CPU_FOREACH(cs) {
        CPUMIPSState *env = &MIPS_CPU(cs)->env;

        cpu_reset(cs);
        env->CP0_Config0 =
            (env->CP0_Config0 & ~(uint32_t)SN0_PROCESSOR_MODE_MASK) |
            (mode_bits & SN0_PROCESSOR_MODE_MASK);
    }
    for (node = 0; node < s->num_nodes; node++) {
        SN0NodeState *node_state = s->nodes[node];

        for (slice = 0; slice < node_state->nr_cpus; slice++) {
            sgi_hub_set_cpu_enable(&node_state->hub, slice, true, true);
        }
    }
}

/* ---- Machine properties (once, on the family base) ---- */

/*
 * The topology, its node count and its processor population choose what
 * the machine builds, and the machine keeps reading them afterwards (a
 * system reset walks the built topology). Once it is built, a change
 * would describe a machine that does not exist, so it is refused.
 */
static bool sgi_machine_built(const char *name, Error **errp)
{
    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "%s cannot change once the machine is built", name);
        return true;
    }
    return false;
}

static char *sgi_machine_get_population(Object *obj, Error **errp)
{
    return g_strdup(SGI_MACHINE(obj)->population ?
                    SGI_MACHINE(obj)->population : "");
}

static void sgi_machine_set_population(Object *obj, const char *value,
                                       Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    if (sgi_machine_built("population", errp)) {
        return;
    }
    g_free(s->population);
    s->population = *value ? g_strdup(value) : NULL;
}

static char *sgi_machine_get_topology(Object *obj, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    return g_strdup(s->topology_name ? s->topology_name : "");
}

static void sgi_machine_set_topology(Object *obj, const char *value,
                                     Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    if (sgi_machine_built("topology", errp)) {
        return;
    }
    g_free(s->topology_name);
    s->topology_name = g_strdup(value);
    s->view = NULL;
}

static void sgi_machine_get_nodes(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);
    uint32_t value = s->nodes_requested;

    if (!value) {
        const SGIModelTopology *chassis = sgi_machine_chassis(MACHINE(obj));
        const SGIModelNodePopulation *row =
            chassis ? sgi_model_default_population(chassis) : NULL;

        value = row ? QAPI_LIST_LENGTH(row->nodes) : 0;
    }
    visit_type_uint32(v, name, &value, errp);
}

static void sgi_machine_set_nodes(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);
    uint32_t value;

    if (sgi_machine_built(name, errp) ||
        !visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!value) {
        error_setg(errp, "nodes must be at least 1");
        return;
    }
    s->nodes_requested = value;
    s->view = NULL;
}

/*
 * origin2000 only: which sgi-xio-baseio preset builds the product's one
 * catalogue-declared I/O board. "io6" (the plain card) by default; "io6g"
 * asks for the variant whose second IOC3 carries a keyboard/mouse pair.
 */
static char *sgi_machine_get_baseio_preset(Object *obj, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    return g_strdup(s->baseio_preset ? s->baseio_preset : "io6");
}

static void sgi_machine_set_baseio_preset(Object *obj, const char *value,
                                          Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    /*
     * Only the two BaseIO cards may take IO1. sgi-xio-baseio builds other
     * presets too (the MENET), and would put one in place of the BaseIO.
     */
    if (!g_str_equal(value, "io6") && !g_str_equal(value, "io6g")) {
        error_setg(errp, "baseio-preset '%s' is not a BaseIO card; expected "
                   "io6 or io6g", value);
        return;
    }
    g_free(s->baseio_preset);
    s->baseio_preset = g_strdup(value);
}

static bool sgi_machine_get_debug_leds(Object *obj, Error **errp)
{
    return SGI_MACHINE(obj)->debug_leds;
}

static void sgi_machine_set_debug_leds(Object *obj, bool value, Error **errp)
{
    SGI_MACHINE(obj)->debug_leds = value;
}

static bool sgi_machine_get_junk_uart(Object *obj, Error **errp)
{
    return SGI_MACHINE(obj)->junk_uart_present;
}

static void sgi_machine_set_junk_uart(Object *obj, bool value, Error **errp)
{
    SGI_MACHINE(obj)->junk_uart_present = value;
}

static void sgi_machine_get_elsc_dip_switches(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    uint8_t value = SGI_MACHINE(obj)->elsc_dip_switches;

    visit_type_uint8(v, name, &value, errp);
}

static void sgi_machine_set_elsc_dip_switches(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    uint8_t value;

    if (visit_type_uint8(v, name, &value, errp)) {
        SGI_MACHINE(obj)->elsc_dip_switches = value;
    }
}

static void sgi_machine_get_fuel_board_id_word(Object *obj, Visitor *v,
                                                const char *name, void *opaque,
                                                Error **errp)
{
    uint64_t value = SGI_MACHINE(obj)->fuel_board_id_word;

    visit_type_uint64(v, name, &value, errp);
}

static void sgi_machine_set_fuel_board_id_word(Object *obj, Visitor *v,
                                                const char *name, void *opaque,
                                                Error **errp)
{
    uint64_t value;

    if (visit_type_uint64(v, name, &value, errp)) {
        SGI_MACHINE(obj)->fuel_board_id_word = value;
        SGI_MACHINE(obj)->fuel_board_id_word_set = true;
    }
}

static void sgi_machine_get_fuel_ioc3_subsystem_id(Object *obj, Visitor *v,
                                                    const char *name,
                                                    void *opaque,
                                                    Error **errp)
{
    uint16_t value = SGI_MACHINE(obj)->fuel_ioc3_subsystem_id;

    visit_type_uint16(v, name, &value, errp);
}

static void sgi_machine_set_fuel_ioc3_subsystem_id(Object *obj, Visitor *v,
                                                    const char *name,
                                                    void *opaque,
                                                    Error **errp)
{
    uint16_t value;

    if (visit_type_uint16(v, name, &value, errp)) {
        SGI_MACHINE(obj)->fuel_ioc3_subsystem_id = value;
        SGI_MACHINE(obj)->fuel_ioc3_subsystem_id_set = true;
    }
}

static char *sgi_machine_get_fuel_mac_eeprom(Object *obj, Error **errp)
{
    return g_strdup(SGI_MACHINE(obj)->fuel_mac_eeprom);
}

static void sgi_machine_set_fuel_mac_eeprom(Object *obj, const char *value,
                                            Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    g_free(s->fuel_mac_eeprom);
    s->fuel_mac_eeprom = g_strdup(value);
}

static void sgi_machine_get_fuel_l1_type_code(Object *obj, Visitor *v,
                                               const char *name, void *opaque,
                                               Error **errp)
{
    uint8_t value = SGI_MACHINE(obj)->fuel_l1_type_code;

    visit_type_uint8(v, name, &value, errp);
}

static void sgi_machine_set_fuel_l1_type_code(Object *obj, Visitor *v,
                                               const char *name, void *opaque,
                                               Error **errp)
{
    uint8_t value;

    if (visit_type_uint8(v, name, &value, errp)) {
        SGI_MACHINE(obj)->fuel_l1_type_code = value;
        SGI_MACHINE(obj)->fuel_l1_type_code_set = true;
    }
}

static char *sgi_machine_get_fuel_l1_revision(Object *obj, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    if (!s->fuel_l1_revision_set) {
        return NULL;
    }
    return g_strdup_printf("%d.%d.%d", s->fuel_l1_revision[0],
                           s->fuel_l1_revision[1], s->fuel_l1_revision[2]);
}

static void sgi_machine_set_fuel_l1_revision(Object *obj, const char *value,
                                              Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);
    int32_t revision[3];
    const char *p = value;

    if (!p) {
        goto invalid;
    }
    for (size_t i = 0; i < ARRAY_SIZE(revision); i++) {
        int32_t number = 0;

        if (!g_ascii_isdigit(*p)) {
            goto invalid;
        }
        do {
            unsigned digit = *p++ - '0';

            if (number > (INT32_MAX - digit) / 10) {
                goto invalid;
            }
            number = number * 10 + digit;
        } while (g_ascii_isdigit(*p));
        revision[i] = number;
        if (i < ARRAY_SIZE(revision) - 1) {
            if (*p++ != '.') {
                goto invalid;
            }
        } else if (*p) {
            goto invalid;
        }
    }
    memcpy(s->fuel_l1_revision, revision, sizeof(revision));
    s->fuel_l1_revision_set = true;
    return;

invalid:
    error_setg(errp, "fuel-l1-revision requires three decimal components "
               "(major.minor.patch), each at most %d", INT32_MAX);
}

static void sgi_machine_get_fuel_bedrock_revision(Object *obj, Visitor *v,
                                                   const char *name,
                                                   void *opaque, Error **errp)
{
    uint8_t value = SGI_MACHINE(obj)->fuel_bedrock_revision;

    visit_type_uint8(v, name, &value, errp);
}

static void sgi_machine_set_fuel_bedrock_revision(Object *obj, Visitor *v,
                                                   const char *name,
                                                   void *opaque, Error **errp)
{
    uint8_t value;

    if (visit_type_uint8(v, name, &value, errp)) {
        SGI_MACHINE(obj)->fuel_bedrock_revision = value;
        SGI_MACHINE(obj)->fuel_bedrock_revision_set = true;
    }
}

/*
 * Asks each Hub for its own currently derived reachability, replacing
 * the fabric's central "routes" property: the same string, from each
 * Hub's own tables instead of one shared object's map.
 */
static char *sgi_machine_get_routes(Object *obj, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);
    GString *result = g_string_new(NULL);
    unsigned int from, to;

    for (from = 0; from < s->num_nodes; from++) {
        for (to = 0; to < s->num_nodes; to++) {
            uint32_t number;
            uint64_t vector;
            bool reachable;

            if (from == to) {
                continue;
            }
            number = sgi_hub_programmed_nasid(&s->nodes[to]->hub);
            reachable = sgi_hub_route(&s->nodes[from]->hub, number, &vector);
            g_string_append_printf(result, "%s%u>%u=", result->len ? " " : "",
                                   from, to);
            if (reachable) {
                g_string_append_printf(result, "0x%" PRIx64, vector);
            } else {
                g_string_append_c(result, '-');
            }
        }
    }
    return g_string_free(result, false);
}

static void sgi_machine_instance_init(Object *obj)
{
    MachineState *machine = MACHINE(obj);
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(obj);
    const SGIModelTopology *t = smc->default_topology ?
        sgi_model_find_topology(smc->default_topology) : NULL;

    SGI_MACHINE(obj)->debug_leds = true;
    /*
     * With no -smp at all QEMU leaves smp.sockets at 1 beside the class's
     * default_cpus; the product's default population is that many
     * single-core sockets, which is what -smp N would have said.
     */
    if (t) {
        machine->smp.sockets = sgi_machine_topology_default_cpus(t);
        machine->smp.cores = 1;
        machine->smp.threads = 1;
    }
}

static void sgi_machine_instance_finalize(Object *obj)
{
    SGIMachineState *s = SGI_MACHINE(obj);
    size_t i;

    if (s->management_registered) {
        vmstate_unregister(NULL, &vmstate_sgi_machine_management, s);
    }
    sgi_l1_query_router_destroy(&s->query_router);
    for (i = 0; i < s->controller_count; i++) {
        if (s->controller_reset_registered[i]) {
            qemu_unregister_resettable(OBJECT(s->controllers[i]));
            qemu_unregister_resettable(OBJECT(s->controller_bics[i]));
        }
        if (s->controller_bics[i]) {
            object_unref(OBJECT(s->controller_bics[i]));
        }
        if (s->controllers[i]) {
            object_unref(OBJECT(s->controllers[i]));
        }
    }
    for (i = 0; i < s->io_peer_count; i++) {
        if (s->io_peer_notifiers[i].endpoint) {
            notifier_remove(&s->io_peer_notifiers[i].notifier);
        }
        if (s->io_peers[i]) {
            object_unref(OBJECT(s->io_peers[i]));
        }
    }
    g_free(s->controllers);
    g_free(s->controller_bics);
    g_free(s->controller_reset_registered);
    g_free(s->owner_controllers);
    g_free(s->query_owners);
    g_free(s->query_associations);
    g_free(s->query_attachments);
    g_free(s->initial_reachability);
    g_free(s->io_peer_notifiers);
    g_free(s->assembly_ownership);
    g_free(s->nodes);
    g_free(s->bedrock_nodes);
    g_free(s->node_config);
    g_free(s->l1_config);
    g_free(s->io_config);
    g_free(s->io_peers);
    g_free(s->routers);
    g_free(s->crossbars);
    g_free(s->io_boards);
    g_free(s->node_links);
    g_free(s->board_links);
    g_free(s->slot_links);
    g_free(s->slot_claimed);
    g_free(s->topology_name);
    g_free(s->population);
    g_free(s->baseio_preset);
    g_free(s->fuel_mac_eeprom);
    for (i = 0; i < ARRAY_SIZE(sgi_machine_identity_records); i++) {
        qapi_free_strList(*sgi_machine_identity_slot(
            obj, (void *)&sgi_machine_identity_records[i]));
    }
}

static void sgi_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIXioSlotsClass *xc = SGI_XIO_SLOTS_CLASS(oc);

    xc->claim = sgi_machine_claim_xio_slot;
    xc->claim_fabric = sgi_machine_claim_xio_fabric;

    mc->init = sgi_machine_init;
    mc->reset = sgi_machine_reset;
    mc->possible_cpu_arch_ids = sgi_machine_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = sgi_machine_cpu_index_to_props;
    mc->get_default_cpu_node_id = sgi_machine_get_default_cpu_node_id;
    mc->default_nic = "sgi-ioc3-eth";
    mc->block_default_type = IF_SCSI;
    /* Node memory is memdev-backed -numa nodes; bare mem= has no backend. */
    mc->numa_mem_supported = false;
    /*
     * Every processor is a single-core socket, so a bare -smp N (or no
     * -smp at all, completing to mc->default_cpus) should complete to N
     * sockets of one core each, not one socket of N cores.
     */
    mc->smp_props.prefer_sockets = true;

    /*
     * Every XIO board class is TYPE_SYS_BUS_DEVICE (sgi-xio-board's own
     * parent); qdev's -device path refuses any dynamic sysbus device the
     * machine has not named here, on top of each class's own
     * user_creatable. An empty XIO slot's link (sgi_machine_claim_xio_slot())
     * is what a late board plugs into.
     */
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_PCI_EXPANSION);
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_PCI_CARRIER);
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_PCI_SHOEHORN);
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_XIO_BASEIO);
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_KONA);
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_MGRAS);

    object_class_property_add_str(oc, "topology", sgi_machine_get_topology,
                                  sgi_machine_set_topology);
    object_class_property_set_description(oc, "topology",
        "Select the product's wiring: nodes, modules, cabling and I/O "
        "population. Defaults to the product's shipped configuration; "
        "an unrecognized name (including 'help') reports every topology "
        "this product accepts.");

    object_class_property_add(oc, "nodes", "uint32", sgi_machine_get_nodes,
                              sgi_machine_set_nodes, NULL, NULL);
    object_class_property_set_description(oc, "nodes",
        "How many node boards the chassis fits, in the order SGI fills "
        "its node slots (n1, then n2, and so on). Defaults to the most "
        "the topology accepts; the count decides the Routers fitted and "
        "which XIO slots work.");

    object_class_property_add_str(oc, "population",
                                  sgi_machine_get_population,
                                  sgi_machine_set_population);
    object_class_property_set_description(oc, "population",
        "Per-node processor counts in node order, colon-separated (4:2), "
        "naming one of the topology's accepted populations. Needed only "
        "where -smp alone matches more than one.");

    object_class_property_add_bool(oc, "debug-leds", sgi_machine_get_debug_leds,
                                   sgi_machine_set_debug_leds);
    object_class_property_set_description(oc, "debug-leds",
        "Show the firmware's boot-progress indicator, decoded. On by "
        "default; the codes go to the log destination.");

    object_class_property_add_bool(oc, "junk-uart", sgi_machine_get_junk_uart,
                                   sgi_machine_set_junk_uart);
    object_class_property_set_description(oc, "junk-uart",
        "Fit the junk-bus UART, a manufacturing/bring-up serial port a "
        "shipped machine does not have. Off by default; on, it takes the "
        "fourth character device (serial_hd index 3).");

    object_class_property_add(oc, "elsc-dip-switches", "uint8",
                              sgi_machine_get_elsc_dip_switches,
                              sgi_machine_set_elsc_dip_switches, NULL, NULL);
    object_class_property_set_description(oc, "elsc-dip-switches",
        "Set the eight physical ELSC DIP switches as asserted bits. The "
        "controller reports their active-low wire value to firmware.");

    object_class_property_add(oc, "fuel-board-id-word", "uint64",
                              sgi_machine_get_fuel_board_id_word,
                              sgi_machine_set_fuel_board_id_word, NULL, NULL);
    object_class_property_set_description(oc, "fuel-board-id-word",
        "Full 64-bit board-identification launch input; applicability and "
        "requirements are declared by the selected model.");

    object_class_property_add(oc, "fuel-ioc3-subsystem-id", "uint16",
                              sgi_machine_get_fuel_ioc3_subsystem_id,
                              sgi_machine_set_fuel_ioc3_subsystem_id,
                              NULL, NULL);
    object_class_property_set_description(oc, "fuel-ioc3-subsystem-id",
        "IOC3 PCI subsystem ID launch input; applicability and requirements "
        "are declared by the selected model.");

    object_class_property_add(oc, "fuel-l1-type-code", "uint8",
                              sgi_machine_get_fuel_l1_type_code,
                              sgi_machine_set_fuel_l1_type_code, NULL, NULL);
    object_class_property_set_description(oc, "fuel-l1-type-code",
        "L1 local identity reply byte; applicability and requirements are "
        "declared by the selected model. Accepts a nonzero byte.");

    object_class_property_add_str(oc, "fuel-l1-revision",
                                  sgi_machine_get_fuel_l1_revision,
                                  sgi_machine_set_fuel_l1_revision);
    object_class_property_set_description(oc, "fuel-l1-revision",
        "L1 firmware revision as decimal major.minor.patch; applicability "
        "and requirements are declared by the selected model.");

    object_class_property_add(oc, "fuel-bedrock-revision", "uint8",
                              sgi_machine_get_fuel_bedrock_revision,
                              sgi_machine_set_fuel_bedrock_revision,
                              NULL, NULL);
    object_class_property_set_description(oc, "fuel-bedrock-revision",
        "Fitted Bedrock revision, 0 through 15; applicability and requirements "
        "are declared by the selected model.");

    object_class_property_add_str(oc, "fuel-mac-eeprom",
                                  sgi_machine_get_fuel_mac_eeprom,
                                  sgi_machine_set_fuel_mac_eeprom);
    object_class_property_set_description(oc, "fuel-mac-eeprom",
        "Path to a 512-byte Fuel L1 MAC EEPROM image; applicability and "
        "requirements are declared by the selected model");

    object_class_property_add_str(oc, "routes", sgi_machine_get_routes, NULL);
    object_class_property_set_description(oc, "routes",
        "The route each ordered pair of Hubs takes, as source>destination="
        "vector in the PROM manual's notation, or - when unreachable, "
        "asked of each Hub's own currently derived tables");

    for (size_t i = 0; i < ARRAY_SIZE(sgi_machine_identity_records); i++) {
        const SGIMachineIdentityRecords *records =
            &sgi_machine_identity_records[i];

        object_class_property_add(oc, records->name, "strList",
                                  sgi_machine_get_identity_records,
                                  sgi_machine_set_identity_records, NULL,
                                  (void *)records);
        object_class_property_set_description(oc, records->name,
                                              records->description);
    }
}

/* ---- Product classes ---- */

static const char * const sgi_sn0_r10000_cpu_types[] = {
    MIPS_CPU_TYPE_NAME("R10000"),
    NULL,
};

static void origin200_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelTopology *t = sgi_model_find_topology("origin200");

    g_assert(t);
    smc->product = "origin200";
    smc->default_topology = "origin200";
    smc->node_memory = &sn0_hub_memory;
    /* sn0-origin200-model.c's own values, real IP27 PROM fields. */
    smc->firmware_machine_type = 1;
    smc->flash_read_cycles = 15;
    smc->flash_write_cycles = 4;
    mc->desc = t->description;
    mc->valid_cpu_types = sgi_sn0_r10000_cpu_types;
    mc->default_cpus = sgi_machine_topology_default_cpus(t);
    mc->max_cpus = sgi_machine_product_max_cpus("origin200");
    mc->default_ram_size = sgi_machine_topology_default_ram(t);
    mc->default_ram_id = "sn0.node0.ram";
}

static void origin2000_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelTopology *t = sgi_model_find_topology("origin2000-rack");

    g_assert(t);
    smc->product = "origin2000";
    smc->default_topology = "origin2000-rack";
    smc->node_memory = &sn0_ip27_memory;
    mc->desc = t->description;
    mc->valid_cpu_types = sgi_sn0_r10000_cpu_types;
    mc->default_cpus = sgi_machine_topology_default_cpus(t);
    mc->max_cpus = sgi_machine_product_max_cpus("origin2000");
    mc->default_ram_size = sgi_machine_topology_default_ram(t);
    mc->default_ram_id = "sn0.ram";

    object_class_property_add_str(oc, "baseio-preset",
                                  sgi_machine_get_baseio_preset,
                                  sgi_machine_set_baseio_preset);
    object_class_property_set_description(oc, "baseio-preset",
        "Select the BaseIO card variant this product's I/O board builds: "
        "'io6' (the plain card, the default) or 'io6g' (adds the MediaIO "
        "daughtercard's second IOC3 and both chips' keyboard/mouse pair).");
}

/*
 * Onyx2 is Origin 2000 silicon: the same IP27 node boards, memory and
 * stock PROM, told apart only by the hardware its topologies fit (a
 * BaseIO-G and KONA pipes, and on the deskside a midplane whose node
 * slots drive different Hub slot bits). The rack is its default.
 */
static void onyx2_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelTopology *t = sgi_model_find_topology("onyx2-rack");

    g_assert(t);
    smc->product = "onyx2";
    smc->default_topology = "onyx2-rack";
    smc->node_memory = &sn0_ip27_memory;
    mc->desc = t->description;
    mc->valid_cpu_types = sgi_sn0_r10000_cpu_types;
    mc->default_cpus = sgi_machine_topology_default_cpus(t);
    mc->max_cpus = sgi_machine_product_max_cpus("onyx2");
    mc->default_ram_size = sgi_machine_topology_default_ram(t);
    mc->default_ram_id = "sn0.ram";
}

/*
 * A bare -M origin2000, with no -smp at all, never runs smp_parse: the
 * generic machine_initfn default of one socket survives unless this
 * preset corrects it before realize's own validation runs. Every other
 * product's default topology is one node, where one socket is already
 * right.
 */

static const TypeInfo sgi_machine_types[] = {
    {
        .name              = TYPE_SGI_SN_MACHINE,
        .parent            = TYPE_MACHINE,
        .abstract          = true,
        .instance_size     = sizeof(SGIMachineState),
        .instance_init     = sgi_machine_instance_init,
        .instance_finalize = sgi_machine_instance_finalize,
        .class_size        = sizeof(SGIMachineClass),
        .class_init        = sgi_machine_class_init,
        .interfaces        = (const InterfaceInfo[]) {
            { TYPE_SGI_XIO_SLOTS },
            { }
        },
    },
    {
        .name       = MACHINE_TYPE_NAME("origin200"),
        .parent     = TYPE_SGI_SN_MACHINE,
        .class_init = origin200_class_init,
    },
    {
        .name          = MACHINE_TYPE_NAME("origin2000"),
        .parent        = TYPE_SGI_SN_MACHINE,
        .class_init    = origin2000_class_init,
    },
    {
        .name          = MACHINE_TYPE_NAME("onyx2"),
        .parent        = TYPE_SGI_SN_MACHINE,
        .class_init    = onyx2_class_init,
    },
};

DEFINE_TYPES(sgi_machine_types)
