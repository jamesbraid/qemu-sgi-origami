/*
 * ColdFire core tests.
 *
 * Each test loads a small test-owned program and its vector table into
 * the RAM of the cf-core-test machine, resets the machine so that the
 * CPU picks up the initial stack pointer and program counter from the
 * table, runs it and checks what the program left in RAM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define VECTOR_TABLE    0x00000
#define CODE_BASE       0x01000
#define RESULTS         0x02000
#define INITIAL_SP      0x40000
#define HANDLER         (CODE_BASE + 0x100)

#define SCOREBOARD      0x40000000
#define SB_CACR         (SCOREBOARD + 0x00)
#define SB_VBR          (SCOREBOARD + 0x04)
#define SB_RAMBAR       (SCOREBOARD + 0x08)
#define SB_MBAR         (SCOREBOARD + 0x0c)
#define SB_WRITE_COUNT  (SCOREBOARD + 0x10)
#define SB_LAST_REG     (SCOREBOARD + 0x14)
#define SB_LAST_VAL     (SCOREBOARD + 0x18)
#define SB_LINE_LEVEL   (SCOREBOARD + 0x40)
#define SB_LINE_VECTOR  (SCOREBOARD + 0x44)
#define SB_LINE_ASSERT  (SCOREBOARD + 0x48)
#define SB_ACCEPT_COUNT (SCOREBOARD + 0x4c)
#define SB_LEGACY_RAISE (SCOREBOARD + 0x50)
#define SB_PROVIDER     (SCOREBOARD + 0x54)
#define SB_CPU_HALTED   (SCOREBOARD + 0x58)

#define CR_CACR         0x002
#define CR_ACR0         0x004
#define CR_VBR          0x801
#define CR_RAMBAR       0xc04
#define CR_MBAR         0xc0f

#define VEC_ILLEGAL     4
#define VEC_TRAP1       33

#define RUN_TIMEOUT_US  (20 * G_USEC_PER_SEC)

/* Tiny assembler for the handful of ColdFire ISA A forms the tests need. */
typedef struct Asm {
    uint8_t buf[512];
    size_t len;
} Asm;

static void emit16(Asm *a, uint16_t v)
{
    a->buf[a->len++] = v >> 8;
    a->buf[a->len++] = v;
}

static void emit32(Asm *a, uint32_t v)
{
    emit16(a, v >> 16);
    emit16(a, v);
}

/* move.l #imm,Dn */
static void movei_d(Asm *a, int n, uint32_t imm)
{
    emit16(a, 0x203c | (n << 9));
    emit32(a, imm);
}

/* move.l Dn,(addr).l */
static void move_d_abs(Asm *a, int n, uint32_t addr)
{
    emit16(a, 0x23c0 | n);
    emit32(a, addr);
}

/* movec Dn,Rc */
static void movec_d(Asm *a, int n, int cr)
{
    emit16(a, 0x4e7b);
    emit16(a, (n << 12) | cr);
}

static void trap(Asm *a, int n)
{
    emit16(a, 0x4e40 | n);
}

/* move.l Dn,An-relative forms and the other odds and ends the handlers use */
static void move_sr_d(Asm *a, int n)
{
    emit16(a, 0x40c0 | n);
}

/* move.l (a7),Dn */
static void move_sp_d(Asm *a, int n)
{
    emit16(a, 0x2017 | (n << 9));
}

/* move.l a7,Dn */
static void move_a7_d(Asm *a, int n)
{
    emit16(a, 0x200f | (n << 9));
}

/* move.l (addr).l,Dn */
static void move_abs_d(Asm *a, uint32_t addr, int n)
{
    emit16(a, 0x2039 | (n << 9));
    emit32(a, addr);
}

/* move.w #imm,sr */
static void move_sr_imm(Asm *a, uint16_t sr)
{
    emit16(a, 0x46fc);
    emit16(a, sr);
}

static void rte(Asm *a)
{
    emit16(a, 0x4e73);
}

static void nop(Asm *a)
{
    emit16(a, 0x4e71);
}

