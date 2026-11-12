/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * DSP56300 core: declarations shared between the core, the data ALU and the
 * instruction handlers. Not a device interface; see dsp56300.h in
 * include/hw/coproc/dsp56300/.
 */
#ifndef HW_COPROC_DSP56300_INT_H
#define HW_COPROC_DSP56300_INT_H

#include "qemu/bitops.h"
#include "hw/coproc/dsp56300/dsp56300.h"

#define M24 0x00ffffffu
#define M56 0x00ffffffffffffffULL

#define SR_CCR  0xffu
#define SR_STICKY_MASK 0xc0u            /* S and L */

/* Flow flags a handler leaves in s->flow. */
#define FLOW_JUMP 1u                    /* s->npc is a new target */
#define FLOW_CALL 2u                    /* a subroutine call: long interrupt */

#define SP_SE 0x10u
#define SP_UF 0x20u

typedef void (*DspOpFn)(DSP56300State *s, uint32_t op);

typedef struct DspInsn {
    const char *pat;                    /* 24 characters of 0, 1 and fields */
    DspOpFn fn;
} DspInsn;

extern const DspInsn dsp_alu_insns[];
extern const DspInsn dsp_ctl_insns[];

static inline int64_t dsp_sx56(uint64_t v)
{
    return sextract64(v, 0, 56);
}

static inline int32_t dsp_sx24(uint32_t v)
{
    return sextract32(v, 0, 24);
}

static inline uint32_t dsp_acc2(uint64_t a)
{
    return (a >> 48) & 0xff;
}

static inline uint32_t dsp_acc1(uint64_t a)
{
    return (a >> 24) & M24;
}

static inline uint32_t dsp_acc0(uint64_t a)
{
    return a & M24;
}

/* Memory (core). */
uint32_t dsp_read(DSP56300State *s, DSP56300Space sp, uint32_t addr);
void dsp_write(DSP56300State *s, DSP56300Space sp, uint32_t addr,
               uint32_t val);
uint32_t dsp_fetch_ext(DSP56300State *s);

/* Registers (core). Reads of A and B limit; reading SSH pops. */
uint32_t dsp_reg_read(DSP56300State *s, unsigned int code);
void dsp_reg_write(DSP56300State *s, unsigned int code, uint32_t val);
/*
 * A source operand for moves: the 24-bit word an A or B read would put on
 * the bus, with the S and L flags updated.
 */
uint32_t dsp_acc_bus(DSP56300State *s, unsigned int which);
void dsp_acc_bus48(DSP56300State *s, unsigned int which, uint32_t *hi,
                   uint32_t *lo);
void dsp_acc_write24(DSP56300State *s, unsigned int which, uint32_t val);

/* Address generation. */
uint32_t dsp_agu_add(const DSP56300State *s, unsigned int rn, int32_t off);
/*
 * Resolve an effective address (MMMRRR). Sets *imm when the mode is
 * immediate data, in which case the value is the operand, not an address.
 * Applies the address register update. Returns false for a reserved mode.
 */
bool dsp_ea(DSP56300State *s, unsigned int mmmrrr, uint32_t *val, bool *imm);

/* System stack and flow. */
void dsp_push(DSP56300State *s, uint32_t hi, uint32_t lo);
void dsp_pull(DSP56300State *s, uint32_t *hi, uint32_t *lo);
uint32_t dsp_ret_addr(const DSP56300State *s);
void dsp_jump(DSP56300State *s, uint32_t target);
void dsp_call(DSP56300State *s, uint32_t target);
void dsp_note_branch(DSP56300State *s, uint32_t target);
void dsp_illegal(DSP56300State *s);
/* True (and the illegal-instruction exception raised) inside a REP. */
bool dsp_rep_forbidden(DSP56300State *s);
void dsp_set_sr(DSP56300State *s, uint32_t val);
void dsp_set_omr(DSP56300State *s, uint32_t val);

/* Data ALU: conditions and parallel opcodes. */
bool dsp_cond(const DSP56300State *s, unsigned int cc);
bool dsp_alu_parallel(DSP56300State *s, uint32_t op8);
void dsp_set_ccr_std(DSP56300State *s, uint64_t res, bool v);

#endif /* HW_COPROC_DSP56300_INT_H */
