/*
 * Unit tests for the i960 Rx/Jx core interpreter.
 *
 * Programs are assembled by hand from the instruction formats and opcode
 * tables in the Intel 80960Jx developer's manual (appendix B and C) and
 * run against a small flat-memory machine.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "migration/vmstate.h"
#include "migration/qemu-file-types.h"
#include "../migration/qemu-file.h"
#include "io/channel-file.h"
#include "hw/coproc/i960/i960.h"

/* ------------------------------------------------------------------ */
/* Test machine                                                       */
/* ------------------------------------------------------------------ */

#define RAM_SIZE    0x100000
#define ROM_BASE    0xfef80000u
#define ROM_SIZE    0x80000
#define MMIO_BASE   0xff009000u     /* an MMR-space device register file */
#define POLL_ADDR   0x20000000u     /* a status register for spin tests */

/* Layout of the structures the standard boot image points at. */
#define FAULT_TABLE 0x1000
#define SPT         0x2000
#define INT_TABLE   0x3000
#define CTRL_TABLE  0x4000
#define PRCB        0x5000
#define CODE        0x10000
#define HANDLERS    0x50000
#define ISP_ADDR    0x30000
#define SSP_ADDR    0x40000
#define USER_STACK  0x20000
#define DATA        0x8000

#define G(n) (16 + (n))
#define R(n) (n)

typedef struct Mach {
    I960State cpu;
    uint8_t ram[RAM_SIZE];
    uint8_t rom[ROM_SIZE];
    uint32_t mmio[16];
    uint32_t mmio_wr_addr[16];
    uint32_t mmio_wr_val[16];
    unsigned mmio_wr_n;
    uint32_t poll_val;
    unsigned poll_reads;
    unsigned stack_writes;
    unsigned burst_rd, burst_wr;
    unsigned reinit_calls;
    uint32_t fail_code;
    unsigned halt_calls;
    bool ctl_handled;
    uint32_t ctl_last_src1;
    I960CtlKind ctl_last_kind;
    uint32_t ctl_ret;
    uint32_t ip;
    I960Ops ops;
} Mach;

static uint32_t m_read(void *o, uint32_t a, unsigned sz)
{
    Mach *m = o;

    if (a < RAM_SIZE) {
        return ldn_le_p(m->ram + a, sz);
    }
    if (a >= ROM_BASE && a - ROM_BASE + sz <= ROM_SIZE) {
        return ldn_le_p(m->rom + (a - ROM_BASE), sz);
    }
    if (a >= MMIO_BASE && a < MMIO_BASE + 0x40) {
        return m->mmio[(a - MMIO_BASE) / 4];
    }
    if (a == POLL_ADDR) {
        m->poll_reads++;
        return m->poll_val;
    }
    return 0;
}

static void m_write(void *o, uint32_t a, uint32_t v, unsigned sz)
{
    Mach *m = o;

    if (a < RAM_SIZE) {
        if (a >= USER_STACK && a < ISP_ADDR) {
            m->stack_writes++;
        }
        stn_le_p(m->ram + a, sz, v);
    } else if (a >= MMIO_BASE && a < MMIO_BASE + 0x40) {
        m->mmio[(a - MMIO_BASE) / 4] = v;
    }
    if (a >= 0xff008000u && m->mmio_wr_n < 16) {
        m->mmio_wr_addr[m->mmio_wr_n] = a;
        m->mmio_wr_val[m->mmio_wr_n++] = v;
    }
}

static void m_read_burst(void *o, uint32_t a, uint32_t *w, unsigned n)
{
    Mach *m = o;

    m->burst_rd++;
    for (unsigned i = 0; i < n; i++) {
        w[i] = m_read(o, a + 4 * i, 4);
    }
}

static void m_write_burst(void *o, uint32_t a, const uint32_t *w, unsigned n)
{
    Mach *m = o;

    m->burst_wr++;
    for (unsigned i = 0; i < n; i++) {
        m_write(o, a + 4 * i, w[i], 4);
    }
}

static bool m_ctl(void *o, I960CtlKind kind, uint32_t src1, uint32_t src2,
                  uint32_t *srcdst)
{
    Mach *m = o;

    m->ctl_last_kind = kind;
    m->ctl_last_src1 = src1;
    if (m->ctl_handled) {
        *srcdst = m->ctl_ret;
    }
    return m->ctl_handled;
}

static void m_reinit(void *o)
{
    ((Mach *)o)->reinit_calls++;
}

static void m_halted(void *o)
{
    ((Mach *)o)->halt_calls++;
}

static void m_error(void *o, uint32_t code)
{
    ((Mach *)o)->fail_code = code;
}

static void wr32(Mach *m, uint32_t a, uint32_t v)
{
    stl_le_p(m->ram + a, v);
}

static uint32_t rd32(Mach *m, uint32_t a)
{
    return (uint32_t)ldl_le_p(m->ram + a);
}

/* ------------------------------------------------------------------ */
/* Assembler                                                          */
/* ------------------------------------------------------------------ */

static uint32_t reg3(uint32_t opc, unsigned s1, bool l1, unsigned s2, bool l2,
                     unsigned d)
{
    return ((opc >> 4) << 24) | (d << 19) | (s2 << 14) | ((uint32_t)l2 << 12) |
           ((uint32_t)l1 << 11) | ((opc & 0xf) << 7) | s1;
}

#define RRR(op, s1, s2, d)  reg3(op, s1, false, s2, false, d)
#define LRR(op, s1, s2, d)  reg3(op, s1, true, s2, false, d)
#define RLR(op, s1, s2, d)  reg3(op, s1, false, s2, true, d)
#define LLR(op, s1, s2, d)  reg3(op, s1, true, s2, true, d)

static uint32_t cobr(uint32_t op, unsigned s1, bool l1, unsigned s2,
                     int32_t disp)
{
    return (op << 24) | (s1 << 19) | (s2 << 14) | ((uint32_t)l1 << 13) |
           (((uint32_t)disp >> 2) & 0x7ff) << 2;
}

static uint32_t ctrl(uint32_t op, int32_t disp)
{
    return (op << 24) | (((uint32_t)disp >> 2) & 0x3fffff) << 2;
}

static uint32_t mema(uint32_t op, unsigned r, unsigned abase, uint32_t off,
                     bool based)
{
    return (op << 24) | (r << 19) | (abase << 14) | ((uint32_t)based << 13) |
           (off & 0xfff);
}

static uint32_t memb(uint32_t op, unsigned r, unsigned abase, unsigned mode,
                     unsigned scale, unsigned index)
{
    return (op << 24) | (r << 19) | (abase << 14) | (mode << 10) |
           (scale << 7) | index;
}

typedef struct Asm {
    Mach *m;
    uint32_t pc;
} Asm;

static void emit(Asm *a, uint32_t w)
{
    wr32(a->m, a->pc, w);
    a->pc += 4;
}

static void ldc(Asm *a, unsigned reg, uint32_t val)
{
    emit(a, memb(0x8c, reg, 0, 0xc, 0, 0));
    emit(a, val);
}

enum {
    OP_NOTBIT = 0x580, OP_AND = 0x581, OP_ANDNOT = 0x582, OP_SETBIT = 0x583,
    OP_NOTAND = 0x584, OP_XOR = 0x586, OP_OR = 0x587, OP_NOR = 0x588,
    OP_XNOR = 0x589, OP_NOT = 0x58a, OP_ORNOT = 0x58b, OP_CLRBIT = 0x58c,
    OP_NOTOR = 0x58d, OP_NAND = 0x58e, OP_ALTERBIT = 0x58f,
    OP_ADDO = 0x590, OP_ADDI = 0x591, OP_SUBO = 0x592, OP_SUBI = 0x593,
    OP_CMPOB = 0x594, OP_CMPIB = 0x595, OP_CMPOS = 0x596, OP_CMPIS = 0x597,
    OP_SHRO = 0x598, OP_SHRDI = 0x59a, OP_SHRI = 0x59b, OP_SHLO = 0x59c,
    OP_ROTATE = 0x59d, OP_SHLI = 0x59e,
    OP_CMPO = 0x5a0, OP_CMPI = 0x5a1, OP_CONCMPO = 0x5a2, OP_CONCMPI = 0x5a3,
    OP_CMPINCO = 0x5a4, OP_CMPDECI = 0x5a7, OP_SCANBYTE = 0x5ac,
    OP_BSWAP = 0x5ad, OP_CHKBIT = 0x5ae,
    OP_ADDC = 0x5b0, OP_SUBC = 0x5b2, OP_INTDIS = 0x5b4, OP_INTEN = 0x5b5,
    OP_MOV = 0x5cc, OP_ESHRO = 0x5d8, OP_MOVL = 0x5dc, OP_MOVT = 0x5ec,
    OP_MOVQ = 0x5fc,
    OP_ATMOD = 0x610, OP_ATADD = 0x612,
    OP_SPANBIT = 0x640, OP_SCANBIT = 0x641, OP_MODAC = 0x645,
    OP_MODIFY = 0x650, OP_EXTRACT = 0x651, OP_MODTC = 0x654,
    OP_MODPC = 0x655, OP_INTCTL = 0x658, OP_SYSCTL = 0x659,
    OP_ICCTL = 0x65b, OP_DCCTL = 0x65c, OP_HALT = 0x65d,
    OP_CALLS = 0x660, OP_MARK = 0x66b, OP_FMARK = 0x66c,
    OP_FLUSHREG = 0x66d, OP_SYNCF = 0x66f,
    OP_EMUL = 0x670, OP_EDIV = 0x671,
    OP_MULO = 0x701, OP_REMO = 0x708, OP_DIVO = 0x70b,
    OP_MULI = 0x741, OP_REMI = 0x748, OP_MODI = 0x749, OP_DIVI = 0x74b,
    OP_ADDOE = 0x7a0, OP_ADDONE = 0x7d0, OP_SELE = 0x7a4, OP_SELNE = 0x7d4,
    OP_SUBIG = 0x793,
};

enum {
    C_B = 0x08, C_CALL = 0x09, C_RET = 0x0a, C_BAL = 0x0b,
    C_BG = 0x11, C_BE = 0x12, C_BL = 0x14, C_BNE = 0x15, C_FAULTNE = 0x1d,
    C_FAULTNO = 0x18, C_TESTL = 0x24, C_TESTE = 0x22, C_TESTNE = 0x25,
    C_BBC = 0x30, C_CMPOBGE = 0x33, C_CMPOBE = 0x32, C_CMPOBNE = 0x35,
    C_BBS = 0x37, C_CMPIBL = 0x3c, C_CMPIBG = 0x39, C_CMPIBE = 0x3a,
    M_LDOB = 0x80, M_STOB = 0x82, M_BX = 0x84, M_BALX = 0x85, M_CALLX = 0x86,
    M_LDOS = 0x88, M_STOS = 0x8a, M_LDA = 0x8c, M_LD = 0x90, M_ST = 0x92,
    M_LDL = 0x98, M_STL = 0x9a, M_LDT = 0xa0, M_STT = 0xa2, M_LDQ = 0xb0,
    M_STQ = 0xb2, M_LDIB = 0xc0, M_STIB = 0xc2, M_LDIS = 0xc8, M_STIS = 0xca,
};

#define HALT2(a)    emit(a, LRR(OP_HALT, 2, 0, 0))

/* ------------------------------------------------------------------ */
/* Fixture                                                            */
/* ------------------------------------------------------------------ */

/* Make the 8 IBR words after PMCON sum to zero with end-around carry. */
static void ibr_fix_checksum(Mach *m, uint32_t ibr_off)
{
    uint32_t acc = 0xffffffff, carry = 0;

    for (int i = 0; i < 7; i++) {
        uint32_t w = ldl_le_p(m->rom + ibr_off + 0x10 + 4 * i);
        uint64_t t = (uint64_t)w + acc + carry;

        acc = (uint32_t)t;
        carry = t >> 32;
    }
    stl_le_p(m->rom + ibr_off + 0x10 + 28, 0 - acc - carry);
}

static void write_ibr(Mach *m, uint32_t entry, uint32_t prcb)
{
    uint32_t off = I960_IBR_ADDR - ROM_BASE;

    memset(m->rom + off, 0, 0x30);
    stl_le_p(m->rom + off + 0x10, entry);
    stl_le_p(m->rom + off + 0x14, prcb);
    stl_le_p(m->rom + off + 0x18, 0x11111111);
    stl_le_p(m->rom + off + 0x1c, 0x22222222);
    stl_le_p(m->rom + off + 0x20, 0x33333333);
    stl_le_p(m->rom + off + 0x24, 0x44444444);
    stl_le_p(m->rom + off + 0x28, 0x55555555);
    ibr_fix_checksum(m, off);
}

static void write_prcb(Mach *m, uint32_t at, uint32_t fault_table,
                       uint32_t ctrl_table, uint32_t int_table, uint32_t spt,
                       uint32_t isp)
{
    wr32(m, at + 0x00, fault_table);
    wr32(m, at + 0x04, ctrl_table);
    wr32(m, at + 0x08, 0);              /* AC initial image */
    wr32(m, at + 0x0c, 0);              /* fault config: unaligned faults */
    wr32(m, at + 0x10, int_table);
    wr32(m, at + 0x14, spt);
    wr32(m, at + 0x18, 0);
    wr32(m, at + 0x1c, isp);
    wr32(m, at + 0x20, 0);
    wr32(m, at + 0x24, 0);
}

static Mach *mach_new_variant(I960Variant variant)
{
    Mach *m = g_new0(Mach, 1);

    m->ops.read = m_read;
    m->ops.write = m_write;
    m->ops.system_error = m_error;
    m->ops.reinit = m_reinit;
    m->ops.halted = m_halted;
    i960_init(&m->cpu, variant, &m->ops, m);
    m->cpu.device_id = 0x0082a013;
    return m;
}

/* An Rx-class part (i960 RD) is the default. */
static Mach *mach_new(void)
{
    return mach_new_variant(I960_VARIANT_RX);
}

static void set_fault_entry(Mach *m, unsigned type, uint32_t handler)
{
    wr32(m, FAULT_TABLE + type * 8, handler);
    wr32(m, FAULT_TABLE + type * 8 + 4, 0);
}