static void bra_self(Asm *a)
{
    emit16(a, 0x60fe);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static QTestState *start_cpu(const char *cpu)
{
    return qtest_initf("-machine cf-core-test -cpu %s -accel tcg -S", cpu);
}

static QTestState *start(void)
{
    return start_cpu("m5206");
}

static void set_vector(QTestState *qts, uint32_t vbr, int vec, uint32_t addr)
{
    uint8_t be[4];

    put_be32(be, addr);
    qtest_memwrite(qts, vbr + vec * 4, be, sizeof(be));
}

/* Load program and vectors, reset the machine so the CPU picks them up. */
static void run(QTestState *qts, const Asm *code)
{
    uint8_t vec[8];

    put_be32(vec, INITIAL_SP);
    put_be32(vec + 4, CODE_BASE);
    qtest_memwrite(qts, VECTOR_TABLE, vec, sizeof(vec));
    qtest_memwrite(qts, CODE_BASE, code->buf, code->len);
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
}

/* Wait for the program to store a nonzero value at addr. */
static uint32_t wait_result(QTestState *qts, uint32_t addr)
{
    gint64 end = g_get_monotonic_time() + RUN_TIMEOUT_US;
    uint32_t v;

    while (!(v = qtest_readl(qts, addr))) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
    return v;
}

/*
 * Wait until the core has stopped in HALT or STOP. A fixed sleep after the
 * program reports in is not enough: if the vCPU thread is descheduled between
 * the report and the stop, an interrupt asked for early is taken before the
 * core halts, and a part that is meant to stay halted runs the handler.
 */
static void wait_halted(QTestState *qts)
{
    gint64 end = g_get_monotonic_time() + RUN_TIMEOUT_US;

    while (!qtest_readl(qts, SB_CPU_HALTED)) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
}

static void test_run_program(void)
{
    QTestState *qts = start();
    Asm a = { };

    movei_d(&a, 0, 0xc0ffee);
    move_d_abs(&a, 0, RESULTS);
    bra_self(&a);
    run(qts, &a);

    g_assert_cmphex(wait_result(qts, RESULTS), ==, 0xc0ffee);
    qtest_quit(qts);
}

/* Store a marker, then park: the common tail of a handler or a program. */
static void finish(Asm *a, uint32_t marker)
{
    movei_d(a, 0, marker);
    move_d_abs(a, 0, RESULTS);
    bra_self(a);
}

/* MOVEC writes to the control registers a ColdFire board model relies on. */
static void test_movec_control_registers(void)
{
    QTestState *qts = start();
    Asm a = { };

    movei_d(&a, 0, 0xffffffff);
    movec_d(&a, 0, CR_CACR);
    movec_d(&a, 0, CR_ACR0);
    movec_d(&a, 0, CR_RAMBAR);
    movec_d(&a, 0, CR_MBAR);
    movec_d(&a, 0, CR_VBR);
    finish(&a, 1);
    run(qts, &a);

    g_assert_cmphex(wait_result(qts, RESULTS), ==, 1);
    /* Only the defined bits of each register are held; the rest read zero. */
    g_assert_cmphex(qtest_readl(qts, SB_CACR), ==, 0x98000723);
    g_assert_cmphex(qtest_readl(qts, SB_RAMBAR), ==, 0xffffe13f);
    g_assert_cmphex(qtest_readl(qts, SB_MBAR), ==, 0xfffffc1f);
    /* The vector base is 1 MiB aligned: the low 20 bits are not held. */
    g_assert_cmphex(qtest_readl(qts, SB_VBR), ==, 0xfff00000);
    /* The board hears about every write, with the value the core latched. */
    g_assert_cmpuint(qtest_readl(qts, SB_WRITE_COUNT), ==, 5);
    g_assert_cmphex(qtest_readl(qts, SB_LAST_REG), ==, CR_VBR);
    g_assert_cmphex(qtest_readl(qts, SB_LAST_VAL), ==, 0xfff00000);
    qtest_quit(qts);
}

/*
 * The register masks belong to the ISA A only part. Later parts keep their
 * own layouts, so CACR and VBR are held as written there. RAMBAR and MBAR
 * are not implemented for them and are not exercised.
 */
static void test_movec_unmasked_on_later_parts(const void *cpu)
{
    QTestState *qts = start_cpu(cpu);
    Asm a = { };

    movei_d(&a, 0, 0xffffffff);
    movec_d(&a, 0, CR_CACR);
    movei_d(&a, 0, 0x00100123);
    movec_d(&a, 0, CR_VBR);
    finish(&a, 1);
    run(qts, &a);

    g_assert_cmphex(wait_result(qts, RESULTS), ==, 1);
    g_assert_cmphex(qtest_readl(qts, SB_CACR), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SB_VBR), ==, 0x00100123);
    qtest_quit(qts);
}

