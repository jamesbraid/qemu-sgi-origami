/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/*
 * IA-64 TCG helper ABI adapters for pure packed integer operations.
 */

#include "qemu/osdep.h"
#include "exec/helper-proto.h"
#include "arch/simd-ops.h"

uint64_t HELPER(simd_pavg)(uint32_t op_sel, uint64_t a, uint64_t b)
{
    return ia64_simd_pavg_value(op_sel, a, b);
}

uint64_t HELPER(simd_pcmp)(uint32_t op_sel, uint64_t a, uint64_t b)
{
    return ia64_simd_pcmp_value(op_sel, a, b);
}

uint64_t HELPER(simd_pminmax)(uint32_t op_sel, uint64_t a, uint64_t b)
{
    return ia64_simd_pminmax_value(op_sel, a, b);
}

uint64_t HELPER(simd_pmpy)(uint32_t op_sel, uint64_t a, uint64_t b,
                          uint32_t shift)
{
    return ia64_simd_pmpy_value(op_sel, a, b, shift);
}

uint64_t HELPER(simd_psad1)(uint64_t a, uint64_t b)
{
    return ia64_simd_psad1_value(a, b);
}

uint64_t HELPER(simd_pshladd2)(uint64_t a, uint64_t b, uint32_t count)
{
    return ia64_simd_pshladd2_value(a, b, count);
}

uint64_t HELPER(simd_mux)(uint32_t op_sel, uint64_t value, uint32_t imm)
{
    return ia64_simd_mux_value(op_sel, value, imm);
}

uint64_t HELPER(simd_mix)(uint32_t op_sel, uint64_t a, uint64_t b)
{
    return ia64_simd_mix_value(op_sel, a, b);
}

uint64_t HELPER(simd_unpack)(uint32_t op_sel, uint64_t a, uint64_t b)
{
    return ia64_simd_unpack_value(op_sel, a, b);
}

uint64_t HELPER(simd_pack)(uint32_t op_sel, uint64_t a, uint64_t b)
{
    return ia64_simd_pack_value(op_sel, a, b);
}

uint64_t HELPER(simd_czx)(uint32_t op_sel, uint64_t value)
{
    return ia64_simd_czx_value(op_sel, value);
}

uint64_t HELPER(simd_sum)(uint64_t a, uint64_t b)
{
    return ia64_simd_sum_value(a, b);
}
