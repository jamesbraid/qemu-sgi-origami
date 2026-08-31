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

/* M mode: node number in bits 39:32, 4 GB per node. N mode is unused. */
#define SN0_NODE_SHIFT      32
#define SN0_NODE_SIZE       (4 * GiB)
#define SN0_NODE_BASE(nasid) ((hwaddr)(nasid) << SN0_NODE_SHIFT)

/* The four uncached spaces, shifted so that memory lands at zero. */
#define SN0_SPACE(n)        ((hwaddr)(n) << MIPS_UNCACHED_WINDOW_SHIFT)
#define SN0_SPACE_MEMORY    SN0_SPACE(0)  /* uncached attribute 3 */
#define SN0_SPACE_HUB       SN0_SPACE(1)  /* uncached attribute 0 */
#define SN0_SPACE_XTALK     SN0_SPACE(2)  /* uncached attribute 1 */
#define SN0_SPACE_MEMSPEC   SN0_SPACE(3)  /* uncached attribute 2 */

/* Node memory: up to eight banks of 512 MB at the bottom of the window. */
#define SN0_MEM_BANK_SIZE   (512 * MiB)

/*
 * A bank is four 128 MB slots: larger than one slot, a quarter goes to
 * each on the 128 MB stride; one slot or less sits wholly in the first.
 */
#define SN0_MEM_SLOT_SIZE   (128 * MiB)
#define SN0_MEM_BANK_SLOTS  (SN0_MEM_BANK_SIZE / SN0_MEM_SLOT_SIZE)
#define SN0_MEM_BANKS       8

/* An uncached local view of the node's own memory, before the boot window. */
#define SN0_HUB_LOCAL_MEM_SIZE (256 * MiB)

/* The Hub's processor lines, named by the Cause bit each one raises. */
#define SN0_HUB_BANK0_CAUSE_BIT 10
#define SN0_HUB_BANK1_CAUSE_BIT 11
#define SN0_HUB_RT_CAUSE_BIT    12

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
#define SN0_XTALK_PORT_FIRST     8
#define SN0_XTALK_PORT_LAST      15

/* The seven big windows follow; their aim is the translation table's. */
#define SN0_BIG_WINDOW_BASE      0x20000000

/*
 * Widget 0 is the I/O bridge seen again -- no crossbar routes by widget
 * number -- so its identity lives on the bridge, not here. The revision
 * varies between real machines (3 and 4 are both captured), so it is a
 * settable option, and guests act on it below revision four.
 */
#define SN0_WIDGET0              0
#define SN0_WIDGET0_REVISION     4
/* The identification field is four bits wide. */
#define SN0_WIDGET0_REVISION_MAX 0xf

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
#define SN0_FIRMWARE_WIDGET0_ENTRY  (SN0_HUB_IO_BWIN_WIDGET(SN0_WIDGET0) | \
                                     SN0_HUB_IO_BWIN_SLICE( \
                                         SN0_FIRMWARE_WIDGET0_SLICE))

/* No valid bit: a window is disabled by aiming it at absent widget 3. */
#define SN0_XTALK_INVALID_WIDGET    3

/*
 * The Hub's widget-1 window: the lower half resolves to the reading
 * processor's own node, the upper half names one. The cached
 * compatibility segment reaches node-local physical zero.
 */
#define SN0_COMPAT_CACHED 0xffffffff80000000ull

#define SN0_HUB_LOCAL_WINDOW 0x01000000
#define SN0_HUB_NODE_WINDOW  0x01800000

/*
 * The boot flash, seen three ways over one backing store: the reset
 * vector through the compatibility segments, Hub-special LBOOT, and
 * node-explicit RBOOT. Both Hub views are 256 MB -- the decode is a
 * chip select, not the PROM's size.
 */
#define SN0_FLASH_SIZE      (1 * MiB)
#define SN0_FLASH_LOCAL     0x1fc00000  /* node offset, reset vector view */
#define SN0_FLASH_HUB       0x10000000  /* node offset within Hub special */
#define SN0_FLASH_RBOOT     0x30000000  /* explicit node's Hub-special view */
#define SN0_LBOOT_SIZE      (256 * MiB) /* the Hub special view's real size */
#define SN0_RBOOT_SIZE      (256 * MiB) /* explicit-node view's real size */

/*
 * The directory and ECC back doors. Firmware's probe reads a written
 * pattern back against two masks to learn whether a bank exists and
 * which directory format it carries: an absent bank's share keeps
 * nothing, a populated bank's only the plain format's low sixteen bits
 * (the Origin 200 has no premium directory memory).
 */
#define SN0_BACKDOOR_ECC_HUB  0x80000000  /* node offset within Hub special */
#define SN0_BACKDOOR_ECC_SIZE (1 * GiB)
#define SN0_BACKDOOR_DIR_HUB  0xc0000000  /* node offset within Hub special */
#define SN0_BACKDOOR_DIR_SIZE (1 * GiB)

