/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * DSP56300 core: state, memory access, registers, address generation, the
 * system stack, interrupts and the run loop. The instruction handlers are in
 * dsp56300-ops.c (program control, moves) and dsp56300-alu.c (data ALU).
 *
 * Written from the NXP DSP56300 Family Manual, rev 5, and nothing else.
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "dsp56300-int.h"

/* ---- decode table ------------------------------------------------------ */

#define MAX_INSNS 320

typedef struct DecodeEntry {
    uint32_t mask, val;
    unsigned int nfixed;
    DspOpFn fn;
} DecodeEntry;

/*
 * The table is resolved once. The patterns that can match a word are chosen
 * by its top 16 bits: decode_start[prefix] .. decode_start[prefix + 1] in
 * decode_list, most specific first. Most prefixes leave one or two
 * candidates, which the low byte then tells apart.
 */
static DecodeEntry decode_entries[MAX_INSNS];
static unsigned int decode_count;
static uint32_t decode_start[0x10001];
static uint16_t *decode_list;
static GOnce decode_once = G_ONCE_INIT;

static void decode_add(const DspInsn *insn)
{
    DecodeEntry *e = &decode_entries[decode_count];
    unsigned int bit = 24;

    for (const char *p = insn->pat; *p; p++) {
        if (*p == ' ') {
            continue;
        }
        bit--;
        if (*p == '0' || *p == '1') {
            e->mask |= 1u << bit;
            e->val |= (uint32_t)(*p - '0') << bit;
            e->nfixed++;
        }
    }
    assert(bit == 0);
    e->fn = insn->fn;
    decode_count++;
    assert(decode_count <= MAX_INSNS);
}

static gpointer decode_build(gpointer unused)
{
    const DspInsn *tables[] = { dsp_alu_insns, dsp_ctl_insns };
    size_t total = 0;

    for (unsigned int t = 0; t < ARRAY_SIZE(tables); t++) {
        for (const DspInsn *i = tables[t]; i->pat; i++) {
            decode_add(i);
        }
    }
    /* Most specific first, so a narrow pattern wins over a wide one. */
    for (unsigned int i = 1; i < decode_count; i++) {
        DecodeEntry e = decode_entries[i];
        unsigned int j = i;

        while (j > 0 && decode_entries[j - 1].nfixed < e.nfixed) {
            decode_entries[j] = decode_entries[j - 1];
            j--;
        }
        decode_entries[j] = e;
    }
    /* two passes: count, then fill */
    for (int pass = 0; pass < 2; pass++) {
        size_t n = 0;

        for (uint32_t p = 0; p < 0x10000; p++) {
            decode_start[p] = n;
            for (unsigned int i = 0; i < decode_count; i++) {
                const DecodeEntry *e = &decode_entries[i];

                if (((p << 8) & e->mask & 0xffff00) ==
                    (e->val & 0xffff00)) {
                    if (pass) {
                        decode_list[n] = i;
                    }
                    n++;
                }
            }
        }
        decode_start[0x10000] = n;
        if (!pass) {
            total = n;
            decode_list = g_new(uint16_t, total);
        }
    }
    return NULL;
}

static void dispatch(DSP56300State *s, uint32_t op)
{
    uint32_t p = op >> 8;

    for (uint32_t k = decode_start[p]; k < decode_start[p + 1]; k++) {
        const DecodeEntry *e = &decode_entries[decode_list[k]];

        if ((op & e->mask) == e->val) {
            e->fn(s, op);
            return;
        }
    }
    dsp_illegal(s);
}

/* ---- memory ------------------------------------------------------------ */

uint32_t dsp_read(DSP56300State *s, DSP56300Space sp, uint32_t addr)
{
    addr &= M24;
    if (s->mem[sp] && addr < s->mem_words[sp]) {
        return s->mem[sp][addr] & M24;
    }
    return s->ops.read(s->opaque, sp, addr) & M24;
}

void dsp_write(DSP56300State *s, DSP56300Space sp, uint32_t addr,
               uint32_t val)
{
    addr &= M24;
    val &= M24;
    s->wcount++;
    if (s->mem[sp] && addr < s->mem_words[sp]) {
        s->mem[sp][addr] = val;
        return;
    }
    s->ops.write(s->opaque, sp, addr, val);
}

uint32_t dsp_fetch_ext(DSP56300State *s)
{
    return dsp_read(s, DSP56300_SPACE_P, s->ipc + 1 + s->ext++);
}

/* ---- accumulator access ------------------------------------------------ */

