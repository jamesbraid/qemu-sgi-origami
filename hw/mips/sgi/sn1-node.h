/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 300 (IP35/Bedrock, "SN1") node device: the CPUs, Bedrock,
 * L1 and its UART, boot flash and PROM views one Origin 300 chassis
 * carries. The family builder (sn-machine.c) creates it from the
 * format-2 descriptor's Bedrock node record and its populated I/O
 * board, then realizes it; sn1.c holds only the product class (the
 * node-strided multi-node case is future work, tracked by the plan's
 * board-devices and one-fabric steps: today's Origin 300 is one
 * chassis, one node).
 */

#ifndef HW_MIPS_SGI_SN1_NODE_H
#define HW_MIPS_SGI_SN1_NODE_H

#include "hw/core/clock.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi/sn-model.h"
#include "cpu.h"
#include "sn-promdata.h"

/*
 * Bedrock is fixed at four CPUs on two processor interfaces; see the
 * component taxonomy and the Bedrock processor-interface datasheet.
 */
#define SGI_SN1_NODE_MAX_CPUS 4

/*
 * A cacheable ROM device mirroring the boot flash's current storage. Stores
 * still reach its write callback and are refused; its RAM block migrates.
 */
typedef struct SN1PromView {
    MemoryRegion rom;
    MemoryRegion *backing;
    const char *name;
} SN1PromView;

typedef struct SN1NodeState {
    SysBusDevice parent_obj;

    /* Properties, set by the machine before realize. */
    MemoryRegion *ram;
    uint32_t nr_cpus;
    bool promdata_filter;
    bool promdata_fast;

    /*
     * The topology's Bedrock node record and its populated I/O board
     * (NULL if none), set by the machine before realize -- the format-2
     * descriptor this node builds from (sn-model.h).
     */
    const SGIModelNode *model_node;
    const SGIModelIoBoard *io_board;
    unsigned int pci_widget;
    /*
     * Every widget the crossbar decodes for this node (widget zero, the
     * host's own local view, plus one per xtalk attachment), set by the
     * machine before realize.
     */
    unsigned int *xbridge_widgets;
    size_t xbridge_widget_count;

    Clock *cpuclk;
    MIPSCPU *cpu[SGI_SN1_NODE_MAX_CPUS];
    MemoryRegion cpu_sysmem[SGI_SN1_NODE_MAX_CPUS];
    MemoryRegion sysmem_alias[SGI_SN1_NODE_MAX_CPUS];
    /*
     * The uncached view: the cached one, less the private window. Only
     * wired when promdata_fast is set; see sn-promdata.c.
     */
    MemoryRegion cpu_uncached[SGI_SN1_NODE_MAX_CPUS];
    MemoryRegion cpu_uncached_alias[SGI_SN1_NODE_MAX_CPUS];
    SNPromDataState promdata[SGI_SN1_NODE_MAX_CPUS];
    /* Samples the mapped processors' stack pointers for the exit. */
    SNPromDataPoll promdata_poll;

    DeviceState *l1;
    DeviceState *bedrock;
    DeviceState *xbridge;
    DeviceState *boot_flash;
    MemoryRegion bedrock_node;
    MemoryRegion hub_local_register_block;
    MemoryRegion cpu_c_local_pi_xtalk;
    MemoryRegion cpu_c_local_pi_hub;
    MemoryRegion hub_remote_register_head;
    MemoryRegion hub_remote_register_tail;

    /* The node's own address-space views; see sn1-node.c. */
    MemoryRegion hub_memory;
    MemoryRegion hub_local_registers;
    MemoryRegion board_id;
    MemoryRegion board_id_remote;
    MemoryRegion prom;
    MemoryRegion *prom_views;
    MemoryRegion prom_flash;
    MemoryRegion prom_flash_remote;
    /* Used in place of the aliases above once a flash part is attached. */
    SN1PromView prom_view_reset;
    SN1PromView prom_view_local_high;
} SN1NodeState;

#define TYPE_SGI_SN1_NODE "sgi-sn1-node"
DECLARE_INSTANCE_CHECKER(SN1NodeState, SN1_NODE, TYPE_SGI_SN1_NODE)

/*
 * The Bedrock node's own machine-reset sequence: reset every CPU and
 * apply the reset-latched processor mode bits, reset every other
 * device, refresh the PROM views from whatever the flash part holds
 * now (once one is attached) and release the CPUs -- they start
 * powered off, so the generic device-reset path alone leaves them
 * halted. Self-contained (not sharing sgi_machine_reset's Hub-ordered
 * prefix) because "start-powered-off" needs cpu_reset to run before
 * qemu_devices_reset, the reverse of the Hub's own order, which
 * releases through sgi_hub_set_cpu_enable instead and has no such
 * constraint.
 */
void sn1_node_full_reset(SN1NodeState *node, ResetType type);

#endif /* HW_MIPS_SGI_SN1_NODE_H */
