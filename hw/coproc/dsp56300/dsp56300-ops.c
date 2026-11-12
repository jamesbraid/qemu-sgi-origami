/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * DSP56300 core: parallel moves, program control, loops, bit manipulation
 * and the move instructions. The opcode patterns are in dsp_ctl_insns[]; a
 * letter in a pattern is a field, 0 and 1 are fixed bits.
 *
 * Written from the NXP DSP56300 Family Manual, rev 5, and nothing else.
 */
#include "qemu/osdep.h"
#include "dsp56300-int.h"

#define F(op, hi, lo) (((op) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

#define X_SPACE DSP56300_SPACE_X
#define Y_SPACE DSP56300_SPACE_Y
#define P_SPACE DSP56300_SPACE_P

#define IO_PP 0xffffc0u
#define IO_QQ 0xffff80u

/* ---- operand locations ------------------------------------------------- */

typedef enum LocKind { LOC_MEM, LOC_REG, LOC_IMM } LocKind;

typedef struct Loc {
    LocKind kind;
    DSP56300Space space;
    uint32_t addr;                      /* address, register or immediate */
} Loc;

static bool loc_ea(DSP56300State *s, unsigned int ea, DSP56300Space sp,
                   Loc *l)
{
    uint32_t v;
    bool imm;

    if (!dsp_ea(s, ea, &v, &imm)) {
        return false;
    }
    l->kind = imm ? LOC_IMM : LOC_MEM;
    l->space = sp;
    l->addr = v;
    return true;
}

static void loc_mem(Loc *l, DSP56300Space sp, uint32_t addr)
{
    l->kind = LOC_MEM;
    l->space = sp;
    l->addr = addr;
}

static void loc_reg(Loc *l, unsigned int reg)
{
    l->kind = LOC_REG;
    l->addr = reg;
}

static uint32_t loc_read(DSP56300State *s, const Loc *l)
{
    switch (l->kind) {
    case LOC_MEM:
        return dsp_read(s, l->space, l->addr);
    case LOC_REG:
        return dsp_reg_read(s, l->addr);
    default:
        return l->addr;
    }
}

static void loc_write(DSP56300State *s, const Loc *l, uint32_t v)
{
    if (l->kind == LOC_MEM) {
        dsp_write(s, l->space, l->addr, v);
    } else if (l->kind == LOC_REG) {
        dsp_reg_write(s, l->addr, v);
    }
}

/* ---- parallel moves ---------------------------------------------------- */

typedef struct PMWrite {
    bool mem;
    DSP56300Space space;
    uint16_t reg;
    uint32_t addr, val;
} PMWrite;

typedef struct PMove {
    PMWrite w[4];
    unsigned int n;
    bool ifcc, ifu;
    unsigned int cc;
} PMove;

static void pm_reg(PMove *m, unsigned int reg, uint32_t val)
{
    m->w[m->n++] = (PMWrite){ .reg = reg, .val = val };
}

static void pm_mem(PMove *m, DSP56300Space sp, uint32_t addr, uint32_t val)
{
    m->w[m->n++] = (PMWrite){ .mem = true, .space = sp, .addr = addr,
                              .val = val };
}

static void pm_apply(DSP56300State *s, const PMove *m)
{
    for (unsigned int i = 0; i < m->n; i++) {
        const PMWrite *w = &m->w[i];

        if (w->mem) {
            dsp_write(s, w->space, w->addr, w->val);
        } else {
            dsp_reg_write(s, w->reg, w->val);
        }
    }
}

/* Registers that take an 8-bit short immediate as a fraction. */
static bool reg_is_fraction(unsigned int r)
{
    return (r >= DSP56300_REG_X0 && r <= DSP56300_REG_Y1) ||
           r == DSP56300_REG_A || r == DSP56300_REG_B;
}

/* A register move: the 5-bit register field must name a data register. */
static bool reg5_valid(unsigned int r)
{
    return r >= DSP56300_REG_X0 && r < 0x20;
}

/* Memory operand of a parallel move: X:, Y: with an ea or absolute short. */
static bool pm_mem_loc(DSP56300State *s, uint32_t op, DSP56300Space sp,
                       Loc *l)
{
    unsigned int field = F(op, 13, 8);

    if (F(op, 14, 14)) {
        return loc_ea(s, field, sp, l);
    }
    loc_mem(l, sp, field);
    return true;
}

static bool pm_xy_move(DSP56300State *s, uint32_t op, PMove *m)
{
    DSP56300Space sp = F(op, 19, 19) ? Y_SPACE : X_SPACE;
    unsigned int reg = (F(op, 21, 20) << 3) | F(op, 18, 16);
    bool write_reg = F(op, 15, 15);
    Loc l;

    if (!reg5_valid(reg) || !pm_mem_loc(s, op, sp, &l)) {
        return false;
    }
    if (write_reg) {
        pm_reg(m, reg, loc_read(s, &l));
    } else if (l.kind == LOC_IMM) {
        return false;
    } else {
        pm_mem(m, sp, l.addr, dsp_reg_read(s, reg));
    }
    return true;
}

/* L: moves two registers or accumulators to and from X:ea and Y:ea. */
static bool pm_long_move(DSP56300State *s, uint32_t op, PMove *m)
{
    unsigned int lll = (F(op, 19, 19) << 2) | F(op, 17, 16);
    bool write_reg = F(op, 15, 15);
    Loc l;
    uint32_t xv, yv;
    static const uint8_t first[8] = {
        DSP56300_REG_A1, DSP56300_REG_B1, DSP56300_REG_X1, DSP56300_REG_Y1,
        DSP56300_REG_A, DSP56300_REG_B, DSP56300_REG_A, DSP56300_REG_B,
    };
    static const uint8_t second[8] = {
        DSP56300_REG_A0, DSP56300_REG_B0, DSP56300_REG_X0, DSP56300_REG_Y0,
        DSP56300_REG_A0, DSP56300_REG_B0, DSP56300_REG_B, DSP56300_REG_A,
    };

    if (!pm_mem_loc(s, op, X_SPACE, &l) || l.kind == LOC_IMM) {
        return false;
    }
    if (write_reg) {
        xv = dsp_read(s, X_SPACE, l.addr);
        yv = dsp_read(s, Y_SPACE, l.addr);
        /*
         * For A and B the first write sets A2 from the sign of the word
         * and clears A0, the second then loads A0.
         */
        pm_reg(m, first[lll], xv);
        pm_reg(m, second[lll], yv);
        return true;
    }
    switch (lll) {
    case 4:
        dsp_acc_bus48(s, 0, &xv, &yv);
        break;
    case 5:
        dsp_acc_bus48(s, 1, &xv, &yv);
        break;
    case 6:
        xv = dsp_acc_bus(s, 0);
        yv = dsp_acc_bus(s, 1);
        break;
    case 7:
        xv = dsp_acc_bus(s, 1);
        yv = dsp_acc_bus(s, 0);
        break;
    default:
        xv = dsp_reg_read(s, first[lll]);
        yv = dsp_reg_read(s, second[lll]);
        break;
    }
    pm_mem(m, X_SPACE, l.addr, xv);
    pm_mem(m, Y_SPACE, l.addr, yv);
    return true;
}

