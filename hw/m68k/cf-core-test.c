/*
 * ColdFire core test machine.
 *
 * A bare machine that exists only so qtests can run test-owned ColdFire
 * code on a CPU model and observe what the core does. It has RAM at
 * address zero and a small scoreboard that reports CPU state to the test;
 * it is not a model of any real board.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "target/m68k/cpu.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "hw/core/boards.h"

#define SCOREBOARD_BASE     0x40000000
#define SCOREBOARD_SIZE     0x100

/* Scoreboard registers: CPU control register state as the core holds it. */
#define SB_CACR             0x00
#define SB_VBR              0x04
#define SB_RAMBAR           0x08
#define SB_MBAR             0x0c
/* What the board was told about the control register writes. */
#define SB_WRITE_COUNT      0x10
#define SB_LAST_REG         0x14
#define SB_LAST_VAL         0x18

typedef struct CfCoreTest {
    M68kCPU *cpu;
    MemoryRegion scoreboard;
    uint32_t write_count;
    uint32_t last_reg;
    uint32_t last_val;
} CfCoreTest;

static uint64_t scoreboard_read(void *opaque, hwaddr addr, unsigned size)
{
    CfCoreTest *t = opaque;
    CPUM68KState *env = &t->cpu->env;

    switch (addr) {
    case SB_CACR:
        return env->cacr;
    case SB_VBR:
        return env->vbr;
    case SB_RAMBAR:
        return env->rambar0;
    case SB_MBAR:
        return env->mbar;
    case SB_WRITE_COUNT:
        return t->write_count;
    case SB_LAST_REG:
        return t->last_reg;
    case SB_LAST_VAL:
        return t->last_val;
    default:
        return 0;
    }
}

static void scoreboard_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only scoreboard\n",
                  __func__);
}

static const MemoryRegionOps scoreboard_ops = {
    .read = scoreboard_read,
    .write = scoreboard_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void cf_core_test_control_write(void *opaque, uint32_t reg,
                                       uint32_t val)
{
    CfCoreTest *t = opaque;

    /* The documented contract: a board callback is entered with the BQL. */
    g_assert(bql_locked());
    t->write_count++;
    t->last_reg = reg;
    t->last_val = val;
}

static const M68kBoardOps cf_core_test_board_ops = {
    .control_write = cf_core_test_control_write,
};

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
    CfCoreTest *t = g_new0(CfCoreTest, 1);
    MemoryRegion *sysmem = get_system_memory();

    t->cpu = M68K_CPU(cpu_create(machine->cpu_type));

    memory_region_add_subregion(sysmem, 0, machine->ram);
    memory_region_init_io(&t->scoreboard, NULL, &scoreboard_ops, t,
                          "cf-core-test.scoreboard", SCOREBOARD_SIZE);
    memory_region_add_subregion(sysmem, SCOREBOARD_BASE, &t->scoreboard);

    m68k_cpu_set_board_ops(t->cpu, &cf_core_test_board_ops, t);
    qemu_register_reset(cf_core_test_reset, t->cpu);
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