/* The vector table sits at the aligned base, whatever the low bits said. */
static void test_vbr_alignment(void)
{
    QTestState *qts = start();
    Asm a = { };
    Asm h = { };

    movei_d(&a, 0, 0x00100123);
    movec_d(&a, 0, CR_VBR);
    trap(&a, 1);
    bra_self(&a);
    finish(&h, 0x33);
    set_vector(qts, 0x100000, VEC_TRAP1, HANDLER);
    qtest_memwrite(qts, HANDLER, h.buf, h.len);
    run(qts, &a);

    g_assert_cmphex(wait_result(qts, RESULTS), ==, 0x33);
    qtest_quit(qts);
}

/*
 * The core registers are write-only through MOVEC on ColdFire: the
 * move from a control register is not an instruction here.
 */
static void test_movec_from_is_illegal(void)
{
    QTestState *qts = start();
    Asm a = { };
    Asm h = { };

    emit16(&a, 0x4e7a);
    emit16(&a, CR_VBR);
    bra_self(&a);
    set_vector(qts, 0, VEC_ILLEGAL, HANDLER);
    finish(&h, 0x44);
    qtest_memwrite(qts, HANDLER, h.buf, h.len);
    run(qts, &a);

    g_assert_cmphex(wait_result(qts, RESULTS), ==, 0x44);
    qtest_quit(qts);
}

/* Program a line: level, vector, then assert it. The SR mask is still 7. */
static void line_assert(Asm *a, int level, int vector)
{
    movei_d(a, 0, level);
    move_d_abs(a, 0, SB_LINE_LEVEL);
    movei_d(a, 0, vector);
    move_d_abs(a, 0, SB_LINE_VECTOR);
    movei_d(a, 0, 1);
    move_d_abs(a, 0, SB_LINE_ASSERT);
}

/*
 * A handler that records the SR it runs with and the first word of the
 * exception frame, then reports.
 */
static void frame_handler(Asm *h, uint32_t marker)
{
    move_sr_d(h, 1);
    move_sp_d(h, 2);
    move_d_abs(h, 1, RESULTS + 8);
    move_d_abs(h, 2, RESULTS + 4);
    finish(h, marker);
}

/*
 * The core takes the level and vector the controller holds when it
 * accepts the interrupt, not those it was told when the request was raised.
 */
static void test_irq_taken_late(void)
{
    QTestState *qts = start();
    Asm a = { };
    Asm h = { };

    line_assert(&a, 2, 70);
    /* The controller's choice changes while the interrupt is masked. */
    movei_d(&a, 0, 4);
    move_d_abs(&a, 0, SB_LINE_LEVEL);
    movei_d(&a, 0, 71);
    move_d_abs(&a, 0, SB_LINE_VECTOR);
    move_sr_imm(&a, 0x2000);
    bra_self(&a);
    frame_handler(&h, 0xb1);
    set_vector(qts, 0, 71, HANDLER);
    qtest_memwrite(qts, HANDLER, h.buf, h.len);
    run(qts, &a);

    g_assert_cmphex(wait_result(qts, RESULTS), ==, 0xb1);
    /* Level 4 became the mask, and the frame carries vector 71. */
    g_assert_cmphex(qtest_readl(qts, RESULTS + 8), ==, 0x2400);
    g_assert_cmphex(qtest_readl(qts, RESULTS + 4), ==, 0x411c2000);
    g_assert_cmpuint(qtest_readl(qts, SB_ACCEPT_COUNT), ==, 1);
    qtest_quit(qts);
}

/* A masked request waits, is taken once the mask drops, and RTE returns. */
static void test_irq_masked_then_taken_and_rte(void)
{
    QTestState *qts = start();
    Asm a = { };
    Asm h = { };

    move_sr_imm(&a, 0x2300);
    line_assert(&a, 3, 70);
    nop(&a);
    nop(&a);
    movei_d(&a, 0, 1);
    move_d_abs(&a, 0, RESULTS + 0x10);
    move_sr_imm(&a, 0x2200);
    /* Back from the handler: record the stack pointer, then report. */
    move_a7_d(&a, 3);
    move_d_abs(&a, 3, RESULTS + 0x18);
    finish(&a, 0x99);

    /* Handler: note whether the mask was honoured, then return. */
    move_abs_d(&h, RESULTS + 0x10, 4);
    move_d_abs(&h, 4, RESULTS + 0x14);
    rte(&h);
    set_vector(qts, 0, 70, HANDLER);
    qtest_memwrite(qts, HANDLER, h.buf, h.len);
    run(qts, &a);

    g_assert_cmphex(wait_result(qts, RESULTS), ==, 0x99);
    /* The marker was already set when the handler ran: not taken early. */
    g_assert_cmphex(qtest_readl(qts, RESULTS + 0x14), ==, 1);
    g_assert_cmphex(qtest_readl(qts, RESULTS + 0x18), ==, INITIAL_SP);
    g_assert_cmpuint(qtest_readl(qts, SB_ACCEPT_COUNT), ==, 1);
    qtest_quit(qts);
}

