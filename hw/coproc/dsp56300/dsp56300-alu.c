/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * DSP56300 core: the data ALU. Accumulators are 56 bits (A2:A1:A0), the
 * multiplier is fractional, results update the condition code register as
 * the manual defines. Instructions that take a parallel move are reached
 * through dsp_alu_parallel() with the low byte of the opcode; the rest are
 * registered in dsp_alu_insns[].
 *
 * Written from the NXP DSP56300 Family Manual, rev 5, and nothing else.
 */
#include "qemu/osdep.h"
#include "dsp56300-int.h"

#define F(op, hi, lo) (((op) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define M48 0x0000ffffffffffffULL

typedef struct AluRes {
    uint64_t res;
    bool v, c;
} AluRes;

/* ---- conditions and flags ---------------------------------------------- */

bool dsp_cond(const DSP56300State *s, unsigned int cc)
{
    unsigned int sr = s->sr;
    bool c = sr & DSP56300_SR_C, v = sr & DSP56300_SR_V;
    bool z = sr & DSP56300_SR_Z, n = sr & DSP56300_SR_N;
    bool u = sr & DSP56300_SR_U, e = sr & DSP56300_SR_E;
    bool l = sr & DSP56300_SR_L;
    bool nr = z || (!u && !e);

    switch (cc & 15) {
    case 0x0:  /* CC (HS) */
        return !c;
    case 0x1:  /* GE */
        return !(n ^ v);
    case 0x2:  /* NE */
        return !z;
    case 0x3:  /* PL */
        return !n;
    case 0x4:  /* NN */
        return !nr;
    case 0x5:  /* EC */
        return !e;
    case 0x6:  /* LC */
        return !l;
    case 0x7:  /* GT */
        return !(z || (n ^ v));
    case 0x8:  /* CS (LO) */
        return c;
    case 0x9:  /* LT */
        return n ^ v;
    case 0xa:  /* EQ */
        return z;
    case 0xb:  /* MI */
        return n;
    case 0xc:  /* NR */
        return nr;
    case 0xd:  /* ES */
        return e;
    case 0xe:  /* LS */
        return l;
    default:  /* LE */
        return z || (n ^ v);
    }
}

static void ccr_assign(DSP56300State *s, uint32_t mask, uint32_t val)
{
    s->sr = (s->sr & ~mask) | (val & mask);
}

static void set_c(DSP56300State *s, bool c)
{
    ccr_assign(s, DSP56300_SR_C, c ? DSP56300_SR_C : 0);
}

/* E, U, N, Z and V from a 56-bit result; L latches V. C is left alone. */
void dsp_set_ccr_std(DSP56300State *s, uint64_t res, bool v)
{
    int64_t t = dsp_sx56(res);
    unsigned int mode = (s->sr >> 10) & 3;
    bool e, u;
    uint32_t f = 0;

    switch (mode) {
    case 1:
        e = (t >> 48) != 0 && (t >> 48) != -1;
        u = ((t >> 48) ^ (t >> 47)) & 1 ? false : true;
        break;
    case 2:
        e = (t >> 46) != 0 && (t >> 46) != -1;
        u = ((t >> 46) ^ (t >> 45)) & 1 ? false : true;
        break;
    default:
        e = (t >> 47) != 0 && (t >> 47) != -1;
        u = ((t >> 47) ^ (t >> 46)) & 1 ? false : true;
        break;
    }
    f |= e ? DSP56300_SR_E : 0;
    f |= u ? DSP56300_SR_U : 0;
    f |= t < 0 ? DSP56300_SR_N : 0;
    f |= t == 0 ? DSP56300_SR_Z : 0;
    f |= v ? DSP56300_SR_V : 0;
    ccr_assign(s, DSP56300_SR_E | DSP56300_SR_U | DSP56300_SR_N |
               DSP56300_SR_Z | DSP56300_SR_V, f);
    if (v) {
        s->sr |= DSP56300_SR_L;
    }
}

/* N and Z of a 24-bit logical result held in bits 47-24; V cleared. */
static void set_ccr_logic(DSP56300State *s, uint32_t a1)
{
    ccr_assign(s, DSP56300_SR_N | DSP56300_SR_Z | DSP56300_SR_V,
               ((a1 >> 23) & 1 ? DSP56300_SR_N : 0) |
               (a1 == 0 ? DSP56300_SR_Z : 0));
}

static AluRes add56(uint64_t a, uint64_t b, unsigned int cin)
{
    uint64_t sum = a + b + cin;
    AluRes r;

    r.res = sum & M56;
    r.c = (sum >> 56) & 1;
    r.v = (~(a ^ b) & (a ^ r.res)) >> 55 & 1;
    return r;
}

static AluRes sub56(uint64_t a, uint64_t b, unsigned int cin)
{
    AluRes r;

    r.res = (a - b - cin) & M56;
    r.c = a < b + cin;
    r.v = ((a ^ b) & (a ^ r.res)) >> 55 & 1;
    return r;
}

