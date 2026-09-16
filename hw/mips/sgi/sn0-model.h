/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private interface between the generated SN0 product data and QEMU. */

#ifndef HW_MIPS_SN0_MODEL_H
#define HW_MIPS_SN0_MODEL_H

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

#define SN0_MODEL_MODULE_SERIAL_LEN 10
#define SN0_MODEL_MODULE_IDENTITY_PAGES 2
#define SN0_MODEL_ONEWIRE_PAGE_SIZE 32

typedef struct SN0ModelModuleIdentity {
    uint8_t rom[8];
    uint8_t page[SN0_MODEL_MODULE_IDENTITY_PAGES]
                [SN0_MODEL_ONEWIRE_PAGE_SIZE];
} SN0ModelModuleIdentity;

typedef enum SN0ModelFabricVertexKind {
    SN0_MODEL_FABRIC_HUB,
    SN0_MODEL_FABRIC_ROUTER,
} SN0ModelFabricVertexKind;

typedef enum SN0ModelFabricLinkKind {
    SN0_MODEL_LINK_MIDPLANE,
    SN0_MODEL_LINK_CABLE,
    SN0_MODEL_LINK_XPRESS_CABLE,
    SN0_MODEL_LINK_CARRIER,
    SN0_MODEL_LINK_NULL_ROUTER,
} SN0ModelFabricLinkKind;

typedef struct SN0ModelFabricVertex {
    SN0ModelFabricVertexKind kind;
    unsigned int index;
    unsigned int port;
} SN0ModelFabricVertex;

typedef struct SN0ModelRoutedFabricLink {
    const char *name;
    SN0ModelFabricVertex ends[2];
    SN0ModelFabricLinkKind kind;
} SN0ModelRoutedFabricLink;

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

typedef struct SN0ModelRouter {
    const char *id;
    unsigned int module_index;
    unsigned int slot;
    uint16_t part;
    uint8_t revision;
    uint8_t chip_inputs;
    uint64_t serial;
} SN0ModelRouter;

typedef struct SN0ModelModuleManagement {
    const unsigned int *adapter_nodes;
    const uint64_t *adapter_offsets;
    size_t adapter_count;
    uint8_t msc_address;
    uint8_t nvram_address;
    unsigned int nvram_address_count;
    unsigned int nvram_bytes;
} SN0ModelModuleManagement;

typedef struct SN0ModelModule {
    const char *id;
    char serial[SN0_MODEL_MODULE_SERIAL_LEN + 1];
    /* Selected installation identity; seed newly created MSC NVRAM only. */
    uint8_t initial_module_number;
    const SN0ModelModuleManagement *management;
    const unsigned int *nodes;
    size_t node_count;
} SN0ModelModule;

typedef struct SN0ModelCrossbow {
    const char *id;
    unsigned int module_index;
    uint16_t widget_part;
    uint16_t widget_manufacturer;
    uint8_t widget_revision;
    uint16_t initially_operational_links;
} SN0ModelCrossbow;

typedef struct SN0ModelXtalkAttachment {
    unsigned int crossbow_index;
    unsigned int widget;
    unsigned int node_index;
} SN0ModelXtalkAttachment;

typedef struct SN0ModelIoBoard {
    const char *id;
    unsigned int module_index;
    unsigned int slot;
    unsigned int crossbow_index;
    unsigned int widget;
    const SN0ModelComponent *bridge;
    const SN0ModelComponent *pci;
    size_t pci_count;
    SN0ModelIoc3Population ioc3_population;
} SN0ModelIoBoard;

typedef struct SN0ModelComposition {
    const char *name;
    const SN0ModelHardware *hardware;
    const SN0ModelPhysicalNode *nodes;
    size_t node_count;
    const SN0ModelModule *modules;
    size_t module_count;
    const SN0ModelRouter *routers;
    size_t router_count;
    const SN0ModelRoutedFabricLink *links;
    size_t link_count;
    const SN0ModelCrossbow *crossbows;
    size_t crossbow_count;
    const SN0ModelXtalkAttachment *xtalk_attachments;
    size_t xtalk_attachment_count;
    const SN0ModelIoBoard *io_boards;
    size_t io_board_count;
} SN0ModelComposition;

extern const SN0ModelComposition sn0_origin2000_compositions[];
extern const size_t sn0_origin2000_composition_count;
extern const char sn0_origin2000_default_composition[];

const SN0ModelComposition *sn0_origin2000_find_composition(const char *name);

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

#endif /* HW_MIPS_SN0_MODEL_H */
