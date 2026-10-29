/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The abstract SGI Scalable Node family machine and its builder over an
 * SGIModelTopology from the compiled catalogue (qapi/sgi-machines.json,
 * embedded and parsed by hw/mips/sgi/sgi-machines.c). Origin 200 is the
 * first product on this base; see docs/system/mips/sgi/sn.rst.
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
#include "qemu/log.h"
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
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/mips/sgi/baseio.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/core/split-irq.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/misc/sgi-bic-board-interface.h"
#include "hw/misc/sgi-bedrock-flash.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/misc/sgi-l1.h"
#include "chardev/char.h"
#include "hw/char/sgi-ioc3-parallel.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "hw/misc/sgi-hub.h"
#include "hw/misc/sgi-kona.h"
#include "hw/misc/sgi-mgras.h"
#include "hw/misc/sgi-odyssey.h"
#include "hw/misc/sgi-router.h"
#include "hw/misc/sgi-sn1-router.h"
#include "hw/misc/sgi-sn0-module-management.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci-host/sgi-pic.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "hw/pci/pci.h"
#include "block/block.h"
#include "block/graph-lock.h"
#include "net/util.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/replay.h"
#include "trace.h"
#include "system/runstate.h"
#include "system/system.h"
#include "trace/control.h"
#include "cpu.h"
#include "hw/mips/sgi/sgi-machines.h"
#include "hw/mips/sgi/sgi-graphics.h"
#include "hw/core/hotplug.h"
#include "hw/mips/sgi/sgi-board-mac.h"
#include "hw/mips/sgi/sgi-storage.h"
#include "hw/mips/sgi/initial-flash.h"
#include "hw/mips/sgi/prom-image.h"
#include "hw/mips/sgi/sn-machine.h"
#include "hw/mips/sgi/sgi-synthetic-serial.h"
#include "hw/mips/sgi/sn1-node.h"
#include "hw/mips/sgi/net-cable.h"
#include "sn0.h"
#include "origin300-pci-expansion.h"

/*
 * The compiled catalogue's QAPI-generated types (qapi/sgi-machines.json) hold
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
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelPciHostBridge, pci_host_bridge)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelXioAttachment, xio_attachment)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelXioSlot, xio_slot)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelIoBoard, io_board)
SGI_MODEL_DEFINE_LIST_ARRAY(SGIModelPciFunction, pci_function)
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
    [SGI_INPUT_BOARD_ID] = "board-id-word",
    [SGI_INPUT_IOC3_SUBSYSTEM] = "ioc3-subsystem-id",
    [SGI_INPUT_L1_TYPE] = "l1-type-code",
    [SGI_INPUT_L1_REVISION] = "l1-revision",
    [SGI_INPUT_BEDROCK_REVISION] = "bedrock-revision",
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
    /* An Origin 3000 brick profile, which owns its type and revision. */
    bool has_profile;
    SGIModelL1Profile profile;
    uint8_t reported_type;
    int32_t revision[3];
    const char *mac_eeprom;
    bool has_cpu_context;
    uint32_t cpu_mode_word;
    uint64_t cpu_clock_hz;
    uint64_t hub_clock_hz;
} SGIMachineL1Config;

typedef struct SGIMachineIoConfig {
    uint16_t ioc3_subsystem_id;
    SGIModelMacSource mac_source;
    uint8_t pci_revision[SGI_MODEL_PCI_FUNCTION_KIND__MAX];
} SGIMachineIoConfig;

typedef struct SGIMachinePeerNotifier {
    Notifier notifier;
    SN1FabricEndpoint *endpoint;
    SN1MemoryMappings **memory_mappings;
} SGIMachinePeerNotifier;

/*
 * A cold start owed to one module, or to the whole machine
 * (sgi_machine_module_start()). The start itself runs from a bottom half that
 * migration does not carry, so the debt is what migrates.
 */
typedef struct SGIMachineModuleStart {
    SGIMachineState *machine;
    unsigned int module;
    bool pending;
} SGIMachineModuleStart;

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
     * processor=: the catalogue id of one of the topology's processor
     * options, the module every node fits. NULL fits the nodes' own part.
     */
    char *processor_name;
    /* The processor every node fits, resolved at realize. */
    const SGIModelProcessor *processor;
    /* Its option record, or NULL where the topology offers no choice. */
    const SGIModelProcessorOption *processor_option;
    /*
     * nodes=: how many node boards the chassis fits, in SGI's fill
     * order; 0 until set, which fits the topology's largest row.
     */
    uint32_t nodes_requested;
    /* The topology this machine builds: one row of its chassis. */
    const SGIModelTopology *view;
    char *baseio_preset;
    /* io-prom: an original I/O PROM image for the BASEIO flash parts. */
    char *io_prom;
    bool io_prom_used;
    /*
     * Catalogue storage item name -> the block node or drive its machine
     * property binds, for every item a property has been set for.
     */
    GHashTable *storage;

    unsigned int num_nodes;
    SN0NodeState **nodes;

    /* This machine's Routers, in descriptor order, so a cable's own
     * SGIModelLinkEnd index addresses them. */
    SGIRouterState **routers;
    SGISN1RouterState **sn1_routers;
    SN1MemoryMappings *sn1_memory_mappings;
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

    /*
     * Graphics pipes (docs/system/mips/sgi/sn.rst, Graphics pipes). Every pipe
     * is a -device the catalogue checks before it realizes, or the
     * topology's default pipe, created at init and realized by
     * sgi_machine_done() once no -device turned out to be a pipe.
     * x-uncatalogued-graphics skips the checks and the default, for
     * register-contract tests that use the machine to carry a board.
     */
    bool uncatalogued_graphics;
    SGIGraphicsPipes pipes;
    /*
     * The topology's own graphics boards (its KONA, direct Odyssey and
     * V-brick V12 records) are slots, not boards the builder fits: a pipe
     * claims one by its catalogue name and takes the record's link, port
     * and identity. pipe_record[i] marks io_boards[] entry i as one, with
     * the attachment it names; pipe_claimed[i] that a pipe holds it.
     * pipe_attachment marks those attachments, whose Crossbow ports start
     * empty.
     */
    bool *pipe_record;
    bool *pipe_claimed;
    const SGIModelXioAttachment **pipe_record_attachment;
    bool *pipe_attachment;
    /* The V-brick's own XBridge, which its V12 links to. */
    DeviceState *vbrick_bridge;

    bool debug_leds;
    bool junk_uart_present;
    bool ip53_experimental;
    bool origin300_l1cmd_compat;
    bool ip53_experiment_authorized;
    size_t ip53_widget_zero_host;
    bool ip53_widget_zero_host_valid;
    /*
     * A pipe in a graphics slot on the IP53 PIC fabric (widget-12 on the
     * quad Tezro and Onyx 350): its link to the PIC's PXBOW port, and the
     * widget, which sgi_machine_done() marks alive and routes once the
     * pipe has realized.
     */
    SGIXioLink pic_pipe_link;
    Object *pic_pipe;
    unsigned int pic_pipe_widget;
    uint8_t elsc_dip_switches;
    uint32_t module_start_delay_ms;

    /*
     * Each Origin 2000 module controller's power output, by controller
     * index, and whether the bottom half that decides if the machine powers
     * down is scheduled.
     */
    bool *module_powered;
    unsigned int module_power_count;
    bool module_power_bh_pending;
    /*
     * The power domains. A module is the unit that loses power or is
     * reset, so each event acts on the devices and processors of one
     * topology module (sgi_machine_module_start). node_module maps a node
     * to its module and controller_module a module controller to its own;
     * module_start says whether a cold start of the module is scheduled.
     */
    unsigned int module_count;
    /* module_count modules, then one start for the whole machine. */
    unsigned int start_count;
    unsigned int *node_module;
    unsigned int *controller_module;
    SGIMachineModuleStart *module_start;
    /* The machine's reset is under way: an AC cycle starts every module. */
    bool ac_cycle;
    VMChangeStateEntry *module_start_vm_state;
    bool module_starts_registered;
    /*
     * Each controller's module-management device, and the controller each
     * node's module belongs to (-1 for a node with none).
     */
    SN0ModuleManagementState **module_mgmt;
    int *node_controller;

    /*
     * The Bedrock nodes, for a topology whose node kind is Bedrock: one
     * per t->nodes entry, the way s->nodes is for a Hub topology. The
     * shipped Origin 300 carries one; a second is descriptor data the
     * builder accepts without exercising it (see sn-builder-test.c's
     * synthetic two-node fixture).
     */
    DeviceState **bedrock_nodes;
    DeviceState **pic_hosts;
    /*
     * The XBridge each crossbar record builds in its brick's module, in
     * t->crossbars order: an I/O brick's chip, reached over an XIO cable.
     */
    DeviceState **enclosure_xbridges;
    /* An IX-brick's or PX-brick's middle PIC, its enclosure crossbar. */
    DeviceState *enclosure_pic;
    SGIXioLink *pic_links;
    size_t pic_host_count;
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
    bool ip53_fingerprint_enabled;
    bool ip53_fingerprint_registered;
    bool ip53_fingerprint_finalized;
    uint8_t ip53_local_fingerprint[32];
    uint8_t ip53_incoming_fingerprint[32];
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
    /*
     * Family overrides of catalogue board values. Each one replaces the
     * catalogue value in every node, L1 or I/O board of the selected
     * topology that carries the field.
     */
    uint64_t board_id_override;
    bool board_id_override_set;
    uint16_t ioc3_subsystem_override;
    bool ioc3_subsystem_override_set;
    uint8_t l1_type_override;
    bool l1_type_override_set;
    int32_t l1_revision_override[3];
    bool l1_revision_override_set;
    uint8_t bedrock_revision_override;
    bool bedrock_revision_override_set;
    char *fuel_mac_eeprom;
    /* mac=: node 0's onboard Ethernet address; NULL selects the default. */
    char *mac;
    strList *controller_board_eeprom;
    strList *controller_chardev;
    strList *controller_brick_serial;
    strList *expansion_storage;
    const SGIModelIoAssembly *storage_assembly;
    BlockBackend *storage_backend;
};

/*
 * The controller holds one identification record per file, in order. The
 * machine names the files and claims nothing about what is in them; a file
 * replaces the synthetic record the L1 would otherwise build, and no record
 * is implied by the memory size.
 */
typedef struct SGIMachineIdentityRecords {
    const char *name;
    ptrdiff_t offset;
    const char *description;
    bool targeted;
} SGIMachineIdentityRecords;