/* A system-call entry: the handler is procedure `proc` in the SPT. */
static void set_fault_sys(Mach *m, unsigned type, unsigned proc)
{
    wr32(m, FAULT_TABLE + type * 8, (proc << 2) | 2);
    wr32(m, FAULT_TABLE + type * 8 + 4, 0x27f);
}

static void set_vector(Mach *m, unsigned vector, uint32_t handler)
{
    wr32(m, INT_TABLE + 4 * vector + 4, handler);
}

/*
 * RAM-resident tables, a ROM boot record pointing at them, and entry at
 * CODE.  Supervisor procedure 6 lives at HANDLERS+0x400 and local
 * procedure 5 at HANDLERS+0x500, filled in by the tests that use them.
 */
static void mach_setup(Mach *m, uint32_t entry)
{
    write_prcb(m, PRCB, FAULT_TABLE, CTRL_TABLE, INT_TABLE, SPT, ISP_ADDR);
    wr32(m, SPT + 12, SSP_ADDR);
    wr32(m, SPT + 48 + 4 * 5, HANDLERS + 0x500);
    wr32(m, SPT + 48 + 4 * 6, (HANDLERS + 0x400) | 2);
    write_ibr(m, entry, PRCB);
}

static void mach_boot(Mach *m)
{
    i960_reset(&m->cpu);
    g_assert_false(m->cpu.failed);
}

/* Run until the program halts (idle) or fail. */
static void mach_run(Mach *m)
{
    int64_t used = i960_run(&m->cpu, 1000000);

    g_assert_true(m->cpu.idle_hint);
    g_assert_true(m->cpu.halted);
    g_assert_cmpint(used, <, 1000000);
}

/* Drop to executing state, priority 0, with a user-stack frame. */
static void prelude(Asm *a, uint32_t prio_mask_value)
{
    ldc(a, G(14), 0x001f2000);          /* mask: priority and state */
    ldc(a, G(13), prio_mask_value);
    emit(a, RRR(OP_MODPC, G(14), G(14), G(13)));
    ldc(a, G(15), USER_STACK);
    ldc(a, R(1), USER_STACK + 64);
}

/*
 * Generic fault handler.  Records the fault record fields in g8-g12 and
 * counts in g7, then returns.  Entry point `at`.
 */
static void gen_fault_handler(Mach *m, uint32_t at, bool skip_insn)
{
    Asm a = { m, at };

    emit(&a, LRR(OP_SUBO, 8, G(15), R(4)));
    emit(&a, mema(M_LD, G(8), R(4), 0, true));
    emit(&a, LRR(OP_SUBO, 4, G(15), R(4)));
    emit(&a, mema(M_LD, G(9), R(4), 0, true));
    emit(&a, LRR(OP_SUBO, 24, G(15), R(4)));
    emit(&a, mema(M_LD, G(10), R(4), 0, true));
    emit(&a, LRR(OP_SUBO, 16, G(15), R(4)));
    emit(&a, mema(M_LD, G(11), R(4), 0, true));
    emit(&a, LRR(OP_SUBO, 12, G(15), R(4)));
    emit(&a, mema(M_LD, G(12), R(4), 0, true));
    emit(&a, LRR(OP_ADDO, 1, G(7), G(7)));
    if (skip_insn) {
        /* advance the RIP saved in the faulting frame by one word */
        emit(&a, RRR(OP_FLUSHREG, 0, 0, 0));
        emit(&a, LRR(OP_ANDNOT, 15, R(0), R(4)));
        emit(&a, mema(M_LD, R(5), R(4), 8, true));
        emit(&a, LRR(OP_ADDO, 4, R(5), R(5)));
        emit(&a, mema(M_ST, R(5), R(4), 8, true));
    }
    emit(&a, ctrl(C_RET, 0));
}

static void all_faults_to(Mach *m, uint32_t handler)
{
    for (unsigned t = 1; t <= 0xa; t++) {
        set_fault_entry(m, t, handler);
    }
}

/* ------------------------------------------------------------------ */
/* Tests: REG format                                                  */
/* ------------------------------------------------------------------ */

