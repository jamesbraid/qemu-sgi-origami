/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * R10000-family translation routines: the translation-time checks and
 * TCG generation where the R10000 family differs from the shared MIPS
 * decoder.
 *
 * Unlike the other vendor translate units this one has no decodetree
 * file: the R10000 defines no encodings of its own, it only refuses
 * encodings the shared decoder accepts, so everything here is a check
 * or a variant that decoder calls behind an INSN_R10K test. Those call
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

R10KCp0Result gen_mfc0_r10k(DisasContext *ctx, TCGv arg, int reg, int sel,
                            const char **register_name)
{
    switch (reg) {
    case CP0_REGISTER_22:
        /*
         * Implementation dependent: the R10000 family's Diagnostic register
         * lives here, and every other processor reads zero and ignores writes
         * as it did before that register was modeled.
         */
        if (ctx->insn_flags & INSN_R10K) {
            /* A 64-bit register: a word read returns its low word. */
            TCGv_i64 tmp = tcg_temp_new_i64();

            gen_helper_mfc0_r10k_diagnostic(tmp, tcg_env);
            gen_move_low32(arg, tmp);
        } else {
            tcg_gen_movi_tl(arg, 0);
        }
        *register_name = "Diagnostic";
        break;
    case CP0_REGISTER_25:
        switch (sel) {
        case CP0_REG25__PERFCTL0:
            gen_r10k_mfc0_load32(arg,
                                 offsetof(CPUMIPSState, CP0_Performance[0]));
            *register_name = "Performance0";
            break;
        case CP0_REG25__PERFCNT0:
            if (ctx->performance_counter_pairs < 1) {
                return R10K_CP0_UNIMP;
            }
            /* Assembled from the clock while the counter counts cycles. */
            gen_helper_mfc0_r10k_performance(arg, tcg_env,
                                             tcg_constant_i32(sel));
            *register_name = "Performance1";
            break;
        case CP0_REG25__PERFCTL1:
            if (ctx->performance_counter_pairs < 2) {
                return R10K_CP0_UNIMP;
            }
            gen_r10k_mfc0_load32(arg,
                                 offsetof(CPUMIPSState, CP0_Performance[2]));
            *register_name = "Performance2";
            break;
        case CP0_REG25__PERFCNT1:
            if (ctx->performance_counter_pairs < 2) {
                return R10K_CP0_UNIMP;
            }
            /* Assembled from the clock while the counter counts cycles. */
            gen_helper_mfc0_r10k_performance(arg, tcg_env,
                                             tcg_constant_i32(sel));
            *register_name = "Performance3";
            break;
        case CP0_REG25__PERFCTL2:
            if (ctx->performance_counter_pairs < 3) {
                return R10K_CP0_UNIMP;
            }
            gen_r10k_mfc0_load32(arg,
                                 offsetof(CPUMIPSState, CP0_Performance[4]));
            *register_name = "Performance4";
            break;
        case CP0_REG25__PERFCNT2:
            if (ctx->performance_counter_pairs < 3) {
                return R10K_CP0_UNIMP;
            }
            gen_helper_mfc0_r10k_performance(arg, tcg_env,
                                             tcg_constant_i32(sel));
            *register_name = "Performance5";
            break;
        case CP0_REG25__PERFCTL3:
            if (ctx->performance_counter_pairs < 4) {
                return R10K_CP0_UNIMP;
            }
            gen_r10k_mfc0_load32(arg,
                                 offsetof(CPUMIPSState, CP0_Performance[6]));
            *register_name = "Performance6";
            break;
        case CP0_REG25__PERFCNT3:
            if (ctx->performance_counter_pairs < 4) {
                return R10K_CP0_UNIMP;
            }
            gen_helper_mfc0_r10k_performance(arg, tcg_env,
                                             tcg_constant_i32(sel));
            *register_name = "Performance7";
            break;
        default:
            return R10K_CP0_UNIMP;
        }
        break;
    case CP0_REGISTER_27:
        switch (sel) {
        case CP0_REG27__CACHERR:
            /* Read-only; loaded only by a Cache Error exception. */
            gen_helper_mfc0_r10k_cacherr(arg, tcg_env);
            *register_name = "CacheErr";
            break;
        default:
            return R10K_CP0_UNIMP;
        }
        break;
    }
    return R10K_CP0_DONE;
}