static unsigned int scale_mode(const DSP56300State *s)
{
    return (s->sr >> 10) & 3;
}

static void note_scaling(DSP56300State *s, int64_t v)
{
    bool grow;

    switch (scale_mode(s)) {
    case 1:                             /* scale down */
        grow = ((v >> 47) ^ (v >> 46)) & 1;
        break;
    case 2:                             /* scale up */
        grow = ((v >> 45) ^ (v >> 44)) & 1;
        break;
    default:
        grow = ((v >> 46) ^ (v >> 45)) & 1;
        break;
    }
    if (grow) {
        s->sr |= DSP56300_SR_S;
    }
}

uint32_t dsp_acc_bus(DSP56300State *s, unsigned int which)
{
    int64_t v = dsp_sx56(s->acc[which]);
    unsigned int mode = scale_mode(s);
    int64_t t;

    note_scaling(s, v);
    t = v >> (mode == 1 ? 25 : mode == 2 ? 23 : 24);
    if (t > 0x7fffff) {
        t = 0x7fffff;
        s->sr |= DSP56300_SR_L;
    } else if (t < -0x800000) {
        t = -0x800000;
        s->sr |= DSP56300_SR_L;
    }
    return t & M24;
}

void dsp_acc_bus48(DSP56300State *s, unsigned int which, uint32_t *hi,
                   uint32_t *lo)
{
    int64_t v = dsp_sx56(s->acc[which]);
    unsigned int mode = scale_mode(s);
    int64_t t;

    note_scaling(s, v);
    t = mode == 1 ? v >> 1 : mode == 2 ? v * 2 : v;
    if (t > 0x7fffffffffffLL) {
        t = 0x7fffffffffffLL;
        s->sr |= DSP56300_SR_L;
    } else if (t < -0x800000000000LL) {
        t = -0x800000000000LL;
        s->sr |= DSP56300_SR_L;
    }
    *hi = (t >> 24) & M24;
    *lo = t & M24;
}

void dsp_acc_write24(DSP56300State *s, unsigned int which, uint32_t val)
{
    s->acc[which] = ((uint64_t)(int64_t)dsp_sx24(val) << 24) & M56;
}

/* ---- registers --------------------------------------------------------- */

#define SR_MASK 0x00fbefffu
#define OMR_MASK 0x00ffffdfu
#define OMR_SEN  (1u << 20)

void dsp_set_sr(DSP56300State *s, uint32_t val)
{
    val &= SR_MASK;
    if ((val & (DSP56300_SR_SA | DSP56300_SR_DM)) &&
        !(s->sr & (DSP56300_SR_SA | DSP56300_SR_DM))) {
        qemu_log_mask(LOG_UNIMP, "dsp56300: SR.SA/SR.DM set (%06x): "
                      "sixteen-bit arithmetic and double-precision multiply "
                      "are not modelled\n", val);
    }
    s->sr = val;
}

void dsp_set_omr(DSP56300State *s, uint32_t val)
{
    val &= OMR_MASK;
    if ((val & OMR_SEN) && !(s->omr & OMR_SEN)) {
        qemu_log_mask(LOG_UNIMP, "dsp56300: OMR.SEN set: the stack extension "
                      "is not modelled\n");
    }
    s->omr = val;
}

static uint32_t stack_peek(const DSP56300State *s, unsigned int half)
{
    return s->ss[s->sp & 0xf][half];
}