static void test_reg_logic(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    ldc(&a, G(0), 0xf0f0f0f0);
    ldc(&a, G(1), 0x00ff00ff);
    emit(&a, RRR(OP_AND, G(1), G(0), R(3)));
    emit(&a, RRR(OP_OR, G(1), G(0), R(4)));
    emit(&a, RRR(OP_XOR, G(1), G(0), R(5)));
    emit(&a, RRR(OP_ANDNOT, G(1), G(0), R(6)));
    emit(&a, RRR(OP_NOTAND, G(1), G(0), R(7)));
    emit(&a, RRR(OP_NOR, G(1), G(0), R(8)));
    emit(&a, RRR(OP_XNOR, G(1), G(0), R(9)));
    emit(&a, RRR(OP_NOT, G(1), 0, R(10)));
    emit(&a, RRR(OP_ORNOT, G(1), G(0), R(11)));
    emit(&a, RRR(OP_NOTOR, G(1), G(0), R(12)));
    emit(&a, RRR(OP_NAND, G(1), G(0), R(13)));
    emit(&a, LRR(OP_SETBIT, 0, G(0), R(14)));
    emit(&a, LRR(OP_CLRBIT, 4, G(0), G(2)));
    emit(&a, LRR(OP_NOTBIT, 5, G(0), G(3)));
    emit(&a, LLR(OP_CMPO, 1, 1, 0));        /* cc = equal */
    emit(&a, LRR(OP_ALTERBIT, 8, G(1), G(4)));
    emit(&a, LLR(OP_CMPO, 1, 2, 0));        /* cc = less -> alterbit clears */
    emit(&a, LRR(OP_ALTERBIT, 0, G(1), G(5)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(m->cpu.r[3], ==, 0x00f000f0);
    g_assert_cmphex(m->cpu.r[4], ==, 0xf0fff0ff);
    g_assert_cmphex(m->cpu.r[5], ==, 0xf00ff00f);
    g_assert_cmphex(m->cpu.r[6], ==, 0xf000f000);
    g_assert_cmphex(m->cpu.r[7], ==, 0x000f000f);
    g_assert_cmphex(m->cpu.r[8], ==, 0x0f000f00);
    g_assert_cmphex(m->cpu.r[9], ==, 0x0ff00ff0);
    g_assert_cmphex(m->cpu.r[10], ==, 0xff00ff00);
    g_assert_cmphex(m->cpu.r[11], ==, 0xfff0fff0);
    g_assert_cmphex(m->cpu.r[12], ==, 0x0fff0fff);
    g_assert_cmphex(m->cpu.r[13], ==, 0xff0fff0f);
    g_assert_cmphex(m->cpu.r[14], ==, 0xf0f0f0f1);
    g_assert_cmphex(m->cpu.r[G(2)], ==, 0xf0f0f0e0);
    g_assert_cmphex(m->cpu.r[G(3)], ==, 0xf0f0f0d0);
    g_assert_cmphex(m->cpu.r[G(4)], ==, 0x00ff01ff);     /* set bit 8 */
    g_assert_cmphex(m->cpu.r[G(5)], ==, 0x00ff00fe);     /* clear bit 0 */
    g_free(m);
}

static void test_reg_arith(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    ldc(&a, G(0), 100);
    ldc(&a, G(1), 7);
    ldc(&a, G(2), (uint32_t)-100);
    emit(&a, RRR(OP_ADDO, G(1), G(0), R(3)));        /* 107 */
    emit(&a, RRR(OP_SUBO, G(1), G(0), R(4)));        /* 93 */
    emit(&a, RRR(OP_MULO, G(1), G(0), R(5)));        /* 700 */
    emit(&a, RRR(OP_DIVO, G(1), G(0), R(6)));        /* 14 */
    emit(&a, RRR(OP_REMO, G(1), G(0), R(7)));        /* 2 */
    emit(&a, RRR(OP_MULI, G(1), G(2), R(8)));        /* -700 */
    emit(&a, RRR(OP_DIVI, G(1), G(2), R(9)));        /* -14 */
    emit(&a, RRR(OP_REMI, G(1), G(2), R(10)));       /* -2 */
    emit(&a, RRR(OP_MODI, G(1), G(2), R(11)));       /* 5 */
    ldc(&a, G(4), 0xffffffff);
    emit(&a, RRR(OP_EMUL, G(4), G(4), G(6)));        /* g6,g7 */
    ldc(&a, G(8), 5);
    ldc(&a, G(9), 1);                                /* g8,g9 = 2^32 + 5 */
    emit(&a, RRR(OP_EDIV, G(1), G(8), G(10)));       /* g10 rem, g11 quot */
    emit(&a, LRR(OP_SHLO, 4, G(1), R(12)));          /* 7 << 4 = 112 */
    emit(&a, LRR(OP_SHRO, 1, G(0), R(13)));          /* 50 */
    emit(&a, LRR(OP_SHRI, 2, G(2), R(14)));          /* -25 */
    ldc(&a, G(12), (uint32_t)-7);
    emit(&a, LRR(OP_SHRI, 1, G(12), R(15)));         /* -4 */
    emit(&a, LRR(OP_SHRDI, 1, G(12), G(13)));        /* -3 */
    ldc(&a, G(14), 0x80000001);
    emit(&a, LRR(OP_ROTATE, 1, G(14), G(5)));        /* 3 */
    emit(&a, LRR(OP_SHLO, 31, G(14), G(3)));         /* 0x80000000 */
    ldc(&a, G(15), 32);
    emit(&a, RRR(OP_SHLO, G(15), G(14), G(0)));      /* 0 */
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[3], ==, 107);
    g_assert_cmpuint(m->cpu.r[4], ==, 93);
    g_assert_cmpuint(m->cpu.r[5], ==, 700);
    g_assert_cmpuint(m->cpu.r[6], ==, 14);
    g_assert_cmpuint(m->cpu.r[7], ==, 2);
    g_assert_cmpint((int32_t)m->cpu.r[8], ==, -700);
    g_assert_cmpint((int32_t)m->cpu.r[9], ==, -14);
    g_assert_cmpint((int32_t)m->cpu.r[10], ==, -2);
    g_assert_cmpuint(m->cpu.r[11], ==, 5);
    g_assert_cmphex(m->cpu.r[G(6)], ==, 0x00000001);
    g_assert_cmphex(m->cpu.r[G(7)], ==, 0xfffffffe);
    g_assert_cmpuint(m->cpu.r[G(10)], ==, (uint32_t)((0x100000005ull) % 7));
    g_assert_cmpuint(m->cpu.r[G(11)], ==, (uint32_t)((0x100000005ull) / 7));
    g_assert_cmpuint(m->cpu.r[12], ==, 112);
    g_assert_cmpuint(m->cpu.r[13], ==, 50);
    g_assert_cmpint((int32_t)m->cpu.r[14], ==, -25);
    g_assert_cmpint((int32_t)m->cpu.r[15], ==, -4);
    g_assert_cmpint((int32_t)m->cpu.r[G(13)], ==, -3);
    g_assert_cmphex(m->cpu.r[G(5)], ==, 3);
    g_assert_cmphex(m->cpu.r[G(3)], ==, 0x80000000);
    g_assert_cmphex(m->cpu.r[G(0)], ==, 0);
    g_free(m);
}

static void test_reg_bits_and_compares(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    ldc(&a, G(0), 0x00010000);
    emit(&a, RRR(OP_SCANBIT, G(0), 0, R(3)));
    emit(&a, LLR(OP_MODAC, 0x7, 0, R(4)));           /* r4 = AC (cc=2) */
    emit(&a, LRR(OP_SCANBIT, 0, 0, R(5)));           /* literal 0: -1, cc 0 */
    emit(&a, LLR(OP_MODAC, 0x7, 0, R(6)));           /* r6 = AC (cc=0) */
    ldc(&a, G(1), 0xffff0000);
    emit(&a, RRR(OP_SPANBIT, G(1), 0, R(7)));        /* 15 */
    ldc(&a, G(2), 0x11111111);
    ldc(&a, G(3), 0xabcd1234);
    ldc(&a, G(4), 0xff00);
    emit(&a, RRR(OP_MODIFY, G(4), G(3), G(2)));      /* 0x11111211 */
    ldc(&a, G(5), 0x12345678);
    emit(&a, LLR(OP_EXTRACT, 4, 8, G(5)));           /* 0x67 */
    ldc(&a, G(6), 0x12345678);
    emit(&a, RRR(OP_BSWAP, G(6), 0, G(6)));          /* 0x78563412 */
    ldc(&a, G(8), 0x80000000);
    ldc(&a, G(9), 1);                                /* g8,g9 */
    emit(&a, LRR(OP_ESHRO, 4, G(8), G(10)));         /* 0x18000000 */
    ldc(&a, G(11), 0x1ff);
    ldc(&a, G(12), 0x0ff);
    emit(&a, RRR(OP_CMPOB, G(11), G(12), 0));        /* equal bytes */
    emit(&a, LLR(OP_MODAC, 0x7, 0, R(8)));
    ldc(&a, G(13), 0x80);
    ldc(&a, G(14), 0x7f);
    emit(&a, RRR(OP_CMPIB, G(13), G(14), 0));        /* -128 < 127: less */
    emit(&a, LLR(OP_MODAC, 0x7, 0, R(9)));
    emit(&a, RRR(OP_CMPOB, G(13), G(14), 0));        /* 0x80 > 0x7f */
    emit(&a, LLR(OP_MODAC, 0x7, 0, R(10)));
    emit(&a, RRR(OP_CMPIS, G(13), G(14), 0));        /* 0x80 > 0x7f */
    emit(&a, LLR(OP_MODAC, 0x7, 0, R(11)));
    ldc(&a, G(7), 0x11223344);
    ldc(&a, G(0), 0x99223399);
    emit(&a, RRR(OP_SCANBYTE, G(7), G(0), 0));       /* byte 2 matches */
    emit(&a, LLR(OP_MODAC, 0x7, 0, R(12)));
    emit(&a, LLR(OP_CMPO, 7, 3, 0));                 /* greater */
    emit(&a, LLR(OP_CONCMPO, 2, 5, 0));              /* 2 <= 5 -> equal */
    emit(&a, LLR(OP_MODAC, 0x7, 0, R(13)));
    emit(&a, LLR(OP_CMPO, 1, 3, 0));                 /* less */
    emit(&a, LLR(OP_CONCMPO, 2, 5, 0));              /* unchanged: less */
    emit(&a, LLR(OP_MODAC, 0x7, 0, R(14)));
    ldc(&a, G(1), 0x8);
    emit(&a, LRR(OP_CHKBIT, 3, G(1), 0));            /* bit 3 set: equal */
    emit(&a, LLR(OP_MODAC, 0x7, 0, R(15)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[3], ==, 16);
    g_assert_cmpuint(m->cpu.r[4] & 7, ==, 2);
    g_assert_cmphex(m->cpu.r[5], ==, 0xffffffff);
    g_assert_cmpuint(m->cpu.r[6] & 7, ==, 0);
    g_assert_cmpuint(m->cpu.r[7], ==, 15);
    g_assert_cmphex(m->cpu.r[G(2)], ==, 0x11111211);
    g_assert_cmphex(m->cpu.r[G(5)], ==, 0x67);
    g_assert_cmphex(m->cpu.r[G(6)], ==, 0x78563412);
    g_assert_cmphex(m->cpu.r[G(10)], ==, 0x18000000);
    g_assert_cmpuint(m->cpu.r[8] & 7, ==, 2);
    g_assert_cmpuint(m->cpu.r[9] & 7, ==, 4);
    g_assert_cmpuint(m->cpu.r[10] & 7, ==, 1);
    g_assert_cmpuint(m->cpu.r[11] & 7, ==, 1);
    g_assert_cmpuint(m->cpu.r[12] & 7, ==, 2);
    g_assert_cmpuint(m->cpu.r[13] & 7, ==, 2);
    g_assert_cmpuint(m->cpu.r[14] & 7, ==, 4);
    g_assert_cmpuint(m->cpu.r[15] & 7, ==, 2);
    g_free(m);
}

static void test_reg_extended_carry_cond(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    ldc(&a, G(0), 0xffffffff);
    emit(&a, LLR(OP_MODAC, 7, 0, R(3)));             /* cc = 0 */
    emit(&a, LRR(OP_ADDC, 1, G(0), G(1)));           /* g1 = 0, carry */
    emit(&a, LLR(OP_MODAC, 0, 0, R(4)));             /* r4 = AC (no change) */
    emit(&a, LLR(OP_ADDC, 0, 0, G(2)));              /* g2 = 0 + 0 + carry */
    /* 64-bit subtract: 0x1_00000000 - 1 */
    emit(&a, LLR(OP_MODAC, 7, 0, R(5)));
    emit(&a, LLR(OP_CMPO, 1, 1, 0));                 /* cc=equal: carry set */
    emit(&a, LLR(OP_SUBC, 1, 0, G(3)));              /* 0 - 1 - 1 + 1 */
    /* conditional ops: cc = equal */
    emit(&a, LLR(OP_CMPO, 4, 4, 0));
    emit(&a, LLR(OP_SELE, 7, 9, G(4)));              /* sele: cond -> src2 */
    emit(&a, LLR(OP_SELNE, 7, 9, G(5)));             /* selne: -> src1 */
    ldc(&a, G(6), 0);
    emit(&a, LLR(OP_ADDOE, 3, 4, G(6)));             /* equal: 7 */
    ldc(&a, G(7), 55);
    emit(&a, LLR(OP_ADDONE, 3, 4, G(7)));            /* not equal: unchanged */
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(m->cpu.r[G(1)], ==, 0);
    g_assert_cmpuint(m->cpu.r[4] & 7, ==, 2);        /* carry flag */
    g_assert_cmpuint(m->cpu.r[G(2)], ==, 1);
    g_assert_cmphex(m->cpu.r[G(3)], ==, 0xffffffff); /* 0 - 1 */
    g_assert_cmpuint(m->cpu.r[G(4)], ==, 9);
    g_assert_cmpuint(m->cpu.r[G(5)], ==, 7);
    g_assert_cmpuint(m->cpu.r[G(6)], ==, 7);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 55);
    g_free(m);
}

static void test_reg_move_groups(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    ldc(&a, G(0), 0x11);
    ldc(&a, G(1), 0x22);
    ldc(&a, G(2), 0x33);
    ldc(&a, G(3), 0x44);
    emit(&a, RRR(OP_MOVL, G(0), 0, R(4)));
    emit(&a, RRR(OP_MOVT, G(0), 0, R(8)));
    emit(&a, RRR(OP_MOVQ, G(0), 0, R(12)));
    emit(&a, LRR(OP_MOVQ, 9, 0, G(4)));              /* literal zero-extends */
    emit(&a, LRR(OP_MOVL, 5, 0, G(8)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(m->cpu.r[4], ==, 0x11);
    g_assert_cmphex(m->cpu.r[5], ==, 0x22);
    g_assert_cmphex(m->cpu.r[8], ==, 0x11);
    g_assert_cmphex(m->cpu.r[10], ==, 0x33);
    g_assert_cmphex(m->cpu.r[15], ==, 0x44);
    g_assert_cmphex(m->cpu.r[G(4)], ==, 9);
    g_assert_cmphex(m->cpu.r[G(5)], ==, 0);
    g_assert_cmphex(m->cpu.r[G(7)], ==, 0);
    g_assert_cmphex(m->cpu.r[G(8)], ==, 5);
    g_assert_cmphex(m->cpu.r[G(9)], ==, 0);
    g_free(m);
}

static void test_atomics(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    wr32(m, DATA, 0xaabbccdd);
    wr32(m, DATA + 8, 10);
    ldc(&a, G(0), DATA + 1);
    ldc(&a, G(1), 0x0000ff00);
    ldc(&a, G(2), 0x00001100);
    emit(&a, RRR(OP_ATMOD, G(0), G(1), G(2)));
    ldc(&a, G(3), DATA + 8);
    ldc(&a, G(4), 5);
    emit(&a, RRR(OP_ATADD, G(3), G(4), G(5)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(rd32(m, DATA), ==, 0xaabb11dd);
    g_assert_cmphex(m->cpu.r[G(2)], ==, 0xaabbccdd);
    g_assert_cmpuint(rd32(m, DATA + 8), ==, 15);
    g_assert_cmpuint(m->cpu.r[G(5)], ==, 10);
    g_free(m);
}

/* ------------------------------------------------------------------ */
/* Tests: COBR and CTRL                                               */
/* ------------------------------------------------------------------ */

static void test_cobr_loop(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    uint32_t loop;

    mach_setup(m, CODE);
    emit(&a, LRR(OP_MOV, 0, 0, G(0)));               /* sum = 0 */
    emit(&a, LRR(OP_MOV, 1, 0, G(1)));               /* i = 1 */
    loop = a.pc;
    emit(&a, RRR(OP_ADDO, G(1), G(0), G(0)));
    emit(&a, LRR(OP_ADDO, 1, G(1), G(1)));
    emit(&a, cobr(C_CMPOBGE, 5, true, G(1), loop - a.pc)); /* 5 >= i */
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(0)], ==, 15);
    g_free(m);
}

static void test_cobr_misc(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    ldc(&a, G(0), (uint32_t)-1);
    emit(&a, LRR(OP_MOV, 2, 0, G(1)));
    /* signed: -1 < 2 so cmpibl g0? src1=g0(-1) < src2=g1(2): taken */
    emit(&a, cobr(C_CMPIBL, G(0), false, G(1), 8));
    emit(&a, LRR(OP_MOV, 9, 0, G(2)));               /* skipped */
    /* unsigned: 0xffffffff > 2: cmpobg not taken for src1<src2 */
    emit(&a, cobr(C_CMPOBGE, G(1), false, G(0), 8)); /* 2 >= 0xffffffff? no */
    emit(&a, LRR(OP_MOV, 7, 0, G(3)));               /* executed */
    ldc(&a, G(4), 0x10);
    emit(&a, cobr(C_BBS, 4, true, G(4), 8));         /* bit 4 set: taken */
    emit(&a, LRR(OP_MOV, 9, 0, G(5)));               /* skipped */
    emit(&a, cobr(C_BBC, 4, true, G(4), 8));         /* bit 4 set: not taken */
    emit(&a, LRR(OP_MOV, 3, 0, G(6)));               /* executed */
    emit(&a, LLR(OP_CMPO, 4, 4, 0));
    emit(&a, cobr(C_TESTE, G(8), false, 0, 0));
    emit(&a, cobr(C_TESTL, G(9), false, 0, 0));
    emit(&a, cobr(C_TESTNE, G(10), false, 0, 0));
    emit(&a, ctrl(C_FAULTNO, 0));                    /* cc != 0: no fault */
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(2)], ==, 0);
    g_assert_cmpuint(m->cpu.r[G(3)], ==, 7);
    g_assert_cmpuint(m->cpu.r[G(5)], ==, 0);
    g_assert_cmpuint(m->cpu.r[G(6)], ==, 3);
    g_assert_cmpuint(m->cpu.r[G(8)], ==, 1);
    g_assert_cmpuint(m->cpu.r[G(9)], ==, 0);
    g_assert_cmpuint(m->cpu.r[G(10)], ==, 0);
    g_free(m);
}

static void test_ctrl_branches(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    emit(&a, ctrl(C_B, 8));                          /* skip next */
    emit(&a, LRR(OP_MOV, 9, 0, G(0)));
    emit(&a, ctrl(C_BAL, 8));                        /* g14 = return IP */
    emit(&a, LRR(OP_MOV, 9, 0, G(1)));               /* skipped */
    emit(&a, LLR(OP_CMPO, 2, 1, 0));                 /* 2 > 1: cc greater */
    emit(&a, ctrl(C_BG, 8));
    emit(&a, LRR(OP_MOV, 9, 0, G(2)));               /* skipped */
    emit(&a, ctrl(C_BL, 8));                         /* not taken */
    emit(&a, LRR(OP_MOV, 4, 0, G(3)));               /* executed */
    ldc(&a, G(4), CODE + 0x200);
    emit(&a, memb(M_BX, 0, G(4), 4, 0, 0));          /* bx (g4) */
    emit(&a, LRR(OP_MOV, 9, 0, G(5)));               /* skipped */
    a.pc = CODE + 0x200;
    emit(&a, memb(M_BALX, G(6), 0, 0xc, 0, 0));
    emit(&a, CODE + 0x300);
    a.pc = CODE + 0x300;
    emit(&a, LRR(OP_MOV, 1, 0, G(7)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(m->cpu.r[G(0)], ==, 0x0082a013);   /* mov skipped */
    g_assert_cmphex(m->cpu.r[G(14)], ==, CODE + 12);
    g_assert_cmpuint(m->cpu.r[G(1)], ==, 0);
    g_assert_cmpuint(m->cpu.r[G(2)], ==, 0);
    g_assert_cmpuint(m->cpu.r[G(3)], ==, 4);
    g_assert_cmpuint(m->cpu.r[G(5)], ==, 0);
    g_assert_cmphex(m->cpu.r[G(6)], ==, CODE + 0x200 + 8);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 1);
    g_free(m);
}

/* ------------------------------------------------------------------ */
/* Tests: MEM format                                                  */
/* ------------------------------------------------------------------ */

static void test_mem_sizes_and_modes(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    uint32_t ipref;

    mach_setup(m, CODE);
    wr32(m, DATA, 0x8091a2b3);
    ldc(&a, G(0), DATA);
    emit(&a, mema(M_LDOB, R(3), G(0), 0, true));     /* 0xb3 */
    emit(&a, mema(M_LDIB, R(4), G(0), 0, true));     /* sign-extended */
    emit(&a, mema(M_LDOS, R(5), G(0), 2, true));     /* 0x8091 */
    emit(&a, mema(M_LDIS, R(6), G(0), 2, true));     /* sign-extended */
    emit(&a, memb(M_LD, R(7), 0, 0xc, 0, 0));        /* absolute displacement */
    emit(&a, DATA);
    emit(&a, memb(M_LD, R(8), G(0), 4, 0, 0));       /* (g0) */
    ldc(&a, G(1), 3);
    emit(&a, memb(M_LD, R(9), G(0), 7, 2, G(1)));    /* (g0)[g1*4] */
    emit(&a, memb(M_LDA, R(10), G(0), 0xd, 0, 0));   /* disp(g0) */
    emit(&a, 0x10);
    emit(&a, memb(M_LDA, R(11), 0, 0xe, 3, G(1)));   /* disp[g1*8] */
    emit(&a, 0x100);
    emit(&a, memb(M_LDA, R(12), G(0), 0xf, 1, G(1)));
    emit(&a, 0x1000);
    ipref = a.pc;
    emit(&a, memb(M_LD, R(13), 0, 5, 0, 0));         /* disp(ip) */
    emit(&a, 0x20);                                  /* ip + 8 + 0x20 */
    wr32(m, ipref + 8 + 0x20, 0xcafef00d);
    ldc(&a, G(2), 0x12345678);
    emit(&a, mema(M_STOB, G(2), G(0), 0x20, true));
    emit(&a, mema(M_STOS, G(2), G(0), 0x24, true));
    emit(&a, mema(M_ST, G(2), G(0), 0x28, true));
    emit(&a, mema(M_LDA, R(14), G(0), 0x44, true));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(m->cpu.r[3], ==, 0xb3);
    g_assert_cmphex(m->cpu.r[4], ==, 0xffffffb3);
    g_assert_cmphex(m->cpu.r[5], ==, 0x8091);
    g_assert_cmphex(m->cpu.r[6], ==, 0xffff8091);
    g_assert_cmphex(m->cpu.r[7], ==, rd32(m, DATA));
    g_assert_cmphex(m->cpu.r[8], ==, 0x8091a2b3);
    g_assert_cmphex(m->cpu.r[9], ==, rd32(m, DATA + 12));
    g_assert_cmphex(m->cpu.r[10], ==, DATA + 0x10);
    g_assert_cmphex(m->cpu.r[11], ==, 0x100 + 24);
    g_assert_cmphex(m->cpu.r[12], ==, DATA + 0x1000 + 6);
    g_assert_cmphex(m->cpu.r[13], ==, 0xcafef00d);
    g_assert_cmphex(m->ram[DATA + 0x20], ==, 0x78);
    g_assert_cmphex(ldn_le_p(m->ram + DATA + 0x24, 2), ==, 0x5678);
    g_assert_cmphex(rd32(m, DATA + 0x28), ==, 0x12345678);
    g_assert_cmphex(m->cpu.r[14], ==, DATA + 0x44);
    g_free(m);
}

static void test_mem_groups_and_bursts(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    m->ops.read_burst = m_read_burst;
    m->ops.write_burst = m_write_burst;
    for (int i = 0; i < 4; i++) {
        wr32(m, 0x40000 + 4 * i, 0x100 + i);
        wr32(m, 0x40010 + 4 * i, 0x200 + i);
    }
    ldc(&a, G(0), 0x40000);
    emit(&a, mema(M_LDQ, R(4), G(0), 0, true));
    emit(&a, mema(M_LDL, R(8), G(0), 8, true));
    emit(&a, mema(M_LDT, R(12), G(0), 0x10, true));
    emit(&a, mema(M_STQ, R(4), G(0), 0x20, true));
    emit(&a, mema(M_STL, R(8), G(0), 0x30, true));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(m->cpu.r[4], ==, 0x100);
    g_assert_cmphex(m->cpu.r[7], ==, 0x103);
    g_assert_cmphex(m->cpu.r[8], ==, 0x102);
    g_assert_cmphex(m->cpu.r[9], ==, 0x103);
    g_assert_cmphex(m->cpu.r[12], ==, 0x200);
    g_assert_cmphex(m->cpu.r[14], ==, 0x202);
    g_assert_cmphex(rd32(m, 0x40020), ==, 0x100);
    g_assert_cmphex(rd32(m, 0x4002c), ==, 0x103);
    g_assert_cmphex(rd32(m, 0x40034), ==, 0x103);
    g_assert_cmpuint(m->burst_rd, ==, 3);
    g_assert_cmpuint(m->burst_wr, ==, 2);
    g_free(m);
}

static void test_mem_unaligned(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, false);
    all_faults_to(m, HANDLERS);
    for (int i = 0; i < 8; i++) {
        m->ram[DATA + i] = 0x10 + i;
    }
    ldc(&a, G(0), DATA + 1);
    emit(&a, mema(M_LD, R(4), G(0), 0, true));       /* unaligned word */
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(m->cpu.r[4], ==, 0x14131211);    /* performed */
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 1);
    g_assert_cmphex(m->cpu.r[G(8)], ==, 0x00020003);
    g_assert_cmphex(m->cpu.r[G(10)], ==, DATA + 1);
    g_free(m);

    /* masked by bit 30 of the PRCB fault configuration word */
    m = mach_new();
    a = (Asm){ m, CODE };
    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, false);
    all_faults_to(m, HANDLERS);
    wr32(m, PRCB + 0x0c, 1u << 30);
    ldc(&a, G(0), DATA + 1);
    emit(&a, mema(M_LD, R(4), G(0), 0, true));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 0);
    g_free(m);
}

