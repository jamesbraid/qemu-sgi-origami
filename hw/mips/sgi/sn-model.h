/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Format 2: one generated descriptor for every Scalable Node (SN0, SN1)
 * configuration. QEMU owns this header; Origami's compiler emits
 * sn-model.c against it (manifest sn-model.json, format key "sgi-sn",
 * format: 2). See docs/plans/sn-machine-model.md, "Descriptor, format 2",
 * for the record list this follows.
 *
 * What stays out on purpose: node strides, window splits, unit offsets
 * and the carrier bases a node's address map needs are hardware
 * constants of the node ASIC, not of a configuration, so they live in
 * the node device (sn0.c for HUB, sn1-node.c for BEDROCK), one set of
 * constants per asic_kind. KLCONFIG (IRIX's board/component discovery
 * table, synthesized only for Origin 200) is likewise not carried here:
 * it is mechanically derivable from the records below (a node's asic
 * identity and populated CPUs, an I/O board's identity and PCI
 * population) and synthesized_firmware builds it from them directly.
 *
 * Memory bank geometry (bank stride, interleave slot stride and count,
 * architectural bank count) is a per-asic_kind hardware constant too
 * (identical across every SN0 configuration measured for this format),
 * so it stays a node-builder constant; SGIModelNode.bank_mib[] names
 * only which banks this node ships populated, and supported_memory_mib[]
 * the other total sizes the same fitting rule accepts.
 */

#ifndef HW_MIPS_SGI_SN_MODEL_H
#define HW_MIPS_SGI_SN_MODEL_H

typedef enum SGIModelAsicKind {
    SGI_MODEL_ASIC_HUB,
    SGI_MODEL_ASIC_BEDROCK,
} SGIModelAsicKind;

/*
 * A node ASIC's identity: the fields real device construction and the
 * firmware's synthesized identity records both read. i2c_* and the two
 * population booleans describe the Hub's own embedded system controller
 * and are unset (false/zero) for a Bedrock node, whose controller role
 * belongs to the module's or brick's L1 instead (see SGIModelController).
 */
typedef struct SGIModelAsicIdentity {
    uint8_t ni_revision;
    uint8_t widget_revision;
    uint64_t serial;
    bool i2c_controller;
    bool system_controller;
    uint64_t i2c_controller_offset;
    uint8_t i2c_elsc_addr;
    uint8_t i2c_nvram_first_addr;
    uint8_t i2c_nvram_pages;
} SGIModelAsicIdentity;

/*
 * One populated CPU catalogue entry. identity registers (prid, fpu_prid)
 * and cache facts replace KLCONFIG's per-processor component record;
 * mode_bits is the encoded CP0_Config0 field the flash configuration
 * block and the board latch both compare against (Origami's processors
 * catalogue encodes it once from part-keyed fields, per the plan's
 * "Vocabulary" section).
 */
typedef struct SGIModelProcessor {
    const char *id;
    const char *kind;
    uint32_t prid;
    uint32_t fpu_prid;
    uint64_t clock_hz;
    uint64_t cache_clock_hz;
    uint16_t cache_mib;
    uint32_t mode_bits;
} SGIModelProcessor;

/* A populated or empty processor-interface slot; NULL assembly is empty. */
typedef struct SGIModelCpuSlot {
    unsigned int interface;
    unsigned int slot;
    const SGIModelProcessor *assembly;
} SGIModelCpuSlot;

typedef enum SGIModelFlashKind {
    SGI_MODEL_FLASH_AM29F080,
} SGIModelFlashKind;

/*
 * One firmware execution or maintenance view of the node's boot flash.
 * The view's address is a per-asic_kind constant in the node device
 * (the reset vector, LBOOT, RBOOT, or an SN1 PROM alias); this only
 * names which views this node's flash chip is wired for and how large
 * each is, since not every product's node offers every view its ASIC
 * supports (SN0's flash controller decodes LBOOT only where the flash
 * hub view is wired, for example).
 */
typedef struct SGIModelFlashView {
    const char *name;
    uint64_t size;
} SGIModelFlashView;

typedef struct SGIModelFlash {
    SGIModelFlashKind kind;
    uint64_t size;
    const SGIModelFlashView *views;
    size_t view_count;
} SGIModelFlash;

#define SGI_MODEL_MAX_MEMORY_BANKS 8

/*
 * One node board: a node ASIC, its populated processor slots, its
 * memory banks and its boot flash. module_index and slot place it in
 * its module's nodes[] and in the module's physical slot numbering;
 * management_adapter_offset is the Hub register offset at which this
 * node's I2C adapter answers, whether it carries a private controller
 * (Origin 200) or is one of a shared module controller's per-node
 * adapters (Origin 2000's MSC, one per Hub); slave_role marks a node
 * that answers module-management commands only through its module's
 * master, never on its own external AUX line.
 */