static uint32_t reg_peek(const DSP56300State *s, unsigned int code)
{
    switch (code) {
    case DSP56300_REG_X0:
        return s->x0;
    case DSP56300_REG_X1:
        return s->x1;
    case DSP56300_REG_Y0:
        return s->y0;
    case DSP56300_REG_Y1:
        return s->y1;
    case DSP56300_REG_A0:
        return dsp_acc0(s->acc[0]);
    case DSP56300_REG_B0:
        return dsp_acc0(s->acc[1]);
    case DSP56300_REG_A2:
        return (uint32_t)(int32_t)(int8_t)dsp_acc2(s->acc[0])
                                 & M24;
    case DSP56300_REG_B2:
        return (uint32_t)(int32_t)(int8_t)dsp_acc2(s->acc[1])
                                 & M24;
    case DSP56300_REG_A1:
    case DSP56300_REG_A:
        return dsp_acc1(s->acc[0]);
    case DSP56300_REG_B1:
    case DSP56300_REG_B:
        return dsp_acc1(s->acc[1]);
    case DSP56300_REG_EP:
        return s->ep;
    case DSP56300_REG_VBA:
        return s->vba & 0xffff00;
    case DSP56300_REG_SC:
        return s->sc & 0x1f;
    case DSP56300_REG_SZ:
        return s->sz;
    case DSP56300_REG_SR:
        return s->sr;
    case DSP56300_REG_OMR:
        return s->omr;
    case DSP56300_REG_SP:
        return s->sp;
    case DSP56300_REG_SSH:
        return stack_peek(s, 0);
    case DSP56300_REG_SSL:
        return stack_peek(s, 1);
    case DSP56300_REG_LA:
        return s->la;
    case DSP56300_REG_LC:
        return s->lc;
    default:
        break;
    }
    if (code >= DSP56300_REG_R0 && code < DSP56300_REG_R0 + 8) {
        return s->r[code - DSP56300_REG_R0];
    }
    if (code >= DSP56300_REG_N0 && code < DSP56300_REG_N0 + 8) {
        return s->n[code - DSP56300_REG_N0];
    }
    if (code >= DSP56300_REG_M0 && code < DSP56300_REG_M0 + 8) {
        return s->m[code - DSP56300_REG_M0];
    }
    return 0;
}

/* The register codes the instruction set defines. */
static bool reg_valid(unsigned int code)
{
    return (code >= DSP56300_REG_X0 && code < DSP56300_REG_M0 + 8) ||
           code == DSP56300_REG_EP || code == DSP56300_REG_VBA ||
           code == DSP56300_REG_SC || code >= DSP56300_REG_SZ;
}

uint32_t dsp_reg_read(DSP56300State *s, unsigned int code)
{
    uint32_t hi, lo;

    if (!reg_valid(code)) {
        qemu_log_mask(LOG_GUEST_ERROR, "dsp56300: read of reserved register "
                      "code 0x%02x at P:%06x\n", code, s->ipc);
        return 0;
    }
    switch (code) {
    case DSP56300_REG_A:
        return dsp_acc_bus(s, 0);
    case DSP56300_REG_B:
        return dsp_acc_bus(s, 1);
    case DSP56300_REG_SSH:
        dsp_pull(s, &hi, &lo);
        return hi;
    default:
        return reg_peek(s, code);
    }
}

static void acc_set_part(DSP56300State *s, unsigned int w, unsigned int part,
                         uint32_t val)
{
    uint64_t *a = &s->acc[w];

    switch (part) {
    case 0:
        *a = (*a & ~(uint64_t)M24) | (val & M24);
        break;
    case 1:
        *a = (*a & ~((uint64_t)M24 << 24)) | ((uint64_t)(val & M24) << 24);
        break;
    default:
        *a = (*a & ~(0xffULL << 48)) | ((uint64_t)(val & 0xff) << 48);
        break;
    }
}

void dsp_reg_write(DSP56300State *s, unsigned int code, uint32_t val)
{
    val &= M24;
    if (!reg_valid(code)) {
        qemu_log_mask(LOG_GUEST_ERROR, "dsp56300: write of reserved register "
                      "code 0x%02x at P:%06x\n", code, s->ipc);
        return;
    }
    switch (code) {
    case DSP56300_REG_X0:
        s->x0 = val;
        return;
    case DSP56300_REG_X1:
        s->x1 = val;
        return;
    case DSP56300_REG_Y0:
        s->y0 = val;
        return;
    case DSP56300_REG_Y1:
        s->y1 = val;
        return;
    case DSP56300_REG_A0:
        acc_set_part(s, 0, 0, val);
        return;
    case DSP56300_REG_B0:
        acc_set_part(s, 1, 0, val);
        return;
    case DSP56300_REG_A1:
        acc_set_part(s, 0, 1, val);
        return;
    case DSP56300_REG_B1:
        acc_set_part(s, 1, 1, val);
        return;
    case DSP56300_REG_A2:
        acc_set_part(s, 0, 2, val);
        return;
    case DSP56300_REG_B2:
        acc_set_part(s, 1, 2, val);
        return;
    case DSP56300_REG_A:
        dsp_acc_write24(s, 0, val);
        return;
    case DSP56300_REG_B:
        dsp_acc_write24(s, 1, val);
        return;
    case DSP56300_REG_EP:
        s->ep = val;
        return;
    case DSP56300_REG_VBA:
        s->vba = val & 0xffff00;
        return;
    case DSP56300_REG_SC:
        s->sc = val & 0x1f;
        return;
    case DSP56300_REG_SZ:
        s->sz = val;
        return;
    case DSP56300_REG_SR:
        dsp_set_sr(s, val);
        return;
    case DSP56300_REG_OMR:
        dsp_set_omr(s, val);
        return;
    case DSP56300_REG_SP:
        s->sp = val & 0x3f;
        return;
    case DSP56300_REG_SSH:
        /* the stack pointer moves first, then the high half is written */
        dsp_push(s, val, s->ss[(s->sp + 1) & 0xf][1]);
        return;
    case DSP56300_REG_SSL:
        s->ss[s->sp & 0xf][1] = val;
        return;
    case DSP56300_REG_LA:
        s->la = val;
        return;
    case DSP56300_REG_LC:
        s->lc = val;
        return;
    default:
        break;
    }
    if (code >= DSP56300_REG_R0 && code < DSP56300_REG_R0 + 8) {
        s->r[code - DSP56300_REG_R0] = val;
    } else if (code >= DSP56300_REG_N0 && code < DSP56300_REG_N0 + 8) {
        s->n[code - DSP56300_REG_N0] = val;
    } else if (code >= DSP56300_REG_M0 && code < DSP56300_REG_M0 + 8) {
        s->m[code - DSP56300_REG_M0] = val;
    }
}