/* ------------------------------------------------------------------ */
/* Tests: calls and returns                                           */
/* ------------------------------------------------------------------ */

static void test_call_ret_frames(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    Asm f = { m, HANDLERS };
    uint32_t callpc;

    mach_setup(m, CODE);
    /* callee: record FP, PFP, SP; trash r4; return */
    emit(&f, RRR(OP_MOV, G(15), 0, G(0)));
    emit(&f, RRR(OP_MOV, R(0), 0, G(1)));
    emit(&f, RRR(OP_MOV, R(1), 0, G(2)));
    emit(&f, RRR(OP_MOV, R(2), 0, G(3)));
    emit(&f, LRR(OP_MOV, 0, 0, R(4)));
    emit(&f, ctrl(C_RET, 0));

    prelude(&a, 0);
    ldc(&a, R(4), 0x1234);
    callpc = a.pc;
    emit(&a, ctrl(C_CALL, HANDLERS - a.pc));
    emit(&a, RRR(OP_MOV, R(4), 0, G(4)));
    emit(&a, RRR(OP_MOV, G(15), 0, G(5)));
    emit(&a, RRR(OP_MOV, R(1), 0, G(6)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(m->cpu.r[G(0)], ==, USER_STACK + 64);        /* new FP */
    g_assert_cmphex(m->cpu.r[G(1)], ==, USER_STACK);             /* PFP, rt 0 */
    g_assert_cmphex(m->cpu.r[G(2)], ==, USER_STACK + 64 + 64);   /* SP */
    g_assert_cmphex(m->cpu.r[G(3)], ==, callpc + 4);            /* RIP */
    g_assert_cmphex(m->cpu.r[G(4)], ==, 0x1234);
    g_assert_cmphex(m->cpu.r[G(5)], ==, USER_STACK);
    g_assert_cmphex(m->cpu.r[G(6)], ==, USER_STACK + 64);
    g_free(m);
}

static void test_deep_calls_spill_fill(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    Asm f = { m, HANDLERS };
    uint32_t done;

    mach_setup(m, CODE);
    /* f(g0): if g0 == 0 return; r4 = g0; g0--; f(); g1 += r4 */
    done = HANDLERS + 0x14;
    emit(&f, cobr(C_CMPOBE, 0, true, G(0), done - f.pc));
    emit(&f, RRR(OP_MOV, G(0), 0, R(4)));
    emit(&f, LRR(OP_SUBO, 1, G(0), G(0)));
    emit(&f, ctrl(C_CALL, HANDLERS - f.pc));
    emit(&f, RRR(OP_ADDO, R(4), G(1), G(1)));
    f.pc = done;
    emit(&f, ctrl(C_RET, 0));

    prelude(&a, 0);
    ldc(&a, G(0), 20);
    emit(&a, LRR(OP_MOV, 0, 0, G(1)));
    emit(&a, ctrl(C_CALL, HANDLERS - a.pc));
    emit(&a, RRR(OP_MOV, G(15), 0, G(2)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(1)], ==, 210);       /* 20 + 19 + ... + 1 */
    g_assert_cmphex(m->cpu.r[G(2)], ==, USER_STACK);
    g_assert_cmpuint(m->stack_writes, >, 0);         /* frames spilled */
    g_assert_cmpuint(m->cpu.rcache_n, ==, 0);
    g_free(m);
}

static void test_flushreg(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    Asm f = { m, HANDLERS };

    mach_setup(m, CODE);
    emit(&f, RRR(OP_FLUSHREG, 0, 0, 0));
    emit(&f, ctrl(C_RET, 0));
    prelude(&a, 0);
    ldc(&a, R(4), 0xfeedbeef);
    emit(&a, ctrl(C_CALL, HANDLERS - a.pc));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    /* the caller's local set went to its save area at its FP */
    g_assert_cmphex(rd32(m, USER_STACK + 16), ==, 0xfeedbeef);
    g_free(m);
}

/*
 * Rx manual 6.2.43: modpc src, mask, src/dst. The mask is the middle operand
 * (src2) and src is a dummy literal or the mask's register. Every other test
 * passes the mask register twice, which hides a swap of the two.
 */
static void test_modpc_mask_is_src2(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    prelude(&a, 0x001f0000);    /* priority 31 first */
    ldc(&a, G(12), 0x001f0000); /* mask: the priority field */
    ldc(&a, G(13), 0);          /* value: priority 0 */
    emit(&a, LRR(OP_MODPC, 0, G(12), G(13)));
    HALT2(&a);
    mach_boot(m);
    i960_run(&m->cpu, 100000);
    g_assert_cmpuint(i960_priority(&m->cpu), ==, 0);
    g_assert_cmphex(m->cpu.r[G(13)] & I960_PC_PRIO_MASK, ==,
                    0x001f0000);        /* the old PC comes back */
    g_free(m);
}

static void test_modpc_user_mode(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, true);
    all_faults_to(m, HANDLERS);
    prelude(&a, 0);
    ldc(&a, G(12), I960_PC_EM);
    ldc(&a, G(13), 0);
    emit(&a, LRR(OP_MODPC, 0, G(12), G(13)));        /* to user mode */
    ldc(&a, G(10), 0x1f0000);
    ldc(&a, G(11), 0x1f0000);
    emit(&a, LRR(OP_MODPC, 0, G(10), G(11)));        /* mask != 0: fault */
    emit(&a, LLR(OP_MODPC, 0, 0, G(9)));             /* mask 0: reads PC */
    emit(&a, ctrl(C_B, 0));
    mach_boot(m);
    i960_run(&m->cpu, 100000);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 1);
    g_assert_cmphex(m->cpu.r[G(8)], ==, (I960_FT_TYPE << 16) |
                                        I960_FS_TYPE_MISMATCH);
    g_assert_cmphex(m->cpu.r[G(9)] & I960_PC_EM, ==, 0);
    g_assert_cmpuint(i960_priority(&m->cpu), ==, 0);
    g_free(m);
}

/* modac/modtc/modify with distinct mask and value registers. */
static void test_mod_operand_order(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    ldc(&a, G(1), 0x7);                              /* mask */
    ldc(&a, G(2), 0x5);                              /* value */
    emit(&a, RRR(OP_MODAC, G(1), G(2), G(3)));       /* AC.cc = 5 */
    emit(&a, LLR(OP_MODAC, 0, 0, G(4)));
    ldc(&a, G(5), I960_TC_BRANCH);                   /* mask */
    ldc(&a, G(6), I960_TC_BRANCH | I960_TC_CALL);    /* value */
    emit(&a, RRR(OP_MODTC, G(5), G(6), G(7)));       /* sets branch only */
    ldc(&a, G(8), 0x00ff);                           /* mask */
    ldc(&a, G(9), 0xabcd);                           /* src */
    ldc(&a, G(10), 0x1111);
    emit(&a, RRR(OP_MODIFY, G(8), G(9), G(10)));     /* 0x11cd */
    ldc(&a, G(11), 4);
    ldc(&a, G(12), 8);
    ldc(&a, G(13), 0x12345678);
    emit(&a, RRR(OP_EXTRACT, G(11), G(12), G(13)));  /* bitpos g11, len g12 */
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(4)] & 7, ==, 5);
    g_assert_cmphex(m->cpu.tc, ==, I960_TC_BRANCH);
    g_assert_cmphex(m->cpu.r[G(10)], ==, 0x11cd);
    g_assert_cmphex(m->cpu.r[G(13)], ==, 0x67);
    g_free(m);
}

/* ------------------------------------------------------------------ */
/* Local register preservation across calls                           */
/* ------------------------------------------------------------------ */

static uint32_t pat(unsigned n)
{
    return 0xa5000000u + n * 0x01010101u;
}

/*
 * A call allocates a new local set and ret restores the caller's whole
 * r0-r15 (Jx manual 7.1.3, 7.1.4): r4-r15 must survive a leaf that
 * overwrites them all, including r15 at frame offset 0x3c.
 */
static void test_call_preserves_r4_r15(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    Asm f = { m, HANDLERS };

    mach_setup(m, CODE);
    for (unsigned n = 4; n <= 15; n++) {
        emit(&f, LRR(OP_MOV, 9, 0, R(n)));
    }
    emit(&f, memb(M_LDQ, R(12), 0, 0xc, 0, 0));      /* group loads too */
    emit(&f, 0x40000);
    emit(&f, memb(M_LDT, R(8), 0, 0xc, 0, 0));
    emit(&f, 0x40000);
    emit(&f, ctrl(C_RET, 0));
    prelude(&a, 0);
    for (unsigned n = 4; n <= 15; n++) {
        ldc(&a, R(n), pat(n));
    }
    emit(&a, ctrl(C_CALL, HANDLERS - a.pc));
    for (unsigned n = 4; n <= 15; n++) {
        emit(&a, RRR(OP_MOV, R(n), 0, G(n - 4)));
    }
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    for (unsigned n = 4; n <= 15; n++) {
        g_assert_cmphex(m->cpu.r[G(n - 4)], ==, pat(n));
    }
    g_free(m);
}

/* Each level sets r4, r14 and r15, recurses, then checks them. */
static void test_deep_calls_preserve_r15(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    Asm f = { m, HANDLERS };
    uint32_t done = HANDLERS + 0x40;
    static const unsigned regs[] = { 4, 14, 15 };

    mach_setup(m, CODE);
    emit(&f, cobr(C_CMPOBE, 0, true, G(0), done - f.pc));
    for (unsigned i = 0; i < 3; i++) {
        emit(&f, mema(M_LDA, R(regs[i]), G(0), 0x800 + 0x100 * i, true));
    }
    emit(&f, LRR(OP_SUBO, 1, G(0), G(0)));
    emit(&f, ctrl(C_CALL, HANDLERS - f.pc));
    emit(&f, LRR(OP_ADDO, 1, G(0), G(0)));
    for (unsigned i = 0; i < 3; i++) {
        emit(&f, mema(M_LDA, R(5), G(0), 0x800 + 0x100 * i, true));
        emit(&f, cobr(C_CMPOBE, R(5), false, R(regs[i]), 8));
        emit(&f, LRR(OP_ADDO, 1, G(1), G(1)));
    }
    g_assert_cmpuint(f.pc, ==, done);
    emit(&f, ctrl(C_RET, 0));

    prelude(&a, 0);
    ldc(&a, G(0), 20);                               /* > 7 cached frames */
    emit(&a, LRR(OP_MOV, 0, 0, G(1)));
    ldc(&a, R(15), 0xfeedf00d);
    emit(&a, ctrl(C_CALL, HANDLERS - a.pc));
    emit(&a, RRR(OP_MOV, R(15), 0, G(2)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(1)], ==, 0);         /* no level mismatched */
    g_assert_cmphex(m->cpu.r[G(2)], ==, 0xfeedf00d);
    g_assert_cmpuint(m->stack_writes, >, 0);         /* it did spill */
    g_free(m);
}

