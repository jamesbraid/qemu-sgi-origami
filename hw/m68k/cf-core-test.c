/*
 * ColdFire core test machine.
 *
 * A bare machine that exists only so qtests can run test-owned ColdFire
 * code on a CPU model and observe what the core does. It has RAM at
 * address zero and nothing else; it is not a model of any real board.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "target/m68k/cpu.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "hw/core/boards.h"

/* A ColdFire reset fetches the initial SP and PC from the vector table. */
static void cf_core_test_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static void cf_core_test_init(MachineState *machine)
{
    M68kCPU *cpu = M68K_CPU(cpu_create(machine->cpu_type));

    memory_region_add_subregion(get_system_memory(), 0, machine->ram);
    qemu_register_reset(cf_core_test_reset, cpu);
}

static void cf_core_test_machine_init(MachineClass *mc)
{
    mc->desc = "ColdFire core test machine (qtest only)";
    mc->init = cf_core_test_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m5206");
    mc->default_ram_id = "cf-core-test.ram";
    mc->default_ram_size = 16 * MiB;
}

DEFINE_MACHINE("cf-core-test", cf_core_test_machine_init)
