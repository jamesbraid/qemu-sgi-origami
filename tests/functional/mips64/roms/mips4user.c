/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Status register bit 31 enables the MIPS IV instruction set in user
 * mode,
 * and
 * is ignored everywhere else.
 *
 * The R10000 reaches the MIPS IV extension through the opcode the R4400
 * gave
 * to a fourth coprocessor, so it drops that coprocessor's usable bit
 * and puts
 * the MIPS IV user mode enable in its place. A model that leaves the
 * bit
 * unimplemented lets user code run MIPS IV instructions an operating
 * system
 * has not permitted; a model that lets the bit gate kernel code as well
 * breaks
 * the kernel itself, which never sets it before running its own MIPS IV
 * code.
 * Either way the symptom is a floating-point fault in userland long
 * after
 * boot, which is a miserable thing to debug backwards.
 *
 * Oracle: the MIPS R10000 User's Manual version 2.0.
 *
 * Its Status register section (section 14.10) states that the MIPS IV
 * extension uses the fourth coprocessor's opcode, "For this reason the
 * CU3
 * bit
 * is omitted in the R10000 processor, and is used as the XX bit", that
 * "To
 * enable execution of the MIPS IV instructions in User mode, the MIPS
 * IV User
 * Mode bit, (XX) of the CP0 Status register must be set", and that "In
 * Kernel
 * and Supervisor modes, the state of the XX bit is ignored, and MIPS IV
 * instructions are always available". Its instruction-set-and-mode
 * table
 * (Table 14-9) and the processor modes table (Table 16-1) tabulate the
 * same
 * rule, and the Status register figure (Figure 14-11) puts the bit at
 * 31.
 *
 * Its MIPS IV instructions section (section 17.4) says that in user
 * mode with
 * the bit clear "an attempt is made to execute MIPS IV instructions, an
 * exception will be taken", and its exception table (Table 17-4) gives
 * which,
 * class by class:
 *
 *  - The extended floating-point opcode -- the indexed loads and
 * stores, the
 *    indexed prefetch, and the multiply-add family -- takes a Reserved
 *    Instruction exception whatever the state of the first
 * coprocessor's
 *    usable bit, and a Coprocessor Unusable exception when MIPS IV is
 *    enabled but the coprocessor is not.
 *  - The integer conditional moves, both on a register value and on a
 *    floating-point condition bit, and the prefetch instruction take a
 *    Reserved Instruction exception; for the moves on a condition bit
 * the
 *    table marks the coprocessor bit a don't-care, so the refusal
 * stands
 *    even with the coprocessor unusable.
 *  - A floating-point branch naming a condition bit other than the
 * first
 *    takes a Reserved Instruction exception; naming the first is MIPS I
 * and
 *    is never gated.
 *  - A floating-point compare naming a condition bit other than the
 * first,
 *    the conditional moves of floating-point registers, and the
 * reciprocal
 *    and reciprocal-square-root estimates take the Unimplemented
 * Operation
 *    exception: a Floating-Point exception, code 15 in the exception
 * code
 *    table (Table 14-13), with the E bit of the Floating-Point Status
 *    register's cause field set. That register's figure (Figure 15-7)
 * puts
 *    the cause field at bits 17:12 with E at 17, the first condition
 * bit at
 *    23 and the rest at 31:25, and its bit descriptions say of E that
 * "This
 *    exception is always enabled".
 *
 * The Cause register section (section 14.11, Figure 14-13 and Table
 * 14-12)
 * puts
 * the coprocessor unit number in bits 29 and 28 for a Coprocessor
 * Unusable
 * exception, and its exception code table (Table 14-13) numbers a
 * Reserved
 * Instruction exception 10, a Coprocessor Unusable exception 11 and a
 * system
 * call 8.
 *
 * The first probe uses the indexed floating-point load and store, which
 * exist
 * only in MIPS IV. The very first assertion is that the bit can be
 * written at
 * all, because a processor without it can say nothing useful about the
 * rest
 * and a failure there names the real problem instead of reporting a
 * confusing
 * reserved-instruction fault several assertions later. The probes after
 * it
 * walk the remaining rows of the exception table, one instruction
 * standing
 * for each class the table separates, and each is run both refused and
 * permitted: the permitted leg checks a value the instruction computed,
 * not
 * merely the absence of a trap, so an instruction decoded as a no-op
 * cannot
 * pass for one that ran.
 *
 * One combination the manual covers is deliberately not tested for the
 * extended-opcode classes: user mode with both the MIPS IV bit and the
 * first
 * coprocessor's usable bit clear. The manual's exception table gives a
 * Reserved Instruction exception there, with the coprocessor bit a
 * don't-care, but QEMU checks coprocessor usability before it checks
 * the
 * instruction set for that opcode and so reports Coprocessor Unusable
 * instead. Nothing depends on which arrives: a kernel that handles the
 * coprocessor fault by enabling the unit and retrying gets the Reserved
 * Instruction exception on the retry, so it reaches the same conclusion
 * one
 * fault later. Correcting it would reorder two checks for every MIPS IV
 * processor in the fork, which is a wider change than this milestone
 * wants,
 * so the divergence is recorded here instead, and the extended-opcode
 * assertions keep one of the two bits set so that each isolates a
 * single
 * cause. The integer conditional move on a condition bit is decoded
 * ahead of
 * any coprocessor check, so for that class alone the both-bits-clear
 * row is
 * tested as the table writes it.
 *
 * User mode can only reach translated memory, so the user half of this
 * ROM
 * runs from a page this ROM maps. Extended user addressing is switched
 * on for
 * it, which enables the MIPS III instruction set in user mode as well,
 * so
 * that
 * the only thing being gated in the interesting assertions is the MIPS
 * IV
 * bit.
 * The mapping is cacheable, and this model has no caches, which is what
 * makes
 * it safe to write instructions into the page through a different
 * address and
 * jump to them with no cache maintenance; on real hardware, or a model
 * with
 * caches, they would have to be written back first. A cacheable mapping
 * also
 * keeps the entry's top two bits clear, which matters because they name
 * an
 * uncached space on the R10000 and inhibit reads and instruction
 * fetches on
 * the
 * R4000.
 */