static bool pm_i_r_u(DSP56300State *s, uint32_t op, PMove *m)
{
    unsigned int d5 = F(op, 20, 16);

    if (F(op, 20, 18) != 0) {               /* #xx,D */
        uint32_t imm = F(op, 15, 8);

        pm_reg(m, d5, reg_is_fraction(d5) ? imm << 16 : imm);
        return true;
    }
    switch (F(op, 17, 13)) {
    case 0:                                 /* no move */
        return true;
    case 1:                                 /* IFcc, IFcc.U */
        m->ifcc = true;
        m->ifu = F(op, 12, 12);
        m->cc = F(op, 11, 8);
        return true;
    case 2:
        {                               /* address register update */
        uint32_t addr;
        bool imm;

        return dsp_ea(s, F(op, 12, 8), &addr, &imm);
    }
    default:
        {                              /* S,D */
        unsigned int e5 = F(op, 17, 13), dd = F(op, 12, 8);

        if (!reg5_valid(e5) || !reg5_valid(dd)) {
            return false;
        }
        pm_reg(m, dd, dsp_reg_read(s, e5));
        return true;
    }
    }
}

/* X:R and R:Y, class I (bits 23-20 are 0001) */
static bool pm_xr_ry(DSP56300State *s, uint32_t op, PMove *m)
{
    bool write_reg = F(op, 15, 15), is_y = F(op, 14, 14);
    Loc l;
    uint32_t v;

    if (!loc_ea(s, F(op, 13, 8), is_y ? Y_SPACE : X_SPACE, &l)) {
        return false;
    }
    if (!is_y) {                            /* X:ea,D1  S2,D2 */
        static const uint8_t d1[4] = {
            DSP56300_REG_X0, DSP56300_REG_X1, DSP56300_REG_A, DSP56300_REG_B
        };
        unsigned int reg = d1[F(op, 19, 18)], acc = F(op, 17, 17);

        v = dsp_acc_bus(s, acc);
        if (write_reg) {
            pm_reg(m, reg, loc_read(s, &l));
        } else if (l.kind == LOC_IMM) {
            return false;
        } else {
            pm_mem(m, X_SPACE, l.addr, dsp_reg_read(s, reg));
        }
        pm_reg(m, F(op, 16, 16) ? DSP56300_REG_Y1 : DSP56300_REG_Y0, v);
    } else {                                /* S1,D1  Y:ea,D2 */
        static const uint8_t d2[4] = {
            DSP56300_REG_Y0, DSP56300_REG_Y1, DSP56300_REG_A, DSP56300_REG_B
        };
        unsigned int reg = d2[F(op, 17, 16)], acc = F(op, 19, 19);

        v = dsp_acc_bus(s, acc);
        if (write_reg) {
            pm_reg(m, reg, loc_read(s, &l));
        } else if (l.kind == LOC_IMM) {
            return false;
        } else {
            pm_mem(m, Y_SPACE, l.addr, dsp_reg_read(s, reg));
        }
        pm_reg(m, F(op, 18, 18) ? DSP56300_REG_X1 : DSP56300_REG_X0, v);
    }
    return true;
}

/* X:R and R:Y, class II: A -> X:ea, X0 -> A and Y0 -> A, A -> Y:ea. */
static bool pm_class2(DSP56300State *s, uint32_t op, PMove *m)
{
    unsigned int acc = F(op, 16, 16);
    bool is_y = F(op, 15, 15);
    unsigned int accreg = acc ? DSP56300_REG_B : DSP56300_REG_A;
    Loc l;

    if (!loc_ea(s, F(op, 13, 8), is_y ? Y_SPACE : X_SPACE, &l) ||
        l.kind == LOC_IMM) {
        return false;
    }
    pm_mem(m, l.space, l.addr, dsp_acc_bus(s, acc));
    pm_reg(m, accreg, is_y ? s->y0 : s->x0);
    return true;
}

/* X: and Y: memory in one instruction. */
static bool pm_xy_dual(DSP56300State *s, uint32_t op, PMove *m)
{
    static const uint8_t ee[4] = {
        DSP56300_REG_X0, DSP56300_REG_X1, DSP56300_REG_A, DSP56300_REG_B
    };
    static const uint8_t ff[4] = {
        DSP56300_REG_Y0, DSP56300_REG_Y1, DSP56300_REG_A, DSP56300_REG_B
    };
    static const uint8_t mode[4] = { 4, 1, 2, 3 };  /* (Rn), +Nn, -, + */
    unsigned int rrr = F(op, 10, 8), yreg = F(op, 14, 13) | (rrr & 4 ? 0 : 4);
    uint32_t xa, ya;
    bool imm;
    unsigned int xr = ee[F(op, 19, 18)], yr = ff[F(op, 17, 16)];

    if (!dsp_ea(s, (mode[F(op, 12, 11)] << 3) | rrr, &xa, &imm) ||
        !dsp_ea(s, (mode[F(op, 21, 20)] << 3) | yreg, &ya, &imm)) {
        return false;
    }
    if (F(op, 15, 15)) {
        pm_reg(m, xr, dsp_read(s, X_SPACE, xa));
    } else {
        pm_mem(m, X_SPACE, xa, dsp_reg_read(s, xr));
    }
    if (F(op, 22, 22)) {
        pm_reg(m, yr, dsp_read(s, Y_SPACE, ya));
    } else {
        pm_mem(m, Y_SPACE, ya, dsp_reg_read(s, yr));
    }
    return true;
}

