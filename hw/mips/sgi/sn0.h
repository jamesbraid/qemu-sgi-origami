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
#include "sn-promdata.h"

/*
 * The shared SN0 node builder's own vocabulary: what sn0.c (the Hub node,
 * common to every SN0 product) and the family builder (sn-machine.c) that
 * feeds it need to construct one node. This is not a per-product
 * descriptor -- the family builder populates these from the format-2
 * descriptor (sn-model.h) once per node at machine-init time, the way it
 * already builds SN0ModelComponent from SGIModelAsicIdentity. What used
 * to be the whole per-product descriptor (SN0ModelComposition and the
 * types only it needed: modules, routers, crossbows, xtalk attachments,
 * I/O boards) is gone with the old descriptor and its compilers; a
 * product's shape now comes from sn-model.h alone.
 */

typedef enum SN0ModelCpuKind {
    SN0_CPU_R10000,
} SN0ModelCpuKind;

typedef enum SN0ModelComponentKind {
    SN0_COMPONENT_HUB,
    SN0_COMPONENT_BASE_IO,
    SN0_COMPONENT_IOC3,
    SN0_COMPONENT_ISP1040,
} SN0ModelComponentKind;

typedef struct SN0ModelAddressSpace {
    unsigned int selector;
    uint64_t carrier_base;
    uint64_t unmapped_base;
} SN0ModelAddressSpace;

typedef struct SN0ModelRegion {
    uint64_t offset;
    uint64_t size;
} SN0ModelRegion;

typedef struct SN0ModelAddressMap {
    uint64_t node_stride;
    SN0ModelAddressSpace memory;
    SN0ModelAddressSpace hub_special;
    SN0ModelAddressSpace xtalk;
    SN0ModelAddressSpace memory_special;
    SN0ModelRegion node_memory;
    SN0ModelRegion hub_registers_local;
    SN0ModelRegion hub_registers_node;
    SN0ModelRegion hub_local_memory;
    SN0ModelRegion hub_remote_memory;
    SN0ModelRegion backdoor_ecc;
    SN0ModelRegion backdoor_directory;
    SN0ModelRegion flash_reset;
    const SN0ModelRegion *lboot;
    SN0ModelRegion rboot;
    SN0ModelRegion bridge_registers;
} SN0ModelAddressMap;

typedef struct SN0ModelComponent {
    SN0ModelComponentKind kind;
    uint8_t type;
    uint8_t revision;
    uint8_t ni_revision;
    uint8_t widget_revision;
    uint8_t physical_id;
    uint8_t widget;
    uint8_t device;
    uint8_t function;
    int8_t irq_input;
    uint16_t subsystem_id;
    uint32_t bar[2];
    uint8_t cpu_irq_bank0;
    uint8_t cpu_irq_bank1;
    uint8_t cpu_irq_rt;
    bool i2c_controller;
    bool system_controller;
    uint64_t i2c_controller_offset;
    uint8_t i2c_elsc_addr;
    uint8_t i2c_nvram_first_addr;
    uint8_t i2c_nvram_pages;
    uint8_t discovery_widget;
    uint16_t decoded_widget_mask;
    const char *part;
    uint64_t serial;
    uint8_t prom_pi_probe_widget;
    uint64_t prom_pi_probe_offset;
    uint64_t prom_pi_probe_window_offset;
} SN0ModelComponent;

typedef struct SN0ModelMemory {
    unsigned int physical_banks;
    unsigned int architectural_banks;
    const uint64_t *supported_mib;
    unsigned int supported_size_count;
    uint64_t first_bank_max_mib;
    uint64_t bank_stride_mib;
    uint64_t interleave_slot_stride_mib;
    unsigned int interleave_slots_per_bank;
} SN0ModelMemory;

typedef struct SN0ModelIoc3Population {
    bool ethernet;
    bool serial_a;
    bool serial_b;
    bool keyboard_mouse;
    bool parallel;
    bool timekeeper;
    bool onewire_identity;
} SN0ModelIoc3Population;

/* The physical flash selection, independent of firmware contents. */
typedef enum SN0ModelFlashKind {
    SN0_FLASH_AM29F080,
} SN0ModelFlashKind;

typedef struct SN0ModelFlash {
    SN0ModelFlashKind kind;
} SN0ModelFlash;

#define SN0_MODEL_MODULE_IDENTITY_PAGES 2
#define SN0_MODEL_ONEWIRE_PAGE_SIZE 32

typedef struct SN0ModelModuleIdentity {
    uint8_t rom[8];
    uint8_t page[SN0_MODEL_MODULE_IDENTITY_PAGES]
                [SN0_MODEL_ONEWIRE_PAGE_SIZE];
} SN0ModelModuleIdentity;

typedef struct SN0ModelHardware {
    SN0ModelCpuKind cpu_kind;
    uint64_t cpu_clock_hz;
    uint32_t cpu_mode_bits;
    unsigned int cpu_slots_per_node;
    uint64_t hub_clock_hz;
    uint64_t rtc_clock_hz;
    uint32_t firmware_machine_type;
    uint32_t flash_read_cycles;
    uint32_t flash_write_cycles;
    SN0ModelMemory memory;
} SN0ModelHardware;

typedef struct SN0ModelPhysicalNode {
    const char *id;
    unsigned int module_index;
    unsigned int slot;
    unsigned int cpu_first_id;
    unsigned int cpu_count;
    uint64_t memory_mib;
    uint64_t bank_mib[8];
    const SN0ModelComponent *hub;
    const SN0ModelModuleIdentity *module_identity;
    SN0ModelAddressMap map;
    SN0ModelFlash flash;
} SN0ModelPhysicalNode;

uint64_t sn0_model_node_address(const SN0ModelAddressMap *map,
                                const SN0ModelAddressSpace *space,
                                unsigned int nasid, uint64_t offset);
uint64_t sn0_model_region_address(const SN0ModelAddressMap *map,
                                  const SN0ModelAddressSpace *space,
                                  unsigned int nasid,
                                  const SN0ModelRegion *region,
                                  uint64_t offset);
bool sn0_model_memory_layout(const SN0ModelMemory *memory, uint64_t ram_size,
                             uint16_t *bank_mb,
                             unsigned int *populated_banks);

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

#endif /* HW_MIPS_SN0_H */