static const SGIMachineIdentityRecords sgi_machine_identity_records[] = {
    {
        "expansion-storage", offsetof(SGIMachineState, expansion_storage),
        "Caller-backed expansion configuration storage as module-id=backend-id",
        true,
    },
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
    {
        "controller-brick-serial",
        offsetof(SGIMachineState, controller_brick_serial),
        "L1 brick serial as controller-index=serial (1 to 32 bytes)",
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

static void sgi_machine_set_module_serial(SGIMachineState *s,
    unsigned owner, DeviceState *l1)
{
    const strList *entry;

    for (entry = s->controller_brick_serial; entry; entry = entry->next) {
        unsigned selected;
        const char *payload = NULL;

        /* The machine validated owner=serial before constructing devices. */
        if (qemu_strtoui(entry->value, &payload, 10, &selected) ||
            !payload || *payload != '=') {
            error_report("invalid validated controller brick serial");
            continue;
        }
        payload++;
        if (selected == owner) {
            qdev_prop_set_string(l1, "brick-serial", payload);
        }
    }
    if (s->view && s->view->generation == SGI_MODEL_GENERATION_SN1 &&
        s->view->routers && !SGI_L1(l1)->brick_serial) {
        g_autofree char *serial = g_strdup_printf("model-module-%u", owner);

        qdev_prop_set_string(l1, "brick-serial", serial);
    }
}

/*
 * The onboard I/O board fitted to a Bedrock node: an IO8 or a Fuel system
 * board, each on the node's own XBridge.
 */
static bool sgi_machine_node_io_preset(const SGIModelTopology *t,
                                       unsigned int node_index,
                                       SGIModelIoBoardPreset *preset)
{
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    const SGIModelIoBoardList *entry;
    size_t count;

    attachments = sgi_model_xio_attachment_array(t->xio_attachments, &count);
    for (entry = t->io_boards; entry; entry = entry->next) {
        const SGIModelIoBoard *board = entry->value;

        if (!board->populated || board->attachment >= count ||
            (board->preset != SGI_MODEL_IO_BOARD_PRESET_IO8 &&
             board->preset != SGI_MODEL_IO_BOARD_PRESET_FUEL) ||
            attachments[board->attachment]->node_index != node_index ||
            attachments[board->attachment]->crossbar_index >= 0) {
            continue;
        }
        *preset = board->preset;
        return true;
    }
    return false;
}

/*
 * Whether the machine is an Origin 200: it fits the IP29 System Board's
 * own I/O, direct or through the GIGAchannel's Crossbow. Every Hub node of
 * such a machine is an IP29.
 */
static bool sgi_machine_topology_is_ip29(const SGIModelTopology *t)
{
    const SGIModelIoBoardList *entry;

    for (entry = t->io_boards; entry; entry = entry->next) {
        if (entry->value->populated &&
            (entry->value->preset == SGI_MODEL_IO_BOARD_PRESET_IP29_IO ||
             entry->value->preset ==
             SGI_MODEL_IO_BOARD_PRESET_IP29_IO_CROSSTOWN)) {
            return true;
        }
    }
    return false;
}

/*
 * The Origin 200 PIMM each processor population is, as real inventories
 * print its name, part and revision; the Hub's one-wire part carries its
 * record. The 180 MHz 1 MiB modules are the ones a published dual-module
 * Origin 200 inventory lists in both its modules: 013-1895-001 revision C
 * for one processor, 013-1896-001 revision D for two. The 180 MHz 2 MiB
 * single and 225 MHz dual are as published inventories print them; no
 * inventory gives the 180 MHz 2 MiB dual's or the R12000's record.
 */
static const struct {
    uint32_t processors;
    uint32_t clock_mhz;
    uint32_t cache_mib;
    const char *name;
    const char *part;
    const char *revision;
} sgi_ip29_pimms[] = {
    { 1, 180, 1, "PIMM_1XT5_1MB", "013-1895-001", "C" },
    { 2, 180, 1, "PIMM_2XT5_1MB", "013-1896-001", "D" },
    { 1, 180, 2, "PIMM_1XT5_2MB", "013-2285-002", "A" },
    { 2, 225, 2, "PIMM_225_2M_2P", "013-2382-002", "B" },
};

/*
 * Store an Origin 200 node's PIMM record in its Hub's one-wire part. The
 * barcode is marked synthetic: the processor-module letter and the
 * instance of the Hub part holding the record. A population no real PIMM
 * is known for leaves the part unprogrammed, which IRIX reports as missing
 * page data.
 */
static void sgi_machine_attach_ip29_pimm(SN0NodeState *node,
                                         const SGIModelProcessor *cpu,
                                         uint32_t processors,
                                         uint64_t part_serial)
{
    char barcode[SGI_SYNTHETIC_BARCODE_SIZE];

    if (!cpu) {
        return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(sgi_ip29_pimms); i++) {
        if (sgi_ip29_pimms[i].processors == processors &&
            sgi_ip29_pimms[i].clock_mhz * 1000000ULL == cpu->clock_hz &&
            sgi_ip29_pimms[i].cache_mib == cpu->cache_mib) {
            SGIBoardRecord record = {
                barcode, sgi_ip29_pimms[i].part, sgi_ip29_pimms[i].revision,
                sgi_ip29_pimms[i].name,
            };

            sgi_synthetic_barcode(barcode, 'U', part_serial);
            sgi_hub_attach_board_record(&node->hub, &record, &error_fatal);
            return;
        }
    }
}

/*
 * Store an Origin 2000 or Onyx2 node's record in its Hub's one-wire part.
 * An IP27 node lists only its node board there, which real inventories
 * print as IP27, 030-0733-003 revision G; its processors carry no record
 * of their own. The barcode is marked synthetic: the node-board letter and
 * the instance of the Hub part holding the record.
 */
static void sgi_machine_attach_ip27_node(SN0NodeState *node,
                                         uint64_t part_serial)
{
    char barcode[SGI_SYNTHETIC_BARCODE_SIZE];
    SGIBoardRecord record = { barcode, "030-0733-003", "G", "IP27" };

    sgi_synthetic_barcode(barcode, 'N', part_serial);
    sgi_hub_attach_board_record(&node->hub, &record, &error_fatal);
}

/*
 * The Origin 300 node board each processor count is, as real inventories
 * print its name, part and revision. Another four-processor part,
 * 030-1797-001 revision B, also appears in real inventories; the default
 * keeps the one the launcher used.
 */
static const struct {
    uint32_t processors;
    const char *name;
    const char *part;
    const char *revision;
} sgi_ip45_node_boards[] = {
    { 2, "IP45_2CPU", "030-1780-001", "-C" },
    { 4, "IP45_4CPU", "030-1779-001", "-C" },
};

static uint32_t sgi_machine_l1_node_processors(SGIMachineState *s,
                                               DeviceState *l1)
{
    for (unsigned int i = 0; s->bedrock_nodes && i < s->num_nodes; i++) {
        SN1NodeState *node = SN1_NODE(s->bedrock_nodes[i]);

        if (node->l1 == l1) {
            return node->nr_cpus;
        }
    }
    return 0;
}

/*
 * The Fuel system board as a real IRIX 6.5.22 inventory prints it at the
 * node and at both XBridge widgets: IP34, part 030-1707-003, rev -G. The
 * L1 keeps the printed dashes; the public Linux IP35 series says the
 * EEPROM stores underscores but reads either form, and IRIX printed
 * dashes.
 */
#define SGI_FUEL_BOARD_NAME "IP34"
#define SGI_FUEL_BOARD_PART "030-1707-003"
#define SGI_FUEL_BOARD_REVISION "-G"

/*
 * Gives a node's L1 what its synthetic identity records carry. The station
 * address goes in the IO8 board area on an Origin 300; on a Fuel it goes in
 * the system board's component 1 area and the MAC text record. Each further
 * node takes the next address, so two chassis' IOC3s differ. An Origin 300
 * node also gets its node board's identity for its processor count, and a
 * Fuel its one system board's. An IP53 runtime profile that names a node
 * board gives that board's identity to a node with its processor count. A
 * raw record supplied for an area still wins inside the L1.
 */
static bool sgi_machine_set_l1_defaults(SGIMachineState *s, DeviceState *l1,
                                        unsigned int node_index, Error **errp)
{
    const SGIModelIp53NodeBoard *ip53_board;
    SGIModelIoBoardPreset preset;
    uint32_t processors;
    g_autofree char *address = NULL;

    if (!s->view) {
        return true;
    }
    ip53_board = s->view->ip53_runtime ? s->view->ip53_runtime->node_board :
                 NULL;
    processors = sgi_machine_l1_node_processors(s, l1);
    if (ip53_board && processors == ip53_board->processors) {
        qdev_prop_set_string(l1, "node-board-name", ip53_board->name);
        qdev_prop_set_string(l1, "node-board-part", ip53_board->part);
        qdev_prop_set_string(l1, "node-board-revision", ip53_board->revision);
        return true;
    }
    if (!sgi_machine_node_io_preset(s->view, node_index, &preset)) {
        return true;
    }
    address = sgi_board_mac(s->mac, node_index, errp);
    if (!address) {
        return false;
    }
    qdev_prop_set_string(l1, "mac", address);
    /* Each node's board areas name their own instance, so serials differ. */
    qdev_prop_set_uint8(l1, "identity-index", node_index);
    if (preset != SGI_MODEL_IO_BOARD_PRESET_IO8) {
        qdev_prop_set_string(l1, "mac-record", "system-board");
        qdev_prop_set_string(l1, "node-board-name", SGI_FUEL_BOARD_NAME);
        qdev_prop_set_string(l1, "node-board-part", SGI_FUEL_BOARD_PART);
        qdev_prop_set_string(l1, "node-board-revision",
                             SGI_FUEL_BOARD_REVISION);
        return true;
    }
    qdev_prop_set_string(l1, "mac-record", "io8-board");
    for (size_t i = 0; i < ARRAY_SIZE(sgi_ip45_node_boards); i++) {
        if (sgi_ip45_node_boards[i].processors == processors) {
            qdev_prop_set_string(l1, "node-board-name",
                                 sgi_ip45_node_boards[i].name);
            qdev_prop_set_string(l1, "node-board-part",
                                 sgi_ip45_node_boards[i].part);
            qdev_prop_set_string(l1, "node-board-revision",
                                 sgi_ip45_node_boards[i].revision);
        }
    }
    return true;
}

/*
 * Copies the machine's own identity-record file lists onto an L1 device
 * before it is realized. Takes the L1 explicitly because it lives on the
 * node (sgi-sn1-node), while the file lists it reads are machine
 * properties. With several nodes each list splits evenly, node_index
 * taking its own share in order; a list that does not divide evenly is
 * refused, since no share of it names one chassis's parts.
 */
unsigned int sgi_machine_bedrock_node_index(MachineState *machine,
                                            DeviceState *node)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    unsigned int i;

    for (i = 0; s->bedrock_nodes && i < s->num_nodes; i++) {
        if (s->bedrock_nodes[i] == node) {
            return i;
        }
    }
    /*
     * A qtest fixture that builds its nodes itself has no list; its nodes
     * are placed at their NASIDs, as sgi-sn1-node assumed before.
     */
    return SN1_NODE(node)->nasid;
}

static bool sgi_machine_set_l1_cpu_context(SGIMachineState *s,
                                           DeviceState *l1,
                                           unsigned int node_index,
                                           Error **errp)
{
    static const char *const properties[] = {
        "cpu-context-present", "cpu-mode-word", "cpu-clock-hz",
        "hub-clock-hz",
    };
    g_autofree const SGIModelNode **nodes = NULL;
    g_autofree const SGIModelModule **modules = NULL;
    g_autofree const SGIModelController **controllers = NULL;
    const SGIModelL1CPUContext *context;
    size_t node_count, module_count, controller_count;
    const SGIModelNode *node;
    int32_t controller_index;

    for (size_t i = 0; i < ARRAY_SIZE(properties); i++) {
        const GlobalProperty *prop = qdev_find_global_prop(OBJECT(l1),
                                                            properties[i]);

        if (prop) {
            error_setg(errp, "machine-owned L1 CPU context property '%s' "
                       "cannot be overridden with -global %s.%s",
                       properties[i], prop->driver, prop->property);
            return false;
        }
    }
    if (!s->view || s->view->generation != SGI_MODEL_GENERATION_SN1) {
        return true;
    }
    nodes = sgi_model_node_array(s->view->nodes, &node_count);
    modules = sgi_model_module_array(s->view->modules, &module_count);
    controllers = sgi_model_controller_array(s->view->controllers,
                                              &controller_count);
    if (node_index >= node_count) {
        return true;
    }
    node = nodes[node_index];
    if (node->module_index >= module_count) {
        return true;
    }
    controller_index = modules[node->module_index]->controller_index;
    if (controller_index < 0 || controller_index >= controller_count ||
        !controllers[controller_index]->l1) {
        return true;
    }
    context = controllers[controller_index]->l1->cpu_context;
    if (context) {
        qdev_prop_set_bit(l1, "cpu-context-present", true);
        qdev_prop_set_uint32(l1, "cpu-mode-word", context->cpu_mode_word);
        qdev_prop_set_uint64(l1, "cpu-clock-hz", context->cpu_clock_hz);
        qdev_prop_set_uint64(l1, "hub-clock-hz", context->hub_clock_hz);
    }
    return true;
}

bool sgi_machine_set_l1_identity_records(MachineState *machine,
                                         DeviceState *l1,
                                         unsigned int node_index,
                                         Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    unsigned int nodes = MAX(s->num_nodes, 1);
    size_t i;

    if (s->view && s->view->generation == SGI_MODEL_GENERATION_SN1 &&
        s->view->routers) {
        g_autofree const SGIModelNode **model_nodes = NULL;
        g_autofree const SGIModelModule **modules = NULL;
        size_t node_count, module_count;

        model_nodes = sgi_model_node_array(s->view->nodes, &node_count);
        modules = sgi_model_module_array(s->view->modules, &module_count);
        if (node_index < node_count &&
            model_nodes[node_index]->module_index < module_count) {
            sgi_machine_set_module_serial(s,
                modules[model_nodes[node_index]->module_index]->
                    controller_index,
                l1);
        }
    }
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
    if (!sgi_machine_set_l1_cpu_context(s, l1, node_index, errp)) {
        return false;
    }
    return sgi_machine_set_l1_defaults(s, l1, node_index, errp);
}

static bool sgi_machine_controller_input(const char *text, bool component,
    const char *format,
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
               format);
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
    g_autofree bool *serials = g_new0(bool, MAX(count, 1));
    const strList *entry, *prior;

    for (unsigned kind = 0; kind < 3; kind++) {
        const strList *heads[] = { s->controller_board_eeprom,
                                  s->controller_chardev,
                                  s->controller_brick_serial };
        const char *formats[] = { "owner/component=path", "owner=chardev-id",
                                  "owner=serial" };
        const strList *head = heads[kind];

        for (entry = head; entry; entry = entry->next) {
            unsigned owner, slot = 0;
            const char *payload;
            bool *used;

            if (!sgi_machine_controller_input(entry->value, !kind,
                                               formats[kind], &owner,
                                               &slot, &payload, errp)) {
                return false;
            }
            if (owner >= count ||
                (!sgi_machine_expansion_controller(t, owner) &&
                 !(kind == 2 && t->generation == SGI_MODEL_GENERATION_SN1 &&
                   t->routers))) {
                error_setg(errp, "controller %u is not an expansion L1 owner",
                           owner);
                return false;
            }
            used = kind == 2 ? &serials[owner] :
                   kind == 1 ? &consoles[owner] : &boards[owner * 4 + slot];
            if (*used) {
                error_setg(errp, "duplicate controller input for owner %u",
                           owner);
                return false;
            }
            *used = true;
            if (kind == 2) {
                if (strlen(payload) > 32) {
                    error_setg(errp,
                               "controller brick serial exceeds 32 bytes");
                    return false;
                }
                continue;
            }
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

/*
 * The expansion module's configuration storage bound as catalogue storage
 * item @item, the block node @node: the same writable raw image the
 * deprecated expansion-storage=<module>=<drive> binding names, given a
 * BlockBackend of the machine's own for the module to attach.
 */
static bool sgi_machine_bind_expansion_storage_item(
    SGIMachineState *s, const SGIModelIoAssembly *selected, const char *item,
    const char *node, Error **errp)
{
    BlockDriverState *bs = bdrv_lookup_bs(node, node, errp);
    BlockBackend *backend;

    if (!bs) {
        return false;
    }
    if (strcmp(bdrv_get_format_name(bs), "raw") || bdrv_is_read_only(bs)) {
        error_setg(errp, "%s needs a writable raw block node", item);
        return false;
    }
    backend = blk_new(qemu_get_aio_context(), 0, BLK_PERM_ALL);
    if (blk_insert_bs(backend, bs, errp) < 0) {
        blk_unref(backend);
        return false;
    }
    s->storage_assembly = selected;
    s->storage_backend = backend;
    return true;
}

static bool sgi_machine_validate_expansion_storage(SGIMachineState *s,
    const SGIModelTopology *t, Error **errp)
{
    g_autofree const SGIModelModule **modules = NULL;
    const SGIModelIoAssemblyList *entry;
    const SGIModelIoAssembly *selected = NULL;
    g_autofree char *binding = NULL;
    char *backend_id;
    size_t module_count;
    BlockBackend *backend;
    BlockDriverState *backing;

    for (entry = t->io_assemblies; entry; entry = entry->next) {
        if (entry->value->has_configuration_storage) {
            if (selected) {
                error_setg(errp,
                           "configuration storage supports one selected owner");
                return false;
            }
            selected = entry->value;
        }
    }
    if (!selected) {
        if (s->expansion_storage) {
            error_setg(errp,
                       "configuration storage binding requires selection");
            return false;
        }
        return true;
    }
    modules = sgi_model_module_array(t->modules, &module_count);
    if (selected->module_index < module_count) {
        g_autofree char *item = g_strdup_printf(
            "%s-flash", modules[selected->module_index]->id);
        const char *node = sgi_storage_node(s->storage, item);

        if (node && s->expansion_storage) {
            error_setg(errp, "%s and expansion-storage both back the "
                       "expansion configuration storage", item);
            return false;
        }
        if (node) {
            return sgi_machine_bind_expansion_storage_item(s, selected, item,
                                                           node, errp);
        }
    }
    if (!s->expansion_storage) {
        error_setg(errp,
                   "configuration storage selection requires one image binding");
        return false;
    }
    if (s->expansion_storage->next) {
        error_setg(errp, "configuration storage accepts one image binding");
        return false;
    }
    binding = g_strdup(s->expansion_storage->value);
    backend_id = strchr(binding, '=');
    if (!backend_id || backend_id == binding || !backend_id[1] ||
        strchr(backend_id + 1, '=')) {
        error_setg(errp, "invalid configuration storage binding; "
                   "expected module-id=backend-id");
        return false;
    }
    *backend_id++ = 0;
    if (selected->module_index >= module_count ||
        strcmp(modules[selected->module_index]->id, binding)) {
        error_setg(errp,
                   "configuration storage binding names an unselected module '%s'",
                   binding);
        return false;
    }
    backend = blk_by_name(backend_id);
    backing = backend ? blk_bs(backend) : NULL;
    if (!backing) {
        error_setg(errp,
                   "configuration storage backend '%s' does not exist",
                   backend_id);
        return false;
    }
    if (strcmp(bdrv_get_format_name(backing), "raw") ||
        blk_get_full_raw_file_length(backend) != SGI_BEDROCK_FLASH_SIZE ||
        bdrv_is_read_only(backing)) {
        error_setg(errp, "expansion configuration storage needs a writable "
                   "full regular raw-to-file image of exactly 16 MiB");
        return false;
    }
    if (blk_get_attached_dev(backend)) {
        error_setg(errp,
                   "expansion configuration storage backend is already in use");
        return false;
    }
    s->storage_assembly = selected;
    s->storage_backend = backend;
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

        if (!sgi_machine_controller_input(entry->value, true,
                                           "owner/component=path", &selected,
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

        if (!sgi_machine_controller_input(entry->value, false,
                                           "owner=chardev-id", &selected,
                                           &slot, &payload, errp)) {
            return false;
        }
        if (selected == owner &&
            !object_property_set_str(OBJECT(l1), "chardev", payload, errp)) {
            return false;
        }
    }
    sgi_machine_set_module_serial(s, owner, l1);
    return true;
}

/* ---- Topology resolution and the CPU-population contract ---- */

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

    if (smc->test_topology) {
        return smc->test_topology;
    }

    name = s->topology_name ? s->topology_name : smc->default_topology;
    if (!name || !smc->product) {
        return NULL;
    }
    return sgi_model_product_topology(sgi_machines(), smc->product, name);
}

static bool sgi_machine_preflight_io_descriptors(
    const SGIModelTopology *t, bool allow_ip53_fixture,
    bool allow_ip53_experiment, bool tezro, Error **errp);

/*
 * The profile a PIC host must select: the IP53 runtime profile's own once
 * that runtime is authorized, which only a topology carrying one can be.
 */
static SGIModelPicCompatibilityProfile sgi_machine_ip53_pic_profile(
    const SGIModelTopology *t, bool runtime_experiment)
{
    if (runtime_experiment) {
        g_assert(t->ip53_runtime);
        return t->ip53_runtime->pic_profile;
    }
    return SGI_MODEL_PIC_COMPATIBILITY_PROFILE_PIC_REV3_BEDROCK;
}

static bool sgi_machine_ip53_experiment_policy(
    SGIMachineState *s, SGIMachineClass *smc, const SGIModelTopology *t,
    bool *allow_ip53_experiment, Error **errp)
{
    bool onyx350 = g_strcmp0(smc->product, "onyx350") == 0;
    bool tezro = g_strcmp0(smc->product, "tezro") == 0;
    /*
     * The catalogue marks which topologies are IP53 runtime profiles and
     * which of those admit a normal launch; topology= is product-scoped, so
     * the topology found is already this product's own.
     */
    bool registered_ip53_product = t->ip53_runtime != NULL;
    bool normal_candidate = registered_ip53_product &&
                            t->ip53_runtime->normal_selection != NULL;

    if (onyx350 && !s->topology_name) {
        error_setg(errp, "Onyx 350 runtime trial requires explicit "
                   "topology=onyx350-compat selection");
        return false;
    }
    if (tezro && !s->topology_name) {
        error_setg(errp, "Tezro runtime profile requires explicit topology selection");
        return false;
    }

    if (smc->test_topology && qtest_enabled()) {
        if (s->ip53_experimental) {
            error_setg(errp, "qtest fixture topology cannot enable the "
                       "IP53 runtime experiment");
            return false;
        }
        *allow_ip53_experiment = false;
        return true;
    }
    if (s->ip53_experimental &&
        (!registered_ip53_product || smc->test_topology)) {
        error_setg(errp, "ip53-experimental=on requires a registered IP53 "
                   "runtime profile");
        return false;
    }
    if (registered_ip53_product && !s->ip53_experimental &&
        !normal_candidate) {
        error_setg(errp, "%s requires ip53-experimental=on", smc->product);
        return false;
    }
    *allow_ip53_experiment = s->ip53_experimental || normal_candidate;
    return true;
}

/*
 * The normal-selection exception is deliberately a complete resolved tuple.
 * Keep the early product/topology admission above broad enough to resolve the
 * descriptor, then authorize its IP53 behavior only after population and
 * memory selection have been checked.
 */
static bool sgi_machine_ip53_normal_tuple(
    const SGIModelTopology *t, MachineState *machine,
    const unsigned int *requested, unsigned int node_count,
    const SGIModelNode **nodes, const SGIModelProcessor *processor,
    const SGIModelProcessorOption *processor_option)
{
    const SGIModelIp53NormalSelection *profile =
        t->ip53_runtime ? t->ip53_runtime->normal_selection : NULL;
    const SGIModelCpuSlotList *slot;
    unsigned int fitted_count = 0;

    if (!profile || node_count != 1 ||
        machine->smp.cpus != machine->smp.max_cpus ||
        !processor ||
        (processor_option && processor_option->assembly != processor)) {
        return false;
    }
    if (machine->smp.cpus != profile->cpus ||
        requested[0] != profile->cpus ||
        machine->ram_size != profile->memory_mib * MiB ||
        processor->clock_hz != profile->clock_hz ||
        processor->cache_mib != profile->cache_mib ||
        !processor->has_mode_bits ||
        processor->mode_bits != profile->mode_word ||
        g_strcmp0(processor->kind, "R16000")) {
        return false;
    }
    for (slot = nodes[0]->cpu_slots; slot; slot = slot->next) {
        const SGIModelProcessor *assembly = slot->value->assembly;

        if (!assembly) {
            continue;
        }
        if (g_strcmp0(assembly->kind, "R16000") ||
            assembly->clock_hz != profile->clock_hz ||
            assembly->cache_mib != profile->cache_mib ||
            !assembly->has_mode_bits ||
            assembly->mode_bits != profile->mode_word) {
            return false;
        }
        fitted_count++;
    }
    return fitted_count == profile->fitted_cpus;
}

static bool sgi_machine_controller_module_owned(const SGIModelController *c)
{
    return c->has_owner && c->owner == SGI_MODEL_CONTROLLER_OWNER_MODULE;
}

/* A module-owned L1 with the Cx-brick profile: one L1 for its boards. */
static bool sgi_machine_controller_cx(const SGIModelController *c)
{
    return c->has_profile &&
           c->profile == SGI_MODEL_MODULE_L1_PROFILE_CX_BRICK;
}

static bool sgi_machine_node_is_memory(const SGIModelNode *node)
{
    return node->has_role && node->role == SGI_MODEL_NODE_ROLE_MEMORY;
}

/*
 * A memory node is a Bedrock with banks and no processor. The console
 * node runs the PROM, so it cannot be one.
 */
static bool sgi_machine_validate_memory_node(const SGIModelTopology *t,
                                             unsigned int index,
                                             const SGIModelNode *n,
                                             Error **errp)
{
    const uint64List *bank;
    bool banked = false;

    if (n->asic_kind != SGI_MODEL_ASIC_KIND_BEDROCK) {
        error_setg(errp, "topology '%s' memory node '%s' is not a Bedrock "
                   "node", t->name, n->id);
        return false;
    }
    if ((int64_t)index == t->console) {
        error_setg(errp, "topology '%s' memory node '%s' cannot be the "
                   "console node", t->name, n->id);
        return false;
    }
    for (const SGIModelCpuSlotList *slot = n->cpu_slots; slot;
         slot = slot->next) {
        if (slot->value->assembly) {
            error_setg(errp, "topology '%s' memory node '%s' has a fitted "
                       "processor", t->name, n->id);
            return false;
        }
    }
    for (bank = n->bank_mib; bank; bank = bank->next) {
        banked |= bank->value != 0;
    }
    if (!banked) {
        error_setg(errp, "topology '%s' memory node '%s' has no populated "
                   "memory bank", t->name, n->id);
        return false;
    }
    return true;
}

/*
 * A Cx-brick's L1 serves one to four IP53 node boards, each on the link
 * its slot names, so the slots must be distinct and 0 to 3.
 */
static bool sgi_machine_validate_cx_l1(const SGIModelTopology *t,
                                       const SGIModelController *c,
                                       const SGIModelModule *m, Error **errp)
{
    g_autofree const SGIModelNode **nodes = NULL;
    const uint32List *entry;
    size_t node_count;
    unsigned int seen = 0;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    if (QAPI_LIST_LENGTH(m->nodes) > SGI_L1_CX_BOARDS) {
        error_setg(errp, "topology '%s' controller '%s' is a Cx-brick L1 for "
                   "module '%s', which lists %zu nodes; it serves one to "
                   "four", t->name, c->id, m->id,
                   (size_t)QAPI_LIST_LENGTH(m->nodes));
        return false;
    }
    for (entry = m->nodes; entry; entry = entry->next) {
        const SGIModelNode *n = entry->value < node_count ?
                                nodes[entry->value] : NULL;

        if (!n || !n->has_board_kind ||
            (n->board_kind != SGI_MODEL_BOARD_KIND_IP53 &&
             n->board_kind != SGI_MODEL_BOARD_KIND_IP53_CX)) {
            error_setg(errp, "topology '%s' controller '%s' is a Cx-brick "
                       "L1; module '%s' lists a node that is not an IP53 "
                       "board", t->name, c->id, m->id);
            return false;
        }
        if (n->slot >= SGI_L1_CX_BOARDS || (seen & (1u << n->slot))) {
            error_setg(errp, "topology '%s' Cx-brick node '%s' has slot %u; "
                       "each board takes a distinct slot 0 to 3", t->name,
                       n->id, n->slot);
            return false;
        }
        seen |= 1u << n->slot;
    }
    return true;
}

/*
 * Refuse catalogue records this builder cannot build yet, before anything
 * is created: a node with no boot flash, a module-owned L1 shared by
 * several nodes or by none, and a module's catalogued L1 brick type.
 * Building any of them as the nearest existing shape would show the
 * guest hardware the topology does not describe.
 */
static bool sgi_machine_refuse_unbuilt_records(const SGIModelTopology *t,
                                               Error **errp)
{
    const SGIModelNodeList *node;
    const SGIModelModuleList *module;
    const SGIModelControllerList *controller;
    g_autofree const SGIModelModule **modules = NULL;
    size_t module_count;
    unsigned int index = 0;

    modules = sgi_model_module_array(t->modules, &module_count);
    for (node = t->nodes; node; node = node->next, index++) {
        const SGIModelNode *n = node->value;

        if (sgi_machine_node_is_memory(n) &&
            !sgi_machine_validate_memory_node(t, index, n, errp)) {
            return false;
        }
        /*
         * Whether a memory-only board carries boot flash is unknown, so
         * every node still needs its own.
         */
        if (!n->flash) {
            error_setg(errp, "topology '%s' node '%s' has no boot flash",
                       t->name, n->id);
            return false;
        }
    }
    for (module = t->modules; module; module = module->next) {
        const SGIModelModule *m = module->value;
        const SGIModelControllerList *owner = t->controllers;

        for (int64_t i = 0; owner && i < m->controller_index; i++) {
            owner = owner->next;
        }
        /*
         * A C-brick's type letter is built by the C-brick L1 profile,
         * which answers it; any other catalogued letter has no profile.
         */
        if (m->brick_type &&
            !(!strcmp(m->brick_type, "C") && m->controller_index >= 0 &&
              owner && owner->value->l1 && owner->value->l1->has_profile &&
              owner->value->l1->profile == SGI_MODEL_L1_PROFILE_CBRICK)) {
            error_setg(errp, "topology '%s' module '%s' names L1 brick type "
                       "'%s'; brick types from the catalogue are not built "
                       "yet", t->name, m->id, m->brick_type);
            return false;
        }
    }
    for (controller = t->controllers; controller;
         controller = controller->next) {
        const SGIModelController *c = controller->value;
        const SGIModelModule *m;
        size_t nodes;

        if (sgi_machine_controller_cx(c) &&
            !sgi_machine_controller_module_owned(c)) {
            error_setg(errp, "topology '%s' controller '%s' has the Cx-brick "
                       "L1 profile but is not owned by its module", t->name,
                       c->id);
            return false;
        }
        if (!sgi_machine_controller_module_owned(c)) {
            continue;
        }
        m = c->module_index < module_count ? modules[c->module_index] : NULL;
        if (t->generation != SGI_MODEL_GENERATION_SN1 ||
            c->kind != SGI_MODEL_CONTROLLER_KIND_L1 || !m) {
            error_setg(errp, "topology '%s' controller '%s' is owned by its "
                       "module, which only an SN1 module's L1 builds",
                       t->name, c->id);
            return false;
        }
        nodes = QAPI_LIST_LENGTH(m->nodes);
        if (!nodes) {
            error_setg(errp, "topology '%s' controller '%s' is owned by "
                       "module '%s', which lists no node to link it into",
                       t->name, c->id, m->id);
            return false;
        }
        if (sgi_machine_controller_cx(c)) {
            if (!sgi_machine_validate_cx_l1(t, c, m, errp)) {
                return false;
            }
            continue;
        }
        /*
         * Only the Cx-brick profile says how several Bedrocks reach one
         * L1; for any other module it is unknown.
         */
        if (nodes > 1) {
            error_setg(errp, "topology '%s' controller '%s' is owned by "
                       "module '%s', which lists %zu nodes; no reviewed L1 "
                       "attachment for several nodes", t->name, c->id, m->id,
                       nodes);
            return false;
        }
    }
    return true;
}

/*
 * Tell apart the two readings of a module with no controller record. -1
 * with no controller field keeps its Origin 200 meaning, each node
 * carrying its own controller, which only the SN0 builder reads; every
 * SN1 node's L1 is registered under its module's controller. A module
 * marked absent has no controller at all, which only the SN1 builder
 * builds, and only for a module with no node or I/O peer to answer for.
 */
static bool sgi_machine_validate_module_controllers(const SGIModelTopology *t,
                                                    Error **errp)
{
    const SGIModelModuleList *entry;

    for (entry = t->modules; entry; entry = entry->next) {
        const SGIModelModule *m = entry->value;

        if (m->has_controller) {
            if (t->generation != SGI_MODEL_GENERATION_SN1) {
                error_setg(errp, "topology '%s' module '%s' has controller: "
                           "absent, which only an SN1 topology builds",
                           t->name, m->id);
                return false;
            }
            if (m->nodes || m->io_peers) {
                error_setg(errp, "topology '%s' module '%s' has controller: "
                           "absent but lists a node or I/O peer", t->name,
                           m->id);
                return false;
            }
            if (m->controller_index != -1) {
                error_setg(errp, "topology '%s' module '%s' has controller: "
                           "absent but controller index %" PRId64, t->name,
                           m->id, m->controller_index);
                return false;
            }
        } else if (t->generation == SGI_MODEL_GENERATION_SN1 &&
                   m->controller_index < 0) {
            error_setg(errp, "topology '%s' module '%s' has controller index "
                       "-1 and no controller record; an SN1 module has a "
                       "controller or is marked absent", t->name, m->id);
            return false;
        }
    }
    return true;
}

/*
 * A router that names node ports takes a node's cable only on one of
 * them. On an Origin 3000 R-brick that is ports 2 to 5, the only ones
 * whose cables carry the USB signals that reach a C-brick's L1
 * (007-4311-002 printed page 36). A cable to another router may use any
 * port. Indices and port ranges were checked with the runtime endpoints.
 */
static bool sgi_machine_router_takes_node(const SGIModelRouter *router,
                                          unsigned int port)
{
    const uint8List *entry;

    if (!router->node_ports) {
        return true;
    }
    for (entry = router->node_ports; entry; entry = entry->next) {
        if (entry->value == port) {
            return true;
        }
    }
    return false;
}

static bool sgi_machine_validate_router_ports(const SGIModelTopology *t,
                                              Error **errp)
{
    g_autofree const SGIModelNode **nodes = NULL;
    g_autofree const SGIModelRouter **routers = NULL;
    const SGIModelCableList *entry;
    const uint8List *port;
    size_t node_count, router_count, i;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    routers = sgi_model_router_array(t->routers, &router_count);
    for (i = 0; i < router_count; i++) {
        for (port = routers[i]->node_ports; port; port = port->next) {
            if (port->value < 1 || port->value > SGI_SN1_ROUTER_PORTS) {
                error_setg(errp, "router '%s' names node port %u, which it "
                           "does not have", routers[i]->id, port->value);
                return false;
            }
        }
    }
    for (entry = t->cables; entry; entry = entry->next) {
        const SGIModelLinkEnd *ends[2];

        if (QAPI_LIST_LENGTH(entry->value->ends) != 2) {
            continue;
        }
        ends[0] = entry->value->ends->value;
        ends[1] = entry->value->ends->next->value;
        for (unsigned int side = 0; side < 2; side++) {
            const SGIModelLinkEnd *router = ends[side];
            const SGIModelLinkEnd *node = ends[side ^ 1];
            g_autoptr(GString) allowed = NULL;

            if (router->kind != SGI_MODEL_LINK_END_KIND_ROUTER ||
                node->kind != SGI_MODEL_LINK_END_KIND_NODE ||
                router->index >= router_count || node->index >= node_count ||
                sgi_machine_router_takes_node(routers[router->index],
                                              router->port)) {
                continue;
            }
            allowed = g_string_new(NULL);
            for (port = routers[router->index]->node_ports; port;
                 port = port->next) {
                g_string_append_printf(allowed, "%s%u",
                                       allowed->len ? ", " : "", port->value);
            }
            error_setg(errp, "node '%s' is cabled to router '%s' port %u; "
                       "it takes a node only on ports %s",
                       nodes[node->index]->id, routers[router->index]->id,
                       router->port, allowed->str);
            return false;
        }
    }
    return true;
}

static bool sgi_machine_node_is_ip35(const SGIModelNode *node)
{
    return node->has_board_kind &&
           node->board_kind == SGI_MODEL_BOARD_KIND_IP35;
}

/* A Cx-brick's IP53 node board: cabled I/O like an IP35 board's. */
static bool sgi_machine_node_is_cx(const SGIModelNode *node)
{
    return node->has_board_kind &&
           node->board_kind == SGI_MODEL_BOARD_KIND_IP53_CX;
}

/* A board whose Bedrock has no XBridge of its own and takes an XIO cable. */
static bool sgi_machine_node_is_cabled(const SGIModelNode *node)
{
    return sgi_machine_node_is_ip35(node) || sgi_machine_node_is_cx(node);
}

/* The node an enclosure crossbar's XIO cable comes from, and its port. */
static bool sgi_machine_crossbar_host(const SGIModelTopology *t,
                                      unsigned int crossbar,
                                      unsigned int *node, uint8_t *port)
{
    const SGIModelXioCableList *entry;

    for (entry = t->xio_cables; entry; entry = entry->next) {
        const SGIModelXioEnd *ends[2] = {
            entry->value->ends->value,
            entry->value->ends->next->value,
        };

        for (unsigned int side = 0; side < 2; side++) {
            if (ends[side]->kind == SGI_MODEL_XIO_END_KIND_CROSSBAR &&
                ends[side]->index == crossbar &&
                ends[side ^ 1]->kind == SGI_MODEL_XIO_END_KIND_NODE) {
                *node = ends[side ^ 1]->index;
                *port = ends[side]->port;
                return true;
            }
        }
    }
    return false;
}

/* An XBridge's link ports: Crosstalk 8, 9, C and D, Crosstown2 A and B. */
#define SGI_MACHINE_XIO_LINK_FIRST 0x8
#define SGI_MACHINE_XIO_LINK_LAST  0xd

/*
 * The presets a crossbar-to-board cable can join: XIO cards that build
 * their own widget and take its link from sgi_machine_attach_xio_card(),
 * the V-brick's VPro V12 and the InfiniteReality pipe of a G-brick, pipe
 * records that a pipe claims after machine init
 * (sgi_machine_claim_pipe_record()). The Onyx 3000 owner's guide
 * (007-4264-002, printed pages 52 and 54) puts the V12 boards behind the
 * V-brick's own midplane XBridge, and a G-brick pipe reaches a C-brick
 * only through an I-, X- or N-brick (printed pages 8 and 62). The other
 * pipes and the Origin 2000 BaseIO cards, which bind the console's
 * serial lines and flash, are not built at an enclosure crossbar port
 * until a brick contract needs them.
 */
static bool sgi_machine_enclosure_board_preset(SGIModelIoBoardPreset preset)
{
    switch (preset) {
    case SGI_MODEL_IO_BOARD_PRESET_PCI_CARRIER:
    case SGI_MODEL_IO_BOARD_PRESET_PCI_SHOEHORN:
    case SGI_MODEL_IO_BOARD_PRESET_MENET:
    case SGI_MODEL_IO_BOARD_PRESET_MSCSI:
    case SGI_MODEL_IO_BOARD_PRESET_PIC:
    case SGI_MODEL_IO_BOARD_PRESET_ODYSSEY:
    case SGI_MODEL_IO_BOARD_PRESET_KONA:
        return true;
    default:
        return false;
    }
}

/*
 * One cable from crossbar port @port_end to board @board_end: a link
 * port, at most one cable per board, at the place the board's own
 * attachment names, and a preset the cable loop can build.
 */
static bool sgi_machine_validate_board_cable(
    const SGIModelTopology *t, const SGIModelXioCable *cable,
    const SGIModelXioEnd *board_end, const SGIModelXioEnd *port_end,
    const SGIModelCrossbar **crossbars,
    const SGIModelXioAttachment **attachments, size_t attachment_count,
    const SGIModelIoBoard **boards, bool *board_cabled, Error **errp)
{
    const SGIModelIoBoard *board = boards[board_end->index];
    const SGIModelCrossbar *crossbar = crossbars[port_end->index];
    const SGIModelXioAttachment *a = NULL;
    bool pic_crossbar = crossbar->kind == SGI_MODEL_CROSSBAR_KIND_PIC;

    /*
     * An IX-brick or PX-brick builds single attachment: its middle PIC's
     * links to the outer PICs, 0xc and 0xd, are the only board ports.
     */
    if (pic_crossbar && port_end->port != 0xc && port_end->port != 0xd) {
        error_setg(errp, "topology '%s' XIO cable '%s' meets PIC crossbar "
                   "'%s' at port 0x%x; its outer PICs are on ports 0xc and "
                   "0xd, and no other port has a link", t->name, cable->name,
                   crossbar->id, port_end->port);
        return false;
    }
    if (port_end->port < SGI_MACHINE_XIO_LINK_FIRST ||
        port_end->port > SGI_MACHINE_XIO_LINK_LAST) {
        error_setg(errp, "topology '%s' XIO cable '%s' meets crossbar '%s' "
                   "at port 0x%x; a board's cable meets a link port, 0x%x "
                   "to 0x%x", t->name, cable->name, crossbar->id,
                   port_end->port, SGI_MACHINE_XIO_LINK_FIRST,
                   SGI_MACHINE_XIO_LINK_LAST);
        return false;
    }
    if (board_cabled[board_end->index]) {
        error_setg(errp, "topology '%s' I/O board '%s' carries more than one "
                   "XIO cable", t->name, board->id);
        return false;
    }
    board_cabled[board_end->index] = true;
    if (board->has_attachment && board->attachment < attachment_count) {
        a = attachments[board->attachment];
    }
    if (!a || a->crossbar_index != (int64_t)port_end->index ||
        a->widget != port_end->port) {
        error_setg(errp, "topology '%s' XIO cable '%s' meets I/O board '%s' "
                   "at crossbar '%s' port 0x%x, but the board's XIO "
                   "attachment is not there", t->name, cable->name,
                   board->id, crossbar->id, port_end->port);
        return false;
    }
    if (board->populated &&
        (!sgi_machine_enclosure_board_preset(board->preset) ||
         pic_crossbar != (board->preset == SGI_MODEL_IO_BOARD_PRESET_PIC))) {
        error_setg(errp, "topology '%s' I/O board '%s' is cabled to crossbar "
                   "'%s'; preset '%s' is not built on an enclosure crossbar "
                   "yet", t->name, board->id, crossbar->id,
                   SGIModelIoBoardPreset_str(board->preset));
        return false;
    }
    /* A pipe -device names the slot it claims by the record's label. */
    if (board->populated && board->preset == SGI_MODEL_IO_BOARD_PRESET_KONA &&
        !board->label) {
        error_setg(errp, "topology '%s' pipe '%s' on crossbar '%s' has no "
                   "slot label for a pipe to claim", t->name, board->id,
                   crossbar->id);
        return false;
    }
    return true;
}

/*
 * Check the XIO cables before any device exists. A cable joins an IP35
 * node's II to port A or B of an XBridge in another brick; that node
 * hosts the chip, and every attachment on the chip names it. A cable
 * from another of the chip's link ports reaches one XIO board, whose own
 * attachment sits at that port. Ranges and end counts were checked with
 * the runtime endpoints. The checks also refuse what this builder cannot
 * build honestly yet: a second host node on one XBridge, since whether
 * one chip serves hosts on A and B at once is unknown; a host cabled to
 * a router, which no Origin 3000 rule covers; and a board whose preset
 * this builder cannot join to a crossbar port
 * (sgi_machine_enclosure_board_preset()). A cable
 * between two crossbars is checked like any other, then refused when the
 * cables are built (sgi_machine_build_board_cables()).
 */
static bool sgi_machine_validate_xio_cables(const SGIModelTopology *t,
                                            Error **errp)
{
    g_autofree const SGIModelNode **nodes = NULL;
    g_autofree const SGIModelCrossbar **crossbars = NULL;
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    g_autofree const SGIModelIoBoard **boards = NULL;
    g_autofree bool *node_cabled = NULL;
    g_autofree bool *port_cabled = NULL;
    g_autofree bool *chained = NULL;
    g_autofree int64_t *host = NULL;
    g_autofree bool *board_cabled = NULL;
    bool ip53 = false;
    const SGIModelXioCableList *entry;
    size_t node_count, crossbar_count, attachment_count, board_count, i;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    crossbars = sgi_model_crossbar_array(t->crossbars, &crossbar_count);
    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                 &attachment_count);
    boards = sgi_model_io_board_array(t->io_boards, &board_count);
    if (t->generation != SGI_MODEL_GENERATION_SN1) {
        if (t->xio_cables) {
            error_setg(errp, "topology '%s' has XIO cables, which only an "
                       "SN1 topology builds", t->name);
            return false;
        }
        return true;
    }
    node_cabled = g_new0(bool, MAX(node_count, 1));
    port_cabled = g_new0(bool, MAX(crossbar_count, 1) * SGI_XIO_WIDGETS);
    chained = g_new0(bool, MAX(crossbar_count, 1));
    board_cabled = g_new0(bool, MAX(board_count, 1));
    host = g_new(int64_t, MAX(crossbar_count, 1));
    for (i = 0; i < MAX(crossbar_count, 1); i++) {
        host[i] = -1;
    }

    for (entry = t->xio_cables; entry; entry = entry->next) {
        const SGIModelXioCable *cable = entry->value;
        const SGIModelXioEnd *ends[2] = {
            cable->ends->value, cable->ends->next->value,
        };
        const SGIModelXioEnd *node_end = NULL, *peer_end = NULL;

        for (unsigned int side = 0; side < 2; side++) {
            const SGIModelXioEnd *end = ends[side];

            if (end->kind == SGI_MODEL_XIO_END_KIND_CROSSBAR) {
                bool *used;

                if (end->port >= SGI_XIO_WIDGETS) {
                    error_setg(errp, "topology '%s' XIO cable '%s' names "
                               "crossbar port 0x%x, past a crossbar's ports",
                               t->name, cable->name, end->port);
                    return false;
                }
                used = &port_cabled[end->index * SGI_XIO_WIDGETS + end->port];
                if (*used) {
                    error_setg(errp, "topology '%s' crossbar '%s' port 0x%x "
                               "carries more than one XIO cable", t->name,
                               crossbars[end->index]->id, end->port);
                    return false;
                }
                *used = true;
            } else if (end->kind == SGI_MODEL_XIO_END_KIND_NODE) {
                if (node_cabled[end->index]) {
                    error_setg(errp, "topology '%s' node '%s' carries more "
                               "than one XIO cable", t->name,
                               nodes[end->index]->id);
                    return false;
                }
                node_cabled[end->index] = true;
                if (!node_end) {
                    node_end = end;
                    peer_end = ends[side ^ 1];
                }
            }
        }
        if (node_end) {
            const SGIModelNode *node = nodes[node_end->index];

            switch (peer_end->kind) {
            case SGI_MODEL_XIO_END_KIND_NODE:
                error_setg(errp, "topology '%s' XIO cable '%s' joins node "
                           "'%s' to node '%s'; a node's cable meets an "
                           "XBridge port A or B", t->name, cable->name,
                           node->id, nodes[peer_end->index]->id);
                return false;
            case SGI_MODEL_XIO_END_KIND_IO_BOARD:
                error_setg(errp, "topology '%s' XIO cable '%s' joins node "
                           "'%s' to I/O board '%s'; a node's cable meets an "
                           "XBridge port A or B", t->name, cable->name,
                           node->id, boards[peer_end->index]->id);
                return false;
            default:
                break;
            }
            if (crossbars[peer_end->index]->kind ==
                SGI_MODEL_CROSSBAR_KIND_PIC) {
                if (node->has_board_kind &&
                    node->board_kind == SGI_MODEL_BOARD_KIND_IP53) {
                    error_setg(errp, "topology '%s' node '%s' is an Origin "
                               "350 compute module; it cannot be named as "
                               "an I/O attachment of PIC crossbar '%s'",
                               t->name, node->id,
                               crossbars[peer_end->index]->id);
                    return false;
                }
                if (peer_end->port != 0xa) {
                    error_setg(errp, "topology '%s' XIO cable '%s' meets PIC "
                               "crossbar '%s' at port 0x%x; only single "
                               "attachment at port 0xa is built, and no "
                               "contract gives dual attachment", t->name,
                               cable->name, crossbars[peer_end->index]->id,
                               peer_end->port);
                    return false;
                }
            }
            if (!sgi_machine_node_is_cabled(node)) {
                error_setg(errp, "topology '%s' node '%s' has an XBridge on "
                           "its own board; only an IP35 or Cx-brick node "
                           "takes an XIO cable", t->name, node->id);
                return false;
            }
            if (peer_end->port != 0xa && peer_end->port != 0xb) {
                error_setg(errp, "topology '%s' XIO cable '%s' meets crossbar "
                           "'%s' at port 0x%x; a node's cable meets port A "
                           "or B", t->name, cable->name,
                           crossbars[peer_end->index]->id, peer_end->port);
                return false;
            }
            if (host[peer_end->index] >= 0) {
                error_setg(errp, "topology '%s' XBridge '%s' takes one host "
                           "node; node '%s' on port 0x%x would be a second",
                           t->name, crossbars[peer_end->index]->id, node->id,
                           peer_end->port);
                return false;
            }
            host[peer_end->index] = node_end->index;
        } else if (ends[0]->kind == SGI_MODEL_XIO_END_KIND_CROSSBAR &&
                   ends[1]->kind == SGI_MODEL_XIO_END_KIND_CROSSBAR) {
            chained[ends[0]->index] = chained[ends[1]->index] = true;
        } else if (ends[0]->kind == SGI_MODEL_XIO_END_KIND_CROSSBAR ||
                   ends[1]->kind == SGI_MODEL_XIO_END_KIND_CROSSBAR) {
            bool board_first = ends[0]->kind == SGI_MODEL_XIO_END_KIND_IO_BOARD;

            if (!sgi_machine_validate_board_cable(
                    t, cable, ends[!board_first], ends[board_first],
                    crossbars, attachments, attachment_count, boards,
                    board_cabled, errp)) {
                return false;
            }
        } else {
            error_setg(errp, "topology '%s' XIO cable '%s' joins two I/O "
                       "boards", t->name, cable->name);
            return false;
        }
    }

    /* An IP53 topology's crossbar records are refused by its own rules. */
    for (i = 0; i < node_count; i++) {
        ip53 |= nodes[i]->has_board_kind &&
                nodes[i]->board_kind == SGI_MODEL_BOARD_KIND_IP53;
    }
    for (i = 0; !ip53 && i < crossbar_count; i++) {
        bool attached = false;

        for (size_t a = 0; a < attachment_count; a++) {
            attached |= attachments[a]->crossbar_index == (int64_t)i;
        }
        if (crossbars[i]->kind != SGI_MODEL_CROSSBAR_KIND_XBRIDGE &&
            crossbars[i]->kind != SGI_MODEL_CROSSBAR_KIND_PIC) {
            error_setg(errp, "topology '%s' crossbar '%s' is not an XBridge; "
                       "an SN1 crossbar record is an I/O brick's XBridge",
                       t->name, crossbars[i]->id);
            return false;
        }
        if (host[i] < 0 && !chained[i]) {
            error_setg(errp, "topology '%s' crossbar '%s' has %sno XIO cable "
                       "from a node", t->name, crossbars[i]->id,
                       attached ? "XIO attachments and " : "");
            return false;
        }
    }
    for (i = 0; i < attachment_count; i++) {
        const SGIModelXioAttachment *a = attachments[i];

        if (a->crossbar_index < 0) {
            if (a->node_index < node_count &&
                sgi_machine_node_is_cabled(nodes[a->node_index])) {
                error_setg(errp, "topology '%s' node '%s' has an XIO "
                           "attachment on its own board; an IP35 or "
                           "Cx-brick board has no XBridge", t->name,
                           nodes[a->node_index]->id);
                return false;
            }
            continue;
        }
        if (ip53) {
            continue;
        }
        if ((size_t)a->crossbar_index >= crossbar_count ||
            a->node_index >= node_count) {
            error_setg(errp, "topology '%s' XIO attachment %zu names a "
                       "crossbar or node out of range", t->name, i);
            return false;
        }
        if (host[a->crossbar_index] != a->node_index) {
            error_setg(errp, "topology '%s' XIO attachment on crossbar '%s' "
                       "names node '%s', which no XIO cable joins to it",
                       t->name, crossbars[a->crossbar_index]->id,
                       nodes[a->node_index]->id);
            return false;
        }
    }
    for (i = 0; !ip53 && i < board_count; i++) {
        const SGIModelXioAttachment *a;

        if (!boards[i]->has_attachment ||
            boards[i]->attachment >= attachment_count || board_cabled[i]) {
            continue;
        }
        a = attachments[boards[i]->attachment];
        if (a->crossbar_index >= 0 && a->widget >= SGI_MACHINE_XIO_LINK_FIRST &&
            a->widget <= SGI_MACHINE_XIO_LINK_LAST) {
            error_setg(errp, "topology '%s' I/O board '%s' sits at crossbar "
                       "'%s' port 0x%x, which no XIO cable reaches", t->name,
                       boards[i]->id, crossbars[a->crossbar_index]->id,
                       a->widget);
            return false;
        }
    }
    for (i = 0; i < node_count; i++) {
        if (sgi_machine_node_is_ip35(nodes[i]) &&
            nodes[i]->has_option_intx_map) {
            error_setg(errp, "topology '%s' IP35 node '%s' selects an option "
                       "INTx map; an IP35 board has no option slots",
                       t->name, nodes[i]->id);
            return false;
        }
    }
    for (i = 0; t->xio_cables && i < board_count; i++) {
        if (boards[i]->populated &&
            boards[i]->preset == SGI_MODEL_IO_BOARD_PRESET_VBRICK) {
            error_setg(errp, "the experimental vbrick preset is Origin "
                       "300-only, and topology '%s' has XIO cables", t->name);
            return false;
        }
    }
    return true;
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
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(machine);
    const SGIModelTopology *chassis = sgi_machine_chassis(machine);
    bool allow_ip53_experiment;
    bool allow_ip53_fixture = smc->test_topology && qtest_enabled();

    if (!chassis) {
        error_setg(errp, "unknown topology");
        return NULL;
    }
    if (!sgi_model_check_runtime_endpoints(chassis, errp) ||
        !sgi_machine_refuse_unbuilt_records(chassis, errp) ||
        !sgi_machine_validate_module_controllers(chassis, errp) ||
        !sgi_machine_validate_xio_cables(chassis, errp) ||
        !sgi_machine_validate_router_ports(chassis, errp) ||
        !sgi_machine_ip53_experiment_policy(
            s, smc, chassis, &allow_ip53_experiment, errp)) {
        return NULL;
    }
    if (!s->view) {
        s->view = sgi_model_resolve_population(chassis, s->nodes_requested,
                                               errp);
    }
    if (!s->view || !sgi_machine_preflight_io_descriptors(
            s->view, allow_ip53_fixture, allow_ip53_experiment,
            g_strcmp0(smc->product, "tezro") == 0, errp)) {
        s->view = NULL;
        return NULL;
    }
    return s->view;
}

static const SGIModelTopology *sgi_machine_topology(MachineState *machine)
{
    return sgi_machine_resolve(machine, NULL);
}

/*
 * -smp defaults to the product's default topology and its most node
 * boards, whatever nodes= says: the machine cannot tell a default from an
 * explicit value. (-m can: see sgi_machine_settle_default_ram().) A
 * refusal says so when nodes= asked for fewer.
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
    return "; -smp defaults to the most node boards, so give it with nodes=";
}

/*
 * A marker, not a size: what default_ram_size holds so that an absent -m
 * can be told from every explicit one. It is the largest value QEMU's 8 KiB
 * rounding of -m leaves unchanged, so no sensible -m reaches it. The one
 * collision left is an explicit -m of exactly this many bytes (16 EiB less
 * 8 KiB), which reads as absent where the parent refused it. Until the
 * machine settles it before RAM is sized, -preconfig's qom-get of memory,
 * and the -numa memdev size check that runs first, show this value.
 */
#define SGI_RAM_UNSET (UINT64_MAX & ~(UINT64_C(8192) - 1))

static uint64_t sgi_machine_nodes_ram(const SGIModelTopology *t);

void sgi_machine_class_set_default_ram(MachineClass *mc, uint64_t bytes)
{
    SGI_MACHINE_CLASS(mc)->fallback_ram_size = bytes;
    mc->default_ram_size = SGI_RAM_UNSET;
}

/*
 * Without -m, fit every node board the selected topology and nodes= build
 * with the memory its catalogue gives each, not the product's default
 * topology's total. A class's default_ram_size is SGI_RAM_UNSET (see
 * sgi_machine_class_set_default_ram()), so QEMU leaves that in ram_size
 * when -m is absent. This runs before QEMU sizes the default RAM backend
 * from ram_size. The boards have no memory hotplug, so -m maxmem= and
 * slots= are refused here; with no size beside them QEMU's own check
 * refuses them first, against the marker.
 */
static bool sgi_machine_settle_default_ram(MachineState *machine,
                                           Error **errp)
{
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(machine);
    const SGIModelTopology *t;

    /*
     * A memory backend replaces ram_size but leaves maxram_size at the
     * marker QEMU copied from default_ram_size: no maxmem was given.
     */
    if (machine->maxram_size == SGI_RAM_UNSET) {
        machine->maxram_size = machine->ram_size;
    }
    if (machine->ram_slots || machine->maxram_size != machine->ram_size) {
        error_setg(errp, "%s: the memory has no hotplug, so -m maxmem= and "
                   "slots= are not supported",
                   MACHINE_GET_CLASS(machine)->name);
        return false;
    }
    if (machine->ram_size != SGI_RAM_UNSET) {
        return true;
    }
    t = sgi_machine_topology(machine);
    machine->ram_size = t ? sgi_machine_nodes_ram(t) : smc->fallback_ram_size;
    machine->maxram_size = machine->ram_size;
    return true;
}

static bool sgi_machine_create_default_memdev(MachineState *machine,
                                              const char *path, Error **errp)
{
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(machine);

    if (!sgi_machine_settle_default_ram(machine, errp)) {
        return false;
    }
    return smc->parent_create_default_memdev(machine, path, errp);
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

    for (entry = sgi_machines()->topologies; entry; entry = entry->next) {
        const SGIModelTopology *t = entry->value;
        const SGIModelNodePopulationList *row;

        if (!sgi_model_topology_matches_product(t, product)) {
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

unsigned int sgi_machine_product_min_cpus(const char *product)
{
    unsigned int min_cpus = UINT_MAX;
    const SGIModelTopologyList *entry;

    for (entry = sgi_machines()->topologies; entry; entry = entry->next) {
        const SGIModelTopology *t = entry->value;
        const SGIModelNodePopulationList *row;

        if (!sgi_model_topology_matches_product(t, product)) {
            continue;
        }
        for (row = t->node_populations; row; row = row->next) {
            const SGIModelCpuPopulationList *population;

            for (population = row->value->supported_cpu_populations;
                 population; population = population->next) {
                unsigned int total =
                    sgi_machine_population_total(population->value);

                min_cpus = MIN(min_cpus, total);
            }
        }
    }
    g_assert(min_cpus != UINT_MAX);
    return min_cpus;
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

/* The memory every node of @t fits, summed over its banks. */
static uint64_t sgi_machine_nodes_ram(const SGIModelTopology *t)
{
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

uint64_t sgi_machine_topology_default_ram(const SGIModelTopology *chassis)
{
    return sgi_machine_nodes_ram(sgi_model_population_view(
        chassis, sgi_model_default_population(chassis), &error_abort));
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
 * The per-node memory sizes this node accepts, per
 * SGIModelNode.accepted-memory-mib: a present list is the DIMM kit
 * sizes the node accepts, and an absent one means the node's own
 * shipped bank-mib total is the only accepted size, not an open
 * fitting rule. A selected processor @option replaces either with its
 * own list, since the processor clock decides which banks the PROM keeps.
 */
static uint64_t *sgi_machine_node_accepted_mib(
    const SGIModelNode *node, const SGIModelProcessorOption *option,
    size_t *count)
{
    g_autofree uint64_t *bank_mib = NULL;
    uint64_t *shipped;
    size_t bank_count, bank;

    if (option) {
        return sgi_model_array_uint64(option->accepted_memory_mib, count);
    }
    if (node->accepted_memory_mib) {
        return sgi_model_array_uint64(node->accepted_memory_mib, count);
    }
    bank_mib = sgi_model_array_uint64(node->bank_mib, &bank_count);
    shipped = g_new0(uint64_t, 1);
    for (bank = 0; bank < bank_count; bank++) {
        *shipped += bank_mib[bank];
    }
    *count = 1;
    return shipped;
}

/*
 * The first node that does not accept the per-node memory size (-m split
 * evenly across every node), or NULL when every node accepts it.
 */
static const SGIModelNode *sgi_machine_refuse_memory_population(
    const SGIModelTopology *t, const SGIModelProcessorOption *option,
    uint64_t per_node_mib)
{
    g_autofree const SGIModelNode **nodes = NULL;
    size_t node_count, node_index;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    for (node_index = 0; node_index < node_count; node_index++) {
        size_t accepted_count, i;
        g_autofree uint64_t *accepted =
            sgi_machine_node_accepted_mib(nodes[node_index], option,
                                          &accepted_count);
        bool found = false;

        for (i = 0; i < accepted_count; i++) {
            if (accepted[i] == per_node_mib) {
                found = true;
                break;
            }
        }
        if (!found) {
            return nodes[node_index];
        }
    }
    return NULL;
}

/* "64, 128 or 256": a node's accepted memory sizes, for a refusal message. */
static char *sgi_machine_accepted_memory(
    const SGIModelNode *node, const SGIModelProcessorOption *option)
{
    GString *out = g_string_new(NULL);
    size_t count, i;
    g_autofree uint64_t *accepted =
        sgi_machine_node_accepted_mib(node, option, &count);

    for (i = 0; i < count; i++) {
        g_string_append_printf(out, "%s%" PRIu64,
                               i == 0 ? "" : i + 1 == count ? " or " : ", ",
                               accepted[i]);
    }
    return g_string_free(out, false);
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

/* The backend id for board @index's parallel port; see the helper. */
static const char *sgi_machine_parallel_label(unsigned int index)
{
    Chardev *chr = sgi_ioc3_parallel_host_backend(index);

    return chr ? chr->label : NULL;
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
 * A crossbar's manufacturer id and part number are fixed by which chip it is
 * (the SGIModelCrossbarKind split: a standalone Crossbow versus an XBridge's
 * internal crossbar section), the same on every machine fitting either, so the
 * compiled catalogue no longer carries them. The manufacturer id is zero for
 * both kinds measured so far; the part number is what every measured
 * configuration's descriptor named before the ownership-rule trim.
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

            if ((unsigned int)attachment->crossbar_index == i &&
                !(s->pipe_attachment && s->pipe_attachment[attachment_index])) {
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
 * widget. The id also names the board's PCI bus, <id>.0. On an SN1
 * machine the only such board is one cabled to an I/O brick's crossbar.
 */
static void sgi_machine_add_xio_board(SGIMachineState *s, DeviceState *dev,
                                      const SGIModelIoBoard *record,
                                      unsigned int board_index,
                                      const SGIModelXioAttachment *attachment)
{
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));
    /*
     * A board cabled to an I/O brick's crossbar sits in no XIO slot the
     * catalogue labels; it is known by its catalogue id instead.
     */
    g_autofree char *name = s->enclosure_xbridges ?
        g_strdup(record->id) :
        sgi_machine_xio_board_name(t, record, board_index);

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
 * module is cabled. The barcode and serial are marked synthetic ones, and
 * the serial only has to read back distinct from the IP29 record's: it is
 * the XIO option board letter with the IP29 Bridge's own instance.
 */
static bool sgi_machine_add_crosstown_record(DeviceState *bridge,
                                             uint64_t bridge_serial,
                                             Error **errp)
{
    uint64_t serial = SGI_SYNTHETIC_ONEWIRE_SERIAL('O', bridge_serial & 0xff);
    char barcode[SGI_SYNTHETIC_BARCODE_SIZE];
    const SGIBridgeBoardRecord record = {
        .barcode = barcode,
        .part = "030-1264-001",
        .revision = "C",
        .name = "XBOX_EXT",
    };

    sgi_synthetic_barcode(barcode, 'O', serial);
    return sgi_bridge_add_onewire_board_part(bridge, &record, serial, errp);
}

/* The block node or drive bound to item @name, or NULL. */
static const char *sgi_machine_storage_node(SGIMachineState *s,
                                            const char *name)
{
    return sgi_storage_node(s->storage, name);
}

/*
 * The module<N>-nvram store binding for @module's controller NVRAM, or
 * NULL when nothing binds it or the module has no such store.
 */
static const char *sgi_machine_module_nvram_node(SGIMachineState *s,
                                                 const SGIModelTopology *t,
                                                 const SGIModelModule *module)
{
    g_autofree char *name = NULL;

    if (!sgi_model_module_has_nvram(t, module)) {
        return NULL;
    }
    name = g_strdup_printf("module%u-nvram",
                           sgi_model_module_nvram_index(t, module));
    return sgi_machine_storage_node(s, name);
}

/*
 * Give an Origin 200 node's embedded ELSC its module's configuration NVRAM:
 * the store binding and the bytes an unbacked store starts with. Each such
 * module has the one node, so the store has one Hub to sit behind.
 */
static void sgi_machine_attach_elsc_nvram(SGIMachineState *s,
                                          const SGIModelTopology *t,
                                          unsigned int node_index,
                                          SN0NodeState *node)
{
    const SGIModelModuleList *entry;

    for (entry = t->modules; entry; entry = entry->next) {
        const SGIModelModule *module = entry->value;
        const uint32List *member;

        for (member = module->nodes; member; member = member->next) {
            if (member->value != node_index) {
                continue;
            }
            if (!sgi_model_module_has_nvram(t, module)) {
                return;
            }
            node->elsc_nvram_node = sgi_machine_module_nvram_node(s, t,
                                                                  module);
            sgi_model_module_nvram_seed(t, module, &node->elsc_nvram_config,
                                        &node->elsc_nvram_module,
                                        &node->elsc_nvram_clock);
            return;
        }
    }
}

/*
 * The BASEIO PROM flash backing for @record: its io<N>-flash binding or
 * the legacy drive @legacy names, refusing both at once.
 */
static bool sgi_machine_io_flash_backing(SGIMachineState *s,
                                         const SGIModelTopology *t,
                                         const SGIModelIoBoard *record,
                                         BlockBackend *legacy,
                                         const char **backing, Error **errp)
{
    g_autofree char *name = g_strdup_printf(
        "io%u-flash",
        sgi_model_io_board_index(t, record, sgi_model_io_board_has_io_flash));
    /* A board with no PROM flash takes no io<N>-flash, as for NVRAM. */
    const char *node = sgi_model_io_board_has_io_flash(record) ?
                       sgi_machine_storage_node(s, name) : NULL;

    if (node && legacy) {
        error_setg(errp, "%s: %s and -drive id=%s both back the same BASEIO "
                   "PROM flash", MACHINE_GET_CLASS(s)->name, name,
                   blk_name(legacy));
        return false;
    }
    *backing = legacy ? blk_name(legacy) : node;
    return true;
}

/*
 * The Timekeeper NVRAM and clock record backing for @record: its nvram<N>
 * and nvram<N>-clock bindings or the legacy backend @legacy, refusing
 * both at once and a clock record bound without its NVRAM. @clock is
 * NULL for a legacy backend, whose model finds "<id>-clock" itself.
 */
static bool sgi_machine_nvram_backing(SGIMachineState *s,
                                      const SGIModelTopology *t,
                                      const SGIModelIoBoard *record,
                                      BlockBackend *legacy,
                                      const char **nvram, const char **clock,
                                      Error **errp)
{
    const char *product = MACHINE_GET_CLASS(s)->name;
    unsigned int index =
        sgi_model_io_board_index(t, record, sgi_model_io_board_has_nvram);
    g_autofree char *name = g_strdup_printf("nvram%u", index);
    g_autofree char *clock_name = g_strdup_printf("nvram%u-clock", index);
    const char *node = NULL, *clock_node = NULL;

    if (sgi_model_io_board_has_nvram(record)) {
        node = sgi_machine_storage_node(s, name);
        clock_node = sgi_machine_storage_node(s, clock_name);
    }
    if (legacy && (node || clock_node)) {
        error_setg(errp, "%s: %s and -drive id=%s both back the same "
                   "Timekeeper NVRAM", product, node ? name : clock_name,
                   blk_name(legacy));
        return false;
    }
    if (clock_node && !node) {
        error_setg(errp, "%s: %s needs %s", product, clock_name, name);
        return false;
    }
    *nvram = legacy ? blk_name(legacy) : node;
    *clock = clock_node;
    return true;
}

/*
 * IP29's onboard I/O, one per Origin 200 node: linked to its node's own
 * Hub, or to a GIGAchannel cabinet's Crossbow port. sn0.c owns the
 * Bridge, the sgi-baseio behind it and the link (see
 * sn0_node_build_baseio()'s own comment for why); this function resolves
 * every fitted-part value and backend id the descriptor and command line
 * carry, the same policy the Crossbow-attached BaseIO card below applies,
 * and names the port the Bridge links to.
 */
/*
 * Whether @board is an onboard Ethernet: the IP29 I/O of an Origin 200
 * node, the BaseIO card of an Origin 2000 or Onyx2, or an IP53 IO9 board
 * that fits its BCM5701. Add-in cards such as MENET and the MediaIO's
 * second IOC3 keep taking -nic.
 */
static bool sgi_machine_onboard_ethernet(const SGIModelIoBoard *board)
{
    const SGIModelPciFunctionList *entry;

    switch (board->preset) {
    case SGI_MODEL_IO_BOARD_PRESET_IP29_IO:
    case SGI_MODEL_IO_BOARD_PRESET_IP29_IO_CROSSTOWN:
    case SGI_MODEL_IO_BOARD_PRESET_IO6:
    case SGI_MODEL_IO_BOARD_PRESET_IO6G:
        return board->populated;
    case SGI_MODEL_IO_BOARD_PRESET_IO9:
        if (!board->populated) {
            return false;
        }
        for (entry = board->pci_functions; entry; entry = entry->next) {
            if (entry->value->kind == SGI_MODEL_PCI_FUNCTION_KIND_BCM5701) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

/*
 * The station address of @t's I/O board @board_index: mac=, or the
 * shared default, plus how many onboard Ethernets come before it, as
 * each Bedrock node takes node 0's address plus its index.
 */
static char *sgi_machine_onboard_mac(SGIMachineState *s,
                                     const SGIModelTopology *t,
                                     unsigned int board_index, Error **errp)
{
    const SGIModelIoBoardList *entry = t->io_boards;
    unsigned int ethernet = 0;

    for (unsigned int i = 0; i < board_index && entry;
         i++, entry = entry->next) {
        ethernet += sgi_machine_onboard_ethernet(entry->value);
    }
    return sgi_board_mac(s->mac, ethernet, errp);
}

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
    const char *parallel_id = sgi_machine_parallel_label(board_index);
    const char *nvram, *nvram_clock;
    g_autofree char *mac = sgi_machine_onboard_mac(s, t, board_index, errp);

    if (!mac || !sgi_machine_nvram_backing(s, t, record, nvram_blk, &nvram,
                                           &nvram_clock, errp)) {
        return false;
    }
    if (attachment->crossbar_index >= 0) {
        upstream = OBJECT(s->crossbars[attachment->crossbar_index]);
        upstream_index = attachment->widget;
    }
    if (!sn0_node_build_baseio(
            node, OBJECT(s), &s->board_links[board_index], upstream,
            upstream_index, record->serial, ioc3_serial, bus_name,
            nvram, nvram_clock, chardev_a ? chardev_a->label : NULL,
            chardev_b ? chardev_b->label : NULL,
            record_a ? record_a->label : NULL,
            record_b ? record_b->label : NULL, parallel_id, mac, &bridge,
            errp)) {
        return false;
    }
    if (record->preset == SGI_MODEL_IO_BOARD_PRESET_IP29_IO_CROSSTOWN &&
        !sgi_machine_add_crosstown_record(bridge, record->serial, errp)) {
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
    Object *crossbar = s->enclosure_xbridges ?
        OBJECT(s->enclosure_xbridges[attachment->crossbar_index]) :
        OBJECT(s->crossbars[attachment->crossbar_index]);

    sgi_xio_link_init(link, OBJECT(s), name, crossbar, attachment->widget,
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
    const char *flash_backing, *nvram, *nvram_clock;
    g_autofree char *mac = sgi_machine_onboard_mac(s, t, board_index, errp);
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

    if (!mac) {
        object_unref(OBJECT(dev));
        return false;
    }
    if (!sgi_machine_io_flash_backing(s, t, record, flash_blk, &flash_backing,
                                      errp) ||
        !sgi_machine_nvram_backing(s, t, record, nvram_blk, &nvram,
                                   &nvram_clock, errp)) {
        object_unref(OBJECT(dev));
        return false;
    }
    sgi_machine_add_xio_board(s, dev, record, board_index, attachment);
    qdev_prop_set_uint64(dev, "onewire-serial", record->serial);
    if (record->has_ioc3_serial) {
        qdev_prop_set_uint64(dev, "ioc3-onewire-serial", record->ioc3_serial);
    }
    qdev_prop_set_string(dev, "preset", preset);
    if (nvram) {
        qdev_prop_set_string(dev, "nvram-drive", nvram);
    }
    if (nvram_clock) {
        qdev_prop_set_string(dev, "nvram-clock-drive", nvram_clock);
    }
    if (flash_backing) {
        qdev_prop_set_string(dev, "flash-drive", flash_backing);
    }
    qdev_prop_set_string(dev, "ioc3-mac", mac);
    if (s->io_prom && sgi_baseio_preset_lookup(preset)->bridge_flash) {
        qdev_prop_set_string(dev, "firmware", s->io_prom);
        s->io_prom_used = true;
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
     * card has no parallel port, and an empty slot has a null backend
     * whether or not -parallel was given, so only io6g takes it.
     */
    if (g_str_equal(preset, "io6g")) {
        const char *parallel_id = sgi_machine_parallel_label(board_index);

        if (parallel_id) {
            qdev_prop_set_string(dev, "parallel-chardev", parallel_id);
        }
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
 * The board's identity and the card's one-wire part come from the compiled
 * machine model.
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
    const char *flash_backing;

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
    if (!sgi_machine_io_flash_backing(s, t, record, flash_blk, &flash_backing,
                                      errp)) {
        object_unref(OBJECT(dev));
        return false;
    }

    object_property_add_child(OBJECT(s), SGI_MACHINE_GIGACHANNEL_NAME,
                              OBJECT(dev));
    object_unref(OBJECT(dev));
    /* Set directly: qdev_set_id() would reparent under /machine/peripheral. */
    dev->id = g_strdup(SGI_MACHINE_GIGACHANNEL_NAME);
    qdev_prop_set_uint32(dev, "module", record->module_index);
    sgi_machine_set_board_widget(dev, attachment->widget);
    qdev_prop_set_uint64(dev, "onewire-serial", record->serial);
    if (flash_backing) {
        qdev_prop_set_string(dev, "flash-drive", flash_backing);
    }
    if (s->io_prom) {
        qdev_prop_set_string(dev, "firmware", s->io_prom);
        s->io_prom_used = true;
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
 * The Hub a KONA pipe's host-memory writes and interrupt packets are
 * composed for: the first XIO attachment of the board's own node on the
 * board's crossbar, which sgi_machine_index_xio_attachments() claims as
 * that node's own Hub link (later attachments of the node are boards). A
 * one-node topology has exactly one such Hub. On a multi-node crossbar the
 * pipe posts to its own node's Hub. That is an approximation: the node the
 * driver's reason page lives on is not yet taken from the address the
 * driver registers.
 */
static unsigned int sgi_machine_kona_hub_widget(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelXioAttachment *board)
{
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    size_t attachment_count;
    size_t i;

    /*
     * On an I/O brick's crossbar the Bedrock is the widget at the port
     * its XIO cable reaches, the chip's host port, which the crossbar
     * routes the pipe's packets to.
     */
    if (board->crossbar_index >= 0 && s->enclosure_xbridges) {
        return object_property_get_uint(
            OBJECT(s->enclosure_xbridges[board->crossbar_index]),
            "host-port", &error_abort);
    }
    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                 &attachment_count);
    for (i = 0; i < attachment_count; i++) {
        const SGIModelXioAttachment *a = attachments[i];

        if (a->node_index == board->node_index &&
            a->crossbar_index == board->crossbar_index) {
            return a->widget;
        }
    }
    warn_report("topology '%s': no Hub attachment found for the KONA board's"
                " node %u on crossbar %" PRId64 "; composing its packets"
                " for widget 0",
                t->name, board->node_index, board->crossbar_index);
    return 0;
}

/*
 * The topology's own graphics pipe records: the Onyx2 InfiniteReality
 * pipe's KONA widget, Crossbow-attached at IO3 on the rack and IO4 on the
 * deskside, and a VPro wired straight to a node's XBridge, the Fuel's
 * graphics board at widget 13, whose node already selected that widget as
 * the chip's external port, and the V-brick's V12. None is built with the
 * machine. Each is a slot the catalogue names (io3, io4, widget-13,
 * widget-11): a pipe -device, or the default
 * pipe the machine fits when there is none, claims it, and takes the
 * record's port and identity (sgi_machine_claim_pipe_record()).
 */
static bool sgi_machine_pipe_preset(SGIModelIoBoardPreset preset)
{
    return preset == SGI_MODEL_IO_BOARD_PRESET_KONA ||
           preset == SGI_MODEL_IO_BOARD_PRESET_ODYSSEY ||
           preset == SGI_MODEL_IO_BOARD_PRESET_VBRICK;
}

/*
 * Mark the topology's pipe records, with the XIO attachment each names:
 * @board_attachment's entry for a Hub topology's boards, the record's own
 * otherwise. A Crossbow-attached pipe's port starts empty, as an empty
 * XIO slot's does, until the pipe that claims it attaches live.
 */
static void sgi_machine_mark_pipe_records(SGIMachineState *s,
                                          const SGIModelTopology *t,
                                          const unsigned int *board_attachment)
{
    g_autofree const SGIModelIoBoard **records = NULL;
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    size_t count, attachment_count, i;

    records = sgi_model_io_board_array(t->io_boards, &count);
    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                 &attachment_count);
    s->pipe_record = g_new0(bool, count + 1);
    s->pipe_claimed = g_new0(bool, count + 1);
    s->pipe_record_attachment = g_new0(const SGIModelXioAttachment *,
                                       count + 1);
    s->pipe_attachment = g_new0(bool, attachment_count + 1);
    for (i = 0; i < count; i++) {
        unsigned int index = board_attachment ? board_attachment[i] :
                             records[i]->attachment;

        if (!records[i]->populated ||
            !sgi_machine_pipe_preset(records[i]->preset) ||
            index >= attachment_count) {
            continue;
        }
        s->pipe_record[i] = true;
        s->pipe_record_attachment[i] = attachments[index];
        if (records[i]->preset != SGI_MODEL_IO_BOARD_PRESET_VBRICK) {
            s->pipe_attachment[index] = true;
        }
    }
}

/*
 * The catalogue's name for pipe record @index: its catalogue id when it
 * is cabled to an I/O brick's crossbar, as every board there is known;
 * its XIO slot label when it is Crossbow-attached; else widget-<N> for
 * the XBridge port it is wired to.
 */
static char *sgi_machine_pipe_record_slot(SGIMachineState *s,
                                          const SGIModelIoBoard *record,
                                          size_t index)
{
    const SGIModelXioAttachment *attachment =
        s->pipe_record_attachment[index];

    if (s->enclosure_xbridges && attachment->crossbar_index >= 0) {
        return g_strdup(record->id);
    }
    if (attachment->crossbar_index >= 0) {
        return g_strdup(record->label);
    }
    return g_strdup_printf("widget-%u", attachment->widget);
}

/* The pipe record XIO slot @name names, or -1. */
static ssize_t sgi_machine_find_pipe_record(SGIMachineState *s,
                                            const SGIModelTopology *t,
                                            const char *name)
{
    g_autofree const SGIModelIoBoard **records = NULL;
    size_t count, i;

    if (!s->pipe_record || !name) {
        return -1;
    }
    records = sgi_model_io_board_array(t->io_boards, &count);
    for (i = 0; i < count; i++) {
        g_autofree char *slot = NULL;

        if (!s->pipe_record[i]) {
            continue;
        }
        slot = sgi_machine_pipe_record_slot(s, records[i], i);
        if (g_str_equal(slot, name)) {
            return i;
        }
    }
    return -1;
}

/*
 * The catalogue slot position whose pipe the shared boards of pipe record
 * @index follow: the first KONA record of the same module listing the
 * same other identity parts, as the two pipes of an Onyx2 graphics module
 * list its one Ktown. A record with no other parts shares nothing.
 */
static int sgi_machine_pipe_shared_position(SGIMachineState *s,
                                            const SGIModelTopology *t,
                                            const SGIModelGraphics *g,
                                            size_t index)
{
    g_autofree const SGIModelIoBoard **records = NULL;
    const SGIModelIoBoard *own;
    size_t count;

    records = sgi_model_io_board_array(t->io_boards, &count);
    own = records[index];
    if (!own->has_other_identity_parts || !own->other_identity_parts) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        const SGIModelIoBoard *other = records[i];
        g_autofree char *slot = NULL;

        if (!s->pipe_record[i] ||
            other->preset != SGI_MODEL_IO_BOARD_PRESET_KONA ||
            other->module_index != own->module_index ||
            !other->has_other_identity_parts || !other->other_identity_parts ||
            !g_str_equal(other->other_identity_parts->value,
                         own->other_identity_parts->value)) {
            continue;
        }
        slot = sgi_machine_pipe_record_slot(s, other, i);
        return sgi_graphics_slot_position(g, slot);
    }
    return -1;
}

/*
 * What a KONA pipe in the Onyx2's own pipe slot takes from the record,
 * before it realizes: the pipe's one-wire records, the geometry board's
 * own part (the rack's GE16-4 or the deskside's GE14-4) and the records
 * after it (the rack's Ktown, or none on the deskside, whose pipe has no
 * Ktown), and the Hub its DMA transfers and reason-page writes go to,
 * since they name no destination widget of their own. A `-device
 * sgi-kona` in any other slot names its Hub with fabric-hub-widget.
 *
 * Each pipe's serials follow its slot's place in the catalogue's slot
 * list, not plug order: the geometry board takes the record's own serial
 * and every other record sixteen instances per slot on, except a board
 * two pipes share, which keeps the first pipe's.
 */
static void sgi_machine_fill_pipe_record(SGIMachineState *s,
                                         const SGIModelTopology *t,
                                         DeviceState *dev)
{
    g_autofree const SGIModelIoBoard **records = NULL;
    g_autofree char *name = NULL;
    const SGIModelIoBoard *record;
    const strList *part;
    QList *parts = qlist_new();
    size_t count;
    ssize_t index;
    int position;

    if (!object_dynamic_cast(OBJECT(dev), TYPE_SGI_KONA)) {
        qobject_unref(parts);
        return;
    }
    name = object_property_get_str(OBJECT(dev), "slot", &error_abort);
    index = sgi_machine_find_pipe_record(s, t, name);
    records = sgi_model_io_board_array(t->io_boards, &count);
    if (index < 0 || records[index]->preset != SGI_MODEL_IO_BOARD_PRESET_KONA) {
        qobject_unref(parts);
        return;
    }
    record = records[index];
    qdev_prop_set_string(dev, "board-part", record->part);
    part = record->has_other_identity_parts ?
           record->other_identity_parts : NULL;
    for (; part; part = part->next) {
        qlist_append_str(parts, part->value);
    }
    qdev_prop_set_array(dev, "other-identity-parts", parts);
    qdev_prop_set_uint32(dev, "fabric-hub-widget",
                         sgi_machine_kona_hub_widget(
                             s, t, s->pipe_record_attachment[index]));
    position = sgi_graphics_slot_position(
        sgi_machine_chassis(MACHINE(s))->graphics, name);
    if (position >= 0) {
        int shared = sgi_machine_pipe_shared_position(
            s, t, sgi_machine_chassis(MACHINE(s))->graphics, index);

        qdev_prop_set_uint8(dev, "identity-index", position);
        if (shared >= 0 && shared != position) {
            qdev_prop_set_uint8(dev, "shared-identity-index", shared);
        }
    }
    qdev_prop_set_uint64(dev, "geometry-serial", record->serial);
    /* A pipe cabled to an I/O brick's crossbar is a G-brick's. */
    qdev_prop_set_bit(dev, "ktown2", s->enclosure_xbridges &&
                      s->pipe_record_attachment[index]->crossbar_index >= 0);
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

/*
 * A V-brick cabled to a node's XBridge external port. No topology uses it:
 * the Origin 300 pairing it was written for was never an SGI configuration
 * and was removed, and it stays for a later Onyx 3000 or Origin 350
 * topology. The published enclosure has its own XBridge, L1 and V12; the
 * cable ports and nested processor address are not established. The
 * 8 MiB split is a machine-local QEMU projection: V12 at the cable widget
 * base, then the enclosure XBridge's crossbar.
 * The enclosure's V12 at XBridge port 8 is the topology's pipe slot
 * widget-11, which a pipe -device or the default pipe claims
 * (sgi_machine_claim_pipe_record()).
 */
static bool sgi_machine_instantiate_vbrick(
    SGIMachineState *s, const SGIModelIoBoard *record,
    unsigned int board_index, const SGIModelXioAttachment *attachment,
    Error **errp)
{
    SN1NodeState *node;
    Object *module;
    DeviceState *bridge;
    g_autofree char *module_name = NULL;

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

    /* The logical registry constructs the enclosure's reference controller. */
    g_assert(object_resolve_path_component(module, "vbrick-l1"));

    sgi_xbridge_project_vbrick_pio(
        SGI_XBRIDGE(node->xbridge),
        sgi_xbridge_widget_view(SGI_XBRIDGE(bridge), 8),
        sgi_xbridge_widget_view(SGI_XBRIDGE(bridge), 0), 8 * MiB);
    s->vbrick_bridge = bridge;
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
            !clock->has_year_base || clock->year_base != 1968 ||
            clock->initial_ip27_fixture) {
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
        const char *nvram, *nvram_clock;

        if (!sgi_machine_nvram_backing(s, t, record, nvram_blk, &nvram,
                                       &nvram_clock, errp)) {
            return false;
        }
        if (nvram) {
            qdev_prop_set_string(baseio, "nvram-drive", nvram);
        }
        if (nvram_clock) {
            qdev_prop_set_string(baseio, "nvram-clock-drive", nvram_clock);
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
        if (node_index == 0) {
            const char *parallel_id = sgi_machine_parallel_label(0);

            if (parallel_id) {
                qdev_prop_set_string(baseio, "parallel-chardev", parallel_id);
            }
        }
    }
    return qdev_realize(DEVICE(baseio), NULL, errp);
}

/*
 * The I-brick's IO7, on head F of the XBridge in its own brick: an
 * sgi-baseio "io7" instance under that brick's module. The serial
 * line it takes is the next one after the hosting node's L1, as the
 * IO-8's is; its one serial port takes port A's console and record
 * lines.
 */
static bool sgi_machine_instantiate_io7(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXioAttachment *attachment, Error **errp)
{
    size_t crossbar_count;
    g_autofree const SGIModelCrossbar **crossbars =
        sgi_model_crossbar_array(t->crossbars, &crossbar_count);
    g_autofree char *module_name = g_strdup_printf(
        "module%u", crossbars[attachment->crossbar_index]->module_index);
    Object *module = object_resolve_path_component(OBJECT(s), module_name);
    DeviceState *xbridge = s->enclosure_xbridges[attachment->crossbar_index];
    unsigned int node_index = attachment->node_index;
    DriveInfo *nvram_dinfo = drive_get(IF_MTD, 0, node_index);
    g_autofree char *nvram_name = g_strdup_printf("sgi-nvram%u", node_index);
    BlockBackend *nvram_blk = nvram_dinfo ? blk_by_legacy_dinfo(nvram_dinfo) :
                              blk_by_name(nvram_name);
    const char *nvram, *nvram_clock;
    DeviceState *io7;
    Chardev *chr;

    if (attachment->widget != 15) {
        error_setg(errp, "topology '%s' I/O board '%s' places the IO7 at "
                   "widget %u; it is on its XBridge's head F, widget 15",
                   t->name, record->id, attachment->widget);
        return false;
    }
    io7 = qdev_new(TYPE_SGI_BASEIO);
    object_property_add_child(module, "io7", OBJECT(io7));
    object_unref(OBJECT(io7));
    qdev_prop_set_string(io7, "preset", "io7");
    if (record->has_ieee1394_absent && record->ieee1394_absent) {
        qdev_prop_set_bit(io7, "ieee1394-absent", true);
    }
    if (record->has_fibre_channel && record->fibre_channel) {
        qdev_prop_set_bit(io7, "fibre-channel-fitted", true);
    }
    if (record->system_timekeeper) {
        const SGIModelSystemTimekeeper *clock = record->system_timekeeper;

        if (clock->kind != SGI_MODEL_SYSTEM_TIMEKEEPER_KIND_M48T35_COMPATIBLE ||
            !clock->has_year_base || clock->year_base != 1968 ||
            clock->initial_ip27_fixture) {
            error_setg(errp, "unsupported fitted system Timekeeper policy");
            return false;
        }
        qdev_prop_set_uint8(io7, "system-timekeeper-kind",
                            SGI_IOC3_TIMEKEEPER_M48T35);
        qdev_prop_set_uint32(io7, "system-timekeeper-year-base",
                             clock->year_base);
        if (!sgi_machine_nvram_backing(s, t, record, nvram_blk, &nvram,
                                       &nvram_clock, errp)) {
            return false;
        }
        if (nvram) {
            qdev_prop_set_string(io7, "nvram-drive", nvram);
        }
        if (nvram_clock) {
            qdev_prop_set_string(io7, "nvram-clock-drive", nvram_clock);
        }
    }
    qdev_prop_set_uint32(io7, "ioc3-subsystem-id",
                         s->io_config[board_index].ioc3_subsystem_id);
    if (record->has_ioc3_serial) {
        qdev_prop_set_uint64(io7, "onewire-serial", record->ioc3_serial);
    }
    chr = serial_hd(sgi_sn1_serial_line(node_index, 1));
    if (chr) {
        qdev_prop_set_string(io7, "chardev-a", chr->label);
    }
    chr = serial_hd(sgi_sn1_serial_line(node_index, 3));
    if (chr) {
        qdev_prop_set_string(io7, "record-a", chr->label);
    }
    object_property_set_link(OBJECT(io7), "pci-bus",
                             OBJECT(sgi_xbridge_pci_bus(SGI_XBRIDGE(xbridge),
                                                        15)),
                             &error_abort);
    return qdev_realize(io7, NULL, errp);
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
static bool sgi_machine_instantiate_enclosure_pic(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXioAttachment *attachment, Error **errp);

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
    case SGI_MODEL_IO_BOARD_PRESET_IO7:
        return sgi_machine_instantiate_io7(s, t, record, board_index,
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
    case SGI_MODEL_IO_BOARD_PRESET_ODYSSEY:
        /*
         * The topology's own pipe: a slot that a pipe -device or the
         * default pipe claims once machine init has returned
         * (sgi_machine_claim_pipe_record()).
         */
        g_assert(s->pipe_record[board_index]);
        return true;
    case SGI_MODEL_IO_BOARD_PRESET_MGRAS:
        return sgi_machine_instantiate_mgras(s, record, board_index,
                                             attachment, errp);
    case SGI_MODEL_IO_BOARD_PRESET_VBRICK:
        return sgi_machine_instantiate_vbrick(s, record, board_index,
                                              attachment, errp);
    case SGI_MODEL_IO_BOARD_PRESET_PIC:
        return sgi_machine_instantiate_enclosure_pic(s, t, record,
                                                     board_index, attachment,
                                                     errp);
    default:
        error_setg(errp, "topology '%s' io board %u names an unsupported "
                  "preset for this family", t->name, board_index);
        return false;
    }
}

/*
 * An I-brick's Fibre Channel card is a 64-bit adapter, and the stock
 * PROM lays its control block out for the doubleword reversal a REQ64#
 * slot gives, so head F's slot for the card drives REQ64#. Inferred from
 * that PROM behaviour alone: no held document gives the slot's wiring.
 */
static void sgi_machine_set_enclosure_req64(const SGIModelTopology *t,
                                            size_t crossbar_index,
                                            DeviceState *xbridge)
{
    g_autofree const SGIModelIoBoard **boards = NULL;
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    size_t board_count, attachment_count, b;

    boards = sgi_model_io_board_array(t->io_boards, &board_count);
    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                 &attachment_count);
    for (b = 0; b < board_count; b++) {
        const SGIModelIoBoard *board = boards[b];

        if (board->populated &&
            board->preset == SGI_MODEL_IO_BOARD_PRESET_IO7 &&
            board->has_fibre_channel && board->fibre_channel &&
            board->attachment < attachment_count &&
            attachments[board->attachment]->crossbar_index ==
            (int64_t)crossbar_index) {
            qdev_prop_set_uint8(xbridge, "req64-slots-f",
                                BIT(SGI_IBRICK_FC_DEVICE));
        }
    }
}

/*
 * Create each I/O brick's XBridge in its brick's module and hand it to
 * the node whose XIO cable reaches it. The chip is left unrealized: the
 * Bedrock's widget links are fixed when the Bedrock realizes, inside node
 * realize, and a widget face exists only once the chip has realized, so
 * the node joins and realizes it there. The machine owns every chip
 * property: the host port is the one the cable meets, and the model's
 * external port, which must differ from it, is the other Crosstown2 port,
 * to which no Bedrock widget is linked. PCI sections are named from their
 * bus, so each chip's buses take its module's id as their prefix. The
 * N-brick's four chips share it; they carry no PCI device.
 */
/*
 * Connect INTA of each configuration device d on both heads of an
 * enclosure PIC to the head's interrupt input d - 1; INTB to INTD stay
 * unconnected, so the head logs an assertion of one. A reviewed transfer
 * of the Origin 350 IO9 route, not measured brick wiring.
 */
static void sgi_machine_wire_enclosure_pic_intx(SGIPICState *pic)
{
    for (unsigned int head = 0; head < 2; head++) {
        DeviceState *dev = sgi_pic_head_device(pic, head);

        for (unsigned int device = 1; device <= 4; device++) {
            qdev_connect_gpio_out_named(dev, "pci-intx", device * 4,
                                        qdev_get_gpio_in(dev, device - 1));
        }
    }
}

/*
 * An IX-brick's or PX-brick's middle PIC, built in its module as the
 * enclosure crossbar under the enclosure head policy and handed to the
 * node whose cable reaches it, which links and realizes it. Its PXBOW's
 * revision is fixed by the composition, so a record naming another
 * cannot be presented. Validation left one such crossbar, on port 0xa.
 */
static bool sgi_machine_create_pic_crossbar(SGIMachineState *s,
                                            const SGIModelTopology *t,
                                            const SGIModelCrossbar *record,
                                            unsigned int node, Error **errp)
{
    g_autofree char *module_name = g_strdup_printf("module%u",
                                                   record->module_index);
    Object *container = object_resolve_path_component(OBJECT(s),
                                                      module_name);
    DeviceState *pic;
    uint64_t revision;

    g_assert(container);
    if (s->enclosure_pic) {
        error_setg(errp, "topology '%s' crossbar '%s' would be a second PIC "
                   "enclosure crossbar; one is built", t->name, record->id);
        return false;
    }
    pic = qdev_new(TYPE_SGI_PIC);
    object_property_add_child(container, "pic", OBJECT(pic));
    object_unref(OBJECT(pic));
    revision = object_property_get_uint(OBJECT(pic), "pxbow-revision",
                                        &error_abort);
    if (revision != record->widget_revision) {
        error_setg(errp, "topology '%s' crossbar '%s' names revision %u; "
                   "the PIC enclosure crossbar presents revision %" PRIu64,
                   t->name, record->id, record->widget_revision, revision);
        return false;
    }
    qdev_prop_set_enum(pic, "compatibility-profile",
        SGI_MODEL_PIC_COMPATIBILITY_PROFILE_PIC_REV3_ENCLOSURE_EXPERIMENTAL);
    if (!sgi_pic_enable_enclosure_crossbar(SGI_PIC(pic),
                                           SGI_BEDROCK_IO_PIO_SIZE, errp) ||
        !sn1_node_supply_pic_crossbar(SN1_NODE(s->bedrock_nodes[node]), pic,
                                      errp)) {
        return false;
    }
    /*
     * The IX-brick's IO9 waits on its IOC4 port presence, its QLogic part
     * and its BCM5701 identity; neither brick has another on-board device.
     */
    qemu_log_mask(LOG_UNIMP, "%s: PIC enclosure '%s' fits no on-board "
                  "device; an IX-brick's IO9 in bus 1 slot 1 is not "
                  "modelled\n", t->name, record->id);
    s->enclosure_pic = pic;
    return true;
}

/*
 * An outer PIC of an IX-brick or PX-brick: its own widget on one link
 * to the middle PIC's crossbar port the cable meets, under the same head
 * policy. It is linked before it realizes, so its packets enter the
 * crossbar at that port.
 */
static bool sgi_machine_instantiate_enclosure_pic(
    SGIMachineState *s, const SGIModelTopology *t,
    const SGIModelIoBoard *record, unsigned int board_index,
    const SGIModelXioAttachment *attachment, Error **errp)
{
    g_autofree char *module_name = g_strdup_printf("module%u",
                                                   record->module_index);
    g_autofree char *link_name = g_strdup_printf("io-board%u-link",
                                                 board_index);
    Object *container = object_resolve_path_component(OBJECT(s),
                                                      module_name);
    Object *crossbar =
        OBJECT(s->enclosure_xbridges[attachment->crossbar_index]);
    SGIXioLink *link = &s->board_links[board_index];
    DeviceState *pic;

    g_assert(container && object_dynamic_cast(crossbar, TYPE_SGI_PIC));
    pic = qdev_new(TYPE_SGI_PIC);
    object_property_add_child(container, record->id, OBJECT(pic));
    object_unref(OBJECT(pic));
    qdev_prop_set_enum(pic, "compatibility-profile",
        SGI_MODEL_PIC_COMPATIBILITY_PROFILE_PIC_REV3_ENCLOSURE_EXPERIMENTAL);
    sgi_xio_link_init(link, OBJECT(s), link_name, crossbar,
                      attachment->widget, OBJECT(pic), 0);
    if (!sgi_xio_link_connect(link, errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(pic), errp) ||
        !sgi_xio_link_connect(link, errp)) {
        return false;
    }
    sgi_machine_wire_enclosure_pic_intx(SGI_PIC(pic));
    return true;
}

static bool sgi_machine_create_enclosure_xbridges(SGIMachineState *s,
                                                  const SGIModelTopology *t,
                                                  Error **errp)
{
    g_autofree const SGIModelCrossbar **crossbars = NULL;
    g_autofree const SGIModelModule **modules = NULL;
    size_t crossbar_count, module_count, i;

    crossbars = sgi_model_crossbar_array(t->crossbars, &crossbar_count);
    modules = sgi_model_module_array(t->modules, &module_count);
    s->enclosure_xbridges = g_new0(DeviceState *, MAX(crossbar_count, 1));
    for (i = 0; i < crossbar_count; i++) {
        const SGIModelCrossbar *record = crossbars[i];
        g_autofree char *module_name = NULL;
        g_autofree char *chip_name = NULL;
        g_autofree char *prefix = NULL;
        unsigned int position = 0, in_module = 0;
        Object *container;
        DeviceState *xbridge;
        unsigned int node;
        uint64_t revision;
        uint8_t port;
        bool hosted;

        /*
         * Validation left only crossbars a node's cable reaches and the
         * downstream chips of a chain, which are refused before they
         * could be built.
         */
        hosted = sgi_machine_crossbar_host(t, i, &node, &port);
        g_assert(record->module_index < module_count);
        if (!hosted) {
            continue;
        }
        if (record->kind == SGI_MODEL_CROSSBAR_KIND_PIC) {
            if (!sgi_machine_create_pic_crossbar(s, t, record, node, errp)) {
                return false;
            }
            s->enclosure_xbridges[i] = s->enclosure_pic;
            continue;
        }
        /*
         * A brick of one chip names it xbridge. The N-brick's four are
         * xbridge0 to xbridge3, in record order within the module.
         */
        for (size_t other = 0; other < crossbar_count; other++) {
            if (crossbars[other]->module_index == record->module_index) {
                if (other < i) {
                    position++;
                }
                in_module++;
            }
        }
        chip_name = in_module == 1 ? g_strdup("xbridge") :
                    g_strdup_printf("xbridge%u", position);
        module_name = g_strdup_printf("module%u", record->module_index);
        container = object_resolve_path_component(OBJECT(s), module_name);
        g_assert(container);
        xbridge = qdev_new(TYPE_SGI_XBRIDGE);
        object_property_add_child(container, chip_name, OBJECT(xbridge));
        object_unref(OBJECT(xbridge));
        /*
         * The crossbar section's revision is fixed by the device model;
         * a record naming another cannot be presented.
         */
        revision = object_property_get_uint(
            object_resolve_path_component(OBJECT(xbridge), "xbow"),
            "revision", &error_abort);
        if (revision != record->widget_revision) {
            error_setg(errp, "topology '%s' crossbar '%s' names revision %u; "
                       "the XBridge model presents revision %" PRIu64,
                       t->name, record->id, record->widget_revision,
                       revision);
            return false;
        }
        qdev_prop_set_uint8(xbridge, "host-port", port);
        qdev_prop_set_uint8(xbridge, "xio-port", port == 0xa ? 0xb : 0xa);
        sgi_machine_set_enclosure_req64(t, i, xbridge);
        prefix = g_strdup_printf("%s-", modules[record->module_index]->id);
        qdev_prop_set_string(xbridge, "bus-prefix", prefix);
        if (!sn1_node_supply_xbridge(SN1_NODE(s->bedrock_nodes[node]),
                                     xbridge, errp)) {
            return false;
        }
        s->enclosure_xbridges[i] = xbridge;
    }
    return true;
}

/*
 * Configure and realize a Cx-brick's L1 for its fitted boards, then hand
 * it to each node on the link its slot names. The brick's one console is
 * the serial line of the module's first node; the other nodes' L1 lines
 * stay unused. The Cx-brick's type character is unknown, so the L1 reports
 * one only when the catalogue's controller record supplies it, as a stated
 * approximation.
 */
static bool sgi_machine_supply_cx_l1(SGIMachineState *s,
                                     const SGIModelTopology *t,
                                     const SGIModelController *c,
                                     const SGIModelModule *m,
                                     DeviceState *l1, Error **errp)
{
    g_autofree const SGIModelNode **nodes = NULL;
    const uint32List *entry;
    size_t node_count;
    unsigned int boards = 0, compute = 0;
    Chardev *console;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    for (entry = m->nodes; entry; entry = entry->next) {
        const SGIModelNode *n = nodes[entry->value];

        boards |= 1u << n->slot;
        if (!sgi_machine_node_is_memory(n)) {
            compute |= 1u << n->slot;
        }
    }
    qdev_prop_set_string(l1, "brick-type", "cxbrick");
    if (c->l1 && c->l1->has_reported_type) {
        qdev_prop_set_uint32(l1, "brick-type-code", c->l1->reported_type);
    }
    if (m->initial_module_number) {
        qdev_prop_set_uint32(l1, "bay", m->initial_module_number);
        qdev_prop_set_bit(l1, "named", true);
    }
    qdev_prop_set_uint32(l1, "boards", boards);
    qdev_prop_set_uint32(l1, "compute-boards", compute);
    if (c->l1 && c->l1->cpu_context) {
        qdev_prop_set_bit(l1, "cpu-context-present", true);
        qdev_prop_set_uint32(l1, "cpu-mode-word",
                             c->l1->cpu_context->cpu_mode_word);
        qdev_prop_set_uint64(l1, "cpu-clock-hz",
                             c->l1->cpu_context->cpu_clock_hz);
        qdev_prop_set_uint64(l1, "hub-clock-hz",
                             c->l1->cpu_context->hub_clock_hz);
    }
    sgi_machine_set_module_serial(s, m->controller_index, l1);
    console = serial_hd(sgi_sn1_serial_line(m->nodes->value, 0));
    if (console) {
        qdev_prop_set_chr(l1, "chardev", console);
    }
    if (!qdev_realize(l1, NULL, errp)) {
        return false;
    }
    for (entry = m->nodes; entry; entry = entry->next) {
        if (!sn1_node_supply_l1(SN1_NODE(s->bedrock_nodes[entry->value]), l1,
                                nodes[entry->value]->slot, errp)) {
            return false;
        }
    }
    return true;
}

/*
 * Create each module-owned L1 as /machine/module<M>/l1. A one-node
 * module's goes unrealized to its node, which configures and realizes it
 * as it would its own. A Cx-brick's is built here and shared by its
 * nodes. Validation left only these two shapes.
 */
static bool sgi_machine_supply_module_l1s(SGIMachineState *s,
                                          const SGIModelTopology *t,
                                          Error **errp)
{
    g_autofree const SGIModelModule **modules = NULL;
    const SGIModelControllerList *entry;
    size_t module_count;

    modules = sgi_model_module_array(t->modules, &module_count);
    for (entry = t->controllers; entry; entry = entry->next) {
        const SGIModelController *c = entry->value;
        g_autofree char *module_name = NULL;
        const SGIModelModule *m;
        Object *container;
        DeviceState *l1;

        if (!sgi_machine_controller_module_owned(c)) {
            continue;
        }
        g_assert(c->module_index < module_count);
        m = modules[c->module_index];
        g_assert(m->nodes && m->nodes->value < s->num_nodes);
        module_name = g_strdup_printf("module%u", c->module_index);
        container = object_resolve_path_component(OBJECT(s), module_name);
        g_assert(container);
        l1 = qdev_new(TYPE_SGI_L1);
        object_property_add_child(container, "l1", OBJECT(l1));
        object_unref(OBJECT(l1));
        if (sgi_machine_controller_cx(c)) {
            if (!sgi_machine_supply_cx_l1(s, t, c, m, l1, errp)) {
                return false;
            }
            continue;
        }
        g_assert(QAPI_LIST_LENGTH(m->nodes) == 1);
        if (!sn1_node_supply_l1(SN1_NODE(s->bedrock_nodes[m->nodes->value]),
                                l1, 0, errp)) {
            return false;
        }
    }
    return true;
}

/* The XIO cable whose far end is board @board_index, or NULL. */
static const SGIModelXioCable *sgi_machine_board_cable(
    const SGIModelTopology *t, unsigned int board_index)
{
    const SGIModelXioCableList *entry;

    for (entry = t->xio_cables; entry; entry = entry->next) {
        const SGIModelXioEndList *end;

        for (end = entry->value->ends; end; end = end->next) {
            if (end->value->kind == SGI_MODEL_XIO_END_KIND_IO_BOARD &&
                end->value->index == board_index) {
                return entry->value;
            }
        }
    }
    return NULL;
}

/*
 * Build the XIO cables that do not end on a node. Every node has
 * realized, and with it each crossbar a node hosts.
 *
 * A cable between two crossbars was validated but is not built. The
 * XBridge model takes one host port, so a second link from one chip
 * would arrive on the other as an ordinary external port, and building
 * either would pick a downstream mode no contract gives (108-0240-002
 * Figure 2-24 joins a P-brick's chips by two links each).
 *
 * A cable to a board becomes one XIO link from the crossbar port to the
 * board, connected before and after the board realizes. Validation put
 * the board's own attachment at that port, so the preset's builder
 * places the board by it, and the hosting node's Bedrock already decodes
 * that widget.
 */
static bool sgi_machine_build_board_cables(SGIMachineState *s,
                                           const SGIModelTopology *t,
                                           Error **errp)
{
    g_autofree const SGIModelIoBoard **boards = NULL;
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    const SGIModelXioCableList *entry;
    size_t board_count, attachment_count, i;

    for (entry = t->xio_cables; entry; entry = entry->next) {
        const SGIModelXioEndList *ends = entry->value->ends;

        if (ends->value->kind == SGI_MODEL_XIO_END_KIND_CROSSBAR &&
            ends->next->value->kind == SGI_MODEL_XIO_END_KIND_CROSSBAR) {
            error_setg(errp, "topology '%s' XIO cable '%s': no reviewed "
                       "contract for chained crossbars", t->name,
                       entry->value->name);
            return false;
        }
    }

    /* The middle PIC realized with its node; its heads' INTx wire now. */
    if (s->enclosure_pic) {
        sgi_machine_wire_enclosure_pic_intx(SGI_PIC(s->enclosure_pic));
    }
    boards = sgi_model_io_board_array(t->io_boards, &board_count);
    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                 &attachment_count);
    for (i = 0; i < board_count; i++) {
        if (!sgi_machine_board_cable(t, i)) {
            continue;
        }
        g_assert(boards[i]->attachment < attachment_count);
        if (!sgi_machine_instantiate_io_board(
                s, t, boards[i], i, attachments[boards[i]->attachment],
                errp)) {
            return false;
        }
    }
    return true;
}

static bool sgi_machine_create_pic_hosts(SGIMachineState *s,
                                         const SGIModelTopology *t,
                                         Error **errp)
{
    g_autofree const SGIModelPciHostBridge **hosts = NULL;
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    size_t host_count, attachment_count, i;

    hosts = sgi_model_pci_host_bridge_array(t->pci_host_bridges, &host_count);
    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                  &attachment_count);
    s->pic_hosts = g_new0(DeviceState *, host_count);
    s->pic_links = g_new0(SGIXioLink, host_count);
    s->pic_host_count = host_count;

    for (i = 0; i < host_count; i++) {
        const SGIModelPciHostBridge *host = hosts[i];
        SGIPICState *pic;
        g_autofree char *child_name = NULL;
        const SGIModelIoBoardList *board_entry;
        uint8_t req64_slots[2] = { 0 };

        if (host->xio_attachment_index >= attachment_count) {
            error_setg(errp, "PCI host '%s' names an invalid XIO attachment",
                       host->id);
            return false;
        }
        pic = SGI_PIC(qdev_new(TYPE_SGI_PIC));
        child_name = g_strdup_printf("pic-%s", host->id);

        object_property_add_child(OBJECT(s), child_name, OBJECT(pic));
        object_unref(OBJECT(pic));
        if (s->ip53_experiment_authorized) {
            const SGIModelXioAttachment *attachment =
                attachments[host->xio_attachment_index];

            qdev_prop_set_enum(DEVICE(pic), "compatibility-profile",
                               host->compatibility_profile);
            qdev_prop_set_bit(DEVICE(pic), "test-fixture", false);
            qdev_prop_set_uint8(DEVICE(pic), "pxbow-host-widget", 10);
            qdev_prop_set_uint8(DEVICE(pic), "pxbow-pic-widget",
                                attachment->widget);
            for (board_entry = t->io_boards; board_entry;
                 board_entry = board_entry->next) {
                const SGIModelIoBoard *board = board_entry->value;
                const SGIModelPciFunctionList *function_entry;

                if (!board->populated ||
                    board->preset != SGI_MODEL_IO_BOARD_PRESET_IO9 ||
                    board->pci_host_index != i || board->pci_head >= 2) {
                    continue;
                }
                for (function_entry = board->pci_functions; function_entry;
                     function_entry = function_entry->next) {
                    const SGIModelPciFunction *function =
                        function_entry->value;

                    if (function->kind ==
                        SGI_MODEL_PCI_FUNCTION_KIND_BCM5701) {
                        /* IO9 REQ64 wiring is an experimental assumption. */
                        req64_slots[board->pci_head] |=
                            BIT(function->pci_device);
                    }
                }
            }
            qdev_prop_set_uint8(DEVICE(pic), "req64-slots-head0",
                                req64_slots[0]);
            qdev_prop_set_uint8(DEVICE(pic), "req64-slots-head1",
                                req64_slots[1]);
            if (!sgi_pic_enable_pxbow_experiment(
                    pic, SGI_BEDROCK_IO_PIO_SIZE, errp)) {
                return false;
            }
        } else {
            qdev_prop_set_bit(DEVICE(pic), "test-fixture", true);
            qdev_prop_set_bit(DEVICE(pic), "linked-transport", true);
            qdev_prop_set_bit(DEVICE(pic), "bedrock-interrupts", true);
        }
        s->pic_hosts[i] = DEVICE(pic);
    }
    return true;
}

static bool sgi_machine_set_pic_widget_views(SGIMachineState *s,
                                             const SGIModelTopology *t,
                                             unsigned int node_index,
                                             Error **errp)
{
    g_autofree const SGIModelPciHostBridge **hosts = NULL;
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    size_t host_count, attachment_count, i;
    size_t widget_zero_host = G_MAXSIZE;
    SN1NodeState *node = SN1_NODE(s->bedrock_nodes[node_index]);

    hosts = sgi_model_pci_host_bridge_array(t->pci_host_bridges, &host_count);
    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                  &attachment_count);
    if (s->ip53_experiment_authorized) {
        const SGIModelIoBoardList *board_entry;

        for (board_entry = t->io_boards; board_entry;
             board_entry = board_entry->next) {
            const SGIModelIoBoard *board = board_entry->value;

            if (board->preset == SGI_MODEL_IO_BOARD_PRESET_IO9 &&
                board->has_pci_host_index) {
                widget_zero_host = board->pci_host_index;
                break;
            }
        }
        if (widget_zero_host >= host_count) {
            error_setg(errp, "IP53 widget-zero view has no IO9 host");
            return false;
        }
        s->ip53_widget_zero_host = widget_zero_host;
        s->ip53_widget_zero_host_valid = true;
    }
    for (i = 0; i < host_count; i++) {
        const SGIModelPciHostBridge *host = hosts[i];
        const SGIModelXioAttachment *attachment =
            attachments[host->xio_attachment_index];
        MemoryRegion *face;

        if (attachment->node_index != node_index) {
            continue;
        }
        if (s->ip53_experiment_authorized) {
            SGIPICState *pic = SGI_PIC(s->pic_hosts[i]);
            MemoryRegion *widget_zero = sgi_pic_pxbow_widget_zero(pic);
            MemoryRegion *routed = sgi_pic_routed_widget(
                pic, attachment->widget, errp);

            if (!routed) {
                return false;
            }
            if ((i == widget_zero_host &&
                 !sn1_node_set_ip53_pic_widget(node, 0, widget_zero, errp)) ||
                !sn1_node_set_ip53_pic_widget(node, attachment->widget,
                                               routed, errp)) {
                return false;
            }
            continue;
        }
        face = sgi_xio_port_cpu_face(OBJECT(s->pic_hosts[i]), 0);
        if (!sn1_node_set_ip53_pic_widget(node, attachment->widget, face,
                                          errp)) {
            return false;
        }
    }
    return true;
}

/*
 * The DS1742 data and clock backings for IO9 board @board, as names the
 * ds1742's drive properties take: its ds1742-<id>-data and ds1742-<id>-clock
 * storage items, or the deprecated -drive if=none pair
 * sgi-ds1742-data-<id> and sgi-ds1742-clock-<id>. Refuses a store backed
 * both ways and a pair with a half missing.
 */
static bool sgi_machine_ds1742_backing(SGIMachineState *s,
                                       const SGIModelIoBoard *board,
                                       const char **data, const char **clock,
                                       Error **errp)
{
    const char *product = MACHINE_GET_CLASS(s)->name;
    g_autofree char *name = g_strdup_printf("ds1742-%s-data", board->id);
    g_autofree char *clock_name = g_strdup_printf("ds1742-%s-clock",
                                                  board->id);
    g_autofree char *data_id = g_strdup_printf("sgi-ds1742-data-%s",
                                               board->id);
    g_autofree char *clock_id = g_strdup_printf("sgi-ds1742-clock-%s",
                                                board->id);
    const char *node = sgi_machine_storage_node(s, name);
    const char *clock_node = sgi_machine_storage_node(s, clock_name);
    BlockBackend *legacy = blk_by_name(data_id);
    BlockBackend *legacy_clock = blk_by_name(clock_id);

    if ((node || clock_node) && (legacy || legacy_clock)) {
        error_setg(errp, "%s: %s and -drive id=%s both back IO9 board '%s''s "
                   "DS1742", product, node ? name : clock_name,
                   legacy ? data_id : clock_id, board->id);
        return false;
    }
    if (node || clock_node) {
        if (!node || !clock_node) {
            error_setg(errp, "%s: IO9 board '%s' needs both %s and %s",
                       product, board->id, name, clock_name);
            return false;
        }
        *data = node;
        *clock = clock_node;
        return true;
    }
    if (!legacy || !legacy_clock) {
        error_setg(errp, "%s: IO9 board '%s' needs its DS1742 stores %s and "
                   "%s, or the deprecated -drive ids '%s' and '%s'", product,
                   board->id, name, clock_name, data_id, clock_id);
        return false;
    }
    *data = blk_name(legacy);
    *clock = blk_name(legacy_clock);
    return true;
}

/*
 * The IO9's BCM5701 takes its -nic backend and the board's station
 * address: mac=, or the shared SGI default, plus the onboard Ethernets
 * before it, as the IOC3 boards do. A -nic that names a different
 * address is refused rather than leaving the board's identity and the
 * NIC disagreeing; one whose address QEMU generated takes the board's.
 */
static bool sgi_machine_io9_ethernet(SGIMachineState *s,
                                     const SGIModelTopology *t,
                                     const SGIModelIoBoard *board,
                                     DeviceState *nic, Error **errp)
{
    const SGIModelIoBoardList *entry = t->io_boards;
    NICInfo *nd = qemu_find_nic_info(object_get_typename(OBJECT(nic)),
                                     true, NULL);
    unsigned int board_index = 0;
    g_autofree char *mac = NULL;
    MACAddr address;

    for (; entry && entry->value != board; entry = entry->next) {
        board_index++;
    }
    mac = sgi_machine_onboard_mac(s, t, board_index, errp);
    if (!mac) {
        return false;
    }
    if (net_parse_macaddr(address.a, mac) < 0) {
        error_setg(errp, "IO9 board '%s' has no valid station address",
                   board->id);
        return false;
    }
    if (nd && !nd->mac_generated &&
        memcmp(nd->macaddr.a, address.a, sizeof(address.a))) {
        error_setg(errp, "IO9 board '%s' NIC MAC conflicts with board "
                   "identity %s; set the machine's mac= instead", board->id,
                   mac);
        return false;
    }
    qemu_configure_nic_device(nic, true, NULL);
    qdev_prop_set_macaddr(nic, "mac", address.a);
    return true;
}

static bool sgi_machine_build_pic_io9(SGIMachineState *s,
                                      const SGIModelTopology *t,
                                      Error **errp)
{
    g_autofree const SGIModelPciHostBridge **hosts = NULL;
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    g_autofree const SGIModelNode **nodes = NULL;
    const SGIModelIoBoardList *board_entry;
    size_t host_count, attachment_count, node_count, i;

    hosts = sgi_model_pci_host_bridge_array(t->pci_host_bridges, &host_count);
    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                  &attachment_count);
    nodes = sgi_model_node_array(t->nodes, &node_count);
    if (s->ip53_experiment_authorized &&
        host_count > SGI_BEDROCK_PI_LEVEL_SOURCES / SGI_PIC_LEVEL_SOURCES) {
        error_setg(errp, "IP53 PIC count exceeds the supported interrupt "
                   "source count");
        return false;
    }

    for (i = 0; i < host_count; i++) {
        const SGIModelPciHostBridge *host = hosts[i];
        const SGIModelXioAttachment *attachment =
            attachments[host->xio_attachment_index];
        SN1NodeState *node = SN1_NODE(
            s->bedrock_nodes[attachment->node_index]);
        SGIBedrockIOState *bedrock_io = sn1_node_bedrock_io(node);
        SGIPICState *pic = SGI_PIC(s->pic_hosts[i]);
        g_autofree char *link_name = g_strdup_printf("pic-link-%s", host->id);

        if (!bedrock_io) {
            error_setg(errp, "PCI host '%s' has no realized Bedrock endpoint",
                       host->id);
            return false;
        }
        sgi_xio_link_init(&s->pic_links[i], OBJECT(s), link_name,
                          OBJECT(bedrock_io), 0, OBJECT(pic), 0);
        if (!sgi_xio_link_connect(&s->pic_links[i], errp) ||
            (s->ip53_experiment_authorized &&
             !sgi_pic_attach_pxbow_packet_endpoint(
                 pic, sgi_xio_port_packet_face(OBJECT(bedrock_io), 0),
                 errp)) ||
            !sysbus_realize(SYS_BUS_DEVICE(pic), errp) ||
            !sgi_xio_link_connect(&s->pic_links[i], errp)) {
            return false;
        }
        if (s->ip53_experiment_authorized) {
            SGIBedrockState *bedrock = SGI_BEDROCK(node->bedrock);

            sgi_pic_set_bedrock_pi(pic, 0, &bedrock->pi[0],
                                   i * SGI_PIC_LEVEL_SOURCES);
            sgi_pic_set_bedrock_pi(pic, 1, &bedrock->pi[1],
                                   i * SGI_PIC_LEVEL_SOURCES);
        }
    }

    for (board_entry = t->io_boards; board_entry;
         board_entry = board_entry->next) {
        const SGIModelIoBoard *board = board_entry->value;
        const SGIModelPciFunctionList *function_entry;
        DeviceState *rtc = NULL;
        MemoryRegion *rtc_clock = NULL;

        if (!board->populated ||
            board->preset != SGI_MODEL_IO_BOARD_PRESET_IO9) {
            continue;
        }
        if (board->system_timekeeper) {
            SGIModelSystemTimekeeper *policy = board->system_timekeeper;
            const char *data_name, *clock_name;
            g_autofree char *child_name = g_strdup_printf(
                "ds1744-%s", board->id);

            if ((board->has_ds1742 && board->ds1742) ||
                policy->kind != SGI_MODEL_SYSTEM_TIMEKEEPER_KIND_DS1744 ||
                policy->initial_ip27_fixture ||
                (policy->has_year_base && policy->year_base != 0 &&
                 policy->year_base != 1968)) {
                error_setg(errp, "IO9 board '%s' has an unsupported "
                           "system timekeeper policy", board->id);
                return false;
            }
            if (!sgi_machine_nvram_backing(s, t, board, NULL,
                                           &data_name, &clock_name, errp)) {
                return false;
            }
            if (!data_name || !clock_name) {
                error_setg(errp, "IO9 board '%s' requires its standard "
                           "Timekeeper NVRAM and clock stores", board->id);
                return false;
            }
            rtc = qdev_new("ds1744");
            object_property_add_child(OBJECT(s), child_name, OBJECT(rtc));
            object_unref(OBJECT(rtc));
            if ((policy->has_year_base &&
                 !object_property_set_uint(OBJECT(rtc), "fresh-year-base",
                                           policy->year_base, errp)) ||
                !object_property_set_str(OBJECT(rtc), "drive", data_name,
                                         errp) ||
                !object_property_set_str(OBJECT(rtc), "clock-drive",
                                         clock_name, errp) ||
                !sysbus_realize(SYS_BUS_DEVICE(rtc), errp)) {
                return false;
            }
            rtc_clock = sysbus_mmio_get_region(SYS_BUS_DEVICE(rtc), 0);
        }
        if (board->has_ds1742 && board->ds1742) {
            if (rtc) {
                error_setg(errp, "IO9 board '%s' has both legacy and "
                           "generic timekeeper selections", board->id);
                return false;
            }

            g_autofree char *child_name = g_strdup_printf(
                "ds1742-%s", board->id);
            const char *data, *clock;

            if (!sgi_machine_ds1742_backing(s, board, &data, &clock, errp)) {
                return false;
            }
            rtc = qdev_new("ds1742");
            object_property_add_child(OBJECT(s), child_name, OBJECT(rtc));
            object_unref(OBJECT(rtc));
            if (!object_property_set_uint(
                    OBJECT(rtc), "fresh-year-base",
                    board->has_ds1742_fresh_year_base ?
                    board->ds1742_fresh_year_base : 0, errp) ||
                !object_property_set_str(OBJECT(rtc), "drive", data, errp) ||
                !object_property_set_str(OBJECT(rtc), "clock-drive", clock,
                                         errp) ||
                !sysbus_realize(SYS_BUS_DEVICE(rtc), errp)) {
                return false;
            }
            rtc_clock = sysbus_mmio_get_region(SYS_BUS_DEVICE(rtc), 0);
        }
        for (function_entry = board->pci_functions; function_entry;
             function_entry = function_entry->next) {
            const SGIModelPciFunction *function = function_entry->value;
            SGIPICState *pic = SGI_PIC(s->pic_hosts[board->pci_host_index]);
            DeviceState *head = sgi_pic_head_device(pic, board->pci_head);
            PCIBus *bus = sgi_pic_pci_bus(pic, board->pci_head);
            PCIDevice *pci;
            const char *type;
            g_autofree char *child_name = NULL;
            unsigned int irq_line = function->pci_device * 4 +
                                    function->intx_pin;

            switch (function->kind) {
            case SGI_MODEL_PCI_FUNCTION_KIND_IOC4:
                type = "sgi-ioc4";
                break;
            case SGI_MODEL_PCI_FUNCTION_KIND_ISP12160:
                type = "qlogic-isp12160";
                break;
            case SGI_MODEL_PCI_FUNCTION_KIND_BCM5701:
                type = "bcm570x";
                break;
            default:
                error_setg(errp, "IO9 board '%s' has an unsupported PCI "
                           "function", board->id);
                return false;
            }
            child_name = g_strdup_printf("io9-%s-d%u-f%u", board->id,
                                         function->pci_device,
                                         function->pci_function);
            pci = PCI_DEVICE(qdev_new(type));
            object_property_add_child(OBJECT(s), child_name, OBJECT(pci));
            pci->devfn = PCI_DEVFN(function->pci_device,
                                   function->pci_function);
            if (function->kind == SGI_MODEL_PCI_FUNCTION_KIND_IOC4) {
                unsigned int node_index =
                    attachments[hosts[board->pci_host_index]->
                                xio_attachment_index]->node_index;
                unsigned int port;

                qdev_prop_set_uint32(DEVICE(pci), "clock-hz",
                                     function->clock_hz);
                qdev_prop_set_bit(DEVICE(pci), "serial-dma-experimental",
                                  function->serial_dma_experimental);
                qdev_prop_set_bit(
                    DEVICE(pci), "pci-status-compat",
                    nodes[node_index]->has_board_kind &&
                    nodes[node_index]->board_kind ==
                        SGI_MODEL_BOARD_KIND_IP53);
                qdev_prop_set_bit(
                    DEVICE(pci), "ata-readback-compat",
                    nodes[node_index]->has_board_kind &&
                    nodes[node_index]->board_kind ==
                        SGI_MODEL_BOARD_KIND_IP53);
                if (rtc_clock) {
                    object_property_set_link(OBJECT(pci), "clock",
                                             OBJECT(rtc_clock), &error_abort);
                }
                for (port = 0; port < 4; port++) {
                    static const char *const chardev_props[] = {
                        "chardev1", "chardev2", "chardev3", "chardev4",
                    };
                    Chardev *chr = serial_hd(
                        sgi_sn1_serial_line(node_index, port + 1));

                    qdev_prop_set_chr(DEVICE(pci), chardev_props[port], chr);
                }
            } else if (function->kind ==
                       SGI_MODEL_PCI_FUNCTION_KIND_BCM5701) {
                qdev_prop_set_string(DEVICE(pci), "model", "bcm5701");
                if (!sgi_machine_io9_ethernet(s, t, board, DEVICE(pci),
                                              errp)) {
                    return false;
                }
            }
            qdev_connect_gpio_out_named(head, "pci-intx", irq_line,
                                        qdev_get_gpio_in(head,
                                                         function->pic_input));
            if (!pci_realize_and_unref(pci, bus, errp)) {
                return false;
            }
        }
    }
    return true;
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
 * A module with a shared controller carries its MODULEID part and, where
 * the catalogue knows its midplane's record, its midplane part on the
 * midplane one-wire net the controller's nic line drives. Of the node
 * slots only n1 is wired to that net, so those parts answer on the n1
 * node's Hub bus and on no other; a module with n1 empty reaches them
 * through the controller alone. The midplane barcode is marked synthetic,
 * the midplane letter and the instance of the part holding it.
 */
static void sgi_machine_attach_module_parts(SGIMachineState *s,
                                            const SGIModelTopology *t,
                                            const SGIModelModule *module,
                                            SN0ModuleManagementState *mgmt)
{
    g_autofree const SGIModelNode **nodes = NULL;
    g_autofree unsigned int *module_nodes = NULL;
    char barcode[SGI_SYNTHETIC_BARCODE_SIZE];
    SGIBoardRecord midplane;
    size_t node_count, module_node_count, i;

    if (!module->has_identity_serial) {
        error_report("module '%s' has a shared controller but no MODULEID "
                     "part", module->id);
        exit(EXIT_FAILURE);
    }
    if (module->midplane_record) {
        sgi_synthetic_barcode(barcode, 'L', module->midplane_serial);
        midplane = (SGIBoardRecord) {
            barcode, module->midplane_record->part,
            module->midplane_record->revision, module->midplane_record->name,
        };
    }
    sgi_sn0_module_management_attach_parts(
        mgmt, module->serial, module->identity_serial,
        module->midplane_record ? &midplane : NULL, module->midplane_serial,
        &error_fatal);

    nodes = sgi_model_node_array(t->nodes, &node_count);
    module_nodes = sgi_model_array_uint32(module->nodes, &module_node_count);
    for (i = 0; i < module_node_count; i++) {
        if (nodes[module_nodes[i]]->slot == 1) {
            sgi_hub_attach_midplane_onewire(
                &s->nodes[module_nodes[i]]->hub,
                sgi_sn0_module_management_midplane_onewire(mgmt),
                &error_fatal);
        }
    }
}

/*
 * A module controller removed power, and the module's processors are
 * already stopped (sgi_machine_hold_node). Once every module is off with
 * no power-up to come -- no power-cycle countdown and no heartbeat
 * watchdog that will cycle it -- the machine powers down. A module off
 * while others run stays stopped.
 */
static void sgi_machine_module_power_bh(void *opaque)
{
    SGIMachineState *s = opaque;
    unsigned int i, total = 0, gone = 0;

    s->module_power_bh_pending = false;
    for (i = 0; i < s->module_power_count; i++) {
        SGIElscState *msc = s->module_mgmt[i]->msc;

        total++;
        if (!s->module_powered[i] && !sgi_elsc_power_return_pending(msc)) {
            gone++;
        }
    }
    for (i = 0; s->nodes && i < s->num_nodes; i++) {
        SGIElscState *elsc = s->nodes[i]->hub.elsc;

        if (!elsc) {
            continue;
        }
        total++;
        if (!sgi_elsc_module_powered(elsc) &&
            !sgi_elsc_power_return_pending(elsc)) {
            gone++;
        }
    }
    if (total && gone == total) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}

static void sgi_machine_schedule_power_bh(SGIMachineState *s)
{
    if (!s->module_power_bh_pending) {
        s->module_power_bh_pending = true;
        replay_bh_schedule_oneshot_event(qemu_get_aio_context(),
                                         sgi_machine_module_power_bh, s);
    }
}

/*
 * Stop a node's processors as its module loses power: each is reset and
 * halted with its Hub enable clear (sgi_hub_power_off_cpu). The enables
 * and the processors' state are migrated, and a controller that is off
 * says so again after a load, which holds the node again in case the
 * reset was still queued when the source stopped. The machine's reset
 * keeps the enables clear while the module stays unpowered
 * (sgi_machine_reset).
 */
static void sgi_machine_hold_node(SGIMachineState *s, unsigned int node)
{
    SN0NodeState *node_state = s->nodes[node];
    unsigned int slice;

    for (slice = 0; slice < node_state->nr_cpus; slice++) {
        sgi_hub_power_off_cpu(&node_state->hub, slice);
    }
}

/* Whether the module a node belongs to has power. */
static bool sgi_machine_node_powered(SGIMachineState *s, unsigned int node)
{
    SN0NodeState *node_state = s->nodes[node];

    if (node_state->hub.elsc) {
        return sgi_elsc_module_powered(node_state->hub.elsc);
    }
    if (s->node_controller && s->node_controller[node] >= 0) {
        return s->module_mgmt[s->node_controller[node]]->power_on;
    }
    return true;
}

/*
 * The topology module's own parts: the objects a module's power feeds and
 * its reset reaches, derived from the topology the machine built. The node
 * boards, the Routers, the Crossbows and the I/O boards the topology puts
 * in the module bring their whole QOM subtrees (a Hub's adapter and
 * identity parts, a Bridge and the PCI devices behind it), as does a board
 * a user plugged into one of the module's XIO slots.
 *
 * The module's controller runs on standby power and is in none of it.
 * Processors are not devices to reset from here; their power-up is
 * sgi_machine_module_start()'s.
 */
static bool sgi_machine_under_controller(Object *obj)
{
    for (; obj; obj = obj->parent) {
        if (object_dynamic_cast(obj, TYPE_SGI_ELSC) ||
            object_dynamic_cast(obj, TYPE_SGI_SN0_MODULE_MANAGEMENT)) {
            return true;
        }
    }
    return false;
}

static int sgi_machine_collect_child(Object *obj, void *opaque)
{
    GPtrArray *devices = opaque;

    if (object_dynamic_cast(obj, TYPE_DEVICE) &&
        !object_dynamic_cast(obj, TYPE_CPU) &&
        !sgi_machine_under_controller(obj)) {
        g_ptr_array_add(devices, obj);
    }
    return 0;
}

static void sgi_machine_collect_part(GPtrArray *devices, Object *part)
{
    if (!part) {
        return;
    }
    sgi_machine_collect_child(part, devices);
    object_child_foreach_recursive(part, sgi_machine_collect_child, devices);
}

/*
 * Parts to cold-reset. A part sitting on a bus of another part in the list
 * is reset with that part's bus, so only the roots stay. Consumes @all.
 */
static GPtrArray *sgi_machine_part_roots(GPtrArray *all)
{
    GPtrArray *roots = g_ptr_array_new();
    guint i;

    for (i = 0; i < all->len; i++) {
        DeviceState *dev = g_ptr_array_index(all, i);
        BusState *bus = qdev_get_parent_bus(dev);

        if (bus && bus->parent && g_ptr_array_find(all, bus->parent, NULL)) {
            continue;
        }
        if (!g_ptr_array_find(roots, dev, NULL)) {
            g_ptr_array_add(roots, dev);
        }
    }
    g_ptr_array_free(all, true);
    return roots;
}

/*
 * The module whose power a topology module follows. A module with nodes
 * follows itself. One with none, the GIGAchannel cabinet, has a switched
 * domain that "turns on only when the attached module turns on" (the
 * contract), and its Crossbow's XIO link names the node it is attached to.
 */
static unsigned int sgi_machine_domain_module(const SGIModelTopology *t,
                                              unsigned int declared)
{
    g_autofree const SGIModelModule **modules = NULL;
    g_autofree const SGIModelCrossbar **crossbars = NULL;
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    g_autofree const SGIModelNode **nodes = NULL;
    size_t module_count, crossbar_count, attachment_count, node_count, c, a;

    modules = sgi_model_module_array(t->modules, &module_count);
    if (declared >= module_count || modules[declared]->nodes) {
        return declared;
    }
    crossbars = sgi_model_crossbar_array(t->crossbars, &crossbar_count);
    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                 &attachment_count);
    nodes = sgi_model_node_array(t->nodes, &node_count);
    for (c = 0; c < crossbar_count; c++) {
        if (crossbars[c]->module_index != declared) {
            continue;
        }
        for (a = 0; a < attachment_count; a++) {
            if (attachments[a]->crossbar_index == (int64_t)c &&
                attachments[a]->node_index < node_count) {
                return nodes[attachments[a]->node_index]->module_index;
            }
        }
    }
    return declared;
}

/* The module's parts, as devices to cold-reset. */
static GPtrArray *sgi_machine_module_parts(SGIMachineState *s,
                                           unsigned int module)
{
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));
    GPtrArray *all = g_ptr_array_new();
    g_autofree const SGIModelModule **modules = NULL;
    g_autofree const SGIModelCrossbar **crossbars = NULL;
    g_autofree const SGIModelRouter **routers = NULL;
    g_autofree const SGIModelIoBoard **boards = NULL;
    g_autofree const SGIModelXioSlot **slots = NULL;
    g_autofree uint32_t *module_nodes = NULL;
    size_t count, node_count, i;

    modules = sgi_model_module_array(t->modules, &count);
    g_assert(module < count);
    module_nodes = sgi_model_array_uint32(modules[module]->nodes, &node_count);
    for (i = 0; i < node_count; i++) {
        SN0NodeState *node = s->nodes[module_nodes[i]];
        unsigned int adapter;

        sgi_machine_collect_part(all, OBJECT(node));
        sgi_machine_collect_part(all, OBJECT(node->flash));
        if (s->module_mgmt && s->node_controller[module_nodes[i]] >= 0) {
            SN0ModuleManagementState *mgmt =
                s->module_mgmt[s->node_controller[module_nodes[i]]];

            /* The shared controller's adapters are the nodes' own. */
            for (adapter = 0; adapter < SGI_O2000_ADAPTER_COUNT; adapter++) {
                DeviceState *dev =
                    sgi_sn0_module_management_get_adapter(mgmt, adapter);

                if (dev && !g_ptr_array_find(all, dev, NULL)) {
                    g_ptr_array_add(all, dev);
                }
            }
        }
    }

    crossbars = sgi_model_crossbar_array(t->crossbars, &count);
    for (i = 0; i < count; i++) {
        if (sgi_machine_domain_module(t, crossbars[i]->module_index) ==
            module && s->crossbars[i]) {
            sgi_machine_collect_part(all, OBJECT(s->crossbars[i]));
        }
    }
    routers = sgi_model_router_array(t->routers, &count);
    for (i = 0; i < count; i++) {
        if (sgi_machine_domain_module(t, routers[i]->module_index) ==
            module && s->routers[i]) {
            sgi_machine_collect_part(all, OBJECT(s->routers[i]));
        }
    }
    boards = sgi_model_io_board_array(t->io_boards, &count);
    for (i = 0; i < count; i++) {
        if (sgi_machine_domain_module(t, boards[i]->module_index) ==
            module && s->io_boards[i]) {
            sgi_machine_collect_part(all, OBJECT(s->io_boards[i]));
        }
    }
    slots = sgi_model_xio_slot_array(t->xio_slots, &count);
    for (i = 0; i < count; i++) {
        if (sgi_machine_domain_module(
                t, sgi_machine_xio_slot_module(t, slots[i])) == module &&
            s->slot_claimed[i]) {
            sgi_machine_collect_part(all, s->slot_links[i].end[1].port);
        }
    }

    return sgi_machine_part_roots(all);
}

/*
 * Power to a module comes or goes: its Hubs and the I/O windows they host
 * switch on or off, and so do its Routers (a cabinet's parts follow the node
 * they are attached to). A node or Router without power answers nothing and
 * its links read down at their peers (sgi_hub_set_power(),
 * sgi_router_set_power()).
 */
static void sgi_machine_set_module_power(SGIMachineState *s,
                                         unsigned int module, bool on)
{
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));
    g_autofree const SGIModelModule **modules = NULL;
    g_autofree const SGIModelRouter **routers = NULL;
    g_autofree uint32_t *module_nodes = NULL;
    size_t count, node_count, i;

    modules = sgi_model_module_array(t->modules, &count);
    module_nodes = sgi_model_array_uint32(modules[module]->nodes, &node_count);
    /*
     * Each node gates seventeen windows, and every commit renders the flat
     * view of every address space in the machine, so one transaction defers
     * them to a single commit. Switching power only gates regions and
     * re-derives link state; nothing here accesses an address space.
     */
    memory_region_transaction_begin();
    for (i = 0; i < node_count; i++) {
        sn0_node_set_power(s->nodes[module_nodes[i]], on);
    }
    if (s->routers) {
        routers = sgi_model_router_array(t->routers, &count);
        for (i = 0; i < count; i++) {
            if (s->routers[i] &&
                sgi_machine_domain_module(t, routers[i]->module_index) ==
                module) {
                sgi_router_set_power(s->routers[i], on);
            }
        }
    }
    memory_region_transaction_commit();
}

/*
 * Bring every module's parts in line with its controller's power. Power
 * is derived state, not migrated, so this runs wherever it could have been
 * lost: after the machine's own reset and when a machine starts running,
 * which includes the end of a load.
 */
static void sgi_machine_sync_power(SGIMachineState *s)
{
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));
    g_autofree const SGIModelModule **modules = NULL;
    size_t count, module, i;

    /* A machine without Hubs (the builder's test boards) has no power. */
    if (!s->nodes) {
        return;
    }
    modules = sgi_model_module_array(t->modules, &count);
    for (module = 0; module < count; module++) {
        g_autofree uint32_t *module_nodes = NULL;
        size_t node_count;

        module_nodes = sgi_model_array_uint32(modules[module]->nodes,
                                              &node_count);
        for (i = 0; i < node_count; i++) {
            if (s->nodes[module_nodes[i]]->hub.elsc ||
                (s->node_controller &&
                 s->node_controller[module_nodes[i]] >= 0)) {
                sgi_machine_set_module_power(
                    s, module, sgi_machine_node_powered(s, module_nodes[i]));
                break;
            }
        }
    }
}

/*
 * A cold start under way. The processors stop first; the start waits for
 * them without blocking, so it cannot deadlock under record/replay, where
 * the processor thread needs the lock the main loop holds. Work is queued
 * on each processor behind its power-off (async_run_on_cpu(), as
 * arm_set_cpu_off() and arm_set_cpu_on() queue theirs) and the last of the
 * completions schedules the rest of the start as a bottom half, through the
 * replay-aware helper so it lands at a reproducible point.
 */
typedef struct SGIMachineRestart {
    SGIMachineState *machine;
    SGIMachineModuleStart *start;
    uint32_t *nodes;
    size_t node_count;
    GPtrArray *parts;
    int power_module;
    /* Processors still to stop, plus one for the loop that queues them. */
    int outstanding;
} SGIMachineRestart;

/*
 * The rest of a cold start: the @parts reset, and the processors run again
 * if the node's module has power when the start gets here. Nothing else is
 * touched, and the controller, which runs on standby power, is reset by
 * nothing that this does (the Origin 200 controller sits on its Hub's
 * adapter bus and rides through that adapter's reset).
 */
static void sgi_machine_restart_finish_bh(void *opaque)
{
    SGIMachineRestart *r = opaque;
    SGIMachineState *s = r->machine;
    size_t i;
    unsigned int slice;

    /*
     * The debt this start pays is what migrates, so it is cleared here, not
     * when the start began. A start owed twice, as a resumed machine may
     * ask, or one the machine's own reset has made, finds it paid.
     */
    if (!r->start->pending) {
        goto out;
    }
    r->start->pending = false;
    trace_sgi_machine_module_start(r->start->module,
                                   r->start->module >= s->module_count);
    for (i = 0; i < r->node_count; i++) {
        SN0NodeState *node = s->nodes[r->nodes[i]];

        if (node->hub.elsc) {
            sgi_elsc_ride_through_reset(node->hub.elsc, true);
        }
    }
    for (i = 0; i < r->parts->len; i++) {
        device_cold_reset(g_ptr_array_index(r->parts, i));
    }
    for (i = 0; i < r->node_count; i++) {
        SN0NodeState *node = s->nodes[r->nodes[i]];

        if (node->hub.elsc) {
            sgi_elsc_ride_through_reset(node->hub.elsc, false);
        }
        for (slice = 0; slice < node->nr_cpus; slice++) {
            sn_promdata_reset(&node->promdata[slice]);
        }
    }

    /*
     * A module coming back from a power-off answers again before its
     * processors run, with the parts it has just reset.
     */
    if (r->power_module >= 0 &&
        sgi_machine_node_powered(s, r->nodes[0])) {
        sgi_machine_set_module_power(s, r->power_module, true);
    }
    for (i = 0; i < r->node_count; i++) {
        SN0NodeState *node = s->nodes[r->nodes[i]];

        if (!sgi_machine_node_powered(s, r->nodes[i])) {
            continue;
        }
        for (slice = 0; slice < node->nr_cpus; slice++) {
            sgi_hub_set_cpu_enable(&node->hub, slice, true, true);
        }
    }
out:
    g_ptr_array_unref(r->parts);
    g_free(r->nodes);
    g_free(r);
}

/*
 * Runs on a processor, queued behind its power-off: the last one to stop
 * lets the start go on.
 */
static void sgi_machine_cpu_settled(CPUState *cs, run_on_cpu_data data)
{
    SGIMachineRestart *r = data.host_ptr;

    if (qatomic_fetch_dec(&r->outstanding) != 1) {
        return;
    }
    /*
     * Queued work runs between executions, where the replay lock is not
     * held, and the event that carries the rest of the start needs it.
     * The lock is taken before the BQL, as rr_cpu_thread_fn() takes it.
     */
    if (replay_mode != REPLAY_MODE_NONE) {
        bql_unlock();
        replay_mutex_lock();
        bql_lock();
    }
    replay_bh_schedule_oneshot_event(qemu_get_aio_context(),
                                     sgi_machine_restart_finish_bh, r);
    if (replay_mode != REPLAY_MODE_NONE) {
        replay_mutex_unlock();
    }
}

/*
 * Cold-start nodes: stop their processors and, once every one has stopped,
 * reset the @parts and run the processors again (above). This is the main
 * loop's and never waits for a processor. It takes its own copies of the
 * node list and a reference to @parts.
 */
static void sgi_machine_restart_nodes(SGIMachineState *s,
                                      SGIMachineModuleStart *start,
                                      const uint32_t *node_list,
                                      size_t node_count, GPtrArray *parts,
                                      int power_module)
{
    SGIMachineRestart *r = g_new0(SGIMachineRestart, 1);
    size_t i;
    unsigned int slice;

    r->machine = s;
    r->start = start;
    r->nodes = g_memdup2(node_list, node_count * sizeof(*node_list));
    r->node_count = node_count;
    r->parts = g_ptr_array_ref(parts);
    r->power_module = power_module;
    r->outstanding = 1;

    for (i = 0; i < node_count; i++) {
        SN0NodeState *node = s->nodes[node_list[i]];

        for (slice = 0; slice < node->nr_cpus; slice++) {
            sgi_hub_power_off_cpu(&node->hub, slice);
            /* A processor running no guest code has stopped already. */
            if (!sgi_hub_cpu_paused(&node->hub, slice)) {
                qatomic_inc(&r->outstanding);
                async_run_on_cpu(CPU(node->cpu[slice]),
                                 sgi_machine_cpu_settled,
                                 RUN_ON_CPU_HOST_PTR(r));
            }
        }
    }
    /*
     * With nothing to wait for the start goes on here, so a paused machine
     * and a qtest see it done when the request returns, as they always did.
     */
    if (qatomic_fetch_dec(&r->outstanding) == 1) {
        sgi_machine_restart_finish_bh(r);
    }
}

/*
 * A start owed to a module (its index below module_count) or to the whole
 * machine (module_count): every module that has power starts together, so
 * that none comes up and numbers a neighbour that is about to lose its state.
 */
static void sgi_machine_module_start_bh(void *opaque)
{
    SGIMachineModuleStart *start = opaque;
    SGIMachineState *s = start->machine;
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));
    g_autofree const SGIModelModule **modules = NULL;
    g_autofree uint32_t *module_nodes = NULL;
    g_autoptr(GPtrArray) parts = NULL;
    size_t count;

    /* A start owed twice, as a resumed machine may ask, runs once. */
    if (!start->pending) {
        return;
    }
    modules = sgi_model_module_array(t->modules, &count);
    if (start->module >= s->module_count) {
        g_autofree uint32_t *nodes = g_new(uint32_t, s->num_nodes);
        size_t module, n = 0, i;

        parts = g_ptr_array_new();
        for (module = 0; module < count; module++) {
            g_autofree uint32_t *list = NULL;
            g_autoptr(GPtrArray) roots = NULL;
            size_t node_count;

            list = sgi_model_array_uint32(modules[module]->nodes,
                                          &node_count);
            if (!node_count || !sgi_machine_node_powered(s, list[0])) {
                continue;
            }
            for (i = 0; i < node_count; i++) {
                nodes[n++] = list[i];
            }
            roots = sgi_machine_module_parts(s, module);
            for (guint r = 0; r < roots->len; r++) {
                if (!g_ptr_array_find(parts, g_ptr_array_index(roots, r),
                                      NULL)) {
                    g_ptr_array_add(parts, g_ptr_array_index(roots, r));
                }
            }
        }
        sgi_machine_restart_nodes(s, start, nodes, n, parts, -1);
        return;
    }
    module_nodes = sgi_model_array_uint32(modules[start->module]->nodes,
                                          &count);
    parts = sgi_machine_module_parts(s, start->module);
    sgi_machine_restart_nodes(s, start, module_nodes, count, parts,
                              start->module);
}

/*
 * A module starts from its cold state: it came back from a power-off, or
 * its controller reset it. The whole machine starts when a Hub's port reset
 * register is written with bit 0 set. The start is the main loop's (above);
 * the machine's own reset, an AC cycle, starts everything already.
 */
static void sgi_machine_module_start(SGIMachineState *s, unsigned int module)
{
    SGIMachineModuleStart *start = &s->module_start[module];

    if (s->ac_cycle || start->pending) {
        return;
    }
    start->pending = true;
    replay_bh_schedule_oneshot_event(qemu_get_aio_context(),
                                     sgi_machine_module_start_bh, start);
}

static const VMStateDescription vmstate_sgi_machine_module_start = {
    .name = "sgi-machine/module-start",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(pending, SGIMachineModuleStart),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * Power is derived from the controllers' state, which this section loads
 * after: it is registered when the machine is done, so after every device,
 * and sections load in registration order. A destination that is paused after
 * the load already shows its modules gated as the stream had them.
 */
static int sgi_machine_module_starts_post_load(void *opaque, int version_id)
{
    sgi_machine_sync_power(opaque);
    return 0;
}

static const VMStateDescription vmstate_sgi_machine_module_starts = {
    .name = "sgi-machine/module-starts",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sgi_machine_module_starts_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(module_start, SGIMachineState,
                                             start_count,
                                             vmstate_sgi_machine_module_start,
                                             SGIMachineModuleStart),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * A start owed when the stream was taken has not run. The destination asks
 * for it once it runs, not while it loads, where it would reset devices
 * that later sections load over.
 */
static void sgi_machine_module_start_vm_state(void *opaque, bool running,
                                              RunState state)
{
    SGIMachineState *s = opaque;
    unsigned int module;

    if (!running) {
        return;
    }
    sgi_machine_sync_power(s);
    for (module = 0; module < s->start_count; module++) {
        if (s->module_start[module].pending) {
            replay_bh_schedule_oneshot_event(qemu_get_aio_context(),
                                             sgi_machine_module_start_bh,
                                             &s->module_start[module]);
        }
    }
}

/*
 * A Hub's port reset register was written with bit 0 set. Hardware describes
 * this as resetting the node; the model restarts the whole machine, every
 * powered module together, as the standby controllers keep their state. The
 * processors stop here, in the writer's context, so none runs past the write.
 */
static void sgi_machine_restart(void *opaque, int n, int level)
{
    SGIMachineState *s = opaque;
    unsigned int node, slice;

    if (!level) {
        return;
    }
    for (node = 0; node < s->num_nodes; node++) {
        if (!sgi_machine_node_powered(s, node)) {
            continue;
        }
        for (slice = 0; slice < s->nodes[node]->nr_cpus; slice++) {
            sgi_hub_power_off_cpu(&s->nodes[node]->hub, slice);
        }
    }
    sgi_machine_module_start(s, s->module_count);
}

/* An Origin 200 node's own controller removed or restored its power. */
static void sgi_machine_elsc_power(void *opaque, int n, int level)
{
    SGIMachineState *s = opaque;

    if (!level) {
        sgi_machine_hold_node(s, n);
        sgi_machine_set_module_power(s, s->node_module[n], false);
        sgi_machine_schedule_power_bh(s);
    } else {
        sgi_machine_module_start(s, s->node_module[n]);
    }
}

/*
 * An Origin 2000 controller says again that its module is off, as a
 * cancelled power-up leaves nothing to bring it back: only the machine's
 * decision to end is wanted.
 */
static void sgi_machine_module_recheck(void *opaque, int n, int level)
{
    if (!level) {
        sgi_machine_schedule_power_bh(opaque);
    }
}

/* An Origin 200 node's controller reset its module. */
static void sgi_machine_elsc_reset(void *opaque, int n, int level)
{
    SGIMachineState *s = opaque;

    if (level) {
        sgi_machine_module_start(s, s->node_module[n]);
    }
}

/*
 * An Origin 2000 module controller's power output moved. A fall holds the
 * module's processors and may power the machine down (the bottom half). A
 * return is a cold start of that module, whatever the machine's other
 * modules are doing.
 */
static void sgi_machine_module_power(void *opaque, int n, int level)
{
    SGIMachineState *s = opaque;
    bool was = s->module_powered[n];

    s->module_powered[n] = level;
    if (!level) {
        unsigned int node;

        for (node = 0; node < s->num_nodes; node++) {
            if (s->node_controller[node] == n) {
                sgi_machine_hold_node(s, node);
            }
        }
        sgi_machine_set_module_power(s, s->controller_module[n], false);
        if (was) {
            sgi_machine_schedule_power_bh(s);
        }
    } else if (!was && !s->module_mgmt[n]->resyncing) {
        /*
         * A load that finds the module on drives the line high again: the
         * stream holds the module's state, which a start would wipe.
         */
        sgi_machine_module_start(s, s->controller_module[n]);
    }
}

/*
 * A module reset holds every node in the module in reset, and nothing
 * else: a rack of several modules resets only the one whose controller
 * pulsed it.
 */
static void sgi_machine_module_reset(void *opaque, int n, int level)
{
    SGIMachineState *s = opaque;

    if (level && !s->module_mgmt[n]->resyncing) {
        sgi_machine_module_start(s, s->controller_module[n]);
    }
}

/*
 * The module controller's NMI output reaches every processor on every node
 * of its module, straight to each processor's NMI pin; nothing between the
 * midplane line and the pin is modelled.
 */
static void sgi_machine_connect_module_nmi(SGIMachineState *s,
                                           DeviceState *module,
                                           const unsigned int *nodes,
                                           size_t node_count)
{
    DeviceState *split = qdev_new(TYPE_SPLIT_IRQ);
    unsigned int lines = 0, line = 0, slice;
    size_t i;

    for (i = 0; i < node_count; i++) {
        lines += s->nodes[nodes[i]]->nr_cpus;
    }
    qdev_prop_set_uint32(split, "num-lines", MAX(lines, 1));
    object_property_add_child(OBJECT(module), "nmi-split", OBJECT(split));
    qdev_realize_and_unref(split, NULL, &error_fatal);
    qdev_connect_gpio_out_named(module, "nmi", 0, qdev_get_gpio_in(split, 0));
    for (i = 0; i < node_count; i++) {
        SN0NodeState *node = s->nodes[nodes[i]];

        for (slice = 0; slice < node->nr_cpus; slice++) {
            qdev_connect_gpio_out(split, line++,
                                  qdev_get_gpio_in(node->nmi_or[slice], 0));
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
    g_autofree const SGIModelNode **nodes = NULL;
    size_t controller_count, module_count, controller_index, n, node_count;

    controllers = sgi_model_controller_array(t->controllers, &controller_count);
    modules = sgi_model_module_array(t->modules, &module_count);
    s->module_power_count = controller_count;
    s->module_powered = g_new(bool, controller_count);
    s->module_mgmt = g_new0(SN0ModuleManagementState *, controller_count);
    s->node_controller = g_new(int, s->num_nodes);
    s->module_count = module_count;
    s->node_module = g_new0(unsigned int, s->num_nodes);
    s->controller_module = g_new0(unsigned int, controller_count);
    s->start_count = module_count + 1;
    s->module_start = g_new0(SGIMachineModuleStart, s->start_count);
    for (n = 0; n < s->start_count; n++) {
        s->module_start[n].machine = s;
        s->module_start[n].module = n;
    }
    s->module_start_vm_state = qemu_add_vm_change_state_handler(
        sgi_machine_module_start_vm_state, s);
    nodes = sgi_model_node_array(t->nodes, &node_count);
    for (n = 0; n < s->num_nodes; n++) {
        g_assert(n < node_count);
        s->node_module[n] = nodes[n]->module_index;
        s->node_controller[n] = -1;
        qdev_connect_gpio_out_named(DEVICE(&s->nodes[n]->hub),
                                    "machine-restart", 0,
                                    qemu_allocate_irq(sgi_machine_restart,
                                                      s, n));
        /* An Origin 200 node's own controller holds its module's power. */
        if (s->nodes[n]->hub.elsc) {
            qdev_connect_gpio_out_named(
                DEVICE(s->nodes[n]->hub.elsc), "power", 0,
                qemu_allocate_irq(sgi_machine_elsc_power, s, n));
            qdev_connect_gpio_out_named(
                DEVICE(s->nodes[n]->hub.elsc), "reset", 0,
                qemu_allocate_irq(sgi_machine_elsc_reset, s, n));
        }
    }
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
        s->controller_module[controller_index] = i;
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
        if (sgi_machine_module_nvram_node(s, t, module)) {
            qdev_prop_set_string(dev, "nvram-drive",
                                 sgi_machine_module_nvram_node(s, t, module));
        }
        qdev_prop_set_uint8(dev, "dip-switches", s->elsc_dip_switches);
        qdev_prop_set_uint32(dev, "start-delay-ms", s->module_start_delay_ms);
        qdev_prop_set_bit(dev, "can-reset", true);
        qdev_prop_set_bit(dev, "can-nmi", true);
        qdev_prop_set_bit(dev, "can-power", true);
        s->module_powered[controller_index] = true;
        qdev_connect_gpio_out_named(dev, "reset", 0,
                                    qemu_allocate_irq(sgi_machine_module_reset,
                                                      s, controller_index));
        qdev_connect_gpio_out_named(dev, "power", 0,
                                    qemu_allocate_irq(sgi_machine_module_power,
                                                      s, controller_index));
        sgi_machine_connect_module_nmi(s, dev, module_nodes,
                                       module_node_count);
        aux = serial_hd(sgi_machine_compute_serial_line(
            t, module_nodes[0], SN0_ELSC_CHARDEV));
        if (aux) {
            qdev_prop_set_chr(dev, "chardev", aux);
        }
        qdev_realize_and_unref(dev, NULL, &error_fatal);
        sgi_machine_attach_module_parts(s, t, module,
                                        SGI_SN0_MODULE_MANAGEMENT(dev));
        s->module_mgmt[controller_index] = SGI_SN0_MODULE_MANAGEMENT(dev);
        qdev_connect_gpio_out_named(
            DEVICE(s->module_mgmt[controller_index]->msc), "power", 0,
            qemu_allocate_irq(sgi_machine_module_recheck, s,
                              controller_index));

        for (i = 0; i < module_node_count; i++) {
            unsigned int node_index = module_nodes[i];
            SGIHubState *hub = &s->nodes[node_index]->hub;
            SysBusDevice *adapter = SYS_BUS_DEVICE(
                sgi_sn0_module_management_get_adapter(
                    SGI_SN0_MODULE_MANAGEMENT(dev), i));

            s->node_controller[node_index] = controller_index;
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

typedef struct SGIMachineNetEnd {
    Object *port;
    unsigned int index;
    char cable_property[32];
} SGIMachineNetEnd;

/* Resolve cable wiring independently of terminal transaction callbacks. */
static bool sgi_machine_bedrock_net_end(
    SGIMachineState *s, const SGIModelLinkEnd *end, SGIMachineNetEnd *net_end,
    Error **errp)
{
    DeviceState *dev;
    SN1FabricEndpoint *endpoint;

    switch (end->kind) {
    case SGI_MODEL_LINK_END_KIND_ROUTER:
        if (!s->sn1_routers || end->index >= s->router_count ||
            end->port < 1 || end->port > SGI_SN1_ROUTER_PORTS) {
            error_setg(errp, "Bedrock cable names an absent router port");
            return false;
        }
        net_end->port = OBJECT(s->sn1_routers[end->index]);
        net_end->index = end->port;
        snprintf(net_end->cable_property, sizeof(net_end->cable_property),
                 "port%u-cable", end->port);
        return true;
    case SGI_MODEL_LINK_END_KIND_NODE:
        if (end->index >= s->num_nodes) {
            error_setg(errp, "Bedrock cable names node %u out of range",
                       end->index);
            return false;
        }
        dev = s->bedrock_nodes[end->index];
        break;
    case SGI_MODEL_LINK_END_KIND_IO_PEER:
        if (end->index >= s->io_peer_count) {
            error_setg(errp, "Bedrock cable names I/O peer %u without a "
                       "constructed assembly", end->index);
            return false;
        }
        dev = s->io_peers[end->index];
        break;
    default:
        error_setg(errp, "Bedrock cable endpoint kind is unsupported");
        return false;
    }
    if (!dev || !object_dynamic_cast(OBJECT(dev), TYPE_SGI_SN1_FABRIC)) {
        error_setg(errp, "Bedrock cable endpoint has no fabric interface");
        return false;
    }
    endpoint = SGI_SN1_FABRIC_GET_CLASS(dev)->endpoint(OBJECT(dev));
    if (!endpoint) {
        return false;
    }
    *net_end = (SGIMachineNetEnd) {
        .port = OBJECT(endpoint->bedrock),
        .index = end->port,
        .cable_property = "net-cable",
    };
    return true;
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
        SGIMachineNetEnd endpoints[2];
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
            if (!sgi_machine_bedrock_net_end(s, ends[side],
                                               &endpoints[side], errp)) {
                return false;
            }
        }
        dev = qdev_new(TYPE_SGI_NET_CABLE);
        object_property_add_child(OBJECT(s), cable->name, OBJECT(dev));
        qdev_prop_set_uint32(dev, "kind", cable->kind);
        for (side = 0; side < 2; side++) {
            sgi_net_cable_set_end(SGI_NET_CABLE(dev), side,
                                 endpoints[side].port, endpoints[side].index);
        }
        if (!qdev_realize(dev, NULL, errp)) {
            object_unref(OBJECT(dev));
            return false;
        }
        for (side = 0; side < 2; side++) {
            if (!object_property_set_link(endpoints[side].port,
                                           endpoints[side].cable_property,
                                           OBJECT(dev), errp)) {
                object_unref(OBJECT(dev));
                return false;
            }
        }
        if (s->sn1_memory_mappings) {
            sn1_memory_mappings_add_cable(s->sn1_memory_mappings,
                                          SGI_NET_CABLE(dev));
        }
        object_unref(OBJECT(dev));

    }
    return true;
}

/* Controller ownership resolves existing objects; links never create owners. */
static DeviceState *sgi_machine_l1_peer_endpoint(SGIMachineState *s,
    const SGIModelTopology *t, unsigned index, Error **errp)
{
    const SGIModelControllerList *controller = t->controllers;
    const SGIModelModuleList *module = t->modules;
    const uint32List *node;
    DeviceState *result = NULL;
    unsigned i;

    for (i = 0; controller && i < index; i++) {
        controller = controller->next;
    }
    if (!controller ||
        controller->value->kind != SGI_MODEL_CONTROLLER_KIND_L1) {
        goto invalid;
    }
    for (i = 0; module && i < controller->value->module_index; i++) {
        module = module->next;
    }
    if (!module || module->value->controller_index != index) {
        goto invalid;
    }
    if (s->controllers && index < s->controller_count) {
        result = s->controllers[index];
    }
    for (node = module->value->nodes; node; node = node->next) {
        DeviceState *candidate;

        if (node->value >= s->num_nodes) {
            goto invalid;
        }
        candidate = SN1_NODE(s->bedrock_nodes[node->value])->l1;
        if (!candidate || (result && result != candidate)) {
            goto invalid;
        }
        result = candidate;
    }
    if (result) {
        return result;
    }
invalid:
    error_setg(errp, "invalid L1 peer controller ownership at index %u", index);
    return NULL;
}

static bool sgi_machine_build_l1_peer_links(SGIMachineState *s,
    const SGIModelTopology *t, Error **errp)
{
    size_t count = QAPI_LIST_LENGTH(t->controllers);
    g_autofree int64_t *peers = NULL;
    g_autofree DeviceState **objects = NULL;
    const SGIModelL1PeerLinkList *entry;
    size_t i;

    if (!t->l1_peer_links) {
        return true;
    }
    peers = g_new(int64_t, count);
    objects = g_new0(DeviceState *, count);
    for (i = 0; i < count; i++) {
        peers[i] = -1;
    }
    for (entry = t->l1_peer_links; entry; entry = entry->next) {
        unsigned source = entry->value->source_controller_index;
        unsigned target = entry->value->target_controller_index;

        if (source >= count || target >= count || source == target ||
            peers[source] >= 0) {
            error_setg(errp, "invalid L1 peer endpoints or duplicate source");
            return false;
        }
        peers[source] = target;
    }
    /* Validate every endpoint before changing any peer property. */
    for (i = 0; i < count; i++) {
        if (peers[i] < 0) {
            continue;
        }
        if (peers[peers[i]] != i) {
            error_setg(errp, "invalid L1 peer: reciprocal link required");
            return false;
        }
        objects[i] = sgi_machine_l1_peer_endpoint(s, t, i, errp);
        if (!objects[i]) {
            return false;
        }
    }
    for (i = 0; i < count; i++) {
        if (peers[i] >= 0 && objects[i] == objects[peers[i]]) {
            error_setg(errp, "invalid L1 peer: endpoints share one controller");
            return false;
        }
    }
    for (i = 0; i < count; i++) {
        if (peers[i] >= 0 &&
            !object_property_set_link(OBJECT(objects[i]), "peer",
                                      OBJECT(objects[peers[i]]), errp)) {
            return false;
        }
    }
    return true;
}

/*
 * The slot number of the G-brick whose pipe sits on @crossbar_index, the
 * crossbar an I/O brick's XIO cable ends on, or 0 for none. The I/O brick's
 * L1 lists that G-brick in its attached-brick reply.
 */
static unsigned int sgi_machine_pipe_brick_number(
    SGIMachineState *s, size_t crossbar_index,
    const SGIModelModule **modules, size_t module_count,
    const SGIModelIoBoard **boards, size_t board_count)
{
    for (size_t pipe = 0; s->pipe_record && pipe < board_count; pipe++) {
        const SGIModelXioAttachment *where = s->pipe_record_attachment[pipe];

        if (!s->pipe_record[pipe] || !where ||
            boards[pipe]->preset != SGI_MODEL_IO_BOARD_PRESET_KONA ||
            where->crossbar_index < 0 ||
            (size_t)where->crossbar_index != crossbar_index ||
            boards[pipe]->module_index >= module_count) {
            continue;
        }
        return modules[boards[pipe]->module_index]->initial_module_number;
    }
    return 0;
}

/*
 * Each I/O brick's L1, an I-brick's, an N-brick's or a V-brick's: a controller
 * of the brick's own module, reached from the L1 of every C-brick whose XIO
 * cable ends on that module's crossbars. An I-brick or V-brick has one such
 * C-brick, an N-brick up to four. The link between them is an in-process
 * call with no wire format. The I-brick's record its L1 serves carries the
 * IO7's station address; a V-brick's and an N-brick's L1 serve no such
 * record. A C-brick whose chip has a graphics pipe on its other port also
 * learns the pipe's G-brick, which its I/O brick's attached-brick reply
 * names.
 */
static bool sgi_machine_build_ibrick_l1s(SGIMachineState *s,
    const SGIModelTopology *t, Error **errp)
{
    g_autofree const SGIModelModule **modules = NULL;
    g_autofree const SGIModelCrossbar **crossbars = NULL;
    g_autofree const SGIModelIoBoard **boards = NULL;
    size_t module_count, crossbar_count, board_count;
    unsigned int index = 0;

    modules = sgi_model_module_array(t->modules, &module_count);
    crossbars = sgi_model_crossbar_array(t->crossbars, &crossbar_count);
    boards = sgi_model_io_board_array(t->io_boards, &board_count);
    for (const SGIModelControllerList *entry = t->controllers; entry;
         entry = entry->next, index++) {
        const SGIModelController *controller = entry->value;
        const SGIModelModule *module;
        g_autofree char *module_name = NULL;
        g_autofree char *mac = NULL;
        g_autofree DeviceState **hosts = NULL;
        g_autofree unsigned int *gfx_bays = NULL;
        DeviceState *l1;
        unsigned int host_count = 0;
        unsigned int board_index = 0;
        bool found_board = false;
        bool nbrick;
        bool vbrick;

        if (!s->l1_config || !s->l1_config[index].has_profile ||
            (s->l1_config[index].profile != SGI_MODEL_L1_PROFILE_IBRICK &&
             s->l1_config[index].profile != SGI_MODEL_L1_PROFILE_NBRICK &&
             s->l1_config[index].profile != SGI_MODEL_L1_PROFILE_VBRICK)) {
            continue;
        }
        nbrick = s->l1_config[index].profile == SGI_MODEL_L1_PROFILE_NBRICK;
        vbrick = s->l1_config[index].profile == SGI_MODEL_L1_PROFILE_VBRICK;
        if (controller->module_index >= module_count) {
            error_setg(errp, "I/O brick L1 '%s' names an absent module",
                       controller->id);
            return false;
        }
        module = modules[controller->module_index];
        hosts = g_new0(DeviceState *, s->num_nodes);
        gfx_bays = g_new0(unsigned int, s->num_nodes);
        for (const SGIModelXioCableList *cable = t->xio_cables; cable;
             cable = cable->next) {
            const SGIModelXioEndList *ends = cable->value->ends;
            const SGIModelXioEnd *a = ends->value, *b = ends->next->value;
            const SGIModelXioEnd *node_end =
                a->kind == SGI_MODEL_XIO_END_KIND_NODE ? a : b;
            const SGIModelXioEnd *bar_end = node_end == a ? b : a;

            if (node_end->kind != SGI_MODEL_XIO_END_KIND_NODE ||
                bar_end->kind != SGI_MODEL_XIO_END_KIND_CROSSBAR ||
                bar_end->index >= crossbar_count ||
                crossbars[bar_end->index]->module_index !=
                    controller->module_index) {
                continue;
            }
            if ((host_count && !nbrick) || node_end->index >= s->num_nodes) {
                error_setg(errp, "I/O brick L1 '%s' takes exactly one "
                           "C-brick", controller->id);
                return false;
            }
            hosts[host_count] = DEVICE(SN1_NODE(
                s->bedrock_nodes[node_end->index])->l1);
            gfx_bays[host_count++] = sgi_machine_pipe_brick_number(
                s, bar_end->index, modules, module_count, boards,
                board_count);
        }
        if (!host_count) {
            error_setg(errp, "I/O brick L1 '%s' has no C-brick cabled to "
                       "its brick", controller->id);
            return false;
        }
        if (!vbrick && !nbrick) {
            for (const SGIModelIoBoardList *board = t->io_boards; board;
                 board = board->next, board_index++) {
                if (board->value->module_index == controller->module_index &&
                    board->value->preset == SGI_MODEL_IO_BOARD_PRESET_IO7) {
                    found_board = true;
                    break;
                }
            }
            if (!found_board) {
                error_setg(errp, "I-brick L1 '%s' has no IO7 in its brick",
                           controller->id);
                return false;
            }
            mac = sgi_machine_onboard_mac(s, t, board_index, errp);
            if (!mac) {
                return false;
            }
        }
        module_name = g_strdup_printf("module%u", controller->module_index);
        l1 = qdev_new(TYPE_SGI_L1);
        object_property_add_child(
            object_resolve_path_component(OBJECT(s), module_name), "l1",
            OBJECT(l1));
        object_unref(OBJECT(l1));
        qdev_prop_set_string(l1, "brick-type",
                             nbrick ? "nbrick" : vbrick ? "vbrick" : "ibrick");
        if (mac) {
            qdev_prop_set_string(l1, "mac", mac);
        }
        if (module->initial_module_number) {
            qdev_prop_set_uint32(l1, "bay", module->initial_module_number);
            qdev_prop_set_bit(l1, "named", true);
        }
        if (!qdev_realize(l1, NULL, errp)) {
            return false;
        }
        for (unsigned int host = 0; host < host_count; host++) {
            if (gfx_bays[host] &&
                !object_property_set_uint(OBJECT(hosts[host]), "io-gfx-bay",
                                          gfx_bays[host], errp)) {
                return false;
            }
            if (!object_property_set_link(OBJECT(hosts[host]), "io-peer",
                                          OBJECT(l1), errp)) {
                return false;
            }
        }
        qemu_register_resettable(OBJECT(l1));
    }
    return true;
}

static void sgi_machine_io_peer_number_changed(Notifier *notifier, void *data)
{
    SGIMachinePeerNotifier *peer =
        container_of(notifier, SGIMachinePeerNotifier, notifier);

    sn1_fabric_update_claims(peer->endpoint);
    if (peer->memory_mappings) {
        sn1_memory_mappings_refresh(*peer->memory_mappings, NULL);
    }
}

#define SGI_MACHINE_IP53_FINGERPRINT_SIZE 32

static void sgi_machine_fingerprint_update(GChecksum *checksum,
                                           const void *data, size_t length)
{
    g_assert(length <= G_MAXSSIZE);
    g_checksum_update(checksum, data, length);
}

static void sgi_machine_fingerprint_u8(GChecksum *checksum, uint8_t value)
{
    sgi_machine_fingerprint_update(checksum, &value, sizeof(value));
}

static void sgi_machine_fingerprint_u32(GChecksum *checksum, uint32_t value)
{
    uint8_t encoded[] = {
        (value >> 24) & 0xff, (value >> 16) & 0xff,
        (value >> 8) & 0xff, value & 0xff,
    };

    sgi_machine_fingerprint_update(checksum, encoded, sizeof(encoded));
}

static void sgi_machine_fingerprint_u64(GChecksum *checksum, uint64_t value)
{
    uint8_t encoded[] = {
        (value >> 56) & 0xff, (value >> 48) & 0xff,
        (value >> 40) & 0xff, (value >> 32) & 0xff,
        (value >> 24) & 0xff, (value >> 16) & 0xff,
        (value >> 8) & 0xff, value & 0xff,
    };

    sgi_machine_fingerprint_update(checksum, encoded, sizeof(encoded));
}

static void sgi_machine_fingerprint_string(GChecksum *checksum,
                                           const char *value)
{
    size_t length = strlen(value);

    g_assert(length <= UINT32_MAX);
    sgi_machine_fingerprint_u32(checksum, length);
    sgi_machine_fingerprint_update(checksum, value, length);
}

static int sgi_machine_compare_host_ids(const void *left, const void *right)
{
    const SGIModelPciHostBridge *const *a = left;
    const SGIModelPciHostBridge *const *b = right;

    return g_strcmp0((*a)->id, (*b)->id);
}

static int sgi_machine_compare_board_ids(const void *left, const void *right)
{
    const SGIModelIoBoard *const *a = left;
    const SGIModelIoBoard *const *b = right;

    return g_strcmp0((*a)->id, (*b)->id);
}

static int sgi_machine_compare_devfns(const void *left, const void *right)
{
    const SGIModelPciFunction *const *a = left;
    const SGIModelPciFunction *const *b = right;
    unsigned int a_devfn = PCI_DEVFN((*a)->pci_device, (*a)->pci_function);
    unsigned int b_devfn = PCI_DEVFN((*b)->pci_device, (*b)->pci_function);

    return (a_devfn > b_devfn) - (a_devfn < b_devfn);
}

static bool sgi_machine_ip53_fingerprint(
    const SGIModelTopology *t,
    const SGIMachineNodeConfig *node_config,
    const SGIMachineL1Config *l1_config,
    bool runtime_permission,
    uint8_t pxbow_revision,
    uint8_t digest[32], Error **errp)
{
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_autofree const SGIModelNode **nodes = NULL;
    g_autofree const SGIModelController **controllers = NULL;
    g_autofree const SGIModelPciHostBridge **hosts = NULL;
    g_autofree const SGIModelPciHostBridge **sorted_hosts = NULL;
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    g_autofree const SGIModelCrossbar **crossbars = NULL;
    g_autofree const SGIModelIoBoard **boards = NULL;
    g_autofree const SGIModelIoBoard **sorted_boards = NULL;
    size_t node_count, controller_count, host_count, attachment_count;
    size_t crossbar_count;
    size_t board_count, io9_count = 0, i;
    static const uint8_t domain[] = "sgi-ip53-assembly";
    gsize digest_length = SGI_MACHINE_IP53_FINGERPRINT_SIZE;

    nodes = sgi_model_node_array(t->nodes, &node_count);
    controllers = sgi_model_controller_array(t->controllers,
                                              &controller_count);
    hosts = sgi_model_pci_host_bridge_array(t->pci_host_bridges, &host_count);
    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                  &attachment_count);
    crossbars = sgi_model_crossbar_array(t->crossbars, &crossbar_count);
    boards = sgi_model_io_board_array(t->io_boards, &board_count);
    sorted_hosts = g_memdup2(hosts, sizeof(*hosts) * host_count);
    sorted_boards = g_memdup2(boards, sizeof(*boards) * board_count);
    for (i = 0; i < board_count; i++) {
        io9_count += boards[i]->preset == SGI_MODEL_IO_BOARD_PRESET_IO9;
    }
    qsort(sorted_hosts, host_count, sizeof(*sorted_hosts),
          sgi_machine_compare_host_ids);
    qsort(sorted_boards, board_count, sizeof(*sorted_boards),
          sgi_machine_compare_board_ids);

    sgi_machine_fingerprint_update(checksum, domain, sizeof(domain));
    /* Version 14 adds the selected IO9 timekeeper geometry and seed policy. */
    sgi_machine_fingerprint_u32(checksum, runtime_permission ? 14 : 7);
    sgi_machine_fingerprint_u8(checksum, runtime_permission);
    sgi_machine_fingerprint_u32(checksum, runtime_permission ? 1 : 0);
    sgi_machine_fingerprint_u32(checksum, runtime_permission ? 10 : 0);
    sgi_machine_fingerprint_u32(checksum,
                                runtime_permission ? pxbow_revision : 0);
    sgi_machine_fingerprint_string(checksum, t->product ? t->product : "");
    sgi_machine_fingerprint_string(checksum, t->name);
    if (runtime_permission) {
        /* Reset PROM reads use the shared bounded IP35 carrier. */
        sgi_machine_fingerprint_u8(checksum, 1);
        /* The PIC-owned status-source field changes migration identity. */
        sgi_machine_fingerprint_u8(checksum, 1);
        /* The CPU PIO lane policy is part of the runtime composition. */
        sgi_machine_fingerprint_u8(checksum, 1);
        /* Direct64 DMA placement and attributes are runtime policy. */
        sgi_machine_fingerprint_u8(checksum, 3);
    }
    sgi_machine_fingerprint_u32(checksum, node_count);
    for (i = 0; i < node_count; i++) {
        const SGIModelNode *node = nodes[i];

        if (!node->has_board_kind ||
            node->board_kind != SGI_MODEL_BOARD_KIND_IP53 ||
            !node->identity || !node->identity->has_clock_hz ||
            !node->identity->clock_hz ||
            !node->identity->has_ni_revision ||
            node->identity->ni_revision > 15 ||
            !node->identity->has_widget_revision ||
            node->identity->widget_revision > 15) {
            error_setg(errp, "IP53 fingerprint has invalid Bedrock node '%s'",
                       node->id);
            return false;
        }
        if (node_config[i].controller_index >= controller_count ||
            !controllers[node_config[i].controller_index]->l1 ||
            !l1_config[node_config[i].controller_index].reported_type) {
            error_setg(errp, "IP53 fingerprint has invalid L1 controller for "
                       "Bedrock node '%s'", node->id);
            return false;
        }
        /* Node order assigns NASID and the node's address-space owner. */
        sgi_machine_fingerprint_u32(checksum, i);
        sgi_machine_fingerprint_string(checksum, node->id);
        sgi_machine_fingerprint_u64(checksum,
                                    node_config[i].board_id_word);
        sgi_machine_fingerprint_u32(checksum,
                                    node_config[i].controller_index);
        sgi_machine_fingerprint_u8(
            checksum,
            l1_config[node_config[i].controller_index].reported_type);
        {
            const SGIMachineL1Config *l1 =
                &l1_config[node_config[i].controller_index];

            sgi_machine_fingerprint_u8(checksum, l1->has_cpu_context);
            if (l1->has_cpu_context) {
                sgi_machine_fingerprint_u32(checksum, l1->cpu_mode_word);
                sgi_machine_fingerprint_u64(checksum, l1->cpu_clock_hz);
                sgi_machine_fingerprint_u64(checksum, l1->hub_clock_hz);
            }
        }
        sgi_machine_fingerprint_u64(checksum, node->identity->clock_hz);
        sgi_machine_fingerprint_u32(checksum,
                                    node->identity->ni_revision);
        sgi_machine_fingerprint_u32(checksum,
                                    node->identity->widget_revision);
        {
            const SGIModelCpuSlotList *slot;
            const uint64List *bank;
            size_t cpu_count = 0, bank_count = 0;

            for (slot = node->cpu_slots; slot; slot = slot->next) {
                cpu_count++;
            }
            sgi_machine_fingerprint_u32(checksum, cpu_count);
            for (slot = node->cpu_slots; slot; slot = slot->next) {
                const SGIModelCpuSlot *cpu_slot = slot->value;
                const SGIModelProcessor *processor = cpu_slot->assembly;

                sgi_machine_fingerprint_u32(checksum,
                                            cpu_slot->interface);
                sgi_machine_fingerprint_u32(checksum, cpu_slot->slot);
                sgi_machine_fingerprint_u8(checksum, processor != NULL);
                if (!processor) {
                    continue;
                }
                sgi_machine_fingerprint_string(checksum, processor->kind);
                sgi_machine_fingerprint_u8(checksum,
                                           processor->has_mode_bits);
                sgi_machine_fingerprint_u32(checksum,
                                            processor->mode_bits);
                sgi_machine_fingerprint_u64(checksum, processor->clock_hz);
                sgi_machine_fingerprint_u8(checksum,
                                           processor->has_cache_clock_hz);
                sgi_machine_fingerprint_u64(checksum,
                                            processor->cache_clock_hz);
                sgi_machine_fingerprint_u32(checksum, processor->cache_mib);
                sgi_machine_fingerprint_u64(checksum, SN1_PROM_RESET);
                sgi_machine_fingerprint_u64(checksum,
                                            node->flash->size);
            }
            for (bank = node->bank_mib; bank; bank = bank->next) {
                bank_count++;
            }
            sgi_machine_fingerprint_u32(checksum, bank_count);
            for (bank = node->bank_mib; bank; bank = bank->next) {
                sgi_machine_fingerprint_u64(checksum, bank->value);
            }
        }
    }

    sgi_machine_fingerprint_u32(checksum, host_count);
    for (i = 0; i < host_count; i++) {
        const SGIModelPciHostBridge *host = sorted_hosts[i];
        const SGIModelXioAttachment *attachment;
        const SGIModelNode *node;

        if (host->xio_attachment_index >= attachment_count ||
            !host->has_compatibility_profile) {
            error_setg(errp, "IP53 fingerprint has invalid PCI host '%s'",
                       host->id);
            return false;
        }
        attachment = attachments[host->xio_attachment_index];
        if (attachment->node_index >= node_count) {
            error_setg(errp, "IP53 fingerprint host '%s' has invalid owner",
                       host->id);
            return false;
        }
        node = nodes[attachment->node_index];
        sgi_machine_fingerprint_string(checksum, host->id);
        sgi_machine_fingerprint_string(checksum, node->id);
        sgi_machine_fingerprint_u32(checksum, attachment->widget);
        sgi_machine_fingerprint_u8(checksum,
                                   attachment->crossbar_index >= 0);
        if (attachment->crossbar_index >= 0) {
            if ((size_t)attachment->crossbar_index >= crossbar_count) {
                error_setg(errp, "IP53 fingerprint host '%s' has invalid "
                           "Crossbow owner", host->id);
                return false;
            }
            sgi_machine_fingerprint_string(
                checksum, crossbars[attachment->crossbar_index]->id);
        }
        sgi_machine_fingerprint_u32(checksum,
                                    host->compatibility_profile);
    }

    sgi_machine_fingerprint_u32(checksum, io9_count);
    for (i = 0; i < board_count; i++) {
        const SGIModelIoBoard *board = sorted_boards[i];
        const SGIModelPciHostBridge *host;
        const SGIModelXioAttachment *attachment;
        g_autofree const SGIModelPciFunction **functions = NULL;
        size_t function_count, function_index;

        if (board->preset != SGI_MODEL_IO_BOARD_PRESET_IO9) {
            continue;
        }
        if (!board->has_pci_host_index || board->pci_host_index >= host_count ||
            !board->has_pci_head) {
            error_setg(errp, "IP53 fingerprint has invalid IO9 board '%s'",
                       board->id);
            return false;
        }
        host = hosts[board->pci_host_index];
        attachment = attachments[host->xio_attachment_index];
        sgi_machine_fingerprint_string(checksum, board->id);
        sgi_machine_fingerprint_string(checksum, host->id);
        sgi_machine_fingerprint_u32(checksum, board->pci_head);
        sgi_machine_fingerprint_string(checksum,
                                       nodes[attachment->node_index]->id);
        sgi_machine_fingerprint_u32(checksum, attachment->widget);
        sgi_machine_fingerprint_u8(checksum, board->populated);
        sgi_machine_fingerprint_u8(checksum,
                                   board->has_ds1742 && board->ds1742);
        if (board->has_ds1742 && board->ds1742) {
            sgi_machine_fingerprint_string(checksum, board->id);
        }
        sgi_machine_fingerprint_u8(checksum,
                                   board->system_timekeeper != NULL);
        if (board->system_timekeeper) {
            const SGIModelSystemTimekeeper *clock =
                board->system_timekeeper;

            sgi_machine_fingerprint_u32(checksum, clock->kind);
            sgi_machine_fingerprint_u8(checksum, clock->has_year_base);
            if (clock->has_year_base) {
                sgi_machine_fingerprint_u32(checksum, clock->year_base);
            }
            sgi_machine_fingerprint_u8(checksum,
                                       clock->initial_ip27_fixture);
        }
        functions = sgi_model_pci_function_array(
            board->populated ? board->pci_functions : NULL, &function_count);
        qsort(functions, function_count, sizeof(*functions),
              sgi_machine_compare_devfns);
        sgi_machine_fingerprint_u32(checksum, function_count);
        for (function_index = 0; function_index < function_count;
             function_index++) {
            const SGIModelPciFunction *function = functions[function_index];

            sgi_machine_fingerprint_u32(checksum, function->kind);
            sgi_machine_fingerprint_u8(checksum, function->revision);
            sgi_machine_fingerprint_u8(checksum, function->pci_device);
            sgi_machine_fingerprint_u8(checksum, function->pci_function);
            sgi_machine_fingerprint_u32(checksum, function->intx_pin);
            sgi_machine_fingerprint_u32(checksum, function->pic_input);
            sgi_machine_fingerprint_u8(checksum, function->has_clock_hz);
            if (function->has_clock_hz) {
                sgi_machine_fingerprint_u32(checksum, function->clock_hz);
            }
            sgi_machine_fingerprint_u8(
                checksum, function->has_serial_dma_experimental);
            if (function->has_serial_dma_experimental) {
                sgi_machine_fingerprint_u8(
                    checksum, function->serial_dma_experimental);
            }
        }
    }
    g_checksum_get_digest(checksum, digest, &digest_length);
    if (digest_length != SGI_MACHINE_IP53_FINGERPRINT_SIZE) {
        error_setg(errp, "could not compute IP53 assembly fingerprint");
        return false;
    }
    return true;
}

static int sgi_machine_ip53_fingerprint_pre_save(void *opaque)
{
    SGIMachineState *s = opaque;

    memcpy(s->ip53_incoming_fingerprint, s->ip53_local_fingerprint,
           sizeof(s->ip53_incoming_fingerprint));
    return 0;
}

static void sgi_machine_finalize_ip53_fingerprint(SGIMachineState *s)
{
    static const uint8_t domain[] = "sgi-ip53-effective-l1-eeprom";
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    uint8_t digest[32], eeprom_digest[32];
    gsize digest_length = sizeof(digest);

    g_assert(!s->ip53_fingerprint_finalized);
    g_assert(s->bedrock_nodes);
    g_checksum_update(checksum, domain, sizeof(domain));
    g_checksum_update(checksum, s->ip53_local_fingerprint,
                      sizeof(s->ip53_local_fingerprint));
    sgi_machine_fingerprint_u32(checksum, s->num_nodes);
    for (unsigned i = 0; i < s->num_nodes; i++) {
        DeviceState *controller = SN1_NODE(s->bedrock_nodes[i])->l1;

        g_assert(controller);
        sgi_machine_fingerprint_u32(checksum, i);
        sgi_l1_eeprom_digest(controller, eeprom_digest);
        sgi_machine_fingerprint_update(checksum, eeprom_digest,
                                       sizeof(eeprom_digest));
    }
    g_checksum_get_digest(checksum, digest, &digest_length);
    g_assert(digest_length == sizeof(digest));
    memcpy(s->ip53_local_fingerprint, digest, sizeof(digest));
    s->ip53_fingerprint_finalized = true;
}

static bool sgi_machine_ip53_fingerprint_post_load(void *opaque,
                                                    int version_id,
                                                    Error **errp)
{
    SGIMachineState *s = opaque;
    bool mismatch;

    mismatch = memcmp(s->ip53_incoming_fingerprint,
                      s->ip53_local_fingerprint,
                      sizeof(s->ip53_local_fingerprint)) != 0;
    if (mismatch) {
        error_setg(errp, "IP53 assembly fingerprint mismatch");
        return false;
    }
    return true;
}

static const VMStateDescription vmstate_sgi_machine_ip53_fingerprint = {
    .name = "sgi-machine/ip53-assembly",
    .version_id = 1,
    .minimum_version_id = 1,
    /* Reject a changed memory layout before the incoming RAM section. */
    .early_setup = true,
    .priority = MIG_PRI_BACKEND,
    .pre_save = sgi_machine_ip53_fingerprint_pre_save,
    .post_load_errp = sgi_machine_ip53_fingerprint_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(ip53_incoming_fingerprint, SGIMachineState, 32),
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_machine_register_ip53_fingerprint(SGIMachineState *s)
{
    if (s->ip53_fingerprint_enabled) {
        sgi_machine_finalize_ip53_fingerprint(s);
        vmstate_register_any(NULL, &vmstate_sgi_machine_ip53_fingerprint, s);
        s->ip53_fingerprint_registered = true;
    }
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

static bool sgi_machine_expansion_power_request(Object *receiver,
    SGIL1State *issuer, uint64_t identity, bool on, Error **errp)
{
    return sgi_origin300_pci_expansion_request_power(
        SGI_ORIGIN300_PCI_EXPANSION(receiver), issuer, identity, on, errp);
}

static uint64_t sgi_machine_controller_identity(const SGIModelTopology *t,
                                                unsigned controller)
{
    const SGIModelL1OwnerList *owner;

    for (owner = t->l1_management->owners; owner; owner = owner->next) {
        if (owner->value->controller_index == controller) {
            return owner->value->identity;
        }
    }
    return 0;
}

static uint32_t sgi_machine_io_origin_count(SGIMachineState *s,
    const SGIModelTopology *t, uint32_t peer)
{
    const SGIModelCableList *entry;

    for (entry = t->cables; entry; entry = entry->next) {
        const SGIModelLinkEnd *ends[2] = {
            entry->value->ends->value, entry->value->ends->next->value,
        };

        for (unsigned side = 0; side < 2; side++) {
            if (ends[side]->kind == SGI_MODEL_LINK_END_KIND_IO_PEER &&
                ends[side]->index == peer &&
                ends[1 - side]->kind == SGI_MODEL_LINK_END_KIND_NODE) {
                unsigned node = ends[1 - side]->index;

                return SN1_NODE(s->bedrock_nodes[node])->nr_cpus;
            }
        }
    }
    if (t->generation == SGI_MODEL_GENERATION_SN1 && t->routers) {
        unsigned origins = 0;

        /* Routed request origins name CPUs within a node. */
        for (unsigned node = 0; node < s->num_nodes; node++) {
            origins = MAX(origins, SN1_NODE(s->bedrock_nodes[node])->nr_cpus);
        }
        return origins;
    }
    return 0;
}

static bool sgi_machine_build_sn1_routers(SGIMachineState *s,
    const SGIModelTopology *t, Error **errp)
{
    const SGIModelRouterList *entry;
    g_autofree const SGIModelModule **modules = NULL;
    size_t module_count;
    unsigned index = 0;

    modules = sgi_model_module_array(t->modules, &module_count);
    s->router_count = QAPI_LIST_LENGTH(t->routers);
    s->sn1_routers = g_new0(SGISN1RouterState *, s->router_count);
    for (entry = t->routers; entry; entry = entry->next, index++) {
        const SGIModelRouter *record = entry->value;
        const SGIModelModule *model_module = modules[record->module_index];
        g_autofree char *name =
            g_strdup_printf("module%u", record->module_index);
        Object *module = object_resolve_path_component(OBJECT(s), name);
        unsigned owner = model_module->controller_index;
        DeviceState *l1, *uart, *router;
        size_t record_count;
        g_autofree const SGIModelController **controller_records =
            sgi_model_controller_array(t->controllers, &record_count);
        bool cx_brick = owner < record_count &&
            sgi_machine_controller_cx(controller_records[owner]);

        if (cx_brick && s->controllers[owner]) {
            /*
             * A Cx-brick's router sits on its one L1's board, and the stock
             * PROM reads the router's identity through the router's UART
             * window, which no source describes for a Cx-brick. Approximate
             * it with a router-profile L1 behind the window, sharing the
             * brick's serial and taking the slot after its four boards' (the
             * PROM refuses two modules at one slot). It is not
             * a second controller of the brick: it is not listed among the
             * topology's controllers and no node reaches it.
             */
            l1 = qdev_new(TYPE_SGI_L1);
            object_property_add_child(module, "router-l1", OBJECT(l1));
            sgi_machine_set_module_serial(s, owner, l1);
            qdev_prop_set_string(l1, "brick-type", "router");
            qdev_prop_set_uint32(l1, "brick-type-code", 0x52);
            qdev_prop_set_uint32(l1, "bay",
                                 model_module->initial_module_number +
                                 SGI_L1_CX_BOARDS);
            if (!qdev_realize(l1, NULL, errp)) {
                return false;
            }
            qemu_register_resettable(OBJECT(l1));
        } else {
            if (!module || owner >= s->controller_count ||
                s->controllers[owner]) {
                error_setg(errp,
                           "SN1 router needs a unique module L1 controller");
                return false;
            }
            l1 = qdev_new(TYPE_SGI_L1);
            object_property_add_child(module, "l1", OBJECT(l1));
            s->controllers[owner] = l1;
            sgi_machine_set_module_serial(s, owner, l1);
            qdev_prop_set_string(l1, "brick-type", "router");
            qdev_prop_set_uint32(l1, "brick-type-code", 0x52);
            qdev_prop_set_uint32(l1, "bay",
                                 model_module->initial_module_number);
            if (!qdev_realize(l1, NULL, errp)) {
                return false;
            }
            qemu_register_resettable(OBJECT(l1));
            s->controller_reset_registered[owner] = true;
        }
        uart = qdev_new(TYPE_SGI_L1_UART);
        object_property_add_child(module, "l1-uart", OBJECT(uart));
        object_property_set_link(OBJECT(uart), "controller", OBJECT(l1),
                                 &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(uart), errp)) {
            object_unref(OBJECT(uart));
            return false;
        }
        router = qdev_new(TYPE_SGI_SN1_ROUTER);
        if (record->has_synthetic_diagnostics &&
            record->synthetic_diagnostics) {
            sgi_sn1_router_enable_synthetic_diagnostics(
                SGI_SN1_ROUTER(router));
        }
        object_property_add_child(module, "router", OBJECT(router));
        qdev_prop_set_uint8(router, "chip-revision", record->revision);
        object_property_set_link(OBJECT(router), "l1-uart", OBJECT(uart),
                                 &error_abort);
        object_unref(OBJECT(uart));
        if (!sysbus_realize(SYS_BUS_DEVICE(router), errp)) {
            object_unref(OBJECT(router));
            return false;
        }
        s->sn1_routers[index] = SGI_SN1_ROUTER(router);
        object_unref(OBJECT(router));
    }
    return true;
}

/*
 * A routed fabric tells its modules apart by their L1s' brick serials,
 * so no two built controllers may share one.
 */
static bool sgi_machine_check_controller_serials(SGIMachineState *s,
                                                 Error **errp)
{
    for (unsigned index = 0; index < s->controller_count; index++) {
        const char *serial = s->controllers[index] ?
            SGI_L1(s->controllers[index])->brick_serial : NULL;

        for (unsigned other = 0; serial && other < index; other++) {
            if (s->controllers[other] &&
                !g_strcmp0(serial,
                           SGI_L1(s->controllers[other])->brick_serial)) {
                error_setg(errp, "controllers %u and %u share brick "
                           "serial '%s'; use controller-brick-serial "
                           "to assign distinct module identities",
                           other, index, serial);
                return false;
            }
        }
    }
    return true;
}

/*
 * Register each node's L1 under its module's controller and build every
 * SN1 router with its module's L1. Routers need this whether or not the
 * topology carries the L1 management registry.
 */
static bool sgi_machine_build_sn1_controllers(SGIMachineState *s,
    const SGIModelTopology *t, Error **errp)
{
    g_autofree const SGIModelModule **modules = NULL;
    const SGIModelNodeList *node_entry;
    size_t module_count;
    unsigned index;

    if (!t->l1_management && !t->routers) {
        return true;
    }
    modules = sgi_model_module_array(t->modules, &module_count);
    s->controller_count = QAPI_LIST_LENGTH(t->controllers);
    s->controllers = g_new0(DeviceState *, s->controller_count);
    s->controller_reset_registered = g_new0(bool, s->controller_count);
    s->controller_bics = g_new0(DeviceState *, s->controller_count);
    for (node_entry = t->nodes, index = 0; node_entry;
         node_entry = node_entry->next, index++) {
        const SGIModelNode *node = node_entry->value;
        int64_t owner;

        if (node->module_index >= module_count) {
            error_setg(errp, "compute L1 names an absent module");
            return false;
        }
        owner = modules[node->module_index]->controller_index;
        /* A Cx-brick's nodes share one controller. */
        if (owner >= 0 && owner < s->controller_count &&
            s->controllers[owner] &&
            s->controllers[owner] == SN1_NODE(s->bedrock_nodes[index])->l1) {
            continue;
        }
        if (owner < 0 || owner >= s->controller_count ||
            s->controllers[owner]) {
            error_setg(errp, "compute L1 needs unique module-owned controller");
            return false;
        }
        s->controllers[owner] = SN1_NODE(s->bedrock_nodes[index])->l1;
        object_ref(OBJECT(s->controllers[owner]));
    }
    if (!sgi_machine_build_sn1_routers(s, t, errp)) {
        return false;
    }
    /* With the registry, the check waits for every controller it builds. */
    return t->l1_management || sgi_machine_check_controller_serials(s, errp);
}

static bool sgi_machine_build_io_assemblies(SGIMachineState *s,
    const SGIModelTopology *t, Error **errp)
{
    g_autofree const SGIModelModule **modules = NULL;
    const SGIModelIoAssemblyList *entry;
    const SGIModelL1OwnerList *owner_entry;
    const SGIModelL1AssociationList *association;
    const SGIModelL1AttachmentList *attachment;
    size_t module_count;
    unsigned index;
    Error *local_error = NULL;
    static const char *const port_names[] = {
        "chip1-e-port", "chip1-f-port", "chip2-e-port", "chip2-f-port",
    };

    if (!t->l1_management) {
        return true;
    }
    modules = sgi_model_module_array(t->modules, &module_count);
    s->io_peer_count = QAPI_LIST_LENGTH(t->io_peers);
    s->io_peers = g_new0(DeviceState *, s->io_peer_count);
    s->io_peer_notifiers = g_new0(SGIMachinePeerNotifier, s->io_peer_count);
    s->assembly_ownership =
        g_new0(SGIMachineAssemblyOwnership, s->io_peer_count);
    for (const SGIModelIoBoardList *board = t->io_boards; board;
         board = board->next) {
        const SGIModelIoBoard *record = board->value;
        const SGIModelModule *model_module;
        DeviceState *l1;
        Object *module;
        unsigned owner;
        g_autofree char *name = NULL;

        if (record->preset != SGI_MODEL_IO_BOARD_PRESET_VBRICK) {
            continue;
        }
        if (record->module_index >= module_count) {
            error_setg(errp, "V-brick L1 module is absent");
            return false;
        }
        model_module = modules[record->module_index];
        owner = model_module->controller_index;
        if (owner >= s->controller_count || s->controllers[owner] ||
            !model_module->initial_module_number) {
            error_setg(errp, "V-brick needs a distinct "
                       "module-owned L1 location");
            return false;
        }
        name = g_strdup_printf("module%u", record->module_index);
        module = object_resolve_path_component(OBJECT(s), name);
        l1 = qdev_new(TYPE_SGI_L1);
        object_property_add_child(module, "vbrick-l1", OBJECT(l1));
        s->controllers[owner] = l1;
        qdev_prop_set_string(l1, "brick-type", "vbrick-discovery-reference");
        qdev_prop_set_uint32(l1, "bay", model_module->initial_module_number);
        if (!qdev_realize(l1, NULL, errp)) {
            return false;
        }
        qemu_register_resettable(OBJECT(l1));
        s->controller_reset_registered[owner] = true;
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

        notifier->memory_mappings = &s->sn1_memory_mappings;

        object_property_add_child(module, "l1", OBJECT(l1));
        s->controllers[a->controller_index] = l1;
        qdev_prop_set_string(l1, "brick-type", a->controller_profile);
        if (modules[a->module_index]->initial_module_number) {
            qdev_prop_set_uint32(l1, "bay",
                modules[a->module_index]->initial_module_number);
        }
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
        if (a->has_main_power) {
            uint64_t identity = sgi_machine_controller_identity(
                t, a->controller_index);

            if (!sgi_origin300_pci_expansion_bind_power(assembly, SGI_L1(l1),
                    identity, a->initial_main_power, NULL, NULL, errp) ||
                !sgi_l1_bind_expansion_power(SGI_L1(l1), OBJECT(dev), identity,
                    sgi_machine_expansion_power_request, errp)) {
                object_unref(OBJECT(interface));
                object_unref(OBJECT(dev));
                return false;
            }
            if (!s->controller_reset_registered[a->controller_index]) {
                s->controller_bics[a->controller_index] = interface;
                object_ref(OBJECT(interface));
                qemu_register_resettable(OBJECT(interface));
                qemu_register_resettable(OBJECT(l1));
                s->controller_reset_registered[a->controller_index] = true;
            }
        }
        if (a->has_configuration_storage) {
            const SGIModelControllerList *controller = t->controllers;

            for (unsigned owner_index = 0;
                 owner_index < a->controller_index; owner_index++) {
                controller = controller->next;
            }
            if (s->storage_assembly != a || !controller ||
                !sgi_origin300_pci_expansion_bind_storage(assembly,
                    modules[a->module_index]->id, controller->value->id,
                    s->storage_backend, errp)) {
                object_unref(OBJECT(interface));
                object_unref(OBJECT(dev));
                return false;
            }
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
        if (a->has_controller_startup &&
            a->controller_startup ==
                SGI_MODEL_CONTROLLER_STARTUP_BIC_FUNCTIONAL_EXPERIMENT &&
            !sgi_origin300_pci_expansion_enable_processor_absence(assembly,
                                                                  errp)) {
            return false;
        }
        if (a->has_unconnected_uart) {
            if (!a->unconnected_uart) {
                error_setg(errp, "synthetic expansion UART selection is false");
                return false;
            }
            qdev_prop_set_bit(dev, "unconnected-uart-experiment", true);
            qdev_prop_set_uint32(dev, "uart-origin-count",
                sgi_machine_io_origin_count(s, t, a->peer_index));
        }
        if (a->has_pending_register_experiment &&
            (!a->pending_register_experiment ||
             !sgi_origin300_pci_expansion_enable_pending_registers(assembly,
                 sgi_machine_io_origin_count(s, t, a->peer_index), errp))) {
            return false;
        }
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
    for (entry = t->io_assemblies; entry; entry = entry->next) {
        const SGIModelIoAssembly *a = entry->value;

        if (a->has_power_command_issuer &&
            !sgi_l1_bind_power_command(SGI_L1(s->controllers[
                a->power_command_issuer]), SGI_L1(s->controllers[
                a->controller_index]), sgi_machine_controller_identity(t,
                a->controller_index), errp)) {
            return false;
        }
    }
    for (index = 0; index < s->controller_count; index++) {
        if (!s->controllers[index]) {
            error_setg(errp, "controller %u has no constructed L1 owner",
                       index);
            return false;
        }
    }
    if (t->routers && !sgi_machine_check_controller_serials(s, errp)) {
        return false;
    }
    for (index = 0; index < s->controller_count; index++) {
        SGIL1State *l1 = SGI_L1(s->controllers[index]);

        if (strcmp(l1->brick_type, "vbrick-discovery-reference")) {
            continue;
        }
        for (unsigned other = 0; other < s->controller_count; other++) {
            SGIL1State *peer = SGI_L1(s->controllers[other]);

            if (other != index && l1->rack == peer->rack &&
                l1->bay == peer->bay) {
                error_setg(errp, "V-brick L1 location duplicates "
                           "another owner");
                return false;
            }
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
    SN1NodeState *first = SN1_NODE(s->bedrock_nodes[0]);
    g_autoptr(SGIPromImage) image = NULL;
    g_autofree char *path = NULL;
    const uint8_t *contents = NULL;
    size_t length = 0;
    unsigned int node_index, bound = 0;

    for (node_index = 0; node_index < s->num_nodes; node_index++) {
        SN1NodeState *node = SN1_NODE(s->bedrock_nodes[node_index]);

        bound += node->flash_node || drive_get(IF_PFLASH, 0, node_index);
    }
    if (!sgi_storage_check_boot_prom(MACHINE_GET_CLASS(machine)->name,
                                     machine->firmware, bound, s->num_nodes,
                                     false, errp)) {
        return false;
    }
    /*
     * Without -bios every node must boot from its own bound or pflash
     * flash; sn1_node_attach_prom() refuses a node that has neither.
     */
    if (machine->firmware) {
        path = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        if (!path) {
            error_setg(errp, "%s: cannot find PROM image '%s'",
                       MACHINE_GET_CLASS(machine)->name, machine->firmware);
            return false;
        }
        image = sgi_prom_image_load(path, first->model_node->flash->size,
                                    errp);
        if (!image) {
            return false;
        }
        contents = g_bytes_get_data(image->payload, &length);
    }

    for (node_index = 0; node_index < s->num_nodes; node_index++) {
        SN1NodeState *node = SN1_NODE(s->bedrock_nodes[node_index]);

        if (!sn1_node_attach_prom(node, node_index, contents, length,
                                  errp)) {
            return false;
        }
    }
    return true;
}

/*
 * -bios fills every node's boot flash unless a pflash drive already backs
 * it (set on the node before it realized); a short -bios image leaves the
 * default template's own bytes behind it (sgi_ip27_initial_flash()),
 * matching a factory-formatted chip rather than a merely erased one.
 * Bedrock's own policy has no synthesized default and lives in
 * sgi_machine_load_bedrock_flash above; this function's own body stays the
 * Hub's.
 */
static bool sgi_machine_load_flash(MachineState *machine,
                                   const SGIModelTopology *t,
                                   SGIMachineState *s, Error **errp)
{
    const SGIModelIp27FirmwareConfig *firmware = t->ip27_firmware;
    const SGIModelProcessor *cpu;
    SGIIP27FlashValues values;
    g_autoptr(SGIPromImage) image = NULL;
    g_autofree uint8_t *flash = NULL;
    const uint8_t *payload = NULL;
    size_t length = 0;
    uint64_t flash_size;
    unsigned int node_index;

    /* Every I/O board is built by now, on both the Hub and Bedrock paths. */
    if (s->io_prom && !s->io_prom_used) {
        error_setg(errp, "io-prom requires a BASEIO or GIGAchannel flash "
                   "board in the selected topology");
        return false;
    }
    if (sgi_machine_topology_node0(t)->asic_kind != SGI_MODEL_ASIC_KIND_HUB) {
        return sgi_machine_load_bedrock_flash(machine, s, errp);
    }

    if (!firmware) {
        error_setg(errp, "%s: topology '%s' carries no IP27 flash "
                   "configuration values", MACHINE_GET_CLASS(machine)->name,
                   t->name);
        return false;
    }
    flash_size = sn0_hub_address_map.flash_reset.size;
    if (machine->firmware) {
        g_autofree char *path =
            qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);

        if (!path) {
            error_setg(errp, "%s: cannot read the firmware image '%s'",
                      MACHINE_GET_CLASS(machine)->name, machine->firmware);
            return false;
        }
        image = sgi_prom_image_load(path, flash_size, errp);
        if (!image) {
            return false;
        }
        payload = g_bytes_get_data(image->payload, &length);
    }

    cpu = s->processor;
    values = (SGIIP27FlashValues) {
        .machine_type = firmware->machine_type,
        .read_cycles = firmware->flash_read_cycles,
        .write_cycles = firmware->flash_write_cycles,
        .cpu_mode_bits = cpu->mode_bits,
        .cpu_clock_hz = cpu->clock_hz,
    };
    flash = g_malloc(flash_size);
    sgi_ip27_initial_flash(flash, flash_size, &values, payload, length);
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
        return s->board_id_override_set;
    case SGI_INPUT_IOC3_SUBSYSTEM:
        return s->ioc3_subsystem_override_set;
    case SGI_INPUT_L1_TYPE:
        return s->l1_type_override_set;
    case SGI_INPUT_L1_REVISION:
        return s->l1_revision_override_set;
    case SGI_INPUT_BEDROCK_REVISION:
        return s->bedrock_revision_override_set;
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

/*
 * A family override applies to every instance of its field, so it is
 * marked used rather than bound once. Without an override the catalogue
 * value stands; a topology with neither cannot be built.
 */
static bool sgi_machine_resolve_uint(SGIMachineState *s,
                                    const SGIModelTopology *t,
                                    SGIMachineInput input, unsigned int *used,
                                    bool has_default, uint64_t default_value,
                                    uint64_t *value, Error **errp)
{
    *used |= 1U << input;
    if (!sgi_machine_input_present(s, input)) {
        if (!has_default) {
            error_setg(errp, "topology '%s' has no catalogue value for %s",
                       t->name, sgi_machine_input_names[input]);
            return false;
        }
        *value = default_value;
        return true;
    }
    switch (input) {
    case SGI_INPUT_BOARD_ID:
        *value = s->board_id_override;
        break;
    case SGI_INPUT_IOC3_SUBSYSTEM:
        *value = s->ioc3_subsystem_override;
        break;
    case SGI_INPUT_L1_TYPE:
        *value = s->l1_type_override;
        break;
    case SGI_INPUT_BEDROCK_REVISION:
        *value = s->bedrock_revision_override;
        break;
    default:
        g_assert_not_reached();
    }
    return true;
}

/* Refuse authored board identities outside the builder's supported set. */
static bool sgi_machine_topology_has_ip53(const SGIModelTopology *t)
{
    const SGIModelNodeList *entry;

    for (entry = t->nodes; entry; entry = entry->next) {
        const SGIModelNode *node = entry->value;

        if (node->has_board_kind &&
            node->board_kind == SGI_MODEL_BOARD_KIND_IP53) {
            return true;
        }
    }
    return false;
}

static bool sgi_machine_validate_board_kinds(const SGIModelTopology *t,
                                            bool allow_ip53,
                                            bool runtime_experiment,
                                            Error **errp)
{
    const SGIModelNodeList *entry;

    for (entry = t->nodes; entry; entry = entry->next) {
        const SGIModelNode *node = entry->value;

        if (!node->has_board_kind) {
            continue;
        }
        if (node->asic_kind != SGI_MODEL_ASIC_KIND_BEDROCK) {
            error_setg(errp, "topology '%s' node '%s' has board-kind on a "
                       "non-Bedrock node", t->name, node->id);
            return false;
        }
        if (node->board_kind == SGI_MODEL_BOARD_KIND_IP53) {
            if (!allow_ip53) {
                error_setg(errp, "topology '%s' node '%s' selects unsupported "
                           "board kind 'ip53'", t->name, node->id);
                return false;
            }
            if (!node->identity || !node->identity->has_clock_hz ||
                !node->identity->clock_hz) {
                error_setg(errp, "topology '%s' IP53 node '%s' requires "
                           "an explicitly compiled positive Bedrock clock",
                           t->name, node->id);
                return false;
            }
            if (runtime_experiment && node->identity->clock_hz != 200000000) {
                error_setg(errp, "IP53 runtime profile requires a "
                           "200000000 Hz Bedrock clock");
                return false;
            }
        }
    }
    return true;
}

/* Validate optional XIO and PCI board references before config resolution. */
static bool sgi_machine_preflight_io_descriptors(
    const SGIModelTopology *t, bool allow_ip53_fixture,
    bool allow_ip53_experiment, bool tezro, Error **errp)
{
    g_autofree const SGIModelIoBoard **boards = NULL;
    g_autofree const SGIModelPciHostBridge **hosts = NULL;
    g_autofree const SGIModelXioAttachment **attachments = NULL;
    g_autofree const SGIModelCrossbar **crossbars = NULL;
    g_autofree const SGIModelNode **nodes = NULL;
    g_autofree bool *owned_attachments = NULL;
    g_autofree bool *owned_heads = NULL;
    g_autofree bool *owned_devfns = NULL;
    g_autofree bool *owned_routes = NULL;
    size_t board_count, host_count, attachment_count, crossbar_count;
    size_t node_count, i;
    size_t io9_count = 0;
    bool has_io9 = false;
    bool has_ip53 = false;
    bool all_ip53 = true;
    size_t expected_host_count = tezro ? 0 : 1;
    const SGIModelNodeList *node_entry;

    for (node_entry = t->nodes; node_entry; node_entry = node_entry->next) {
        const SGIModelNode *node = node_entry->value;
        bool ip53 = node->has_board_kind &&
            node->board_kind == SGI_MODEL_BOARD_KIND_IP53;

        has_ip53 |= ip53;
        all_ip53 &= ip53;
    }

    boards = sgi_model_io_board_array(t->io_boards, &board_count);
    hosts = sgi_model_pci_host_bridge_array(t->pci_host_bridges, &host_count);
    if (tezro && allow_ip53_experiment) {
        if (host_count == 0 || host_count > 2) {
            error_setg(errp, "Tezro runtime profile requires one or two PIC hosts");
            return false;
        }
        expected_host_count = host_count;
    }
    attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                  &attachment_count);
    crossbars = sgi_model_crossbar_array(t->crossbars, &crossbar_count);
    nodes = sgi_model_node_array(t->nodes, &node_count);
    owned_attachments = g_new0(bool, attachment_count ? attachment_count : 1);
    owned_heads = g_new0(bool, host_count ? host_count * 2 : 1);
    owned_devfns = g_new0(bool, MAX(host_count * 2 * 256, 1));
    owned_routes = g_new0(bool, MAX(host_count * 2 * 128, 1));

    if (has_ip53 && crossbar_count) {
        error_setg(errp, "topology '%s' IP53 construction does not support "
                   "XIO crossbars", t->name);
        return false;
    }

    for (i = 0; i < host_count; i++) {
        const SGIModelPciHostBridge *host = hosts[i];
        unsigned int attachment = host->xio_attachment_index;

        if (host->kind != SGI_MODEL_PCI_HOST_KIND_PIC) {
            error_setg(errp, "topology '%s' PCI host '%s' has unsupported kind",
                       t->name, host->id);
            return false;
        }
        if (host->has_compatibility_profile &&
            host->compatibility_profile !=
                sgi_machine_ip53_pic_profile(t, allow_ip53_experiment)) {
            if (allow_ip53_experiment) {
                error_setg(errp, "topology '%s' PCI host '%s' must select "
                           "its authored runtime PIC profile", t->name,
                           host->id);
            } else {
                error_setg(errp, "topology '%s' PCI host '%s' has unsupported "
                           "compatibility profile", t->name, host->id);
            }
            return false;
        }
        if (attachment >= attachment_count) {
            error_setg(errp, "topology '%s' PCI host '%s' has out-of-range "
                       "XIO attachment index %u", t->name, host->id,
                       attachment);
            return false;
        }
        if (owned_attachments[attachment]) {
            error_setg(errp, "topology '%s' PCI hosts share XIO attachment "
                       "index %u", t->name, attachment);
            return false;
        }
        owned_attachments[attachment] = true;
        if (has_ip53) {
            const SGIModelXioAttachment *selected = attachments[attachment];
            size_t other;

            if (selected->node_index >= node_count ||
                selected->widget == 0 || selected->widget >= SGI_XIO_WIDGETS ||
                selected->crossbar_index != -1 ||
                !nodes[selected->node_index]->has_board_kind ||
                nodes[selected->node_index]->board_kind !=
                    SGI_MODEL_BOARD_KIND_IP53) {
                error_setg(errp, "topology '%s' PCI host '%s' has an "
                           "unsupported node, widget, or Crossbow "
                           "attachment", t->name, host->id);
                return false;
            }
            for (other = 0; other < i; other++) {
                const SGIModelXioAttachment *prior =
                    attachments[hosts[other]->xio_attachment_index];

                if (prior->node_index == selected->node_index &&
                    prior->widget == selected->widget) {
                    error_setg(errp, "topology '%s' PCI hosts share node %u "
                               "widget %u", t->name, selected->node_index,
                               selected->widget);
                    return false;
                }
            }
        }
    }

    if (has_ip53) {
        for (i = 0; i < attachment_count; i++) {
            if (!owned_attachments[i]) {
                error_setg(errp, "topology '%s' IP53 construction has "
                           "unowned XIO attachment %zu", t->name, i);
                return false;
            }
        }
    }

    for (i = 0; i < board_count; i++) {
        const SGIModelIoBoard *board = boards[i];
        bool io9 = board->preset == SGI_MODEL_IO_BOARD_PRESET_IO9;

        if (board->system_timekeeper &&
            board->system_timekeeper->kind ==
            SGI_MODEL_SYSTEM_TIMEKEEPER_KIND_DS1744 && !io9) {
            error_setg(errp, "DS1744 timekeeper is only supported on IO9 "
                       "boards (found '%s')", board->id);
            return false;
        }

        if (io9) {
            size_t host_index;
            size_t key;

            has_io9 = true;
            io9_count++;
            if (board->has_attachment || !board->has_pci_host_index ||
                !board->has_pci_head) {
                error_setg(errp, "topology '%s' IO9 board '%s' has "
                           "inconsistent XIO and PCI references",
                           t->name, board->id);
                return false;
            }
            host_index = board->pci_host_index;
            if (host_index >= host_count) {
                error_setg(errp, "topology '%s' IO9 board '%s' has "
                           "out-of-range PCI host index %zu", t->name,
                           board->id, host_index);
                return false;
            }
            if (board->pci_head >= 2) {
                error_setg(errp, "topology '%s' IO9 board '%s' has "
                           "out-of-range PCI head %u", t->name,
                           board->id, board->pci_head);
                return false;
            }
            key = host_index * 2 + board->pci_head;
            if (owned_heads[key]) {
                error_setg(errp, "topology '%s' assigns PCI host/head pair "
                           "%zu/%u to multiple IO9 boards", t->name,
                           host_index, board->pci_head);
                return false;
            }
            owned_heads[key] = true;
            if (board->system_timekeeper) {
                SGIModelSystemTimekeeper *policy = board->system_timekeeper;

                if ((board->has_ds1742 && board->ds1742) ||
                    policy->kind !=
                    SGI_MODEL_SYSTEM_TIMEKEEPER_KIND_DS1744 ||
                    policy->initial_ip27_fixture ||
                    (policy->has_year_base && policy->year_base != 0 &&
                     policy->year_base != 1968)) {
                    error_setg(errp, "IO9 board '%s' has an unsupported "
                               "system timekeeper policy", board->id);
                    return false;
                }
            }
            if (board->has_ds1742 && board->ds1742) {
                const char *id;
                const char *cursor;

                for (id = board->id, cursor = id; *cursor; cursor++) {
                    if (!(*cursor >= 'a' && *cursor <= 'z') &&
                        !(*cursor >= '0' && *cursor <= '9') &&
                        *cursor != '-') {
                        error_setg(errp, "IO9 board '%s' has an invalid "
                                   "DS1742 owner ID", board->id);
                        return false;
                    }
                }
                if (board->id[0] < 'a' || board->id[0] > 'z') {
                    error_setg(errp, "IO9 board '%s' has an invalid DS1742 "
                               "owner ID", board->id);
                    return false;
                }
            }
            if (board->populated) {
                const SGIModelPciFunctionList *function_entry;
                bool has_ioc4 = false;
                bool has_isp12160 = false;
                bool has_bcm5701 = false;

                for (function_entry = board->pci_functions; function_entry;
                     function_entry = function_entry->next) {
                    const SGIModelPciFunction *function =
                        function_entry->value;
                    unsigned int devfn, route, route_index;
                    unsigned int expected_revision;

                    if (!function->has_pci_device ||
                        !function->has_pci_function ||
                        !function->has_intx_pin || !function->has_pic_input ||
                        function->pci_device < 1 ||
                        function->pci_device > 4 ||
                        function->pci_function > 7 ||
                        function->intx_pin != SGI_MODEL_PCI_INTX_PIN_INTA ||
                        function->pic_input >= 8) {
                        error_setg(errp, "topology '%s' IO9 board '%s' has "
                                   "incomplete PCI position or route",
                                   t->name, board->id);
                        return false;
                    }
                    switch (function->kind) {
                    case SGI_MODEL_PCI_FUNCTION_KIND_IOC4:
                        has_ioc4 = true;
                        expected_revision = 0x53;
                        if (!function->has_clock_hz ||
                            (function->clock_hz != 33333333 &&
                             function->clock_hz != 66666667) ||
                            !function->has_serial_dma_experimental) {
                            error_setg(errp, "topology '%s' IO9 board '%s' "
                                       "has incomplete IOC4 clock or DMA "
                                       "policy", t->name, board->id);
                            return false;
                        }
                        break;
                    case SGI_MODEL_PCI_FUNCTION_KIND_ISP12160:
                        has_isp12160 = true;
                        expected_revision = 0x06;
                        break;
                    case SGI_MODEL_PCI_FUNCTION_KIND_BCM5701:
                        has_bcm5701 = true;
                        expected_revision = 0x00;
                        break;
                    default:
                        error_setg(errp, "topology '%s' IO9 board '%s' has "
                                   "an unsupported PCI function",
                                   t->name, board->id);
                        return false;
                    }
                    if (function->revision != expected_revision) {
                        error_setg(errp, "topology '%s' IO9 board '%s' has "
                                   "unsupported PCI function revision",
                                   t->name, board->id);
                        return false;
                    }
                    devfn = function->pci_device * 8 + function->pci_function;
                    route = function->pci_device * 4 + function->intx_pin;
                    route_index = (host_index * 2 + board->pci_head) * 128 +
                                  route;
                    if (owned_devfns[(host_index * 2 + board->pci_head) *
                                     256 + devfn] ||
                        owned_routes[route_index]) {
                        error_setg(errp, "topology '%s' IO9 board '%s' has "
                                   "a duplicate PCI position or interrupt "
                                   "route", t->name, board->id);
                        return false;
                    }
                    owned_devfns[(host_index * 2 + board->pci_head) *
                                 256 + devfn] = true;
                    owned_routes[route_index] = true;
                }
                if (!board->pci_functions) {
                    error_setg(errp, "topology '%s' IO9 board '%s' has no "
                               "PCI functions", t->name, board->id);
                    return false;
                }
                if (!has_ioc4 || !has_isp12160 || !has_bcm5701) {
                    error_setg(errp, "topology '%s' IO9 board '%s' requires "
                               "IOC4, ISP12160 and BCM5701 functions",
                               t->name, board->id);
                    return false;
                }
            }
        } else if (!board->has_attachment || board->has_pci_host_index ||
                   board->has_pci_head) {
            error_setg(errp, "topology '%s' board '%s' has inconsistent "
                       "XIO and PCI references", t->name, board->id);
            return false;
        }
    }

    if (has_ip53 && ((!allow_ip53_fixture && !allow_ip53_experiment) ||
                     !all_ip53 || !host_count || !has_io9)) {
        error_setg(errp, "topology '%s' IP53 construction requires the "
                   "runtime experiment permission or a qtest fixture",
                   t->name);
        return false;
    }
    if (allow_ip53_experiment &&
        (!has_ip53 || !all_ip53 || node_count != 1 ||
         host_count != expected_host_count ||
         io9_count != 1)) {
        error_setg(errp, "IP53 runtime profile requires one IP53 node, "
                   "%zu PIC host%s and one IO9 board", expected_host_count,
                   expected_host_count == 1 ? "" : "s");
        return false;
    }
    if (host_count || has_io9) {
        if ((!allow_ip53_fixture && !allow_ip53_experiment) ||
            !has_ip53 || !all_ip53 ||
            !host_count || !has_io9) {
            error_setg(errp, "topology '%s' has unsupported PIC/IO9 "
                       "descriptors", t->name);
            return false;
        }
        for (i = 0; i < host_count; i++) {
            if (!hosts[i]->has_compatibility_profile ||
                hosts[i]->compatibility_profile !=
                    sgi_machine_ip53_pic_profile(t,
                                                 allow_ip53_experiment)) {
                error_setg(errp, "topology '%s' PCI host '%s' has no "
                           "compatibility profile", t->name, hosts[i]->id);
                return false;
            }
        }
        if (allow_ip53_experiment &&
            (node_count != 1 || host_count != expected_host_count)) {
            error_setg(errp, "IP53 runtime profile requires one node and "
                       "%zu PIC host%s", expected_host_count,
                       expected_host_count == 1 ? "" : "s");
            return false;
        }
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
        if (!sgi_machine_resolve_uint(s, t, SGI_INPUT_BOARD_ID, &used,
                config->has_board_id_word, config->board_id_word,
                &resolved->board_id_word, errp)) {
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
        if (!sgi_machine_resolve_uint(s, t, SGI_INPUT_BEDROCK_REVISION, &used,
                node->identity->has_widget_revision,
                node->identity->widget_revision, &value, errp)) {
            return false;
        }
        if (value > 15) {
            error_setg(errp, "%s must fit four bits",
                       sgi_machine_input_names[SGI_INPUT_BEDROCK_REVISION]);
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
        size_t owner_node = node_count;

        if (!config) {
            continue;
        }
        for (size_t n = 0; n < node_count; n++) {
            if (nodes[n]->asic_kind == SGI_MODEL_ASIC_KIND_BEDROCK &&
                s->node_config[n].controller_index == i) {
                owner_node = n;
                break;
            }
        }
        if (config->has_profile) {
            /*
             * A C-brick's L1 belongs to its node and an I-brick's to its
             * module alone; neither takes a type code or revision.
             */
            if ((config->profile == SGI_MODEL_L1_PROFILE_CBRICK) !=
                (owner_node != node_count) ||
                config->has_reported_type || config->revision ||
                config->mac_eeprom_input || config->cpu_context) {
                error_setg(errp, "topology '%s' controller '%s' has an "
                           "invalid L1 %s profile", t->name,
                           controllers[i]->id,
                           SGIModelL1Profile_str(config->profile));
                return false;
            }
            resolved->has_profile = true;
            resolved->profile = config->profile;
            for (unsigned int j = 0; j < ARRAY_SIZE(resolved->revision); j++) {
                resolved->revision[j] = -1;
            }
            continue;
        }
        if (owner_node == node_count) {
            error_setg(errp, "topology '%s' controller '%s' has L1 "
                       "configuration without a modeled Bedrock node",
                       t->name, controllers[i]->id);
            return false;
        }
        if (!sgi_machine_resolve_uint(s, t, SGI_INPUT_L1_TYPE, &used,
                config->has_reported_type, config->reported_type, &value,
                errp)) {
            return false;
        }
        if (!value || value > UINT8_MAX) {
            error_setg(errp, "%s must be nonzero and fit eight bits",
                       sgi_machine_input_names[SGI_INPUT_L1_TYPE]);
            return false;
        }
        resolved->reported_type = value;
        used |= 1U << SGI_INPUT_L1_REVISION;
        if (s->l1_revision_override_set) {
            memcpy(resolved->revision, s->l1_revision_override,
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
        if (config->cpu_context) {
            const SGIModelL1CPUContext *context = config->cpu_context;
            const SGIModelNode *node = nodes[owner_node];
            const SGIModelProcessor *processor = NULL;
            const SGIModelCpuSlotList *slot;

            for (slot = node->cpu_slots; slot; slot = slot->next) {
                if (slot->value->assembly) {
                    processor = slot->value->assembly;
                    break;
                }
            }
            if (!node->has_board_kind ||
                (node->board_kind != SGI_MODEL_BOARD_KIND_IP53 &&
                 node->board_kind != SGI_MODEL_BOARD_KIND_IP53_CX) ||
                !t->ip35_firmware ||
                context->cpu_mode_word != t->ip35_firmware->cpu_mode_word ||
                !processor || !processor->has_mode_bits ||
                (context->cpu_mode_word & 0x01ffffff) !=
                (processor->mode_bits & 0x01ffffff) ||
                context->cpu_clock_hz != processor->clock_hz ||
                !node->identity || !node->identity->has_clock_hz ||
                context->hub_clock_hz != node->identity->clock_hz ||
                !context->cpu_clock_hz || !context->hub_clock_hz ||
                context->cpu_clock_hz % 1000000 ||
                context->hub_clock_hz % 1000000 ||
                context->cpu_clock_hz / 1000000 > UINT32_MAX ||
                context->hub_clock_hz / 1000000 > UINT32_MAX) {
                error_setg(errp, "topology '%s' controller '%s' has an "
                           "invalid IP53 CPU context", t->name,
                           controllers[i]->id);
                return false;
            }
            resolved->has_cpu_context = true;
            resolved->cpu_mode_word = context->cpu_mode_word;
            resolved->cpu_clock_hz = context->cpu_clock_hz;
            resolved->hub_clock_hz = context->hub_clock_hz;
        }
    }
    for (i = 0; i < board_count; i++) {
        const SGIModelIoBoard *board = boards[i];
        SGIMachineIoConfig *resolved = &s->io_config[i];
        const SGIModelPciFunctionList *entry;
        const unsigned int required =
            (1U << SGI_MODEL_PCI_FUNCTION_KIND_ISP12160) |
            (1U << SGI_MODEL_PCI_FUNCTION_KIND_IOC3) |
            (1U << SGI_MODEL_PCI_FUNCTION_KIND_LUCENT_OHCI);
        unsigned int found = 0;
        uint64_t value;

        if (board->has_ieee1394_absent &&
            board->preset != SGI_MODEL_IO_BOARD_PRESET_IO7) {
            error_setg(errp, "topology '%s' board '%s' leaves out an IEEE "
                       "1394 controller; only an io7 board has one", t->name,
                       board->id);
            return false;
        }
        if (board->has_fibre_channel &&
            board->preset != SGI_MODEL_IO_BOARD_PRESET_IO7) {
            error_setg(errp, "topology '%s' board '%s' fits an I-brick Fibre "
                       "Channel card; only an io7 board's head carries one",
                       t->name, board->id);
            return false;
        }
        if (board->populated &&
            board->preset == SGI_MODEL_IO_BOARD_PRESET_IO7) {
            /*
             * No capture gives the IO7 functions' revisions or where its
             * Ethernet address comes from, so a record may name neither.
             */
            if (board->pci_functions ||
                (board->has_mac_source &&
                 board->mac_source != SGI_MODEL_MAC_SOURCE_NONE)) {
                error_setg(errp, "topology '%s' board '%s' names IO7 "
                           "function revisions or a MAC source, which no "
                           "source gives", t->name, board->id);
                return false;
            }
            if (!sgi_machine_resolve_uint(s, t, SGI_INPUT_IOC3_SUBSYSTEM,
                    &used, board->has_ioc3_subsystem_id,
                    board->ioc3_subsystem_id, &value, errp)) {
                return false;
            }
            if (value > UINT16_MAX) {
                error_setg(errp, "topology '%s' board '%s' has invalid IOC3 "
                           "identity configuration", t->name, board->id);
                return false;
            }
            s->io_config[i].ioc3_subsystem_id = value;
            continue;
        }
        if (!board->populated ||
            (board->preset != SGI_MODEL_IO_BOARD_PRESET_IO8 &&
             board->preset != SGI_MODEL_IO_BOARD_PRESET_FUEL)) {
            continue;
        }
        if (!sgi_machine_resolve_uint(s, t, SGI_INPUT_IOC3_SUBSYSTEM, &used,
                board->has_ioc3_subsystem_id, board->ioc3_subsystem_id,
                &value, errp)) {
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
        if (found != required) {
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

/*
 * The processor every node fits: processor='s option, or the nodes' own
 * part. Where the topology offers a choice, -cpu must name the chosen
 * module's part, since the catalogue profile and the flash configuration
 * block both describe that part.
 */
static bool sgi_machine_resolve_processor(SGIMachineState *s,
                                          const SGIModelTopology *t,
                                          Error **errp)
{
    MachineState *machine = MACHINE(s);
    const char *name = MACHINE_GET_CLASS(s)->name;
    const SGIModelCpuSlotList *slots = sgi_machine_topology_node0(t)->cpu_slots;
    g_autofree char *cpu_model = NULL;

    s->processor = slots ? slots->value->assembly : NULL;
    s->processor_option = t->processor_options ?
                          t->processor_options->value : NULL;
    if (s->processor_name) {
        s->processor_option = sgi_model_find_processor_option(
            t, s->processor_name, errp);
        if (!s->processor_option) {
            error_prepend(errp, "%s: ", name);
            return false;
        }
        s->processor = s->processor_option->assembly;
    }
    if (!s->processor_option) {
        return true;
    }
    cpu_model = cpu_model_from_type(machine->cpu_type);
    if (!g_str_equal(cpu_model, s->processor->kind)) {
        error_setg(errp, "%s: processor '%s' is an %s, but -cpu names %s; "
                   "pass -cpu %s, or name a module of that part with "
                   "processor=", name, s->processor->id, s->processor->kind,
                   cpu_model, s->processor->kind);
        return false;
    }
    return true;
}

/*
 * Whether processor='s module comes with @cpus processors on a node: a
 * real Origin 200 shows each module only in the sizes its option lists.
 */
static bool sgi_machine_processor_fits(const SGIMachineState *s,
                                       unsigned int cpus)
{
    const uint32List *entry;

    if (!s->processor_option) {
        return true;
    }
    for (entry = s->processor_option->cpus_per_node; entry;
         entry = entry->next) {
        if (entry->value == cpus) {
            return true;
        }
    }
    return false;
}

static bool sgi_machine_realize(MachineState *machine, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    SGIMachineClass *smc = SGI_MACHINE_GET_CLASS(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    Error *local_err = NULL;
    const SGIModelTopology *t;
    const SN0ModelMemory *node_memory =
        smc->node_memory ? smc->node_memory : &sn0_hub_memory;
    g_autofree unsigned int *requested = NULL;
    g_autofree unsigned int *node_attachment = NULL;
    g_autofree unsigned int *board_attachment = NULL;
    g_autofree BlockBackend **flash_drives = NULL;
    g_autofree const char **flash_nodes = NULL;
    g_autofree const SGIModelNode **nodes = NULL;
    g_autofree uint64_t *node_ram_size = NULL;
    g_autofree uint64_t *node_ram_offset = NULL;
    const SGIModelNode *refused;
    unsigned int flash_count = 0;
    unsigned int node_count, node_index, board_index;
    unsigned int cpu_index = 0, cpu_base = 0;
    size_t io_board_count = 0, crossbar_count = 0, node_count_check;
    uint64_t ram_offset = 0;
    SGIModelAsicKind kind;
    bool allow_ip53_experiment = false;
    bool allow_ip53_fixture = smc->test_topology && qtest_enabled();

    if (!sgi_machine_settle_default_ram(machine, errp)) {
        return false;
    }
    t = sgi_machine_resolve(machine, &local_err);
    if (!t && sgi_machine_chassis(machine)) {
        error_propagate_prepend(errp, local_err, "%s: ", mc->name);
        return false;
    }
    if (!t) {
        GString *names = g_string_new(NULL);
        const SGIModelTopologyList *entry;

        error_free(local_err);

        for (entry = sgi_machines()->topologies; entry;
             entry = entry->next) {
            if (sgi_model_topology_matches_product(entry->value,
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
    if (!sgi_machine_ip53_experiment_policy(
            s, smc, t, &allow_ip53_experiment, errp)) {
        return false;
    }
    if (!sgi_machine_validate_board_kinds(
            t, allow_ip53_fixture || allow_ip53_experiment,
            allow_ip53_experiment, errp) ||
        !sgi_machine_resolve_config(s, t, errp) ||
        !sgi_storage_check(s->storage, MACHINE_GET_CLASS(s)->name,
                           SGI_MACHINE_GET_CLASS(s)->product, t, errp)) {
        return false;
    }
    s->ip53_fingerprint_enabled = sgi_machine_topology_has_ip53(t);
    if (!sgi_machine_validate_controller_inputs(s, t, errp) ||
        !sgi_machine_validate_expansion_storage(s, t, errp) ||
        !sgi_machine_resolve_processor(s, t, errp)) {
        return false;
    }
    if (s->processor) {
        node_memory = sn0_model_board_memory(node_memory,
                                             s->processor->clock_hz);
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
            error_report("%s: chassis-eeprom/board-eeprom/spd-eeprom "
                        "name an L1 identity controller, which topology '%s' "
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
    nodes = sgi_model_node_array(t->nodes, &node_count_check);
    g_assert(node_count_check == node_count);
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
            if (sgi_machine_node_is_memory(nodes[node_index])) {
                if (requested[node_index]) {
                    error_setg(errp, "%s: topology '%s' population fits "
                               "processors to memory node %u", mc->name,
                               t->name, node_index);
                    return false;
                }
                continue;
            }
            if (!sgi_machine_processor_fits(s, requested[node_index])) {
                error_setg(errp, "%s: no %u-processor module of processor "
                          "'%s' is known; give -smp a population its "
                          "modules make", mc->name, requested[node_index],
                          s->processor->id);
                return false;
            }
        }
    }

    node_ram_size = g_new0(uint64_t, node_count);
    node_ram_offset = g_new0(uint64_t, node_count);
    {
        bool heterogeneous = false;
        uint64_t aggregate_mib = 0;

        for (node_index = 0; node_index < node_count; node_index++) {
            const uint64List *bank;
            uint64_t total_mib = 0;

            for (bank = nodes[node_index]->bank_mib; bank;
                 bank = bank->next) {
                if (uadd64_overflow(total_mib, bank->value, &total_mib)) {
                    error_setg(errp, "%s: topology '%s' node %u bank total "
                               "overflows", mc->name, t->name, node_index);
                    return false;
                }
            }
            if (node_index && total_mib != node_ram_size[0] / MiB) {
                heterogeneous = true;
            }
            if (total_mib > UINT64_MAX / MiB ||
                uadd64_overflow(aggregate_mib, total_mib, &aggregate_mib)) {
                error_setg(errp, "%s: topology '%s' memory total overflows",
                           mc->name, t->name);
                return false;
            }
            node_ram_size[node_index] = total_mib * MiB;
        }

        if (heterogeneous) {
            if (!s->ip53_fingerprint_enabled ||
                sgi_machine_topology_node0(t)->asic_kind !=
                    SGI_MODEL_ASIC_KIND_BEDROCK) {
                error_setg(errp, "%s: topology '%s' has unsupported "
                           "heterogeneous node memory", mc->name, t->name);
                return false;
            }
            for (node_index = 0; node_index < node_count; node_index++) {
                size_t accepted_count, i;
                g_autofree uint64_t *accepted =
                    sgi_machine_node_accepted_mib(nodes[node_index],
                                                  s->processor_option,
                                                  &accepted_count);
                bool found = false;

                if (nodes[node_index]->asic_kind !=
                        SGI_MODEL_ASIC_KIND_BEDROCK ||
                    !nodes[node_index]->has_board_kind ||
                    nodes[node_index]->board_kind !=
                        SGI_MODEL_BOARD_KIND_IP53 ||
                    !node_ram_size[node_index]) {
                    error_setg(errp, "%s: topology '%s' has unsupported "
                               "heterogeneous node memory", mc->name,
                               t->name);
                    return false;
                }
                if (requested[node_index] !=
                    QAPI_LIST_LENGTH(nodes[node_index]->cpu_slots)) {
                    error_setg(errp, "%s: topology '%s' heterogeneous "
                               "memory requires the descriptor processor "
                               "population", mc->name, t->name);
                    return false;
                }
                for (i = 0; i < accepted_count; i++) {
                    if (accepted[i] == node_ram_size[node_index] / MiB) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    g_autofree char *accepted_text =
                        sgi_machine_accepted_memory(nodes[node_index],
                                                    s->processor_option);

                    error_setg(errp, "%s: topology '%s' node %u has "
                               "unsupported descriptor memory size of %" PRIu64
                               " MiB; it accepts %s MiB", mc->name, t->name,
                               node_index, node_ram_size[node_index] / MiB,
                               accepted_text);
                    return false;
                }
                if (node_index && uadd64_overflow(
                        node_ram_offset[node_index - 1],
                        node_ram_size[node_index - 1],
                        &node_ram_offset[node_index])) {
                    error_setg(errp, "%s: topology '%s' memory offsets "
                               "overflow", mc->name, t->name);
                    return false;
                }
            }
            if (aggregate_mib > UINT64_MAX / MiB ||
                aggregate_mib * MiB != machine->ram_size) {
                error_setg(errp, "%s: topology '%s' descriptor bank total "
                           "does not match -m", mc->name, t->name);
                return false;
            }
        } else {
            if (node_count > 1 && machine->ram_size %
                ((uint64_t)node_count * MiB)) {
                error_setg(errp, "%s: topology '%s' splits -m evenly across "
                           "%u nodes", mc->name, t->name, node_count);
                return false;
            }

            refused = sgi_machine_refuse_memory_population(
                t, s->processor_option,
                machine->ram_size / node_count / MiB);
            if (refused) {
                g_autofree char *accepted =
                    sgi_machine_accepted_memory(refused, s->processor_option);

                error_setg(errp, "%s: topology '%s' does not accept %" PRIu64
                           " MiB per node, only %s MiB%s", mc->name, t->name,
                           machine->ram_size / node_count / MiB, accepted,
                           sgi_machine_nodes_hint(machine));
                return false;
            }
            for (node_index = 0; node_index < node_count; node_index++) {
                node_ram_size[node_index] = machine->ram_size / node_count;
                node_ram_offset[node_index] = node_index ?
                    node_ram_offset[node_index - 1] +
                    node_ram_size[node_index - 1] : 0;
            }
        }
    }

    if (allow_ip53_experiment && !s->ip53_experimental &&
        !sgi_machine_ip53_normal_tuple(
            t, machine, requested, node_count, nodes, s->processor,
            s->processor_option)) {
        error_setg(errp, "%s: topology '%s' does not match an approved normal "
                   "IP53 serial profile tuple; pass ip53-experimental=on for "
                   "an already-admitted experimental profile", mc->name,
                   t->name);
        return false;
    }
    s->ip53_experiment_authorized = allow_ip53_experiment;

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
    clock_set_hz(s->cpu_refclk, s->processor->clock_hz);

    sgi_machine_build_module_containers(machine, t);

    kind = sgi_machine_topology_node0(t)->asic_kind;
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

    if (kind != SGI_MODEL_ASIC_KIND_HUB &&
        (!sgi_machine_create_pic_hosts(s, t, errp) ||
         !sgi_machine_create_enclosure_xbridges(s, t, errp) ||
         !sgi_machine_supply_module_l1s(s, t, errp))) {
        return false;
    }
    if (s->ip53_fingerprint_enabled) {
        uint8_t pxbow_revision = 0;

        if (allow_ip53_experiment) {
            size_t i;

            if (!s->pic_host_count) {
                error_setg(errp, "IP53 PXBOW composition has no PIC host");
                return false;
            }
            pxbow_revision = sgi_pic_pxbow_revision(
                SGI_PIC(s->pic_hosts[0]));
            for (i = 1; i < s->pic_host_count; i++) {
                if (sgi_pic_pxbow_revision(
                        SGI_PIC(s->pic_hosts[i])) != pxbow_revision) {
                    error_setg(errp, "IP53 PIC hosts have different PXBOW "
                               "revisions");
                    return false;
                }
            }
        }
        if (!sgi_machine_ip53_fingerprint(t, s->node_config, s->l1_config,
                                          allow_ip53_experiment,
                                          pxbow_revision,
                                          s->ip53_local_fingerprint, errp)) {
            return false;
        }
    }

    if (kind != SGI_MODEL_ASIC_KIND_HUB) {
        g_autofree const SGIModelXioAttachment **attachments = NULL;
        size_t attachment_count;

        size_t bedrock_board_capacity = sgi_machine_topology_io_board_count(t);

        s->io_boards = g_new0(SGIXioBoardState *, bedrock_board_capacity);
        s->board_links = g_new0(SGIXioLink, bedrock_board_capacity);
        sgi_machine_mark_pipe_records(s, t, NULL);

        attachments = sgi_model_xio_attachment_array(t->xio_attachments,
                                                     &attachment_count);
        for (node_index = 0; node_index < node_count; node_index++) {
            SN1NodeState *node = SN1_NODE(s->bedrock_nodes[node_index]);
            g_autofree unsigned int *widgets = NULL;
            size_t widget_count = 0;
            size_t i;
            const SGIModelIoBoardList *board_entry;
            bool found_board = false;
            bool ip53 = nodes[node_index]->has_board_kind &&
                nodes[node_index]->board_kind == SGI_MODEL_BOARD_KIND_IP53;
            bool ip35 = sgi_machine_node_is_cabled(nodes[node_index]);

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
            if (ip53) {
                if (!sn1_node_set_ip53_assembly_validated(node, errp)) {
                    return false;
                }
            } else if (ip35) {
                /*
                 * No I/O of its own: a cabled XBridge decodes this node's
                 * attachments in the other brick.
                 */
                node->xbridge_widgets = g_steal_pointer(&widgets);
                node->xbridge_widget_count = widget_count;
            } else {
            for (const SGIModelCableList *entry = t->cables; entry;
                 entry = entry->next) {
                const SGIModelLinkEndList *ends = entry->value->ends;

                if (QAPI_LIST_LENGTH(ends) != 2) {
                    continue;
                }
                for (unsigned side = 0; side < 2; side++) {
                    const SGIModelLinkEnd *own = side ?
                        ends->next->value : ends->value;
                    const SGIModelLinkEnd *peer = side ?
                        ends->value : ends->next->value;

                    if (own->kind == SGI_MODEL_LINK_END_KIND_NODE &&
                        own->index == node_index &&
                        peer->kind == SGI_MODEL_LINK_END_KIND_ROUTER) {
                        node->ni_route = true;
                    }
                }
            }
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
                qdev_prop_set_uint64(DEVICE(node), "io-board-serial",
                                     board->serial);
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
            /*
             * A graphics board wired straight to this node's XBridge
             * occupies the chip's external port at its own widget: 0xb
             * behind the Origin 300's cable connector, 13 in a Fuel.
             */
            for (board_entry = t->io_boards; board_entry;
                 board_entry = board_entry->next) {
                const SGIModelIoBoard *board = board_entry->value;
                const SGIModelXioAttachment *placement;

                if (board->preset != SGI_MODEL_IO_BOARD_PRESET_ODYSSEY ||
                    !board->populated ||
                    board->attachment >= attachment_count) {
                    continue;
                }
                placement = attachments[board->attachment];
                if (placement->node_index == node_index &&
                    placement->crossbar_index < 0) {
                    qdev_prop_set_uint8(DEVICE(node), "xbridge-xio-port",
                                        placement->widget);
                }
            }
            node->xbridge_widgets = g_steal_pointer(&widgets);
            node->xbridge_widget_count = widget_count;
            }
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
                                         node_ram_offset[node_index],
                                         node_ram_size[node_index]);
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
                if (t->ip35_firmware) {
                    qdev_prop_set_uint32(DEVICE(node), "firmware-machine-type",
                                         t->ip35_firmware->machine_type);
                    qdev_prop_set_uint32(DEVICE(node), "firmware-mode-word",
                                         t->ip35_firmware->cpu_mode_word);
                }
                if (t->ip53_runtime) {
                    qdev_prop_set_bit(DEVICE(node), "l1-irouter-compat",
                                      t->ip53_runtime->l1_irouter_compat);
                }
                qdev_prop_set_uint8(DEVICE(node), "bedrock-revision",
                                    config->revision);
                qdev_prop_set_uint8(DEVICE(node), "local-region",
                                    config->local_region);
                qdev_prop_set_uint8(DEVICE(node), "initiator-region",
                                    config->initiator_region);
                qdev_prop_set_uint8(DEVICE(node), "installed-bank-mask",
                                    config->installed_bank_mask);
                if (l1->has_profile) {
                    qdev_prop_set_string(DEVICE(node), "l1-brick-type",
                                         SGIModelL1Profile_str(l1->profile));
                }
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
            {
                g_autofree char *name =
                    g_strdup_printf("node%u-flash", node_index);
                const char *flash = sgi_machine_storage_node(s, name);

                if (flash) {
                    qdev_prop_set_string(DEVICE(node), "flash-node", flash);
                }
            }
            qdev_prop_set_uint32(DEVICE(node), "l1-bay",
                                 sgi_machine_node_module(t, node_index) ?
                                 sgi_machine_node_module(t, node_index)->
                                 initial_module_number : 0);
            if (nodes[node_index]->has_option_intx_map) {
                uint8_t map[SGI_PCI_INTX_MAP_SIZE];
                const uint8List *entry;
                unsigned index = 0;

                for (entry = nodes[node_index]->option_intx_map; entry;
                     entry = entry->next) {
                    if (index == ARRAY_SIZE(map)) {
                        error_setg(errp, "compute option INTx map is too long");
                        return false;
                    }
                    map[index++] = entry->value;
                }
                if (index != ARRAY_SIZE(map)) {
                    error_setg(errp, "compute option INTx map requires "
                               "eight inputs");
                    return false;
                }
                if (!sn1_node_set_option_intx_map(node, map, errp)) {
                    return false;
                }
            }
            qdev_connect_clock_in(DEVICE(node), "cpu-refclk", s->cpu_refclk);
        }

        for (node_index = 0; node_index < node_count; node_index++) {
            SN1NodeState *node = SN1_NODE(s->bedrock_nodes[node_index]);
            size_t i;
            bool ip53 = nodes[node_index]->has_board_kind &&
                nodes[node_index]->board_kind == SGI_MODEL_BOARD_KIND_IP53;

            sysbus_realize(SYS_BUS_DEVICE(node), &error_fatal);
            if (ip53 && !sgi_machine_set_pic_widget_views(
                    s, t, node_index, errp)) {
                return false;
            }
            for (i = 0; i < requested[node_index]; i++) {
                machine->possible_cpus->cpus[cpu_index++].cpu =
                    CPU(node->cpu[i]);
            }
        }
        g_assert(cpu_index == machine->smp.cpus);
        if (!sgi_machine_build_sn1_controllers(s, t, errp) ||
            !sgi_machine_build_io_assemblies(s, t, errp)) {
            return false;
        }
        /*
         * The NUMAlink cables join realized Bedrocks: each node creates its
         * Bedrock while it realizes, and a Bedrock's cable reference may be
         * set afterwards. Then every node derives which numbers it reaches.
         */
        if (t->routers) {
            s->sn1_memory_mappings = sn1_memory_mappings_new(s->bedrock_nodes,
                s->num_nodes, s->sn1_routers, s->router_count);
        }
        if (!sgi_machine_build_bedrock_links(s, t, errp)) {
            return false;
        }
        if (!sgi_machine_build_ibrick_l1s(s, t, errp) ||
            !sgi_machine_build_l1_peer_links(s, t, errp)) {
            return false;
        }
        for (node_index = 0; node_index < node_count; node_index++) {
            SN1NodeState *node = SN1_NODE(s->bedrock_nodes[node_index]);

            sn1_node_update_claims(node);
            sgi_bedrock_lock_net_cable(SGI_BEDROCK(node->bedrock));
        }
        for (unsigned i = 0; i < s->router_count; i++) {
            sgi_sn1_router_lock_cables(s->sn1_routers[i]);
        }
        for (unsigned i = 0; i < s->io_peer_count; i++) {
            SN1FabricEndpoint *endpoint = s->io_peer_notifiers[i].endpoint;

            if (endpoint) {
                sgi_bedrock_lock_net_cable(endpoint->bedrock);
            }
        }
        sn1_memory_mappings_refresh(s->sn1_memory_mappings, NULL);

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

                if (record->preset == SGI_MODEL_IO_BOARD_PRESET_IO9 ||
                    sgi_machine_board_cable(t, board_index)) {
                    continue;
                }
                if (record->attachment >= attachment_count) {
                    error_setg(errp, "topology '%s' io board %u names "
                              "attachment %u, past its %zu XIO "
                              "attachment(s)", t->name, board_index,
                              record->attachment, attachment_count);
                    return false;
                }
                if (attachments[record->attachment]->node_index >= node_count ||
                    (attachments[record->attachment]->crossbar_index >= 0 &&
                     record->preset != SGI_MODEL_IO_BOARD_PRESET_IO7) ||
                    (attachments[record->attachment]->crossbar_index < 0 &&
                     record->preset == SGI_MODEL_IO_BOARD_PRESET_IO7)) {
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
        if (!sgi_machine_build_board_cables(s, t, errp) ||
            !sgi_machine_build_pic_io9(s, t, errp)) {
            return false;
        }
        if (!sgi_machine_load_flash(machine, t, s, errp)) {
            return false;
        }
        sgi_machine_register_ip53_fingerprint(s);
        return true;
    }

    flash_drives = g_new0(BlockBackend *, node_count);
    flash_nodes = g_new0(const char *, node_count);
    for (node_index = 0; node_index < node_count; node_index++) {
        DriveInfo *drive = drive_get_by_index(IF_PFLASH, node_index);
        g_autofree char *name = g_strdup_printf("node%u-flash", node_index);

        flash_nodes[node_index] = sgi_machine_storage_node(s, name);
        if (drive && flash_nodes[node_index]) {
            error_setg(errp, "%s: %s and -drive if=pflash,unit=%u both "
                       "back node %u's flash", mc->name, name, node_index,
                       node_index);
            return false;
        }
        if (drive) {
            flash_drives[node_index] = blk_by_legacy_dinfo(drive);
        }
        if (drive || flash_nodes[node_index]) {
            flash_count++;
        }
    }
    /* A qtest of the Hub's registers may start with no PROM at all. */
    if (!sgi_storage_check_boot_prom(mc->name, machine->firmware, flash_count,
                                     node_count, qtest_enabled(), errp)) {
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
        node->flash_node = flash_nodes[node_index];
        sgi_machine_attach_elsc_nvram(s, t, node_index, node);
        node->model_node = model_node;
        node->memory = node_memory;
        node->slave_role = model_node->slave_role;
        qdev_prop_set_uint32(DEVICE(node), "cpu-base", cpu_base);
        qdev_prop_set_uint32(DEVICE(node), "nr-cpus", requested[node_index]);
        qdev_prop_set_string(DEVICE(node), "cpu-type", machine->cpu_type);
        node->processor = s->processor;
        if (sgi_machine_topology_is_ip29(t)) {
            sgi_machine_attach_ip29_pimm(node, s->processor,
                                         requested[node_index],
                                         model_node->identity->serial);
        } else {
            sgi_machine_attach_ip27_node(node, model_node->identity->serial);
        }
        object_property_set_link(OBJECT(node), "ram", OBJECT(machine->ram),
                                 &error_abort);
        qdev_prop_set_uint64(DEVICE(node), "ram-start", ram_offset);
        qdev_prop_set_array(DEVICE(node), "banks-mb", banks_mb);
        qdev_connect_clock_in(DEVICE(node), "cpu-refclk", s->cpu_refclk);
        qdev_connect_clock_in(DEVICE(node), "rt-refclk", s->rt_refclk);

        qdev_prop_set_uint32(DEVICE(&node->hub), "nasid", 0);
        qdev_prop_set_uint8(DEVICE(&node->hub), "elsc-dip-switches",
                            s->elsc_dip_switches);
        qdev_prop_set_uint32(DEVICE(&node->hub), "elsc-start-delay-ms",
                             s->module_start_delay_ms);
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

    sgi_machine_mark_pipe_records(s, t, board_attachment);
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
    sgi_machine_register_ip53_fingerprint(s);
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
 *
 * It first fits the topology's default graphics pipe, when no -device was
 * a pipe and -vga none did not ask for none: every -device exists by now,
 * and this is the first point that is known. The pipe claims its slot the
 * way a -device does, so a Crossbow-attached one comes up here too. A pipe
 * on the IP53 PIC fabric is marked alive on the PXBOW, and the node's
 * processor window for its widget routed to it, the same way.
 */
static void sgi_machine_done(Notifier *notifier, void *data)
{
    SGIMachineState *s = container_of(notifier, SGIMachineState,
                                      machine_done);
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));
    g_autofree const SGIModelXioSlot **slots = NULL;
    g_autofree const SGIModelIoBoard **records = NULL;
    size_t slot_count, record_count, i;

    sgi_graphics_pipes_done(&s->pipes);
    if (s->module_start) {
        vmstate_register_any(NULL, &vmstate_sgi_machine_module_starts, s);
        s->module_starts_registered = true;
    }
    slots = sgi_model_xio_slot_array(t->xio_slots, &slot_count);
    for (i = 0; i < slot_count; i++) {
        if (!sgi_xio_link_complete(&s->slot_links[i])) {
            continue;
        }
        sgi_xbow_mark_widget_alive(s->crossbars[slots[i]->crossbar_index],
                                   slots[i]->widget, true, &error_fatal);
    }
    records = sgi_model_io_board_array(t->io_boards, &record_count);
    for (i = 0; i < record_count; i++) {
        const SGIModelXioAttachment *attachment;

        if (!s->pipe_claimed || !s->pipe_claimed[i]) {
            continue;
        }
        attachment = s->pipe_record_attachment[i];
        /* An XBridge marks a link present when the widget attaches. */
        if (!s->enclosure_xbridges && attachment->crossbar_index >= 0) {
            sgi_xbow_mark_widget_alive(
                s->crossbars[attachment->crossbar_index], attachment->widget,
                true, &error_fatal);
        }
    }
    if (s->pic_pipe && sgi_xio_link_complete(&s->pic_pipe_link)) {
        SGIPICState *pic = SGI_PIC(s->pic_hosts[s->ip53_widget_zero_host]);
        MemoryRegion *route;

        sgi_xbow_mark_widget_alive(sgi_pic_pxbow(pic), s->pic_pipe_widget,
                                   true, &error_fatal);
        route = sgi_pic_routed_widget(pic, s->pic_pipe_widget, &error_fatal);
        sn1_node_set_ip53_pic_widget(SN1_NODE(s->bedrock_nodes[0]),
                                     s->pic_pipe_widget, route, &error_fatal);
    }
    if (s->bedrock_nodes) {
        g_autofree SN1NodeState **nodes = g_new(SN1NodeState *, s->num_nodes);

        for (i = 0; i < s->num_nodes; i++) {
            nodes[i] = SN1_NODE(s->bedrock_nodes[i]);
        }
        sn1_register_reset_notifiers(nodes, s->num_nodes);
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

/*
 * A pipe claiming the topology's own pipe record @index (see
 * sgi_machine_pipe_preset()): link it to the record's port, a Crossbow
 * port, a port of an I/O brick's XBridge its cable meets, a node's
 * XBridge external port or the experimental V-brick XBridge's port 8,
 * and give it the record's module, its name for the board's QOM child and
 * qdev id (io4, graphics-slot, vbrick-v12, as the builder used to, or
 * the catalogue id on an I/O brick's crossbar) and the identity serials
 * the board's own defaults already carry.
 */
static SGIXioLink *sgi_machine_claim_pipe_record(SGIMachineState *s,
                                                 const SGIModelTopology *t,
                                                 size_t index, Object *board,
                                                 SGIXioSlotPlace *place,
                                                 Error **errp)
{
    g_autofree const SGIModelIoBoard **records = NULL;
    const SGIModelXioAttachment *attachment =
        s->pipe_record_attachment[index];
    SGIXioLink *link = &s->board_links[index];
    g_autofree char *link_name = g_strdup_printf("io-board%zu-link", index);
    g_autofree char *slot = NULL;
    const SGIModelIoBoard *record;
    size_t count;

    records = sgi_model_io_board_array(t->io_boards, &count);
    record = records[index];
    slot = sgi_machine_pipe_record_slot(s, record, index);
    if (s->pipe_claimed[index]) {
        error_setg(errp, "graphics slot %s already holds a pipe", slot);
        return NULL;
    }
    if (record->preset == SGI_MODEL_IO_BOARD_PRESET_VBRICK) {
        sgi_xio_link_init(link, OBJECT(s), link_name,
                          OBJECT(s->vbrick_bridge), 8, board, 0);
        place->widget = 8;
        place->name = g_strdup("vbrick-v12");
    } else if (s->enclosure_xbridges && attachment->crossbar_index >= 0) {
        /*
         * Validation put the record's attachment at the crossbar port its
         * cable meets, and the hosting node's Bedrock decodes that widget.
         * The XBridge takes a widget on a link port after it realizes.
         */
        sgi_xio_link_init(
            link, OBJECT(s), link_name,
            OBJECT(s->enclosure_xbridges[attachment->crossbar_index]),
            attachment->widget, board, 0);
        sgi_machine_link_board_pi(s, board, attachment->node_index);
        place->widget = attachment->widget;
        place->name = g_strdup(record->id);
    } else if (attachment->crossbar_index >= 0) {
        sgi_xio_link_init(link, OBJECT(s), link_name,
                          OBJECT(s->crossbars[attachment->crossbar_index]),
                          attachment->widget, board, 0);
        sgi_machine_link_board_pi(s, board, attachment->node_index);
        place->widget = attachment->widget;
        place->name = sgi_machine_xio_board_name(t, record, index);
    } else {
        SN1NodeState *node =
            SN1_NODE(s->bedrock_nodes[attachment->node_index]);

        if (attachment->widget !=
            sgi_xbridge_xio_port(SGI_XBRIDGE(node->xbridge))) {
            error_setg(errp, "graphics slot %s must be the node XBridge's "
                       "external port", slot);
            return NULL;
        }
        sgi_xio_link_init(link, OBJECT(s), link_name, OBJECT(node->xbridge),
                          attachment->widget, board, 0);
        sgi_machine_link_board_pi(s, board, attachment->node_index);
        place->widget = attachment->widget;
        place->name = sgi_machine_xio_board_name(t, record, index);
    }
    s->pipe_claimed[index] = true;
    s->io_boards[index] = SGI_XIO_BOARD(board);
    place->module = record->module_index;
    place->position = 0;
    place->label = g_strdup(record->label ? record->label : slot);
    place->own_identity = true;
    return link;
}

/*
 * A pipe claiming graphics slot @name, widget-<n>, on the IP53 PIC fabric:
 * linked to that port of the PXBOW of the PIC that owns widget 0, as the
 * quad Tezro and Onyx 350 captures place a VPro at widget 12. It takes
 * the node's graphics credits like any XIO board; sgi_machine_done()
 * marks the widget alive and routes the node's processor window to it.
 */
static SGIXioLink *sgi_machine_claim_pic_pipe(SGIMachineState *s,
                                              Object *board, const char *name,
                                              SGIXioSlotPlace *place,
                                              Error **errp)
{
    g_autofree char *link_name = g_strdup_printf("%s-link", name);
    SGIXbowState *pxbow;
    unsigned int widget;

    if (qemu_strtoui(name + strlen("widget-"), NULL, 10, &widget) ||
        widget >= 16) {
        error_setg(errp, "graphics slot %s is not a PIC widget", name);
        return NULL;
    }
    if (s->ip53_widget_zero_host >= s->pic_host_count) {
        error_setg(errp, "graphics slot %s has no PIC that owns widget 0",
                   name);
        return NULL;
    }
    pxbow = sgi_pic_pxbow(SGI_PIC(s->pic_hosts[s->ip53_widget_zero_host]));
    if (!pxbow) {
        error_setg(errp, "graphics slot %s has no PIC PXBOW", name);
        return NULL;
    }
    if (s->pic_pipe) {
        error_setg(errp, "graphics slot %s already holds a pipe", name);
        return NULL;
    }
    sgi_xio_link_init(&s->pic_pipe_link, OBJECT(s), link_name,
                      OBJECT(pxbow), widget, board, 0);
    sgi_machine_link_board_pi(s, board, 0);
    s->pic_pipe = board;
    s->pic_pipe_widget = widget;
    place->module = 0;
    place->widget = widget;
    place->position = 0;
    place->name = g_strdup(name);
    place->label = g_strdup(name);
    place->own_identity = true;
    return &s->pic_pipe_link;
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

    found = sgi_machine_find_pipe_record(s, t, name);
    if (found >= 0) {
        return sgi_machine_claim_pipe_record(s, t, found, board, place,
                                             errp);
    }
    if (s->ip53_widget_zero_host_valid && g_str_has_prefix(name, "widget-")) {
        return sgi_machine_claim_pic_pipe(s, board, name, place, errp);
    }

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

bool sgi_machine_ip53_experiment_authorized(MachineState *machine)
{
    return SGI_MACHINE(machine)->ip53_experiment_authorized;
}

/*
 * The node board that controls the slot pipe @dev names: a pipe record's
 * own node, the controlling node the live topology gives an empty XIO
 * slot, or node 0 for the IP53 PIC fabric's graphics slot.
 */
static const char *sgi_machine_pipe_host(void *opaque, DeviceState *dev)
{
    SGIMachineState *s = opaque;
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));
    g_autofree const SGIModelNode **nodes = NULL;
    const SGIModelXioSlotList *entry;
    g_autofree char *name = NULL;
    ssize_t record;
    int64_t node = -1;
    size_t count;

    if (!object_property_find(OBJECT(dev), "slot")) {
        return NULL;
    }
    name = object_property_get_str(OBJECT(dev), "slot", &error_abort);
    record = sgi_machine_find_pipe_record(s, t, name);
    if (record >= 0) {
        node = s->pipe_record_attachment[record]->node_index;
    } else if (name && g_str_has_prefix(name, "widget-") && s->pic_hosts) {
        node = 0;
    }
    for (entry = t->xio_slots; node < 0 && name && entry;
         entry = entry->next) {
        g_autofree char *slot = sgi_machine_xio_slot_name(t, entry->value);

        if (g_str_equal(slot, name)) {
            node = entry->value->controlling_node;
        }
    }
    nodes = sgi_model_node_array(t->nodes, &count);
    return node >= 0 && (uint64_t)node < count ? nodes[node]->id : NULL;
}

/*
 * Check -vga and create the topology's default pipe, which
 * sgi_machine_done() realizes once it knows no -device was a pipe. It is
 * created now, while the machine initializes, so it is not taken for a
 * hot-plugged device when it realizes.
 */
static bool sgi_machine_init_graphics(SGIMachineState *s, Error **errp)
{
    const SGIModelTopology *t = sgi_machine_topology(MACHINE(s));

    s->pipes.host_node = sgi_machine_pipe_host;
    s->pipes.opaque = s;
    return sgi_graphics_pipes_init(&s->pipes, MACHINE_GET_CLASS(s)->name,
                                   sgi_machine_chassis(MACHINE(s)),
                                   QAPI_LIST_LENGTH(t->nodes),
                                   s->uncatalogued_graphics, errp);
}

/* The machine judges the graphics pipes; every other device is its bus's. */
static const HotplugHandler *sgi_machine_get_hotplug_handler(
    MachineState *machine, DeviceState *dev)
{
    SGIModelGraphicsFamilyKind family;

    return sgi_graphics_pipe_family(dev, &family) ?
           HOTPLUG_HANDLER(machine) : NULL;
}

/*
 * Every pipe, a -device or the default, before it realizes: checked
 * against the catalogue with every unset setting filled from the slot's
 * defaults, then given what the topology's own pipe record in that slot
 * fixes (sgi_machine_fill_pipe_record()).
 */
static void sgi_machine_pre_plug(const HotplugHandler *hotplug_dev,
                                 DeviceState *dev, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(hotplug_dev);
    SGIModelGraphicsFamilyKind family;

    if (!sgi_graphics_pipe_family(dev, &family) ||
        !sgi_graphics_pipes_pre_plug(&s->pipes, dev, errp)) {
        return;
    }
    sgi_machine_fill_pipe_record(s, sgi_machine_topology(MACHINE(s)), dev);
}

static void sgi_machine_plug(const HotplugHandler *hotplug_dev,
                             DeviceState *dev, Error **errp)
{
    sgi_graphics_pipes_plug(&SGI_MACHINE(hotplug_dev)->pipes, dev);
}

void sgi_machine_init(MachineState *machine)
{
    SGIMachineState *s = SGI_MACHINE(machine);

    /*
     * Every device the builder realizes maps its regions with a commit of
     * its own, and each commit re-renders the flat view of every address
     * space built so far, so start-up cost grows with the square of the
     * board: 2.2 CPU seconds and 859 MB to start an Origin 2000.
     * One transaction defers that to its commit. Until then, an access
     * through an address space resolves against its view from before the
     * transaction; nothing the builder runs makes one, and the board's
     * first accesses come after machine init has returned.
     */
    memory_region_transaction_begin();
    sgi_machine_realize(machine, &error_fatal);
    memory_region_transaction_commit();
    sgi_machine_init_graphics(s, &error_fatal);
    s->machine_done.notify = sgi_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
}

static void sgi_machine_reset(MachineState *machine, ResetType type)
{
    SGIMachineState *s = SGI_MACHINE(machine);
    const SGIModelTopology *t = sgi_machine_topology(machine);
    CPUState *cs;
    unsigned int node, slice;

    for (unsigned int i = 0; i < s->controller_count; i++) {
        SGIL1State *l1 = SGI_L1(s->controllers[i]);

        /* Without the L1 registry, a controller may have no built L1. */
        if (l1 && l1->power_command_owner) {
            l1->power_command_reachable = true;
        }
    }
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

        for (i = 0; i < s->num_nodes; i++) {
            sn1_node_reset_cpus(SN1_NODE(s->bedrock_nodes[i]));
        }
        qemu_devices_reset(type);
        sn1_memory_mappings_refresh(s->sn1_memory_mappings, NULL);
        for (i = 0; i < s->num_nodes; i++) {
            sn1_node_release_cpus(SN1_NODE(s->bedrock_nodes[i]));
        }
        return;
    }

    /*
     * The machine's own reset stands for an AC cycle: every module and
     * every controller starts over, so no module start is wanted of the
     * power lines it moves.
     */
    s->ac_cycle = true;
    qemu_devices_reset(type);
    s->ac_cycle = false;
    /* The start-up powers every module up. */
    sgi_machine_sync_power(s);
    /* A start scheduled before the reset has been made by it. */
    for (unsigned int i = 0; i < s->start_count; i++) {
        s->module_start[i].pending = false;
    }
    CPU_FOREACH(cs) {
        cpu_reset(cs);
    }
    /*
     * The processors of a module whose controller has its power off stay
     * stopped, through the Hub's processor enable, while the rest of the
     * machine and the clock run on.
     */
    for (node = 0; node < s->num_nodes; node++) {
        SN0NodeState *node_state = s->nodes[node];
        bool powered = sgi_machine_node_powered(s, node);

        for (slice = 0; slice < node_state->nr_cpus; slice++) {
            sgi_hub_set_cpu_enable(&node_state->hub, slice, powered, true);
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

static char *sgi_machine_get_processor(Object *obj, Error **errp)
{
    return g_strdup(SGI_MACHINE(obj)->processor_name ?
                    SGI_MACHINE(obj)->processor_name : "");
}

static void sgi_machine_set_processor(Object *obj, const char *value,
                                      Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    if (sgi_machine_built("processor", errp)) {
        return;
    }
    g_free(s->processor_name);
    s->processor_name = *value ? g_strdup(value) : NULL;
}

static bool sgi_machine_get_uncatalogued_graphics(Object *obj, Error **errp)
{
    return SGI_MACHINE(obj)->uncatalogued_graphics;
}

static void sgi_machine_set_uncatalogued_graphics(Object *obj, bool value,
                                                  Error **errp)
{
    if (sgi_machine_built("x-uncatalogued-graphics", errp)) {
        return;
    }
    SGI_MACHINE(obj)->uncatalogued_graphics = value;
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

static char *sgi_machine_get_io_prom(Object *obj, Error **errp)
{
    return g_strdup(SGI_MACHINE(obj)->io_prom);
}

static void sgi_machine_set_io_prom(Object *obj, const char *value,
                                    Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    if (sgi_machine_built("io-prom", errp)) {
        return;
    }
    g_free(s->io_prom);
    s->io_prom = g_strdup(value);
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

static bool sgi_machine_get_ip53_experimental(Object *obj, Error **errp)
{
    return SGI_MACHINE(obj)->ip53_experimental;
}

static void sgi_machine_set_ip53_experimental(Object *obj, bool value,
                                              Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    if (sgi_machine_built("ip53-experimental", errp)) {
        return;
    }
    s->ip53_experimental = value;
}

static bool sgi_machine_get_origin300_l1cmd_compat(Object *obj, Error **errp)
{
    return SGI_MACHINE(obj)->origin300_l1cmd_compat;
}

static void sgi_machine_set_origin300_l1cmd_compat(Object *obj, bool value,
                                                    Error **errp)
{
    if (sgi_machine_built("origin300-l1cmd-compat", errp)) {
        return;
    }
    if (value && g_strcmp0(SGI_MACHINE_GET_CLASS(obj)->product,
                            "origin300")) {
        error_setg(errp, "origin300-l1cmd-compat requires origin300");
        return;
    }
    SGI_MACHINE(obj)->origin300_l1cmd_compat = value;
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

static void sgi_machine_get_module_start_delay(Object *obj, Visitor *v,
                                               const char *name,
                                               void *opaque, Error **errp)
{
    uint32_t value = SGI_MACHINE(obj)->module_start_delay_ms;

    visit_type_uint32(v, name, &value, errp);
}

static void sgi_machine_set_module_start_delay(Object *obj, Visitor *v,
                                               const char *name,
                                               void *opaque, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);
    uint32_t value;

    if (sgi_machine_built(name, errp) ||
        !visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    s->module_start_delay_ms = value;
}

static void sgi_machine_get_board_id_word(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint64_t value = SGI_MACHINE(obj)->board_id_override;

    visit_type_uint64(v, name, &value, errp);
}

static void sgi_machine_set_board_id_word(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint64_t value;

    if (visit_type_uint64(v, name, &value, errp)) {
        SGI_MACHINE(obj)->board_id_override = value;
        SGI_MACHINE(obj)->board_id_override_set = true;
    }
}

static void sgi_machine_get_ioc3_subsystem_id(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    uint16_t value = SGI_MACHINE(obj)->ioc3_subsystem_override;

    visit_type_uint16(v, name, &value, errp);
}

static void sgi_machine_set_ioc3_subsystem_id(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    uint16_t value;

    if (visit_type_uint16(v, name, &value, errp)) {
        SGI_MACHINE(obj)->ioc3_subsystem_override = value;
        SGI_MACHINE(obj)->ioc3_subsystem_override_set = true;
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

static void sgi_machine_get_l1_type_code(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint8_t value = SGI_MACHINE(obj)->l1_type_override;

    visit_type_uint8(v, name, &value, errp);
}

static void sgi_machine_set_l1_type_code(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint8_t value;

    if (visit_type_uint8(v, name, &value, errp)) {
        SGI_MACHINE(obj)->l1_type_override = value;
        SGI_MACHINE(obj)->l1_type_override_set = true;
    }
}

static char *sgi_machine_get_l1_revision(Object *obj, Error **errp)
{
    SGIMachineState *s = SGI_MACHINE(obj);

    if (!s->l1_revision_override_set) {
        return NULL;
    }
    return g_strdup_printf("%d.%d.%d", s->l1_revision_override[0],
                           s->l1_revision_override[1],
                           s->l1_revision_override[2]);
}

static void sgi_machine_set_l1_revision(Object *obj, const char *value,
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
    memcpy(s->l1_revision_override, revision, sizeof(revision));
    s->l1_revision_override_set = true;
    return;

invalid:
    error_setg(errp, "l1-revision requires three decimal components "
               "(major.minor.patch), each at most %d", INT32_MAX);
}

static void sgi_machine_get_bedrock_revision(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    uint8_t value = SGI_MACHINE(obj)->bedrock_revision_override;

    visit_type_uint8(v, name, &value, errp);
}

static void sgi_machine_set_bedrock_revision(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    uint8_t value;

    if (visit_type_uint8(v, name, &value, errp)) {
        SGI_MACHINE(obj)->bedrock_revision_override = value;
        SGI_MACHINE(obj)->bedrock_revision_override_set = true;
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
    GString *result;
    unsigned int from, to;

    /* A Bedrock topology has no Hubs to ask, and no s->nodes array. */
    if (!s->nodes) {
        error_setg(errp, "routes are not modelled for this topology");
        return NULL;
    }
    result = g_string_new(NULL);
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

/* ---- Writable storage bound through machine properties ---- */

/*
 * Which catalogue storage items this family binds through properties
 * (hw/mips/sgi/sgi-storage.h): the devices are built from topology= and
 * nodes=, so each name is held until the builder creates its device.
 */
static bool sgi_machine_storage_bindable(const SGIMachineClass *smc,
                                         const char *name)
{
    const SGIModelTopology *t = smc->default_topology ?
        sgi_model_find_topology(smc->default_topology) : NULL;
    unsigned int index;
    const char *end;

    if (!t || !t->nodes) {
        return false;
    }
    if (g_str_has_prefix(name, "node") &&
        !qemu_strtoui(name + strlen("node"), &end, 10, &index) &&
        g_str_equal(end, "-flash")) {
        return t->nodes->value->asic_kind == SGI_MODEL_ASIC_KIND_HUB ||
               t->nodes->value->asic_kind == SGI_MODEL_ASIC_KIND_BEDROCK;
    }
    if (g_str_has_prefix(name, "io") &&
        !qemu_strtoui(name + strlen("io"), &end, 10, &index) &&
        g_str_equal(end, "-flash")) {
        return t->nodes->value->asic_kind == SGI_MODEL_ASIC_KIND_HUB;
    }
    if (g_str_has_prefix(name, "module") &&
        !qemu_strtoui(name + strlen("module"), &end, 10, &index) &&
        g_str_equal(end, "-nvram")) {
        return t->nodes->value->asic_kind == SGI_MODEL_ASIC_KIND_HUB;
    }
    if (g_str_has_prefix(name, "nvram") &&
        !qemu_strtoui(name + strlen("nvram"), &end, 10, &index) &&
        (!*end || g_str_equal(end, "-clock"))) {
        return t->nodes->value->asic_kind == SGI_MODEL_ASIC_KIND_HUB ||
               t->nodes->value->asic_kind == SGI_MODEL_ASIC_KIND_BEDROCK;
    }
    /* An expansion module's configuration storage, named for the module. */
    if (g_str_has_suffix(name, "-flash")) {
        return t->nodes->value->asic_kind == SGI_MODEL_ASIC_KIND_BEDROCK;
    }
    /* An IO9 board's DS1742 and its clock record, named for the board. */
    if (g_str_has_prefix(name, "ds1742-")) {
        return t->nodes->value->asic_kind == SGI_MODEL_ASIC_KIND_BEDROCK;
    }
    return false;
}

/*
 * One class property per bindable item any of @oc's product offerings
 * lists. Each product class calls this once its product is set.
 */
void sgi_machine_class_add_storage(ObjectClass *oc)
{
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelOfferingList *entry;

    g_assert(smc->product);
    for (entry = sgi_machines()->offerings; entry; entry = entry->next) {
        const SGIModelStorageItemList *item;

        if (!g_str_equal(entry->value->product, smc->product)) {
            continue;
        }
        for (item = entry->value->storage; item; item = item->next) {
            const char *name = item->value->name;

            if (object_class_property_find(oc, name) ||
                !sgi_machine_storage_bindable(smc, name)) {
                continue;
            }
            sgi_storage_class_add_property(
                oc, offsetof(SGIMachineState, storage), name);
        }
    }
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

    sn1_memory_mappings_stop(s->sn1_memory_mappings);
    g_free(s->sn1_memory_mappings);
    s->sn1_memory_mappings = NULL;
    size_t i;

    if (s->ip53_fingerprint_registered) {
        vmstate_unregister(NULL, &vmstate_sgi_machine_ip53_fingerprint, s);
    }
    if (s->management_registered) {
        vmstate_unregister(NULL, &vmstate_sgi_machine_management, s);
    }
    if (s->module_starts_registered) {
        vmstate_unregister(NULL, &vmstate_sgi_machine_module_starts, s);
        qemu_del_vm_change_state_handler(s->module_start_vm_state);
    }
    g_free(s->node_module);
    sgi_graphics_pipes_cleanup(&s->pipes);
    g_free(s->pipe_record);
    g_free(s->pipe_claimed);
    g_free(s->pipe_record_attachment);
    g_free(s->pipe_attachment);
    g_free(s->controller_module);
    g_free(s->module_start);
    sgi_l1_query_router_destroy(&s->query_router);
    for (i = 0; i < s->controller_count; i++) {
        if (s->controller_reset_registered[i]) {
            qemu_unregister_resettable(OBJECT(s->controllers[i]));
            if (s->controller_bics[i]) {
                qemu_unregister_resettable(OBJECT(s->controller_bics[i]));
            }
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
    for (i = 0; i < s->pic_host_count; i++) {
        g_free(s->pic_links[i].name);
    }
    g_free(s->pic_hosts);
    g_free(s->enclosure_xbridges);
    g_free(s->pic_links);
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
    g_free(s->sn1_routers);
    g_free(s->crossbars);
    g_free(s->io_boards);
    g_free(s->node_links);
    g_free(s->board_links);
    g_free(s->slot_links);
    g_free(s->slot_claimed);
    g_free(s->topology_name);
    g_free(s->population);
    g_free(s->processor_name);
    g_free(s->baseio_preset);
    g_free(s->io_prom);
    if (s->storage) {
        g_hash_table_destroy(s->storage);
    }
    g_free(s->fuel_mac_eeprom);
    g_free(s->mac);
    for (i = 0; i < ARRAY_SIZE(sgi_machine_identity_records); i++) {
        qapi_free_strList(*sgi_machine_identity_slot(
            obj, (void *)&sgi_machine_identity_records[i]));
    }
}

static void sgi_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIXioSlotsClass *xc = SGI_XIO_SLOTS_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(oc);

    xc->claim = sgi_machine_claim_xio_slot;
    xc->claim_fabric = sgi_machine_claim_xio_fabric;
    mc->get_hotplug_handler = sgi_machine_get_hotplug_handler;
    hc->pre_plug = sgi_machine_pre_plug;
    hc->plug = sgi_machine_plug;

    smc->parent_create_default_memdev = mc->create_default_memdev;
    mc->create_default_memdev = sgi_machine_create_default_memdev;
    mc->init = sgi_machine_init;
    mc->reset = sgi_machine_reset;
    mc->possible_cpu_arch_ids = sgi_machine_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = sgi_machine_cpu_index_to_props;
    mc->get_default_cpu_node_id = sgi_machine_get_default_cpu_node_id;
    mc->default_nic = "sgi-ioc3-eth";
    /*
     * QEMU's default parallel backend is a text console nothing reads, and
     * a screendump by device name aborts when its console walk reaches it.
     * sgi_machine_parallel_label() puts a null backend there instead.
     */
    mc->no_parallel = true;
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
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_ODYSSEY);
#if defined(CONFIG_TEST_DEVICES) && defined(CONFIG_SGI_BOARD_TEST)
    machine_class_allow_dynamic_sysbus_dev(mc,
                                         "sgi-compute-option-intx-fixture");
#endif

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

    object_class_property_add_str(oc, "processor", sgi_machine_get_processor,
                                  sgi_machine_set_processor);
    object_class_property_set_description(oc, "processor",
        "The processor module every node board fits, by catalogue id "
        "(r12000-270mhz-4mb), one of the topology's processor options; "
        "-cpu must name its part. Defaults to the product's shipped "
        "module.");

    object_class_property_add_bool(oc, "x-uncatalogued-graphics",
                                   sgi_machine_get_uncatalogued_graphics,
                                   sgi_machine_set_uncatalogued_graphics);
    object_class_property_set_description(oc, "x-uncatalogued-graphics",
        "Skip the graphics catalogue: fit no default pipe, accept any -vga "
        "and check no graphics -device against the topology. For "
        "register-contract tests that use the machine to carry a board.");

    object_class_property_add_str(oc, "io-prom", sgi_machine_get_io_prom,
                                  sgi_machine_set_io_prom);
    object_class_property_set_description(oc, "io-prom",
        "An original I/O PROM image, raw or in SGI's distribution "
        "container, stored as given in every BASEIO and GIGAchannel flash "
        "part that has no flash drive. A part with a drive keeps the "
        "drive's contents and the image is not read.");

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

    object_class_property_add_bool(oc, "ip53-experimental",
                                   sgi_machine_get_ip53_experimental,
                                   sgi_machine_set_ip53_experimental);
    object_class_property_set_description(oc, "ip53-experimental",
        "Permit a registered IP53-backed software experiment. The Onyx 4 "
        "host serial trial uses IP53 guest compatibility behavior. Off by "
        "default and immutable after the machine is built.");

    object_class_property_add_bool(oc, "origin300-l1cmd-compat",
                                   sgi_machine_get_origin300_l1cmd_compat,
                                   sgi_machine_set_origin300_l1cmd_compat);
    object_class_property_set_description(oc, "origin300-l1cmd-compat",
        "Answer selected Origin 300 L1 brick, version and serial all guest "
        "queries with marked synthetic values. Off by default.");

    object_class_property_add(oc, "elsc-dip-switches", "uint8",
                              sgi_machine_get_elsc_dip_switches,
                              sgi_machine_set_elsc_dip_switches, NULL, NULL);
    object_class_property_set_description(oc, "elsc-dip-switches",
        "Set the eight physical ELSC DIP switches as asserted bits. The "
        "controller reports their active-low wire value to firmware.");

    object_class_property_add(oc, "module-start-delay", "uint32",
                              sgi_machine_get_module_start_delay,
                              sgi_machine_set_module_start_delay, NULL, NULL);
    object_class_property_set_description(oc, "module-start-delay",
        "Start every module powered off and let its controller power it "
        "up this many milliseconds after the machine starts, as the "
        "controller does on hardware (about 6000 on an Origin 2000; an "
        "Origin 200 master whose NVRAM says automatic power-on, and its "
        "slave through the master's forward). Zero, the default, powers an "
        "Origin 2000 module up as its controller starts, through the "
        "controller's power-up with no wait, and starts an Origin 200 "
        "module powered.");

    object_class_property_add(oc, "board-id-word", "uint64",
                              sgi_machine_get_board_id_word,
                              sgi_machine_set_board_id_word, NULL, NULL);
    object_class_property_set_description(oc, "board-id-word",
        "Override the catalogue's 64-bit board-identification word for "
        "every Bedrock node. The product's catalogue mask still applies.");

    object_class_property_add(oc, "ioc3-subsystem-id", "uint16",
                              sgi_machine_get_ioc3_subsystem_id,
                              sgi_machine_set_ioc3_subsystem_id, NULL, NULL);
    object_class_property_set_description(oc, "ioc3-subsystem-id",
        "Override the catalogue's IOC3 PCI subsystem ID for every Bedrock "
        "I/O board.");

    object_class_property_add(oc, "l1-type-code", "uint8",
                              sgi_machine_get_l1_type_code,
                              sgi_machine_set_l1_type_code, NULL, NULL);
    object_class_property_set_description(oc, "l1-type-code",
        "Override the catalogue's nonzero L1 local identity reply byte for "
        "every node L1.");

    object_class_property_add_str(oc, "l1-revision",
                                  sgi_machine_get_l1_revision,
                                  sgi_machine_set_l1_revision);
    object_class_property_set_description(oc, "l1-revision",
        "Override the catalogue's L1 firmware revision, as decimal "
        "major.minor.patch, for every node L1.");

    object_class_property_add(oc, "bedrock-revision", "uint8",
                              sgi_machine_get_bedrock_revision,
                              sgi_machine_set_bedrock_revision, NULL, NULL);
    object_class_property_set_description(oc, "bedrock-revision",
        "Override the catalogue's Bedrock revision, 0 through 15, for "
        "every Bedrock node.");

    object_class_property_add_str(oc, "fuel-mac-eeprom",
                                  sgi_machine_get_fuel_mac_eeprom,
                                  sgi_machine_set_fuel_mac_eeprom);
    object_class_property_set_description(oc, "fuel-mac-eeprom",
        "Path to a 512-byte Fuel L1 MAC EEPROM image; applicability and "
        "requirements are declared by the selected model");

    sgi_board_mac_class_add_property(oc, offsetof(SGIMachineState, mac),
        "Onboard Ethernet address of node 0, carried in its L1 identity "
        "record; each further node takes the next address (default "
        SGI_BOARD_DEFAULT_MAC ", a placeholder)");

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

/* The Origin 200 takes R12000 modules too, through processor=. */
static const char * const sgi_origin200_cpu_types[] = {
    MIPS_CPU_TYPE_NAME("R10000"),
    MIPS_CPU_TYPE_NAME("R12000"),
    NULL,
};

/*
 * With no -cpu, the part of the processor module processor= names, so the
 * option alone selects an R12000 module; an unknown name falls back to the
 * shipped R10000 and realize refuses it with the names it accepts.
 */
static const char *origin200_get_default_cpu_type(const MachineState *ms)
{
    SGIMachineState *s = SGI_MACHINE(ms);
    const SGIModelTopology *t;
    const SGIModelProcessorOptionList *entry;

    if (!s->processor_name) {
        return sgi_origin200_cpu_types[0];
    }
    t = sgi_model_product_topology(sgi_machines(), "origin200",
                                   s->topology_name ? s->topology_name :
                                   SGI_MACHINE_GET_CLASS(s)->default_topology);
    for (entry = t ? t->processor_options : NULL; entry;
         entry = entry->next) {
        if (g_str_equal(entry->value->assembly->id, s->processor_name) &&
            g_str_equal(entry->value->assembly->kind, "R12000")) {
            return sgi_origin200_cpu_types[1];
        }
    }
    return sgi_origin200_cpu_types[0];
}

static void origin200_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelTopology *t = sgi_model_machine_topology("origin200");

    g_assert(t);
    smc->product = "origin200";
    smc->default_topology = t->name;
    smc->node_memory = &sn0_hub_memory;
    mc->desc = t->description;
    mc->valid_cpu_types = sgi_origin200_cpu_types;
    mc->get_default_cpu_type = origin200_get_default_cpu_type;
    mc->default_cpus = sgi_machine_topology_default_cpus(t);
    mc->max_cpus = sgi_machine_product_max_cpus("origin200");
    sgi_machine_class_set_default_ram(
        mc, sgi_machine_topology_default_ram(t));
    mc->default_ram_id = "sn0.node0.ram";
    sgi_machine_class_add_storage(oc);
}

static void origin2000_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIMachineClass *smc = SGI_MACHINE_CLASS(oc);
    const SGIModelTopology *t = sgi_model_machine_topology("origin2000");

    g_assert(t);
    smc->product = "origin2000";
    smc->default_topology = t->name;
    smc->node_memory = &sn0_ip27_memory;
    mc->desc = t->description;
    mc->valid_cpu_types = sgi_sn0_r10000_cpu_types;
    mc->default_cpus = sgi_machine_topology_default_cpus(t);
    mc->max_cpus = sgi_machine_product_max_cpus("origin2000");
    sgi_machine_class_set_default_ram(
        mc, sgi_machine_topology_default_ram(t));
    mc->default_ram_id = "sn0.ram";

    object_class_property_add_str(oc, "baseio-preset",
                                  sgi_machine_get_baseio_preset,
                                  sgi_machine_set_baseio_preset);
    object_class_property_set_description(oc, "baseio-preset",
        "Select the BaseIO card variant this product's I/O board builds: "
        "'io6' (the plain card, the default) or 'io6g' (adds the MediaIO "
        "daughtercard's second IOC3 and both chips' keyboard/mouse pair).");
    sgi_machine_class_add_storage(oc);
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
    const SGIModelTopology *t = sgi_model_machine_topology("onyx2");

    g_assert(t);
    smc->product = "onyx2";
    smc->default_topology = t->name;
    smc->node_memory = &sn0_ip27_memory;
    mc->desc = t->description;
    mc->valid_cpu_types = sgi_sn0_r10000_cpu_types;
    mc->default_cpus = sgi_machine_topology_default_cpus(t);
    mc->max_cpus = sgi_machine_product_max_cpus("onyx2");
    sgi_machine_class_set_default_ram(
        mc, sgi_machine_topology_default_ram(t));
    mc->default_ram_id = "sn0.ram";
    sgi_machine_class_add_storage(oc);
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
            { TYPE_HOTPLUG_HANDLER },
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
