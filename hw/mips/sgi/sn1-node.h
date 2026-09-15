/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 300 (IP35/Bedrock, "SN1") node device: the CPUs, Bedrock,
 * L1 and its UART, and boot flash one Origin 300 chassis carries. sn1.c
 * instantiates it and still owns the XBridge's PCI population and the
 * machine's own address decode outside the node (the node-strided
 * multi-node case is future work, tracked by the plan's board-devices and
 * one-fabric steps; today's Origin 300 is one chassis, one node).
 */

#ifndef HW_MIPS_SGI_SN1_NODE_H
#define HW_MIPS_SGI_SN1_NODE_H

#include "hw/core/clock.h"
#include "hw/core/sysbus.h"
#include "cpu.h"
#include "sn-promdata.h"

/*
 * Bedrock is fixed at four CPUs on two processor interfaces; see the
 * component taxonomy and the Bedrock processor-interface datasheet.
 */
#define SGI_SN1_NODE_MAX_CPUS 4

typedef struct SN1NodeState {
    SysBusDevice parent_obj;

    /* Properties, set by the machine before realize. */
    MemoryRegion *ram;
    bool promdata_filter;
    bool promdata_fast;

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
} SN1NodeState;

#define TYPE_SGI_SN1_NODE "sgi-sn1-node"
DECLARE_INSTANCE_CHECKER(SN1NodeState, SN1_NODE, TYPE_SGI_SN1_NODE)

/*
 * Creates the node's boot-flash part from -drive if=pflash and returns the
 * processor's read view of its first selection, or NULL with no error when
 * no such drive was given (the caller then falls back to an immutable
 * -bios image). Callable before the node itself is realized, so sn1.c's
 * PROM loading -- which decides the flash-or-bios question for the whole
 * machine, not a node-local one -- can still run in its existing place
 * ahead of the node's own realize.
 */
MemoryRegion *sn1_node_create_boot_flash(SN1NodeState *node,
                                         MachineState *machine,
                                         Error **errp);

#endif /* HW_MIPS_SGI_SN1_NODE_H */
