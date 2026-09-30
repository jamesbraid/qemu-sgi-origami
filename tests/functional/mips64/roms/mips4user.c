/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Status.XX gates MIPS IV instructions in user mode
 * (R10000 User's Manual 2.0).
 */

#include "rom.h"
#include "probe-symbols.h"

const char rom_name[] = "mips4user";

/*
 * Use kernel-reserved $26/$27 to pass probe inputs without a C ABI. The
 * exception handler clobbers them, so each probe runs once.
 */
__asm__(".pushsection .text.rom_user_probe, \"ax\"\n\t"
        ".set push\n\t"
        ".set noreorder\n\t"
        ".set mips4\n\t"
        ".set hardfloat\n\t"
        ".globl rom_user_probe\n"
        "rom_user_probe:\n\t"
        "ldxc1 $f0, $0($26)\n\t"        /* MIPS IV instruction under test */
        "sw $27, 8($26)\n\t"            /* Probe succeeded. */
        "syscall\n\t"                   /* Return control to the kernel. */
        "nop\n\t"
        ".globl rom_user_probe_end\n"
        "rom_user_probe_end:\n\t"
        ".set pop\n\t"
        ".popsection");

/* Probe kernel addressing by loading the uncached alias of the probe page. */
__asm__(".pushsection .text.rom_kload_probe, \"ax\"\n\t"
        ".set push\n\t"
        ".set noreorder\n\t"
        ".globl rom_kload_probe\n"
        "rom_kload_probe:\n\t"
        "lui $25, 0xa030\n\t"           /* Sign-extends to the KSEG1 address. */
        "ld $25, 0($25)\n\t"            /* User mode must raise AdEL. */
        "sw $27, 8($26)\n\t"            /* Access was allowed. */
        "syscall\n\t"
        "nop\n\t"
        ".globl rom_kload_probe_end\n"
        "rom_kload_probe_end:\n\t"
        ".set pop\n\t"
        ".popsection");

/* Build one isolated user routine per instruction class. */
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

/* The nonzero marker makes MOVN take its move path. */
USER_PROBE(movn,
           "movn $27, $26, $27\n\t"
           "sw $27, 8($26)\n\t"
           "syscall\n\t"
           "nop");

/* FCC0 starts clear, so MOVF takes its move path. */
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

/* Store a distinct value on fall-through to catch a misdirected branch. */
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

/* MIPS III L-format conversion, gated by UX. */
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

/* Probes above 2 GiB require UX; their backing pages are unused by the ROM. */
#define USER_CODE_VA 0x0000000080400000ull
#define USER_DATA_VA 0x0000000080401000ull
#define USER_CODE_PA 0x0000000000300000ull
#define USER_DATA_PA 0x0000000000301000ull

/* Low aliases let the UX-clear probe reach the same backing pages. */
#define USER32_CODE_VA 0x0000000000400000ull
#define USER32_DATA_VA 0x0000000000401000ull

/* Uncached kernel aliases of the probe pages. */
#define KSEG1_OF(pa) (0xffffffffa0000000ull + (pa))

#define MARKER_OFFSET 8
#define MARKER_VALUE  0x5a5aa5a5u

/* Integer conditional moves store the low word of USER_DATA_VA. */
#define MOVED_POINTER_LO ((uint32_t)USER_DATA_VA)

/* Keep user mappings in wired entries. */
#define USER_TLB_INDEX 0

/* FSR condition bits and Unimplemented Operation cause bit. */
#define FSR_FCC0    (1u << 23)
#define FSR_FCC1    (1u << 25)
#define FSR_CAUSE_E (1u << 17)

/* Exactly representable operands keep results independent of rounding mode. */
#define SINGLE_2_0 0x40000000u
#define SINGLE_4_0 0x40800000u
#define SINGLE_0_5 0x3f000000u
#define SINGLE_6_0 0x40c00000u
#define DOUBLE_2_0 0x4000000000000000ull
#define DOUBLE_4_0 0x4010000000000000ull
#define DOUBLE_0_5 0x3fe0000000000000ull

/* The ROM is soft-float, so staged FPU registers remain available to probes. */
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
    /* Volatile writes preserve accesses through the uncached alias. */
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
    volatile uint32_t *m =
        (volatile uint32_t *)KSEG1_OF(USER_DATA_PA + MARKER_OFFSET);

    *m = v;
}