/* callx through a register: globals pass in and out, locals survive. */
static void test_callx_register_target(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    Asm f = { m, HANDLERS };

    mach_setup(m, CODE);
    emit(&f, RRR(OP_MOV, G(0), 0, G(1)));            /* g0 as passed */
    emit(&f, LRR(OP_MOV, 5, 0, G(2)));
    emit(&f, LRR(OP_MOV, 9, 0, R(14)));
    emit(&f, LRR(OP_MOV, 9, 0, R(15)));
    emit(&f, ctrl(C_RET, 0));
    prelude(&a, 0);
    ldc(&a, R(10), HANDLERS);
    ldc(&a, R(14), 0x14141414);
    ldc(&a, R(15), 0x15151515);
    ldc(&a, G(0), 0x77);
    emit(&a, memb(M_CALLX, 0, R(10), 4, 0, 0));      /* callx (r10) */
    emit(&a, RRR(OP_MOV, R(15), 0, G(0)));           /* right after return */
    emit(&a, RRR(OP_MOV, R(14), 0, G(3)));
    emit(&a, memb(M_CALLX, 0, R(10), 4, 0, 0));
    emit(&a, RRR(OP_MOV, R(15), 0, G(4)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(m->cpu.r[G(0)], ==, 0x15151515);
    g_assert_cmphex(m->cpu.r[G(1)], ==, 0x15151515);  /* 2nd call saw g0 */
    g_assert_cmphex(m->cpu.r[G(2)], ==, 5);
    g_assert_cmphex(m->cpu.r[G(3)], ==, 0x14141414);
    g_assert_cmphex(m->cpu.r[G(4)], ==, 0x15151515);
    g_free(m);
}

/* The saved frame: r15 at FP + 0x3c, all 16 words round-trip. */
static void test_frame_layout(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    Asm f = { m, HANDLERS };
    uint32_t callpc;

    mach_setup(m, CODE);
    emit(&f, RRR(OP_FLUSHREG, 0, 0, 0));
    emit(&f, ctrl(C_RET, 0));
    prelude(&a, 0);
    for (unsigned n = 3; n <= 15; n++) {
        ldc(&a, R(n), pat(n));
    }
    callpc = a.pc;
    emit(&a, ctrl(C_CALL, HANDLERS - a.pc));
    for (unsigned n = 3; n <= 15; n++) {
        emit(&a, RRR(OP_MOV, R(n), 0, G(n)));
    }
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(rd32(m, USER_STACK + 0x3c), ==, pat(15));
    for (unsigned n = 3; n <= 15; n++) {
        g_assert_cmphex(rd32(m, USER_STACK + 4 * n), ==, pat(n));
        g_assert_cmphex(m->cpu.r[G(n)], ==, pat(n));
    }
    g_assert_cmphex(rd32(m, USER_STACK + 4), ==, USER_STACK + 64);   /* SP */
    g_assert_cmphex(rd32(m, USER_STACK + 8), ==, callpc + 4);        /* RIP */
    g_free(m);
}

static void test_calls_modes(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    Asm sup = { m, HANDLERS + 0x400 };
    Asm loc = { m, HANDLERS + 0x500 };

    mach_setup(m, CODE);
    emit(&sup, LLR(OP_MODPC, 0, 0, G(8)));
    emit(&sup, RRR(OP_MOV, R(0), 0, G(9)));
    emit(&sup, RRR(OP_MOV, G(15), 0, G(10)));
    emit(&sup, ctrl(C_RET, 0));
    emit(&loc, RRR(OP_MOV, G(15), 0, G(11)));
    emit(&loc, ctrl(C_RET, 0));

    prelude(&a, 0);
    ldc(&a, G(12), 2);
    ldc(&a, G(13), 0);
    emit(&a, RRR(OP_MODPC, G(12), G(12), G(13)));    /* to user mode */
    emit(&a, LRR(OP_CALLS, 5, 0, 0));
    emit(&a, LRR(OP_CALLS, 6, 0, 0));
    emit(&a, RRR(OP_MOV, G(15), 0, G(5)));
    emit(&a, LLR(OP_MODPC, 0, 0, G(14)));            /* mask 0 works in user */
    HALT2(&a);                                       /* halt is privileged */
    set_fault_entry(m, I960_FT_TYPE, HANDLERS + 0x600);
    {
        Asm h = { m, HANDLERS + 0x600 };

        emit(&h, LRR(OP_ADDO, 1, G(7), G(7)));
        emit(&h, ctrl(C_RET, 0));
    }
    mach_boot(m);
    i960_run(&m->cpu, 100000);
    /* local system call: same stack, no mode switch */
    g_assert_cmphex(m->cpu.r[G(11)], ==, USER_STACK + 64);
    /* supervisor call: switched to the supervisor stack and mode */
    g_assert_cmphex(m->cpu.r[G(8)] & I960_PC_EM, ==, I960_PC_EM);
    g_assert_cmphex(m->cpu.r[G(9)] & 7, ==, 2);       /* return type 010 */
    g_assert_cmphex(m->cpu.r[G(10)], ==, SSP_ADDR);
    /* returned to the user stack and mode */
    g_assert_cmphex(m->cpu.r[G(5)], ==, USER_STACK);
    g_assert_cmphex(m->cpu.r[G(14)] & I960_PC_EM, ==, 0);
    /*
     * halt in user mode: TYPE.MISMATCH, handled, then the handler's ret
     * resumes at the halt again, so the run never idles.
     */
    g_assert_cmpuint(m->cpu.r[G(7)], >, 0);
    g_free(m);
}

static void test_calls_protection_length(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, true);
    all_faults_to(m, HANDLERS);
    ldc(&a, G(0), 260);
    emit(&a, RRR(OP_CALLS, G(0), 0, 0));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 1);
    g_assert_cmphex(m->cpu.r[G(8)], ==, (I960_FT_PROTECTION << 16) |
                                        I960_FS_PROTECTION_LENGTH);
    g_free(m);
}

/* ------------------------------------------------------------------ */
/* Tests: faults                                                      */
/* ------------------------------------------------------------------ */

static void test_fault_zero_divide(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    uint32_t divpc;

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, false);
    all_faults_to(m, HANDLERS);
    ldc(&a, G(0), 10);
    emit(&a, LLR(OP_CMPO, 4, 4, 0));                 /* AC.cc = equal */
    divpc = a.pc;
    emit(&a, LRR(OP_DIVO, 0, G(0), G(1)));
    emit(&a, LRR(OP_MOV, 1, 0, G(2)));               /* resumes here */
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 1);
    g_assert_cmphex(m->cpu.r[G(8)], ==, (I960_FT_ARITHMETIC << 16) |
                                        I960_FS_ARITH_ZERO_DIVIDE);
    g_assert_cmphex(m->cpu.r[G(9)], ==, divpc);      /* faulting IP */
    g_assert_cmphex(m->cpu.r[G(11)], ==, 0x001f2002);/* saved PC */
    g_assert_cmpuint(m->cpu.r[G(12)] & 7, ==, 2);    /* saved AC */
    g_assert_cmpuint(m->cpu.r[G(2)], ==, 1);         /* resumed after */
    g_free(m);
}

static void test_fault_integer_overflow(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, false);
    all_faults_to(m, HANDLERS);
    ldc(&a, G(0), 0x7fffffff);
    emit(&a, LRR(OP_ADDI, 1, G(0), G(1)));           /* faults, dst written */
    emit(&a, LLR(OP_MODAC, 0, 0, G(3)));
    ldc(&a, G(4), I960_AC_OM);
    emit(&a, RRR(OP_MODAC, G(4), G(4), G(5)));       /* mask overflow */
    emit(&a, LRR(OP_ADDI, 1, G(0), G(2)));           /* no fault; AC.of */
    emit(&a, LLR(OP_MODAC, 0, 0, G(6)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 1);
    g_assert_cmphex(m->cpu.r[G(8)], ==, (I960_FT_ARITHMETIC << 16) |
                                        I960_FS_ARITH_OVERFLOW);
    g_assert_cmphex(m->cpu.r[G(1)], ==, 0x80000000);
    g_assert_cmphex(m->cpu.r[G(2)], ==, 0x80000000);
    g_assert_cmphex(m->cpu.r[G(6)] & I960_AC_OF, ==, I960_AC_OF);
    g_free(m);
}

static void test_fault_invalid_opcode_and_range(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, false);
    gen_fault_handler(m, HANDLERS + 0x100, true);
    all_faults_to(m, HANDLERS);
    set_fault_entry(m, I960_FT_OPERATION, HANDLERS + 0x100);
    emit(&a, 0x40000000);                            /* no such opcode */
    emit(&a, LLR(OP_CMPO, 4, 4, 0));
    emit(&a, ctrl(C_FAULTNE, 0));                    /* cc equal: not taken */
    emit(&a, ctrl(0x1f, 0));                         /* faulto: CONSTRAINT */
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 2);
    g_assert_cmphex(m->cpu.r[G(8)], ==, (I960_FT_CONSTRAINT << 16) |
                                        I960_FS_CONSTRAINT_RANGE);
    g_free(m);
}

static void test_fault_type_mismatch_and_mmr(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, true);
    all_faults_to(m, HANDLERS);
    prelude(&a, 0);
    ldc(&a, G(12), I960_PC_EM);
    ldc(&a, G(13), 0);
    emit(&a, RRR(OP_MODPC, G(12), G(12), G(13)));    /* to user mode */
    ldc(&a, G(0), 0x1f2000);
    emit(&a, RRR(OP_MODPC, G(0), G(0), G(13)));      /* TYPE.MISMATCH */
    emit(&a, ctrl(C_B, 0));                          /* spin */
    mach_boot(m);
    i960_run(&m->cpu, 100000);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 1);
    g_assert_cmphex(m->cpu.pc & I960_PC_EM, ==, 0);   /* still user mode */
    g_assert_cmphex(m->cpu.r[G(8)], ==, (I960_FT_TYPE << 16) |
                                        I960_FS_TYPE_MISMATCH);
    g_free(m);
}

static void test_mmr_access_rules(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, true);
    all_faults_to(m, HANDLERS);
    ldc(&a, G(0), I960_MMR_IMSK);
    ldc(&a, G(1), 0x55);
    emit(&a, mema(M_ST, G(1), G(0), 0, true));       /* IMSK = 0x55 */
    emit(&a, mema(M_LD, G(2), G(0), 0, true));
    emit(&a, mema(M_LDOB, G(3), G(0), 0, true));     /* byte: UNIMPLEMENTED */
    ldc(&a, G(4), I960_MMR_IMSK + 2);
    emit(&a, mema(M_LD, G(5), G(4), 0, true));       /* unaligned MMR */
    ldc(&a, G(6), 0xff);
    emit(&a, RRR(OP_ATMOD, G(0), G(6), G(1)));       /* atomic modify */
    ldc(&a, G(9), MMIO_BASE + 8);
    ldc(&a, G(10), 0xdead0001);
    emit(&a, mema(M_ST, G(10), G(9), 0, true));      /* device register */
    emit(&a, mema(M_LD, G(11), G(9), 0, true));
    HALT2(&a);
    m->mmio[2] = 0;
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(m->cpu.r[G(2)], ==, 0x55);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 2);
    g_assert_cmphex(m->cpu.r[G(8)], ==, (I960_FT_OPERATION << 16) |
                                        I960_FS_OP_UNIMPLEMENTED);
    g_assert_cmphex(m->cpu.imsk, ==, 0x55);
    g_assert_cmphex(m->cpu.r[G(1)], ==, 0x55);        /* old value */
    g_assert_cmphex(m->mmio[2], ==, 0xdead0001);
    g_assert_cmphex(m->cpu.r[G(11)], ==, 0xdead0001);
    g_free(m);
}

/* ------------------------------------------------------------------ */
/* Tests: interrupts                                                  */
/* ------------------------------------------------------------------ */

/* Interrupt handler: g6++, vector -> g8, saved IMSK (r3) -> g9, FP -> g10. */
static void gen_int_handler(Mach *m, uint32_t at)
{
    Asm a = { m, at };

    emit(&a, LRR(OP_SUBO, 8, G(15), R(4)));
    emit(&a, mema(M_LD, G(8), R(4), 0, true));
    emit(&a, RRR(OP_MOV, R(3), 0, G(9)));
    emit(&a, RRR(OP_MOV, G(15), 0, G(10)));
    emit(&a, LRR(OP_ADDO, 1, G(6), G(6)));
    emit(&a, ctrl(C_RET, 0));
}

static void test_sw_interrupt_sysctl(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_int_handler(m, HANDLERS);
    set_vector(m, 0x31, HANDLERS);                   /* priority 6 */
    prelude(&a, 0);
    ldc(&a, G(0), 0x31);
    emit(&a, RRR(OP_SYSCTL, G(0), G(0), G(0)));      /* post */
    emit(&a, RRR(OP_MOV, G(6), 0, G(1)));            /* serviced already */
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(6)], ==, 1);
    g_assert_cmpuint(m->cpu.r[G(1)], ==, 1);
    g_assert_cmpuint(m->cpu.r[G(8)], ==, 0x31);      /* full vector number */
    g_assert_cmphex(m->cpu.r[G(10)], ==, ISP_ADDR + 16); /* interrupt stack */
    g_assert_cmphex(m->cpu.pc & I960_PC_S, ==, 0);   /* PC restored */
    g_assert_cmpuint(i960_priority(&m->cpu), ==, 0);
    g_assert_cmphex(m->cpu.r[G(15)], ==, USER_STACK);
    g_assert_cmphex(rd32(m, INT_TABLE), ==, 0);      /* no longer pending */
    g_free(m);
}

static void test_sw_interrupt_pending_until_priority_drops(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_int_handler(m, HANDLERS);
    set_vector(m, 0x31, HANDLERS);
    prelude(&a, 10u << 16);                          /* priority 10 */
    ldc(&a, G(0), 0x31);
    emit(&a, RRR(OP_SYSCTL, G(0), G(0), G(0)));      /* priority 6 < 10 */
    emit(&a, RRR(OP_MOV, G(6), 0, G(1)));            /* not yet */
    ldc(&a, G(2), 0x1f0000);
    ldc(&a, G(3), 0);
    emit(&a, RRR(OP_MODPC, G(2), G(2), G(3)));       /* priority 0 */
    emit(&a, RRR(OP_MOV, G(6), 0, G(4)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(1)], ==, 0);
    g_assert_cmpuint(m->cpu.r[G(4)], ==, 1);
    g_free(m);
}