/* Arithmetic saturation mode: clamp to 48 bits (Table 3-1). */
static uint64_t sat_sm(DSP56300State *s, uint64_t res, bool *v)
{
    bool b55 = res >> 55 & 1, b48 = res >> 48 & 1, b47 = res >> 47 & 1;

    if (!b55 && (b48 || b47)) {
        *v = true;
        return 0x007fffffffffffULL;
    }
    if (b55 && !(b48 && b47)) {
        *v = true;
        return 0xff800000000000ULL;
    }
    return res;
}

static void commit(DSP56300State *s, unsigned int d, AluRes r, bool sm,
                   bool upd_c)
{
    bool v = r.v;
    uint64_t res = r.res;

    if (sm && (s->sr & DSP56300_SR_SM)) {
        res = sat_sm(s, res, &v);
    }
    s->acc[d] = res;
    dsp_set_ccr_std(s, res, v);
    if (upd_c) {
        set_c(s, r.c);
    }
}

/* Rounding (RND, MPYR, MACR): the scaling mode picks the position. */
static uint64_t round56(DSP56300State *s, uint64_t a, bool *ov)
{
    unsigned int mode = (s->sr >> 10) & 3;
    unsigned int p = mode == 1 ? 24 : mode == 2 ? 22 : 23;
    uint64_t rc = 1ULL << p, mask = (rc << 1) - 1;
    uint64_t sum = (a + rc) & M56;

    *ov = !(a >> 55 & 1) && (sum >> 55 & 1);
    if (!(s->sr & DSP56300_SR_RM) && (sum & mask) == 0) {
        sum &= ~(rc << 1);
    }
    return sum & ~mask;
}

/* ---- operands ---------------------------------------------------------- */

static uint32_t xy_word(const DSP56300State *s, unsigned int jj)
{
    switch (jj & 3) {
    case 0:
        return s->x0;
    case 1:
        return s->y0;
    case 2:
        return s->x1;
    default:
        return s->y1;
    }
}

static uint64_t word56(uint32_t w)
{
    return ((uint64_t)(int64_t)dsp_sx24(w) << 24) & M56;
}

static uint64_t long56(uint32_t hi, uint32_t lo)
{
    uint64_t v = ((uint64_t)hi << 24) | lo;

    return (uint64_t)sextract64(v, 0, 48) & M56;
}

/* Control-register operand of the shift and bit-field instructions. */
static uint32_t ctl_reg(const DSP56300State *s, unsigned int sss)
{
    switch (sss & 7) {
    case 2:
        return dsp_acc1(s->acc[0]);
    case 3:
        return dsp_acc1(s->acc[1]);
    case 4:
        return s->x0;
    case 5:
        return s->y0;
    case 6:
        return s->x1;
    default:
        return s->y1;
    }
}

/* qqq: the INSERT source register. */
static uint32_t ins_reg(const DSP56300State *s, unsigned int qqq)
{
    switch (qqq & 7) {
    case 2:
        return dsp_acc0(s->acc[0]);
    case 3:
        return dsp_acc0(s->acc[1]);
    case 4:
        return s->x0;
    case 5:
        return s->y0;
    case 6:
        return s->x1;
    default:
        return s->y1;
    }
}

/* JJJ source operand of ADD/SUB/CMP/TFR; false when reserved. */
static bool src_jjj(const DSP56300State *s, unsigned int jjj, unsigned int d,
                    uint64_t *out)
{
    switch (jjj & 7) {
    case 0:
        *out = s->acc[d ^ 1];
        return true;
    case 2:
        *out = long56(s->x1, s->x0);
        return true;
    case 3:
        *out = long56(s->y1, s->y0);
        return true;
    case 1:
        return false;
    default:
        *out = word56(xy_word(s, (jjj & 7) - 4));
        return true;
    }
}

/* ---- multiplier -------------------------------------------------------- */

static const uint8_t qqqq_s1[16] = {
    0, 1, 2, 3, 0, 1, 2, 3, 2, 3, 0, 1, 3, 0, 1, 2
};
static const uint8_t qqqq_s2[16] = {
    0, 1, 0, 1, 3, 0, 1, 2, 2, 3, 2, 3, 0, 1, 2, 3
};

/*
 * Multiplier input registers in the table order: X0=0, Y0=1, X1=2, Y1=3.
 * The two tables above hold those indices for each QQQQ code.
 */
static uint32_t mul_in(const DSP56300State *s, unsigned int idx)
{
    return xy_word(s, idx);
}

static int64_t product(uint32_t a, uint32_t b, bool a_signed, bool b_signed)
{
    int64_t x = a_signed ? dsp_sx24(a) : (int64_t)a;
    int64_t y = b_signed ? dsp_sx24(b) : (int64_t)b;

    return (x * y) << 1;
}

/*
 * MPY, MPYR, MAC, MACR with the product already formed (sign applied).
 * kind: 0 MPY, 1 MPYR, 2 MAC, 3 MACR.
 */
