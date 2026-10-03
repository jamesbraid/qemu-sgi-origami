/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * R10000-family translation routines: the translation-time checks and
 * TCG generation where the R10000 family differs from the shared MIPS
 * decoder.
 *
 * This file implements decodetree patterns for R10000-specific variants
 * (such as PREF, PREFX, and upper-clearing single FP conditional moves)
 * alongside translation checks and CP0 dispatch tables. Those call
 * sites carry no TARGET_MIPS64 guard, so this file builds for every
 * MIPS target even though only mips64 can create an R10000.
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
    if (unlikely(ctx->xx_gates_mips4 &&
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
    if (unlikely(ctx->xx_gates_mips4 &&
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

/*
 * The implementation-dependent coprocessor 0 registers the R10000 family
 * carries, split out of the shared MFC0/MTC0/DMFC0/DMTC0 decoders: the
 * Diagnostic register (22), the model-selected performance counters (25)
 * and the read-only CacheErr register (27). The shared decoder calls in
 * here for those three register numbers and treats R10K_CP0_UNIMP as its
 * own cp0_unimplemented case, so an unhandled selector still logs and reads
 * back zero exactly as before. Every other processor reads the Diagnostic
 * and CacheErr registers as zero and ignores writes, as it did while these
 * arms lived in the shared decoder.
 */

static inline void gen_r10k_mfc0_load32(TCGv arg, target_ulong off)
{
    TCGv_i32 t0 = tcg_temp_new_i32();

    tcg_gen_ld_i32(t0, tcg_env, off);
    tcg_gen_ext_i32_tl(arg, t0);
}

static inline void gen_r10k_mfc0_load32_r10k(TCGv arg, target_ulong off)
{
    TCGv_ptr r10k = tcg_temp_new_ptr();
    TCGv_i32 t0 = tcg_temp_new_i32();

    tcg_gen_ld_ptr(r10k, tcg_env, offsetof(CPUMIPSState, r10k));
    tcg_gen_ld_i32(t0, r10k, off);
    tcg_gen_ext_i32_tl(arg, t0);
}

static inline void gen_r10k_mfc0_load64_r10k(TCGv arg, target_ulong off,
                                             bool low32_only)
{
    TCGv_ptr r10k = tcg_temp_new_ptr();
    TCGv_i64 tmp = tcg_temp_new_i64();

    tcg_gen_ld_ptr(r10k, tcg_env, offsetof(CPUMIPSState, r10k));
    tcg_gen_ld_i64(tmp, r10k, off);
    if (low32_only) {
        gen_move_low32(arg, tmp);
    } else {
        tcg_gen_mov_tl(arg, tmp);
    }
}

static const char * const perf_mfc0_names[8] = {
    "Performance0", "Performance1", "Performance2", "Performance3",
    "Performance4", "Performance5", "Performance6", "Performance7"
};

static const char * const perf_mtc0_names[8] = {
    "Performance0", "Performance",  "Performance",  "Performance",
    "Performance4", "Performance4", "Performance6", "Performance6"
};

static const uint8_t perf_min_pairs[8] = { 0, 1, 2, 2, 3, 3, 4, 4 };

static R10KCp0Result gen_diag_mfc0(DisasContext *ctx, TCGv arg, int sel,
                                   bool is_64, const char **register_name)
{
    *register_name = "Diagnostic";
    if (ctx->diagnostic_rw_bitmask) {
        gen_r10k_mfc0_load64_r10k(arg, offsetof(R10KState, CP0_Diagnostic),
                                  !is_64);
    } else {
        tcg_gen_movi_tl(arg, 0);
    }
    return R10K_CP0_DONE;
}

static R10KCp0Result gen_diag_mtc0(DisasContext *ctx, TCGv arg, int sel,
                                   bool is_64, const char **register_name)
{
    *register_name = "Diagnostic";
    if (ctx->diagnostic_rw_bitmask) {
        gen_helper_mtc0_r10k_diagnostic(tcg_env, arg);
    }
    return R10K_CP0_DONE;
}

static R10KCp0Result gen_perf_mfc0(DisasContext *ctx, TCGv arg, int sel,
                                   bool is_64, const char **register_name)
{
    if (sel < 0 || sel >= 8) {
        return R10K_CP0_UNIMP;
    }
    *register_name = perf_mfc0_names[sel];

    if (is_64) {
        if (sel >= 4) {
            return R10K_CP0_UNIMP;
        }
        if (sel == 0) {
            if (ctx->performance_counter_pairs == 4) {
                return R10K_CP0_UNIMP;
            }
        } else if (ctx->performance_counter_pairs != 2) {
            return R10K_CP0_UNIMP;
        }
    } else {
        if (ctx->performance_counter_pairs < perf_min_pairs[sel]) {
            return R10K_CP0_UNIMP;
        }
    }

    if ((sel & 1) == 0) {
        gen_r10k_mfc0_load32(arg, offsetof(CPUMIPSState, CP0_Performance[sel]));
    } else {
        gen_helper_mfc0_r10k_performance(arg, tcg_env, tcg_constant_i32(sel));
    }
    return R10K_CP0_DONE;
}

static R10KCp0Result gen_perf_mtc0(DisasContext *ctx, TCGv arg, int sel,
                                   bool is_64, const char **register_name)
{
    if (sel < 0 || sel >= 8) {
        return R10K_CP0_UNIMP;
    }
    *register_name = perf_mtc0_names[sel];

    if (is_64) {
        if (sel >= 4) {
            return R10K_CP0_UNIMP;
        }
        if (sel == 0) {
            if (ctx->performance_counter_pairs == 2) {
                gen_helper_mtc0_r10k_performance(tcg_env, arg,
                                                 tcg_constant_i32(0));
            } else if (!ctx->performance_counter_pairs) {
                gen_helper_mtc0_performance0(tcg_env, arg);
            } else {
                return R10K_CP0_UNIMP;
            }
            return R10K_CP0_DONE;
        }
        if (ctx->performance_counter_pairs != 2) {
            return R10K_CP0_UNIMP;
        }
        gen_helper_mtc0_r10k_performance(tcg_env, arg, tcg_constant_i32(sel));
        return R10K_CP0_DONE;
    }

    if (sel == 0) {
        if (ctx->performance_counter_pairs) {
            gen_helper_mtc0_r10k_performance(tcg_env, arg,
                                             tcg_constant_i32(0));
        } else {
            gen_helper_mtc0_performance0(tcg_env, arg);
        }
        return R10K_CP0_DONE;
    }

    if (ctx->performance_counter_pairs < perf_min_pairs[sel]) {
        return R10K_CP0_UNIMP;
    }
    gen_helper_mtc0_r10k_performance(tcg_env, arg, tcg_constant_i32(sel));
    return R10K_CP0_DONE;
}

static R10KCp0Result gen_cacherr_mfc0(DisasContext *ctx, TCGv arg, int sel,
                                      bool is_64, const char **register_name)
{
    if (sel != CP0_REG27__CACHERR) {
        return R10K_CP0_UNIMP;
    }
    *register_name = "CacheErr";
    if (ctx->has_cache_diagnostics) {
        gen_r10k_mfc0_load32_r10k(arg, offsetof(R10KState, CP0_CacheErr));
    } else {
        tcg_gen_movi_tl(arg, 0);
    }
    return R10K_CP0_DONE;
}

static R10KCp0Result gen_cacherr_mtc0(DisasContext *ctx, TCGv arg, int sel,
                                      bool is_64, const char **register_name)
{
    if (sel != CP0_REG27__CACHERR) {
        return R10K_CP0_UNIMP;
    }
    *register_name = "CacheErr";
    return R10K_CP0_DONE;
}

typedef struct R10KCp0Handler {
    R10KCp0Result (*read)(DisasContext *ctx, TCGv arg, int sel, bool is_64,
                          const char **name);
    R10KCp0Result (*write)(DisasContext *ctx, TCGv arg, int sel, bool is_64,
                           const char **name);
} R10KCp0Handler;

static const R10KCp0Handler r10k_cp0_handlers[32] = {
    [CP0_REGISTER_22] = { gen_diag_mfc0, gen_diag_mtc0 },
    [CP0_REGISTER_25] = { gen_perf_mfc0, gen_perf_mtc0 },
    [CP0_REGISTER_27] = { gen_cacherr_mfc0, gen_cacherr_mtc0 },
};

R10KCp0Result gen_mfc0_r10k(DisasContext *ctx, TCGv arg, int reg, int sel,
                            const char **register_name)
{
    if (reg >= 0 && reg < ARRAY_SIZE(r10k_cp0_handlers) &&
        r10k_cp0_handlers[reg].read) {
        return r10k_cp0_handlers[reg].read(ctx, arg, sel, false,
                                           register_name);
    }
    return R10K_CP0_UNIMP;
}

R10KCp0Result gen_dmfc0_r10k(DisasContext *ctx, TCGv arg, int reg, int sel,
                             const char **register_name)
{
    if (reg >= 0 && reg < ARRAY_SIZE(r10k_cp0_handlers) &&
        r10k_cp0_handlers[reg].read) {
        return r10k_cp0_handlers[reg].read(ctx, arg, sel, true,
                                           register_name);
    }
    return R10K_CP0_UNIMP;
}

R10KCp0Result gen_mtc0_r10k(DisasContext *ctx, TCGv arg, int reg, int sel,
                            const char **register_name)
{
    if (reg >= 0 && reg < ARRAY_SIZE(r10k_cp0_handlers) &&
        r10k_cp0_handlers[reg].write) {
        return r10k_cp0_handlers[reg].write(ctx, arg, sel, false,
                                            register_name);
    }
    return R10K_CP0_UNIMP;
}

R10KCp0Result gen_dmtc0_r10k(DisasContext *ctx, TCGv arg, int reg, int sel,
                             const char **register_name)
{
    if (reg >= 0 && reg < ARRAY_SIZE(r10k_cp0_handlers) &&
        r10k_cp0_handlers[reg].write) {
        return r10k_cp0_handlers[reg].write(ctx, arg, sel, true,
                                            register_name);
    }
    return R10K_CP0_UNIMP;
}

/*
 * PREF and PREFX. The R10000 family moves the block into its caches, which
 * the model has none of, but the read or read-exclusive it issues for a
 * block it does not hold is a request the rest of the system sees (R10000
 * User's Manual version 2.0, Table 1-1 and the processor block read
 * protocol). The helper passes it on; machines without coherent bus
 * handlers return immediately, and user-mode has no system to see it.
 */
static void gen_prefetch_r10k(DisasContext *ctx, int hint, TCGv addr)
{
#ifndef CONFIG_USER_ONLY
    gen_helper_r10k_prefetch(tcg_env, addr, tcg_constant_i32(hint));
#endif
}

void gen_pref_r10k(DisasContext *ctx, int hint, int base, int16_t offset)
{
    TCGv addr = tcg_temp_new();

    gen_base_offset_addr(ctx, addr, base, offset);
    gen_prefetch_r10k(ctx, hint, addr);
}

void gen_prefx_r10k(DisasContext *ctx, int hint, int base, int index)
{
    TCGv addr = tcg_temp_new();

    gen_base_index_addr(ctx, addr, base, index);
    gen_prefetch_r10k(ctx, hint, addr);
}

/* Include the auto-generated decoder. */
#include "decode-r10k.c.inc"

static bool trans_PREF(DisasContext *ctx, arg_PREF *a)
{
    if (!ctx->has_r10k_prefetch) {
        return false;
    }
    check_mips4_enabled(ctx);
    gen_pref_r10k(ctx, a->hint, a->base, a->offset);
    return true;
}

static bool trans_PREFX(DisasContext *ctx, arg_PREFX *a)
{
    if (!ctx->has_r10k_prefetch) {
        return false;
    }
    check_cp1_enabled(ctx);
    check_mips4_enabled(ctx);
    gen_prefx_r10k(ctx, a->hint, a->base, a->index);
    return true;
}

static bool trans_MOVCF_S(DisasContext *ctx, arg_MOVCF_S *a)
{
    if (!ctx->fp_single_result_clears_upper ||
        !(ctx->hflags & MIPS_HFLAG_F64)) {
        return false;
    }
    check_cp1_enabled(ctx);
    check_mips4_enabled_fp(ctx);
    gen_movcf_s_r10k(ctx, a->fs, a->fd, a->cc, a->tf);
    return true;
}

static bool trans_MOVZ_S(DisasContext *ctx, arg_MOVZ_S *a)
{
    if (!ctx->fp_single_result_clears_upper ||
        !(ctx->hflags & MIPS_HFLAG_F64)) {
        return false;
    }
    check_cp1_enabled(ctx);
    check_mips4_enabled_fp(ctx);
    gen_movzn_s_r10k(ctx, a->fs, a->fd, a->ft, true);
    return true;
}

static bool trans_MOVN_S(DisasContext *ctx, arg_MOVN_S *a)
{
    if (!ctx->fp_single_result_clears_upper ||
        !(ctx->hflags & MIPS_HFLAG_F64)) {
        return false;
    }
    check_cp1_enabled(ctx);
    check_mips4_enabled_fp(ctx);
    gen_movzn_s_r10k(ctx, a->fs, a->fd, a->ft, false);
    return true;
}