uint32_t dsp56300_get_reg(const DSP56300State *s, unsigned int reg)
{
    return reg_peek(s, reg);
}

void dsp56300_set_reg(DSP56300State *s, unsigned int reg, uint32_t val)
{
    if (reg == DSP56300_REG_SSH) {
        s->ss[s->sp & 0xf][0] = val & M24;
        return;
    }
    dsp_reg_write(s, reg, val);
}

int64_t dsp56300_get_acc(const DSP56300State *s, unsigned int which)
{
    return dsp_sx56(s->acc[which & 1]);
}

void dsp56300_set_acc(DSP56300State *s, unsigned int which, int64_t val)
{
    s->acc[which & 1] = (uint64_t)val & M56;
}

/* ---- address generation ------------------------------------------------ */

static uint32_t rev24(uint32_t v)
{
    return revbit32(v) >> 8;
}

/*
 * Add a signed offset to address register rn with the arithmetic its
 * modifier register selects: linear, bit-reverse, modulo, or multiple
 * wrap-around modulo.
 */
uint32_t dsp_agu_add(const DSP56300State *s, unsigned int rn, int32_t off)
{
    uint32_t r = s->r[rn], m = s->m[rn], lo = m & 0xffff;
    uint32_t mod, block;

    if (lo == 0xffff) {
        return (r + off) & M24;
    }
    if (lo == 0) {
        uint32_t mag = off < 0 ? -(uint32_t)off : off;

        mag = rev24(mag & M24);
        return rev24(off < 0 ? rev24(r) - mag : rev24(r) + mag) & M24;
    }
    if (lo < 0x8000) {
        mod = lo + 1;
        block = pow2ceil(mod);
        uint32_t mag = off < 0 ? -(uint32_t)off : off;

        if (mag > mod && (mag & (block - 1)) == 0) {
            /* a multiple of the block size: same place, other block */
            return (r + off) & M24;
        }
    } else if ((lo & 0xc000) == 0x8000) {
        mod = (lo & 0x3fff) + 1;
        block = pow2ceil(mod);
    } else {
        return (r + off) & M24;         /* reserved encoding: linear */
    }
    {
        uint32_t base = r & ~(block - 1), pos = r & (block - 1);
        int64_t t;

        if (pos >= mod) {
            return (r + off) & M24;     /* outside the buffer: undefined */
        }
        t = ((int64_t)pos + off) % (int64_t)mod;
        if (t < 0) {
            t += mod;
        }
        return (base | (uint32_t)t) & M24;
    }
}