static void mul_finish(DSP56300State *s, unsigned int d, int64_t p,
                       unsigned int kind)
{
    AluRes r;
    uint64_t pm = (uint64_t)p & M56;

    if (kind >= 2) {
        r = add56(s->acc[d], pm, 0);
    } else {
        r.res = pm;
        r.v = false;
        r.c = false;
    }
    if (kind & 1) {
        bool ov;

        r.res = round56(s, r.res, &ov);
        r.v |= ov;
    }
    commit(s, d, r, true, false);
}

static void alu_mul(DSP56300State *s, uint32_t op8)
{
    static const uint8_t first[8] = { 0, 1, 2, 3, 0, 1, 2, 3 };
    static const uint8_t second[8] = { 0, 1, 0, 1, 3, 0, 1, 2 };
    unsigned int q = F(op8, 6, 4), d = F(op8, 3, 3), k = F(op8, 2, 2);
    unsigned int kind = op8 & 3;
    int64_t p = product(mul_in(s, first[q]), mul_in(s, second[q]), true, true);

    mul_finish(s, d, k ? -p : p, kind);
}

/* ---- parallel-capable ALU operations ----------------------------------- */

static void do_logic(DSP56300State *s, unsigned int d, unsigned int kind,
                     uint32_t src)
{
    uint32_t a1 = dsp_acc1(s->acc[d]);

    switch (kind) {
    case 0:
        a1 &= src;
        break;
    case 1:
        a1 |= src;
        break;
    default:
        a1 ^= src;
        break;
    }
    s->acc[d] = (s->acc[d] & ~((uint64_t)M24 << 24)) | ((uint64_t)a1 << 24);
    set_ccr_logic(s, a1);
}

static void do_asl1(DSP56300State *s, unsigned int d)
{
    uint64_t v = s->acc[d];
    AluRes r;

    r.res = (v << 1) & M56;
    r.c = v >> 55 & 1;
    r.v = ((v >> 55) ^ (v >> 54)) & 1;
    s->acc[d] = r.res;
    dsp_set_ccr_std(s, r.res, r.v);
    set_c(s, r.c);
}

static void do_asr1(DSP56300State *s, unsigned int d)
{
    uint64_t v = s->acc[d];
    uint64_t res = (uint64_t)(dsp_sx56(v) >> 1) & M56;

    s->acc[d] = res;
    dsp_set_ccr_std(s, res, false);
    set_c(s, v & 1);
}

