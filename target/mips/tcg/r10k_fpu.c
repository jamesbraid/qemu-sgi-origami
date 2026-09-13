/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The R10000 family's floating-point behaviours, split out of the shared FP
 * helpers: the Unimplemented Operation exception the family raises for a
 * denormalized result or an out-of-range CVT.L, and the arithmetic ABS.fmt
 * and NEG.fmt whose NaN operand signals Invalid.
 *
 * gen_farith() emits the ABS/NEG variants for every MIPS target, linux-user
 * included, so this lives beside the shared fpu_helper.c in the common source
 * set rather than in an R10000 system file. The shared FP helpers call the
 * denormal-result and CVT.L range checks here by a one-line delegation; both
 * are inert on any processor whose definition does not set the matching
 * config flag. update_fcr31() is the shared FP result epilogue, reused from
 * fpu_helper.c.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "internal.h"
#include "exec/helper-proto.h"
#include "fpu/softfloat.h"

/*
 * Refuse an instruction with the Unimplemented Operation exception: a
 * Floating-Point exception whose FCSR cause field holds only the E bit,
 * which has no enable and always traps. The caller has already saved the
 * CPU state.
 */
void helper_raise_fp_unimplemented(CPUMIPSState *env)
{
    SET_FP_CAUSE(env->active_fpu.fcr31, FP_UNIMPLEMENTED);
    do_raise_exception(env, EXCP_FPE, 0);
}

/*
 * Refuse a denormalized result the R10000 way: with the FSR's FS bit
 * clear "A denormalized result causes an Unimplemented Operation
 * exception" (R10000 User's Manual version 2.0, section 15.4); with it
 * set softfloat has already flushed the result to zero. The refused
 * operation is not performed, so its softfloat flags are discarded and
 * the cause field carries E alone.
 */
static G_NORETURN void raise_unimplemented_operation(CPUMIPSState *env,
                                                     uintptr_t pc)
{
    set_float_exception_flags(0, &env->active_fpu.fp_status);
    SET_FP_CAUSE(env->active_fpu.fcr31, FP_UNIMPLEMENTED);
    do_raise_exception(env, EXCP_FPE, pc);
}

void check_denormal_result_d(CPUMIPSState *env, uint64_t ret, uintptr_t pc)
{
    if (unlikely(env->cfg.fp_traps_denormal_result &&
                 !(env->active_fpu.fcr31 & (1 << FCR31_FS)) &&
                 float64_is_denormal(ret))) {
        raise_unimplemented_operation(env, pc);
    }
}

void check_denormal_result_s(CPUMIPSState *env, uint32_t ret, uintptr_t pc)
{
    if (unlikely(env->cfg.fp_traps_denormal_result &&
                 !(env->active_fpu.fcr31 & (1 << FCR31_FS)) &&
                 float32_is_denormal(ret))) {
        raise_unimplemented_operation(env, pc);
    }
}

/*
 * "In the R10000, the conversion allows only up to 51 bits; otherwise an
 * Unimplemented Operation exception is taken" (R10000 User's Manual
 * version 2.0, section 15.5, CVT.L.fmt errata). A NaN, an infinity or an
 * out-of-range source has no 51-bit result either and is refused the
 * same way, in place of the architectural Invalid and saturated result
 * the operating system's assist path delivers after the trap.
 */
void check_cvt_l_result(CPUMIPSState *env, uint64_t ret, bool invalid,
                        uintptr_t pc)
{
    if (unlikely(env->cfg.fp_cvt_l_limited_to_51_bits &&
                 (invalid || ret != (uint64_t)sextract64(ret, 0, 52)))) {
        raise_unimplemented_operation(env, pc);
    }
}

/*
 * The same operations with the R10000's semantics. There ABS.fmt and
 * NEG.fmt are arithmetic — "This operation is arithmetic; a NaN operand
 * signals invalid operation" (VR10000 Instruction Set User's Manual,
 * ABS.fmt and NEG.fmt) — so a NaN operand signals Invalid, trapping when
 * the enable is set and otherwise delivering the default quiet NaN.
 */
uint64_t helper_float_abs_d_r10k(CPUMIPSState *env, uint64_t fdt0)
{
    uint64_t dt2;

    if (float64_is_any_nan(fdt0)) {
        float_raise(float_flag_invalid, &env->active_fpu.fp_status);
        dt2 = float64_default_nan(&env->active_fpu.fp_status);
    } else {
        dt2 = float64_abs(fdt0);
        if ((env->active_fpu.fcr31 & (1 << FCR31_FS)) &&
            float64_is_denormal(dt2)) {
            /* Arithmetic, so the FS flush applies; the sign survives. */
            dt2 &= 1ULL << 63;
        }
    }
    check_denormal_result_d(env, dt2, GETPC());
    update_fcr31(env, GETPC());
    return dt2;
}

uint32_t helper_float_abs_s_r10k(CPUMIPSState *env, uint32_t fst0)
{
    uint32_t wt2;

    if (float32_is_any_nan(fst0)) {
        float_raise(float_flag_invalid, &env->active_fpu.fp_status);
        wt2 = float32_default_nan(&env->active_fpu.fp_status);
    } else {
        wt2 = float32_abs(fst0);
        if ((env->active_fpu.fcr31 & (1 << FCR31_FS)) &&
            float32_is_denormal(wt2)) {
            wt2 &= 1U << 31;
        }
    }
    check_denormal_result_s(env, wt2, GETPC());
    update_fcr31(env, GETPC());
    return wt2;
}

uint64_t helper_float_chs_d_r10k(CPUMIPSState *env, uint64_t fdt0)
{
    uint64_t dt2;

    if (float64_is_any_nan(fdt0)) {
        float_raise(float_flag_invalid, &env->active_fpu.fp_status);
        dt2 = float64_default_nan(&env->active_fpu.fp_status);
    } else {
        dt2 = float64_chs(fdt0);
        if ((env->active_fpu.fcr31 & (1 << FCR31_FS)) &&
            float64_is_denormal(dt2)) {
            dt2 &= 1ULL << 63;
        }
    }
    check_denormal_result_d(env, dt2, GETPC());
    update_fcr31(env, GETPC());
    return dt2;
}

uint32_t helper_float_chs_s_r10k(CPUMIPSState *env, uint32_t fst0)
{
    uint32_t wt2;

    if (float32_is_any_nan(fst0)) {
        float_raise(float_flag_invalid, &env->active_fpu.fp_status);
        wt2 = float32_default_nan(&env->active_fpu.fp_status);
    } else {
        wt2 = float32_chs(fst0);
        if ((env->active_fpu.fcr31 & (1 << FCR31_FS)) &&
            float32_is_denormal(wt2)) {
            wt2 &= 1U << 31;
        }
    }
    check_denormal_result_s(env, wt2, GETPC());
    update_fcr31(env, GETPC());
    return wt2;
}