static void test_sw_interrupt_pending_bits(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    set_vector(m, 0x31, HANDLERS);
    prelude(&a, 10u << 16);
    ldc(&a, G(0), 0x31);
    emit(&a, RRR(OP_SYSCTL, G(0), G(0), G(0)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(rd32(m, INT_TABLE), ==, 1u << 6);
    g_assert_cmphex(m->ram[INT_TABLE + 4 + 6], ==, 1u << (0x31 & 7));
    /* the device posting the same way an external agent would */
    i960_post_interrupt(&m->cpu, 0x32);
    g_assert_cmphex(m->ram[INT_TABLE + 4 + 6], ==, 0x06);
    g_free(m);
}

static void test_nested_interrupt(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    Asm h = { m, HANDLERS };

    mach_setup(m, CODE);
    /*
     * The handler for vector 0x31 posts vector 0x51 (priority 10) and then
     * records how many times the higher handler already ran.
     */
    ldc(&h, G(0), 0x51);
    emit(&h, RRR(OP_SYSCTL, G(0), G(0), G(0)));
    emit(&h, RRR(OP_MOV, G(5), 0, G(1)));
    emit(&h, RRR(OP_MOV, R(1), 0, G(2)));
    emit(&h, ctrl(C_RET, 0));
    {
        Asm n = { m, HANDLERS + 0x100 };

        emit(&n, LRR(OP_ADDO, 1, G(5), G(5)));
        emit(&n, RRR(OP_MOV, G(15), 0, G(3)));
        emit(&n, ctrl(C_RET, 0));
    }
    set_vector(m, 0x31, HANDLERS);
    set_vector(m, 0x51, HANDLERS + 0x100);
    prelude(&a, 0);
    ldc(&a, G(0), 0x31);
    emit(&a, RRR(OP_SYSCTL, G(0), G(0), G(0)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(1)], ==, 1);          /* nested ran first */
    g_assert_cmpuint(m->cpu.r[G(5)], ==, 1);
    g_assert_cmpuint(i960_priority(&m->cpu), ==, 0);
    g_free(m);
}

static void test_xint_dedicated(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_int_handler(m, HANDLERS);
    set_vector(m, 0x32, HANDLERS);                   /* XINT3 -> 0x32 */
    wr32(m, CTRL_TABLE + 0x10, 3u << 12);            /* IMAP0: pin 3 */
    wr32(m, CTRL_TABLE + 0x1c,
         (0xffu << 2) | (1u << I960_ICON_MO_SHIFT)); /* edge, clear on dedic. */
    prelude(&a, 0);
    ldc(&a, G(0), I960_MMR_IMSK);
    ldc(&a, G(1), 0xff);
    emit(&a, mema(M_ST, G(1), G(0), 0, true));
    emit(&a, LRR(OP_HALT, 1, 0, 0));                 /* enable and halt */
    HALT2(&a);
    mach_boot(m);
    g_assert_cmphex(m->cpu.imap[0], ==, 3u << 12);
    i960_run(&m->cpu, 100000);
    g_assert_true(m->cpu.halted);
    g_assert_cmpuint(m->halt_calls, ==, 1);
    i960_set_xint(&m->cpu, 1 << 3);
    g_assert_false(m->cpu.idle_hint);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(6)], ==, 1);
    g_assert_cmpuint(m->cpu.r[G(8)], ==, 0x32);
    g_assert_cmphex(m->cpu.r[G(9)], ==, 0xff);        /* IMSK saved in r3 */
    g_assert_cmphex(m->cpu.imsk, ==, 0);              /* and cleared */
    g_assert_cmphex(m->cpu.ipnd, ==, 0);
    g_free(m);
}

static void test_xint_expanded_and_nmi(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_int_handler(m, HANDLERS);
    set_vector(m, 0x45, HANDLERS);
    set_vector(m, 248, HANDLERS + 0x100);
    {
        Asm n = { m, HANDLERS + 0x100 };

        emit(&n, LRR(OP_ADDO, 1, G(5), G(5)));
        emit(&n, LRR(OP_SUBO, 8, G(15), R(4)));
        emit(&n, mema(M_LD, G(3), R(4), 0, true));
        emit(&n, ctrl(C_RET, 0));
    }
    wr32(m, CTRL_TABLE + 0x1c, 1 | (2u << I960_ICON_MO_SHIFT)); /* expanded */
    prelude(&a, 0);
    ldc(&a, G(0), I960_MMR_IMSK);
    ldc(&a, G(1), 1);
    emit(&a, mema(M_ST, G(1), G(0), 0, true));
    emit(&a, LRR(OP_HALT, 1, 0, 0));
    emit(&a, LRR(OP_HALT, 1, 0, 0));
    HALT2(&a);
    mach_boot(m);
    i960_run(&m->cpu, 100000);
    i960_set_xint(&m->cpu, 0x45);
    i960_run(&m->cpu, 100000);
    g_assert_cmpuint(m->cpu.r[G(6)], ==, 1);
    g_assert_cmpuint(m->cpu.r[G(8)], ==, 0x45);
    g_assert_cmphex(m->cpu.r[G(9)], ==, 1);
    g_assert_cmphex(m->cpu.imsk, ==, 0);
    i960_set_xint(&m->cpu, 0);
    /* NMI interrupts even priority 31 code and uses the cached vector */
    i960_nmi(&m->cpu);
    i960_run(&m->cpu, 100000);
    g_assert_cmpuint(m->cpu.r[G(5)], ==, 1);
    g_assert_cmpuint(m->cpu.r[G(3)], ==, 248);
    g_assert_false(m->cpu.block_nmi);
    g_free(m);
}

static void test_intctl_inten_intdis(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    emit(&a, RRR(OP_INTDIS, 0, 0, 0));
    emit(&a, LRR(OP_INTCTL, 2, 0, G(0)));            /* status: disabled = 0 */
    emit(&a, RRR(OP_INTEN, 0, 0, 0));
    emit(&a, LRR(OP_INTCTL, 2, 0, G(1)));            /* enabled = 1 */
    emit(&a, LRR(OP_INTCTL, 0, 0, G(2)));            /* disable; was 1 */
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(0)], ==, 0);
    g_assert_cmpuint(m->cpu.r[G(1)], ==, 1);
    g_assert_cmpuint(m->cpu.r[G(2)], ==, 1);
    g_assert_cmphex(m->cpu.icon & I960_ICON_GIE, ==, I960_ICON_GIE);
    g_free(m);
}

/* ------------------------------------------------------------------ */
/* Tests: boot, reinit, controls                                      */
/* ------------------------------------------------------------------ */

static void test_ibr_boot(void)
{
    Mach *m = mach_new();

    /* everything the boot reads sits in the ROM image, as on a board */
    memset(m->rom, 0, sizeof(m->rom));
    mach_setup(m, ROM_BASE + 0x100);
    /* relocate the tables into the ROM image */
    {
        uint32_t base = ROM_BASE + 0x1000;
        uint8_t *r = m->rom + 0x1000;

        stl_le_p(r + 0x00, base + 0x100);            /* fault table */
        stl_le_p(r + 0x04, base + 0x200);            /* control table */
        stl_le_p(r + 0x08, 0x1005);                  /* AC: OM|cc=101 */
        stl_le_p(r + 0x0c, 0);
        stl_le_p(r + 0x10, base + 0x400);            /* interrupt table */
        stl_le_p(r + 0x14, base + 0x800);            /* system proc table */
        stl_le_p(r + 0x1c, ISP_ADDR + 8);            /* unaligned ISP */
        stl_le_p(r + 0x24, 3u << 8);                 /* reserve 3 frames */
        stl_le_p(r + 0x200 + 0x10, 0x1234);          /* IMAP0 */
        stl_le_p(r + 0x200 + 0x1c, 0x44);            /* ICON */
        stl_le_p(r + 0x200 + 0x40, 0x0badf00d);      /* PMCON8_9 */
        stl_le_p(r + 0x200 + 0x68, 0x0000002);       /* TC */
        stl_le_p(r + 0x400 + 4 * 248 + 4, 0xfeed0000); /* NMI vector */
        stl_le_p(r + 0x800 + 12, SSP_ADDR | 1);      /* SSP, trace bit */
        write_ibr(m, ROM_BASE + 0x100, base);
        stl_le_p(m->rom + (I960_IBR_ADDR - ROM_BASE) + 0, 0x81);   /* PMCON */
    }
    m->cpu.device_id = 0x0082a013;
    i960_reset(&m->cpu);
    g_assert_false(m->cpu.failed);
    g_assert_cmphex(m->cpu.ip, ==, ROM_BASE + 0x100);
    g_assert_cmphex(m->cpu.pc, ==, 0x001f2002);
    g_assert_cmphex(m->cpu.ac, ==, 0x1005);
    g_assert_cmphex(m->cpu.r[I960_G0], ==, 0x0082a013);
    g_assert_cmphex(m->cpu.r[I960_FP], ==, ISP_ADDR);   /* aligned down */
    g_assert_cmphex(m->cpu.r[I960_SP], ==, ISP_ADDR + 64);
    g_assert_cmphex(m->cpu.isp, ==, ISP_ADDR + 8);
    g_assert_cmphex(m->cpu.ssp, ==, SSP_ADDR);
    g_assert_true(m->cpu.ssp_te);
    g_assert_cmphex(m->cpu.imap[0], ==, 0x1234);
    g_assert_cmphex(m->cpu.icon, ==, 0x44);
    g_assert_cmphex(m->cpu.tc, ==, 2);
    g_assert_cmpuint(m->cpu.rcache_reserved, ==, 3);
    g_assert_cmphex((uint32_t)ldl_le_p(m->cpu.data_ram), ==, 0xfeed0000);
    g_assert_cmphex(m->cpu.boot_pmcon14_15 & 0xff, ==, 0x81);
    g_assert_cmpuint(m->mmio_wr_n, >, 0);
    {
        bool seen = false;

        /* the unmodelled PMCON registers are written through to the device */
        for (unsigned i = 0; i < m->mmio_wr_n; i++) {
            if (m->mmio_wr_addr[i] == 0xff008620) {
                g_assert_cmphex(m->mmio_wr_val[i], ==, 0x0badf00d);
                seen = true;
            }
        }
        g_assert_true(seen);
    }
    g_free(m);
}

static void test_ibr_bad_checksum_and_alignment(void)
{
    Mach *m = mach_new();

    mach_setup(m, CODE);
    m->rom[I960_IBR_ADDR - ROM_BASE + 0x28] ^= 1;
    i960_reset(&m->cpu);
    g_assert_true(m->cpu.failed);
    g_assert_cmphex(m->fail_code, ==, 0xfeffff64);
    g_assert_cmpint(i960_run(&m->cpu, 1000), ==, 0);
    g_free(m);

    m = mach_new();
    mach_setup(m, CODE);
    wr32(m, PRCB + 0x10, INT_TABLE + 2);             /* misaligned table */
    i960_reset(&m->cpu);
    g_assert_true(m->cpu.failed);
    g_assert_cmphex(m->fail_code, ==, 0xfeffff70);
    g_free(m);
}

static void test_boot_from_rom_and_run(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    Asm sp = { m, HANDLERS + 0x400 };

    mach_setup(m, CODE);
    emit(&sp, LRR(OP_MOV, 7, 0, G(2)));
    emit(&sp, ctrl(C_RET, 0));
    emit(&a, LRR(OP_CALLS, 6, 0, 0));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(2)], ==, 7);
    g_free(m);
}

static void test_sysctl_reinit_and_mmr_modify(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    write_prcb(m, 0x6000, FAULT_TABLE, CTRL_TABLE, INT_TABLE, SPT, ISP_ADDR);
    wr32(m, 0x6008, 0x2);                            /* new AC image: cc=010 */
    ldc(&a, G(0), I960_MMR_IMSK & 0xffff0000);
    ldc(&a, G(1), (I960_MMR_IMSK << 16) | 0x500);    /* addr low half, type 5 */
    ldc(&a, G(2), 0xa5);
    ldc(&a, G(3), 0xf0);
    emit(&a, RRR(OP_SYSCTL, G(1), G(2), G(3)));      /* IMSK mask-modify */
    emit(&a, RRR(OP_MOV, G(3), 0, G(4)));
    ldc(&a, G(5), 0x300);                            /* type 3: reinit */
    ldc(&a, G(6), CODE + 0x1000);
    ldc(&a, G(7), 0x6000);
    emit(&a, RRR(OP_SYSCTL, G(5), G(6), G(7)));
    HALT2(&a);                                       /* never reached */
    {
        Asm n = { m, CODE + 0x1000 };

        HALT2(&n);
    }
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(m->cpu.imsk, ==, 0xa0);   /* modified, kept over reinit */
    g_assert_cmphex(m->cpu.prcb, ==, 0x6000);
    g_assert_cmpuint(m->reinit_calls, ==, 1);
    g_assert_cmphex(m->cpu.ac & 7, ==, 2);
    /* globals survive a re-init; only a cold reset sets g0 = device ID */
    g_assert_cmphex(m->cpu.r[I960_G0], ==, I960_MMR_IMSK & 0xffff0000);
    g_assert_cmphex(m->cpu.r[G(2)], ==, 0xa5);
    g_free(m);
}

static void test_reinit_keeps_globals(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    write_prcb(m, 0x6000, FAULT_TABLE, CTRL_TABLE, INT_TABLE, SPT,
               ISP_ADDR + 0x8008);
    for (unsigned n = 0; n <= 14; n++) {
        ldc(&a, G(n), pat(n));
    }
    ldc(&a, R(4), 0x300);                            /* type 3: reinit */
    ldc(&a, R(5), CODE + 0x1000);
    ldc(&a, R(6), 0x6000);
    emit(&a, RRR(OP_SYSCTL, R(4), R(5), R(6)));
    HALT2(&a);                                       /* never reached */
    {
        Asm n = { m, CODE + 0x1000 };

        HALT2(&n);
    }
    mach_boot(m);
    g_assert_cmphex(m->cpu.r[I960_G0], ==, 0x0082a013);   /* cold reset */
    mach_run(m);
    g_assert_cmpuint(m->reinit_calls, ==, 1);
    for (unsigned n = 0; n <= 14; n++) {
        g_assert_cmphex(m->cpu.r[G(n)], ==, pat(n));
    }
    g_assert_cmphex(m->cpu.r[I960_FP], ==, ISP_ADDR + 0x8000);
    g_assert_cmphex(m->cpu.r[I960_SP], ==, ISP_ADDR + 0x8000 + 64);
    g_assert_cmphex(m->cpu.r[I960_PFP], ==, ISP_ADDR + 0x8000);
    g_assert_cmphex(m->cpu.r[R(5)], ==, CODE + 0x1000);   /* locals kept */
    g_free(m);
}

