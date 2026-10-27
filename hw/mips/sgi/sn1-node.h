/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI Bedrock node device. Its Origin 300/IP45 path builds the node's XBridge;
 * a machine-validated IP53 assembly reuses the node without that bridge. The
 * family builder (sn-machine.c) creates it from a compiled node record, then
 * realizes it. Each node's address-space views are strided by its nasid
 * property (sn1-node.c), so a second node's windows land clear of the first's.
 */

#ifndef HW_MIPS_SGI_SN1_NODE_H
#define HW_MIPS_SGI_SN1_NODE_H

#include "hw/core/clock.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi/sgi-machines.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/misc/sgi-bedrock-io.h"
#include "cpu.h"
#include "sn-promdata.h"
#include "sn1-fabric.h"
#include "pci-intx-wiring.h"

/*
 * Bedrock is fixed at four CPUs on two processor interfaces, two processors
 * per interface.
 */
#define SGI_SN1_NODE_MAX_CPUS 4
#define SN1_PROM_RESET UINT64_C(0x1fc00000)

/*
 * The four processor-selected spaces a node's window appears in (memory, Hub
 * special, I/O, memory special), each a 1 TiB carrier of 128 node
 * windows; see sn1-node.c's claim layer.
 */
#define SN1_NODE_SPACES SN1_FABRIC_SPACES
/* A node's window and its one cabled peer's. */
#define SN1_NODE_CLAIMS SN1_FABRIC_CLAIMS
/* The claim views: one per processor, then the node's fabric view. */
#define SN1_NODE_VIEWS (SGI_SN1_NODE_MAX_CPUS + 1)
#define SN1_NODE_FABRIC_VIEW SGI_SN1_NODE_MAX_CPUS
/* The reader-relative forms a processor view overlays; see sn1-node.c. */
#define SN1_NODE_LOCAL_FORMS 4

/*
 * A cacheable ROM device mirroring the boot flash's current storage. Stores
 * still reach its write callback and are refused; its RAM block migrates.
 */
typedef struct SN1PicWidget {
    unsigned int widget;
    MemoryRegion *face;
} SN1PicWidget;

typedef struct SN1PromView {
    MemoryRegion rom;
    MemoryRegion *backing;
    const char *name;
} SN1PromView;

