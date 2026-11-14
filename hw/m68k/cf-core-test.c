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
/* An interrupt line: write the level and vector, then assert or clear it. */
#define SB_LINE_LEVEL       0x40
#define SB_LINE_VECTOR      0x44
#define SB_LINE_ASSERT      0x48
#define SB_ACCEPT_COUNT     0x4c
/* Raise a request by the plain level and vector call: (level << 8) | vector */
#define SB_LEGACY_RAISE     0x50
/* Write 0 to run without the interrupt provider, 1 to have it. */
#define SB_PROVIDER         0x54
/*
 * Reads 1 once the core has stopped in HALT or STOP, 0 while it runs. The
 * flag is set by the instruction itself, with no interrupt check between
 * the store and the stop, so a test that has seen 1 knows the core cannot
 * take an interrupt first.
 */
#define SB_CPU_HALTED       0x58
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
    bool line_asserted;
    uint32_t line_level;
    uint32_t line_vector;
    uint32_t accept_count;
} CfCoreTest;

static const M68kBoardOps cf_core_test_board_ops;
static const M68kBoardOps cf_core_test_board_ops_no_irq;

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
    case SB_ACCEPT_COUNT:
        return t->accept_count;
    case SB_CPU_HALTED:
        return qatomic_read(&CPU(t->cpu)->halted);
    default:
        return 0;
    }
}

static void scoreboard_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    CfCoreTest *t = opaque;

    switch (addr) {
    case SB_LINE_LEVEL:
        t->line_level = val;
        break;
    case SB_LINE_VECTOR:
        t->line_vector = val;
        break;
    case SB_LINE_ASSERT:
        t->line_asserted = val;
        /*
         * Tell the core there is a request, with a level and vector that
         * are deliberately wrong: with a provider the core must take
         * what the line holds when it accepts, not what it was told here.
         */
        m68k_set_irq_level(t->cpu, val ? 7 : 0, 0xff);
        break;
    case SB_LEGACY_RAISE:
        m68k_set_irq_level(t->cpu, (val >> 8) & 7, val & 0xff);
        break;
    case SB_PROVIDER:
        m68k_cpu_set_board_ops(t->cpu, val ? &cf_core_test_board_ops
                                           : &cf_core_test_board_ops_no_irq, t);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only 0x%"
                      HWADDR_PRIx "\n", __func__, addr);
    }
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

static bool cf_core_test_irq_select(void *opaque, int *level, int *vector)
{
    CfCoreTest *t = opaque;

    g_assert(bql_locked());
    if (!t->line_asserted || !t->line_level) {
        return false;
    }
    *level = t->line_level;
    *vector = t->line_vector;
    return true;
}

static void cf_core_test_irq_accept(void *opaque)
{
    CfCoreTest *t = opaque;

    g_assert(bql_locked());
    t->line_asserted = false;
    t->accept_count++;
    m68k_set_irq_level(t->cpu, 0, 0);
}

static const M68kBoardOps cf_core_test_board_ops = {
    .control_write = cf_core_test_control_write,
    .irq_select = cf_core_test_irq_select,
    .irq_accept = cf_core_test_irq_accept,
};

static const M68kBoardOps cf_core_test_board_ops_no_irq = {
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
