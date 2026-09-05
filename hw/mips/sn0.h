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

/* The identification field is four bits wide. */
/*
 * The firmware's widget-0 redirection: the last big window aimed at
 * widget 0, which both guests expect done. OpenBSD's allocator would
 * read the entry's contents as a free entry; a hard-coded index
 * exclusion is the only protection.
 */
#define SN0_HUB_IO_BWIN_TABLE       0x400160
#define SN0_HUB_IO_BWIN_STRIDE      8
#define SN0_HUB_IO_BWIN_ENTRY(n)    (SN0_HUB_IO_BWIN_TABLE + \
                                     (n) * SN0_HUB_IO_BWIN_STRIDE)
#define SN0_HUB_IO_BWIN_WIDGET(w)   ((uint64_t)(w) << 8)
#define SN0_HUB_IO_BWIN_SLICE(x)    ((uint64_t)(x))

/* The last window carries it; software counts 0-6, SGI manuals 1-7. */
#define SN0_FIRMWARE_WIDGET0_WINDOW 6

/* Term by term: each address error here was a dropped term in a sum. */
#define SN0_FIRMWARE_WIDGET0_SLICE  0

/*
 * The Hub's widget-1 window: the lower half resolves to the reading
 * processor's own node, the upper half names one. The cached
 * compatibility segment reaches node-local physical zero.
 */
#define SN0_COMPAT_CACHED 0xffffffff80000000ull

/* Ten entry points at the start of the flash, eight bytes apart. */
#define SN0_FLASH_ENTRIES   10
#define SN0_FLASH_ENTRY_STRIDE 8

/* The low-memory structures, fixed by the boot structures datasheet. */
#define SN0_PARAM_BLOCK         0x1000
#define SN0_PARAM_SIGNATURE     0x53435241  /* a constant, not "ARCS" */
#define SN0_HW_DIRECTORY        0x2000
#define SN0_HW_DIR_ENTRIES      16
#define SN0_HW_DIR_ENTRY_SIZE   64
#define SN0_HW_DIR_SIGNATURE    0x434d5f53505f5357ULL
#define SN0_HW_DIR_LAUNCH       0
#define SN0_HW_DIR_CONFIG_GRAPH 1
#define SN0_HW_DIR_NMI          2
#define SN0_HW_DIR_GDA          3
#define SN0_HW_DIR_FREEMEM      4
#define SN0_HW_DIR_DEBUG_STACKS 5
#define SN0_HW_DIR_PI_ERROR     6

/* The directory's size field spans both launch blocks together. */
#define SN0_LAUNCH_BLOCKS       0x2400
#define SN0_LAUNCH_ENTRIES      2
#define SN0_LAUNCH_STRIDE       0x200
#define SN0_LAUNCH_DIR_SIZE     (SN0_LAUNCH_ENTRIES * SN0_LAUNCH_STRIDE)
#define SN0_LAUNCH_SIGNATURE    0xaddbead2addbead3ULL

/* Here the size field is one block -- the element a guest indexes. */
#define SN0_NMI_BLOCKS          0x3000
#define SN0_NMI_ENTRIES         2
#define SN0_NMI_STRIDE          0x40
#define SN0_NMI_DIR_SIZE        SN0_NMI_STRIDE

/* Entries with no content of their own: offset, size, count, stride. */
#define SN0_HWDIR_FREEMEM_SIZE        ((uint64_t)-1)
#define SN0_HWDIR_DEBUG_STACKS_BASE   0x25000
#define SN0_HWDIR_DEBUG_STACKS_SIZE   0xe000
#define SN0_HWDIR_DEBUG_STACKS_N      2
#define SN0_HWDIR_DEBUG_STACKS_STRIDE 0x7000
#define SN0_HWDIR_PI_ERROR_BASE       0x12000
#define SN0_HWDIR_PI_ERROR_SIZE       0x4000

/* KLCONFIG component state, independent of product contents. */
#define SN0_COMP_ENABLED        1

/* The -serial line assignment lives in hw/misc/sgi-hub.h; both units see it. */
#define SN0_GDA                 0x11000
#define SN0_GDA_SIZE            0x400
#define SN0_GDA_SIGNATURE       0x58464552  /* "XFER" */
#define SN0_GDA_VERSION         2
#define SN0_GDA_NODE_TABLE      0x80
/* 64 signed halfword entries; a byte-per-entry write reads as node 255. */
#define SN0_GDA_NODES           64
#define SN0_GDA_NODE_ABSENT     0xffff
#define SN0_FIRST_FREE          0x19000

/* A guest holds the pointer and reads through it, so the list is real. */
#define SN0_MEM_LIST            0x900