typedef struct SN1NodeState {
    SysBusDevice parent_obj;

    uint8_t option_intx_selected;
    uint8_t option_intx_map[SGI_PCI_INTX_MAP_SIZE];
    uint32_t option_intx_raw;
    uint8_t option_intx_levels;
    qemu_irq *option_intx_inputs;
    qemu_irq option_intx_outputs[SGI_PCI_INTX_INPUTS];

    /* Properties, set by the machine before realize. */
    MemoryRegion *ram;
    uint32_t nr_cpus;
    uint32_t nasid;
    uint32_t l1_bay;
    uint64_t board_id_word;
    /* The catalogue's IP35 configuration-block machine type and mode. */
    uint32_t firmware_machine_type;
    uint32_t firmware_mode_word;
    uint32_t l1_type_code;
    bool l1_irouter_compat;
    int32_t l1_revision[3];
    char *l1_mac_eeprom;
    uint8_t bedrock_revision;
    uint8_t local_region;
    uint8_t initiator_region;
    uint8_t installed_bank_mask;
    uint8_t io_board_widget;
    char *io_board_part;
    /* The IO-8's one-wire serial, from the machine's catalogue entry. */
    uint64_t io_board_serial;
    /* The XBridge port a directly attached graphics board answers on. */
    uint8_t xbridge_xio_port;
    /*
     * The block node or drive the machine's node<N>-flash property binds
     * to this node's 16 MiB boot flash, in place of a pflash drive.
     */
    char *flash_node;
    bool last_node;
    bool ni_route;
    bool promdata_filter;
    bool promdata_fast;

    /*
     * The topology's Bedrock node record, set by the machine before
     * realize -- the compiled catalogue this node builds from
     * (qapi/sgi-machines.json). The machine passes the I/O board's
     * fitted identity through properties before this node realizes.
     */
    const SGIModelNode *model_node;
    /* Set by the machine after validating the selected IP53 assembly. */
    bool ip53_assembly_validated;
    /*
     * Every widget the crossbar decodes for this node (widget zero, the
     * host's own local view, plus one per XIO attachment), set by the
     * machine before realize.
     */
    unsigned int *xbridge_widgets;
    size_t xbridge_widget_count;
    SN1PicWidget *pic_widgets;
    size_t pic_widget_count;

    /*
     * An IP35 node's XBridge in another brick, which the machine supplies
     * (sn1_node_supply_xbridge()); NULL on every other board.
     */
    DeviceState *supplied_xbridge;
    /* The Bedrock's I/O interface linked to the XBridge host port. */
    SGIXioLink xbridge_link;

    Clock *cpuclk;
    MIPSCPU *cpu[SGI_SN1_NODE_MAX_CPUS];
    MemoryRegion cpu_sysmem[SGI_SN1_NODE_MAX_CPUS];
    MemoryRegion sysmem_alias[SGI_SN1_NODE_MAX_CPUS];
    /*
     * The uncached view: the cached one, with the processor's own I/O-space
     * path over the I/O space. The PROM data filter builds it, less its
     * private window, when promdata_fast is set (sn-promdata.c); otherwise
     * the node does. cpu_uncached_ready says it was built.
     */
    MemoryRegion cpu_uncached[SGI_SN1_NODE_MAX_CPUS];
    MemoryRegion cpu_uncached_alias[SGI_SN1_NODE_MAX_CPUS];
    bool cpu_uncached_ready[SGI_SN1_NODE_MAX_CPUS];
    SNPromDataState promdata[SGI_SN1_NODE_MAX_CPUS];
    /* Samples the mapped processors' stack pointers for the exit. */
    SNPromDataPoll promdata_poll;

    DeviceState *l1;
    /*
     * The L1 the node's module owns, which the machine supplies
     * (sn1_node_supply_l1()); NULL when the node builds its own.
     */
    DeviceState *supplied_l1;
    DeviceState *bedrock;
    DeviceState *xbridge;
    DeviceState *boot_flash;
    MemoryRegion bedrock_node;
    /*
     * The Hub-special register windows' own blocks, shared by the local and
     * remote forms: the unidentified block below the L1 UART and the
     * per-processor LEDs above it (sn1_node_create_bedrock()).
     */
    MemoryRegion hub_unidentified;
    MemoryRegion hub_unidentified_remote;
    MemoryRegion hub_leds;
    MemoryRegion hub_leds_remote;
    uint8_t led[SGI_SN1_NODE_MAX_CPUS];
    /*
     * OpenBSD expects a local PI access to select the requester's interface
     * (sn1_node_create_bedrock()); indexed by logical CPU, one entry per
     * processor this node realizes whose physical slot sits on processor
     * interface 1; at most two of the four (both are used only in the
     * four-processor population; the two-processor population fits one).
     */
    MemoryRegion cpu_local_pi_xio[SGI_SN1_NODE_MAX_CPUS];

    /* The node's address-space views; see sn1-node.c. */
    MemoryRegion hub_memory;
    MemoryRegion hub_local_registers;
    MemoryRegion board_id;
    MemoryRegion board_id_remote;
    MemoryRegion prom;
    MemoryRegion *prom_views;
    MemoryRegion prom_flash;
    MemoryRegion prom_flash_remote;
    /* Used in place of the aliases above once a flash part is attached. */
    SN1PromView prom_view_reset;
    IOMMUMemoryRegion reset_router;
    /* The router's place in the system space while notifiers register. */
    MemoryRegion reset_probe;
    MemoryRegion reset_memory;
    MemoryRegion reset_prom_alias;
    MemoryRegion reset_ram_alias;
    AddressSpace reset_prom_as;
    AddressSpace reset_memory_as;
    MemoryListener reset_memory_listener;
    bool reset_router_ready;
    bool reset_memory_changed;
    SN1PromView prom_view_local_high;
    char *prom_view_names[2];

    /*
     * The claim layer (sn1-node.c): which node each processor's and the
     * Bedrock's DMA accesses reach, keyed by the node numbers the local
     * blocks carry. Views 0..3 are the processors', view 4 the fabric view
     * the Bedrock's DMA and BTE use.
     */
    MemoryRegion fabric;
    MemoryRegion fabric_sysmem;
    MemoryRegion claim_floor[SN1_NODE_SPACES];
    MemoryRegion claim_floor_alias[SN1_NODE_VIEWS][SN1_NODE_SPACES];
    MemoryRegion claim[SN1_NODE_VIEWS][SN1_NODE_SPACES][SN1_NODE_CLAIMS];
    MemoryRegion local_form[SGI_SN1_NODE_MAX_CPUS][SN1_NODE_LOCAL_FORMS];
    Notifier node_notifier;
    Notifier cable_notifier;
    Notifier cached_alias_notifier[2];
    bool cable_notifier_added;
    SN1FabricEndpoint endpoint;
    SN1MemoryMappings *memory_mappings;
    SN1FabricProcessorView ni_processor[SGI_SN1_NODE_MAX_CPUS][SN1_NODE_SPACES];
    SN1FabricProcessorView ni_fabric_memory;
    MemoryRegion ni_memory_view[SGI_SN1_NODE_MAX_CPUS][2];
    MemoryRegion ni_target[9];
    MemoryRegion ni_target_part[9][2];
    AddressSpace ni_ram_as;
    AddressSpace ni_hub_as;
    AddressSpace ni_uart_as;
    AddressSpace ni_board_id_as;
    AddressSpace ni_boot_as;
    bool ni_boot_initialized;
    AddressSpace ni_pio_as;
    AddressSpace ni_md_as[4];
} SN1NodeState;