bool dsp_ea(DSP56300State *s, unsigned int mmmrrr, uint32_t *val, bool *imm)
{
    unsigned int mode = (mmmrrr >> 3) & 7, rn = mmmrrr & 7;
    int32_t n = dsp_sx24(s->n[rn]);

    *imm = false;
    switch (mode) {
    case 0:                             /* (Rn)-Nn */
        *val = s->r[rn];
        s->r[rn] = dsp_agu_add(s, rn, (int32_t)-(int64_t)n);
        break;
    case 1:                             /* (Rn)+Nn */
        *val = s->r[rn];
        s->r[rn] = dsp_agu_add(s, rn, n);
        break;
    case 2:                             /* (Rn)- */
        *val = s->r[rn];
        s->r[rn] = dsp_agu_add(s, rn, -1);
        break;
    case 3:                             /* (Rn)+ */
        *val = s->r[rn];
        s->r[rn] = dsp_agu_add(s, rn, 1);
        break;
    case 4:                             /* (Rn) */
        *val = s->r[rn];
        break;
    case 5:                             /* (Rn+Nn) */
        *val = dsp_agu_add(s, rn, n);
        s->icycles++;
        break;
    case 6:
        if (rn == 0) {                  /* absolute address */
            *val = dsp_fetch_ext(s);
            s->icycles++;
        } else if (rn == 4) {           /* immediate data */
            *val = dsp_fetch_ext(s);
            *imm = true;
            s->icycles++;
        } else {
            return false;
        }
        break;
    default:                            /* -(Rn) */
        s->r[rn] = dsp_agu_add(s, rn, -1);
        *val = s->r[rn];
        s->icycles++;
        break;
    }
    return true;
}

/* ---- system stack ------------------------------------------------------ */

/*
 * SP[3:0] is the pointer; SE (bit 4) and UF (bit 5) are sticky flags that
 * only a write to SP clears (Table 5-2). A push past location 15 sets SE and
 * wraps the pointer, a pull from location 0 sets UF and SE and wraps the
 * other way. The stack-error exception is taken when SE goes from 0 to 1.
 */
static void stack_error(DSP56300State *s, uint32_t flags)
{
    bool first = !(s->sp & SP_SE);

    s->sp |= flags;
    if (first) {
        dsp56300_irq_raise(s, DSP56300_VEC_STACK_ERROR, 3);
    }
}

void dsp_push(DSP56300State *s, uint32_t hi, uint32_t lo)
{
    uint32_t ptr = s->sp & 0xf;

    if (ptr == 0xf) {
        stack_error(s, SP_SE);
    }
    s->sp = (s->sp & ~0xfu) | ((ptr + 1) & 0xf);
    s->ss[s->sp & 0xf][0] = hi & M24;
    s->ss[s->sp & 0xf][1] = lo & M24;
    s->wcount++;
}

void dsp_pull(DSP56300State *s, uint32_t *hi, uint32_t *lo)
{
    uint32_t ptr = s->sp & 0xf;

    *hi = s->ss[ptr][0];
    *lo = s->ss[ptr][1];
    if (ptr == 0) {
        stack_error(s, SP_SE | SP_UF);
    }
    s->sp = (s->sp & ~0xfu) | ((ptr - 1) & 0xf);
}

/* ---- flow -------------------------------------------------------------- */

uint32_t dsp_ret_addr(const DSP56300State *s)
{
    return s->in_int ? s->pc : (s->ipc + 1 + s->ext) & M24;
}

static uint64_t state_hash(const DSP56300State *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    uint64_t w[] = {
        s->acc[0], s->acc[1], s->x0, s->x1, s->y0, s->y1, s->sr, s->omr,
        s->la, s->lc, s->sp, s->ss[s->sp & 0xf][0], s->ss[s->sp & 0xf][1],
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(w); i++) {
        h = (h ^ w[i]) * 0x100000001b3ULL;
    }
    for (unsigned int i = 0; i < 8; i++) {
        h = (h ^ (((uint64_t)s->r[i] << 24) | s->n[i])) * 0x100000001b3ULL;
        h = (h ^ s->m[i]) * 0x100000001b3ULL;
    }
    return h;
}

/*
 * A taken backward branch of a few words. If the architectural state is the
 * same as the last time this branch was taken and nothing has been stored in
 * between, the loop cannot change anything by itself: it is waiting for the
 * outside world.
 */
void dsp_note_branch(DSP56300State *s, uint32_t target)
{
    uint64_t h;

    if (s->in_int || target > s->ipc || s->ipc - target > 16) {
        return;
    }
    h = state_hash(s);
    if (s->idle_pc == s->ipc && s->idle_hash == h &&
        s->idle_wcount == s->wcount) {
        s->idle = true;
    } else {
        s->idle_pc = s->ipc;
        s->idle_hash = h;
        s->idle_wcount = s->wcount;
    }
}

void dsp_jump(DSP56300State *s, uint32_t target)
{
    if (dsp_rep_forbidden(s)) {
        return;
    }
    s->npc = target & M24;
    s->flow |= FLOW_JUMP;
    dsp_note_branch(s, target);
}