static bool pm_prepare(DSP56300State *s, uint32_t op, PMove *m)
{
    uint32_t hi = op >> 16;

    m->n = 0;
    m->ifcc = false;
    if (hi & 0x80) {
        return pm_xy_dual(s, op, m);
    }
    if ((hi >> 6) == 1) {
        if (F(op, 21, 20) == 0 && !F(op, 18, 18)) {
            return pm_long_move(s, op, m);
        }
        return pm_xy_move(s, op, m);
    }
    if ((hi & 0xe0) == 0x20) {
        return pm_i_r_u(s, op, m);
    }
    if ((hi & 0xf0) == 0x10) {
        return pm_xr_ry(s, op, m);
    }
    if ((hi & 0xfe) == 0x08 && !F(op, 14, 14)) {
        return pm_class2(s, op, m);
    }
    return false;
}

/* Any opcode that is an ALU operation with a parallel move. */
static void op_parallel(DSP56300State *s, uint32_t op)
{
    PMove m;
    uint64_t save_acc[2];
    uint32_t save_sr = 0;
    bool cond = true;

    if (!pm_prepare(s, op, &m)) {
        dsp_illegal(s);
        return;
    }
    if (m.ifcc) {
        cond = dsp_cond(s, m.cc);
        save_acc[0] = s->acc[0];
        save_acc[1] = s->acc[1];
        save_sr = s->sr;
    }
    if (!dsp_alu_parallel(s, op & 0xff)) {
        dsp_illegal(s);
        return;
    }
    if (m.ifcc) {
        if (!cond) {
            s->acc[0] = save_acc[0];
            s->acc[1] = save_acc[1];
        }
        if (!cond || !m.ifu) {
            s->sr = (s->sr & ~SR_CCR) | (save_sr & SR_CCR);
        }
    }
    pm_apply(s, &m);
}

/* ---- control flow ------------------------------------------------------ */

static void op_nop(DSP56300State *s, uint32_t op)
{
}

static void op_illegal(DSP56300State *s, uint32_t op)
{
    s->icycles = 5;
    dsp_illegal(s);
}

static void op_trap(DSP56300State *s, uint32_t op)
{
    if (dsp_rep_forbidden(s)) {
        return;
    }
    s->icycles = 9;
    dsp56300_irq_raise(s, DSP56300_VEC_TRAP, 3);
}

static void op_trapcc(DSP56300State *s, uint32_t op)
{
    if (dsp_rep_forbidden(s)) {
        return;
    }
    if (dsp_cond(s, F(op, 3, 0))) {
        op_trap(s, op);
    }
}

static void op_rti(DSP56300State *s, uint32_t op)
{
    uint32_t pc, sr;

    if (dsp_rep_forbidden(s)) {
        return;
    }
    dsp_pull(s, &pc, &sr);
    dsp_set_sr(s, sr);
    s->icycles = 3;
    s->npc = pc;
    s->flow |= FLOW_JUMP;
}

static void op_rts(DSP56300State *s, uint32_t op)
{
    uint32_t pc, sr;

    if (dsp_rep_forbidden(s)) {
        return;
    }
    dsp_pull(s, &pc, &sr);
    s->icycles = 3;
    s->npc = pc;
    s->flow |= FLOW_JUMP;
}

static void op_reset(DSP56300State *s, uint32_t op)
{
    s->icycles = 7;
    if (s->ops.peripheral_reset) {
        s->ops.peripheral_reset(s->opaque);
    }
}

static void op_wait(DSP56300State *s, uint32_t op)
{
    if (dsp_rep_forbidden(s)) {
        return;
    }
    s->icycles = 10;
    s->run_state = DSP56300_WAIT;
}

static void op_stop(DSP56300State *s, uint32_t op)
{
    if (dsp_rep_forbidden(s)) {
        return;
    }
    s->icycles = 10;
    s->run_state = DSP56300_STOP;
}

/* Instruction cache control: the cache is not modelled. */
static void op_cache(DSP56300State *s, uint32_t op)
{
    if (!(s->sr & DSP56300_SR_CE)) {
        dsp_illegal(s);
        return;
    }
    if ((op & 0xffffff) == 0x00000f || (op & 0xffffff) == 0x00000e) {
        dsp_fetch_ext(s);               /* PLOCKR, PUNLOCKR */
    } else if ((op >> 16) >= 0x0a) {
        uint32_t v;
        bool imm;

        if (!dsp_ea(s, F(op, 13, 8), &v, &imm)) {   /* PLOCK, PUNLOCK */
            dsp_illegal(s);
        }
    }
}

static void op_andi(DSP56300State *s, uint32_t op)
{
    uint32_t v = ~F(op, 15, 8) & 0xff;

    switch (F(op, 1, 0)) {
    case 0:
        dsp_set_sr(s, s->sr & ~(v << 8));
        break;
    case 1:
        dsp_set_sr(s, s->sr & ~v);
        break;
    case 2:
        dsp_set_omr(s, s->omr & ~v);
        break;
    default:
        dsp_set_omr(s, s->omr & ~(v << 8));
        break;
    }
    s->icycles = 3;
}

static void op_ori(DSP56300State *s, uint32_t op)
{
    uint32_t imm = F(op, 15, 8);

    switch (F(op, 1, 0)) {
    case 0:
        dsp_set_sr(s, s->sr | imm << 8);
        break;
    case 1:
        dsp_set_sr(s, s->sr | imm);
        break;
    case 2:
        dsp_set_omr(s, s->omr | imm);
        break;
    default:
        dsp_set_omr(s, s->omr | imm << 8);
        break;
    }
    s->icycles = 3;
}

/* Jumps and branches. */
static uint32_t short_disp9(uint32_t op)
{
    uint32_t d = (F(op, 9, 6) << 5) | F(op, 4, 0);

    return (uint32_t)sextract32(d, 0, 9) & M24;
}

static void op_jmp_xxx(DSP56300State *s, uint32_t op)
{
    s->icycles = 3;
    dsp_jump(s, F(op, 11, 0));
}

static void op_jsr_xxx(DSP56300State *s, uint32_t op)
{
    s->icycles = 3;
    dsp_call(s, F(op, 11, 0));
}