bool dsp_alu_parallel(DSP56300State *s, uint32_t op8)
{
    unsigned int jjj = F(op8, 6, 4), d = F(op8, 3, 3), kkk = F(op8, 2, 0);
    uint64_t a = s->acc[d], src = 0, abs_a;
    int64_t sa = dsp_sx56(a);
    AluRes r;
    bool ov;

    if (op8 & 0x80) {
        alu_mul(s, op8);
        return true;
    }
    abs_a = (uint64_t)(sa < 0 ? -sa : sa) & M56;
    if (jjj >= 4) {                     /* X0, Y0, X1, Y1 as the operand */
        uint32_t w = xy_word(s, jjj - 4);

        switch (kkk) {
        case 0:
            commit(s, d, add56(a, word56(w), 0), true, true);
            return true;
        case 1:
            s->acc[d] = word56(w);
            return true;
        case 2:
            do_logic(s, d, 1, w);
            return true;
        case 3:
            do_logic(s, d, 2, w);
            return true;
        case 4:
            commit(s, d, sub56(a, word56(w), 0), true, true);
            return true;
        case 5:
            r = sub56(a, word56(w), 0);
            dsp_set_ccr_std(s, r.res, r.v);
            set_c(s, r.c);
            return true;
        case 6:
            do_logic(s, d, 0, w);
            return true;
        default: {
            int64_t sw = dsp_sx24(w);
            int64_t t = sw < 0 ? -sw : sw;

            r = sub56(abs_a, (uint64_t)t << 24 & M56, 0);
            dsp_set_ccr_std(s, r.res, r.v);
            set_c(s, r.c);
            return true;
        }
        }
    }
    switch (jjj) {
    case 0:
        switch (kkk) {
        case 0:
            return d == 0;              /* MOVE; $08 is reserved */
        case 1:  /* TFR */
            s->acc[d] = s->acc[d ^ 1];
            return true;
        case 2:                                                 /* ADDR */
            r = add56((uint64_t)(sa >> 1) & M56, s->acc[d ^ 1], 0);
            commit(s, d, r, true, true);
            return true;
        case 3:                                                 /* TST */
            dsp_set_ccr_std(s, a, false);
            return true;
        case 5:                                                 /* CMP */
            r = sub56(a, s->acc[d ^ 1], 0);
            dsp_set_ccr_std(s, r.res, r.v);
            set_c(s, r.c);
            return true;
        case 6:                                                 /* SUBR */
            r = sub56((uint64_t)(sa >> 1) & M56, s->acc[d ^ 1], 0);
            commit(s, d, r, true, true);
            return true;
        case 7:
            {                                               /* CMPM */
            int64_t so = dsp_sx56(s->acc[d ^ 1]);
            uint64_t abs_o = (uint64_t)(so < 0 ? -so : so) & M56;

            r = sub56(abs_a, abs_o, 0);
            dsp_set_ccr_std(s, r.res, r.v);
            set_c(s, r.c);
            return true;
        }
        default:
            return false;
        }
    case 1:
        switch (kkk) {
        case 0:
            commit(s, d, add56(a, s->acc[d ^ 1], 0), true, true);
            return true;
        case 1:                                                 /* RND */
            r.res = round56(s, a, &ov);
            r.v = ov;
            r.c = false;
            commit(s, d, r, true, false);
            return true;
        case 2:                                                 /* ADDL */
            r = add56((a << 1) & M56, s->acc[d ^ 1], 0);
            r.v |= ((a >> 55) ^ (a >> 54)) & 1;
            commit(s, d, r, true, true);
            return true;
        case 3:                                                 /* CLR */
            s->acc[d] = 0;
            ccr_assign(s, DSP56300_SR_E | DSP56300_SR_U | DSP56300_SR_N |
                       DSP56300_SR_Z | DSP56300_SR_V,
                       DSP56300_SR_U | DSP56300_SR_Z);
            return true;
        case 4:
            commit(s, d, sub56(a, s->acc[d ^ 1], 0), true, true);
            return true;
        case 5:
            {                                               /* MAX(M) */
            int64_t ssrc = dsp_sx56(s->acc[0]), sdst = dsp_sx56(s->acc[1]);
            bool transfer;

            if (d == 0) {
                transfer = (sdst < 0 ? -sdst : sdst) <=
                           (ssrc < 0 ? -ssrc : ssrc);
            } else {
                transfer = sdst - ssrc <= 0;
            }
            if (transfer) {
                s->acc[1] = s->acc[0];
            }
            set_c(s, !transfer);
            return true;
        }
        case 6:                                                 /* SUBL */
            r = sub56((a << 1) & M56, s->acc[d ^ 1], 0);
            r.v |= ((a >> 55) ^ (a >> 54)) & 1;
            commit(s, d, r, true, true);
            return true;
        default:
            {                                              /* NOT */
            uint32_t a1 = ~dsp_acc1(a) & M24;

            s->acc[d] = (a & ~((uint64_t)M24 << 24)) | ((uint64_t)a1 << 24);
            set_ccr_logic(s, a1);
            return true;
        }
        }
    case 2:
    case 3: {
        unsigned int y = jjj & 1;

        src = y ? long56(s->y1, s->y0) : long56(s->x1, s->x0);
        switch (kkk) {
        case 0:
            commit(s, d, add56(a, src, 0), true, true);
            return true;
        case 1:
            commit(s, d, add56(a, src, s->sr & DSP56300_SR_C), true, true);
            return true;
        case 4:
            commit(s, d, sub56(a, src, 0), true, true);
            return true;
        case 5:
            commit(s, d, sub56(a, src, s->sr & DSP56300_SR_C), true, true);
            return true;
        }
        if (y == 0) {
            switch (kkk) {
            case 2:
                do_asr1(s, d);
                return true;
            case 3:
                {                                           /* LSR */
                uint32_t a1 = dsp_acc1(a);

                set_c(s, a1 & 1);
                a1 >>= 1;
                s->acc[d] = (a & ~((uint64_t)M24 << 24)) |
                            ((uint64_t)a1 << 24);
                set_ccr_logic(s, a1);
                return true;
            }
            case 6:                                             /* ABS */
                r.res = abs_a;
                r.v = sa == -(1LL << 55);
                r.c = false;
                commit(s, d, r, true, false);
                return true;
            case 7:
                {                                           /* ROR */
                uint32_t a1 = dsp_acc1(a);
                bool cin = s->sr & DSP56300_SR_C;

                set_c(s, a1 & 1);
                a1 = (a1 >> 1) | (cin ? 0x800000 : 0);
                s->acc[d] = (a & ~((uint64_t)M24 << 24)) |
                            ((uint64_t)a1 << 24);
                set_ccr_logic(s, a1);
                return true;
            }
            }
        } else {
            switch (kkk) {
            case 2:
                do_asl1(s, d);
                return true;
            case 3:
                {                                           /* LSL */
                uint32_t a1 = dsp_acc1(a);

                set_c(s, a1 >> 23 & 1);
                a1 = (a1 << 1) & M24;
                s->acc[d] = (a & ~((uint64_t)M24 << 24)) |
                            ((uint64_t)a1 << 24);
                set_ccr_logic(s, a1);
                return true;
            }
            case 6:                                             /* NEG */
                r.res = (0 - a) & M56;
                r.v = sa == -(1LL << 55);
                r.c = false;
                commit(s, d, r, true, false);
                return true;
            case 7:
                {                                           /* ROL */
                uint32_t a1 = dsp_acc1(a);
                bool cin = s->sr & DSP56300_SR_C;

                set_c(s, a1 >> 23 & 1);
                a1 = ((a1 << 1) | cin) & M24;
                s->acc[d] = (a & ~((uint64_t)M24 << 24)) |
                            ((uint64_t)a1 << 24);
                set_ccr_logic(s, a1);
                return true;
            }
            }
        }
        return false;
    }
    default:
        return false;
    }
}

