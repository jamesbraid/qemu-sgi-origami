/*
 * Pure MIPS64 instruction word encoders shared by SGI hand-assembled qtest
 * images.
 *
 * Encodings named as in target/mips/tcg/translate.c, copied verbatim from
 * the emit_* bodies in sn0-mmio-branch-test.c, sn0-nmi-test.c,
 * sn0-r10k-llsc-test.c, sn0-status-re-test.c and sn0-mttcg-test.c, which
 * agreed once buffer writing was set aside. Each function here only computes
 * the instruction word: buffer type, bounds checking, delay-slot policy and
 * branch-offset arithmetic against a live write position stay with each
 * caller's own local emit_*(). They use a byte-addressed CodeBuf with a
 * bounds assert here and a word-addressed GuestImage there. A shared
 * function owning the write would force
 * every other caller onto one pointer type instead of just removing the
 * duplicated encoding.
 *
 * mfc0/mtc0/dmtc0 take no select register: no caller here ever names one, so
 * every caller agrees on sel 0. A caller needing another select stays on its
 * own local encoding.
 *
 * sn0-user-image.h's sn0_emit_i(), sn0_emit_mtc0(), sn0_emit_dmtc0() and
 * sn0_emit_mfc0() delegate their word computation here too, kept as a second
 * file because their SN0Image writer, and the fixed four-nop delay each CP0
 * write appends for the R10000's missing EHB, are policy this header still
 * leaves to each caller.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SGI_MIPS_EMIT_H
#define SGI_MIPS_EMIT_H

#include "qemu/osdep.h"

#define MIPS_OPC_SPECIAL  (0x00u << 26)
#define MIPS_OPC_ADDIU    (0x09u << 26)
#define MIPS_OPC_LUI      (0x0fu << 26)
#define MIPS_OPC_LD       (0x37u << 26)
#define MIPS_OPC_SD       (0x3fu << 26)
#define MIPS_OPC_BEQ      (0x04u << 26)
#define MIPS_OPC_BNE      (0x05u << 26)
#define MIPS_OPC_ANDI     (0x0cu << 26)
#define MIPS_OPC_ORI      (0x0du << 26)
#define MIPS_OPC_DADDIU   (0x19u << 26)
#define MIPS_OPC_LW       (0x23u << 26)
#define MIPS_OPC_SH       (0x29u << 26)
#define MIPS_OPC_SW       (0x2bu << 26)
#define MIPS_OPC_CACHE    (0x2fu << 26)
#define MIPS_OPC_AND      (0x24u | MIPS_OPC_SPECIAL)
#define MIPS_OPC_CP0      (0x10u << 26)
#define MIPS_OPC_MFC0     (MIPS_OPC_CP0 | (0x00u << 21))
#define MIPS_OPC_MTC0     (MIPS_OPC_CP0 | (0x04u << 21))
#define MIPS_OPC_DMTC0    (MIPS_OPC_CP0 | (0x05u << 21))

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

static inline uint32_t mips_ori(int rt, int rs, uint16_t imm)
{
    return MIPS_OPC_ORI | (rs << 21) | (rt << 16) | imm;
}

static inline uint32_t mips_andi(int rt, int rs, uint16_t imm)
{
    return MIPS_OPC_ANDI | (rs << 21) | (rt << 16) | imm;
}

static inline uint32_t mips_daddiu(int rt, int rs, int16_t imm)
{
    return MIPS_OPC_DADDIU | (rs << 21) | (rt << 16) | (uint16_t)imm;
}

static inline uint32_t mips_lw(int rt, int base, uint16_t off)
{
    return MIPS_OPC_LW | (base << 21) | (rt << 16) | off;
}

static inline uint32_t mips_sh(int rt, int base, uint16_t off)
{
    return MIPS_OPC_SH | (base << 21) | (rt << 16) | off;
}

static inline uint32_t mips_sw(int rt, int base, uint16_t off)
{
    return MIPS_OPC_SW | (base << 21) | (rt << 16) | off;
}

/* @op is the cache operation, which sits in the rt field. */
static inline uint32_t mips_cache(int op, int base, uint16_t off)
{
    return MIPS_OPC_CACHE | (base << 21) | (op << 16) | off;
}

/* R-type: sll/dsll style shifts take the amount in @sa, others pass 0. */
static inline uint32_t mips_rtype(int rs, int rt, int rd, int sa,
                                  unsigned funct)
{
    return MIPS_OPC_SPECIAL | (rs << 21) | (rt << 16) | (rd << 11) |
           (sa << 6) | funct;
}

/* I-type with @op unshifted, as the manuals tabulate it. */
static inline uint32_t mips_itype(unsigned op, int rs, int rt, uint16_t imm)
{
    return (op << 26) | (rs << 21) | (rt << 16) | imm;
}

static inline uint32_t mips_daddu(int rd, int rs, int rt)
{
    return mips_rtype(rs, rt, rd, 0, 0x2d);
}

static inline uint32_t mips_dsll(int rd, int rt, int sa)
{
    return mips_rtype(0, rt, rd, sa, 0x38);
}

static inline uint32_t mips_dsll32(int rd, int rt)
{
    return mips_rtype(0, rt, rd, 0, 0x3c);
}

static inline uint32_t mips_dsrl32(int rd, int rt)
{
    return mips_rtype(0, rt, rd, 0, 0x3e);
}

static inline uint32_t mips_sync(void)
{
    return 0x0f;
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

static inline uint32_t mips_dmtc0(int rt, int rd)
{
    return MIPS_OPC_DMTC0 | (rt << 16) | (rd << 11);
}

/*
 * Encode an I-type instruction with @op pre-shifted (for example,
 * OPC_BEQ/OPC_BNE) and @offset resolved in words against the caller's PC.
 */
static inline uint32_t mips_branch(uint32_t op, int rs, int rt, int16_t offset)
{
    return op | (rs << 21) | (rt << 16) | (uint16_t)offset;
}

/* sll $0, $0, 0 is the canonical MIPS nop, word 0. */
static inline uint32_t mips_nop(void)
{
    return 0;
}

#endif /* SGI_MIPS_EMIT_H */
