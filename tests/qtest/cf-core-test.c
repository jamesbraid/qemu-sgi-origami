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
    return g_test_run();
}
