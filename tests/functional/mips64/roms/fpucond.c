/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The floating-point status register's eight condition bits, through
 * the
 * control-register write a kernel uses to restore them.
 *
 * The R10000 implements the MIPS IV compare model: eight condition
 * bits,
 * bit 23 and bits 31:25, each settable by a compare naming it and each
 * testable by branches and conditional moves naming it. A compiler
 * targeting this processor schedules several compares onto different
 * condition bits as a matter of course, and a kernel context switch
 * saves
 * and restores the whole status register with CFC1 and CTC1. A model
 * that
 * lets compares set all eight bits but lets CTC1 write only bit 23 —
 * the
 * single-condition-bit mask of the older processors — corrupts every
 * restored context that held a result in bits 31:25, silently.
 *
 * Oracle: the manual's FSR chapter. "Condition Bits [31:25,23]: The
 * Condition bits indicate the result of floating-point compare
 * instructions." The FS bit is 24, the cause bits are 17:12 (E, the
 * always-enabled unimplemented-operation bit, at 17), enables 11:7,
 * flags 6:2, rounding mode 1:0 — and bits 22:18 are not implemented,
 * ignored on write and zero on read. So every bit outside 22:18 accepts
 * a control-register write, and 22:18 never do.
 *
 * The writes below keep every enable bit zero: writing a cause bit and
 * its enable together signals the exception at the CTC1 itself, which
 * is
 * not the contract under test.
 *
 * The compare model is the R10000's MIPS IV one, so this ROM runs
 * against
 * the R10000 alone.
 */

#include "rom.h"

const char rom_name[] = "fpucond";

/*
 * One as an IEEE double, for compares whose result is known.
 */
#define ONE_D  0x3ff0000000000000ull
#define TWO_D  0x4000000000000000ull

/*
 * A junk pattern planted in a register whose upper half is under test.
 */
#define JUNK_D 0xaaaaaaaabbbbbbbbull

/*
 * A word for LWC1 to load, planted at run time.
 */
static volatile uint32_t loaded_word;

static uint32_t cfc1_fsr(void)
{
    uint32_t v;

    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "cfc1 %0, $31\n\t.set pop" : "=r"(v));
    return v;
}

static void ctc1_fsr(uint32_t v)
{
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "ctc1 %0, $31\n\t.set pop" : : "r"(v));
}

