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

typedef enum SN0ModelModuleRole {
    SN0_MODULE_MASTER,
    SN0_MODULE_SLAVE,
} SN0ModelModuleRole;

typedef enum SN0KlRecordKind {
    SN0_KL_RECORD_COMPONENT,
    SN0_KL_RECORD_PROCESSOR,
    SN0_KL_RECORD_HUB,
    SN0_KL_RECORD_MEMORY_BANK,
} SN0KlRecordKind;

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

typedef struct SN0KlComponentModel {
    SN0KlRecordKind record_kind;
    const SN0ModelComponent *component;
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
    const SN0ModelComponent *hub;
    const SN0ModelComponent *base_io;
    const SN0ModelComponent *pci;
    size_t pci_count;
    SN0ModelAddressMap map;
    SN0ModelIoc3Population ioc3_population;
    SN0KlConfigModel klconfig;
} SN0ModelNodeTopology;

typedef struct SN0ModelNodeInstance {
    uint8_t nasid;
    uint8_t cpu_first_id;
    uint8_t cpu_count;
    SN0ModelModuleRole module_role;
    uint64_t hub_serial;
    uint64_t base_io_serial;
    uint64_t ioc3_serial;
    const SN0ModelNodeTopology *topology;
} SN0ModelNodeInstance;

typedef struct SN0ModelFabricLink {
    const char *name;
    unsigned int endpoint[2];
} SN0ModelFabricLink;

typedef struct SN0ModelProfile {
    const SN0ModelNodeInstance *nodes;
    size_t node_count;
    const SN0ModelFabricLink *links;
    size_t link_count;
} SN0ModelProfile;

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
    SN0ModelMemory memory;
    const SN0ModelProfile *profiles;
    size_t profile_count;
    unsigned int junk_uart_node;
    bool junk_uart_default_enabled;
} SN0MachineModel;

extern const SN0MachineModel sn0_origin200_model;

const SN0ModelProfile *sn0_model_find_profile(const SN0MachineModel *model,
                                              unsigned int nodes,
                                              const unsigned int *cpus);
unsigned int sn0_model_max_nodes(const SN0MachineModel *model);
unsigned int sn0_model_max_cpus(const SN0MachineModel *model);
uint64_t sn0_model_node_address(const SN0ModelAddressMap *map,
                                const SN0ModelAddressSpace *space,
                                unsigned int nasid, uint64_t offset);
uint64_t sn0_model_region_address(const SN0ModelAddressMap *map,
                                  const SN0ModelAddressSpace *space,
                                  unsigned int nasid,
                                  const SN0ModelRegion *region,
                                  uint64_t offset);
bool sn0_model_memory_layout(const SN0MachineModel *model, uint64_t ram_size,
                             uint16_t *bank_mb,
                             unsigned int *populated_banks);

#endif /* HW_MIPS_SN0_MODEL_H */