#include "rom.h"
#include "probe-symbols.h"

const char rom_name[] = "mips4user";

/*
 * The user routine. It runs the instruction under test, then leaves a
 * marker
 * and traps back into the kernel; reaching either of those means the
 * instruction was allowed. It uses only the two registers the
 * architecture
 * reserves for kernel scratch, which the compiler never allocates, so
 * the
 * kernel can hand it a pointer and a value without agreeing on a
 * calling
 * convention. The exception handler overwrites both, which does not
 * matter
 * because the routine never runs twice.
 */
__asm__(".pushsection .text.rom_user_probe, \"ax\"\n\t"
        ".set push\n\t"
        ".set noreorder\n\t"
        ".set mips4\n\t"
        ".set hardfloat\n\t"
        ".globl rom_user_probe\n"
        "rom_user_probe:\n\t"
        "ldxc1 $f0, $0($26)\n\t"        /* the MIPS IV instruction under test */
        "sw $27, 8($26)\n\t"            /* got here: leave the marker behind */
        "syscall\n\t"                   /* hand control back to the kernel */
        "nop\n\t"
        ".globl rom_user_probe_end\n"
        "rom_user_probe_end:\n\t"
        ".set pop\n\t"
        ".popsection");

/*
 * A user routine for the mode itself rather than an instruction: it
 * loads
 * from a kernel-segment address, which user mode refuses with an
 * address
 * error before any translation happens. Reaching the marker means the
 * processor granted kernel addressing. The load address is built inside
 * the
 * routine — the upper-immediate load sign-extends, giving the uncached
 * window
 * onto the same spare physical page the probes are copied to.
 */
__asm__(".pushsection .text.rom_kload_probe, \"ax\"\n\t"
        ".set push\n\t"
        ".set noreorder\n\t"
        ".globl rom_kload_probe\n"
        "rom_kload_probe:\n\t"
        "lui $25, 0xa030\n\t"           /* 0xffffffffa0300000, kernel space */
        "ld $25, 0($25)\n\t"            /* refused in user mode: AdEL */
        "sw $27, 8($26)\n\t"            /* got here: kernel addressing worked */
        "syscall\n\t"
        "nop\n\t"
        ".globl rom_kload_probe_end\n"
        "rom_kload_probe_end:\n\t"
        ".set pop\n\t"
        ".popsection");

/*
 * The other user routines, one per class the exception table separates.
 * Each
 * has the same shape as the first: the instruction under test, the
 * marker
 * store, the system call. The two integer conditional moves overwrite
 * the
 * marker register on purpose, so the value that lands in the marker
 * word says
 * whether the move itself happened rather than merely that the routine
 * survived it.
 */
#define USER_PROBE(name, body)                                                \
    __asm__(".pushsection .text.rom_probe_" #name ", \"ax\"\n\t"              \
            ".set push\n\t"                                                   \
            ".set noreorder\n\t"                                              \
            ".set mips4\n\t"                                                  \
            ".set hardfloat\n\t"                                              \
            ".globl rom_probe_" #name "\n"                                    \
            "rom_probe_" #name ":\n\t"                                        \
            body "\n\t"                                                       \
            ".globl rom_probe_" #name "_end\n"                                \
            "rom_probe_" #name "_end:\n\t"                                    \
            ".set pop\n\t"                                                    \
            ".popsection");

/*
 * The marker register is never zero, so the move-on-nonzero happens.
 */