/* Thirty-five entries, none null -- guests jump through five of them. */
#define SN0_ARCS_VECTOR         0x2800
#define SN0_ARCS_ENTRIES        35
#define SN0_ARCS_GET_CHILD      10      /* entry at offset 0x50 */
#define SN0_ARCS_GET_SYSTEM_ID  17      /* entry at offset 0x88 */
#define SN0_ARCS_GET_MEMORY     18      /* entry at offset 0x90 */
#define SN0_ARCS_WRITE          27      /* entry at offset 0xd8 */
#define SN0_ARCS_GET_ENV        30      /* entry at offset 0xf0 */

/* Seventeen more entries continuing the same trap page and op numbers. */
#define SN0_ARCS_PRIVATE_VECTOR   (SN0_ARCS_VECTOR + 0x400)
#define SN0_ARCS_PRIVATE_ENTRIES  17
#define SN0_ARCS_PRIVATE_BASE_OP  SN0_ARCS_ENTRIES
/* Capability level, private-vector entry at offset 0x48 -> index 9. */
#define SN0_ARCS_CAPABILITY_OP    (SN0_ARCS_PRIVATE_BASE_OP + 9)
/* Fixed-PROM launch_slave entry, implemented by the synthesized firmware. */
#define SN0_FIRMWARE_LAUNCH_OP    (SN0_ARCS_PRIVATE_BASE_OP + \
                                   SN0_ARCS_PRIVATE_ENTRIES)
/* Level three: the firmware can load a relocatable object. */
#define SN0_ARCS_CAPABILITY_LEVEL 3

/* A stub is four instructions: form the address, load from it, return. */
#define SN0_ARCS_STUB_SIZE      16
#define SN0_ARCS_TRAP_LOCAL     0x1fd00000  /* node offset, past the flash */
#define SN0_ARCS_TRAP_SIZE      0x1000

/* Outside the flash, which belongs to whatever image was put there. */
#define SN0_FW_TEXT_LOCAL       0x1fd01000
#define SN0_FW_TEXT_SIZE        0x1000
#define SN0_FW_BOOTLOADER       0x000       /* offset within that region */
#define SN0_ARCS_STUBS          0x100
#define SN0_FW_LAUNCH_STUB      0x500
#define SN0_FW_SECONDARY_BOOT   0x520

/* launch_slave is fixed entry seven in the IP27 PROM jump table. */
#define SN0_FLASH_LAUNCHSLAVE   (7 * SN0_FLASH_ENTRY_STRIDE)

/* The firmware configuration block, at flash offset 0x60. */
#define SN0_CONFIG_OFFSET   0x60
#define SN0_CONFIG_MAGIC    0x69703237636f6e66ULL  /* "ip27conf" */

/*
 * The R10000 latches bits 24:0 from the mode bits driven at reset and
 * hardwires the rest; not software-writable, so a write must not stick.
 */
#define SN0_PROCESSOR_MODE_MASK 0x01ffffff  /* bits 24:0, latched at reset */

/* Fixed state capacities; the product model supplies the active counts. */
#define SN0_MAX_CPUS_PER_NODE 2
#define SN0_MAX_ADDRESS_SPACES 4

typedef struct SN0BackdoorState {
    uint8_t *data;
    uint32_t data_size;
    uint64_t bank_stride;
    uint64_t bank_offset[SN0_MAX_MEMORY_BANKS];
    uint64_t bank_span[SN0_MAX_MEMORY_BANKS];
    unsigned int populated_banks;
} SN0BackdoorState;

typedef struct SN0PromDataState {
    MIPSCPU *cpu;
    const SN0ModelAddressMap *map;
    hwaddr node_base;
    MemoryRegion data;
    MemoryRegion filter;
    bool present;           /* the filter was installed (real PROM boot) */
    bool leaving_private;   /* the overlay has been (or is being) disabled */
    bool phase_private;     /* the cache-as-RAM phase is latched for this CPU */
} SN0PromDataState;