typedef struct SGIModelNode {
    const char *id;
    unsigned int module_index;
    unsigned int slot;
    SGIModelAsicKind asic_kind;
    SGIModelAsicIdentity identity;
    const SGIModelCpuSlot *cpu_slots;
    size_t cpu_slot_count;
    uint64_t bank_mib[SGI_MODEL_MAX_MEMORY_BANKS];
    /*
     * Other total sizes (MiB) the node builder's fitting rule accepts
     * for this node besides the shipped bank_mib[] above; empty means
     * the shipped population only (every Origin 2000 node today). Named
     * for the plan's topology-level accepted_memory_populations; kept
     * per node here since bank_mib[] is already per node and every
     * measured configuration populates its nodes uniformly.
     */
    const uint64_t *accepted_memory_mib;
    size_t accepted_memory_mib_count;
    SGIModelFlash flash;
    uint64_t management_adapter_offset;
    bool slave_role;
} SGIModelNode;

/* A fabric endpoint: a node's Hub network port, or a router's numbered port. */
typedef enum SGIModelFabricVertexKind {
    SGI_MODEL_FABRIC_NODE,
    SGI_MODEL_FABRIC_ROUTER,
} SGIModelFabricVertexKind;

typedef struct SGIModelFabricVertex {
    SGIModelFabricVertexKind kind;
    unsigned int index;
    unsigned int port;
} SGIModelFabricVertex;

typedef enum SGIModelFabricLinkKind {
    SGI_MODEL_LINK_MIDPLANE,
    SGI_MODEL_LINK_CABLE,
    SGI_MODEL_LINK_XPRESS_CABLE,
    SGI_MODEL_LINK_CARRIER,
    SGI_MODEL_LINK_NULL_ROUTER,
} SGIModelFabricLinkKind;

typedef struct SGIModelFabricLink {
    const char *name;
    SGIModelFabricVertex ends[2];
    SGIModelFabricLinkKind kind;
} SGIModelFabricLink;

/* A router board: a routing chip with port_count numbered ports. */
typedef struct SGIModelRouter {
    const char *id;
    unsigned int module_index;
    unsigned int slot;
    unsigned int port_count;
    uint16_t part;
    uint8_t revision;
    uint8_t chip_inputs;
    uint64_t serial;
} SGIModelRouter;

typedef enum SGIModelCrossbarKind {
    SGI_MODEL_CROSSBAR_XBOW,
    SGI_MODEL_CROSSBAR_XBRIDGE,
} SGIModelCrossbarKind;

/* A Crossbow chip, or an XBridge's internal crossbar. */
typedef struct SGIModelCrossbar {
    const char *id;
    unsigned int module_index;
    SGIModelCrossbarKind kind;
    uint16_t widget_part;
    uint16_t widget_manufacturer;
    uint8_t widget_revision;
    uint16_t initially_operational_links;
} SGIModelCrossbar;

/*
 * Which node hosts a crosstalk widget: through a crossbar's numbered
 * link (crossbar_index, widget), or, with no crossbar between the node
 * and the widget, direct attachment described by decoded_widget_mask
 * (which widget numbers the node's Bridge answers on) and
 * discovery_widget (the widget number firmware's initial probe uses).
 * crossbar_index -1 means direct attachment.
 */
typedef struct SGIModelXtalkAttachment {
    int crossbar_index;
    unsigned int widget;
    unsigned int node_index;
    uint16_t decoded_widget_mask;
    unsigned int discovery_widget;
} SGIModelXtalkAttachment;

/* Where a board may plug: a crossbar link with no board attached yet. */
typedef struct SGIModelXtalkSlot {
    unsigned int crossbar_index;
    unsigned int widget;
    unsigned int controlling_node;
    const char *name;
} SGIModelXtalkSlot;

typedef enum SGIModelIoBoardKind {
    SGI_MODEL_IO_IP29_IO,
    SGI_MODEL_IO_BASEIO,
    SGI_MODEL_IO_IO8,
    SGI_MODEL_IO_IO9,
} SGIModelIoBoardKind;

/*
 * One PCI function on an I/O board's Bridge. Board internals beyond
 * identity and population are device properties with datasheet
 * defaults, per the plan's "One contract for every product" table; what
 * stays here is what the builder must place (a device kind, its BAR/IRQ
 * wiring) or what firmware's synthesized identity records need.
 */
typedef enum SGIModelPciKind {
    SGI_MODEL_PCI_ISP1040,
    SGI_MODEL_PCI_ISP12160,
    SGI_MODEL_PCI_IOC3,
    SGI_MODEL_PCI_LUCENT_OHCI,
} SGIModelPciKind;

