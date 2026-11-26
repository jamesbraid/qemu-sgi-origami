/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Stubs and checks shared by the emulator-only little-endian restart tests.
 * A stub in each byte order stores the word 0x11223344 into a mailbox in
 * RAM, so raw RAM shows the order. The includer defines MAILBOX_PHYS, the
 * mailbox's physical address, and MAILBOX_KSEG_HI, the upper half of the
 * KSEG address of MAILBOX_PHYS - MAILBOX_OFF.
 */

#ifndef SGI_RESTART_LE_H
#define SGI_RESTART_LE_H

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-mips-emit.h"
#include "sgi-wait.h"

#define MAILBOX_OFF     0xc00u

#define R_ZERO          0
#define R_T0            8
#define R_T1            9
#define R_T2            10
#define R_S6            22
#define OP_LL           0x30
#define OP_SC           0x38

/* Each stub owns a block of mailbox words. */
#define BE_BASE         0x00
#define LE_BASE         0x20
#define W_CONFIG0       0x00
#define W_STORED        0x04
#define W_LOADED        0x08
#define W_MARKER        0x0c
#define W_COUNT         0x10
#define W_CAUSE         0x14

#define BE_DONE         0xbe5b1001u
#define LE_DONE         0x1e5b2002u
#define CONFIG0_BE      (1u << 15)
#define WORD            0x11223344u

/* emit_stub() extras: add one to a shared count, record Cause. */
#define STUB_COUNT      1
#define STUB_CAUSE      2

typedef struct Code {
    uint8_t *b;
    unsigned int pos;
    bool be;
    /* Little-endian code stored as a doubleword-invariant flash image. */
    bool dword;
    /* Run first by a big-endian stub, for a machine that needs setting up. */
    void (*prologue)(struct Code *c);
    uint64_t md_config;
} Code;

static void emit(Code *c, uint32_t w)
{
    if (c->dword) {
        stl_be_p(c->b + (c->pos ^ 4), w);
    } else if (c->be) {
        stl_be_p(c->b + c->pos, w);
    } else {
        stl_le_p(c->b + c->pos, w);
    }
    c->pos += 4;
}

static void li32(Code *c, int rt, uint32_t v)
{
    emit(c, mips_lui(rt, v >> 16));
    emit(c, mips_ori(rt, rt, v & 0xffff));
}

/*
 * Record Config0 and the word stored then loaded back, optionally Cause and
 * a shared count added to with ll/sc, then set a marker and spin.
 */
static void emit_stub(Code *c, unsigned int base, uint32_t done,
                      unsigned int flags)
{
    unsigned int retry;

    if (c->be && c->prologue) {
        c->prologue(c);
    }
    emit(c, mips_lui(R_S6, MAILBOX_KSEG_HI));
    emit(c, mips_mfc0(R_T0, 16));
    emit(c, mips_sw(R_T0, R_S6, MAILBOX_OFF + base + W_CONFIG0));
    if (flags & STUB_CAUSE) {
        emit(c, mips_mfc0(R_T0, 13));
        emit(c, mips_sw(R_T0, R_S6, MAILBOX_OFF + base + W_CAUSE));
    }
    li32(c, R_T1, WORD);
    emit(c, mips_sw(R_T1, R_S6, MAILBOX_OFF + base + W_STORED));
    emit(c, mips_lw(R_T2, R_S6, MAILBOX_OFF + base + W_STORED));
    emit(c, mips_sw(R_T2, R_S6, MAILBOX_OFF + base + W_LOADED));
    if (flags & STUB_COUNT) {
        retry = c->pos;
        emit(c, mips_itype(OP_LL, R_S6, R_T0, MAILBOX_OFF + base + W_COUNT));
        emit(c, mips_addiu(R_T0, R_T0, 1));
        emit(c, mips_itype(OP_SC, R_S6, R_T0, MAILBOX_OFF + base + W_COUNT));
        emit(c, mips_branch(MIPS_OPC_BEQ, R_T0, R_ZERO,
                            ((int)retry - (int)(c->pos + 4)) / 4));
        emit(c, mips_nop());
    }
    li32(c, R_T0, done);
    emit(c, mips_sw(R_T0, R_S6, MAILBOX_OFF + base + W_MARKER));
    emit(c, mips_branch(MIPS_OPC_BEQ, R_ZERO, R_ZERO, -1));
    emit(c, mips_nop());
}

static uint32_t mailbox(QTestState *qts, unsigned int off, bool be)
{
    uint8_t raw[4];

    qtest_memread(qts, MAILBOX_PHYS + off, raw, 4);
    return be ? ldl_be_p(raw) : ldl_le_p(raw);
}

static void wait_mailbox(QTestState *qts, unsigned int off, uint32_t want,
                         bool be)
{
    SgiWait w;

    sgi_wait_start(&w, "mailbox 0x%x", off);
    while (mailbox(qts, off, be) != want && sgi_wait_continue(&w, 5000)) {
        /* poll */
    }
    g_assert_cmphex(mailbox(qts, off, be), ==, want);
}

static void set_order(QTestState *qts, const char *order)
{
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': '/machine', 'property': "
                             "'x-cpu-byte-order', 'value': %s}}", order);
}

/* Stop, change the byte order and resume, as the harness does. */
static void restart(QTestState *qts, uint32_t entry)
{
    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");
    if (entry) {
        qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                                 "{'path': '/machine', 'property': "
                                 "'x-restart-entry', 'value': %u}}", entry);
    }
    set_order(qts, "little");
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
}

static bool order_is(QTestState *qts, const char *want)
{
    QDict *rsp = qtest_qmp(
        qts, "{'execute': 'qom-get', 'arguments': {'path': '/machine', "
        "'property': 'x-cpu-byte-order'}}");
    bool same = !strcmp(qdict_get_str(rsp, "return"), want);

    qobject_unref(rsp);
    return same;
}

/* A stub ran in this order: Config0.BE, and the word's bytes in RAM. */
static void check_order(QTestState *qts, bool be, unsigned int cpus)
{
    unsigned int base = be ? BE_BASE : LE_BASE;
    uint8_t raw[4], want[4];

    if (cpus) {
        wait_mailbox(qts, base + W_COUNT, cpus, false);
    }
    wait_mailbox(qts, base + W_MARKER, be ? BE_DONE : LE_DONE, be);
    g_assert_cmphex(mailbox(qts, base + W_CONFIG0, be) & CONFIG0_BE, ==,
                    be ? CONFIG0_BE : 0);
    if (be) {
        stl_be_p(want, WORD);
    } else {
        stl_le_p(want, WORD);
    }
    qtest_memread(qts, MAILBOX_PHYS + base + W_STORED, raw, 4);
    g_assert_cmpmem(raw, 4, want, 4);
    g_assert_cmphex(mailbox(qts, base + W_LOADED, be), ==, WORD);
}

#endif /* SGI_RESTART_LE_H */