/* ---- instructions without parallel moves ------------------------------- */

/* ADD/SUB/CMP/AND/OR/EOR with a 6-bit or 24-bit immediate. */
static void imm_alu(DSP56300State *s, uint32_t op, unsigned int kind)
{
    unsigned int d = F(op, 3, 3);
    bool lng = F(op, 6, 6);
    uint32_t imm = lng ? dsp_fetch_ext(s) : F(op, 13, 8);
    uint64_t a = s->acc[d], v56;
    AluRes r;

    if (lng) {
        s->icycles++;
    }
    v56 = lng ? word56(imm) : (uint64_t)imm << 24;
    switch (kind) {
    case 0:
        commit(s, d, add56(a, v56, 0), true, true);
        break;
    case 1:
        commit(s, d, sub56(a, v56, 0), true, true);
        break;
    case 2:
        r = sub56(a, v56, 0);
        dsp_set_ccr_std(s, r.res, r.v);
        set_c(s, r.c);
        break;
    case 3:
        do_logic(s, d, 0, imm);
        break;
    case 4:
        do_logic(s, d, 1, imm);
        break;
    default:
        do_logic(s, d, 2, imm);
        break;
    }
}

static void op_add_imm(DSP56300State *s, uint32_t op) { imm_alu(s, op, 0); }
static void op_sub_imm(DSP56300State *s, uint32_t op) { imm_alu(s, op, 1); }
static void op_cmp_imm(DSP56300State *s, uint32_t op) { imm_alu(s, op, 2); }
static void op_and_imm(DSP56300State *s, uint32_t op) { imm_alu(s, op, 3); }
static void op_or_imm(DSP56300State *s, uint32_t op) { imm_alu(s, op, 4); }
static void op_eor_imm(DSP56300State *s, uint32_t op) { imm_alu(s, op, 5); }

static void op_inc(DSP56300State *s, uint32_t op)
{
    unsigned int d = F(op, 0, 0);

    commit(s, d, add56(s->acc[d], 1, 0), false, true);
}

static void op_dec(DSP56300State *s, uint32_t op)
{
    unsigned int d = F(op, 0, 0);

    commit(s, d, sub56(s->acc[d], 1, 0), false, true);
}

/* Multi-bit arithmetic shifts of a 56-bit accumulator. */
static void shift_acc(DSP56300State *s, unsigned int src, unsigned int dst,
                      unsigned int cnt, bool left)
{
    uint64_t v = s->acc[src], res = v;
    bool c = false, ovf = false;

    if (cnt > 56) {
        cnt = 56;
    }
    if (cnt && left) {
        if (cnt == 56) {
            res = 0;
            c = v & 1;
            ovf = v != 0;
        } else {
            /*
             * V: bit 55 changed at any point, so the bits that pass
             * through it must all equal it.
             */
            uint64_t top = v >> (55 - cnt), all = (1ULL << (cnt + 1)) - 1;

            ovf = top != 0 && top != all;
            c = (v >> (56 - cnt)) & 1;
            res = (v << cnt) & M56;
        }
    } else if (cnt) {
        int64_t sv = dsp_sx56(v);

        c = cnt == 56 ? sv < 0 : (sv >> (cnt - 1)) & 1;
        res = (uint64_t)(cnt == 56 ? (sv < 0 ? -1 : 0) : sv >> cnt) & M56;
    }
    s->acc[dst] = res;
    dsp_set_ccr_std(s, res, ovf);
    set_c(s, c);
}

static void op_asl_imm(DSP56300State *s, uint32_t op)
{
    shift_acc(s, F(op, 7, 7), F(op, 0, 0), F(op, 6, 1), true);
}

static void op_asr_imm(DSP56300State *s, uint32_t op)
{
    shift_acc(s, F(op, 7, 7), F(op, 0, 0), F(op, 6, 1), false);
}

static void op_asl_reg(DSP56300State *s, uint32_t op)
{
    shift_acc(s, F(op, 4, 4), F(op, 0, 0),
              ctl_reg(s, F(op, 3, 1)) & 0x3f, true);
}

static void op_asr_reg(DSP56300State *s, uint32_t op)
{
    shift_acc(s, F(op, 4, 4), F(op, 0, 0),
              ctl_reg(s, F(op, 3, 1)) & 0x3f, false);
}

