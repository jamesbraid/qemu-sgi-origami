/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Word operations must leave a sign-extended 64-bit result.
 *
 * On a 64-bit MIPS processor every instruction whose name says "word"
 * computes
 * 32 bits and writes the sign extension of that result into the full
 * 64-bit
 * register. Getting this wrong is the classic emulator defect for this
 * architecture, and it is invisible until a kernel compares a pointer
 * it
 * derived with a word instruction against one it derived with a
 * doubleword
 * instruction.
 *
 * Oracle: the MIPS64 instruction set reference (MD00087 revision 6.06),
 * which
 * states the rule per instruction: for SLL, "the word result is
 * sign-extended
 * and placed in GPR rd", and its programming note adds that a zero
 * shift
 * amount "truncates a 64-bit value to 32 bits and sign-extends it"; for
 * ADDU
 * and the rest, the operation writes sign_extend of bits 31 to 0.
 *
 * The same reference makes the result UNPREDICTABLE when a word
 * instruction
 * is
 * given an operand that is not already a sign-extended word, except for
 * the
 * shifts, which are defined for any input. So every addition,
 * subtraction,
 * multiplication and division below is fed properly sign-extended
 * words, and
 * only the shifts are handed a value with meaningful high bits.
 *
 * Nothing here is specific to the R10000; it is the shared 64-bit
 * behaviour a
 * mature model already gets right, which is exactly why this ROM is
 * also the
 * check that the harness itself is sound.
 */

#include "rom.h"

const char rom_name[] = "sext";

/*
 * Keeping the operands in variables the compiler cannot fold is the
 * whole
 * point: a constant-folded test proves nothing about the processor.
 */
static volatile uint64_t v_wide = 0x0123456789abcdefull;
/*
 * Volatile forces a memory load at the operand width under test.
 */
static volatile uint64_t v_word_pos = 0x000000007fffffffull;
/*
 * Volatile forces a memory load at the operand width under test.
 */
static volatile uint64_t v_word_one = 0x0000000000000001ull;
/*
 * Volatile forces a memory load at the operand width under test.
 */
static volatile uint64_t v_word_neg = 0xffffffff80000000ull;
/*
 * Volatile forces a memory load at the operand width under test.
 */
static volatile uint64_t v_big = 0x0000000040000000ull;
/*
 * Volatile forces a memory load at the operand width under test.
 */
static volatile uint32_t v_mem_neg = 0x80000000u;

void rom_main(void)
{
    uint64_t a, b, r, hi, lo;

    /*
     * A zero shift truncates to 32 bits and sign-extends: 0x89abcdef is
     * negative.
     */
    a = v_wide;
    __asm__("sll %0, %1, 0" : "=r"(r) : "r"(a));
    rom_assert_eq(1, 0xffffffff89abcdefull, r);

    /*
     * The high half of the same value is positive once truncated.
     */
    __asm__("dsrl32 %0, %1, 0" : "=r"(b) : "r"(a));
    __asm__("sll %0, %1, 0" : "=r"(r) : "r"(b));
    rom_assert_eq(2, 0x0000000001234567ull, r);

    /*
     * A left shift that pushes a one into bit 31 produces a negative
     * result.
     */
    a = v_word_one;
    __asm__("sll %0, %1, 31" : "=r"(r) : "r"(a));
    rom_assert_eq(3, 0xffffffff80000000ull, r);

    /*
     * The variable-shift form must agree with the immediate form.
     */
    a = v_word_one;
    b = 31;
    __asm__("sllv %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    rom_assert_eq(4, 0xffffffff80000000ull, r);

    /*
     * A logical right shift of a negative word clears the sign, so the
     * result
     * is positive; the shift reads only the low 32 bits of its operand.
     */
    a = v_word_neg;
    __asm__("srl %0, %1, 4" : "=r"(r) : "r"(a));
    rom_assert_eq(5, 0x0000000008000000ull, r);

    /*
     * An arithmetic right shift keeps it negative.
     */
    __asm__("sra %0, %1, 4" : "=r"(r) : "r"(a));
    rom_assert_eq(6, 0xfffffffff8000000ull, r);

    /*
     * Adding one to the largest positive word wraps into a negative
     * word.
     */
    a = v_word_pos;
    b = v_word_one;
    __asm__("addu %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    rom_assert_eq(7, 0xffffffff80000000ull, r);

    /*
     * The immediate form of the same wrap.
     */
    __asm__("addiu %0, %1, 1" : "=r"(r) : "r"(a));
    rom_assert_eq(8, 0xffffffff80000000ull, r);

    /*
     * Subtracting one from the most negative word wraps the other way.
     */
    a = v_word_neg;
    b = v_word_one;
    __asm__("subu %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    rom_assert_eq(9, 0x000000007fffffffull, r);

    /*
     * A word multiply writes both halves of the 64-bit product as
     * separate
     * sign-extended words, not as one 64-bit value.
     */
    a = v_big;                          /* 0x40000000 */
    b = 4;
    __asm__("mult %1, %2\n\tmfhi %0" : "=r"(hi) : "r"(a), "r"(b) : "hi", "lo");
    __asm__("mult %1, %2\n\tmflo %0" : "=r"(lo) : "r"(a), "r"(b) : "hi", "lo");
    rom_assert_eq(10, 0x0000000000000001ull, hi);
    rom_assert_eq(11, 0x0000000000000000ull, lo);

    /*
     * 0x40000000 * 2 is 0x80000000: a negative low half, a zero high
     * half.
     */
    b = 2;
    __asm__("mult %1, %2\n\tmflo %0" : "=r"(lo) : "r"(a), "r"(b) : "hi", "lo");
    rom_assert_eq(12, 0xffffffff80000000ull, lo);
    __asm__("mult %1, %2\n\tmfhi %0" : "=r"(hi) : "r"(a), "r"(b) : "hi", "lo");
    rom_assert_eq(13, 0x0000000000000000ull, hi);

    /*
     * An unsigned word divide of a negative dividend treats it as
     * unsigned,
     * and still sign-extends the quotient it produces.
     */
    a = v_word_neg;
    b = v_word_one;
    __asm__("divu $0, %1, %2\n\tmflo %0"
            : "=r"(lo)
            : "r"(a), "r"(b)
            : "hi", "lo");
    rom_assert_eq(14, 0xffffffff80000000ull, lo);

    /*
     * A signed word divide of the same dividend gives a positive
     * quotient.
     */
    b = v_word_neg;
    __asm__("div $0, %1, %2\n\tmflo %0"
            : "=r"(lo)
            : "r"(a), "r"(b)
            : "hi", "lo");
    rom_assert_eq(15, 0x0000000000000001ull, lo);

    /*
     * A word load sign-extends; the unsigned word load does not.
     */
    __asm__("lw %0, 0(%1)" : "=r"(r) : "r"(&v_mem_neg) : "memory");
    rom_assert_eq(16, 0xffffffff80000000ull, r);
    __asm__("lwu %0, 0(%1)" : "=r"(r) : "r"(&v_mem_neg) : "memory");
    rom_assert_eq(17, 0x0000000080000000ull, r);

    /*
     * The doubleword shifts, by contrast, keep all 64 bits.
     */
    a = v_wide;
    __asm__("dsll32 %0, %1, 0" : "=r"(r) : "r"(a));
    rom_assert_eq(18, 0x89abcdef00000000ull, r);
    __asm__("dsra32 %0, %1, 0" : "=r"(r) : "r"(a));
    rom_assert_eq(19, 0x0000000001234567ull, r);
}