#define TYPE_SGI_SN1_NODE "sgi-sn1-node"
DECLARE_INSTANCE_CHECKER(SN1NodeState, SN1_NODE, TYPE_SGI_SN1_NODE)

/*
 * The Bedrock node's machine-reset sequence: reset every CPU and apply the
 * reset-latched processor mode bits, reset every other device, refresh the
 * PROM views from whatever the flash part holds now (once one is attached)
 * and release the CPUs; they start powered off, so the generic device-reset
 * path alone leaves them halted. Self-contained (not sharing
 * sgi_machine_reset's Hub-ordered prefix) because "start-powered-off" needs
 * cpu_reset to run before qemu_devices_reset, the reverse of the Hub's order,
 * which releases through sgi_hub_set_cpu_enable instead and has no such
 * constraint.
 */
void sn1_node_full_reset(SN1NodeState *node, ResetType type);

/* Authorize this node's IP53 construction after machine-level preflight. */
bool sn1_node_set_ip53_assembly_validated(SN1NodeState *node, Error **errp);
bool sn1_node_set_ip53_pic_widget(SN1NodeState *node, unsigned int widget,
                                  MemoryRegion *face, Error **errp);

/*
 * The two halves of sn1_node_full_reset() for a machine of several nodes,
 * which resets every node's processors, then every device once, then releases
 * every node's processors: the same order, with the device reset shared
 * rather than repeated per node.
 */
void sn1_node_reset_cpus(SN1NodeState *node);
void sn1_node_release_cpus(SN1NodeState *node);

/*
 * Registers every processor's TCG notifier on the reset router of each of
 * @nodes. The machine calls this once every node and processor exists and
 * before any vCPU runs.
 */
void sn1_register_reset_notifiers(SN1NodeState *const *nodes,
                                  unsigned int count);

/*
 * Attaches the node's boot flash: a part over the node's node<N>-flash
 * binding or -drive if=pflash at unit node_index if one was given, else an
 * immutable ROM holding contents (exactly length bytes, checked against the
 * node's descriptor flash size). contents may be NULL, with no -bios, only
 * when the node has such a part,
 * and maps every PROM view either backs. The machine calls this once every
 * node this run builds has realized (sgi_machine_load_flash() in
 * sn-machine.c), after resolving -bios and the per-node pflash override the
 * same way the Hub path does; realize itself no longer loads PROM content,
 * only sn1_node_create_boot_flash's device-level mechanics run here, the
 * node's own concern.
 */
bool sn1_node_attach_prom(SN1NodeState *node, unsigned int node_index,
                          const uint8_t *contents, uint64_t length,
                          Error **errp);

/*
 * The PCI bus behind one of the node's own XBridge heads (0xe or 0xf),
 * for the machine to place a populated head's fixed devices on
 * (sgi_machine_instantiate_io8() in sn-machine.c) -- the same attachment
 * path Origin 200's direct board and Origin 2000's Crossbow board both
 * read their own XIO widget from, since node->xbridge is a fitted
 * part of this node's own board rather than something
 * a board-owning attachment builds. The node must already be realized.
 */
PCIBus *sn1_node_xbridge_pci_bus(SN1NodeState *node, unsigned int widget);
bool sn1_node_set_option_intx_map(SN1NodeState *node,
                                 const uint8_t map[SGI_PCI_INTX_MAP_SIZE],
                                 Error **errp);

/*
 * Hand an IP35 node the XBridge its XIO cable reaches, before the node
 * realizes; the node joins and realizes it. Adds the node's "xbridge"
 * link.
 */
bool sn1_node_supply_xbridge(SN1NodeState *node, DeviceState *xbridge,
                             Error **errp);

/*
 * Hand a node the unrealized L1 its module owns, before the node
 * realizes. The node configures and realizes it in place of building
 * its own, and adds its "l1" link.
 */
bool sn1_node_supply_l1(SN1NodeState *node, DeviceState *l1, Error **errp);

/* The node-owned Bedrock I/O endpoint, available after node realization. */
SGIBedrockIOState *sn1_node_bedrock_io(SN1NodeState *node);

/*
 * Re-derive this node's claim layer and its cabled peer's: which node each
 * number reaches. The machine calls it once the cable is wired; the local
 * block's number changes and the cable's state changes call it themselves.
 */
void sn1_node_update_claims(SN1NodeState *node);

/*
 * The node whose local block currently carries this number and that this node
 * can reach (itself, or its peer over an up cable), or NULL.
 */
SN1NodeState *sn1_node_claimant(SN1NodeState *node, unsigned int number);

/* The node's placement in the global map: its windows sit at this << 33. */
uint64_t sn1_node_canonical_address(const SN1NodeState *node,
                                    uint64_t address);


#endif /* HW_MIPS_SGI_SN1_NODE_H */
