/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * R10000-family floating-point behaviours split out of the shared FP helpers:
 * Unimplemented Operation for denormalized results, denormal inputs to
 * ADD.S and DIV.S with FS clear when selected by the CPU policy, and
 * out-of-range CVT.L.
 * Arithmetic ABS.fmt and NEG.fmt signal Invalid for NaN operands.
 *
 * gen_farith() emits the ABS/NEG variants for every MIPS target, linux-user
 * included, so this lives beside the shared fpu_helper.c in the common source
 * set rather than in an R10000 system file. The shared FP helpers call the
 * input-denormal check for ADD.S and DIV.S and the result-denormal and CVT.L
 * checks here. Each check is inert unless the CPU model sets its config flag.
 * update_fcr31() is the shared FP result epilogue every FP helper ends with.
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
    float_status *st = &env->active_fpu.fp_status;

    /* Nothing the refused operation computed may reach the FCSR. */
    set_float_exception_flags(0, st);
    SET_FP_CAUSE(env->active_fpu.fcr31, FP_UNIMPLEMENTED);
    do_raise_exception(env, EXCP_FPE, pc);
}

void check_denormal_input_s(CPUMIPSState *env, uint32_t a, uint32_t b,
                            uintptr_t pc)
{
    if (unlikely(env->cfg.fp_traps_denormal_input &&
                 !(env->active_fpu.fcr31 & (1 << FCR31_FS)) &&
                 (float32_is_denormal(a) || float32_is_denormal(b)))) {
        raise_unimplemented_operation(env, pc);
    }
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
 * ABS.fmt and NEG.fmt with the R10000's semantics. There they are
 * arithmetic — "This operation is arithmetic; a NaN operand signals
 * invalid operation" (VR10000 Instruction Set User's Manual, ABS.fmt and
 * NEG.fmt) — so a NaN operand signals Invalid, trapping when the enable
 * is set and otherwise delivering the default quiet NaN, and with FS set a
 * denormalized result flushes to a zero of the same sign.
 *
 * One body per width serves both operations; it is forced inline so each
 * helper keeps the cost of a hand-written one, with @negate folded away.
 * @ra is the helper's own return address.
 */
static inline QEMU_ALWAYS_INLINE
uint64_t r10k_sign_op_d(CPUMIPSState *env, uint64_t src, bool negate,
                        uintptr_t ra)
{
    float_status *st = &env->active_fpu.fp_status;
    uint64_t res;

    if (unlikely(float64_is_any_nan(src))) {
        float_raise(float_flag_invalid, st);
        res = float64_default_nan(st);
    } else {
        res = negate ? float64_chs(src) : float64_abs(src);
        if ((env->active_fpu.fcr31 & (1 << FCR31_FS)) &&
            float64_is_denormal(res)) {
            res &= 1ULL << 63;     /* keep only the sign bit */
        }
    }
    check_denormal_result_d(env, res, ra);
    update_fcr31(env, ra);
    return res;
}

static inline QEMU_ALWAYS_INLINE
uint32_t r10k_sign_op_s(CPUMIPSState *env, uint32_t src, bool negate,
                        uintptr_t ra)
{
    float_status *st = &env->active_fpu.fp_status;
    uint32_t res;

    if (unlikely(float32_is_any_nan(src))) {
        float_raise(float_flag_invalid, st);
        res = float32_default_nan(st);
    } else {
        res = negate ? float32_chs(src) : float32_abs(src);
        if ((env->active_fpu.fcr31 & (1 << FCR31_FS)) &&
            float32_is_denormal(res)) {
            res &= 1U << 31;       /* keep only the sign bit */
        }
    }
    check_denormal_result_s(env, res, ra);
    update_fcr31(env, ra);
    return res;
}

uint64_t helper_float_abs_d_r10k(CPUMIPSState *env, uint64_t fdt0)
{
    return r10k_sign_op_d(env, fdt0, false, GETPC());
}

uint32_t helper_float_abs_s_r10k(CPUMIPSState *env, uint32_t fst0)
{
    return r10k_sign_op_s(env, fst0, false, GETPC());
}

uint64_t helper_float_chs_d_r10k(CPUMIPSState *env, uint64_t fdt0)
{
    return r10k_sign_op_d(env, fdt0, true, GETPC());
}

uint32_t helper_float_chs_s_r10k(CPUMIPSState *env, uint32_t fst0)
{
    return r10k_sign_op_s(env, fst0, true, GETPC());
}