R10KCp0Result gen_mtc0_r10k(DisasContext *ctx, TCGv arg, int reg, int sel,
                            const char **register_name)
{
    switch (reg) {
    case CP0_REGISTER_22:
        /* Implementation dependent; only the R10000 family has a register. */
        if (ctx->insn_flags & INSN_R10K) {
            gen_helper_mtc0_r10k_diagnostic(tcg_env, arg);
        }
        *register_name = "Diagnostic";
        break;
    case CP0_REGISTER_25:
        switch (sel) {
        case CP0_REG25__PERFCTL0:
            /*
             * The R10000 family's control registers are narrower than the
             * MIPS32 one this helper was written for, and its two must agree
             * with each other.
             */
            if (ctx->performance_counter_pairs) {
                gen_helper_mtc0_r10k_performance(tcg_env, arg,
                                                 tcg_constant_i32(sel));
            } else {
                gen_helper_mtc0_performance0(tcg_env, arg);
            }
            *register_name = "Performance0";
            break;
        case CP0_REG25__PERFCNT0:
            if (ctx->performance_counter_pairs < 1) {
                return R10K_CP0_UNIMP;
            }
            gen_helper_mtc0_r10k_performance(tcg_env, arg,
                                             tcg_constant_i32(sel));
            *register_name = "Performance";
            break;
        case CP0_REG25__PERFCTL1:
        case CP0_REG25__PERFCNT1:
            if (ctx->performance_counter_pairs < 2) {
                return R10K_CP0_UNIMP;
            }
            gen_helper_mtc0_r10k_performance(tcg_env, arg,
                                             tcg_constant_i32(sel));
            *register_name = "Performance";
            break;
        case CP0_REG25__PERFCTL2:
        case CP0_REG25__PERFCNT2:
            if (ctx->performance_counter_pairs < 3) {
                return R10K_CP0_UNIMP;
            }
            gen_helper_mtc0_r10k_performance(tcg_env, arg,
                                             tcg_constant_i32(sel));
            *register_name = "Performance4";
            break;
        case CP0_REG25__PERFCTL3:
        case CP0_REG25__PERFCNT3:
            if (ctx->performance_counter_pairs < 4) {
                return R10K_CP0_UNIMP;
            }
            gen_helper_mtc0_r10k_performance(tcg_env, arg,
                                             tcg_constant_i32(sel));
            *register_name = "Performance6";
            break;
        default:
            return R10K_CP0_UNIMP;
        }
        break;
    case CP0_REGISTER_27:
        switch (sel) {
        case CP0_REG27__CACHERR:
            /* Read-only: loaded by a Cache Error exception, never by MTC0. */
            *register_name = "CacheErr";
            break;
        default:
            return R10K_CP0_UNIMP;
        }
        break;
    }
    return R10K_CP0_DONE;
}