typedef struct SN0NodeState {
    SysBusDevice parent_obj;

    /* Static product description supplied by the owning machine. */
    const SN0MachineModel *model;
    const SN0ModelNodeInstance *instance;
    const SN0ModelNodeTopology *topology;

    /*
     * Properties. local-memory-base is where a NASID-less memory-space
     * address lands -- node 0's window once synthesized firmware hands a
     * directly loaded kernel control; promdata-filter installs the
     * PROM's cache-as-RAM overlay.
     */
    uint32_t nasid;
    uint32_t cpu_base;
    uint32_t nr_cpus;
    char *cpu_type;
    MemoryRegion *ram;
    uint64_t ram_start;
    uint64_t local_memory_base;
    bool promdata_filter;
    uint32_t nr_banks_mb;
    uint16_t *banks_mb;

    Clock *cpuclk;
    MIPSCPU *cpu[SN0_MAX_CPUS_PER_NODE];
    MemoryRegion cpu_sysmem[SN0_MAX_CPUS_PER_NODE];
    MemoryRegion sysmem_alias[SN0_MAX_CPUS_PER_NODE];
    MemoryRegion claim_view[SN0_MAX_CPUS_PER_NODE];
    MemoryRegion local_space[SN0_MAX_CPUS_PER_NODE]
                                  [SN0_MAX_ADDRESS_SPACES];
    MemoryRegion hub_private[SN0_MAX_CPUS_PER_NODE][2];
    SN0PromDataState promdata[SN0_MAX_CPUS_PER_NODE];
    SGIHubState hub;

    /*
     * The Hub's own engines' view: the claim layer alone, so an engine
     * address resolves by programmed node number like a processor's.
     */
    MemoryRegion device_view;

    PFlashCFI02 *flash;
    uint64_t flash_size;
    MemoryRegion flash_reset_view;
    MemoryRegion flash_hub_view;
    MemoryRegion flash_rboot_view;
    MemoryRegion flash_hub_fast;
    MemoryRegion hub_regs_node_view;
    MemoryRegion arcs_trap;
    MemoryRegion fw_text;
    MemoryRegion backdoor_ecc;
    MemoryRegion backdoor_ecc_absent;
    MemoryRegion backdoor_dir;
    MemoryRegion backdoor_dir_absent;
    SN0BackdoorState backdoor_ecc_state;
    SN0BackdoorState backdoor_dir_state;

    MemoryRegion xtalk;
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

    uint64_t mem_addr;
    unsigned int mem_count;
} SN0NodeState;

#define TYPE_SN0_MACHINE MACHINE_TYPE_NAME("sn0")

/* The flight recorder behind diagnostic-recording (sn0-diag.c). */
typedef struct SN0DiagnosticEvent SN0DiagnosticEvent;
typedef struct SN0DiagnosticReadCacheEntry SN0DiagnosticReadCacheEntry;

typedef struct SN0DiagnosticSinkContext {
    void *machine;
    uint32_t node;
} SN0DiagnosticSinkContext;

struct SN0MachineState {
    MachineState parent_obj;

    /* The installed chassis; nodes and bridges are QOM children. */
    unsigned int num_nodes;
    SN0NodeState **nodes;
    SGIBridgeState **io_bridge;

    /* What joins the nodes' Hubs; every Hub holds one endpoint on it. */
    SN0FabricState *fabric;

    /* The product reference clock every node's processors derive from. */
    Clock *cpu_refclk;

    /* Host diagnostics only; not guest-visible and not migrated. */
    bool diagnostic_recording;
    uint64_t diagnostic_query_sequence;
    SN0DiagnosticEvent *diagnostic_events;
    uint32_t diagnostic_event_head;
    uint32_t diagnostic_event_count;
    uint64_t diagnostic_event_next_sequence;
    uint64_t diagnostic_event_dropped;
    SN0DiagnosticReadCacheEntry *diagnostic_read_cache;
    uint16_t *diagnostic_read_cache_next;
    DeviceDiagnosticSink *diagnostic_sink;
    SN0DiagnosticSinkContext *diagnostic_sink_context;

    /* Widget 0: a second view of the bridge needs its own alias storage. */
    MemoryRegion *widget0;

    /* The bridge at every other non-zero widget: net-new 2-7 and 9-15. */
    MemoryRegion (*widget_alias)[SN0_XTALK_WIDGETS];

    /* Off by default: not a shipped part, and firmware would prefer it. */
    bool junk_uart_present;

    /* On by default: the only output while firmware picks a console. */
    bool debug_leds;

    /* Whether this machine presents firmware of its own. */
    bool synthesized_firmware;

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
    const SN0MachineModel *model;
};

void sn0_model_populate_node(SN0MachineState *s, unsigned int node_index);
void sn0_model_configure_diagnostic(SN0MachineState *s,
                                    unsigned int node_index);

extern const SN0FlashImplementation sn0_flash;

void sn0_diagnostic_attach(SN0MachineState *s, unsigned int node_index);
void sn0_diagnostic_machine_reset(SN0MachineState *s);
void sn0_diagnostic_finalize(SN0MachineState *s);

/* Reapplied on every processor reset and by the firmware launch path. */
void sn0_mode_bits_reset(void *opaque);

/*
 * The synthesized firmware and direct kernel boot (sn0-firmware.c); the
 * reset handlers redo the widget-0 redirection and the PCI assignment.
 */
void sn0_firmware_init(MachineState *machine);
void sn0_firmware_reset(void *opaque);
void sn0_assign_pci_resources(void *opaque);

#endif /* HW_MIPS_SN0_H */
