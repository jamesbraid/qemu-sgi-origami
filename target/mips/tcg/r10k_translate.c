/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * R10000-family translation routines: the translation-time checks and
 * TCG generation where the R10000 family differs from the shared MIPS
 * decoder.
 */

#include "qemu/osdep.h"
#include "translate.h"

/*
 * Verify that the MIPS IV extension is enabled right now. On the R10000 the
 * Status register's XX bit gates it in user mode while kernel and supervisor
 * mode always have it, which is exactly what the COP1X hflag tracks for that
 * processor; the MIPS IV instructions outside the COP1X opcode are refused
 * with a Reserved Instruction exception (R10000 User's Manual version 2.0,
 * section 17.4 and Table 17-4). Narrowed to the R10000: whether the VR5432
 * gates these instructions the same way has not been checked against its
 * manual, and widening the gate would change that model's behaviour.
 */
void check_mips4_enabled(DisasContext *ctx)
{
    if (unlikely((ctx->insn_flags & INSN_R10K) &&
                 !(ctx->hflags & MIPS_HFLAG_COP1X))) {
        gen_reserved_instruction(ctx);
    }
}

/*
 * Refuse the current instruction with the Unimplemented Operation
 * exception: a Floating-Point exception with the E bit of the FCSR cause
 * field, which has no enable and always traps.
 */
void gen_fp_unimplemented(DisasContext *ctx)
{
    save_cpu_state(ctx, 1);
    gen_helper_raise_fp_unimplemented(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
}

/*
 * Refuse a COP2-family instruction the R10000 way: the coprocessor is
 * defined but absent, and Status.CU2 — an ordinary read/write bit —
 * selects between Coprocessor Unusable and Reserved Instruction (User's
 * Manual version 2.0, sections 14.11 and 17.7), so the choice has to be
 * made from live state at execution.
 */
void gen_cop2_exception_r10k(DisasContext *ctx)
{
    save_cpu_state(ctx, 1);
    gen_helper_raise_cop2_exception(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
}

/*
 * The same gate for the MIPS IV floating-point operations the R10000 refuses
 * with the Unimplemented Operation exception instead: raised for the compares
 * naming a nonzero condition code, the conditional moves of floating-point
 * registers, and the reciprocal estimates (the same manual's Table 17-4).
 */
void check_mips4_enabled_fp(DisasContext *ctx)
{
    if (unlikely((ctx->insn_flags & INSN_R10K) &&
                 !(ctx->hflags & MIPS_HFLAG_COP1X))) {
        gen_fp_unimplemented(ctx);
    }
}

/*
 * Whether a COP1 word in the format space (bits 25:24 = 2 or 3) is one of
 * the R10000's instructions: the MIPS IV set, formats S, D, W and L. Any
 * other function or format is refused with the Unimplemented Operation
 * exception at the decode above, section 17.6's format-space half.
 */
bool r10k_cop1_fmt_defined(uint32_t opcode)
{
    uint32_t fmt = extract32(opcode, 21, 5);
    uint32_t func = extract32(opcode, 0, 6);

    switch (fmt) {
    case 0x10: /* S */
    case 0x11: /* D */
        switch (func) {
        case 0x00 ... 0x0f: /* arithmetic, ABS/MOV/NEG, the rounds */
        case 0x11 ... 0x13: /* MOVCF, MOVZ, MOVN */
        case 0x15:          /* RECIP */
        case 0x16:          /* RSQRT */
        case 0x24:          /* CVT.W */
        case 0x25:          /* CVT.L */
        case 0x30 ... 0x3f: /* the compares */
            return true;
        case 0x20:          /* CVT.S, from double only */
            return fmt == 0x11;
        case 0x21:          /* CVT.D, from single only */
            return fmt == 0x10;
        default:
            return false;
        }
    case 0x14: /* W */
    case 0x15: /* L */
        return func == 0x20 || func == 0x21; /* CVT.S, CVT.D */
    default:
        return false;
    }
}

/*
 * MOVT.S, MOVF.S, MOVZ.S and MOVN.S on the R10000 clear the upper 32
 * bits of the destination "even though the condition is false" (User's
 * Manual version 2.0, section 15.5), so the false leg writes the
 * destination's own low half back instead of skipping the write.
 */
void gen_movcf_s_r10k(DisasContext *ctx, int fs, int fd, int cc, int tf)
{
    TCGv_i32 t0 = tcg_temp_new_i32();
    TCGv_i64 t1 = tcg_temp_new_i64();
    TCGv_i64 t_fs = tcg_temp_new_i64();
    TCGv_i64 t_fd = tcg_temp_new_i64();

    tcg_gen_andi_i32(t0, fpu_fcr31, 1 << get_fp_bit(cc));
    tcg_gen_extu_i32_i64(t1, t0);
    tcg_gen_extract_i64(t_fs, fpu_f64[fs], 0, 32);
    tcg_gen_extract_i64(t_fd, fpu_f64[fd], 0, 32);
    tcg_gen_movcond_i64(tf ? TCG_COND_NE : TCG_COND_EQ, fpu_f64[fd],
                        t1, tcg_constant_i64(0), t_fs, t_fd);
}

/*
 * The register-conditional forms of the same clearing rule: the selected
 * low half — the source's when the condition holds, the destination's own
 * when it does not — lands zero-extended.
 */
void gen_movzn_s_r10k(DisasContext *ctx, int fs, int fd, int ft,
                      bool on_zero)
{
    TCGv t0 = tcg_temp_new();
    TCGv_i64 cond = tcg_temp_new_i64();
    TCGv_i64 t_fs = tcg_temp_new_i64();
    TCGv_i64 t_fd = tcg_temp_new_i64();

    gen_load_gpr(t0, ft);
    tcg_gen_extu_tl_i64(cond, t0);
    tcg_gen_extract_i64(t_fs, fpu_f64[fs], 0, 32);
    tcg_gen_extract_i64(t_fd, fpu_f64[fd], 0, 32);
    tcg_gen_movcond_i64(on_zero ? TCG_COND_EQ : TCG_COND_NE, fpu_f64[fd],
                        cond, tcg_constant_i64(0), t_fs, t_fd);
}
