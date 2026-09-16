/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) physical address map and family
 * machine, shared by the family's translation units. One flat 40-bit
 * physical address space: the node number is the top eight bits, so any
 * node's resources are reached by changing those bits alone; the two
 * bits above pick which of four spaces an uncached reference means.
 */

#ifndef HW_MIPS_SN0_H
#define HW_MIPS_SN0_H

#include "cpu.h"
#include "qapi/error.h"
#include "hw/block/flash.h"
#include "hw/core/boards.h"
#include "hw/misc/sgi-hub.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci/pci.h"
#include "sn0-model.h"
#include "sn-promdata.h"

/* Fixed storage capacities; each product supplies its active geometry. */
#define SN0_MAX_MEMORY_BANKS 8
#define SN0_MAX_MEMORY_SLOTS 4

typedef struct SN0FlashImplementation {
    uint32_t sector_size;
    uint8_t width;
    uint8_t mappings;
    bool big_endian;
    uint16_t manufacturer_id;
    uint16_t device_id;
    uint16_t unlock_address[2];
} SN0FlashImplementation;

/*
 * Crosstalk space: a widget number and an offset -- one container
 * indexed by widget, so a small window and a big window are two views
 * of one address. A translation entry supplies bits 33:29 (16 GB per
 * widget); entry bit 12 picks between a widget's two spaces, and the
 * redirection selects memory space, from IRIX's own write of this entry.
 */
#define SN0_XTALK_WIDGETS       16
#define SN0_XTALK_WIDGET_SHIFT  34
#define SN0_XTALK_WIDGET_SIZE   (1ULL << SN0_XTALK_WIDGET_SHIFT)
#define SN0_XTALK_BASE(widget)  ((hwaddr)(widget) << SN0_XTALK_WIDGET_SHIFT)
#define SN0_XTALK_SIZE          (SN0_XTALK_WIDGETS * SN0_XTALK_WIDGET_SIZE)

/*
 * The sixteen small windows fill the bottom sixteenth, one per widget,
 * each reaching that widget's first 16 MB. Widget 0's must decode:
 * Linux reads widget 0 through it deliberately, while OpenBSD goes
 * through the redirected big window; the machine owes both paths.
 */
#define SN0_SMALL_WINDOW_SIZE    (16 * MiB)
#define SN0_SMALL_WINDOW(widget) ((hwaddr)(widget) * SN0_SMALL_WINDOW_SIZE)
#define SN0_XTALK_PORT_LAST      15

/* The seven big windows follow; their aim is the translation table's. */
#define SN0_BIG_WINDOW_BASE      0x20000000

/*
 * The R10000 latches bits 24:0 from the mode bits driven at reset and
 * hardwires the rest; not software-writable, so a write must not stick.
 */
#define SN0_PROCESSOR_MODE_MASK 0x01ffffff  /* bits 24:0, latched at reset */

/* Fixed state capacities; the product model supplies the active counts. */
#define SN0_MAX_CPUS_PER_NODE 2
#define SN0_MAX_ADDRESS_SPACES 4

typedef struct SN0BackdoorState {
    unsigned int node;          /* the window's node, for tracing */
    const char *window;         /* "ecc" or "dir", for tracing */
    /* Read from RAM through the region, written through its callback. */
    MemoryRegion storage;
    uint8_t *data;
    uint64_t data_size;
    uint64_t bank_offset[SN0_MAX_MEMORY_BANKS];
    /* One alias of the storage for each place in the window it repeats. */
    MemoryRegion *fold;
    unsigned int folds;
    uint64_t writes;            /* through the callback, for tracing */
} SN0BackdoorState;