/* Without a provider the level and vector from the raise call are used. */
static void test_irq_without_provider(void)
{
    QTestState *qts = start();
    Asm a = { };
    Asm h = { };

    movei_d(&a, 0, 0);
    move_d_abs(&a, 0, SB_PROVIDER);
    movei_d(&a, 0, (5 << 8) | 72);
    move_d_abs(&a, 0, SB_LEGACY_RAISE);
    move_sr_imm(&a, 0x2000);
    bra_self(&a);
    frame_handler(&h, 0xc1);
    set_vector(qts, 0, 72, HANDLER);
    qtest_memwrite(qts, HANDLER, h.buf, h.len);
    run(qts, &a);

    g_assert_cmphex(wait_result(qts, RESULTS), ==, 0xc1);
    g_assert_cmphex(qtest_readl(qts, RESULTS + 8), ==, 0x2500);
    g_assert_cmphex(qtest_readl(qts, RESULTS + 4), ==, 0x41202000);
    g_assert_cmpuint(qtest_readl(qts, SB_ACCEPT_COUNT), ==, 0);
    qtest_quit(qts);
}

/* movea.l #imm,a7 */
static void movea_a7_imm(Asm *a, uint32_t imm)
{
    emit16(a, 0x2e7c);
    emit32(a, imm);
}

/* The exception frame for TRAP #0 with the stack pointer at sp. */
static void check_trap_frame(uint32_t sp, uint32_t format)
{
    QTestState *qts = start();
    Asm a = { };
    Asm h = { };
    uint32_t trap_pc;

    movea_a7_imm(&a, sp);
    trap_pc = CODE_BASE + a.len;
    trap(&a, 0);
    move_a7_d(&a, 3);
    move_d_abs(&a, 3, RESULTS + 0x18);
    finish(&a, 0x99);

    move_a7_d(&h, 5);
    move_d_abs(&h, 5, RESULTS + 0x10);
    move_sp_d(&h, 2);
    move_d_abs(&h, 2, RESULTS + 4);
    emit16(&h, 0x2c2f);     /* move.l 4(a7),d6 */
    emit16(&h, 4);
    move_d_abs(&h, 6, RESULTS + 8);
    rte(&h);
    set_vector(qts, 0, 32, HANDLER);
    qtest_memwrite(qts, HANDLER, h.buf, h.len);
    run(qts, &a);

    g_assert_cmphex(wait_result(qts, RESULTS), ==, 0x99);
    /* Format and vector fields, status register as it was at the trap. */
    g_assert_cmphex(qtest_readl(qts, RESULTS + 4), ==,
                    (format << 28) | (32 << 18) | 0x2700);
    /* A TRAP stacks the address of the next instruction. */
    g_assert_cmphex(qtest_readl(qts, RESULTS + 8), ==, trap_pc + 2);
    /* The frame is two longwords at a longword boundary below the stack. */
    g_assert_cmphex(qtest_readl(qts, RESULTS + 0x10), ==,
                    (sp & ~3) - 8);
    /* RTE gives back the stack pointer as it was, odd bits included. */
    g_assert_cmphex(qtest_readl(qts, RESULTS + 0x18), ==, sp);
    qtest_quit(qts);
}

static void test_frame_aligned(void)
{
    check_trap_frame(INITIAL_SP, 4);
}

/* The format field records how far the stack pointer was from alignment. */
static void test_frame_misaligned(void)
{
    check_trap_frame(INITIAL_SP + 1, 5);
    check_trap_frame(INITIAL_SP + 2, 6);
    check_trap_frame(INITIAL_SP + 3, 7);
}

/*
 * HALT is the end of the line: an interrupt cannot wake it, only a reset
 * restarts the core.
 */