/* Logical shifts of bits 47-24. */
static void lshift(DSP56300State *s, unsigned int d, unsigned int cnt,
                   bool left)
{
    uint64_t a = s->acc[d];
    uint64_t a1 = dsp_acc1(a);
    bool c = false;

    if (cnt) {
        if (left) {
            c = cnt <= 24 ? (a1 >> (24 - cnt)) & 1 : 0;
            a1 = cnt >= 24 ? 0 : (a1 << cnt) & M24;
        } else {
            c = cnt <= 24 ? (a1 >> (cnt - 1)) & 1 : 0;
            a1 = cnt >= 24 ? 0 : a1 >> cnt;
        }
    }
    s->acc[d] = (a & ~((uint64_t)M24 << 24)) | (a1 << 24);
    set_ccr_logic(s, a1);
    set_c(s, c);
}

static void op_lsl_imm(DSP56300State *s, uint32_t op)
{
    lshift(s, F(op, 0, 0), F(op, 5, 1), true);
}

static void op_lsr_imm(DSP56300State *s, uint32_t op)
{
    lshift(s, F(op, 0, 0), F(op, 5, 1), false);
}

static void op_lsl_reg(DSP56300State *s, uint32_t op)
{
    lshift(s, F(op, 0, 0), ctl_reg(s, F(op, 3, 1)) & 0x1f, true);
}

static void op_lsr_reg(DSP56300State *s, uint32_t op)
{
    lshift(s, F(op, 0, 0), ctl_reg(s, F(op, 3, 1)) & 0x1f, false);
}

static void op_clb(DSP56300State *s, uint32_t op)
{
    uint64_t v = s->acc[F(op, 1, 1)];
    unsigned int d = F(op, 0, 0), count = 0;
    int32_t res;
    bool sign = v >> 55 & 1;

    while (count < 56 && ((v >> (55 - count)) & 1) == sign) {
        count++;
    }
    res = v == 0 ? 0 : 9 - (int32_t)count;
    s->acc[d] = ((uint64_t)(int64_t)res << 24) & M56;
    set_ccr_logic(s, (uint32_t)res & M24);
}

static void op_normf(DSP56300State *s, uint32_t op)
{
    int32_t sh = dsp_sx24(ctl_reg(s, F(op, 3, 1)));
    unsigned int d = F(op, 0, 0);
    bool old_c = s->sr & DSP56300_SR_C;

    if (sh >= 0) {
        shift_acc(s, d, d, sh > 63 ? 63 : sh, false);
    } else {
        shift_acc(s, d, d, -sh > 63 ? 63 : -sh, true);
    }
    set_c(s, old_c);                    /* NORMF leaves C alone */
}

static void op_cmpu(DSP56300State *s, uint32_t op)
{
    unsigned int d = F(op, 0, 0), g = F(op, 3, 1);
    uint64_t a = s->acc[d] & M48, b, res;

    if (g == 0) {
        b = s->acc[d ^ 1] & M48;
    } else if (g >= 4) {
        b = (uint64_t)xy_word(s, g - 4) << 24;
    } else {
        dsp_illegal(s);
        return;
    }
    res = (a - b) & M48;
    ccr_assign(s, DSP56300_SR_N | DSP56300_SR_Z | DSP56300_SR_V |
               DSP56300_SR_C,
               (res >> 47 & 1 ? DSP56300_SR_N : 0) |
               (res == 0 ? DSP56300_SR_Z : 0) |
               (a < b ? DSP56300_SR_C : 0));
}

static void op_div(DSP56300State *s, uint32_t op)
{
    unsigned int d = F(op, 3, 3);
    uint64_t a = s->acc[d];
    uint32_t src = xy_word(s, F(op, 5, 4));
    bool dsign = a >> 55 & 1, ssign = src >> 23 & 1;
    bool c = s->sr & DSP56300_SR_C, v;
    uint64_t sh = ((a << 1) & M56) | c;
    uint64_t sv = word56(src);
    uint64_t res;

    v = ((a >> 55) ^ (a >> 54)) & 1;
    res = (dsign ^ ssign) ? (sh + sv) & M56 : (sh - sv) & M56;
    s->acc[d] = res;
    ccr_assign(s, DSP56300_SR_V | DSP56300_SR_C,
               (v ? DSP56300_SR_V : 0) | (res >> 55 & 1 ? 0 : DSP56300_SR_C));
    if (v) {
        s->sr |= DSP56300_SR_L;
    }
}

static void bitfield(DSP56300State *s, uint32_t op, unsigned int kind,
                     bool imm)
{
    unsigned int d = F(op, 0, 0);
    uint32_t cw = imm ? dsp_fetch_ext(s) : ctl_reg(s, F(op, 3, 1));
    unsigned int off = cw & 0x3f, width = (cw >> 12) & 0x3f;
    uint64_t res, mask = width >= 56 ? M56 : (1ULL << width) - 1;

    if (kind == 2) {                    /* INSERT */
        uint32_t src = ins_reg(s, F(op, 6, 4));
        uint64_t m = (mask << (off > 55 ? 55 : off)) & M56;

        res = (s->acc[d] & ~m) |
              (((uint64_t)src << (off > 55 ? 55 : off)) & m);
        s->acc[d] = res & M56;
    } else {
        unsigned int sacc = F(op, 4, 4);
        uint64_t field = (s->acc[sacc] >> (off > 55 ? 55 : off)) & mask;

        if (kind == 0 && width && (field >> (width - 1) & 1)) {
            field |= ~mask & M56;       /* EXTRACT sign-extends */
        }
        s->acc[d] = field & M56;
        res = s->acc[d];
    }
    dsp_set_ccr_std(s, res & M56, false);
    set_c(s, false);
}

