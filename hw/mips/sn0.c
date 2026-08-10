/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) family machines.
 *
 * Skeleton: one node, CPU and node-0 memory only. The SN0 physical
 * address space is NASID-windowed; until the Hub model exists, node 0
 * memory is mapped at its node-local base only.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/mips/mips.h"
#include "system/address-spaces.h"
#include "cpu.h"

/*
 * A stock Origin 200 runs its R10000 at 180 MHz and its hub at 90 MHz. The
 * processor's own counter runs at half the processor clock, which is what the
 * CPU model's counter divisor expresses, so this is the input that counter is
 * derived from.
 */
#define SN0_CPU_REFCLK_HZ (180 * 1000 * 1000)

static void sn0_init(MachineState *machine)
{
    Clock *cpuclk;
    MIPSCPU *cpu;

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, SN0_CPU_REFCLK_HZ);
    cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk, true);
    cpu_mips_irq_init_cpu(cpu);
    cpu_mips_clock_init(cpu);

    memory_region_add_subregion(get_system_memory(), 0, machine->ram);
}

static void origin200_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI Origin 200 (IP27, skeleton)";
    mc->init = sn0_init;
    mc->max_cpus = 1;
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "sn0.node0.ram";
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R10000");
}

static const TypeInfo origin200_type = {
    .name = MACHINE_TYPE_NAME("origin200"),
    .parent = TYPE_MACHINE,
    .class_init = origin200_class_init,
};

static void sn0_machine_types(void)
{
    type_register_static(&origin200_type);
}

type_init(sn0_machine_types)