void dsp_call(DSP56300State *s, uint32_t target)
{
    if (dsp_rep_forbidden(s)) {
        return;
    }
    dsp_push(s, dsp_ret_addr(s), s->sr);
    s->npc = target & M24;
    s->flow |= FLOW_JUMP | FLOW_CALL;
}

void dsp_illegal(DSP56300State *s)
{
    qemu_log_mask(LOG_GUEST_ERROR, "dsp56300: illegal instruction at P:%06x\n",
                  s->ipc);
    dsp56300_irq_raise(s, DSP56300_VEC_ILLEGAL, 3);
}

/*
 * The instruction after REP is fetched once and repeated, so it cannot
 * change the flow, start a loop or stop the clock (appendix A, REP
 * restrictions). Such an instruction is illegal and ends the REP.
 */
bool dsp_rep_forbidden(DSP56300State *s)
{
    if (!s->in_rep) {
        return false;
    }
    s->rep_illegal = true;
    dsp_illegal(s);
    return true;
}

/* ---- interrupts -------------------------------------------------------- */

static void irq_set(DSP56300State *s, uint32_t vector, unsigned int ipl,
                    bool level)
{
    unsigned int idx = (vector >> 1) & 0x7f;

    if (!s->irq_pending[idx]) {
        s->irq_pending[idx] = 1;
        s->irq_count++;
    }
    s->irq_ipl[idx] = ipl & 3;
    s->irq_level[idx] = level;
    s->irq_best_valid = false;
    s->idle = false;
}

void dsp56300_irq_raise(DSP56300State *s, uint32_t vector, unsigned int ipl)
{
    irq_set(s, vector, ipl, false);
}

void dsp56300_irq_raise_level(DSP56300State *s, uint32_t vector,
                              unsigned int ipl)
{
    irq_set(s, vector, ipl, true);
}

void dsp56300_irq_lower(DSP56300State *s, uint32_t vector)
{
    unsigned int idx = (vector >> 1) & 0x7f;

    if (s->irq_pending[idx]) {
        s->irq_pending[idx] = 0;
        s->irq_count--;
        s->irq_best_valid = false;
    }
}

static unsigned int exec_insn(DSP56300State *s, uint32_t addr, bool in_int)
{
    uint32_t op = dsp_read(s, DSP56300_SPACE_P, addr);

    s->ipc = addr & M24;
    s->ext = 0;
    s->flow = 0;
    s->icycles = 1;
    s->in_int = in_int;
    s->npc = (addr + 1) & M24;
    dispatch(s, op);
    if (!(s->flow & FLOW_JUMP)) {
        s->npc = (addr + 1 + s->ext) & M24;
    }
    return 1 + s->ext;
}

/*
 * Highest priority acceptable interrupt, or -1; the lowest vector wins a
 * tie. The result is cached: it only changes when something is raised,
 * lowered or taken, or when SR.I changes, so a masked request that stays
 * pending costs a compare per instruction, not a scan.
 */
static int irq_arbitrate(DSP56300State *s)
{
    unsigned int mask = (s->sr >> 8) & 3;
    int best = -1;

    if (!s->irq_count) {
        return -1;
    }
    if (s->irq_best_valid && s->irq_best_mask == mask) {
        return s->irq_best;
    }
    for (unsigned int i = 0; i < 128; i++) {
        if (!s->irq_pending[i]) {
            continue;
        }
        if (s->irq_ipl[i] != 3 && s->irq_ipl[i] < mask) {
            continue;
        }
        if (best < 0 || s->irq_ipl[i] > s->irq_ipl[best]) {
            best = i;
        }
    }
    s->irq_best = best;
    s->irq_best_mask = mask;
    s->irq_best_valid = true;
    return best;
}

/*
 * Run the two words at the vector. If either is a subroutine call the
 * interrupt is long: the call stacked the return address and SR, and the
 * mask is raised here. Any other change of flow abandons the second word.
 */
static unsigned int irq_service(DSP56300State *s, unsigned int idx)
{
    uint32_t vec = ((s->vba & 0xffff00) + idx * 2) & M24;
    unsigned int ipl = s->irq_ipl[idx], cycles = 0, len;

    if (!s->irq_level[idx]) {
        s->irq_pending[idx] = 0;
        s->irq_count--;
    }
    s->irq_best_valid = false;
    for (unsigned int word = 0; word < 2;) {
        len = exec_insn(s, vec + word, true);
        cycles += s->icycles;
        if (s->flow & FLOW_CALL) {
            s->sr &= ~(DSP56300_SR_LF | DSP56300_SR_S0 | DSP56300_SR_S1 |
                       DSP56300_SR_SA | DSP56300_SR_I0 | DSP56300_SR_I1);
            s->sr |= (ipl < 3 ? ipl + 1 : 3) << 8;
            s->pc = s->npc;
            break;
        }
        if (s->flow & FLOW_JUMP) {
            s->pc = s->npc;
            break;
        }
        word += len;
    }
    s->in_int = false;
    return cycles ? cycles : 1;
}

