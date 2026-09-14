/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private interface between generated Origin 300 topology and QEMU. */

#ifndef HW_MIPS_SN1_MODEL_H
#define HW_MIPS_SN1_MODEL_H

typedef enum SN1ModelCpuKind {
    SN1_CPU_R14000,
} SN1ModelCpuKind;

typedef enum SN1ModelPciKind {
    SN1_PCI_ISP12160,
    SN1_PCI_IOC3,
    SN1_PCI_LUCENT_OHCI,
} SN1ModelPciKind;

typedef struct SN1ModelRegion {
    uint64_t address;
    uint64_t size;
} SN1ModelRegion;

typedef struct SN1ModelPromView {
    const char *name;
    uint64_t address;
    uint64_t offset;
    uint64_t size;
} SN1ModelPromView;

typedef struct SN1ModelPciDevice {
    SN1ModelPciKind kind;
    const char *name;
    uint8_t widget;
    uint8_t device;
    uint8_t function;
    uint8_t interrupt_pin;
    uint8_t superio_pin;
    uint8_t revision;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint32_t population;
    uint8_t timekeeper_kind;
} SN1ModelPciDevice;

typedef struct SN1MachineModel {
    const char *description;
    SN1ModelCpuKind cpu_kind;
    unsigned int cpu_count;
    uint64_t cpu_clock_hz;
    uint64_t ram_size;
    uint64_t ram_address;
    SN1ModelRegion hub_memory;
    SN1ModelRegion hub_local_registers;
    uint64_t xtalk_address;
    uint64_t bedrock_local_address;
    SN1ModelRegion bedrock_node;
    uint64_t hub_remote_registers_address;
    uint8_t bedrock_revision;
    uint8_t bedrock_local_region;
    uint8_t bedrock_initiator_region;
    uint64_t prom_size;
    const SN1ModelPromView *prom_views;
    size_t num_prom_views;
    SN1ModelRegion prom_second_selection;
    uint8_t xbridge_host_port;
    const uint8_t *xbridge_widgets;
    size_t num_xbridge_widgets;
    const SN1ModelPciDevice *pci_devices;
    size_t num_pci_devices;
} SN1MachineModel;

extern const SN1MachineModel sn1_origin300_model;

#endif
