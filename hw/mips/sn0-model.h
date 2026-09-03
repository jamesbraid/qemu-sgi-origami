/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private interface between the generated SN0 product data and QEMU. */

#ifndef HW_MIPS_SN0_MODEL_H
#define HW_MIPS_SN0_MODEL_H

typedef enum SN0ModelAddressSpaceKind {
    SN0_MODEL_SPACE_MEMORY,
    SN0_MODEL_SPACE_HUB_SPECIAL,
    SN0_MODEL_SPACE_XTALK,
    SN0_MODEL_SPACE_MEMORY_SPECIAL,
} SN0ModelAddressSpaceKind;

typedef enum SN0ModelRegionRole {
    SN0_MODEL_REGION_NODE_MEMORY,
    SN0_MODEL_REGION_HUB_REGISTERS_LOCAL,
    SN0_MODEL_REGION_HUB_REGISTERS_NODE,
    SN0_MODEL_REGION_HUB_LOCAL_MEMORY,
    SN0_MODEL_REGION_HUB_REMOTE_MEMORY,
    SN0_MODEL_REGION_BACKDOOR_ECC,
    SN0_MODEL_REGION_BACKDOOR_DIRECTORY,
    SN0_MODEL_REGION_BRIDGE_REGISTERS,
    SN0_MODEL_REGION_PFLASH_RESET,
    SN0_MODEL_REGION_PFLASH_LBOOT,
    SN0_MODEL_REGION_PFLASH_RBOOT,
} SN0ModelRegionRole;

typedef enum SN0ModelCpuKind {
    SN0_MODEL_CPU_R10000,
} SN0ModelCpuKind;

typedef enum SN0ModelPciKind {
    SN0_MODEL_PCI_IOC3,
    SN0_MODEL_PCI_ISP1040,
} SN0ModelPciKind;

typedef enum SN0ModelModuleRole {
    SN0_MODEL_MODULE_MASTER,
    SN0_MODEL_MODULE_SLAVE,
} SN0ModelModuleRole;

typedef enum SN0ModelTimekeeperKind {
    SN0_MODEL_TIMEKEEPER_NONE,
    SN0_MODEL_TIMEKEEPER_M48T35,
} SN0ModelTimekeeperKind;

typedef enum SN0ModelOnewireKind {
    SN0_MODEL_ONEWIRE_NONE,
    SN0_MODEL_ONEWIRE_BLANK_DS2505,
} SN0ModelOnewireKind;

typedef enum SN0ModelFlashKind {
    SN0_MODEL_FLASH_AM29F080,
} SN0ModelFlashKind;

typedef enum SN0ModelEndianness {
    SN0_MODEL_ENDIAN_BIG,
    SN0_MODEL_ENDIAN_LITTLE,
} SN0ModelEndianness;

typedef enum SN0KlRecordKind {
    SN0_KL_RECORD_COMPONENT,
    SN0_KL_RECORD_PROCESSOR,
    SN0_KL_RECORD_HUB,
    SN0_KL_RECORD_MEMORY_BANK,
} SN0KlRecordKind;

typedef enum SN0KlComponentSource {
    SN0_KL_COMPONENT_SOURCE_LITERAL,
    SN0_KL_COMPONENT_SOURCE_HUB,
    SN0_KL_COMPONENT_SOURCE_BASE_IO,
    SN0_KL_COMPONENT_SOURCE_PCI_FUNCTION,
} SN0KlComponentSource;

typedef struct SN0ModelNodeInstance {
    uint8_t nasid;
    uint8_t cpu_first_id;
    uint8_t cpu_count;
    int peer_index;
    size_t topology_index;
} SN0ModelNodeInstance;

typedef struct SN0ModelProfile {
    const SN0ModelNodeInstance *nodes;
    size_t node_count;
} SN0ModelProfile;

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