static void op_jcc_xxx(DSP56300State *s, uint32_t op)
{
    s->icycles = 4;
    if (dsp_cond(s, F(op, 15, 12))) {
        dsp_jump(s, F(op, 11, 0));
    }
}

static void op_jscc_xxx(DSP56300State *s, uint32_t op)
{
    s->icycles = 4;
    if (dsp_cond(s, F(op, 15, 12))) {
        dsp_call(s, F(op, 11, 0));
    }
}

/* ea of a jump: the effective address itself is the target. */
static bool jump_ea(DSP56300State *s, uint32_t op, uint32_t *target)
{
    bool imm;

    if (!dsp_ea(s, F(op, 13, 8), target, &imm) || imm) {
        dsp_illegal(s);
        return false;
    }
    return true;
}

static void op_jmp_ea(DSP56300State *s, uint32_t op)
{
    uint32_t t;

    s->icycles = 3;
    if (jump_ea(s, op, &t)) {
        dsp_jump(s, t);
    }
}

static void op_jsr_ea(DSP56300State *s, uint32_t op)
{
    uint32_t t;

    s->icycles = 3;
    if (jump_ea(s, op, &t)) {
        dsp_call(s, t);
    }
}

static void op_jcc_ea(DSP56300State *s, uint32_t op)
{
    uint32_t t;

    s->icycles = 4;
    if (jump_ea(s, op, &t) && dsp_cond(s, F(op, 3, 0))) {
        dsp_jump(s, t);
    }
}

static void op_jscc_ea(DSP56300State *s, uint32_t op)
{
    uint32_t t;

    s->icycles = 4;
    if (jump_ea(s, op, &t) && dsp_cond(s, F(op, 3, 0))) {
        dsp_call(s, t);
    }
}

/* Relative branches: bra/bsr/bcc/bscc with a short, long or Rn offset. */
static void branch(DSP56300State *s, uint32_t target, bool take, bool call)
{
    if (!take) {
        return;
    }
    if (call) {
        dsp_call(s, target);
    } else {
        dsp_jump(s, target);
    }
}

static void op_bcc_short(DSP56300State *s, uint32_t op)
{
    s->icycles = 4;
    branch(s, s->ipc + short_disp9(op), dsp_cond(s, F(op, 15, 12)), false);
}

static void op_bscc_short(DSP56300State *s, uint32_t op)
{
    s->icycles = 4;
    branch(s, s->ipc + short_disp9(op), dsp_cond(s, F(op, 15, 12)), true);
}

static void op_bra_short(DSP56300State *s, uint32_t op)
{
    s->icycles = 4;
    branch(s, s->ipc + short_disp9(op), true, false);
}

static void op_bsr_short(DSP56300State *s, uint32_t op)
{
    s->icycles = 4;
    branch(s, s->ipc + short_disp9(op), true, true);
}

static uint32_t long_target(DSP56300State *s)
{
    uint32_t d = dsp_fetch_ext(s);

    s->icycles = 5;
    return s->ipc + dsp_sx24(d);
}

static void op_bcc_long(DSP56300State *s, uint32_t op)
{
    branch(s, long_target(s), dsp_cond(s, F(op, 3, 0)), false);
}

static void op_bscc_long(DSP56300State *s, uint32_t op)
{
    branch(s, long_target(s), dsp_cond(s, F(op, 3, 0)), true);
}

static void op_bra_long(DSP56300State *s, uint32_t op)
{
    branch(s, long_target(s), true, false);
}

static void op_bsr_long(DSP56300State *s, uint32_t op)
{
    branch(s, long_target(s), true, true);
}

static uint32_t rn_target(DSP56300State *s, uint32_t op)
{
    s->icycles = 4;
    return s->ipc + dsp_sx24(s->r[F(op, 10, 8)]);
}

static void op_bcc_rn(DSP56300State *s, uint32_t op)
{
    branch(s, rn_target(s, op), dsp_cond(s, F(op, 3, 0)), false);
}

static void op_bscc_rn(DSP56300State *s, uint32_t op)
{
    branch(s, rn_target(s, op), dsp_cond(s, F(op, 3, 0)), true);
}

static void op_bra_rn(DSP56300State *s, uint32_t op)
{
    branch(s, rn_target(s, op), true, false);
}

static void op_bsr_rn(DSP56300State *s, uint32_t op)
{
    branch(s, rn_target(s, op), true, true);
}

/* ---- hardware loops ---------------------------------------------------- */

static void loop_end_pop(DSP56300State *s)
{
    uint32_t hi, lo;

    dsp_pull(s, &hi, &lo);
    s->sr = (s->sr & ~(DSP56300_SR_LF | DSP56300_SR_FV)) |
            (lo & (DSP56300_SR_LF | DSP56300_SR_FV));
    dsp_pull(s, &hi, &lo);
    s->la = hi;
    s->lc = lo;
}

/*
 * Second half of DO/DOR/DO FOREVER, after the first stack entry (the old
 * LA and LC) has been pushed and the count is known. An empty loop is
 * skipped and the stack left as it was; with SC set a zero count means
 * 65536 passes.
 */
static void do_start(DSP56300State *s, uint32_t count, uint32_t last,
                     bool forever)
{
    s->icycles = 5;
    if (!forever) {
        if (count == 0 && !(s->sr & DSP56300_SR_SC)) {
            uint32_t la, lc;

            dsp_pull(s, &la, &lc);
            s->la = la;
            s->lc = lc;
            dsp_jump(s, last + 1);
            return;
        }
        s->lc = (count ? count : 0x10000) & M24;
    }
    dsp_push(s, dsp_ret_addr(s), s->sr);
    s->la = last & M24;
    s->sr |= DSP56300_SR_LF;
    if (forever) {
        s->sr |= DSP56300_SR_FV;
    } else {
        s->sr &= ~DSP56300_SR_FV;
    }
}

