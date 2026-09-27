/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) physical address map and family machine,
 * shared by the family's translation units. One flat 40-bit physical address
 * space: the node number is the top eight bits, so any node's resources are
 * reached by changing those bits alone; the two bits above pick which of four
 * spaces an uncached reference means.
 */

#ifndef HW_MIPS_SN0_H
#define HW_MIPS_SN0_H

#include "cpu.h"
#include "qapi/error.h"
#include "hw/block/flash.h"
#include "hw/core/boards.h"
#include "hw/misc/sgi-hub.h"
#include "hw/mips/sgi/sn-catalogue.h"
#include "hw/mips/sgi/xio-link.h"
#include "sn-promdata.h"

/*
 * The Hub ASIC's address-map and memory-geometry shapes: device facts shared
 * by sn0.c (the Hub node, common to every SN0 product) and the family builder
 * (sn-machine.c) that feeds it, identical on every SN0 node measured and
 * defined once in sn0.c (sn0_hub_address_map, sn0_hub_memory below). A node's
 * fitted-part facts (identity, slot, flash kind, populated memory banks)
 * come straight from its compiled-catalogue record (SGIModelNode,
 * qapi/sgi-sn.json); the node device takes that record directly, the way the
 * SN1 node does, rather than through a repacked copy.
 */

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
    SN0ModelAddressSpace io;
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

typedef struct SN0ModelMemory {
    const char *board;
    unsigned int physical_banks;
    unsigned int architectural_banks;
    const uint64_t *supported_mib;
    unsigned int supported_size_count;
    uint64_t first_bank_max_mib;
    uint64_t bank_stride_mib;
    uint64_t interleave_slot_stride_mib;
    unsigned int interleave_slots_per_bank;
} SN0ModelMemory;

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
 * XIO space as the Hub decodes it: a widget number and an offset; one
 * container indexed by widget, so a small window and a big window
 * are two views of one address. A translation entry supplies bits 33:29
 * (16 GB per widget); entry bit 12 picks between a widget's two spaces,
 * and the redirection selects memory space, from IRIX's write of this entry.
 */
#define SN0_XIO_WIDGETS       16
#define SN0_XIO_WIDGET_SHIFT  34
#define SN0_XIO_WIDGET_SIZE   (1ULL << SN0_XIO_WIDGET_SHIFT)
#define SN0_XIO_BASE(widget)  ((hwaddr)(widget) << SN0_XIO_WIDGET_SHIFT)
#define SN0_XIO_SIZE          (SN0_XIO_WIDGETS * SN0_XIO_WIDGET_SIZE)

/*
 * The sixteen small windows fill the bottom sixteenth, one per widget, each
 * reaching that widget's first 16 MB. Widget 0's must decode: Linux reads
 * widget 0 through it deliberately, while OpenBSD goes through the redirected
 * big window; the machine owes both paths.
 */
#define SN0_SMALL_WINDOW_SIZE    (16 * MiB)
#define SN0_SMALL_WINDOW(widget) ((hwaddr)(widget) * SN0_SMALL_WINDOW_SIZE)
#define SN0_XIO_WIDGET_LAST      15

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

    /*
     * The topology's node record, set by the machine before realize: the
     * compiled catalogue's descriptor this node builds from
     * (qapi/sgi-sn.json).
     */
    const SGIModelNode *model_node;

    /*
     * The node board's bank wiring and sizes, set by the machine before
     * realize; NULL keeps the Origin 200 board's.
     */
    const SN0ModelMemory *memory;

    /*
     * Properties. promdata-filter installs the PROM's cache-as-RAM overlay;
     * every stock-PROM boot wants it.
     */
    uint32_t nasid;
    uint32_t cpu_base;
    uint32_t nr_cpus;
    char *cpu_type;
    /*
     * Answers module-management commands only through its module's master,
     * never on its external AUX line. This is true for one node of a
     * two-chassis Origin 200, and every other product's nodes.
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
     * The Hub's engines' view: the claim layer alone, so an engine address
     * resolves by programmed node number like a processor's.
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

    MemoryRegion xio;
    MemoryRegion small_window[SN0_XIO_WIDGETS];
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
 * Shared by every product: one nameless big-window alias per CPU plus one
 * routed alias per (source CPU, target node) pair. owner parents the new
 * MemoryRegions; *route_out receives the array so the caller can free it
 * (nothing frees it today; the machine object outlives the process, as
 * sn0_instance_finalize's comment notes).
 */
void sn0_map_cpu_big_windows(Object *owner, SN0NodeState **nodes,
                             unsigned int num_nodes,
                             MemoryRegion **route_out);

/*
 * Builds node's onboard base I/O (a Bridge and an sgi-baseio behind it,
 * the "ip29" preset), joins the Bridge through link to upstream's port
 * upstream_index (the node's own Hub, or a GIGAchannel cabinet's Crossbow
 * port) and returns it through bridge_out. Called once node has realized.
 * link is the caller's own storage; the various ids are resolved backend
 * names, not claimed backends, for the claim-timing reason sgi-baseio's
 * own header explains.
 */
bool sn0_node_build_baseio(SN0NodeState *node, Object *owner,
                           SGIXioLink *link, Object *upstream,
                           unsigned int upstream_index,
                           uint64_t bridge_serial,
                           const uint64_t *ioc3_serial, const char *pci_bus_name,
                           const char *nvram_drive_id,
                           const char *chardev_a_id,
                           const char *chardev_b_id,
                           const char *record_a_id,
                           const char *record_b_id,
                           const char *parallel_chardev_id,
                           DeviceState **bridge_out, Error **errp);

extern const SN0FlashImplementation sn0_flash;

/* The Hub's physical address map and memory-bank geometry, defined once. */
extern const SN0ModelAddressMap sn0_hub_address_map;
extern const SN0ModelMemory sn0_hub_memory;
extern const SN0ModelMemory sn0_ip27_memory;

/* The Hub's two fixed clock rates: hardwired, not configuration. */
#define SN0_HUB_CLOCK_HZ     0x55d4a80ULL
#define SN0_HUB_RTC_CLOCK_HZ 0x1312d0ULL

/*
 * The Hub register offset its I2C adapter answers at, whether it carries a
 * private controller (Origin 200) or is one of a shared module controller's
 * per-node adapters (Origin 2000's MSC): fixed by the Hub chip, the same on
 * every node measured, so the compiled catalogue no longer carries it.
 */
#define SN0_HUB_MODULE_ADAPTER_OFFSET 0x220000ULL

/*
 * A node's embedded ELSC answers I2C address 8 on every Hub that carries one
 * (identity->system_controller); its NVRAM pages start at address 80 and
 * there are 8 of them, again fixed by the ELSC/PCF8584 pairing, the same on
 * every node measured. The compiled catalogue no longer carries these (unread
 * where the corresponding identity flag is false, since that Hub has no
 * embedded ELSC or I2C controller to address).
 */
#define SN0_HUB_I2C_ELSC_ADDR         8
#define SN0_HUB_I2C_NVRAM_FIRST_ADDR  80
#define SN0_HUB_I2C_NVRAM_PAGES       8

#endif /* HW_MIPS_SN0_H */