void rom_main(void)
{
    uint32_t taken;
    uint64_t wide;

    loaded_word = 0x3f800000u;
    rom_exc_reset();
    rom_set_status(rom_get_status() | ROM_ST_CU1);

    /*
     * The status register starts empty.
     */
    rom_assert_eq(1, 0, cfc1_fsr());

    /*
     * A control write carrying results in all eight condition bits, the
     * flush bit and a rounding mode reads back exactly; nothing outside
     * the unimplemented field is dropped. This is the write a context
     * restore performs, and the single-condition-bit mask fails it.
     */
    ctc1_fsr(0xff800003u);
    rom_assert_eq(2, 0xff800003u, cfc1_fsr());

    /*
     * Each upper condition bit stands on its own.
     */
    ctc1_fsr(1u << 25);
    rom_assert_eq(3, 1u << 25, cfc1_fsr());
    ctc1_fsr(1u << 31);
    rom_assert_eq(4, 1u << 31, cfc1_fsr());
    ctc1_fsr(1u << 23);
    rom_assert_eq(5, 1u << 23, cfc1_fsr());

    /*
     * The unimplemented field ignores writes and reads zero.
     */
    ctc1_fsr(0x007c0000u);
    rom_assert_eq(6, 0, cfc1_fsr());

    /*
     * A compare sets the condition bit it names — here bit 28,
     * condition
     * four — and the branch and the conditional move both read it. The
     * operands are planted through the coprocessor move so no memory or
     * format conversion is involved.
     */
    ctc1_fsr(0);
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[a], $f0\n\t"
                         "dmtc1 %[a], $f2\n\t"
                         "dmtc1 %[b], $f4\n\t"
                         "c.eq.d $fcc4, $f0, $f2\n\t"
                         "c.lt.d $fcc6, $f0, $f4\n\t"
                         ".set pop"
                         : : [a] "r"(ONE_D), [b] "r"(TWO_D));
    rom_assert_eq(7, (1u << 28) | (1u << 30),
                  cfc1_fsr() & ((1u << 28) | (1u << 30)));

    taken = 0;
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         ".set noreorder\n\t"
                         "bc1t $fcc4, 1f\n\t"
                         " nop\n\t"
                         "b 2f\n\t"
                         " nop\n"
                         "1:\tli %[t], 1\n"
                         "2:\t.set pop"
                         : [t] "+r"(taken));
    rom_assert_eq(8, 1, taken);

    taken = 0;
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "movt %[t], %[one], $fcc6\n\t"
                         ".set pop"
                         : [t] "+r"(taken) : [one] "r"(1));
    rom_assert_eq(9, 1, taken);

    /*
     * Nothing above took an exception, and nothing is left behind.
     */
    rom_assert_eq(10, 0, rom_exc.count);

    /*
     * The upper half of a 64-bit floating-point register after a single
     * or
     * word format write. The architecture leaves those bits undefined,
     * but
     * the manual documents what this processor does with them: "In the
     * R10000 processor, the implementation clears the upper 32 bits,
     * including MOV.S, whereas R4400 and R4200 processors preserve the
     * upper 32 bits", the conditional moves MOVT.S, MOVF.S, MOVZ.S and
     * MOVN.S clear them "even though the condition is false", and MTC1
     * and
     * LWC1 with the FR bit set clear them too. A model carrying the
     * R4400
     * behaviour merges every single-precision result into a register
     * half,
     * which no R10000 ever did.
     *
     * Everything below runs with FR set: with it clear the odd singles
     * live in the upper halves of the even registers, and the pairing
     * is
     * the meaning of those bits rather than something a write clears.
     */
    rom_set_status(rom_get_status() | ROM_ST_FR);
    ctc1_fsr(0);
    rom_exc_reset();

    /*
     * MTC1 lands zero-extended, not merged over the planted junk.
     */
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[junk], $f4\n\t"
                         "mtc1 %[w], $f4\n\t"
                         "dmfc1 %[out], $f4\n\t"
                         ".set pop"
                         : [out] "=r"(wide)
                         : [junk] "r"(JUNK_D), [w] "r"(0x40490fdbu));
    rom_assert_eq(11, 0x40490fdbull, wide);

    /*
     * So does MOV.S, the case the manual singles out.
     */
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[junk], $f6\n\t"
                         "mov.s $f6, $f4\n\t"
                         "dmfc1 %[out], $f6\n\t"
                         ".set pop"
                         : [out] "=r"(wide) : [junk] "r"(JUNK_D));
    rom_assert_eq(12, 0x40490fdbull, wide);

    /*
     * And a single-precision arithmetic result: 1.0f + 1.0f.
     */
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "mtc1 %[one], $f8\n\t"
                         "dmtc1 %[junk], $f10\n\t"
                         "add.s $f10, $f8, $f8\n\t"
                         "dmfc1 %[out], $f10\n\t"
                         ".set pop"
                         : [out] "=r"(wide)
                         : [junk] "r"(JUNK_D), [one] "r"(0x3f800000u));
    rom_assert_eq(13, 0x40000000ull, wide);

    /*
     * And a word loaded from memory.
     */
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[junk], $f12\n\t"
                         "lwc1 $f12, 0(%[p])\n\t"
                         "dmfc1 %[out], $f12\n\t"
                         ".set pop"
                         : [out] "=r"(wide)
                         : [junk] "r"(JUNK_D), [p] "r"(&loaded_word));
    rom_assert_eq(14, 0x3f800000ull, wide);

    /*
     * A conditional move whose condition is false: the value stays, the
     * upper half is cleared anyway. First through a condition bit, then
     * through the register form whose condition register zero makes the
     * move statically impossible.
     */
    ctc1_fsr(0);
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[junk], $f14\n\t"
                         "mtc1 %[w], $f16\n\t"
                         "movt.s $f14, $f16, $fcc0\n\t"
                         "dmfc1 %[out], $f14\n\t"
                         ".set pop"
                         : [out] "=r"(wide)
                         : [junk] "r"(JUNK_D), [w] "r"(0x22222222u));
    rom_assert_eq(15, 0xbbbbbbbbull, wide);

    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[junk], $f18\n\t"
                         "movn.s $f18, $f16, $0\n\t"
                         "dmfc1 %[out], $f18\n\t"
                         ".set pop"
                         : [out] "=r"(wide) : [junk] "r"(JUNK_D));
    rom_assert_eq(16, 0xbbbbbbbbull, wide);

    /*
     * A true condition moves the value and clears the upper half too.
     */
    ctc1_fsr(1u << 23);
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[junk], $f20\n\t"
                         "movt.s $f20, $f16, $fcc0\n\t"
                         "dmfc1 %[out], $f20\n\t"
                         ".set pop"
                         : [out] "=r"(wide) : [junk] "r"(JUNK_D));
    rom_assert_eq(17, 0x22222222ull, wide);

    rom_assert_eq(18, 0, rom_exc.count);

    /*
     * ABS.fmt and NEG.fmt are arithmetic on this processor: "This
     * operation is arithmetic; a NaN operand signals invalid operation"
     * (VR10000 Instruction Set User's Manual, ABS.fmt and NEG.fmt pages
     * —
     * the same silicon, and the R10000 manual's FPU chapter carries no
     * contrary statement). A model that implements them as bare
     * sign-bit
     * moves signals nothing, so a NaN slips through with the status
     * register untouched. With the invalid enable clear the operation
     * completes and delivers a quiet NaN; with it set the operation
     * traps.
     *
     * The NaN encodings are the pre-2008 ones this processor uses: a
     * set
     * most-significant mantissa bit marks a signalling NaN, a clear one
     * a
     * quiet NaN.
     */
    ctc1_fsr(0);
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[a], $f0\n\t"
                         "abs.d $f2, $f0\n\t"
                         "dmfc1 %[out], $f2\n\t"
                         ".set pop"
                         : [out] "=r"(wide) : [a] "r"(0x7ff0000000000001ull));
    rom_assert_eq(19, (1u << 16) | (1u << 6),
                  cfc1_fsr() & ((1u << 16) | (1u << 6)));
    rom_assert(20, ((wide >> 52) & 0x7ff) == 0x7ff &&
                   (wide & 0xfffffffffffffull) != 0);

    ctc1_fsr(0);
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[a], $f0\n\t"
                         "neg.d $f2, $f0\n\t"
                         ".set pop"
                         : : [a] "r"(0x7ff8000000000000ull));
    rom_assert_eq(21, (1u << 16) | (1u << 6),
                  cfc1_fsr() & ((1u << 16) | (1u << 6)));

    ctc1_fsr(0);
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "mtc1 %[a], $f4\n\t"
                         "abs.s $f6, $f4\n\t"
                         ".set pop"
                         : : [a] "r"(0x7f800001u));
    rom_assert_eq(22, (1u << 16) | (1u << 6),
                  cfc1_fsr() & ((1u << 16) | (1u << 6)));

    /*
     * With the invalid enable set the same operand traps at the ABS.
     */
    ctc1_fsr(1u << 11);
    rom_exc_reset();
    ROM_TRY(".set mips4\n\t.set hardfloat\n\t"
            "dmtc1 %[a], $f0\n\t"
            "abs.d $f2, $f0",
            [a] "r"(0x7ff0000000000001ull));
    rom_assert_eq(23, 1, rom_exc.count);
    rom_assert_eq(24, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(25, 1u << 16, cfc1_fsr() & (1u << 16));

    /*
     * An ordinary operand is untouched by the rule and raises nothing.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[a], $f0\n\t"
                         "abs.d $f2, $f0\n\t"
                         "dmfc1 %[out], $f2\n\t"
                         ".set pop"
                         : [out] "=r"(wide) : [a] "r"(0xc000000000000000ull));
    rom_assert_eq(26, TWO_D, wide);
    rom_assert_eq(27, 0, cfc1_fsr());
    rom_assert_eq(28, 0, rom_exc.count);

    /*
     * The FS bit and denormalized results, from the FSR chapter: with
     * FS
     * clear "A denormalized result causes an Unimplemented Operation
     * exception", with FS set the result "is replaced with zero. No
     * exception is flagged." E has no enable bit and always traps, so a
     * model that simply computes the denormal delivers a value this
     * processor never produces. The smallest normal number times one
     * half
     * is the denormalized product used throughout.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    ROM_TRY(".set mips4\n\t.set hardfloat\n\t"
            "dmtc1 %[a], $f0\n\t"
            "dmtc1 %[b], $f2\n\t"
            "mul.d $f4, $f0, $f2",
            [a] "r"(0x0010000000000000ull), [b] "r"(0x3fe0000000000000ull));
    rom_assert_eq(29, 1, rom_exc.count);
    rom_assert_eq(30, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    /*
     * The cause field carries the E bit alone.
     */
    rom_assert_eq(31, 0x20, (cfc1_fsr() >> 12) & 0x3f);

    /*
     * Single precision reaches the same refusal.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    ROM_TRY(".set mips4\n\t.set hardfloat\n\t"
            "mtc1 %[a], $f6\n\t"
            "mtc1 %[b], $f8\n\t"
            "mul.s $f10, $f6, $f8",
            [a] "r"(0x00800000u), [b] "r"(0x3f000000u));
    rom_assert_eq(32, 1, rom_exc.count);
    rom_assert_eq(33, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(34, 0x20, (cfc1_fsr() >> 12) & 0x3f);

    /*
     * With FS set the same product is a signless nothing: flushed to
     * zero, delivered, and no cause, flag or exception raised.
     */
    ctc1_fsr(1u << 24);
    rom_exc_reset();
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[a], $f0\n\t"
                         "dmtc1 %[b], $f2\n\t"
                         "mul.d $f4, $f0, $f2\n\t"
                         "dmfc1 %[out], $f4\n\t"
                         ".set pop"
                         : [out] "=r"(wide)
                         : [a] "r"(0x0010000000000000ull),
                           [b] "r"(0x3fe0000000000000ull));
    rom_assert_eq(35, 0, rom_exc.count);
    rom_assert_eq(36, 0, wide);
    rom_assert_eq(37, 1u << 24, cfc1_fsr());

    /*
     * And an exact normal product with FS clear still just completes.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[a], $f0\n\t"
                         "dmtc1 %[b], $f2\n\t"
                         "mul.d $f4, $f0, $f2\n\t"
                         "dmfc1 %[out], $f4\n\t"
                         ".set pop"
                         : [out] "=r"(wide)
                         : [a] "r"(ONE_D), [b] "r"(TWO_D));
    rom_assert_eq(38, TWO_D, wide);
    rom_assert_eq(39, 0, rom_exc.count);
    rom_assert_eq(40, 0, cfc1_fsr());

    /*
     * CVT.L.fmt, from the FPU instructions chapter's errata: "In the
     * R10000, the conversion allows only up to 51 bits; otherwise an
     * Unimplemented Operation exception is taken. The back-conversion
     * from a 51-bit long integer to single/double no longer takes an
     * Unimplemented Operation exception." The generic conversion runs
     * to
     * the full sixty-four and delivers results this hardware refuses.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[a], $f0\n\t"
                         "cvt.l.d $f2, $f0\n\t"
                         "dmfc1 %[out], $f2\n\t"
                         ".set pop"
                         : [out] "=r"(wide) : [a] "r"(0x4310000000000000ull));
    rom_assert_eq(41, 1ull << 50, wide);
    rom_assert_eq(42, 0, rom_exc.count);

    /*
     * Two to the fifty-second needs 53 bits, so it is refused.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    ROM_TRY(".set mips4\n\t.set hardfloat\n\t"
            "dmtc1 %[a], $f0\n\t"
            "cvt.l.d $f2, $f0",
            [a] "r"(0x4330000000000000ull));
    rom_assert_eq(43, 1, rom_exc.count);
    rom_assert_eq(44, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(45, 0x20, (cfc1_fsr() >> 12) & 0x3f);

    /*
     * The single-precision source reaches the same refusal.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    ROM_TRY(".set mips4\n\t.set hardfloat\n\t"
            "mtc1 %[a], $f4\n\t"
            "cvt.l.s $f6, $f4",
            [a] "r"(0x59800000u));
    rom_assert_eq(46, 1, rom_exc.count);
    rom_assert_eq(47, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * A NaN has no 51-bit result either.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    ROM_TRY(".set mips4\n\t.set hardfloat\n\t"
            "dmtc1 %[a], $f0\n\t"
            "cvt.l.d $f2, $f0",
            [a] "r"(0x7ff0000000000001ull));
    rom_assert_eq(48, 1, rom_exc.count);
    rom_assert_eq(49, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(50, 0x20, (cfc1_fsr() >> 12) & 0x3f);

    /*
     * The back-conversion of a wide long integer no longer traps.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "dmtc1 %[a], $f0\n\t"
                         "cvt.d.l $f2, $f0\n\t"
                         "dmfc1 %[out], $f2\n\t"
                         ".set pop"
                         : [out] "=r"(wide) : [a] "r"(1ull << 52));
    rom_assert_eq(51, 0x4330000000000000ull, wide);
    rom_assert_eq(52, 0, rom_exc.count);

    /*
     * The undefined COP1 encodings, split by bits 25:24 of the word
     * (chapter 17.6): "undefined opcodes (bits 25:24 are 0 or 1) take
     * Reserved Instruction exceptions and undefined formats (bits 25:24
     * are 2 or 3) take Unimplemented Operation exceptions", and the
     * moves footnote gives an illegal-format move a cause field of
     * 100000. The words are spelled out because no assembler emits
     * them.
     */
    /*
     * MOV.W — an illegal-format move: Unimplemented, cause E alone.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    ROM_TRY(".word 0x46800086");
    rom_assert_eq(53, 1, rom_exc.count);
    rom_assert_eq(54, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(55, 0x20, (cfc1_fsr() >> 12) & 0x3f);

    /*
     * An undefined format, sub field 0x12.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    ROM_TRY(".word 0x46400000");
    rom_assert_eq(56, 1, rom_exc.count);
    rom_assert_eq(57, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(58, 0x20, (cfc1_fsr() >> 12) & 0x3f);

    /*
     * ADD.PS — the paired-single format this processor does not have.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    ROM_TRY(".word 0x46c41000");
    rom_assert_eq(59, 1, rom_exc.count);
    rom_assert_eq(60, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * An undefined function within the S format.
     */
    ctc1_fsr(0);
    rom_exc_reset();
    ROM_TRY(".word 0x46000017");
    rom_assert_eq(61, 1, rom_exc.count);
    rom_assert_eq(62, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * The other half of the split: an undefined opcode, bits 25:24
     * zero — the later revisions' MFHC1 slot — stays Reserved.
     */
    rom_exc_reset();
    ROM_TRY(".word 0x44600000");
    rom_assert_eq(63, 1, rom_exc.count);
    rom_assert_eq(64, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    ctc1_fsr(0);
    rom_set_status((rom_get_status() & ~ROM_ST_FR) & ~ROM_ST_CU1);
}