/* The count operand of DO and REP: #xxx, a register, X:/Y: ea or aa. */
static bool loop_count_loc(DSP56300State *s, uint32_t op, Loc *l)
{
    DSP56300Space sp = F(op, 6, 6) ? Y_SPACE : X_SPACE;
    unsigned int form = F(op, 15, 14);

    if (F(op, 7, 7)) {
        l->kind = LOC_IMM;
        l->addr = (F(op, 3, 0) << 8) | F(op, 15, 8);
        return true;
    }
    if (form == 3) {
        loc_reg(l, F(op, 13, 8));
        return true;
    }
    if (form == 1) {
        return loc_ea(s, F(op, 13, 8), sp, l);
    }
    loc_mem(l, sp, F(op, 13, 8));
    return true;
}

/*
 * DO and DOR. The loop address comes from the extension word that follows
 * (after any the effective address needs); DOR makes it PC-relative. The
 * count is read after the first stack push so that DO SP and DO SSL see the
 * values the manual describes.
 */
static void op_do(DSP56300State *s, uint32_t op, bool rel)
{
    uint32_t last;
    Loc l;

    if (dsp_rep_forbidden(s)) {
        return;
    }
    if (!loop_count_loc(s, op, &l)) {
        dsp_illegal(s);
        return;
    }
    last = dsp_fetch_ext(s);
    if (rel) {
        last = s->ipc + dsp_sx24(last);
    }
    dsp_push(s, s->la, s->lc);
    do_start(s, loc_read(s, &l), last, false);
}

static void op_do_gen(DSP56300State *s, uint32_t op)
{
    op_do(s, op, false);
}

static void op_dor_gen(DSP56300State *s, uint32_t op)
{
    op_do(s, op, true);
}

static void op_do_forever(DSP56300State *s, uint32_t op)
{
    uint32_t last;

    if (dsp_rep_forbidden(s)) {
        return;
    }
    last = dsp_fetch_ext(s);
    dsp_push(s, s->la, s->lc);
    do_start(s, 0, last, true);
    s->icycles = 4;
}

static void op_dor_forever(DSP56300State *s, uint32_t op)
{
    uint32_t last;

    if (dsp_rep_forbidden(s)) {
        return;
    }
    last = s->ipc + dsp_sx24(dsp_fetch_ext(s));
    dsp_push(s, s->la, s->lc);
    do_start(s, 0, last, true);
    s->icycles = 4;
}

static void op_enddo(DSP56300State *s, uint32_t op)
{
    if (dsp_rep_forbidden(s)) {
        return;
    }
    loop_end_pop(s);
}

static void op_brkcc(DSP56300State *s, uint32_t op)
{
    if (dsp_rep_forbidden(s)) {
        return;
    }
    s->icycles = 5;
    if (dsp_cond(s, F(op, 3, 0))) {
        uint32_t la = s->la;

        loop_end_pop(s);
        dsp_jump(s, la + 1);
    }
}

static void op_rep(DSP56300State *s, uint32_t op)
{
    Loc l;
    uint32_t count;

    if (dsp_rep_forbidden(s)) {
        return;
    }
    if (!loop_count_loc(s, op, &l)) {
        dsp_illegal(s);
        return;
    }
    count = loc_read(s, &l);
    s->icycles = 5;
    s->rep_save = s->lc;
    s->rep_count = count ? count : 0x10000;
    s->rep_active = true;
    s->rep_ipc = (s->ipc + 1 + s->ext) & M24;
}

/* ---- bit manipulation -------------------------------------------------- */

typedef enum BitKind {
    BK_BCLR, BK_BSET, BK_BCHG, BK_BTST,
    BK_JCLR, BK_JSET, BK_JSCLR, BK_JSSET,
    BK_BRCLR, BK_BRSET, BK_BSCLR, BK_BSSET,
} BitKind;

typedef enum BitForm { BF_AA, BF_EA, BF_PP, BF_QQ, BF_REG } BitForm;

/* Work out which of the twelve bit instructions, on which operand form. */
static void bit_decode(uint32_t op, BitKind *kind, BitForm *form)
{
    unsigned int hi = op >> 16, f = F(op, 15, 14), set = F(op, 5, 5);
    bool jump = F(op, 7, 7);

    switch (hi) {
    case 0x0a:
    case 0x0b:
        *form = f == 0 ? BF_AA : f == 1 ? BF_EA : f == 2 ? BF_PP : BF_REG;
        if (f == 3) {
            jump = !F(op, 6, 6);            /* 00x: jump, 01x: bit op */
        }
        if (jump) {
            *kind = hi == 0x0a ? (set ? BK_JSET : BK_JCLR)
                               : (set ? BK_JSSET : BK_JSCLR);
        } else {
            *kind = hi == 0x0a ? (set ? BK_BSET : BK_BCLR)
                               : (set ? BK_BTST : BK_BCHG);
        }
        break;
    case 0x01:
        *form = BF_QQ;
        if (f == 0) {
            *kind = set ? BK_BSET : BK_BCLR;
        } else if (f == 1) {
            *kind = set ? BK_BTST : BK_BCHG;
        } else if (f == 2) {
            *kind = set ? BK_JSET : BK_JCLR;
        } else {
            *kind = set ? BK_JSSET : BK_JSCLR;
        }
        break;
    default:
        {                              /* 0x0c, 0x0d, 0x04 */
        bool sub = hi == 0x0d || (hi == 0x04 && jump);

        if (hi == 0x04) {
            *form = BF_QQ;
        } else if (f == 2) {
            *form = jump ? BF_AA : BF_EA;
        } else {
            *form = jump ? BF_REG : BF_PP;
        }
        *kind = sub ? (set ? BK_BSSET : BK_BSCLR)
                    : (set ? BK_BRSET : BK_BRCLR);
        break;
    }
    }
}

