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

typedef enum SN0ModelInterconnectKind {
    SN0_MODEL_INTERCONNECT_NONE,
    SN0_MODEL_INTERCONNECT_DIRECT_PEER,
} SN0ModelInterconnectKind;

typedef enum SN0ModelPciKind {
    SN0_MODEL_PCI_IOC3,
    SN0_MODEL_PCI_ISP1040,
} SN0ModelPciKind;

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
    uint16_t default_memory_mib;
} SN0ModelNodeInstance;

typedef struct SN0ModelProfile {
    const SN0ModelNodeInstance *nodes;
    size_t node_count;
    SN0ModelInterconnectKind interconnect;
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

typedef struct SN0ModelHub {
    uint8_t kl_type;
    uint8_t ni_revision;
    uint64_t serial_base;
    uint8_t serial_stride;
} SN0ModelHub;

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
} SN0ModelBaseIO;

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
    const SN0KlBoardModel *boards;
    size_t board_count;
} SN0KlConfigModel;

typedef struct SN0MachineModel {
    const char *description;
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
    SN0ModelHub hub;
    SN0ModelBaseIO base_io;
    SN0KlConfigModel klconfig;
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
unsigned int sn0_model_max_nodes(const SN0MachineModel *model);
unsigned int sn0_model_max_cpus(const SN0MachineModel *model);
uint64_t sn0_model_node_offset(const SN0MachineModel *model,
                               unsigned int nasid);
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