typedef struct SGIModelIoc3Population {
    bool ethernet;
    bool serial_a;
    bool serial_b;
    bool keyboard_mouse;
    bool parallel;
    bool timekeeper;
    bool onewire_identity;
} SGIModelIoc3Population;

typedef struct SGIModelPciFunction {
    SGIModelPciKind kind;
    uint8_t device;
    uint8_t function;
    uint8_t revision;
    int8_t irq_input;
    uint16_t subsystem_id;
    uint64_t serial;
    SGIModelIoc3Population ioc3_population;
} SGIModelPciFunction;

/*
 * An I/O board: its Bridge's identity and PCI population. slot_index
 * names a physical XIO/PCI slot; onboard is true for a board the
 * product wires in permanently (Origin 200's BaseIO), in which case
 * slot_index is meaningless and removable must be false.
 */
typedef struct SGIModelIoBoard {
    const char *id;
    unsigned int module_index;
    SGIModelIoBoardKind board_kind;
    bool onboard;
    unsigned int slot_index;
    bool removable;
    uint8_t revision;
    uint64_t serial;
    const char *part;
    const SGIModelPciFunction *pci;
    size_t pci_count;
    SGIModelIoc3Population ioc3_population;
} SGIModelIoBoard;

typedef enum SGIModelControllerKind {
    SGI_MODEL_CONTROLLER_MSC,
    SGI_MODEL_CONTROLLER_L1,
} SGIModelControllerKind;

/*
 * A module's or brick's shared system controller: an Origin 2000 MSC
 * over the module management wire, or an Origin 300/350 L1 over its
 * UART. A node whose own Hub carries the controller instead (Origin
 * 200's embedded ELSC) has no entry here; see SGIModelAsicIdentity.
 */
typedef struct SGIModelController {
    const char *id;
    unsigned int module_index;
    SGIModelControllerKind kind;
    uint8_t msc_address;
    uint8_t nvram_address;
    unsigned int nvram_address_count;
    unsigned int nvram_bytes;
} SGIModelController;

#define SGI_MODEL_MODULE_SERIAL_LEN 10

/*
 * One module or brick: the nodes it carries and, when it has one, the
 * shared controller index. controller_index -1 means the module's nodes
 * each carry their own controller instead (Origin 200).
 */
typedef struct SGIModelModule {
    const char *id;
    char serial[SGI_MODEL_MODULE_SERIAL_LEN + 1];
    uint8_t initial_module_number;
    int controller_index;
    const unsigned int *nodes;
    size_t node_count;
} SGIModelModule;

/* One accepted -smp/-numa population: CPUs populated, per node, in order. */
typedef struct SGIModelCpuPopulation {
    const unsigned int *cpus_per_node;
    size_t node_count;
} SGIModelCpuPopulation;

typedef enum SGIModelGeneration {
    SGI_MODEL_GENERATION_SN0,
    SGI_MODEL_GENERATION_SN1,
} SGIModelGeneration;

/*
 * One authored configuration: a product's wiring, its shipped
 * population and its accepted -smp/-m populations. console names the
 * component the firmware treats as its console (a node index into
 * nodes[] whose IOC3 port A is line 0 on SN0; the module's or brick's
 * controller on SN1) -- the rest of the serial-line order is the one
 * rule in sn-machine.c, never data here. junk_uart is the node index
 * that offers the manufacturing junk-bus UART when the junk-uart
 * property is set, or -1 where the product has none.
 */
typedef struct SGIModelTopology {
    const char *name;
    SGIModelGeneration generation;
    const char *description;

    const SGIModelNode *nodes;
    size_t node_count;
    const SGIModelModule *modules;
    size_t module_count;
    const SGIModelRouter *routers;
    size_t router_count;
    const SGIModelFabricLink *links;
    size_t link_count;
    const SGIModelCrossbar *crossbars;
    size_t crossbar_count;
    const SGIModelXtalkAttachment *xtalk_attachments;
    size_t xtalk_attachment_count;
    const SGIModelXtalkSlot *xtalk_slots;
    size_t xtalk_slot_count;
    const SGIModelIoBoard *io_boards;
    size_t io_board_count;
    const SGIModelController *controllers;
    size_t controller_count;

    int console;
    int junk_uart;

    const SGIModelCpuPopulation *supported_cpu_populations;
    size_t supported_cpu_population_count;
} SGIModelTopology;

extern const SGIModelTopology sgi_model_entries[];
extern const size_t sgi_model_entry_count;

/* The topology name each product boots with when topology= is omitted. */
const char *sgi_model_default_topology(const char *product);

const SGIModelTopology *sgi_model_find_topology(const char *name);

#endif /* HW_MIPS_SGI_SN_MODEL_H */