static void op_bit(DSP56300State *s, uint32_t op)
{
    BitKind kind;
    BitForm form;
    DSP56300Space sp = F(op, 6, 6) ? Y_SPACE : X_SPACE;
    unsigned int bit = F(op, 4, 0), field = F(op, 13, 8);
    bool is_jump, is_branch, test_set, is_call;
    uint32_t target = 0, v;
    bool b;
    Loc l;

    bit_decode(op, &kind, &form);
    is_jump = kind >= BK_JCLR && kind <= BK_JSSET;
    is_branch = kind >= BK_BRCLR;
    if (is_jump || is_branch) {
        target = dsp_fetch_ext(s);
        if (is_branch) {
            target = s->ipc + dsp_sx24(target);
        }
        s->icycles = is_branch ? 5 : 4;
    } else {
        s->icycles = 2;
    }
    switch (form) {
    case BF_AA:
        loc_mem(&l, sp, field);
        break;
    case BF_PP:
        loc_mem(&l, sp, IO_PP | field);
        break;
    case BF_QQ:
        loc_mem(&l, sp, IO_QQ | field);
        break;
    case BF_REG:
        loc_reg(&l, field);
        break;
    default:
        if (!loc_ea(s, field, sp, &l) || l.kind == LOC_IMM) {
            dsp_illegal(s);
            return;
        }
        break;
    }
    v = loc_read(s, &l);
    if (bit > 23) {
        bit = 0;
    }
    b = (v >> bit) & 1;
    switch (kind) {
    case BK_BCLR:
    case BK_BSET:
    case BK_BCHG:
        if (kind == BK_BCLR) {
            v &= ~(1u << bit);
        } else if (kind == BK_BSET) {
            v |= 1u << bit;
        } else {
            v ^= 1u << bit;
        }
        loc_write(s, &l, v);
        if (!(l.kind == LOC_REG && l.addr == DSP56300_REG_SR)) {
            s->sr = (s->sr & ~DSP56300_SR_C) | (b ? DSP56300_SR_C : 0);
        }
        return;
    case BK_BTST:
        s->sr = (s->sr & ~DSP56300_SR_C) | (b ? DSP56300_SR_C : 0);
        return;
    default:
        break;
    }
    test_set = kind == BK_JSET || kind == BK_JSSET || kind == BK_BRSET ||
               kind == BK_BSSET;
    is_call = kind == BK_JSCLR || kind == BK_JSSET || kind == BK_BSCLR ||
              kind == BK_BSSET;
    if (b == test_set) {
        if (is_call) {
            dsp_call(s, target);
        } else {
            dsp_jump(s, target);
        }
    }
}

/* ---- moves ------------------------------------------------------------- */

static void op_lua(DSP56300State *s, uint32_t op)
{
    unsigned int rn = F(op, 10, 8), mm = F(op, 12, 11);
    int32_t n = dsp_sx24(s->n[rn]);
    int32_t off = mm == 0 ? -n : mm == 1 ? n : mm == 2 ? -1 : 1;

    s->icycles = 3;
    dsp_reg_write(s, F(op, 4, 0), dsp_agu_add(s, rn, off));
}

static void op_lea(DSP56300State *s, uint32_t op)
{
    unsigned int rn = F(op, 10, 8), d = F(op, 3, 0);
    int32_t off = sextract32((F(op, 13, 11) << 4) | F(op, 7, 4), 0, 7);

    s->icycles = 3;
    dsp_reg_write(s, (d & 8 ? DSP56300_REG_N0 : DSP56300_REG_R0) + (d & 7),
                  dsp_agu_add(s, rn, off));
}

static void op_lra_rn(DSP56300State *s, uint32_t op)
{
    s->icycles = 3;
    dsp_reg_write(s, F(op, 4, 0), s->ipc + s->r[F(op, 10, 8)]);
}

static void op_lra_long(DSP56300State *s, uint32_t op)
{
    uint32_t d = dsp_fetch_ext(s);

    s->icycles = 3;
    dsp_reg_write(s, F(op, 4, 0), s->ipc + dsp_sx24(d));
}

/* MOVEC and the 6-bit register move share their register mapping. */
static void movec_xfer(DSP56300State *s, const Loc *other, bool to_ctl,
                       unsigned int ctl)
{
    if (to_ctl) {
        dsp_reg_write(s, ctl + 0x20, loc_read(s, other));
    } else {
        loc_write(s, other, dsp_reg_read(s, ctl + 0x20));
    }
}

static void op_movec_ea(DSP56300State *s, uint32_t op)
{
    Loc l;

    s->icycles = 1;
    if (!loc_ea(s, F(op, 13, 8), F(op, 6, 6) ? Y_SPACE : X_SPACE, &l)) {
        dsp_illegal(s);
        return;
    }
    if (l.kind == LOC_IMM && !F(op, 15, 15)) {
        dsp_illegal(s);
        return;
    }
    movec_xfer(s, &l, F(op, 15, 15), F(op, 4, 0));
}

static void op_movec_aa(DSP56300State *s, uint32_t op)
{
    Loc l;

    s->icycles = 1;
    loc_mem(&l, F(op, 6, 6) ? Y_SPACE : X_SPACE, F(op, 13, 8));
    movec_xfer(s, &l, F(op, 15, 15), F(op, 4, 0));
}

static void op_movec_reg(DSP56300State *s, uint32_t op)
{
    Loc l;

    s->icycles = 1;
    loc_reg(&l, F(op, 13, 8));
    movec_xfer(s, &l, F(op, 15, 15), F(op, 4, 0));
}

static void op_movec_imm(DSP56300State *s, uint32_t op)
{
    s->icycles = 1;
    dsp_reg_write(s, F(op, 4, 0) + 0x20, F(op, 15, 8));
}

static void op_movem(DSP56300State *s, uint32_t op)
{
    Loc l;

    s->icycles = 6;
    if (F(op, 14, 14)) {
        if (!loc_ea(s, F(op, 13, 8), P_SPACE, &l) || l.kind == LOC_IMM) {
            dsp_illegal(s);
            return;
        }
    } else {
        loc_mem(&l, P_SPACE, F(op, 13, 8));
    }
    if (F(op, 15, 15)) {
        dsp_reg_write(s, F(op, 5, 0), loc_read(s, &l));
    } else {
        loc_write(s, &l, dsp_reg_read(s, F(op, 5, 0)));
    }
}

/*
 * MOVEP: the peripheral at pa in space ps against a register, an X: or Y:
 * location, or P:. W set writes the peripheral.
 */
static void movep_do(DSP56300State *s, bool write, const Loc *periph,
                     const Loc *other)
{
    if (write) {
        loc_write(s, periph, loc_read(s, other));
    } else {
        loc_write(s, other, loc_read(s, periph));
    }
}

static void op_movep_ea(DSP56300State *s, uint32_t op)
{
    Loc per, other;

    s->icycles = 2;
    loc_mem(&per, F(op, 16, 16) ? Y_SPACE : X_SPACE, IO_PP | F(op, 5, 0));
    if (!loc_ea(s, F(op, 13, 8), F(op, 6, 6) ? Y_SPACE : X_SPACE, &other)) {
        dsp_illegal(s);
        return;
    }
    movep_do(s, F(op, 15, 15), &per, &other);
}