typedef struct SN0ModelAddressSpace {
    SN0ModelAddressSpaceKind kind;
    unsigned int selector;
    uint64_t carrier_base;
    uint64_t unmapped_base;
} SN0ModelAddressSpace;

typedef struct SN0ModelRegion {
    SN0ModelRegionRole role;
    SN0ModelAddressSpaceKind space;
    uint64_t offset;
    uint64_t size;
    int alias_of;
} SN0ModelRegion;

typedef struct SN0ModelCpuIrqLines {
    uint8_t bank0;
    uint8_t bank1;
    uint8_t rt;
} SN0ModelCpuIrqLines;

typedef struct SN0ModelHub {
    uint8_t kl_type;
    uint8_t ni_revision;
    uint64_t serial_base;
    uint8_t serial_stride;
    SN0ModelCpuIrqLines cpu_irq_lines;
} SN0ModelHub;

typedef struct SN0ModelPromPiProbe {
    uint8_t widget;
    uint64_t offset;
    uint64_t window_offset;
} SN0ModelPromPiProbe;

typedef struct SN0ModelPciFunction {
    SN0ModelPciKind kind;
    uint8_t device;
    uint8_t function;
    uint16_t subsystem_id;
    uint32_t bar[2];
    uint64_t bridge_window_base;
    uint64_t serial_base;
    uint8_t serial_stride;
    int8_t irq_input;
    uint8_t kl_type;
    uint8_t pci_revision;
} SN0ModelPciFunction;

typedef struct SN0ModelBaseIO {
    uint8_t kl_type;
    uint8_t widget;
    uint8_t discovery_widget;
    uint8_t default_revision;
    uint16_t decoded_widget_mask;
    uint64_t serial_base;
    uint8_t serial_stride;
    const SN0ModelPciFunction *functions;
    size_t function_count;
    SN0ModelPromPiProbe prom_pi_probe;
} SN0ModelBaseIO;

typedef struct SN0ModelIoc3Population {
    bool ethernet;
    bool serial_a;
    bool serial_b;
    bool keyboard_mouse;
    bool parallel;
    bool onewire_identity;
    SN0ModelTimekeeperKind timekeeper;
} SN0ModelIoc3Population;

typedef struct SN0ModelHubPopulation {
    bool i2c_controller;
    bool system_controller;
    uint8_t i2c_nvram_pages;
    SN0ModelOnewireKind onewire_identity;
} SN0ModelHubPopulation;

typedef struct SN0ModelFlash {
    SN0ModelFlashKind kind;
    uint32_t sector_size;
    uint8_t width;
    uint8_t mappings;
    SN0ModelEndianness endianness;
    uint16_t manufacturer_id;
    uint16_t device_id;
    uint16_t unlock_address[2];
} SN0ModelFlash;

typedef struct SN0KlComponentModel {
    SN0KlRecordKind record_kind;
    SN0KlComponentSource source;
    SN0ModelPciKind pci_kind;
    uint8_t source_ordinal;
    uint8_t type;
    uint8_t revision;
    uint8_t physical_id;
    uint8_t widget;
    uint16_t identification;
    uint16_t fp_identification;
    uint32_t clock_mhz;
    uint64_t clock_hz;
    uint16_t cache_mib;
    uint16_t cache_clock_mhz;
    uint64_t port;
    uint16_t bank_mapping;
} SN0KlComponentModel;

typedef struct SN0KlBoardModel {
    uint8_t type;
    uint8_t record_kind;
    uint8_t version;
    uint8_t slot;
    int declared_count;
    const SN0KlComponentModel *components;
    size_t component_count;
} SN0KlBoardModel;

typedef struct SN0KlConfigModel {
    uint64_t base;
    uint64_t size;
    uint64_t signature;
    uint32_t version;
    uint32_t arena_offset;
    uint32_t board_base;
    uint32_t board_stride;
    uint32_t component_base;
    uint32_t component_stride;
    uint32_t console_config_window;
    uint32_t console_port_offset;
    uint16_t console_baud;
    uint32_t console_type;
    const SN0KlBoardModel *boards;
    size_t board_count;
} SN0KlConfigModel;

