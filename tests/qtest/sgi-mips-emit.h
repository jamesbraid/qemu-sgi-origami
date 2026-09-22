/*
 * Pure MIPS64 instruction word encoders shared by SGI hand-assembled
 * qtest images.
 *
 * Encodings named as in target/mips/tcg/translate.c, copied verbatim
 * from the emit_* bodies in sn0-mmio-branch-test.c, sn0-nmi-test.c,
 * sn0-r10k-llsc-test.c, sn0-status-re-test.c and sn0-mttcg-test.c,
 * which agreed once buffer writing was set aside. Each function here
 * only computes the instruction word: buffer type, bounds checking,
 * delay-slot policy and branch-offset arithmetic against a live write
 * position stay with each caller's own local emit_*(), which differ
 * enough across files -- a byte-addressed CodeBuf with a bounds assert
 * here, a word-addressed GuestImage there -- that a shared function
 * owning the write would force every other caller onto one pointer
 * type instead of just removing the duplicated encoding.
 *
 * mfc0/mtc0 take no select register: no caller here ever names one,
 * so all five agree on sel 0. A caller needing another select stays
 * on its own local encoding.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SGI_MIPS_EMIT_H
#define SGI_MIPS_EMIT_H

#include "qemu/osdep.h"

#define MIPS_OPC_SPECIAL  (0x00u << 26)
#define MIPS_OPC_ADDIU    (0x09u << 26)
#define MIPS_OPC_LUI      (0x0fu << 26)
#define MIPS_OPC_LD       (0x37u << 26)
#define MIPS_OPC_SD       (0x3fu << 26)
#define MIPS_OPC_AND      (0x24u | MIPS_OPC_SPECIAL)
#define MIPS_OPC_CP0      (0x10u << 26)
#define MIPS_OPC_MFC0     (MIPS_OPC_CP0 | (0x00u << 21))
#define MIPS_OPC_MTC0     (MIPS_OPC_CP0 | (0x04u << 21))

static inline uint32_t mips_lui(int rt, uint16_t imm)
{
    return MIPS_OPC_LUI | (rt << 16) | imm;
}

static inline uint32_t mips_addiu(int rt, int rs, int16_t imm)
{
    return MIPS_OPC_ADDIU | (rs << 21) | (rt << 16) | (uint16_t)imm;
}

static inline uint32_t mips_ld(int rt, int base, uint16_t off)
{
    return MIPS_OPC_LD | (base << 21) | (rt << 16) | off;
}

static inline uint32_t mips_sd(int rt, int base, uint16_t off)
{
    return MIPS_OPC_SD | (base << 21) | (rt << 16) | off;
}

static inline uint32_t mips_and(int rd, int rs, int rt)
{
    return MIPS_OPC_AND | (rs << 21) | (rt << 16) | (rd << 11);
}

static inline uint32_t mips_mfc0(int rt, int rd)
{
    return MIPS_OPC_MFC0 | (rt << 16) | (rd << 11);
}

static inline uint32_t mips_mtc0(int rt, int rd)
{
    return MIPS_OPC_MTC0 | (rt << 16) | (rd << 11);
}

/*
 * A branch or other I-type instruction with @op already shifted into
 * position (OPC_BEQ, OPC_BNE, ...) and @offset the word count already
 * resolved against whichever program-counter reference the caller uses.
 */
static inline uint32_t mips_branch(uint32_t op, int rs, int rt, int16_t offset)
{
    return op | (rs << 21) | (rt << 16) | (uint16_t)offset;
}

/* sll $0, $0, 0 -- the canonical MIPS nop, word 0. */
static inline uint32_t mips_nop(void)
{
    return 0;
}

#endif /* SGI_MIPS_EMIT_H */