/*
 * Rx manual example 11-2 (p. 11-18): Process_PRCB resets the data RAM state
 * but notes it is unpredictable whether the RAM keeps its contents.  Observed
 * on a shipped i960 RD boot ROM: state left at data RAM 0x40 is read after
 * its sysctl reinitialisation, so real silicon keeps it.
 */
static void test_reinit_keeps_data_ram(void)
{
    Mach *m = mach_new();

    mach_setup(m, CODE);
    write_prcb(m, 0x6000, FAULT_TABLE, CTRL_TABLE, INT_TABLE, SPT, ISP_ADDR);
    mach_boot(m);
    m->cpu.data_ram[0x40] = 0xcd;                    /* set before reinit */
    m->cpu.data_ram[0x41] = 0xab;
    m->cpu.data_ram[0x42] = 0x34;
    m->cpu.data_ram[0x43] = 0x12;
    m->cpu.ipnd = 0x20;                              /* kept over reinit */
    m->cpu.imsk = 0x20;
    m->cpu.pc = I960_PC_EM;                          /* reinit restores PC */
    {
        Asm b = { m, CODE };

        ldc(&b, G(5), 0x300);
        ldc(&b, G(6), CODE + 0x1000);
        ldc(&b, G(7), 0x6000);
        emit(&b, RRR(OP_SYSCTL, G(5), G(6), G(7)));
        HALT2(&b);
        Asm n = { m, CODE + 0x1000 };

        HALT2(&n);
    }
    m->cpu.ip = CODE;
    i960_run(&m->cpu, 100000);
    g_assert_cmphex(ldl_le_p(&m->cpu.data_ram[0x40]), ==, 0x1234abcd);
    g_assert_cmphex(m->cpu.ipnd, ==, 0x20);
    g_assert_cmphex(m->cpu.imsk, ==, 0x20);
    g_assert_cmphex(m->cpu.pc, ==, 0x001f2002);
    /* a cold reset clears the RAM and IPND (undefined in the manual) */
    i960_reset(&m->cpu);
    g_assert_cmphex(ldl_le_p(&m->cpu.data_ram[0x40]), ==, 0);
    g_assert_cmphex(m->cpu.ipnd, ==, 0);
    g_free(m);
}

static void test_cache_control_ops(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    emit(&a, LRR(OP_ICCTL, 1, 0, G(0)));             /* enable */
    emit(&a, LRR(OP_ICCTL, 4, 0, G(1)));             /* status bit 0 */
    emit(&a, LRR(OP_DCCTL, 1, 0, G(2)));
    emit(&a, LRR(OP_DCCTL, 4, 0, G(3)));
    emit(&a, LRR(OP_DCCTL, 7, 0, G(4)));             /* reserved */
    HALT2(&a);
    gen_fault_handler(m, HANDLERS, true);
    all_faults_to(m, HANDLERS);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(1)], ==, 1);
    g_assert_cmpuint(m->cpu.r[G(3)], ==, 1);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 1);
    g_assert_cmphex(m->cpu.r[G(8)], ==, (I960_FT_OPERATION << 16) |
                                        I960_FS_OP_INVALID_OPERAND);
    g_free(m);

    /* a device hook that claims the operation */
    m = mach_new();
    a = (Asm){ m, CODE };
    mach_setup(m, CODE);
    m->ops.ctl = m_ctl;
    m->ctl_handled = true;
    m->ctl_ret = 0xc0ffee;
    emit(&a, LRR(OP_SYSCTL, 0x7, 0, G(0)));          /* device-defined type */
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->ctl_last_kind, ==, I960_CTL_SYSCTL);
    g_assert_cmphex(m->cpu.r[G(0)], ==, 0xc0ffee);
    g_free(m);
}

/* ------------------------------------------------------------------ */
/* Tests: idle, trace, accounting                                     */
/* ------------------------------------------------------------------ */

static void test_halt_idles_and_interrupt_wakes(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_int_handler(m, HANDLERS);
    set_vector(m, 0x31, HANDLERS);
    prelude(&a, 0);
    emit(&a, LRR(OP_HALT, 2, 0, 0));
    emit(&a, LRR(OP_MOV, 5, 0, G(1)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);                                     /* first halt */
    g_assert_cmpuint(m->cpu.r[G(1)], ==, 0);
    g_assert_cmpint(i960_run(&m->cpu, 1000), ==, 0); /* nothing to do */
    g_assert_true(m->cpu.idle_hint);
    i960_post_interrupt(&m->cpu, 0x31);
    g_assert_false(m->cpu.idle_hint);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(6)], ==, 1);
    g_assert_cmpuint(m->cpu.r[G(1)], ==, 5);         /* resumed after halt */
    g_free(m);
}

static void test_spin_loop_idle_hint(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    uint32_t loop;

    mach_setup(m, CODE);
    loop = a.pc;
    emit(&a, memb(M_LD, G(1), 0, 0xc, 0, 0));
    emit(&a, POLL_ADDR);
    emit(&a, cobr(C_CMPIBE, 0, true, G(1), loop - a.pc));   /* while zero */
    HALT2(&a);
    mach_boot(m);
    m->poll_val = 0;
    i960_run(&m->cpu, 1000000);
    g_assert_true(m->cpu.idle_hint);
    g_assert_false(m->cpu.halted);
    g_assert_cmpuint(m->poll_reads, <, 1000);
    m->poll_val = 1;
    i960_set_xint(&m->cpu, 0);
    i960_run(&m->cpu, 1000000);
    g_assert_true(m->cpu.halted);
    g_free(m);
}

static void test_trace_instruction(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, false);
    /* trace faults need a system-supervisor call so PC.te is cleared */
    wr32(m, SPT + 48 + 4 * 7, HANDLERS | 2);
    set_fault_sys(m, I960_FT_TRACE, 7);
    prelude(&a, 0);
    ldc(&a, G(0), I960_TC_INSN);
    emit(&a, RRR(OP_MODTC, G(0), G(0), G(1)));       /* instruction trace */
    ldc(&a, G(2), I960_PC_TE);
    ldc(&a, G(9), I960_PC_TE);
    emit(&a, RRR(OP_MODPC, G(2), G(2), G(9)));       /* enable: not traced */
    emit(&a, LRR(OP_ADDO, 1, G(5), G(5)));           /* traced */
    emit(&a, LRR(OP_ADDO, 1, G(5), G(5)));           /* traced */
    emit(&a, LRR(OP_MOV, 0, 0, G(3)));               /* traced */
    emit(&a, LRR(OP_MOV, 0, 0, G(3)));               /* traced (modpc below) */
    ldc(&a, G(4), 0);
    emit(&a, RRR(OP_MODPC, G(2), G(2), G(4)));       /* disable: traced */
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    /* add, add, mov, mov, the lda in ldc, and the modpc that turns it off */
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 6);
    g_assert_cmphex(m->cpu.r[G(8)], ==, (I960_FT_TRACE << 16) |
                                        I960_FS_TRACE_INSN);
    g_free(m);
}

static void test_fault_operands_and_overflow_stores(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, false);
    gen_fault_handler(m, HANDLERS + 0x100, true);
    all_faults_to(m, HANDLERS);
    set_fault_entry(m, I960_FT_OPERATION, HANDLERS + 0x100);
    ldc(&a, G(0), 0x40000);
    emit(&a, mema(M_LDL, R(5), G(0), 0, true));      /* odd register group */
    emit(&a, LRR(OP_MOV, 1, 0, R(2)));               /* write to the RIP */
    emit(&a, memb(M_LDQ, R(6), G(0), 4, 0, 0));      /* quad at r6: unaligned */
    ldc(&a, G(1), 200);
    emit(&a, mema(M_STIB, G(1), G(0), 0x10, true));  /* does not fit a byte */
    ldc(&a, G(2), 0xc8);
    emit(&a, LRR(OP_MODAC, 0, 0, G(3)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    /* ldl odd, mov r2, ldq r6: three INVALID_OPERAND; stib: OVERFLOW */
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 4);
    g_assert_cmphex(m->cpu.r[G(8)], ==, (I960_FT_ARITHMETIC << 16) |
                                        I960_FS_ARITH_OVERFLOW);
    g_assert_cmphex(m->ram[0x40010], ==, 200);       /* stored regardless */
    g_free(m);

    m = mach_new();
    a = (Asm){ m, CODE };
    mach_setup(m, CODE);
    ldc(&a, G(0), 0x20000001);
    ldc(&a, G(4), I960_AC_OM);
    emit(&a, RRR(OP_MODAC, G(4), G(4), G(5)));       /* mask overflow */
    emit(&a, LRR(OP_SHLI, 2, G(0), G(1)));           /* overflows */
    emit(&a, LLR(OP_MODAC, 0, 0, G(2)));
    ldc(&a, G(3), 0x40000);
    ldc(&a, G(6), 200);
    emit(&a, mema(M_STIS, G(6), G(3), 0x10, true));  /* fits a short */
    emit(&a, mema(M_STIB, G(6), G(3), 0x14, true));  /* does not fit a byte */
    emit(&a, LLR(OP_MODAC, 0, 0, G(7)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmphex(m->cpu.r[G(1)], ==, 0x40000002);   /* as far as it fits */
    g_assert_cmphex(m->cpu.r[G(2)] & I960_AC_OF, ==, I960_AC_OF);
    g_assert_cmphex(m->cpu.r[G(7)] & I960_AC_OF, ==, I960_AC_OF);
    g_free(m);
}

static void test_trace_mark_and_branch(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, false);
    wr32(m, SPT + 48 + 4 * 7, HANDLERS | 2);
    set_fault_sys(m, I960_FT_TRACE, 7);
    prelude(&a, 0);
    ldc(&a, G(0), I960_TC_BRANCH | I960_TC_MARK);
    emit(&a, RRR(OP_MODTC, G(0), G(0), G(1)));
    ldc(&a, G(2), I960_PC_TE);
    ldc(&a, G(9), I960_PC_TE);
    emit(&a, RRR(OP_MODPC, G(2), G(2), G(9)));
    emit(&a, ctrl(C_B, 4));                          /* branch trace */
    emit(&a, RRR(OP_MARK, 0, 0, 0));                 /* mark trace */
    emit(&a, RRR(OP_FMARK, 0, 0, 0));                /* forced mark */
    ldc(&a, G(4), 0);
    emit(&a, RRR(OP_MODPC, G(2), G(2), G(4)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 3);
    g_assert_cmphex(m->cpu.r[G(8)], ==, (I960_FT_TRACE << 16) |
                                        I960_FS_TRACE_MARK);
    g_free(m);
}

static void test_bbc_bbs_condition_codes(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    ldc(&a, G(0), 0x10);                     /* bit 4 set, bit 3 clear */
    emit(&a, cobr(C_BBC, 3, true, G(0), 8));         /* clear: taken, cc=000 */
    emit(&a, LRR(OP_MOV, 9, 0, G(9)));
    emit(&a, LLR(OP_MODAC, 0, 0, G(1)));
    emit(&a, cobr(C_BBC, 4, true, G(0), 8));     /* set: not taken, cc=010 */
    emit(&a, LLR(OP_MODAC, 0, 0, G(2)));
    emit(&a, cobr(C_BBS, 4, true, G(0), 8));         /* set: taken, cc=010 */
    emit(&a, LRR(OP_MOV, 9, 0, G(10)));
    emit(&a, LLR(OP_MODAC, 0, 0, G(3)));
    emit(&a, cobr(C_BBS, 3, true, G(0), 8));     /* clear: not taken, cc=000 */
    emit(&a, LLR(OP_MODAC, 0, 0, G(4)));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(9)], ==, 0);
    g_assert_cmpuint(m->cpu.r[G(10)], ==, 0);
    g_assert_cmpuint(m->cpu.r[G(1)] & 7, ==, 0);
    g_assert_cmpuint(m->cpu.r[G(2)] & 7, ==, 2);
    g_assert_cmpuint(m->cpu.r[G(3)] & 7, ==, 2);
    g_assert_cmpuint(m->cpu.r[G(4)] & 7, ==, 0);
    g_free(m);
}

static void test_countdown_is_not_idle(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    uint32_t loop;

    mach_setup(m, CODE);
    ldc(&a, G(1), 5000);
    loop = a.pc;
    emit(&a, LRR(OP_SUBO, 1, G(1), G(1)));
    emit(&a, cobr(C_CMPOBNE, 0, true, G(1), loop - a.pc));
    HALT2(&a);
    mach_boot(m);
    while (!m->cpu.halted) {
        i960_run(&m->cpu, 100);
        g_assert_false(m->cpu.idle_hint && !m->cpu.halted);
    }
    g_assert_cmpuint(m->cpu.r[G(1)], ==, 0);
    g_free(m);

    /* b . waiting for an interrupt is idle */
    m = mach_new();
    a = (Asm){ m, CODE };
    mach_setup(m, CODE);
    emit(&a, ctrl(C_B, 0));
    mach_boot(m);
    i960_run(&m->cpu, 100000);
    g_assert_true(m->cpu.idle_hint);
    g_free(m);
}

static void test_ipnd_reads_level_pins(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    wr32(m, CTRL_TABLE + 0x1c, 0);                   /* dedicated, level */
    ldc(&a, G(0), I960_MMR_IPND);
    emit(&a, mema(M_LD, G(1), G(0), 0, true));
    HALT2(&a);
    mach_boot(m);
    i960_set_xint(&m->cpu, 0x80);                    /* XINT7 asserted */
    i960_run(&m->cpu, 1000);
    g_assert_cmphex(m->cpu.r[G(1)], ==, 0x80);       /* latched ipnd is 0 */
    g_assert_cmphex(m->cpu.ipnd, ==, 0);
    g_free(m);
}