USER_PROBE(movn,
           "movn $27, $26, $27\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

/*
 * The first condition bit is held zero, so the move-on-false happens.
 */
USER_PROBE(movf,
           "movf $27, $26, $fcc0\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(pref,
           "pref 4, 0($26)\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(prefx,
           "prefx 4, $0($26)\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

/*
 * The branch-on-false is taken when its condition bit is zero, which is
 * how
 * the kernel left it, so the taken path stores the marker value. The
 * fall-through path stores the data address's low half instead: a
 * branch that
 * executes but goes the wrong way is caught, not mistaken for a pass.
 */
USER_PROBE(bc1_cc1,
           "bc1f $fcc1, 1f\n\t"
           "nop\n\t"
           "sw $26, 8($26)\n\t"
           "syscall\n\t"
           "nop\n"
           "1:\tsw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(bc1_cc0,
           "bc1f $fcc0, 1f\n\t"
           "nop\n\t"
           "sw $26, 8($26)\n\t"
           "syscall\n\t"
           "nop\n"
           "1:\tsw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(ceq_cc1,
           "c.eq.s $fcc1, $f4, $f4\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(ceq_cc0,
           "c.eq.s $f4, $f4\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(ceq_d_cc1,
           "c.eq.d $fcc1, $f4, $f4\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(movn_s,
           "movn.s $f2, $f4, $27\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(movf_s,
           "movf.s $f2, $f4, $fcc0\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(recip_s,
           "recip.s $f2, $f4\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(rsqrt_s,
           "rsqrt.s $f2, $f4\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(movz_s,
           "movz.s $f2, $f4, $0\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(movn_d,
           "movn.d $f2, $f4, $27\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(movz_d,
           "movz.d $f2, $f4, $0\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(movf_d,
           "movf.d $f2, $f4, $fcc0\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(recip_d,
           "recip.d $f2, $f4\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

USER_PROBE(rsqrt_d,
           "rsqrt.d $f2, $f4\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

/*
 * MIPS III standing alone: the 64-bit fixed-point conversion.
 */
USER_PROBE(cvt_l_d,
           "cvt.l.d $f2, $f4\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");


USER_PROBE(madd_s,
           "madd.s $f6, $f4, $f4, $f4\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

/*
 * The user page pair. Both addresses are above the two-gigabyte
 * boundary, in
 * the part of the user segment that is translated with no compatibility
 * special cases, and both are inside the segment either processor has.
 * The
 * physical pages are spare: the harness itself uses only the first two
 * megabytes.
 */
#define USER_CODE_VA 0x0000000080400000ull
#define USER_DATA_VA 0x0000000080401000ull
#define USER_CODE_PA 0x0000000000300000ull
#define USER_DATA_PA 0x0000000000301000ull

/*
 * The same physical pages aliased below two gigabytes, for a probe that
 * must run without extended user addressing: with UX clear the user
 * segment ends at two gigabytes, so the ordinary pages are out of
 * reach.
 */
#define USER32_CODE_VA 0x0000000000400000ull
#define USER32_DATA_VA 0x0000000000401000ull

/*
 * The same physical pages seen uncached from kernel mode.
 */
#define KSEG1_OF(pa) (0xffffffffa0000000ull + (pa))

#define MARKER_OFFSET 8
#define MARKER_VALUE  0x5a5aa5a5u

/*
 * What the integer conditional moves leave in the marker word: the
 * moved
 * pointer's low half, which the marker store truncates to.
 */
#define MOVED_POINTER_LO ((uint32_t)USER_DATA_VA)

/*
 * The entry the user mapping lives in, held down so nothing can replace
 * it.
 */
#define USER_TLB_INDEX 0

/*
 * Floating-Point Status register fields, from its figure (Figure 15-7):
 * the
 * first two condition bits and the cause field's E bit.
 */
#define FSR_FCC0    (1u << 23)
#define FSR_FCC1    (1u << 25)
#define FSR_CAUSE_E (1u << 17)

/*
 * Operands and results as register bit patterns, all exactly
 * representable,
 * so no assertion depends on a rounding mode.
 */
#define SINGLE_2_0 0x40000000u
#define SINGLE_4_0 0x40800000u
#define SINGLE_0_5 0x3f000000u
#define SINGLE_6_0 0x40c00000u
#define DOUBLE_2_0 0x4000000000000000ull
#define DOUBLE_4_0 0x4010000000000000ull
#define DOUBLE_0_5 0x3fe0000000000000ull

/*
 * The kernel's own door to the floating-point unit, for staging
 * operands and
 * reading results back. The ROM is compiled soft-float, so the compiler
 * never
 * touches these registers; the probes and these helpers are the only
 * users,
 * and a value staged here is still in place when a probe runs.
 */
static inline void set_fsr(uint32_t v)
{
    __asm__ __volatile__(".set push\n\t.set hardfloat\n\t"
                         "ctc1 %0, $31\n\t"
                         ".set pop" : : "r"(v));
}

static inline uint32_t get_fsr(void)
{
    uint32_t v;

    __asm__ __volatile__(".set push\n\t.set hardfloat\n\t"
                         "cfc1 %0, $31\n\t"
                         ".set pop" : "=r"(v));
    return v;
}

#define ROM_FPR(name)                                                         \
    static inline void set_##name(uint32_t bits)                              \
    {                                                                         \
        __asm__ __volatile__(".set push\n\t.set hardfloat\n\t"                \
                             "mtc1 %0, $" #name "\n\t"                        \
                             ".set pop" : : "r"(bits));                       \
    }                                                                         \
    static inline uint32_t get_##name(void)                                   \
    {                                                                         \
        uint32_t bits;                                                        \
        __asm__ __volatile__(".set push\n\t.set hardfloat\n\t"                \
                             "mfc1 %0, $" #name "\n\t"                        \
                             ".set pop" : "=r"(bits));                        \
        return bits;                                                          \
    }

ROM_FPR(f2)
ROM_FPR(f4)
ROM_FPR(f6)

static inline void set_f4_double(uint64_t bits)
{
    __asm__ __volatile__(".set push\n\t.set hardfloat\n\t"
                         "dmtc1 %0, $f4\n\t"
                         ".set pop" : : "r"(bits));
}

static inline void set_f2_double(uint64_t bits)
{
    __asm__ __volatile__(".set push\n\t.set hardfloat\n\t"
                         "dmtc1 %0, $f2\n\t"
                         ".set pop" : : "r"(bits));
}

static inline uint64_t get_f2_double(void)
{
    uint64_t bits;

    __asm__ __volatile__(".set push\n\t.set hardfloat\n\t"
                         "dmfc1 %0, $f2\n\t"
                         ".set pop" : "=r"(bits));
    return bits;
}

static void map_user_page(void)
{
    rom_set_pagemask(ROM_PAGEMASK_4K);
    rom_set_entryhi(USER_CODE_VA);
    rom_set_entrylo0(rom_entrylo(USER_CODE_PA,
                                 ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G,
                                 ROM_CACHE_NONCOHERENT));
    rom_set_entrylo1(rom_entrylo(USER_DATA_PA,
                                 ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G,
                                 ROM_CACHE_NONCOHERENT));
    rom_set_index(USER_TLB_INDEX);
    __asm__ __volatile__("tlbwi" : : : "memory");
    rom_set_wired(USER_TLB_INDEX + 1);
    rom_set_entryhi(0);
}

static void map_user32_page(void)
{
    rom_set_pagemask(ROM_PAGEMASK_4K);
    rom_set_entryhi(USER32_CODE_VA);
    rom_set_entrylo0(rom_entrylo(USER_CODE_PA,
                                 ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G,
                                 ROM_CACHE_NONCOHERENT));
    rom_set_entrylo1(rom_entrylo(USER_DATA_PA,
                                 ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G,
                                 ROM_CACHE_NONCOHERENT));
    rom_set_index(USER_TLB_INDEX + 1);
    __asm__ __volatile__("tlbwi" : : : "memory");
    rom_set_wired(USER_TLB_INDEX + 2);
    rom_set_entryhi(0);
}

static void copy_user_probe(const unsigned char *begin,
                            const unsigned char *end)
{
    /*
     * Volatile preserves accesses through the uncached KSEG1 alias.
     */
    volatile unsigned char *dst =
        (volatile unsigned char *)KSEG1_OF(USER_CODE_PA); /* KSEG1 uncached */
    unsigned long len = (unsigned long)(end - begin);
    unsigned long i;

    for (i = 0; i < len; i++) {
        dst[i] = begin[i];
    }
}

static void set_marker(uint32_t v)
{
    /*
     * Volatile preserves accesses through the uncached KSEG1 alias.
     */
    volatile uint32_t *m =
        /*
         * Volatile preserves accesses through the uncached KSEG1 alias.
         */
        (volatile uint32_t *)KSEG1_OF(USER_DATA_PA + MARKER_OFFSET);

    *m = v;
}

static uint32_t get_marker(void)
{
    /*
     * Volatile preserves accesses through the uncached KSEG1 alias.
     */
    volatile uint32_t *m =
        /*
         * Volatile preserves accesses through the uncached KSEG1 alias.
         */
        (volatile uint32_t *)KSEG1_OF(USER_DATA_PA + MARKER_OFFSET);

    return *m;
}

/*
 * Drop into the user routine and come back. The return address is armed
 * by
 * the
 * block itself; the Status register to install on the way back has to
 * be
 * staged
 * separately, because resuming at a kernel address while still in user
 * mode
 * would take an address error instead.
 */
static void run_user_probe(uint32_t ustatus, uint32_t kstatus)
{
    set_marker(0);
    rom_exc_reset();
    rom_exc.status_set = kstatus | ROM_ST_EXL;

    ROM_TRY("move $26, %[data]\n\t"
            "move $27, %[mark]\n\t"
            "dmtc0 %[entry], $14\n\t"
            "mtc0 %[ust], $12\n\t"
            "eret",
            [data] "r"(USER_DATA_VA), [mark] "r"((uint64_t)MARKER_VALUE),
            [entry] "r"(USER_CODE_VA), [ust] "r"(ustatus));
}

/*
 * The same entry sequence through the below-two-gigabyte alias.
 */
static void run_user_probe32(uint32_t ustatus, uint32_t kstatus)
{
    set_marker(0);
    rom_exc_reset();
    rom_exc.status_set = kstatus | ROM_ST_EXL;

    ROM_TRY("move $26, %[data]\n\t"
            "move $27, %[mark]\n\t"
            "dmtc0 %[entry], $14\n\t"
            "mtc0 %[ust], $12\n\t"
            "eret",
            [data] "r"(USER32_DATA_VA), [mark] "r"((uint64_t)MARKER_VALUE),
            [entry] "r"(USER32_CODE_VA), [ust] "r"(ustatus));
}

static void run_probe(const unsigned char *begin, const unsigned char *end,
                      uint32_t ustatus, uint32_t kstatus)
{
    copy_user_probe(begin, end);
    run_user_probe(ustatus, kstatus);
}

void rom_main(void)
{
    uint32_t kstatus, ustatus;
    uint64_t scratch = KSEG1_OF(USER_DATA_PA);
    uint64_t moved;

    /*
     * The floating-point unit has to be usable and in its 64-bit
     * register
     * mode
     * before any of the instructions below mean anything, and extended
     * kernel
     * addressing is switched on so that nothing in this ROM depends on
     * which
     * addressing width the kernel happens to be in.
     */
    kstatus = rom_get_status() | ROM_ST_CU1 | ROM_ST_FR | ROM_ST_KX;
    rom_set_status(kstatus);

    /*
     * A processor that does not have the bit cannot answer anything
     * else.
     */
    rom_set_status(kstatus | ROM_ST_XX);
    rom_assert_eq(1, ROM_ST_XX, rom_get_status() & ROM_ST_XX);

    /*
     * It is a real read/write bit, not one stuck on.
     */
    rom_set_status(kstatus);
    rom_assert_eq(2, 0, rom_get_status() & ROM_ST_XX);

    /*
     * In kernel mode the bit is ignored, so with it clear MIPS IV still
     * runs.
     */
    rom_exc_reset();
    ROM_TRY(".set mips4\n\t.set hardfloat\n\t"
            "ldxc1 $f0, $0(%[a])\n\t"
            "sdxc1 $f0, $0(%[a])",
            [a] "r"(scratch));
    rom_assert_eq(3, 0, rom_exc.count);

    /*
     * And with it set, unchanged.
     */
    rom_set_status(kstatus | ROM_ST_XX);
    rom_exc_reset();
    ROM_TRY(".set mips4\n\t.set hardfloat\n\t"
            "ldxc1 $f0, $0(%[a])\n\t"
            "sdxc1 $f0, $0(%[a])",
            [a] "r"(scratch));
    rom_assert_eq(4, 0, rom_exc.count);
    rom_set_status(kstatus);

    /*
     * Everything from here on runs in user mode out of a page we map.
     */
    rom_tlb_park(ROM_TLB_ENTRIES);
    map_user_page();
    copy_user_probe(rom_user_probe, rom_user_probe_end);

    /*
     * User mode with the bit clear: the instruction is refused as a
     * reserved
     * instruction, and the routine never reaches its marker.
     */
    ustatus = ROM_ST_BEV | ROM_ST_CU1 | ROM_ST_FR | ROM_ST_UX |
              ROM_ST_KSU_USER | ROM_ST_EXL;
    run_user_probe(ustatus, kstatus);
    rom_assert_eq(5, 1, rom_exc.count);
    rom_assert_eq(6, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(7, 0, get_marker());

    /*
     * User mode with the bit set: the instruction runs, the marker is
     * written,
     * and the routine gets back to the kernel through its own system
     * call.
     */
    run_user_probe(ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(8, 1, rom_exc.count);
    rom_assert_eq(9, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(10, MARKER_VALUE, get_marker());

    /*
     * User mode with the bit set but the floating-point unit unusable:
     * the
     * instruction is refused for that reason instead, and the Cause
     * register
     * names the coprocessor it wanted.
     */
    run_user_probe((ustatus | ROM_ST_XX) & ~ROM_ST_CU1, kstatus);
    rom_assert_eq(11, 1, rom_exc.count);
    rom_assert_eq(12, ROM_EXC_CPU, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(13, 1, (rom_exc.cause >> ROM_CAUSE_CE_SHIFT) & 3);
    rom_assert_eq(14, 0, get_marker());

    /*
     * The integer conditional move on a register value, refused as a
     * reserved
     * instruction with the bit clear. The exception lands on the
     * instruction
     * itself, and the move never happens.
     */
    run_probe(rom_probe_movn, rom_probe_movn_end, ustatus, kstatus);
    rom_assert_eq(15, 1, rom_exc.count);
    rom_assert_eq(16, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(17, USER_CODE_VA, rom_exc.epc);
    rom_assert_eq(18, 0, get_marker());

    /*
     * Permitted, the move happens: the marker holds the moved pointer.
     */
    run_probe(rom_probe_movn, rom_probe_movn_end, ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(19, 1, rom_exc.count);
    rom_assert_eq(20, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(21, MOVED_POINTER_LO, get_marker());

    /*
     * The integer conditional move on a condition bit: the same
     * reserved-instruction refusal with the bit clear...
     */
    set_fsr(0);
    run_probe(rom_probe_movf, rom_probe_movf_end, ustatus, kstatus);
    rom_assert_eq(22, 1, rom_exc.count);
    rom_assert_eq(23, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(24, 0, get_marker());

    /*
     * ...the move itself when permitted...
     */
    run_probe(rom_probe_movf, rom_probe_movf_end, ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(25, 1, rom_exc.count);
    rom_assert_eq(26, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(27, MOVED_POINTER_LO, get_marker());

    /*
     * ...the coprocessor refusal when MIPS IV is enabled but the
     * coprocessor
     * is not, since the instruction reads a floating-point condition
     * bit...
     */
    run_probe(rom_probe_movf, rom_probe_movf_end,
              (ustatus | ROM_ST_XX) & ~ROM_ST_CU1, kstatus);
    rom_assert_eq(28, 1, rom_exc.count);
    rom_assert_eq(29, ROM_EXC_CPU, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(30, 1, (rom_exc.cause >> ROM_CAUSE_CE_SHIFT) & 3);
    rom_assert_eq(31, 0, get_marker());

    /*
     * ...and, alone among the gated classes, the both-bits-clear row
     * exactly
     * as the exception table writes it: reserved instruction, with the
     * coprocessor bit a don't-care.
     */
    run_probe(rom_probe_movf, rom_probe_movf_end,
              ustatus & ~ROM_ST_CU1, kstatus);
    rom_assert_eq(32, 1, rom_exc.count);
    rom_assert_eq(33, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(34, 0, get_marker());

    /*
     * The prefetch: refused as a reserved instruction with the bit
     * clear.
     */
    run_probe(rom_probe_pref, rom_probe_pref_end, ustatus, kstatus);
    rom_assert_eq(35, 1, rom_exc.count);
    rom_assert_eq(36, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(37, 0, get_marker());

    /*
     * Permitted, it is a hint with no architectural result of its own,
     * so
     * reaching the marker store behind it is all there is to assert.
     */
    run_probe(rom_probe_pref, rom_probe_pref_end, ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(38, 1, rom_exc.count);
    rom_assert_eq(39, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(40, MARKER_VALUE, get_marker());

    /*
     * The indexed prefetch, the extended opcode's hint instruction: the
     * same
     * reserved-instruction refusal, the same hint-only pass.
     */
    run_probe(rom_probe_prefx, rom_probe_prefx_end, ustatus, kstatus);
    rom_assert_eq(41, 1, rom_exc.count);
    rom_assert_eq(42, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(43, 0, get_marker());

    run_probe(rom_probe_prefx, rom_probe_prefx_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(44, 1, rom_exc.count);
    rom_assert_eq(45, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(46, MARKER_VALUE, get_marker());

    /*
     * The floating-point branch naming the second condition bit:
     * refused as
     * a reserved instruction with the bit clear, taken when permitted.
     */
    set_fsr(0);
    run_probe(rom_probe_bc1_cc1, rom_probe_bc1_cc1_end, ustatus, kstatus);
    rom_assert_eq(47, 1, rom_exc.count);
    rom_assert_eq(48, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(49, 0, get_marker());

    run_probe(rom_probe_bc1_cc1, rom_probe_bc1_cc1_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(50, 1, rom_exc.count);
    rom_assert_eq(51, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(52, MARKER_VALUE, get_marker());

    /*
     * The same branch naming the first condition bit is MIPS I, and
     * must not
     * be caught in the gate: with MIPS IV disabled it still executes
     * and is
     * still taken.
     */
    run_probe(rom_probe_bc1_cc0, rom_probe_bc1_cc0_end, ustatus, kstatus);
    rom_assert_eq(53, 1, rom_exc.count);
    rom_assert_eq(54, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(55, MARKER_VALUE, get_marker());

    /*
     * The floating-point compare naming the second condition bit: the
     * Unimplemented Operation refusal with the bit clear -- a
     * Floating-Point
     * exception on the instruction itself, the E cause bit set, and the
     * condition bit left untouched by the killed compare.
     */
    set_fsr(0);
    set_f4(SINGLE_2_0);
    run_probe(rom_probe_ceq_cc1, rom_probe_ceq_cc1_end, ustatus, kstatus);
    rom_assert_eq(56, 1, rom_exc.count);
    rom_assert_eq(57, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(58, USER_CODE_VA, rom_exc.epc);
    rom_assert_eq(59, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);
    rom_assert_eq(60, 0, get_fsr() & FSR_FCC1);
    rom_assert_eq(61, 0, get_marker());

    /*
     * Permitted, the compare of a register with itself sets its bit.
     */
    set_fsr(0);
    run_probe(rom_probe_ceq_cc1, rom_probe_ceq_cc1_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(62, 1, rom_exc.count);
    rom_assert_eq(63, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(64, FSR_FCC1, get_fsr() & FSR_FCC1);
    rom_assert_eq(65, MARKER_VALUE, get_marker());

    /*
     * The compare naming the first condition bit is MIPS I and must not
     * be
     * caught in the gate either.
     */
    set_fsr(0);
    run_probe(rom_probe_ceq_cc0, rom_probe_ceq_cc0_end, ustatus, kstatus);
    rom_assert_eq(66, 1, rom_exc.count);
    rom_assert_eq(67, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(68, FSR_FCC0, get_fsr() & FSR_FCC0);
    rom_assert_eq(69, MARKER_VALUE, get_marker());

    /*
     * The double-precision compare goes through its own leg of the
     * decoder,
     * so one representative pins it to the same rule.
     */
    set_fsr(0);
    set_f4_double(DOUBLE_2_0);
    run_probe(rom_probe_ceq_d_cc1, rom_probe_ceq_d_cc1_end, ustatus, kstatus);
    rom_assert_eq(70, 1, rom_exc.count);
    rom_assert_eq(71, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(72, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);

    set_fsr(0);
    run_probe(rom_probe_ceq_d_cc1, rom_probe_ceq_d_cc1_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(73, 1, rom_exc.count);
    rom_assert_eq(74, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(75, FSR_FCC1, get_fsr() & FSR_FCC1);

    /*
     * The conditional move of floating-point registers on a register
     * value:
     * the Unimplemented Operation refusal with the bit clear, the moved
     * value
     * when permitted. The marker register is never zero, so the
     * permitted
     * move happens.
     */
    set_fsr(0);
    set_f2(0);
    set_f4(SINGLE_2_0);
    run_probe(rom_probe_movn_s, rom_probe_movn_s_end, ustatus, kstatus);
    rom_assert_eq(76, 1, rom_exc.count);
    rom_assert_eq(77, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(78, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);
    rom_assert_eq(79, 0, get_f2());

    run_probe(rom_probe_movn_s, rom_probe_movn_s_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(80, 1, rom_exc.count);
    rom_assert_eq(81, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(82, SINGLE_2_0, get_f2());

    /*
     * The conditional move of floating-point registers on a condition
     * bit,
     * which the decoder handles apart from the move on a register
     * value: the
     * same refusal, and the move-on-false when permitted.
     */
    set_fsr(0);
    set_f2(0);
    set_f4(SINGLE_2_0);
    run_probe(rom_probe_movf_s, rom_probe_movf_s_end, ustatus, kstatus);
    rom_assert_eq(83, 1, rom_exc.count);
    rom_assert_eq(84, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(85, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);
    rom_assert_eq(86, 0, get_f2());

    run_probe(rom_probe_movf_s, rom_probe_movf_s_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(87, 1, rom_exc.count);
    rom_assert_eq(88, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(89, SINGLE_2_0, get_f2());

    /*
     * The reciprocal estimate: the Unimplemented Operation refusal with
     * the
     * bit clear, and the reciprocal of two when permitted.
     */
    set_fsr(0);
    set_f2(0);
    set_f4(SINGLE_2_0);
    run_probe(rom_probe_recip_s, rom_probe_recip_s_end, ustatus, kstatus);
    rom_assert_eq(90, 1, rom_exc.count);
    rom_assert_eq(91, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(92, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);
    rom_assert_eq(93, 0, get_f2());

    run_probe(rom_probe_recip_s, rom_probe_recip_s_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(94, 1, rom_exc.count);
    rom_assert_eq(95, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(96, SINGLE_0_5, get_f2());

    /*
     * The reciprocal square-root estimate, the same way: one over root
     * four.
     */
    set_fsr(0);
    set_f2(0);
    set_f4(SINGLE_4_0);
    run_probe(rom_probe_rsqrt_s, rom_probe_rsqrt_s_end, ustatus, kstatus);
    rom_assert_eq(97, 1, rom_exc.count);
    rom_assert_eq(98, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(99, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);
    rom_assert_eq(100, 0, get_f2());

    run_probe(rom_probe_rsqrt_s, rom_probe_rsqrt_s_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(101, 1, rom_exc.count);
    rom_assert_eq(102, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(103, SINGLE_0_5, get_f2());

    /*
     * The multiply-add, standing for the extended opcode's arithmetic:
     * the
     * reserved-instruction refusal with the bit clear, like the indexed
     * loads, and two times two plus two when permitted.
     */
    set_fsr(0);
    set_f6(0);
    set_f4(SINGLE_2_0);
    run_probe(rom_probe_madd_s, rom_probe_madd_s_end, ustatus, kstatus);
    rom_assert_eq(104, 1, rom_exc.count);
    rom_assert_eq(105, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(106, 0, get_f6());

    run_probe(rom_probe_madd_s, rom_probe_madd_s_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(107, 1, rom_exc.count);
    rom_assert_eq(108, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(109, SINGLE_6_0, get_f6());

    /*
     * Back in kernel mode with the bit clear, one representative of
     * each
     * newly gated class runs ungated, with its result checked: the gate
     * is a
     * user-mode gate, not a processor-wide one.
     */
    rom_set_status(kstatus);
    set_fsr(0);
    set_f2(0);
    set_f4(SINGLE_2_0);
    moved = 0;
    rom_exc_reset();
    ROM_TRY(".set mips4\n\t.set hardfloat\n\t"
            "movn $26, %[a], %[m]\n\t"
            "sd $26, 0(%[out])\n\t"
            "pref 4, 0(%[out])\n\t"
            "c.eq.s $fcc1, $f4, $f4\n\t"
            "recip.s $f2, $f4",
            [a] "r"(scratch), [m] "r"((uint64_t)MARKER_VALUE),
            [out] "r"(&moved));
    rom_assert_eq(110, 0, rom_exc.count);
    rom_assert_eq(111, scratch, moved);
    rom_assert_eq(112, FSR_FCC1, get_fsr() & FSR_FCC1);
    rom_assert_eq(113, SINGLE_0_5, get_f2());

    /*
     * The review of the gate found the model refuses twelve decoder
     * sites
     * while this ROM probed six: every double-precision leg and the
     * move-on-zero forms are distinct cases in the decoder, each with
     * its
     * own gate call, and an ungated one would pass the six-probe ROM
     * silently. One refused-and-permitted pair per remaining site.
     */
    set_fsr(0);
    set_f2(0);
    set_f4(SINGLE_2_0);
    run_probe(rom_probe_movz_s, rom_probe_movz_s_end, ustatus, kstatus);
    rom_assert_eq(114, 1, rom_exc.count);
    rom_assert_eq(115, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(116, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);
    rom_assert_eq(117, 0, get_f2());

    run_probe(rom_probe_movz_s, rom_probe_movz_s_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(118, 1, rom_exc.count);
    rom_assert_eq(119, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(120, SINGLE_2_0, get_f2());

    set_fsr(0);
    set_f2_double(0);
    set_f4_double(DOUBLE_2_0);
    run_probe(rom_probe_movn_d, rom_probe_movn_d_end, ustatus, kstatus);
    rom_assert_eq(121, 1, rom_exc.count);
    rom_assert_eq(122, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(123, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);
    rom_assert_eq(124, 0, get_f2_double());

    run_probe(rom_probe_movn_d, rom_probe_movn_d_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(125, 1, rom_exc.count);
    rom_assert_eq(126, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(127, DOUBLE_2_0, get_f2_double());

    set_fsr(0);
    set_f2_double(0);
    set_f4_double(DOUBLE_2_0);
    run_probe(rom_probe_movz_d, rom_probe_movz_d_end, ustatus, kstatus);
    rom_assert_eq(128, 1, rom_exc.count);
    rom_assert_eq(129, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(130, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);
    rom_assert_eq(131, 0, get_f2_double());

    run_probe(rom_probe_movz_d, rom_probe_movz_d_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(132, 1, rom_exc.count);
    rom_assert_eq(133, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(134, DOUBLE_2_0, get_f2_double());

    set_fsr(0);
    set_f2_double(0);
    set_f4_double(DOUBLE_2_0);
    run_probe(rom_probe_movf_d, rom_probe_movf_d_end, ustatus, kstatus);
    rom_assert_eq(135, 1, rom_exc.count);
    rom_assert_eq(136, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(137, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);
    rom_assert_eq(138, 0, get_f2_double());

    run_probe(rom_probe_movf_d, rom_probe_movf_d_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(139, 1, rom_exc.count);
    rom_assert_eq(140, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(141, DOUBLE_2_0, get_f2_double());

    set_fsr(0);
    set_f2_double(0);
    set_f4_double(DOUBLE_2_0);
    run_probe(rom_probe_recip_d, rom_probe_recip_d_end, ustatus, kstatus);
    rom_assert_eq(142, 1, rom_exc.count);
    rom_assert_eq(143, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(144, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);
    rom_assert_eq(145, 0, get_f2_double());

    run_probe(rom_probe_recip_d, rom_probe_recip_d_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(146, 1, rom_exc.count);
    rom_assert_eq(147, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(148, DOUBLE_0_5, get_f2_double());

    set_fsr(0);
    set_f2_double(0);
    set_f4_double(DOUBLE_4_0);
    run_probe(rom_probe_rsqrt_d, rom_probe_rsqrt_d_end, ustatus, kstatus);
    rom_assert_eq(149, 1, rom_exc.count);
    rom_assert_eq(150, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(151, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);
    rom_assert_eq(152, 0, get_f2_double());

    run_probe(rom_probe_rsqrt_d, rom_probe_rsqrt_d_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(153, 1, rom_exc.count);
    rom_assert_eq(154, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(155, DOUBLE_0_5, get_f2_double());

    /*
     * The L format outside MIPS III mode: "The attempted execution of
     * the
     * L format takes an Unimplemented Operation exception when the MIPS
     * III mode is not enabled" (chapter 17.6). User mode without
     * extended
     * addressing is that mode, whatever the register-width bit says, so
     * the conversion is refused there and runs once UX grants MIPS III.
     */
    map_user32_page();
    set_fsr(0);
    set_f2_double(0);
    set_f4_double(DOUBLE_2_0);
    copy_user_probe(rom_probe_cvt_l_d, rom_probe_cvt_l_d_end);
    run_user_probe32((ustatus & ~ROM_ST_UX) | ROM_ST_XX, kstatus);
    rom_assert_eq(156, 1, rom_exc.count);
    rom_assert_eq(157, ROM_EXC_FPE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(158, FSR_CAUSE_E, get_fsr() & FSR_CAUSE_E);
    rom_assert_eq(159, 0, get_f2_double());

    set_fsr(0);
    set_f2_double(0);
    set_f4_double(DOUBLE_2_0);
    run_probe(rom_probe_cvt_l_d, rom_probe_cvt_l_d_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(160, 1, rom_exc.count);
    rom_assert_eq(161, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(162, 2, get_f2_double());

    /*
     * The mode field itself. First the control: ordinary user mode
     * refuses a
     * load from a kernel segment with an address error naming the
     * address,
     * and
     * the routine never reaches its marker.
     */
    run_probe(rom_kload_probe, rom_kload_probe_end, ustatus, kstatus);
    rom_assert_eq(163, 1, rom_exc.count);
    rom_assert_eq(164, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(165, 0xffffffffa0300000ull, rom_exc.badvaddr);
    rom_assert_eq(166, 0, get_marker());

    /*
     * Now both mode bits at once. The Status register section calls the
     * processor mode undefined for that value and then defines it
     * anyway:
     * "The R10000 processor implements this as User mode." So the same
     * kernel-segment load is refused the same way. A model that routes
     * the
     * value to any kernel-like mode instead lets the load through and
     * comes
     * back by the system call with the marker written.
     *
     * The same section's companion is the performance counters' mode
     * enables,
     * whose user-mode counting must cover this value too; that half
     * shares
     * the fork's mode derivation but is not asserted here, because
     * counting
     * is driven by wall-clock cycles and a handful of user instructions
     * may
     * honestly count zero of them.
     */
    run_probe(rom_kload_probe, rom_kload_probe_end,
              (ustatus & ~ROM_ST_KSU_USER) | (3u << 3), kstatus);
    rom_assert_eq(167, 1, rom_exc.count);
    rom_assert_eq(168, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(169, 0, get_marker());
}