R10KCp0Result gen_dmfc0_r10k(DisasContext *ctx, TCGv arg, int reg, int sel,
                             const char **register_name)
{
    switch (reg) {
    case CP0_REGISTER_22:
        /*
         * Implementation dependent: the R10000 family's Diagnostic register
         * lives here, and every other processor reads zero and ignores writes
         * as it did before that register was modeled.
         */
        if (ctx->insn_flags & INSN_R10K) {
            /* A 64-bit register: a doubleword read returns all of it. */
            gen_helper_mfc0_r10k_diagnostic(arg, tcg_env);
        } else {
            tcg_gen_movi_tl(arg, 0);
        }
        *register_name = "Diagnostic";
        break;
    case CP0_REGISTER_25:
        switch (sel) {
        case CP0_REG25__PERFCTL0:
            if (ctx->performance_counter_pairs == 4) {
                return R10K_CP0_UNIMP;
            }
            gen_r10k_mfc0_load32(arg,
                                 offsetof(CPUMIPSState, CP0_Performance[0]));
            *register_name = "Performance0";
            break;
        case CP0_REG25__PERFCNT0:
            if (ctx->performance_counter_pairs != 2) {
                return R10K_CP0_UNIMP;
            }
            /* Assembled from the clock while the counter counts cycles. */
            gen_helper_mfc0_r10k_performance(arg, tcg_env,
                                             tcg_constant_i32(sel));
            *register_name = "Performance1";
            break;
        case CP0_REG25__PERFCTL1:
            if (ctx->performance_counter_pairs != 2) {
                return R10K_CP0_UNIMP;
            }
            gen_r10k_mfc0_load32(arg,
                                 offsetof(CPUMIPSState, CP0_Performance[2]));
            *register_name = "Performance2";
            break;
        case CP0_REG25__PERFCNT1:
            if (ctx->performance_counter_pairs != 2) {
                return R10K_CP0_UNIMP;
            }
            /* Assembled from the clock while the counter counts cycles. */
            gen_helper_mfc0_r10k_performance(arg, tcg_env,
                                             tcg_constant_i32(sel));
            *register_name = "Performance3";
            break;
        case CP0_REG25__PERFCTL2:
            *register_name = "Performance4";
            return R10K_CP0_UNIMP;
        case CP0_REG25__PERFCNT2:
            *register_name = "Performance5";
            return R10K_CP0_UNIMP;
        case CP0_REG25__PERFCTL3:
            *register_name = "Performance6";
            return R10K_CP0_UNIMP;
        case CP0_REG25__PERFCNT3:
            *register_name = "Performance7";
            return R10K_CP0_UNIMP;
        default:
            return R10K_CP0_UNIMP;
        }
        break;
    case CP0_REGISTER_27:
        switch (sel) {
        case CP0_REG27__CACHERR:
            /* Read-only; loaded only by a Cache Error exception. */
            gen_helper_mfc0_r10k_cacherr(arg, tcg_env);
            *register_name = "CacheErr";
            break;
        default:
            return R10K_CP0_UNIMP;
        }
        break;
    }
    return R10K_CP0_DONE;
}

R10KCp0Result gen_dmtc0_r10k(DisasContext *ctx, TCGv arg, int reg, int sel,
                             const char **register_name)
{
    switch (reg) {
    case CP0_REGISTER_22:
        /* Implementation dependent; only the R10000 family has a register. */
        if (ctx->insn_flags & INSN_R10K) {
            gen_helper_mtc0_r10k_diagnostic(tcg_env, arg);
        }
        *register_name = "Diagnostic";
        break;
    case CP0_REGISTER_25:
        switch (sel) {
        case CP0_REG25__PERFCTL0:
            /*
             * The R10000 family's control registers are narrower than the
             * MIPS32 one this helper was written for, and its two must agree
             * with each other.
             */
            if (ctx->performance_counter_pairs == 2) {
                gen_helper_mtc0_r10k_performance(tcg_env, arg,
                                                 tcg_constant_i32(sel));
            } else if (!ctx->performance_counter_pairs) {
                gen_helper_mtc0_performance0(tcg_env, arg);
            } else {
                return R10K_CP0_UNIMP;
            }
            *register_name = "Performance0";
            break;
        case CP0_REG25__PERFCNT0:
        case CP0_REG25__PERFCTL1:
        case CP0_REG25__PERFCNT1:
            if (ctx->performance_counter_pairs != 2) {
                return R10K_CP0_UNIMP;
            }
            gen_helper_mtc0_r10k_performance(tcg_env, arg,
                                             tcg_constant_i32(sel));
            *register_name = "Performance";
            break;
        case CP0_REG25__PERFCTL2:
            *register_name = "Performance4";
            return R10K_CP0_UNIMP;
        case CP0_REG25__PERFCNT2:
            *register_name = "Performance5";
            return R10K_CP0_UNIMP;
        case CP0_REG25__PERFCTL3:
            *register_name = "Performance6";
            return R10K_CP0_UNIMP;
        case CP0_REG25__PERFCNT3:
            *register_name = "Performance7";
            return R10K_CP0_UNIMP;
        default:
            return R10K_CP0_UNIMP;
        }
        break;
    case CP0_REGISTER_27:
        switch (sel) {
        case CP0_REG27__CACHERR:
            /* Read-only: loaded by a Cache Error exception, never by MTC0. */
            *register_name = "CacheErr";
            break;
        default:
            return R10K_CP0_UNIMP;
        }
        break;
    }
    return R10K_CP0_DONE;
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