/* An even eight-way split, 128 MB per bank, per the public Linux headers. */
#define SN0_BACKDOOR_BANK_STRIDE (SN0_BACKDOOR_ECC_SIZE / SN0_MEM_BANKS)

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
#define SN0_CONFIG_GRAPH        0x4000
#define SN0_CONFIG_GRAPH_SIZE   0xc000

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

/* Every graph link is a 32-bit offset from the start of node memory. */
#define SN0_GRAPH_MAGIC         0xbeedbabeULL
#define SN0_GRAPH_CONSOLE_OFF   0x18    /* where the descriptor is embedded */
#define SN0_GRAPH_BOARDS        0x4100
#define SN0_GRAPH_BOARD_SIZE    0xc0
#define SN0_GRAPH_COMPONENTS    0x4400
#define SN0_GRAPH_COMP_STRIDE   0x80

/* Board types: the high nibble is the class, the low nibble the type in it. */
#define SN0_BOARD_NODE          0x11
#define SN0_BOARD_MIDPLANE      0x41
#define SN0_BOARD_BASE_IO       0x21
#define SN0_BOARD_LOCAL         1       /* record kind */

/* IRIX treats a version below two as unnamed; the node board gets 3. */
#define SN0_BOARD_VERSION_NODE  3
#define SN0_BOARD_VERSION       2       /* every other board */

/* Component types, and the flag that stops a consumer skipping one. */
#define SN0_COMP_PROCESSOR      1
#define SN0_COMP_HUB            2
#define SN0_COMP_MEMORY         3
#define SN0_COMP_BRIDGE         5
#define SN0_COMP_IO_CONTROLLER  6
#define SN0_COMP_SCSI           11
#define SN0_COMP_MODULE_SERIAL  26
#define SN0_COMP_ENABLED        1
#define SN0_COMP_REV_UNKNOWN    0xff
#define SN0_COMP_REV_HUB        3
#define SN0_COMP_REV_BRIDGE     4
#define SN0_COMP_REV_SCSI       5
#define SN0_COMP_REV_IOC3       1

/* A disk controller's PCI device number; a guest's boot walk checks it. */
#define SN0_COMP_PHYSICAL_ID    0x18

/*
 * The console serial port, composed term by term: widget 8's window,
 * plus the bridge's third-device window, plus port A in the controller.
 * A guest drives the low twenty bits and reads the low twelve as the
 * controller's generation -- two masks, two different meanings.
 */
#define SN0_BRIDGE_DEVICE2      0x600000  /* the third device's window */

/*
 * The base I/O board fills devices 0-2. The second-interrupt pin is 3
 * because 3 is the first empty device: OpenBSD scans upward, stops at
 * the first empty slot, and uses that index as the pin.
 */
#define SN0_SCSI_DEVICE0        0         /* first SCSI adapter's PCI device */
#define SN0_SCSI_DEVICE1        1         /* second SCSI adapter's PCI device */
#define SN0_SCSI_ADAPTERS       2         /* what the base I/O board carries */
#define SN0_IOC3_DEVICE         2         /* the I/O controller's PCI device */

/* Board wiring, not the Bridge's slot-equals-pin default. */
#define SN0_IOC3_SUPERIO_PIN    3         /* its second interrupt's Bridge pin */

/*
 * Where firmware places each device's resources. Linux is the source
 * for what a guest requires: PCI_PROBE_ONLY trusts firmware's BARs, and
 * its claimable window starts at the widget base, so each device's PCI
 * address is the widget base plus its own slot's window offset. Every
 * window is pre-aimed -- Linux never aims one itself -- and none of the
 * addresses may be zero: generic PCI code refuses a zero BAR.
 */
#define SN0_PCI_WIDGET_BASE     ((uint32_t)SN0_CONSOLE_WIDGET << 24)
#define SN0_PCI_SCSI0_WINDOW    0x00200000  /* slot 0's own 2 MB window */
#define SN0_PCI_SCSI1_WINDOW    0x00400000  /* slot 1's own 2 MB window */
#define SN0_PCI_IOC3_MEM        (SN0_PCI_WIDGET_BASE + SN0_BRIDGE_DEVICE2)
#define SN0_PCI_SCSI0_MEM       (SN0_PCI_WIDGET_BASE + SN0_PCI_SCSI0_WINDOW)
#define SN0_PCI_SCSI1_MEM       (SN0_PCI_WIDGET_BASE + SN0_PCI_SCSI1_WINDOW)
#define SN0_PCI_SCSI0_IO        SN0_PCI_SCSI0_MEM
#define SN0_PCI_SCSI1_IO        SN0_PCI_SCSI1_MEM
#define SN0_IOC3_SERIAL_A       0x020178  /* serial port A in the controller */
#define SN0_CONSOLE_PORT_OFFSET (SN0_BRIDGE_DEVICE2 + SN0_IOC3_SERIAL_A)
#define SN0_CONSOLE_PORT_MASK   0xfffff   /* the part the guest drives */
#define SN0_CONSOLE_WIDGET      8
#define SN0_CONSOLE_BAUD        9600

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

