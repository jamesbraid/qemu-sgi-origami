/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/* IA-64 TCG helper ABI adapters for floating-point execution. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "arch/fp.h"

void HELPER(sync_rotating_fr)(CPUIA64State *arg0)
{
    ia64_sync_rotating_fr(arg0);
}

void HELPER(fadd)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                 uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fadd(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(fsub)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                 uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fsub(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(fmpy)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                 uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fmpy(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(xma)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                uint32_t arg4, uint32_t arg5)
{
    ia64_fp_xma(arg0, arg1, arg2, arg3, arg4, arg5);
}

void HELPER(fma)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    ia64_fp_fma(arg0, arg1, arg2, arg3);
}

void HELPER(fma4)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                 uint32_t arg3, uint32_t arg4, uint32_t arg5)
{
    ia64_fp_fma4(arg0, arg1, arg2, arg3, arg4, arg5);
}

void HELPER(fcmp)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                 uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6)
{
    ia64_fp_fcmp(arg0, arg1, arg2, arg3, arg4, arg5, arg6);
}

void HELPER(fmin)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                 uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fmin(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(fmax)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                 uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fmax(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(famin)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                  uint32_t arg3, uint32_t arg4)
{
    ia64_fp_famin(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(famax)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                  uint32_t arg3, uint32_t arg4)
{
    ia64_fp_famax(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(frcpa)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                  uint32_t arg3, uint32_t arg4, uint32_t arg5)
{
    ia64_fp_frcpa(arg0, arg1, arg2, arg3, arg4, arg5);
}

void HELPER(fprcpa)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                   uint32_t arg3, uint32_t arg4, uint32_t arg5)
{
    ia64_fp_fprcpa(arg0, arg1, arg2, arg3, arg4, arg5);
}

void HELPER(fclass)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                   uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fclass(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(fmerge_ns)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                      uint32_t arg3)
{
    ia64_fp_fmerge_ns(arg0, arg1, arg2, arg3);
}

void HELPER(fmerge_s)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                     uint32_t arg3)
{
    ia64_fp_fmerge_s(arg0, arg1, arg2, arg3);
}

void HELPER(fmerge_se)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                      uint32_t arg3)
{
    ia64_fp_fmerge_se(arg0, arg1, arg2, arg3);
}

void HELPER(flogical_and)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                         uint32_t arg3)
{
    ia64_fp_flogical_and(arg0, arg1, arg2, arg3);
}

void HELPER(flogical_andcm)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                           uint32_t arg3)
{
    ia64_fp_flogical_andcm(arg0, arg1, arg2, arg3);
}

void HELPER(flogical_or)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                        uint32_t arg3)
{
    ia64_fp_flogical_or(arg0, arg1, arg2, arg3);
}

void HELPER(flogical_xor)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                         uint32_t arg3)
{
    ia64_fp_flogical_xor(arg0, arg1, arg2, arg3);
}

void HELPER(fswap)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                  uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fswap(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(fmix)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                 uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fmix(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(fsxt)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                 uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fsxt(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(fpmerge)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                    uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fpmerge(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(fpminmax)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                     uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6)
{
    ia64_fp_fpminmax(arg0, arg1, arg2, arg3, arg4, arg5, arg6);
}

void HELPER(fpcmp)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                  uint32_t arg3, uint32_t arg4, uint32_t arg5)
{
    ia64_fp_fpcmp(arg0, arg1, arg2, arg3, arg4, arg5);
}

void HELPER(fpcvt)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                  uint32_t arg3, uint32_t arg4, uint32_t arg5)
{
    ia64_fp_fpcvt(arg0, arg1, arg2, arg3, arg4, arg5);
}

void HELPER(fpma)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                 uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6)
{
    ia64_fp_fpma(arg0, arg1, arg2, arg3, arg4, arg5, arg6);
}

void HELPER(fsetc)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                  uint32_t arg3)
{
    ia64_fp_fsetc(arg0, arg1, arg2, arg3);
}

void HELPER(fclrf)(CPUIA64State *arg0, uint32_t arg1)
{
    ia64_fp_fclrf(arg0, arg1);
}

uint64_t HELPER(fchkf)(CPUIA64State *arg0, uint32_t arg1)
{
    return ia64_fp_fchkf(arg0, arg1);
}

void HELPER(fms)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                uint32_t arg4, uint32_t arg5)
{
    ia64_fp_fms(arg0, arg1, arg2, arg3, arg4, arg5);
}

void HELPER(fnma)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                 uint32_t arg3)
{
    ia64_fp_fnma(arg0, arg1, arg2, arg3);
}

void HELPER(fnma4)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                  uint32_t arg3, uint32_t arg4, uint32_t arg5)
{
    ia64_fp_fnma4(arg0, arg1, arg2, arg3, arg4, arg5);
}

void HELPER(fselect)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                    uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fselect(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(fnorm)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                  uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fnorm(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(fpabs)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2)
{
    ia64_fp_fpabs(arg0, arg1, arg2);
}

void HELPER(fpneg)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2)
{
    ia64_fp_fpneg(arg0, arg1, arg2);
}

void HELPER(fpnegabs)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2)
{
    ia64_fp_fpnegabs(arg0, arg1, arg2);
}

void HELPER(fcvt_xf)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2)
{
    ia64_fp_fcvt_xf(arg0, arg1, arg2);
}

void HELPER(fcvt_fx)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                    uint32_t arg3, uint32_t arg4, uint32_t arg5)
{
    ia64_fp_fcvt_fx(arg0, arg1, arg2, arg3, arg4, arg5);
}

uint64_t HELPER(getf)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2)
{
    return ia64_fp_getf(arg0, arg1, arg2);
}

void HELPER(setf_exp)(CPUIA64State *arg0, uint32_t arg1, uint64_t arg2)
{
    ia64_fp_setf_exp(arg0, arg1, arg2);
}

void HELPER(setf_s)(CPUIA64State *arg0, uint32_t arg1, uint64_t arg2)
{
    ia64_fp_setf_s(arg0, arg1, arg2);
}

void HELPER(fmov)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2)
{
    ia64_fp_fmov(arg0, arg1, arg2);
}

void HELPER(fprsqrta)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                     uint32_t arg3, uint32_t arg4)
{
    ia64_fp_fprsqrta(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(frsqrta)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                    uint32_t arg3, uint32_t arg4)
{
    ia64_fp_frsqrta(arg0, arg1, arg2, arg3, arg4);
}

void HELPER(fpack)(CPUIA64State *arg0, uint32_t arg1, uint32_t arg2,
                  uint32_t arg3)
{
    ia64_fp_fpack(arg0, arg1, arg2, arg3);
}

void HELPER(ldfe)(CPUIA64State *arg0, uint32_t arg1, uint64_t arg2)
{
    ia64_fp_ldfe(arg0, arg1, arg2, GETPC());
}

void HELPER(ldf_fill)(CPUIA64State *arg0, uint32_t arg1, uint64_t arg2)
{
    ia64_fp_ldf_fill(arg0, arg1, arg2, GETPC());
}

void HELPER(stfe)(CPUIA64State *arg0, uint64_t arg1, uint32_t arg2)
{
    ia64_fp_stfe(arg0, arg1, arg2, GETPC());
}

void HELPER(stf_spill)(CPUIA64State *arg0, uint64_t arg1, uint32_t arg2)
{
    ia64_fp_stf_spill(arg0, arg1, arg2, GETPC());
}
