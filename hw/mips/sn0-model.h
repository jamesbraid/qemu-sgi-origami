/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private interface between the generated SN0 product data and QEMU. */

#ifndef HW_MIPS_SN0_MODEL_H
#define HW_MIPS_SN0_MODEL_H

typedef enum SN0ModelDeviceKind {
    SN0_MODEL_DEVICE_NONE,
    SN0_MODEL_DEVICE_CPU_R10000,
    SN0_MODEL_DEVICE_MEMORY,
    SN0_MODEL_DEVICE_HUB,
    SN0_MODEL_DEVICE_PFLASH,
    SN0_MODEL_DEVICE_BRIDGE,
    SN0_MODEL_DEVICE_IOC3,
    SN0_MODEL_DEVICE_ISP1040,
} SN0ModelDeviceKind;

typedef enum SN0ModelBusKind {
    SN0_MODEL_BUS_NONE,
    SN0_MODEL_BUS_SYSAD,
    SN0_MODEL_BUS_DIRECTORY,
    SN0_MODEL_BUS_XTALK,
    SN0_MODEL_BUS_PCI,
} SN0ModelBusKind;

typedef enum SN0ModelAddressSpaceKind {
    SN0_MODEL_SPACE_MEMORY,
    SN0_MODEL_SPACE_HUB_SPECIAL,
    SN0_MODEL_SPACE_XTALK,
    SN0_MODEL_SPACE_MEMORY_SPECIAL,
} SN0ModelAddressSpaceKind;

typedef enum SN0ModelRegionRole {
    SN0_MODEL_REGION_NONE,
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

typedef enum SN0KlRecordKind {
    SN0_KL_RECORD_COMPONENT,
    SN0_KL_RECORD_PROCESSOR,
    SN0_KL_RECORD_HUB,
    SN0_KL_RECORD_MEMORY_BANK,
} SN0KlRecordKind;

typedef struct SN0ModelProfile {
    unsigned int nodes;
    const unsigned int *cpus_per_node;
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
    uint64_t node_stride;
} SN0ModelAddressSpace;

typedef struct SN0ModelRegion {
    SN0ModelRegionRole role;
    SN0ModelAddressSpaceKind space;
    uint64_t offset;
    uint64_t size;
    int alias_of;
} SN0ModelRegion;

typedef struct SN0ModelDevice {
    SN0ModelDeviceKind kind;
    SN0ModelBusKind bus;
    SN0ModelDeviceKind parent;
    int pci_device;
    int pci_function;
    uint16_t pci_subsystem_id;
    uint64_t pci_bar0;
    uint64_t pci_bar1;
    uint64_t bridge_window_base;
    int widget;
    unsigned int revision;
    uint64_t serial_base;
    unsigned int serial_stride;
    int irq_input;
} SN0ModelDevice;

typedef struct SN0ModelBridge {
    unsigned int discovery_widget;
    uint16_t decoded_widget_mask;
} SN0ModelBridge;

typedef struct SN0KlComponentModel {
    SN0KlRecordKind record_kind;
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
    uint32_t console_offset;
    uint32_t board_base;
    uint32_t board_stride;
    uint32_t component_base;
    uint32_t component_stride;
    uint8_t console_widget;
    uint8_t console_pci_device;
    uint32_t console_device_window;
    uint32_t console_config_window;
    uint32_t console_port_offset;
    uint16_t console_baud;
    const SN0KlBoardModel *boards;
    size_t board_count;
} SN0KlConfigModel;

typedef struct SN0MachineModel {
    const char *machine_name;
    const char *description;
    uint32_t default_ram_mib;
    unsigned int default_cpus;
    unsigned int max_nodes;
    unsigned int cpu_slots_per_node;
    uint64_t cpu_clock_hz;
    uint32_t cpu_mode_bits;
    uint64_t hub_clock_hz;
    uint64_t rtc_clock_hz;
    uint32_t firmware_machine_type;
    uint32_t flash_read_cycles;
    uint32_t flash_write_cycles;
    SN0ModelMemory memory;
    SN0ModelBridge bridge;
    SN0KlConfigModel klconfig;
    const SN0ModelProfile *profiles;
    size_t profile_count;
    const SN0ModelAddressSpace *address_spaces;
    size_t address_space_count;
    const SN0ModelRegion *regions;
    size_t region_count;
    const SN0ModelDevice *devices;
    size_t device_count;
} SN0MachineModel;

extern const SN0MachineModel sn0_origin200_model;

const SN0ModelProfile *sn0_model_find_profile(const SN0MachineModel *model,
                                              unsigned int nodes,
                                              const unsigned int *cpus);
const SN0ModelDevice *sn0_model_find_device(const SN0MachineModel *model,
                                            SN0ModelDeviceKind kind,
                                            unsigned int instance);
unsigned int sn0_model_device_count(const SN0MachineModel *model,
                                    SN0ModelDeviceKind kind);
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

#endif /* HW_MIPS_SN0_MODEL_H */