/* The Am29F080's erase sector, shared by chip model and image builder. */
#define SN0_FLASH_SECTOR_SIZE (64 * KiB)

/* One chassis is one node: two processor slots beside one Hub. */
#define SN0_CPUS_PER_NODE 2
#define SN0_UNCACHED_SPACES 4

typedef struct SN0BackdoorState {
    uint8_t *data;
    uint32_t data_size;
    uint64_t bank_offset[SN0_MEM_BANKS];
    uint64_t bank_span[SN0_MEM_BANKS];
    unsigned int populated_banks;
} SN0BackdoorState;

typedef struct SN0PromDataState {
    MIPSCPU *cpu;
    hwaddr node_base;
    MemoryRegion data;
    MemoryRegion filter;
    bool present;           /* the filter was installed (real PROM boot) */
    bool leaving_private;   /* the overlay has been (or is being) disabled */
    bool phase_private;     /* the cache-as-RAM phase is latched for this CPU */
} SN0PromDataState;

typedef struct SN0NodeState {
    SysBusDevice parent_obj;

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
    MIPSCPU *cpu[SN0_CPUS_PER_NODE];
    MemoryRegion cpu_sysmem[SN0_CPUS_PER_NODE];
    MemoryRegion sysmem_alias[SN0_CPUS_PER_NODE];
    MemoryRegion local_space[SN0_CPUS_PER_NODE][SN0_UNCACHED_SPACES];
    MemoryRegion hub_private[SN0_CPUS_PER_NODE][2];
    SN0PromDataState promdata[SN0_CPUS_PER_NODE];
    SGIHubState hub;

    /* The Hub's own view of memory, for its block transfer engines. */
    MemoryRegion hub_memory;
    MemoryRegion hub_memory_system;
    MemoryRegion hub_memory_local;

    PFlashCFI02 *flash;
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
    MemoryRegion small_window[SN0_XTALK_WIDGETS];
    MemoryRegion mem_slot[SN0_MEM_BANKS][SN0_MEM_BANK_SLOTS];
    MemoryRegion hub_local_mem_slot[SN0_MEM_BANKS][SN0_MEM_BANK_SLOTS];
    uint16_t bank_mb[SN0_MEM_BANKS];
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

    /*
     * On by default: a real Origin 200 always has one and every device
     * hangs off it, so off is a bring-up aid. It does not gate widget 0,
     * which must keep answering or OpenBSD panics.
     */
    bool io_bridge_present;

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

    /* The devices whose resources firmware reassigns on every reset. */
    PCIDevice **pci_ioc3;
    PCIDevice *(*pci_scsi)[SN0_SCSI_ADAPTERS];

    /*
     * Candidate values: derived facts, settable so two runs can differ
     * in one value and nothing else; moving one out to a constant is how
     * an observation settles it.
     */
    uint32_t widget0_revision;

};

typedef struct SN0MachineState SN0MachineState;
typedef struct SN0MachineClass SN0MachineClass;

DECLARE_OBJ_CHECKERS(SN0MachineState, SN0MachineClass,
                     SN0_MACHINE, TYPE_SN0_MACHINE)

/*
 * What a concrete product's class_init fills in (the PowerNV and Aspeed
 * shape); the processor module is not product identity.
 */
struct SN0MachineClass {
    /*< private >*/
    MachineClass parent_class;

    /*< public >*/
    /* How many chassis the product couples over CrayLink. */
    unsigned int max_nodes;

    /* The node board's clocks, advertised through the configuration block. */
    uint64_t hub_hz;
    uint64_t rtc_hz;

    /* The machine-type word (0: Origin 2000, 1: Origin 200) and timings. */
    uint32_t fw_machine_type;
    uint32_t fprom_cyc;
    uint32_t fprom_wr;

    /*
     * The installed module, not product identity. The mode bits are what
     * the board drives at reset and the block advertises; firmware
     * compares the two, so one place holds both.
     */
    struct {
        uint64_t clock_hz;
        uint32_t scache_clock_mhz;
        uint32_t mode_bits;
    } cpu_module;

    /* Fit a node's memory into the board's banks; false when it cannot. */
    bool (*node_memory_layout)(uint64_t ram_size,
                               uint16_t bank_mb[SN0_MEM_BANKS],
                               unsigned int *populated_banks);

    /* Build the product's I/O topology on one node. */
    void (*populate_io)(SN0MachineState *s, unsigned int node_index);
};

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