typedef struct SN0ModelNodeTopology {
    SN0ModelModuleRole module_role;
    SN0ModelHub hub;
    SN0ModelBaseIO base_io;
    SN0ModelIoc3Population ioc3_population;
    SN0ModelHubPopulation hub_population;
    const char *bridge_part;
    SN0ModelFlash flash;
    SN0KlConfigModel klconfig;
} SN0ModelNodeTopology;

typedef struct SN0MachineModel {
    const char *description;
    SN0ModelCpuKind cpu_kind;
    uint64_t default_ram_mib;
    unsigned int cpu_slots_per_node;
    uint64_t cpu_clock_hz;
    uint32_t cpu_mode_bits;
    uint64_t hub_clock_hz;
    uint64_t rtc_clock_hz;
    uint32_t firmware_machine_type;
    uint32_t flash_read_cycles;
    uint32_t flash_write_cycles;
    uint64_t node_stride;
    SN0ModelMemory memory;
    const SN0ModelNodeTopology *node_topologies;
    size_t node_topology_count;
    unsigned int junk_uart_node;
    const SN0ModelProfile *profiles;
    size_t profile_count;
    const SN0ModelAddressSpace *address_spaces;
    size_t address_space_count;
    const SN0ModelRegion *regions;
    size_t region_count;
} SN0MachineModel;

extern const SN0MachineModel sn0_origin200_model;

const SN0ModelProfile *sn0_model_find_profile(const SN0MachineModel *model,
                                              unsigned int nodes,
                                              const unsigned int *cpus);
const SN0ModelPciFunction *sn0_model_find_pci_function(
    const SN0ModelBaseIO *base_io, SN0ModelPciKind kind,
    unsigned int instance);
const SN0ModelNodeTopology *sn0_model_node_topology(
    const SN0MachineModel *model, const SN0ModelNodeInstance *node);
unsigned int sn0_model_max_nodes(const SN0MachineModel *model);
unsigned int sn0_model_max_cpus(const SN0MachineModel *model);
uint64_t sn0_model_node_address(const SN0MachineModel *model,
                                const SN0ModelAddressSpace *space,
                                unsigned int nasid, uint64_t offset);
uint64_t sn0_model_region_address(const SN0MachineModel *model,
                                  const SN0ModelAddressSpace *space,
                                  unsigned int nasid,
                                  const SN0ModelRegion *region,
                                  uint64_t offset);
const SN0ModelAddressSpace *sn0_model_find_space(
    const SN0MachineModel *model, SN0ModelAddressSpaceKind kind);
const SN0ModelAddressSpace *sn0_model_find_space_by_selector(
    const SN0MachineModel *model, unsigned int selector);
const SN0ModelRegion *sn0_model_find_region(const SN0MachineModel *model,
                                            SN0ModelRegionRole role);
const SN0ModelRegion *sn0_model_region_alias(const SN0MachineModel *model,
                                             const SN0ModelRegion *region);
bool sn0_model_memory_layout(const SN0MachineModel *model, uint64_t ram_size,
                             uint16_t *bank_mb,
                             unsigned int *populated_banks);

static inline const SN0ModelNodeInstance *
sn0_model_profile_node(const SN0ModelProfile *profile, size_t node_index)
{
    g_assert(profile->nodes);
    g_assert(node_index < profile->node_count);
    return &profile->nodes[node_index];
}

static inline unsigned int
sn0_model_node_cpu_id(const SN0ModelNodeInstance *node, unsigned int slice)
{
    g_assert(slice < node->cpu_count);
    g_assert(node->cpu_first_id <= UINT8_MAX - slice);
    return node->cpu_first_id + slice;
}

#endif /* HW_MIPS_SN0_MODEL_H */