static void op_extract(DSP56300State *s, uint32_t op)
{
    bitfield(s, op, 0, false);
}

static void op_extract_i(DSP56300State *s, uint32_t op)
{
    bitfield(s, op, 0, true);
}

static void op_extractu(DSP56300State *s, uint32_t op)
{
    bitfield(s, op, 1, false);
}

static void op_extractu_i(DSP56300State *s, uint32_t op)
{
    bitfield(s, op, 1, true);
}

static void op_insert(DSP56300State *s, uint32_t op)
{
    bitfield(s, op, 2, false);
}

static void op_insert_i(DSP56300State *s, uint32_t op)
{
    bitfield(s, op, 2, true);
}

static void op_merge(DSP56300State *s, uint32_t op)
{
    unsigned int d = F(op, 0, 0);
    uint32_t src = ctl_reg(s, F(op, 3, 1)) & 0xfff;
    uint32_t a1 = dsp_acc1(s->acc[d]);

    a1 = (src << 12) | ((a1 >> 0) & 0xfff);
    s->acc[d] = (s->acc[d] & ~((uint64_t)M24 << 24)) | ((uint64_t)a1 << 24);
    set_ccr_logic(s, a1);
}

/* MPY/MAC/MPYR/MACR with an immediate power of two. */
static void op_mul_pow2(DSP56300State *s, uint32_t op)
{
    static const uint8_t qq[4] = { 3, 0, 1, 2 };    /* Y1, X0, Y0, X1 */
    unsigned int n = F(op, 12, 8), d = F(op, 3, 3), k = F(op, 2, 2);
    int64_t p;

    if (n < 1 || n > 22) {
        dsp_illegal(s);
        return;
    }
    p = (int64_t)dsp_sx24(xy_word(s, qq[F(op, 5, 4)])) * (1LL << (24 - n));
    mul_finish(s, d, k ? -p : p, op & 3);
}

/* MPYI/MACI/MPYRI/MACRI. */
static void op_mul_imm(DSP56300State *s, uint32_t op)
{
    static const uint8_t qq[4] = { 0, 1, 2, 3 };    /* X0, Y0, X1, Y1 */
    uint32_t imm = dsp_fetch_ext(s);
    unsigned int d = F(op, 3, 3), k = F(op, 2, 2);
    int64_t p = product(imm, xy_word(s, qq[F(op, 5, 4)]), true, true);

    s->icycles++;
    mul_finish(s, d, k ? -p : p, op & 3);
}

/* MPYsu/uu and MACsu/uu. */
static void op_mul_mixed(DSP56300State *s, uint32_t op)
{
    unsigned int q = F(op, 3, 0), d = F(op, 5, 5), k = F(op, 4, 4);
    bool uu = F(op, 6, 6);
    int64_t p = product(xy_word(s, qqqq_s1[q]), xy_word(s, qqqq_s2[q]),
                        !uu, false);
    AluRes r;
    uint64_t pm = (uint64_t)(k ? -p : p) & M56;

    if (op & 0x100) {                   /* MPY, not MAC */
        r.res = pm;
        r.v = false;
    } else {
        r = add56(s->acc[d], pm, 0);
    }
    r.c = false;
    commit(s, d, r, false, false);
}

static void op_dmac(DSP56300State *s, uint32_t op)
{
    unsigned int q = F(op, 3, 0), d = F(op, 5, 5), k = F(op, 4, 4);
    unsigned int ss = (F(op, 8, 8) << 1) | F(op, 6, 6);
    bool a_signed = ss == 0 || ss == 2, b_signed = ss == 0;
    int64_t p;
    uint64_t sh = (uint64_t)(dsp_sx56(s->acc[d]) >> 24) & M56;
    AluRes r;

    if (ss == 1) {
        dsp_illegal(s);
        return;
    }
    p = product(xy_word(s, qqqq_s1[q]), xy_word(s, qqqq_s2[q]), a_signed,
                b_signed);
    r = add56(sh, (uint64_t)(k ? -p : p) & M56, 0);
    commit(s, d, r, false, false);
}