typedef struct SN0NodeState {
    SysBusDevice parent_obj;

    /* Static product description supplied by the owning machine. */
    const SN0ModelPhysicalNode *physical;
    const SN0ModelHardware *hardware;

    /*
     * Properties. promdata-filter installs the PROM's cache-as-RAM
     * overlay; every stock-PROM boot wants it.
     */
    uint32_t nasid;
    uint32_t cpu_base;
    uint32_t nr_cpus;
    char *cpu_type;
    /*
     * Answers module-management commands only through its module's
     * master, never on its own external AUX line -- true for one node
     * of a two-chassis Origin 200, and every other product's nodes.
     */
    bool slave_role;
    MemoryRegion *ram;
    uint64_t ram_start;
    bool promdata_filter;
    bool promdata_fast;
    uint32_t nr_banks_mb;
    uint16_t *banks_mb;

    Clock *cpuclk;
    Clock *rtclk;
    MIPSCPU *cpu[SN0_MAX_CPUS_PER_NODE];
    MemoryRegion cpu_sysmem[SN0_MAX_CPUS_PER_NODE];
    MemoryRegion sysmem_alias[SN0_MAX_CPUS_PER_NODE];
    /* The uncached view: the cached one, less the private window. */
    MemoryRegion cpu_uncached[SN0_MAX_CPUS_PER_NODE];
    MemoryRegion cpu_uncached_alias[SN0_MAX_CPUS_PER_NODE];
    MemoryRegion claim_view[SN0_MAX_CPUS_PER_NODE];
    MemoryRegion local_space[SN0_MAX_CPUS_PER_NODE]
                                  [SN0_MAX_ADDRESS_SPACES];
    MemoryRegion hub_private[SN0_MAX_CPUS_PER_NODE][2];
    SNPromDataState promdata[SN0_MAX_CPUS_PER_NODE];
    /* Samples the mapped processors' stack pointers for the exit. */
    SNPromDataPoll promdata_poll;
    SGIHubState hub;

    /*
     * The Hub's own engines' view: the claim layer alone, so an engine
     * address resolves by programmed node number like a processor's.
     */
    MemoryRegion device_view;

    PFlashCFI02 *flash;
    BlockBackend *flash_drive;
    uint64_t flash_size;
    MemoryRegion flash_reset_view;
    MemoryRegion flash_hub_view;
    MemoryRegion flash_rboot_view;
    MemoryRegion flash_hub_fast;
    MemoryRegion hub_regs_node_view;
    MemoryRegion backdoor_ecc_absent;
    MemoryRegion backdoor_dir_absent;
    SN0BackdoorState backdoor_ecc_state;
    SN0BackdoorState backdoor_dir_state;

    MemoryRegion xtalk;
    AddressSpace origin2000_xtalk_as;
    bool origin2000_xtalk_as_ready;
    MemoryRegion origin2000_bus_error[SN0_MAX_CPUS_PER_NODE];
    MemoryRegion prom_pi_probe;
    SGIBridgeState *prom_pi_probe_bridge;
    unsigned int prom_pi_probe_device_window;
    MemoryRegion small_window[SN0_XTALK_WIDGETS];
    MemoryRegion mem_slot[SN0_MAX_MEMORY_BANKS][SN0_MAX_MEMORY_SLOTS];
    MemoryRegion hub_local_mem_slot[SN0_MAX_MEMORY_BANKS]
                                          [SN0_MAX_MEMORY_SLOTS];
    MemoryRegion hub_remote_mem_slot[SN0_MAX_MEMORY_BANKS]
                                           [SN0_MAX_MEMORY_SLOTS];
    uint16_t bank_mb[SN0_MAX_MEMORY_BANKS];
    unsigned int populated_banks;
} SN0NodeState;

#define TYPE_SGI_SN0_NODE "sgi-sn0-node"
DECLARE_INSTANCE_CHECKER(SN0NodeState, SN0_NODE, TYPE_SGI_SN0_NODE)

#define TYPE_SN0_MACHINE MACHINE_TYPE_NAME("sn0")

typedef struct SN0Origin2000XtalkState SN0Origin2000XtalkState;

struct SN0MachineState {
    MachineState parent_obj;

    /* The installed chassis; nodes and bridges are QOM children. */
    const SN0ModelComposition *composition;
    unsigned int num_nodes;
    /* Origin 2000 uses physical slot minus one, not descriptor array order. */
    SN0NodeState **nodes;
    SGIBridgeState **io_bridge;

    /* What joins the nodes' Hubs; every Hub holds one endpoint on it. */
    SN0FabricState *fabric;

    /* Descriptor-owned Crossbows and stable packet forwarding containers. */
    SN0Origin2000XtalkState *origin2000_xtalk;

    /* The product reference clock every node's processors derive from. */
    Clock *cpu_refclk;

    /*
     * The clock every node's real-time counter counts. Separate from the
     * processor reference because no source relates the two rates; see the
     * Hub processor-interface datasheet.
     */
    Clock *rt_refclk;

    /* Widget 0: a second view of the bridge needs its own alias storage. */
    MemoryRegion *widget0;

    /* The bridge at every other non-zero widget: net-new 2-7 and 9-15. */
    MemoryRegion (*widget_alias)[SN0_XTALK_WIDGETS];

    /* Processor views of every explicitly node-qualified big window. */
    MemoryRegion *big_window_route;

    /* Off by default: not a shipped part, and firmware would prefer it. */
    bool junk_uart_present;

    /* On by default: the only output while firmware picks a console. */
    bool debug_leds;

    /* Physical switch mask returned through each node's ELSC. */
    uint8_t elsc_dip_switches;

    /* Where the firmware's console output goes. */
    Chardev *console;

    /* Realized devices follow the generated PCI descriptor order. */
    PCIDevice ***pci_devices;

};

typedef struct SN0MachineState SN0MachineState;
typedef struct SN0MachineClass SN0MachineClass;

DECLARE_OBJ_CHECKERS(SN0MachineState, SN0MachineClass,
                     SN0_MACHINE, TYPE_SN0_MACHINE)

struct SN0MachineClass {
    /*< private >*/
    MachineClass parent_class;

    /*< public >*/
    /* Static product facts compiled from the canonical machine model. */
    const SN0ModelComposition *default_composition;
};

/* Call once in order; set optional node links between these two phases. */
void sn0_origin2000_prepare_nodes(MachineState *machine);
void sn0_origin2000_realize_nodes(MachineState *machine);
/*
 * Shared by every product: one nameless big-window alias per CPU plus
 * one routed alias per (source CPU, target node) pair. owner parents
 * the new MemoryRegions; *route_out receives the array so the caller
 * can free it (nothing frees it today; the machine object outlives the
 * process, as sn0_instance_finalize's own comment notes).
 */
void sn0_map_cpu_big_windows(Object *owner, SN0NodeState **nodes,
                             unsigned int num_nodes,
                             MemoryRegion **route_out);

extern const SN0FlashImplementation sn0_flash;

/* Reapplied on every processor reset. */
void sn0_mode_bits_reset(void *opaque);

#endif /* HW_MIPS_SN0_H */