static void test_data_ram_write_protection(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, true);
    all_faults_to(m, HANDLERS);
    wr32(m, CTRL_TABLE + 0x6c, I960_BCON_IRP);
    prelude(&a, 0);
    ldc(&a, G(0), 0x100);
    ldc(&a, G(1), 0x1234);
    emit(&a, mema(M_ST, G(1), G(0), 0, true));       /* supervisor: ok */
    emit(&a, mema(M_ST, G(1), G(0), 0x4, false));    /* addr 4, supervisor ok */
    ldc(&a, G(12), I960_PC_EM);
    ldc(&a, G(13), 0);
    emit(&a, RRR(OP_MODPC, G(12), G(12), G(13)));    /* to user mode */
    emit(&a, mema(M_ST, G(1), G(0), 0, true));       /* irp: TYPE.MISMATCH */
    emit(&a, mema(M_ST, G(1), 0, 8, false));         /* first 64: mismatch */
    emit(&a, ctrl(C_B, 0));
    mach_boot(m);
    i960_run(&m->cpu, 100000);
    g_assert_cmphex(ldl_le_p(m->cpu.data_ram + 0x100), ==, 0x1234);
    g_assert_cmphex(ldl_le_p(m->cpu.data_ram + 4), ==, 0x1234);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, 2);
    g_assert_cmphex(m->cpu.r[G(8)], ==, (I960_FT_TYPE << 16) |
                                        I960_FS_TYPE_MISMATCH);
    g_free(m);
}

/*
 * BCON.sirp (bit 2) protects the first 64 bytes from supervisor writes on
 * every part; bit 0 is CTV and protects nothing.
 */
static void bcon_case(I960Variant v, uint32_t bcon, bool protect)
{
    Mach *m = mach_new_variant(v);
    Asm a = { m, CODE };

    mach_setup(m, CODE);
    gen_fault_handler(m, HANDLERS, true);
    all_faults_to(m, HANDLERS);
    wr32(m, CTRL_TABLE + 0x6c, bcon);
    ldc(&a, G(1), 0x55);
    emit(&a, mema(M_ST, G(1), 0, 0x10, false));
    emit(&a, mema(M_ST, G(1), 0, 0x80, false));
    HALT2(&a);
    mach_boot(m);
    mach_run(m);
    g_assert_cmpuint(m->cpu.r[G(7)], ==, protect ? 1 : 0);
    g_assert_cmphex(ldl_le_p(m->cpu.data_ram + 0x10), ==, protect ? 0 : 0x55);
    g_assert_cmphex(ldl_le_p(m->cpu.data_ram + 0x80), ==, 0x55);
    g_free(m);
}

static void test_bcon_layouts(void)
{
    for (int v = I960_VARIANT_JX; v <= I960_VARIANT_RX; v++) {
        bcon_case(v, I960_BCON_CTV, false);     /* an RD boot ROM's BCON = 1 */
        bcon_case(v, I960_BCON_SIRP, true);
    }
}

static void test_nmi_blocks_priority_31(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    Asm n = { m, HANDLERS + 0x100 };

    mach_setup(m, CODE);
    gen_int_handler(m, HANDLERS);
    set_vector(m, 0xf9, HANDLERS);                   /* priority 31 */
    set_vector(m, 248, HANDLERS + 0x100);
    /* the NMI handler posts a priority-31 vector, then records g6 */
    ldc(&n, G(0), 0xf9);
    emit(&n, RRR(OP_SYSCTL, G(0), G(0), G(0)));
    emit(&n, RRR(OP_MOV, G(6), 0, G(1)));            /* not yet serviced */
    emit(&n, ctrl(C_RET, 0));
    prelude(&a, 0);
    emit(&a, ctrl(C_B, 0));
    mach_boot(m);
    i960_run(&m->cpu, 1000);
    i960_nmi(&m->cpu);
    i960_run(&m->cpu, 10000);
    g_assert_cmpuint(m->cpu.r[G(1)], ==, 0);         /* waited during NMI */
    g_assert_cmpuint(m->cpu.r[G(6)], ==, 1);         /* ran after the return */
    g_free(m);
}

/* ------------------------------------------------------------------ */
/* Migration                                                          */
/* ------------------------------------------------------------------ */

static int vmstate_roundtrip(I960State *from, I960State *to)
{
    char *path = NULL;
    int fd = g_file_open_tmp("test-i960-XXXXXX", &path, NULL);
    QIOChannel *ioc;
    QEMUFile *f;
    Error *err = NULL;
    int ret;

    g_assert_cmpint(fd, >=, 0);
    ioc = QIO_CHANNEL(qio_channel_file_new_fd(dup(fd)));
    f = qemu_file_new_output(ioc);
    object_unref(OBJECT(ioc));
    g_assert_cmpint(vmstate_save_state(f, &vmstate_i960, from, NULL, &err),
                    ==, 0);
    qemu_fclose(f);
    lseek(fd, 0, SEEK_SET);
    ioc = QIO_CHANNEL(qio_channel_file_new_fd(dup(fd)));
    f = qemu_file_new_input(ioc);
    object_unref(OBJECT(ioc));
    ret = vmstate_load_state(f, &vmstate_i960, to, 1, &err);
    if (ret) {
        error_free(err);
    }
    qemu_fclose(f);
    close(fd);
    unlink(path);
    g_free(path);
    return ret;
}

static void test_migration_roundtrip(void)
{
    Mach *a = mach_new();
    Mach *b = mach_new();
    Asm p = { a, CODE };
    Asm f = { a, HANDLERS };
    uint32_t loop;

    mach_setup(a, CODE);
    emit(&f, LRR(OP_ADDO, 1, G(2), G(2)));
    emit(&f, ctrl(C_RET, 0));
    prelude(&p, 0);
    ldc(&p, G(1), 300);
    loop = p.pc;
    emit(&p, RRR(OP_ADDO, G(1), G(0), G(0)));
    emit(&p, mema(M_ST, G(0), 0, 0x400, false));
    ldc(&p, R(4), 0x1234);
    emit(&p, ctrl(C_CALL, HANDLERS - p.pc));
    emit(&p, LRR(OP_SUBO, 1, G(1), G(1)));
    emit(&p, cobr(C_CMPOBNE, 0, true, G(1), loop - p.pc));
    HALT2(&p);
    mach_boot(a);
    i960_run(&a->cpu, 700);                          /* mid-loop */
    while (a->cpu.ip != HANDLERS + 4) {              /* and inside the call */
        i960_run(&a->cpu, 1);
    }
    g_assert_false(a->cpu.halted);
    g_assert_cmpuint(a->cpu.rcache_n, >, 0);

    memcpy(b->ram, a->ram, sizeof(a->ram));
    memcpy(b->rom, a->rom, sizeof(a->rom));
    g_assert_cmpint(vmstate_roundtrip(&a->cpu, &b->cpu), ==, 0);
    g_assert_true(b->cpu.ops == &b->ops);            /* wiring untouched */
    g_assert_cmpuint(b->cpu.variant, ==, I960_VARIANT_RX);

    mach_run(a);
    mach_run(b);
    g_assert_cmpuint(b->cpu.r[G(2)], ==, 300);
    for (int i = 0; i < 32; i++) {
        g_assert_cmphex(b->cpu.r[i], ==, a->cpu.r[i]);
    }
    g_assert_cmphex(b->cpu.ac, ==, a->cpu.ac);
    g_assert_cmphex(b->cpu.pc, ==, a->cpu.pc);
    g_assert_cmphex(b->cpu.ip, ==, a->cpu.ip);
    g_assert_cmpuint(b->cpu.cycles, ==, a->cpu.cycles);
    g_assert_cmpint(memcmp(a->ram, b->ram, sizeof(a->ram)), ==, 0);
    g_free(a);
    g_free(b);
}

static void test_migration_rejects(void)
{
    Mach *a = mach_new();
    Mach *b = mach_new();

    mach_setup(a, CODE);
    mach_boot(a);
    a->cpu.rcache_n = I960_RCACHE_FRAMES + 1;
    g_assert_cmpint(vmstate_roundtrip(&a->cpu, &b->cpu), <, 0);
    a->cpu.rcache_n = 0;
    a->cpu.rcache_reserved = 8;
    g_assert_cmpint(vmstate_roundtrip(&a->cpu, &b->cpu), <, 0);
    a->cpu.rcache_reserved = 0;
    a->cpu.sw_prio = 32;
    g_assert_cmpint(vmstate_roundtrip(&a->cpu, &b->cpu), <, 0);
    g_free(a);
    g_free(b);
}

static void test_cycle_accounting(void)
{
    Mach *m = mach_new();
    Asm a = { m, CODE };
    int64_t used;

    mach_setup(m, CODE);
    emit(&a, LRR(OP_MOV, 1, 0, G(0)));
    emit(&a, LRR(OP_ADDO, 1, G(0), G(0)));
    emit(&a, LRR(OP_DIVO, 3, G(0), G(1)));
    HALT2(&a);
    mach_boot(m);
    used = i960_run(&m->cpu, 1000);
    g_assert_cmpint(used, >, 40);                    /* the divide dominates */
    g_assert_cmpuint(m->cpu.cycles, ==, (uint64_t)used);
    g_free(m);
}

int main(int argc, char **argv)
{
    module_call_init(MODULE_INIT_QOM);
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/i960/reg/logic", test_reg_logic);
    g_test_add_func("/i960/reg/arith", test_reg_arith);
    g_test_add_func("/i960/reg/bits-compares", test_reg_bits_and_compares);
    g_test_add_func("/i960/reg/carry-cond", test_reg_extended_carry_cond);
    g_test_add_func("/i960/reg/move-groups", test_reg_move_groups);
    g_test_add_func("/i960/reg/atomics", test_atomics);
    g_test_add_func("/i960/cobr/loop", test_cobr_loop);
    g_test_add_func("/i960/cobr/misc", test_cobr_misc);
    g_test_add_func("/i960/ctrl/branches", test_ctrl_branches);
    g_test_add_func("/i960/mem/sizes-modes", test_mem_sizes_and_modes);
    g_test_add_func("/i960/mem/groups-bursts", test_mem_groups_and_bursts);
    g_test_add_func("/i960/mem/unaligned", test_mem_unaligned);
    g_test_add_func("/i960/call/frames", test_call_ret_frames);
    g_test_add_func("/i960/call/spill-fill", test_deep_calls_spill_fill);
    g_test_add_func("/i960/call/flushreg", test_flushreg);
    g_test_add_func("/i960/call/preserves-r4-r15", test_call_preserves_r4_r15);
    g_test_add_func("/i960/call/deep-preserves-r15",
                    test_deep_calls_preserve_r15);
    g_test_add_func("/i960/call/callx-register", test_callx_register_target);
    g_test_add_func("/i960/call/frame-layout", test_frame_layout);
    g_test_add_func("/i960/call/calls-modes", test_calls_modes);
    g_test_add_func("/i960/modpc-mask-is-src2", test_modpc_mask_is_src2);
    g_test_add_func("/i960/modpc-user-mode", test_modpc_user_mode);
    g_test_add_func("/i960/mod-operand-order", test_mod_operand_order);
    g_test_add_func("/i960/call/calls-length", test_calls_protection_length);
    g_test_add_func("/i960/fault/zero-divide", test_fault_zero_divide);
    g_test_add_func("/i960/fault/overflow", test_fault_integer_overflow);
    g_test_add_func("/i960/fault/opcode-range",
                    test_fault_invalid_opcode_and_range);
    g_test_add_func("/i960/fault/type-mismatch",
                    test_fault_type_mismatch_and_mmr);
    g_test_add_func("/i960/fault/mmr-rules", test_mmr_access_rules);
    g_test_add_func("/i960/int/sysctl", test_sw_interrupt_sysctl);
    g_test_add_func("/i960/int/pending-priority",
                    test_sw_interrupt_pending_until_priority_drops);
    g_test_add_func("/i960/int/pending-bits", test_sw_interrupt_pending_bits);
    g_test_add_func("/i960/int/nested", test_nested_interrupt);
    g_test_add_func("/i960/int/xint-dedicated", test_xint_dedicated);
    g_test_add_func("/i960/int/xint-expanded-nmi", test_xint_expanded_and_nmi);
    g_test_add_func("/i960/int/intctl", test_intctl_inten_intdis);
    g_test_add_func("/i960/boot/ibr", test_ibr_boot);
    g_test_add_func("/i960/boot/ibr-errors",
                    test_ibr_bad_checksum_and_alignment);
    g_test_add_func("/i960/boot/run", test_boot_from_rom_and_run);
    g_test_add_func("/i960/ctl/reinit-mmr", test_sysctl_reinit_and_mmr_modify);
    g_test_add_func("/i960/ctl/reinit-keeps-globals",
                    test_reinit_keeps_globals);
    g_test_add_func("/i960/ctl/reinit-keeps-data-ram",
                    test_reinit_keeps_data_ram);
    g_test_add_func("/i960/ctl/cache", test_cache_control_ops);
    g_test_add_func("/i960/idle/halt", test_halt_idles_and_interrupt_wakes);
    g_test_add_func("/i960/idle/spin", test_spin_loop_idle_hint);
    g_test_add_func("/i960/trace/insn", test_trace_instruction);
    g_test_add_func("/i960/trace/mark-branch", test_trace_mark_and_branch);
    g_test_add_func("/i960/fault/operands-overflow",
                    test_fault_operands_and_overflow_stores);
    g_test_add_func("/i960/cobr/bb-cc", test_bbc_bbs_condition_codes);
    g_test_add_func("/i960/idle/countdown", test_countdown_is_not_idle);
    g_test_add_func("/i960/int/ipnd-level", test_ipnd_reads_level_pins);
    g_test_add_func("/i960/mem/data-ram-protect",
                    test_data_ram_write_protection);
    g_test_add_func("/i960/int/nmi-blocks-31", test_nmi_blocks_priority_31);
    g_test_add_func("/i960/mem/bcon-layouts", test_bcon_layouts);
    g_test_add_func("/i960/migration/roundtrip", test_migration_roundtrip);
    g_test_add_func("/i960/migration/reject", test_migration_rejects);
    g_test_add_func("/i960/cycles", test_cycle_accounting);
    return g_test_run();
}