static void test_halt_stops_until_reset(void)
{
    QTestState *qts = start();
    Asm a = { };
    Asm h = { };

    movei_d(&a, 0, 1);
    move_d_abs(&a, 0, RESULTS + 4);
    move_sr_imm(&a, 0x2000);
    emit16(&a, 0x4ac8);     /* halt */
    finish(&a, 0x66);
    finish(&h, 0xd1);
    set_vector(qts, 0, 70, HANDLER);
    qtest_memwrite(qts, HANDLER, h.buf, h.len);
    run(qts, &a);

    g_assert_cmphex(wait_result(qts, RESULTS + 4), ==, 1);
    /* Ask for the interrupt only once the core is in the HALT. */
    wait_halted(qts);
    qtest_writel(qts, SB_LINE_LEVEL, 3);
    qtest_writel(qts, SB_LINE_VECTOR, 70);
    qtest_writel(qts, SB_LINE_ASSERT, 1);
    /* Absence cannot be witnessed: wait long enough for a wrong wake-up. */
    g_usleep(500 * 1000);
    g_assert_cmphex(qtest_readl(qts, RESULTS), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, SB_ACCEPT_COUNT), ==, 0);

    /* A reset restarts it. */
    qtest_writel(qts, SB_LINE_ASSERT, 0);
    qtest_writel(qts, RESULTS + 4, 0);
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    g_assert_cmphex(wait_result(qts, RESULTS + 4), ==, 1);
    qtest_quit(qts);
}

/* Load a program that reports in, runs the HALT and would report again. */
static void load_halt_program(QTestState *qts)
{
    Asm a = { };
    Asm h = { };

    movei_d(&a, 0, 1);
    move_d_abs(&a, 0, RESULTS + 4);
    move_sr_imm(&a, 0x2000);
    emit16(&a, 0x4ac8);     /* halt */
    finish(&a, 0x66);
    finish(&h, 0xd1);
    set_vector(qts, 0, 70, HANDLER);
    qtest_memwrite(qts, HANDLER, h.buf, h.len);
    run(qts, &a);
}

static void raise_line(QTestState *qts)
{
    qtest_writel(qts, SB_LINE_LEVEL, 3);
    qtest_writel(qts, SB_LINE_VECTOR, 70);
    qtest_writel(qts, SB_LINE_ASSERT, 1);
}

/*
 * An ISA A only part stays halted when an interrupt arrives. Later parts
 * are not changed: the interrupt wakes them and the handler runs.
 */
static void test_halt_then_interrupt(const void *cpu)
{
    QTestState *qts = start_cpu(cpu);

    load_halt_program(qts);
    g_assert_cmphex(wait_result(qts, RESULTS + 4), ==, 1);
    /* Ask for the interrupt only once the core is in the HALT. */
    wait_halted(qts);
    raise_line(qts);
    if (!strcmp(cpu, "m5206")) {
        g_usleep(500 * 1000);
        g_assert_cmphex(qtest_readl(qts, RESULTS), ==, 0);
        g_assert_cmpuint(qtest_readl(qts, SB_ACCEPT_COUNT), ==, 0);
    } else {
        g_assert_cmphex(wait_result(qts, RESULTS), ==, 0xd1);
        g_assert_cmpuint(qtest_readl(qts, SB_ACCEPT_COUNT), ==, 1);
    }
    qtest_quit(qts);
}

/* A reset leaves the halted state for good: STOP later wakes normally. */
static void test_halt_reset_then_stop(void)
{
    QTestState *qts = start();
    Asm b = { };

    load_halt_program(qts);
    g_assert_cmphex(wait_result(qts, RESULTS + 4), ==, 1);
    wait_halted(qts);

    /* After the reset: report in, then wait in STOP for the interrupt. */
    movei_d(&b, 0, 2);
    move_d_abs(&b, 0, RESULTS + 4);
    emit16(&b, 0x4e72);     /* stop #0x2000 */
    emit16(&b, 0x2000);
    finish(&b, 0x77);
    qtest_memwrite(qts, CODE_BASE, b.buf, b.len);
    qtest_writel(qts, RESULTS + 4, 0);
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    g_assert_cmphex(wait_result(qts, RESULTS + 4), ==, 2);
    /* The reset cleared the halted flag, so this one is the STOP's. */
    wait_halted(qts);
    raise_line(qts);
    g_assert_cmphex(wait_result(qts, RESULTS), ==, 0xd1);
    qtest_quit(qts);
}

