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

static QTestState *start(void)
{
    return qtest_init("-machine cf-core-test -accel tcg -S");
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is not available");
        return g_test_run();
    }
    qtest_add_func("cf-core/run-program", test_run_program);
    return g_test_run();
}
