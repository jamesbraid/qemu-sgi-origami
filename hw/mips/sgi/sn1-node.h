/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI Origin 300 (IP35/Bedrock, "SN1") node device: the CPUs, Bedrock, L1 and
 * its UART, boot flash and PROM views one Origin 300 chassis carries. The
 * family builder (sn-machine.c) creates it from the format-2 descriptor's
 * Bedrock node record and its populated I/O board, then realizes it; sn1.c
 * holds only the product class. Each node's address-space views are strided
 * by its nasid property (sn1-node.c), so a second node's windows land clear
 * of the first's; the machine still refuses more than one Bedrock node
 * (sn-machine.c) because nothing sources or models the cross-node NUMAlink
 * fabric a real multi-chassis Origin 300 pair would need.
 */

#ifndef HW_MIPS_SGI_SN1_NODE_H
#define HW_MIPS_SGI_SN1_NODE_H

#include "hw/core/clock.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi/sn-catalogue.h"
#include "hw/mips/sgi/xio-link.h"
#include "cpu.h"
#include "sn-promdata.h"
#include "sn1-fabric.h"

/*
 * Bedrock is fixed at four CPUs on two processor interfaces, two processors
 * per interface.
 */
#define SGI_SN1_NODE_MAX_CPUS 4

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
typedef struct SN1PromView {
    MemoryRegion rom;
    MemoryRegion *backing;
    const char *name;
} SN1PromView;

typedef struct SN1NodeState {
    SysBusDevice parent_obj;

    /* Properties, set by the machine before realize. */
    MemoryRegion *ram;
    uint32_t nr_cpus;
    uint32_t nasid;
    uint32_t l1_bay;
    uint64_t board_id_word;
    uint32_t l1_type_code;
    int32_t l1_revision[3];
    char *l1_mac_eeprom;
    uint8_t bedrock_revision;
    uint8_t local_region;
    uint8_t initiator_region;
    uint8_t installed_bank_mask;
    uint8_t io_board_widget;
    char *io_board_part;
    bool last_node;
    bool promdata_filter;
    bool promdata_fast;

    /*
     * The topology's Bedrock node record, set by the machine before
     * realize -- the compiled catalogue this node builds from
     * (qapi/sgi-sn.json). The machine passes the I/O board's fitted identity
     * through properties before this node realizes the XBridge.
     */
    const SGIModelNode *model_node;
    /*
     * Every widget the crossbar decodes for this node (widget zero, the
     * host's own local view, plus one per XIO attachment), set by the
     * machine before realize.
     */
    unsigned int *xbridge_widgets;
    size_t xbridge_widget_count;

    /* The Bedrock's I/O interface linked to the XBridge's host port. */
    SGIXioLink xbridge_link;

    Clock *cpuclk;
    MIPSCPU *cpu[SGI_SN1_NODE_MAX_CPUS];
    /* Where the processors send cache instructions and prefetches. */
    MemoryRegion system_requests;
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
    SN1FabricProcessorView ni_processor[SGI_SN1_NODE_MAX_CPUS][SN1_NODE_SPACES];
    MemoryRegion ni_target[2];
    MemoryRegion ni_target_part[2][2];
    AddressSpace ni_ram_as;
    AddressSpace ni_hub_as;
    AddressSpace ni_pio_as;
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

/*
 * The two halves of sn1_node_full_reset() for a machine of several nodes,
 * which resets every node's processors, then every device once, then releases
 * every node's processors: the same order, with the device reset shared
 * rather than repeated per node.
 */
void sn1_node_reset_cpus(SN1NodeState *node);
void sn1_node_release_cpus(SN1NodeState *node);

/*
 * Attaches the node's boot flash: a pflash part from -drive if=pflash at unit
 * node_index if one was given, else an immutable ROM holding contents
 * (exactly length bytes, checked against the node's descriptor flash size),
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