static void op_movep_p(DSP56300State *s, uint32_t op)
{
    Loc per, other;

    s->icycles = 6;
    loc_mem(&per, F(op, 16, 16) ? Y_SPACE : X_SPACE, IO_PP | F(op, 5, 0));
    if (!loc_ea(s, F(op, 13, 8), P_SPACE, &other)) {
        dsp_illegal(s);
        return;
    }
    movep_do(s, F(op, 15, 15), &per, &other);
}

static void op_movep_reg(DSP56300State *s, uint32_t op)
{
    Loc per, other;

    s->icycles = 1;
    loc_mem(&per, F(op, 16, 16) ? Y_SPACE : X_SPACE, IO_PP | F(op, 5, 0));
    loc_reg(&other, F(op, 13, 8));
    movep_do(s, F(op, 15, 15), &per, &other);
}

static void op_movep_qq_x(DSP56300State *s, uint32_t op)
{
    Loc per, other;

    s->icycles = 2;
    loc_mem(&per, X_SPACE, IO_QQ | F(op, 5, 0));
    if (!loc_ea(s, F(op, 13, 8), F(op, 6, 6) ? Y_SPACE : X_SPACE, &other)) {
        dsp_illegal(s);
        return;
    }
    movep_do(s, F(op, 15, 15), &per, &other);
}

static void op_movep_qq_y(DSP56300State *s, uint32_t op)
{
    Loc per, other;

    s->icycles = 2;
    loc_mem(&per, Y_SPACE, IO_QQ | F(op, 5, 0));
    if (!loc_ea(s, F(op, 13, 8), F(op, 6, 6) ? Y_SPACE : X_SPACE, &other)) {
        dsp_illegal(s);
        return;
    }
    movep_do(s, F(op, 15, 15), &per, &other);
}

static void op_movep_qq_p(DSP56300State *s, uint32_t op)
{
    Loc per, other;

    s->icycles = 6;
    loc_mem(&per, F(op, 6, 6) ? Y_SPACE : X_SPACE, IO_QQ | F(op, 5, 0));
    if (!loc_ea(s, F(op, 13, 8), P_SPACE, &other)) {
        dsp_illegal(s);
        return;
    }
    movep_do(s, F(op, 14, 14), &per, &other);
}

static void op_movep_qq_reg_x(DSP56300State *s, uint32_t op)
{
    Loc per, other;

    s->icycles = 1;
    loc_mem(&per, X_SPACE, IO_QQ | (F(op, 6, 6) << 5) | F(op, 4, 0));
    loc_reg(&other, F(op, 13, 8));
    movep_do(s, F(op, 15, 15), &per, &other);
}

static void op_movep_qq_reg_y(DSP56300State *s, uint32_t op)
{
    Loc per, other;

    s->icycles = 1;
    loc_mem(&per, Y_SPACE, IO_QQ | (F(op, 6, 6) << 5) | F(op, 4, 0));
    loc_reg(&other, F(op, 13, 8));
    movep_do(s, F(op, 15, 15), &per, &other);
}

/* MOVE with an Rn plus displacement address. */
static void disp_move(DSP56300State *s, DSP56300Space sp, unsigned int rn,
                      uint32_t disp, bool write_reg, unsigned int reg)
{
    uint32_t addr = (s->r[rn] + disp) & M24;

    if (write_reg) {
        dsp_reg_write(s, reg, dsp_read(s, sp, addr));
    } else {
        dsp_write(s, sp, addr, dsp_reg_read(s, reg));
    }
}

static void op_move_disp_long(DSP56300State *s, uint32_t op)
{
    uint32_t d = dsp_fetch_ext(s);

    s->icycles = 3;
    disp_move(s, F(op, 16, 16) ? Y_SPACE : X_SPACE, F(op, 10, 8), d,
              F(op, 6, 6), F(op, 5, 0));
}

static void op_move_disp_short(DSP56300State *s, uint32_t op)
{
    int32_t d = sextract32((F(op, 16, 11) << 1) | F(op, 6, 6), 0, 7);

    s->icycles = 2;
    disp_move(s, F(op, 5, 5) ? Y_SPACE : X_SPACE, F(op, 10, 8),
              (uint32_t)d & M24, F(op, 4, 4), F(op, 3, 0));
}

static void op_vsl(DSP56300State *s, uint32_t op)
{
    uint64_t a = s->acc[F(op, 16, 16)];
    uint32_t addr;
    bool imm;

    s->icycles = 1;
    if (!dsp_ea(s, F(op, 13, 8), &addr, &imm) || imm) {
        dsp_illegal(s);
        return;
    }
    dsp_write(s, X_SPACE, addr, dsp_acc1(a));
    dsp_write(s, Y_SPACE, addr, ((dsp_acc0(a) << 1) | F(op, 4, 4)) & M24);
}