static uint32_t get_marker(void)
{
    volatile uint32_t *m =
        (volatile uint32_t *)KSEG1_OF(USER_DATA_PA + MARKER_OFFSET);

    return *m;
}

/*
 * Stage kernel Status for the exception return; resuming in user mode
 * at a kernel address would raise AdEL.
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

/* Enter the same routine through its low address alias. */
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

    /* Enable the FPU, 64-bit FPRs, and extended kernel addressing. */
    kstatus = rom_get_status() | ROM_ST_CU1 | ROM_ST_FR | ROM_ST_KX;
    rom_set_status(kstatus);

    /* Skip no feature check: Status.XX must be implemented and writable. */
    rom_set_status(kstatus | ROM_ST_XX);
    rom_assert_eq(1, ROM_ST_XX, rom_get_status() & ROM_ST_XX);

    rom_set_status(kstatus);
    rom_assert_eq(2, 0, rom_get_status() & ROM_ST_XX);

    /* Status.XX does not gate MIPS IV instructions in kernel mode. */
    rom_exc_reset();
    ROM_TRY(".set mips4\n\t.set hardfloat\n\t"
            "ldxc1 $f0, $0(%[a])\n\t"
            "sdxc1 $f0, $0(%[a])",
            [a] "r"(scratch));
    rom_assert_eq(3, 0, rom_exc.count);

    rom_set_status(kstatus | ROM_ST_XX);
    rom_exc_reset();
    ROM_TRY(".set mips4\n\t.set hardfloat\n\t"
            "ldxc1 $f0, $0(%[a])\n\t"
            "sdxc1 $f0, $0(%[a])",
            [a] "r"(scratch));
    rom_assert_eq(4, 0, rom_exc.count);
    rom_set_status(kstatus);

    /* Run user probes from a mapped, wired page. */
    rom_tlb_park(ROM_TLB_ENTRIES);
    map_user_page();
    copy_user_probe(rom_user_probe, rom_user_probe_end);

    /* In user mode, XX clear must reject the MIPS IV instruction. */
    ustatus = ROM_ST_BEV | ROM_ST_CU1 | ROM_ST_FR | ROM_ST_UX |
              ROM_ST_KSU_USER | ROM_ST_EXL;
    run_user_probe(ustatus, kstatus);
    rom_assert_eq(5, 1, rom_exc.count);
    rom_assert_eq(6, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(7, 0, get_marker());

    /* With XX set, the probe reaches its marker and returns by syscall. */
    run_user_probe(ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(8, 1, rom_exc.count);
    rom_assert_eq(9, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(10, MARKER_VALUE, get_marker());

    /*
     * CU1 clear takes priority for these extended FP instructions. With
     * XX clear, the manual specifies RI, but QEMU reports CPU unusable;
     * MOVF is tested separately because its decoder checks XX first.
     */
    run_user_probe((ustatus | ROM_ST_XX) & ~ROM_ST_CU1, kstatus);
    rom_assert_eq(11, 1, rom_exc.count);
    rom_assert_eq(12, ROM_EXC_CPU, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(13, 1, (rom_exc.cause >> ROM_CAUSE_CE_SHIFT) & 3);
    rom_assert_eq(14, 0, get_marker());

    /* MOVN is RI with XX clear; EPC must identify the trapped instruction. */
    run_probe(rom_probe_movn, rom_probe_movn_end, ustatus, kstatus);
    rom_assert_eq(15, 1, rom_exc.count);
    rom_assert_eq(16, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(17, USER_CODE_VA, rom_exc.epc);
    rom_assert_eq(18, 0, get_marker());

    run_probe(rom_probe_movn, rom_probe_movn_end, ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(19, 1, rom_exc.count);
    rom_assert_eq(20, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(21, MOVED_POINTER_LO, get_marker());

    /* MOVF on FCC0: XX clear, enabled with CU1, and both bits clear. */
    set_fsr(0);
    run_probe(rom_probe_movf, rom_probe_movf_end, ustatus, kstatus);
    rom_assert_eq(22, 1, rom_exc.count);
    rom_assert_eq(23, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(24, 0, get_marker());

    run_probe(rom_probe_movf, rom_probe_movf_end, ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(25, 1, rom_exc.count);
    rom_assert_eq(26, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(27, MOVED_POINTER_LO, get_marker());

    run_probe(rom_probe_movf, rom_probe_movf_end,
              (ustatus | ROM_ST_XX) & ~ROM_ST_CU1, kstatus);
    rom_assert_eq(28, 1, rom_exc.count);
    rom_assert_eq(29, ROM_EXC_CPU, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(30, 1, (rom_exc.cause >> ROM_CAUSE_CE_SHIFT) & 3);
    rom_assert_eq(31, 0, get_marker());

    run_probe(rom_probe_movf, rom_probe_movf_end,
              ustatus & ~ROM_ST_CU1, kstatus);
    rom_assert_eq(32, 1, rom_exc.count);
    rom_assert_eq(33, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(34, 0, get_marker());

    /* PREF is a hint; success is established by reaching the marker. */
    run_probe(rom_probe_pref, rom_probe_pref_end, ustatus, kstatus);
    rom_assert_eq(35, 1, rom_exc.count);
    rom_assert_eq(36, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(37, 0, get_marker());

    run_probe(rom_probe_pref, rom_probe_pref_end, ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(38, 1, rom_exc.count);
    rom_assert_eq(39, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(40, MARKER_VALUE, get_marker());

    /* PREFX is also a hint with no architectural result to inspect. */
    run_probe(rom_probe_prefx, rom_probe_prefx_end, ustatus, kstatus);
    rom_assert_eq(41, 1, rom_exc.count);
    rom_assert_eq(42, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(43, 0, get_marker());

    run_probe(rom_probe_prefx, rom_probe_prefx_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(44, 1, rom_exc.count);
    rom_assert_eq(45, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(46, MARKER_VALUE, get_marker());

    /* FCC1 branch is gated; FCC0 is MIPS I and remains available. */
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

    run_probe(rom_probe_bc1_cc0, rom_probe_bc1_cc0_end, ustatus, kstatus);
    rom_assert_eq(53, 1, rom_exc.count);
    rom_assert_eq(54, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(55, MARKER_VALUE, get_marker());

    /*
     * FCC1 compare is unimplemented when XX is clear; FCC0 compare below
     * is MIPS I and must remain available.
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

    set_fsr(0);
    run_probe(rom_probe_ceq_cc1, rom_probe_ceq_cc1_end,
              ustatus | ROM_ST_XX, kstatus);
    rom_assert_eq(62, 1, rom_exc.count);
    rom_assert_eq(63, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(64, FSR_FCC1, get_fsr() & FSR_FCC1);
    rom_assert_eq(65, MARKER_VALUE, get_marker());

    set_fsr(0);
    run_probe(rom_probe_ceq_cc0, rom_probe_ceq_cc0_end, ustatus, kstatus);
    rom_assert_eq(66, 1, rom_exc.count);
    rom_assert_eq(67, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(68, FSR_FCC0, get_fsr() & FSR_FCC0);
    rom_assert_eq(69, MARKER_VALUE, get_marker());

    /* Check the double-precision decoder path as well. */
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

    /* Floating-point MOVN and MOVF have separate decoder paths. */
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

    /* Reciprocal estimate. */
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

    /* Reciprocal square-root estimate. */
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

    /* MADD.S represents the extended arithmetic opcode class. */
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

    /* These instructions remain available in kernel mode with XX clear. */
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

    /* Exercise separate MOVZ and double-precision decoder cases. */
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
     * L-format conversion raises Unimplemented Operation without MIPS III
     * mode (User's Manual 2.0, 17.6). UX enables that mode in user state.
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

    /* User mode must reject a load from the kernel segment with AdEL. */
    run_probe(rom_kload_probe, rom_kload_probe_end, ustatus, kstatus);
    rom_assert_eq(163, 1, rom_exc.count);
    rom_assert_eq(164, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(165, 0xffffffffa0300000ull, rom_exc.badvaddr);
    rom_assert_eq(166, 0, get_marker());

    /*
     * KSU=3 is implemented as User mode (User's Manual 2.0). Performance
     * counter behavior is not checked: a short probe may count zero cycles.
     */
    run_probe(rom_kload_probe, rom_kload_probe_end,
              (ustatus & ~ROM_ST_KSU_USER) | (3u << 3), kstatus);
    rom_assert_eq(167, 1, rom_exc.count);
    rom_assert_eq(168, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(169, 0, get_marker());
}