static unsigned int irq_take(DSP56300State *s)
{
    int idx;

    if (s->rep_active) {
        return 0;
    }
    idx = irq_arbitrate(s);
    if (idx < 0) {
        return 0;
    }
    s->run_state = DSP56300_RUNNING;
    return irq_service(s, idx);
}

/* ---- run loop ---------------------------------------------------------- */

static void finish_insn(DSP56300State *s, unsigned int len)
{
    if ((s->sr & DSP56300_SR_LF) && !(s->flow & FLOW_JUMP) &&
        ((s->ipc + len - 1) & M24) == s->la) {
        uint32_t hi, lo, la, lc;

        if ((s->sr & DSP56300_SR_FV) || s->lc != 1) {
            s->lc = (s->lc - 1) & M24;
            s->npc = s->ss[s->sp & 0xf][0];
        } else {
            dsp_pull(s, &hi, &lo);
            s->sr = (s->sr & ~(DSP56300_SR_LF | DSP56300_SR_FV)) |
                    (lo & (DSP56300_SR_LF | DSP56300_SR_FV));
            dsp_pull(s, &la, &lc);
            s->la = la;
            s->lc = lc;
        }
    }
    s->pc = s->npc;
}

unsigned int dsp56300_step(DSP56300State *s)
{
    unsigned int cyc, len;

    if (s->run_state == DSP56300_STOP) {
        /* only IRQA, even masked, restarts the clock (manual 2.3.5) */
        if (!s->irq_pending[DSP56300_VEC_IRQA >> 1]) {
            s->idle = true;
            return 0;
        }
        s->run_state = DSP56300_RUNNING;
    } else if (s->run_state == DSP56300_WAIT) {
        cyc = irq_take(s);
        if (!cyc) {
            s->idle = true;
        }
        return cyc;
    }
    if (s->rep_active) {
        s->lc = s->rep_count;
        s->in_rep = true;
        len = exec_insn(s, s->rep_ipc, false);
        s->in_rep = false;
        cyc = s->icycles;
        if (s->rep_illegal || --s->rep_count == 0) {
            s->rep_illegal = false;
            s->rep_active = false;
            s->lc = s->rep_save;
            finish_insn(s, len);
        }
        return cyc;
    }
    cyc = irq_take(s);
    if (cyc) {
        return cyc;
    }
    len = exec_insn(s, s->pc, false);
    cyc = s->icycles;
    finish_insn(s, len);
    return cyc;
}

uint64_t dsp56300_run(DSP56300State *s, uint64_t max_cycles)
{
    uint64_t used = 0;

    s->idle = false;
    while (used < max_cycles) {
        unsigned int c = dsp56300_step(s);

        used += c;
        s->cycles += c;
        if (s->idle) {
            break;
        }
    }
    return used;
}

/* ---- setup ------------------------------------------------------------- */

void dsp56300_init(DSP56300State *s, const DSP56300Ops *ops, void *opaque)
{
    assert(ops->read && ops->write);
    g_once(&decode_once, decode_build, NULL);
    memset(s, 0, sizeof(*s));
    s->ops = *ops;
    s->opaque = opaque;
    dsp56300_reset(s, 0);
}

void dsp56300_set_memory(DSP56300State *s, DSP56300Space space,
                         uint32_t *words, uint32_t nwords)
{
    s->mem[space] = words;
    s->mem_words[space] = nwords;
}

void dsp56300_reset(DSP56300State *s, uint32_t boot_pc)
{
    memset(s->acc, 0, sizeof(s->acc));
    s->x0 = s->x1 = s->y0 = s->y1 = 0;
    memset(s->r, 0, sizeof(s->r));
    memset(s->n, 0, sizeof(s->n));
    for (unsigned int i = 0; i < 8; i++) {
        s->m[i] = M24;
    }
    memset(s->ss, 0, sizeof(s->ss));
    s->sr = DSP56300_SR_RESET;
    s->omr = 0;
    s->la = s->lc = s->vba = s->sz = s->sc = s->ep = s->sp = 0;
    s->pc = boot_pc & M24;
    s->run_state = DSP56300_RUNNING;
    s->cycles = 0;
    s->icycles = 0;
    s->idle = false;
    memset(s->irq_pending, 0, sizeof(s->irq_pending));
    memset(s->irq_level, 0, sizeof(s->irq_level));
    s->irq_count = 0;
    s->irq_best_valid = false;
    s->in_int = false;
    s->in_rep = false;
    s->rep_illegal = false;
    s->rep_active = false;
    s->wcount = 0;
    s->idle_pc = 0xffffffff;
}