/* Wait until a migration command on this side has finished. */
static void wait_migration_done(QTestState *qts)
{
    gint64 end = g_get_monotonic_time() + RUN_TIMEOUT_US;

    for (;;) {
        QDict *rsp = qtest_qmp(qts, "{'execute': 'query-migrate'}");
        QDict *ret = qdict_get_qdict(rsp, "return");
        const char *status = qdict_haskey(ret, "status") ?
            qdict_get_str(ret, "status") : "none";
        bool done = !strcmp(status, "completed");

        g_assert_cmpstr(status, !=, "failed");
        qobject_unref(rsp);
        if (done) {
            return;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(10 * 1000);
    }
}

/*
 * A halted core is still halted after its state is saved and loaded: the
 * interrupt that arrives afterwards must not run, and the instruction
 * after the HALT must not execute.
 */
static void test_halt_survives_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("cf-core-test-XXXXXX", NULL);
    g_autofree char *file = g_build_filename(dir, "state", NULL);
    g_autofree char *uri = g_strdup_printf("file:%s", file);
    QTestState *src = start();
    QTestState *dst;

    load_halt_program(src);
    g_assert_cmphex(wait_result(src, RESULTS + 4), ==, 1);
    wait_halted(src);
    qtest_qmp_assert_success(src, "{'execute': 'stop'}");
    qtest_qmp_assert_success(src, "{'execute': 'migrate', 'arguments': "
                             "{'uri': %s}}", uri);
    wait_migration_done(src);
    qtest_quit(src);

    dst = qtest_initf("-machine cf-core-test -cpu m5206 -accel tcg -S "
                      "-incoming %s", uri);
    wait_migration_done(dst);
    raise_line(dst);
    qtest_qmp_assert_success(dst, "{'execute': 'cont'}");
    g_usleep(500 * 1000);
    g_assert_cmphex(qtest_readl(dst, RESULTS), ==, 0);
    g_assert_cmpuint(qtest_readl(dst, SB_ACCEPT_COUNT), ==, 0);

    /* A reset still restarts it. */
    qtest_writel(dst, SB_LINE_ASSERT, 0);
    qtest_writel(dst, RESULTS + 4, 0);
    qtest_qmp_assert_success(dst, "{'execute': 'system_reset'}");
    g_assert_cmphex(wait_result(dst, RESULTS + 4), ==, 1);
    qtest_quit(dst);
    unlink(file);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is not available");
        return g_test_run();
    }
    qtest_add_func("cf-core/run-program", test_run_program);
    qtest_add_func("cf-core/movec-control-registers",
                   test_movec_control_registers);
    qtest_add_data_func("cf-core/movec-unmasked-m5208", "m5208",
                        test_movec_unmasked_on_later_parts);
    qtest_add_data_func("cf-core/movec-unmasked-cfv4e", "cfv4e",
                        test_movec_unmasked_on_later_parts);
    qtest_add_func("cf-core/vbr-alignment", test_vbr_alignment);
    qtest_add_func("cf-core/movec-from-is-illegal",
                   test_movec_from_is_illegal);
    qtest_add_func("cf-core/irq-taken-late", test_irq_taken_late);
    qtest_add_func("cf-core/irq-masked-then-taken-and-rte",
                   test_irq_masked_then_taken_and_rte);
    qtest_add_func("cf-core/irq-without-provider", test_irq_without_provider);
    qtest_add_func("cf-core/frame-aligned", test_frame_aligned);
    qtest_add_func("cf-core/frame-misaligned", test_frame_misaligned);
    qtest_add_func("cf-core/halt-stops-until-reset",
                   test_halt_stops_until_reset);
    qtest_add_data_func("cf-core/halt-interrupt-m5206", "m5206",
                        test_halt_then_interrupt);
    qtest_add_data_func("cf-core/halt-interrupt-m5208", "m5208",
                        test_halt_then_interrupt);
    qtest_add_data_func("cf-core/halt-interrupt-cfv4e", "cfv4e",
                        test_halt_then_interrupt);
    qtest_add_data_func("cf-core/halt-interrupt-any", "any",
                        test_halt_then_interrupt);
    qtest_add_func("cf-core/halt-reset-then-stop", test_halt_reset_then_stop);
    qtest_add_func("cf-core/halt-survives-migration",
                   test_halt_survives_migration);
    return g_test_run();
}