const DspInsn dsp_ctl_insns[] = {
    { "00000000 00000000 00000000", op_nop },
    { "00000000 00000000 00000101", op_illegal },
    { "00000000 00000000 00000110", op_trap },
    { "00000000 00000000 0001CCCC", op_trapcc },
    { "00000000 00000000 00000100", op_rti },
    { "00000000 00000000 00001100", op_rts },
    { "00000000 00000000 10000100", op_reset },
    { "00000000 00000000 10000110", op_wait },
    { "00000000 00000000 10000111", op_stop },
    { "00000000 00000000 10001100", op_enddo },
    { "00000000 00000010 00000011", op_do_forever },
    { "00000000 00000010 00000010", op_dor_forever },
    { "00000000 00000010 0001CCCC", op_brkcc },
    { "00000000 00000010 00000000", op_nop },        /* DEBUG */
    { "00000000 00000011 0000CCCC", op_nop },        /* DEBUGcc */
    { "00000000 00000000 00000011", op_cache },      /* PFLUSH */
    { "00000000 00000000 00000001", op_cache },      /* PFLUSHUN */
    { "00000000 00000000 00000010", op_cache },      /* PFREE */
    { "00000000 00000000 00001111", op_cache },      /* PLOCKR */
    { "00000000 00000000 00001110", op_cache },      /* PUNLOCKR */
    { "00001011 11MMMRRR 10000001", op_cache },      /* PLOCK */
    { "00001010 11MMMRRR 10000001", op_cache },      /* PUNLOCK */
    { "00000000 iiiiiiii 101110EE", op_andi },
    { "00000000 iiiiiiii 111110EE", op_ori },

    { "00001100 0000aaaa aaaaaaaa", op_jmp_xxx },
    { "00001101 0000aaaa aaaaaaaa", op_jsr_xxx },
    { "00001110 CCCCaaaa aaaaaaaa", op_jcc_xxx },
    { "00001111 CCCCaaaa aaaaaaaa", op_jscc_xxx },
    { "00001010 11MMMRRR 10000000", op_jmp_ea },
    { "00001011 11MMMRRR 10000000", op_jsr_ea },
    { "00001010 11MMMRRR 1010CCCC", op_jcc_ea },
    { "00001011 11MMMRRR 1010CCCC", op_jscc_ea },

    { "00000101 CCCC01aa aa0aaaaa", op_bcc_short },
    { "00000101 CCCC00aa aa0aaaaa", op_bscc_short },
    { "00000101 000011aa aa0aaaaa", op_bra_short },
    { "00000101 000010aa aa0aaaaa", op_bsr_short },
    { "00001101 00010000 0100CCCC", op_bcc_long },
    { "00001101 00010000 0000CCCC", op_bscc_long },
    { "00001101 00010000 11000000", op_bra_long },
    { "00001101 00010000 10000000", op_bsr_long },
    { "00001101 00011RRR 0100CCCC", op_bcc_rn },
    { "00001101 00011RRR 0000CCCC", op_bscc_rn },
    { "00001101 00011RRR 11000000", op_bra_rn },
    { "00001101 00011RRR 10000000", op_bsr_rn },

    { "00000110 01MMMRRR 0S000000", op_do_gen },
    { "00000110 00aaaaaa 0S000000", op_do_gen },
    { "00000110 iiiiiiii 1000hhhh", op_do_gen },
    { "00000110 11DDDDDD 00000000", op_do_gen },
    { "00000110 01MMMRRR 0S010000", op_dor_gen },
    { "00000110 00aaaaaa 0S010000", op_dor_gen },
    { "00000110 iiiiiiii 1001hhhh", op_dor_gen },
    { "00000110 11DDDDDD 00010000", op_dor_gen },
    { "00000110 01MMMRRR 0S100000", op_rep },
    { "00000110 00aaaaaa 0S100000", op_rep },
    { "00000110 iiiiiiii 1010hhhh", op_rep },
    { "00000110 11dddddd 00100000", op_rep },

    { "00001010 00aaaaaa xSxbbbbb", op_bit },
    { "00001010 01MMMRRR xSxbbbbb", op_bit },
    { "00001010 10pppppp xSxbbbbb", op_bit },
    { "00001010 11DDDDDD 0xxbbbbb", op_bit },
    { "00001011 00aaaaaa xSxbbbbb", op_bit },
    { "00001011 01MMMRRR xSxbbbbb", op_bit },
    { "00001011 10pppppp xSxbbbbb", op_bit },
    { "00001011 11DDDDDD 0xxbbbbb", op_bit },
    { "00000001 00qqqqqq 0Sxbbbbb", op_bit },
    { "00000001 01qqqqqq 0Sxbbbbb", op_bit },
    { "00000001 10qqqqqq 1Sxbbbbb", op_bit },
    { "00000001 11qqqqqq 1Sxbbbbb", op_bit },
    { "00001100 10MMMRRR 0Sxbbbbb", op_bit },
    { "00001100 10aaaaaa 1Sxbbbbb", op_bit },
    { "00001100 11pppppp 0Sxbbbbb", op_bit },
    { "00001100 11DDDDDD 10xbbbbb", op_bit },
    { "00001101 10MMMRRR 0Sxbbbbb", op_bit },
    { "00001101 10aaaaaa 1Sxbbbbb", op_bit },
    { "00001101 11pppppp 0Sxbbbbb", op_bit },
    { "00001101 11DDDDDD 10xbbbbb", op_bit },
    { "00000100 10qqqqqq 0Sxbbbbb", op_bit },
    { "00000100 10qqqqqq 1Sxbbbbb", op_bit },

    { "00000100 010MMRRR 000ddddd", op_lua },
    { "00000100 00aaaRRR aaaadddd", op_lea },
    { "00000100 11000RRR 000ddddd", op_lra_rn },
    { "00000100 01000000 010ddddd", op_lra_long },

    { "00000101 W1MMMRRR 0S1ddddd", op_movec_ea },
    { "00000101 W0aaaaaa 0S1ddddd", op_movec_aa },
    { "00000100 W1eeeeee 101ddddd", op_movec_reg },
    { "00000101 iiiiiiii 101ddddd", op_movec_imm },
    { "00000111 W1MMMRRR 10dddddd", op_movem },
    { "00000111 W0aaaaaa 00dddddd", op_movem },

    { "0000100s W1MMMRRR 1Spppppp", op_movep_ea },
    { "0000100s W1MMMRRR 01pppppp", op_movep_p },
    { "0000100s W1dddddd 00pppppp", op_movep_reg },
    { "00000111 W1MMMRRR 0Sqqqqqq", op_movep_qq_x },
    { "00000111 W0MMMRRR 1Sqqqqqq", op_movep_qq_y },
    { "00000000 1WMMMRRR 0Sqqqqqq", op_movep_qq_p },
    { "00000100 W1dddddd 1q0qqqqq", op_movep_qq_reg_x },
    { "00000100 W1dddddd 0q1qqqqq", op_movep_qq_reg_y },

    { "0000101S 01110RRR 1WDDDDDD", op_move_disp_long },
    { "0000001a aaaaaRRR 1a0WDDDD", op_move_disp_short },
    { "0000001a aaaaaRRR 1a1WDDDD", op_move_disp_short },
    { "0000101S 11MMMRRR 110i0000", op_vsl },

    /* everything else is an ALU operation with a parallel move field */
    { "xxxxxxxx xxxxxxxx xxxxxxxx", op_parallel },
    { NULL, NULL }
};