static void op_norm(DSP56300State *s, uint32_t op)
{
    unsigned int rn = F(op, 10, 8), d = F(op, 3, 3);
    unsigned int sr = s->sr;
    bool e = sr & DSP56300_SR_E, u = sr & DSP56300_SR_U;
    bool z = sr & DSP56300_SR_Z;
    uint64_t v = s->acc[d];

    if (!e && u && !z) {
        uint64_t res = (v << 1) & M56;
        bool ovf = ((v >> 55) ^ (v >> 54)) & 1;

        s->acc[d] = res;
        dsp_set_ccr_std(s, res, ovf);
        s->r[rn] = dsp_agu_add(s, rn, -1);
    } else if (e) {
        uint64_t res = (uint64_t)(dsp_sx56(v) >> 1) & M56;

        s->acc[d] = res;
        dsp_set_ccr_std(s, res, false);
        s->r[rn] = dsp_agu_add(s, rn, 1);
    }
}

/* Tcc: transfer on condition. */
static void tcc(DSP56300State *s, uint32_t op, bool acc_move, bool reg_move,
                unsigned int jjj, unsigned int d, unsigned int t,
                unsigned int tt)
{
    if (!dsp_cond(s, F(op, 15, 12))) {
        return;
    }
    if (acc_move) {
        uint64_t src;

        if (!src_jjj(s, jjj, d, &src) || (jjj >= 1 && jjj <= 3)) {
            dsp_illegal(s);
            return;
        }
        s->acc[d] = src;
    }
    if (reg_move) {
        s->r[tt] = s->r[t];
    }
}

static void op_tcc1(DSP56300State *s, uint32_t op)
{
    tcc(s, op, true, false, F(op, 6, 4), F(op, 3, 3), 0, 0);
}

static void op_tcc2(DSP56300State *s, uint32_t op)
{
    tcc(s, op, true, true, F(op, 6, 4), F(op, 3, 3), F(op, 10, 8),
        F(op, 2, 0));
}

static void op_tcc3(DSP56300State *s, uint32_t op)
{
    tcc(s, op, false, true, 0, 0, F(op, 10, 8), F(op, 2, 0));
}

const DspInsn dsp_alu_insns[] = {
    { "00000001 01iiiiii 1000d000", op_add_imm },
    { "00000001 01000000 1100d000", op_add_imm },
    { "00000001 01iiiiii 1000d100", op_sub_imm },
    { "00000001 01000000 1100d100", op_sub_imm },
    { "00000001 01iiiiii 1000d101", op_cmp_imm },
    { "00000001 01000000 1100d101", op_cmp_imm },
    { "00000001 01iiiiii 1000d110", op_and_imm },
    { "00000001 01000000 1100d110", op_and_imm },
    { "00000001 01iiiiii 1000d010", op_or_imm },
    { "00000001 01000000 1100d010", op_or_imm },
    { "00000001 01iiiiii 1000d011", op_eor_imm },
    { "00000001 01000000 1100d011", op_eor_imm },
    { "00000000 00000000 0000100d", op_inc },
    { "00000000 00000000 0000101d", op_dec },
    { "00001100 00011101 SiiiiiiD", op_asl_imm },
    { "00001100 00011100 SiiiiiiD", op_asr_imm },
    { "00001100 00011110 010SsssD", op_asl_reg },
    { "00001100 00011110 011SsssD", op_asr_reg },
    { "00001100 00011110 10iiiiiD", op_lsl_imm },
    { "00001100 00011110 11iiiiiD", op_lsr_imm },
    { "00001100 00011110 0001sssD", op_lsl_reg },
    { "00001100 00011110 0011sssD", op_lsr_reg },
    { "00001100 00011110 0010sssD", op_normf },
    { "00001100 00011110 000000SD", op_clb },
    { "00001100 00011111 1111gggd", op_cmpu },
    { "00001100 00011010 000sSSSD", op_extract },
    { "00001100 00011000 000s000D", op_extract_i },
    { "00001100 00011010 100sSSSD", op_extractu },
    { "00001100 00011000 100s000D", op_extractu_i },
    { "00001100 00011011 0qqqSSSD", op_insert },
    { "00001100 00011001 0qqq000D", op_insert_i },
    { "00001100 00011011 1000SSSD", op_merge },
    { "00000001 10000000 01JJd000", op_div },
    { "00000001 11011RRR 0001d101", op_norm },
    { "00000001 000sssss 11QQdk00", op_mul_pow2 },
    { "00000001 000sssss 11QQdk01", op_mul_pow2 },
    { "00000001 000sssss 11QQdk10", op_mul_pow2 },
    { "00000001 000sssss 11QQdk11", op_mul_pow2 },
    { "00000001 01000001 11qqdk00", op_mul_imm },
    { "00000001 01000001 11qqdk01", op_mul_imm },
    { "00000001 01000001 11qqdk10", op_mul_imm },
    { "00000001 01000001 11qqdk11", op_mul_imm },
    { "00000001 0010010s 1SdkQQQQ", op_dmac },
    { "00000001 00100110 1sdkQQQQ", op_mul_mixed },
    { "00000001 00100111 1sdkQQQQ", op_mul_mixed },
    { "00000010 CCCC0000 0JJJd000", op_tcc1 },
    { "00000011 CCCC0ttt 0JJJdTTT", op_tcc2 },
    { "00000010 CCCC1ttt 00000TTT", op_tcc3 },
    { NULL, NULL }
};