/* ---- migration ----------------------------------------------------------- */

static int dsp56300_post_load(void *opaque, int version_id)
{
    DSP56300State *s = opaque;
    unsigned int count = 0;
    const uint32_t *regs[] = {
        &s->x0, &s->x1, &s->y0, &s->y1, &s->pc, &s->sr, &s->omr, &s->la,
        &s->lc, &s->vba, &s->sz, &s->sc, &s->ep, &s->rep_ipc, &s->rep_save,
    };

    /* every value that later indexes an array or forms an address */
    if (s->run_state > DSP56300_STOP || s->sp > 0x3f ||
        s->rep_count > 0x1000000 || (s->rep_active && !s->rep_count)) {
        return -EINVAL;
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(regs); i++) {
        if (*regs[i] > M24) {
            return -EINVAL;
        }
    }
    for (unsigned int i = 0; i < 8; i++) {
        if (s->r[i] > M24 || s->n[i] > M24 || s->m[i] > M24) {
            return -EINVAL;
        }
    }
    for (unsigned int i = 0; i < 2; i++) {
        if (s->acc[i] > M56) {
            return -EINVAL;
        }
    }
    for (unsigned int i = 0; i < DSP56300_STACK_DEPTH; i++) {
        if (s->ss[i][0] > M24 || s->ss[i][1] > M24) {
            return -EINVAL;
        }
    }
    for (unsigned int i = 0; i < 128; i++) {
        if (s->irq_pending[i] > 1 || s->irq_level[i] > 1 ||
            s->irq_ipl[i] > 3) {
            return -EINVAL;
        }
        count += s->irq_pending[i];
    }
    s->irq_count = count;
    s->irq_best_valid = false;
    s->idle = false;
    s->idle_pc = 0xffffffff;
    s->in_int = false;
    s->in_rep = false;
    s->rep_illegal = false;
    return 0;
}

const VMStateDescription vmstate_dsp56300 = {
    .name = "dsp56300",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = dsp56300_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(acc, DSP56300State, 2),
        VMSTATE_UINT32(x0, DSP56300State),
        VMSTATE_UINT32(x1, DSP56300State),
        VMSTATE_UINT32(y0, DSP56300State),
        VMSTATE_UINT32(y1, DSP56300State),
        VMSTATE_UINT32_ARRAY(r, DSP56300State, 8),
        VMSTATE_UINT32_ARRAY(n, DSP56300State, 8),
        VMSTATE_UINT32_ARRAY(m, DSP56300State, 8),
        VMSTATE_UINT32(pc, DSP56300State),
        VMSTATE_UINT32(sr, DSP56300State),
        VMSTATE_UINT32(omr, DSP56300State),
        VMSTATE_UINT32(la, DSP56300State),
        VMSTATE_UINT32(lc, DSP56300State),
        VMSTATE_UINT32(vba, DSP56300State),
        VMSTATE_UINT32(sz, DSP56300State),
        VMSTATE_UINT32(sc, DSP56300State),
        VMSTATE_UINT32(ep, DSP56300State),
        VMSTATE_UINT32(sp, DSP56300State),
        VMSTATE_UINT32_2DARRAY(ss, DSP56300State, DSP56300_STACK_DEPTH, 2),
        VMSTATE_UINT32(run_state, DSP56300State),
        VMSTATE_UINT64(cycles, DSP56300State),
        VMSTATE_BOOL(rep_active, DSP56300State),
        VMSTATE_UINT32(rep_count, DSP56300State),
        VMSTATE_UINT32(rep_save, DSP56300State),
        VMSTATE_UINT32(rep_ipc, DSP56300State),
        VMSTATE_UINT8_ARRAY(irq_pending, DSP56300State, 128),
        VMSTATE_UINT8_ARRAY(irq_ipl, DSP56300State, 128),
        VMSTATE_UINT8_ARRAY(irq_level, DSP56300State, 128),
        VMSTATE_END_OF_LIST()
    }
};
