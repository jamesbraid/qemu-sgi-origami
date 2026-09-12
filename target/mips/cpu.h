#ifndef MIPS_CPU_H
#define MIPS_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "exec/target_long.h"
#ifndef CONFIG_USER_ONLY
#include "system/memory.h"
#endif
#include "fpu/softfloat-types.h"
#include "hw/core/clock.h"
#include "mips-defs.h"
#include "qemu/qtree.h"

typedef struct CPUMIPSTLBContext CPUMIPSTLBContext;

/*
 * Behaviours a CPU definition selects. Each field names what the processor
 * does, so a use site can say why it branches; a definition maps its own
 * identity onto the set it implements.
 */
typedef struct MIPSCPUConfig {
    /*
     * An uncached or uncached accelerated CCA drives a two-bit attribute out
     * to the memory system, encoded as a window above the physical address.
     */
    bool uncached_attribute;
    /* With ERL set, only the low 2 Gbytes of xuseg are selected. */
    bool erl_restricts_xuseg;
    /*
     * The watch registers hold a physical doubleword address with load and
     * store enables, and have no M bit or ASID.
     */
    bool watch_is_physical;
    /*
     * Soft Reset and NMI both set SR and clear TS, NMI additionally sets NMI,
     * and Cause is left untouched by either.
     */
    bool warm_reset_reports_sr;
    /*
     * A Cache Error exception is error-level: it loads ErrorEPC, sets ERL,
     * writes no exception code, and is ignored entirely while ERL is already
     * set.
     */
    bool cache_error_is_error_level;
    /* Taking any exception breaks the load-linked link. */
    bool exception_breaks_ll_link;
    /* A load, store, SYNC, CACHE, or PREF between LL and SC breaks the link. */
    bool ll_sc_intervening_ops_break_link;
    /* The LLAddr register is plain scratch that the link never writes. */
    bool lladdr_is_scratch;
    /*
     * A denormalized result with the flush bit clear raises Unimplemented
     * Operation.
     */
    bool fp_traps_denormal_result;
    /*
     * Conversion to long refuses results wider than 51 bits with
     * Unimplemented Operation.
     */
    bool fp_cvt_l_limited_to_51_bits;
    /*
     * A failed store is not the processor's to report; the write is posted
     * and completes.
     */
    bool stores_are_posted;
    /*
     * The CACHE instruction's index and hit operations work dedicated tag and
     * data arrays.
     */
    bool has_cache_diagnostics;
    /* The Diagnostic register reaches a branch prediction table. */
    bool has_branch_prediction_table;
    /*
     * The number of control/counter pairs reached through coprocessor 0
     * register 25. Zero means the processor uses the generic register.
     */
    uint8_t performance_counter_pairs;
    /* The control bits accepted by this processor's counter pairs. */
    uint16_t performance_control_rw_bitmask;
    /* Counter overflow contributes to the processor's IP[7] input. */
    bool performance_counter_interrupts;
    /* Writable bits in the R10000-family Diagnostic register. */
    uint64_t diagnostic_rw_bitmask;
    /* External secondary-cache capacity used by diagnostic operations. */
    uint32_t scache_size;
    /*
     * Count and Compare are compared continuously, so a write leaving them
     * equal raises the interrupt at once.
     */
    bool compare_match_is_continuous;
    /*
     * A word-sized MTC0 to a 64-bit EntryLo moves all sixty-four register
     * bits.
     */
    bool mtc0_entrylo_moves_64_bits;
    /* A partial LWR in 64-bit-register mode preserves GPR bits 63:32. */
    bool partial_lwr_preserves_upper;
} MIPSCPUConfig;

/* MSA Context */
#define MSA_WRLEN (128)

typedef union wr_t wr_t;
union wr_t {
    int8_t  b[MSA_WRLEN / 8];
    int16_t h[MSA_WRLEN / 16];
    int32_t w[MSA_WRLEN / 32];
    int64_t d[MSA_WRLEN / 64];
};

typedef union fpr_t fpr_t;
union fpr_t {
    float64  fd;   /* ieee double precision */
    float32  fs[2];/* ieee single precision */
    uint64_t d;    /* binary double fixed-point */
    uint32_t w[2]; /* binary single fixed-point */
/* FPU/MSA register mapping is not tested on big-endian hosts. */
    wr_t     wr;   /* vector data */
};
/*
 *define FP_ENDIAN_IDX to access the same location
 * in the fpr_t union regardless of the host endianness
 */
#if HOST_BIG_ENDIAN
#  define FP_ENDIAN_IDX 1
#else
#  define FP_ENDIAN_IDX 0
#endif

typedef struct CPUMIPSFPUContext CPUMIPSFPUContext;
struct CPUMIPSFPUContext {
    /* Floating point registers */
    fpr_t fpr[32];
    float_status fp_status;
    /* fpu implementation/revision register (fir) */
    uint32_t fcr0;
#define FCR0_FREP 29
#define FCR0_UFRP 28
#define FCR0_HAS2008 23
#define FCR0_F64 22
#define FCR0_L 21
#define FCR0_W 20
#define FCR0_3D 19
#define FCR0_PS 18
#define FCR0_D 17
#define FCR0_S 16
#define FCR0_PRID 8
#define FCR0_REV 0
    /* fcsr */
    uint32_t fcr31_rw_bitmask;
    uint32_t fcr31;
#define FCR31_FS 24
#define FCR31_ABS2008 19
#define FCR31_NAN2008 18
#define SET_FP_COND(num, env)     do { ((env).fcr31) |=                 \
                                       ((num) ? (1 << ((num) + 24)) :   \
                                                (1 << 23));             \
                                     } while (0)
#define CLEAR_FP_COND(num, env)   do { ((env).fcr31) &=                 \
                                       ~((num) ? (1 << ((num) + 24)) :  \
                                                 (1 << 23));            \
                                     } while (0)
#define GET_FP_COND(env)         ((((env).fcr31 >> 24) & 0xfe) |        \
                                 (((env).fcr31 >> 23) & 0x1))
#define GET_FP_CAUSE(reg)        (((reg) >> 12) & 0x3f)
#define GET_FP_ENABLE(reg)       (((reg) >>  7) & 0x1f)
#define GET_FP_FLAGS(reg)        (((reg) >>  2) & 0x1f)
#define SET_FP_CAUSE(reg, v)      do { (reg) = ((reg) & ~(0x3f << 12)) | \
                                               ((v & 0x3f) << 12);       \
                                     } while (0)
#define SET_FP_ENABLE(reg, v)     do { (reg) = ((reg) & ~(0x1f <<  7)) | \
                                               ((v & 0x1f) << 7);        \
                                     } while (0)
#define SET_FP_FLAGS(reg, v)      do { (reg) = ((reg) & ~(0x1f <<  2)) | \
                                               ((v & 0x1f) << 2);        \
                                     } while (0)
#define UPDATE_FP_FLAGS(reg, v)   do { (reg) |= ((v & 0x1f) << 2); } while (0)
#define FP_INEXACT        1
#define FP_UNDERFLOW      2
#define FP_OVERFLOW       4
#define FP_DIV0           8
#define FP_INVALID        16
#define FP_UNIMPLEMENTED  32
};

typedef struct CPUMIPSMVPContext CPUMIPSMVPContext;
struct CPUMIPSMVPContext {
    int32_t CP0_MVPControl;
#define CP0MVPCo_CPA    3
#define CP0MVPCo_STLB   2
#define CP0MVPCo_VPC    1
#define CP0MVPCo_EVP    0
    int32_t CP0_MVPConf0;
#define CP0MVPC0_M      31
#define CP0MVPC0_TLBS   29
#define CP0MVPC0_GS     28
#define CP0MVPC0_PCP    27
#define CP0MVPC0_PTLBE  16
#define CP0MVPC0_TCA    15
#define CP0MVPC0_PVPE   10
#define CP0MVPC0_PTC    0
    int32_t CP0_MVPConf1;
#define CP0MVPC1_CIM    31
#define CP0MVPC1_CIF    30
#define CP0MVPC1_PCX    20
#define CP0MVPC1_PCP2   10
#define CP0MVPC1_PCP1   0
};

typedef struct mips_def_t mips_def_t;

#define MIPS_SHADOW_SET_MAX 16
#define MIPS_TC_MAX 5
#define MIPS_FPU_MAX 1
#define MIPS_DSP_ACC 4
#define MIPS_KSCRATCH_NUM 6
#define MIPS_MAAR_MAX 16 /* Must be an even number. */


/*
 *     Summary of CP0 registers
 *     ========================
 *
 *
 *     Register 0        Register 1        Register 2        Register 3
 *     ----------        ----------        ----------        ----------
 *
 * 0   Index             Random            EntryLo0          EntryLo1
 * 1   MVPControl        VPEControl        TCStatus          GlobalNumber
 * 2   MVPConf0          VPEConf0          TCBind
 * 3   MVPConf1          VPEConf1          TCRestart
 * 4   VPControl         YQMask            TCHalt
 * 5                     VPESchedule       TCContext
 * 6                     VPEScheFBack      TCSchedule
 * 7                     VPEOpt            TCScheFBack       TCOpt
 *
 *
 *     Register 4        Register 5        Register 6        Register 7
 *     ----------        ----------        ----------        ----------
 *
 * 0   Context           PageMask          Wired             HWREna
 * 1   ContextConfig     PageGrain         SRSConf0
 * 2   UserLocal         SegCtl0           SRSConf1
 * 3   XContextConfig    SegCtl1           SRSConf2
 * 4   DebugContextID    SegCtl2           SRSConf3
 * 5   MemoryMapID       PWBase            SRSConf4
 * 6                     PWField           PWCtl
 * 7                     PWSize
 *
 *
 *     Register 8        Register 9        Register 10       Register 11
 *     ----------        ----------        -----------       -----------
 *
 * 0   BadVAddr          Count             EntryHi           Compare
 * 1   BadInstr
 * 2   BadInstrP
 * 3   BadInstrX
 * 4                                       GuestCtl1         GuestCtl0Ext
 * 5                                       GuestCtl2
 * 6                     SAARI             GuestCtl3
 * 7                     SAAR
 *
 *
 *     Register 12       Register 13       Register 14       Register 15
 *     -----------       -----------       -----------       -----------
 *
 * 0   Status            Cause             EPC               PRId
 * 1   IntCtl                                                EBase
 * 2   SRSCtl                              NestedEPC         CDMMBase
 * 3   SRSMap                                                CMGCRBase
 * 4   View_IPL          View_RIPL                           BEVVA
 * 5   SRSMap2           NestedExc
 * 6   GuestCtl0
 * 7   GTOffset
 *
 *
 *     Register 16       Register 17       Register 18       Register 19
 *     -----------       -----------       -----------       -----------
 *
 * 0   Config            LLAddr            WatchLo0          WatchHi
 * 1   Config1           MAAR              WatchLo1          WatchHi
 * 2   Config2           MAARI             WatchLo2          WatchHi
 * 3   Config3                             WatchLo3          WatchHi
 * 4   Config4                             WatchLo4          WatchHi
 * 5   Config5                             WatchLo5          WatchHi
 * 6   Config6                             WatchLo6          WatchHi
 * 7   Config7                             WatchLo7          WatchHi
 *
 *
 *     Register 20       Register 21       Register 22       Register 23
 *     -----------       -----------       -----------       -----------
 *
 * 0   XContext                                              Debug
 * 1                                                         TraceControl
 * 2                                                         TraceControl2
 * 3                                                         UserTraceData1
 * 4                                                         TraceIBPC
 * 5                                                         TraceDBPC
 * 6                                                         Debug2
 * 7
 *
 *
 *     Register 24       Register 25       Register 26       Register 27
 *     -----------       -----------       -----------       -----------
 *
 * 0   DEPC              PerfCnt            ErrCtl          CacheErr
 * 1                     PerfCnt
 * 2   TraceControl3     PerfCnt
 * 3   UserTraceData2    PerfCnt
 * 4                     PerfCnt
 * 5                     PerfCnt
 * 6                     PerfCnt
 * 7                     PerfCnt
 *
 *
 *     Register 28       Register 29       Register 30       Register 31
 *     -----------       -----------       -----------       -----------
 *
 * 0   DataLo            DataHi            ErrorEPC          DESAVE
 * 1   TagLo             TagHi
 * 2   DataLo1           DataHi1                             KScratch<n>
 * 3   TagLo1            TagHi1                              KScratch<n>
 * 4   DataLo2           DataHi2                             KScratch<n>
 * 5   TagLo2            TagHi2                              KScratch<n>
 * 6   DataLo3           DataHi3                             KScratch<n>
 * 7   TagLo3            TagHi3                              KScratch<n>
 *
 */
#define CP0_REGISTER_00     0
#define CP0_REGISTER_01     1
#define CP0_REGISTER_02     2
#define CP0_REGISTER_03     3
#define CP0_REGISTER_04     4
#define CP0_REGISTER_05     5
#define CP0_REGISTER_06     6
#define CP0_REGISTER_07     7
#define CP0_REGISTER_08     8
#define CP0_REGISTER_09     9
#define CP0_REGISTER_10    10
#define CP0_REGISTER_11    11
#define CP0_REGISTER_12    12
#define CP0_REGISTER_13    13
#define CP0_REGISTER_14    14
#define CP0_REGISTER_15    15
#define CP0_REGISTER_16    16
#define CP0_REGISTER_17    17
#define CP0_REGISTER_18    18
#define CP0_REGISTER_19    19
#define CP0_REGISTER_20    20
#define CP0_REGISTER_21    21
#define CP0_REGISTER_22    22
#define CP0_REGISTER_23    23
#define CP0_REGISTER_24    24
#define CP0_REGISTER_25    25
#define CP0_REGISTER_26    26
#define CP0_REGISTER_27    27
#define CP0_REGISTER_28    28
#define CP0_REGISTER_29    29
#define CP0_REGISTER_30    30
#define CP0_REGISTER_31    31


/* CP0 Register 00 */
#define CP0_REG00__INDEX           0
#define CP0_REG00__MVPCONTROL      1
#define CP0_REG00__MVPCONF0        2
#define CP0_REG00__MVPCONF1        3
#define CP0_REG00__VPCONTROL       4
/* CP0 Register 01 */
#define CP0_REG01__RANDOM          0
#define CP0_REG01__VPECONTROL      1
#define CP0_REG01__VPECONF0        2
#define CP0_REG01__VPECONF1        3
#define CP0_REG01__YQMASK          4
#define CP0_REG01__VPESCHEDULE     5
#define CP0_REG01__VPESCHEFBACK    6
#define CP0_REG01__VPEOPT          7
/* CP0 Register 02 */
#define CP0_REG02__ENTRYLO0        0
#define CP0_REG02__TCSTATUS        1
#define CP0_REG02__TCBIND          2
#define CP0_REG02__TCRESTART       3
#define CP0_REG02__TCHALT          4
#define CP0_REG02__TCCONTEXT       5
#define CP0_REG02__TCSCHEDULE      6
#define CP0_REG02__TCSCHEFBACK     7
/* CP0 Register 03 */
#define CP0_REG03__ENTRYLO1        0
#define CP0_REG03__GLOBALNUM       1
#define CP0_REG03__TCOPT           7
/* CP0 Register 04 */
#define CP0_REG04__CONTEXT         0
#define CP0_REG04__CONTEXTCONFIG   1
#define CP0_REG04__USERLOCAL       2
#define CP0_REG04__XCONTEXTCONFIG  3
#define CP0_REG04__DBGCONTEXTID    4
#define CP0_REG04__MMID            5
/* CP0 Register 05 */
#define CP0_REG05__PAGEMASK        0
#define CP0_REG05__PAGEGRAIN       1
#define CP0_REG05__SEGCTL0         2
#define CP0_REG05__SEGCTL1         3
#define CP0_REG05__SEGCTL2         4
#define CP0_REG05__PWBASE          5
#define CP0_REG05__PWFIELD         6
#define CP0_REG05__PWSIZE          7
/* CP0 Register 06 */
#define CP0_REG06__WIRED           0
#define CP0_REG06__SRSCONF0        1
#define CP0_REG06__SRSCONF1        2
#define CP0_REG06__SRSCONF2        3
#define CP0_REG06__SRSCONF3        4
#define CP0_REG06__SRSCONF4        5
#define CP0_REG06__PWCTL           6
/* CP0 Register 07 */
#define CP0_REG07__HWRENA          0
/* CP0 Register 08 */
#define CP0_REG08__BADVADDR        0
#define CP0_REG08__BADINSTR        1
#define CP0_REG08__BADINSTRP       2
#define CP0_REG08__BADINSTRX       3
/* CP0 Register 09 */
#define CP0_REG09__COUNT           0
#define CP0_REG09__SAARI           6
#define CP0_REG09__SAAR            7
/* CP0 Register 10 */
#define CP0_REG10__ENTRYHI         0
#define CP0_REG10__GUESTCTL1       4
#define CP0_REG10__GUESTCTL2       5
#define CP0_REG10__GUESTCTL3       6
/* CP0 Register 11 */
#define CP0_REG11__COMPARE         0
#define CP0_REG11__GUESTCTL0EXT    4
/* CP0 Register 12 */
#define CP0_REG12__STATUS          0
#define CP0_REG12__INTCTL          1
#define CP0_REG12__SRSCTL          2
#define CP0_REG12__SRSMAP          3
#define CP0_REG12__VIEW_IPL        4
#define CP0_REG12__SRSMAP2         5
#define CP0_REG12__GUESTCTL0       6
#define CP0_REG12__GTOFFSET        7
/* CP0 Register 13 */
#define CP0_REG13__CAUSE           0
#define CP0_REG13__VIEW_RIPL       4
#define CP0_REG13__NESTEDEXC       5
/* CP0 Register 14 */
#define CP0_REG14__EPC             0
#define CP0_REG14__NESTEDEPC       2
/* CP0 Register 15 */
#define CP0_REG15__PRID            0
#define CP0_REG15__EBASE           1
#define CP0_REG15__CDMMBASE        2
#define CP0_REG15__CMGCRBASE       3
#define CP0_REG15__BEVVA           4
/* CP0 Register 16 */
#define CP0_REG16__CONFIG          0
#define CP0_REG16__CONFIG1         1
#define CP0_REG16__CONFIG2         2
#define CP0_REG16__CONFIG3         3
#define CP0_REG16__CONFIG4         4
#define CP0_REG16__CONFIG5         5
#define CP0_REG16__CONFIG6         6
#define CP0_REG16__CONFIG7         7
/* CP0 Register 17 */
#define CP0_REG17__LLADDR          0
#define CP0_REG17__MAAR            1
#define CP0_REG17__MAARI           2
/* CP0 Register 18 */
#define CP0_REG18__WATCHLO0        0
#define CP0_REG18__WATCHLO1        1
#define CP0_REG18__WATCHLO2        2
#define CP0_REG18__WATCHLO3        3
#define CP0_REG18__WATCHLO4        4
#define CP0_REG18__WATCHLO5        5
#define CP0_REG18__WATCHLO6        6
#define CP0_REG18__WATCHLO7        7
/* CP0 Register 19 */
#define CP0_REG19__WATCHHI0        0
#define CP0_REG19__WATCHHI1        1
#define CP0_REG19__WATCHHI2        2
#define CP0_REG19__WATCHHI3        3
#define CP0_REG19__WATCHHI4        4
#define CP0_REG19__WATCHHI5        5
#define CP0_REG19__WATCHHI6        6
#define CP0_REG19__WATCHHI7        7
/* CP0 Register 20 */
#define CP0_REG20__XCONTEXT        0
/* CP0 Register 21 */
/* CP0 Register 22 */
/* CP0 Register 23 */
#define CP0_REG23__DEBUG           0
#define CP0_REG23__TRACECONTROL    1
#define CP0_REG23__TRACECONTROL2   2
#define CP0_REG23__USERTRACEDATA1  3
#define CP0_REG23__TRACEIBPC       4
#define CP0_REG23__TRACEDBPC       5
#define CP0_REG23__DEBUG2          6
/* CP0 Register 24 */
#define CP0_REG24__DEPC            0
/* CP0 Register 25 */
#define CP0_REG25__PERFCTL0        0
#define CP0_REG25__PERFCNT0        1
#define CP0_REG25__PERFCTL1        2
#define CP0_REG25__PERFCNT1        3
#define CP0_REG25__PERFCTL2        4
#define CP0_REG25__PERFCNT2        5
#define CP0_REG25__PERFCTL3        6
#define CP0_REG25__PERFCNT3        7
/* CP0 Register 26 */
#define CP0_REG26__ERRCTL          0
/* CP0 Register 27 */
#define CP0_REG27__CACHERR         0

/* External secondary-cache geometry used by R10000-family definitions. */
#define R10K_SCACHE_SIZE       (1024 * 1024)
#define R14K_SCACHE_SIZE       (2 * 1024 * 1024)
#define R10K_SCACHE_4M_SIZE    (4 * 1024 * 1024)
#define R10K_SCACHE_LINE_SIZE  128
#define R10K_SCACHE_WAYS       2
#define R10K_SCACHE_SETS       (R10K_SCACHE_SIZE / R10K_SCACHE_LINE_SIZE / \
                                R10K_SCACHE_WAYS)
#define R10K_SCACHE_TAGS       (R10K_SCACHE_SETS * R10K_SCACHE_WAYS)
#define R10K_SCACHE_QUADWORDS  (R10K_SCACHE_SIZE / 16)
#define R10K_SCACHE_DWORDS     (R10K_SCACHE_SIZE / 8)
#define R14K_SCACHE_SETS       (R14K_SCACHE_SIZE / R10K_SCACHE_LINE_SIZE / \
                                R10K_SCACHE_WAYS)
#define R14K_SCACHE_TAGS       (R14K_SCACHE_SETS * R10K_SCACHE_WAYS)
#define R14K_SCACHE_QUADWORDS  (R14K_SCACHE_SIZE / 16)
#define R14K_SCACHE_DWORDS     (R14K_SCACHE_SIZE / 8)
#define R10K_SCACHE_4M_SETS    (R10K_SCACHE_4M_SIZE / \
                                R10K_SCACHE_LINE_SIZE / R10K_SCACHE_WAYS)
#define R10K_SCACHE_4M_TAGS    (R10K_SCACHE_4M_SETS * R10K_SCACHE_WAYS)
#define R10K_SCACHE_4M_QUADWORDS (R10K_SCACHE_4M_SIZE / 16)
#define R10K_SCACHE_4M_DWORDS  (R10K_SCACHE_4M_SIZE / 8)
#define R10K_SCACHE_TAGLO_MASK 0xffffcdff
#define R10K_SCACHE_TAGHI_MASK 0x8000000f

/*
 * The R10000's on-chip primary caches, both 32 Kbytes and two-way set
 * associative: the instruction cache in 64-byte blocks holding predecoded
 * 36-bit instructions, the data cache in 32-byte blocks. Their index
 * diagnostic operations use the virtual address — VA[13:6] or VA[13:5]
 * names the set, VA[13:2] a word, and VA[0] the way. The tag masks are the
 * defined TagLo/TagHi bits of each cache's tag format; the
 * least-recently-used bit at TagLo[3] belongs to the set rather than to a
 * way, like the secondary cache's most-recently-used bit, and is kept
 * aside the same way.
 */
#define R10K_PCACHE_SIZE       (32 * 1024)
#define R10K_PCACHE_WAYS       2
#define R10K_PCACHE_WORDS      (R10K_PCACHE_SIZE / 4)
#define R10K_ICACHE_LINE_SIZE  64
#define R10K_ICACHE_SETS       (R10K_PCACHE_SIZE / R10K_ICACHE_LINE_SIZE / \
                                R10K_PCACHE_WAYS)
#define R10K_ICACHE_TAGS       (R10K_ICACHE_SETS * R10K_PCACHE_WAYS)
#define R10K_DCACHE_LINE_SIZE  32
#define R10K_DCACHE_SETS       (R10K_PCACHE_SIZE / R10K_DCACHE_LINE_SIZE / \
                                R10K_PCACHE_WAYS)
#define R10K_DCACHE_TAGS       (R10K_DCACHE_SETS * R10K_PCACHE_WAYS)
#define R10K_PCACHE_TAGLO_LRU  (1 << 3)
#define R10K_ICACHE_TAGLO_MASK 0xffffff4d
#define R10K_ICACHE_TAGHI_MASK 0x0000000f
#define R10K_DCACHE_TAGLO_MASK 0xffffffcf
#define R10K_DCACHE_TAGHI_MASK 0xe000000f
/* CP0 Register 28 */
#define CP0_REG28__TAGLO           0
#define CP0_REG28__DATALO          1
#define CP0_REG28__TAGLO1          2
#define CP0_REG28__DATALO1         3
#define CP0_REG28__TAGLO2          4
#define CP0_REG28__DATALO2         5
#define CP0_REG28__TAGLO3          6
#define CP0_REG28__DATALO3         7
/* CP0 Register 29 */
#define CP0_REG29__TAGHI           0
#define CP0_REG29__DATAHI          1
#define CP0_REG29__TAGHI1          2
#define CP0_REG29__DATAHI1         3
#define CP0_REG29__TAGHI2          4
#define CP0_REG29__DATAHI2         5
#define CP0_REG29__TAGHI3          6
#define CP0_REG29__DATAHI3         7
/* CP0 Register 30 */
#define CP0_REG30__ERROREPC        0
/* CP0 Register 31 */
#define CP0_REG31__DESAVE          0
#define CP0_REG31__KSCRATCH1       2
#define CP0_REG31__KSCRATCH2       3
#define CP0_REG31__KSCRATCH3       4
#define CP0_REG31__KSCRATCH4       5
#define CP0_REG31__KSCRATCH5       6
#define CP0_REG31__KSCRATCH6       7


typedef struct TCState TCState;

/*
 * Octeon3 adds a second bank of multiplier/product limbs used by the
 * two-source MTM/MTP forms: MPL0..2/P0..2 from rs and MPL3..5/P3..5 from rt.
 */
#define OCTEON_MULTIPLIER_LANES 3
#define OCTEON_MULTIPLIER_REGS (2 * OCTEON_MULTIPLIER_LANES)

struct TCState {
    target_ulong gpr[32];
#if defined(TARGET_MIPS64)
    /*
     * For CPUs using 128-bit GPR registers, we put the lower halves in gpr[])
     * and the upper halves in gpr_hi[].
     */
    uint64_t gpr_hi[32];
#endif /* TARGET_MIPS64 */
    target_ulong PC;
    target_ulong HI[MIPS_DSP_ACC];
    target_ulong LO[MIPS_DSP_ACC];
    target_ulong ACX[MIPS_DSP_ACC];
    target_ulong DSPControl;
    int32_t CP0_TCStatus;
#define CP0TCSt_TCU3    31
#define CP0TCSt_TCU2    30
#define CP0TCSt_TCU1    29
#define CP0TCSt_TCU0    28
#define CP0TCSt_TMX     27
#define CP0TCSt_RNST    23
#define CP0TCSt_TDS     21
#define CP0TCSt_DT      20
#define CP0TCSt_DA      15
#define CP0TCSt_A       13
#define CP0TCSt_TKSU    11
#define CP0TCSt_IXMT    10
#define CP0TCSt_TASID   0
    int32_t CP0_TCBind;
#define CP0TCBd_CurTC   21
#define CP0TCBd_TBE     17
#define CP0TCBd_CurVPE  0
    target_ulong CP0_TCHalt;
    target_ulong CP0_TCContext;
    target_ulong CP0_TCSchedule;
    target_ulong CP0_TCScheFBack;
    int32_t CP0_Debug_tcstatus;
    target_ulong CP0_UserLocal;
    struct {
        uint64_t MPL[OCTEON_MULTIPLIER_REGS];
        uint64_t P[OCTEON_MULTIPLIER_REGS];
    } octeon;

    int32_t msacsr;

#define MSACSR_FS       24
#define MSACSR_FS_MASK  (1 << MSACSR_FS)
#define MSACSR_NX       18
#define MSACSR_NX_MASK  (1 << MSACSR_NX)
#define MSACSR_CEF      2
#define MSACSR_CEF_MASK (0xffff << MSACSR_CEF)
#define MSACSR_RM       0
#define MSACSR_RM_MASK  (0x3 << MSACSR_RM)
#define MSACSR_MASK     (MSACSR_RM_MASK | MSACSR_CEF_MASK | MSACSR_NX_MASK | \
        MSACSR_FS_MASK)

    float_status msa_fp_status;

#define NUMBER_OF_MXU_REGISTERS 16
    target_ulong mxu_gpr[NUMBER_OF_MXU_REGISTERS - 1];
    target_ulong mxu_cr;
#define MXU_CR_LC       31
#define MXU_CR_RC       30
#define MXU_CR_BIAS     2
#define MXU_CR_RD_EN    1
#define MXU_CR_MXU_EN   0

};

struct MIPSITUState;
typedef struct MIPSOcteonCryptoState {
    union {
        struct {
            uint64_t hsh_dat[16];
            uint64_t hsh_iv[8];
            uint64_t sha3_dat24;
        };
        uint64_t sha3_dat[25];
    };
    uint64_t des3_key[3];
    uint64_t des3_iv;
    uint64_t des3_result;
    uint64_t aes_resinp[2];
    uint64_t aes_iv[2];
    uint64_t aes_key[4];
    uint32_t crc_poly;
    uint32_t crc_iv;
    uint64_t gfm_mul[2];
    uint64_t gfm_resinp[2];
    uint16_t gfm_poly;
    uint8_t aes_keylen;
    uint8_t crc_len;
    uint64_t chord;
    uint64_t llm_data[2];
    QTree *llm36;
    QTree *llm64;
} MIPSOcteonCryptoState;

/*
 * The TagLo bits writable on a processor whose definition does not say.
 * This is the mask this code applied unconditionally before TagLo became
 * a per-processor property, kept so that adding the property changed no
 * existing model's behaviour.
 */
#define CP0_TAGLO_RW_BITMASK_GENERIC 0xFFFFFCF6

/*
 * The Config bits a write preserves, and the PageMask bits a write can set,
 * on a processor whose definition does not say. Both are what this code
 * applied unconditionally before either became a per-processor property: a
 * Config preserve mask cut for the MIPS32 layout, and every bit
 * compute_pagemask() looks at, which narrows no write.
 */
#define CP0_CONFIG0_PRESERVE_BITMASK_GENERIC 0x81FFFFF8
#define CP0_PAGEMASK_RW_BITMASK_GENERIC      0x1FFFE000

typedef struct CPUArchState {
    TCState active_tc;
    CPUMIPSFPUContext active_fpu;

    uint32_t current_tc;

    uint32_t SEGBITS;
    uint32_t PABITS;
#if defined(TARGET_MIPS64)
# define PABITS_BASE 36
#else
# define PABITS_BASE 32
#endif
    target_ulong SEGMask;
    /* The last address xkseg maps, derived from SEGMask at reset. */
    target_ulong xkseg_limit;
    uint64_t PAMask;
#define PAMASK_BASE ((1ULL << PABITS_BASE) - 1)

/*
 * Where the R10000 family's uncached attribute appears to the memory system.
 * The processor drives those two bits beside the address rather than as part
 * of it, so a machine that needs to tell the four uncached spaces apart sees
 * them just above the physical address.
 */
#define MIPS_UNCACHED_WINDOW_SHIFT 40

    int32_t msair;
#define MSAIR_ProcID    8
#define MSAIR_Rev       0

    MIPSOcteonCryptoState octeon_crypto;

/*
 * CP0 Register 0
 */
    int32_t CP0_Index;
    /* CP0_MVP* are per MVP registers. */
    int32_t CP0_VPControl;
#define CP0VPCtl_DIS    0
/*
 * CP0 Register 1
 */
    int32_t CP0_Random;
    /*
     * The virtual-clock reading at the Random register's last consultation,
     * anchoring the cycle-derived decrement. The clock, not Count: Count is
     * guest-writable and a kernel that rewrites it hands the decrement a
     * backward basis. Deliberately unmigrated: a stale anchor costs one
     * arbitrarily-sized step of a register whose contract is to be
     * unpredictable between consultations.
     */
    uint64_t CP0_random_anchor;
    int32_t CP0_VPEControl;
#define CP0VPECo_YSI    21
#define CP0VPECo_GSI    20
#define CP0VPECo_EXCPT  16
#define CP0VPECo_TE     15
#define CP0VPECo_TargTC 0
    int32_t CP0_VPEConf0;
#define CP0VPEC0_M      31
#define CP0VPEC0_XTC    21
#define CP0VPEC0_TCS    19
#define CP0VPEC0_SCS    18
#define CP0VPEC0_DSC    17
#define CP0VPEC0_ICS    16
#define CP0VPEC0_MVP    1
#define CP0VPEC0_VPA    0
    int32_t CP0_VPEConf1;
#define CP0VPEC1_NCX    20
#define CP0VPEC1_NCP2   10
#define CP0VPEC1_NCP1   0
    target_ulong CP0_YQMask;
    target_ulong CP0_VPESchedule;
    target_ulong CP0_VPEScheFBack;
    int32_t CP0_VPEOpt;
#define CP0VPEOpt_IWX7  15
#define CP0VPEOpt_IWX6  14
#define CP0VPEOpt_IWX5  13
#define CP0VPEOpt_IWX4  12
#define CP0VPEOpt_IWX3  11
#define CP0VPEOpt_IWX2  10
#define CP0VPEOpt_IWX1  9
#define CP0VPEOpt_IWX0  8
#define CP0VPEOpt_DWX7  7
#define CP0VPEOpt_DWX6  6
#define CP0VPEOpt_DWX5  5
#define CP0VPEOpt_DWX4  4
#define CP0VPEOpt_DWX3  3
#define CP0VPEOpt_DWX2  2
#define CP0VPEOpt_DWX1  1
#define CP0VPEOpt_DWX0  0
/*
 * CP0 Register 2
 */
    uint64_t CP0_EntryLo0;
/*
 * CP0 Register 3
 */
    uint64_t CP0_EntryLo1;
#if defined(TARGET_MIPS64)
# define CP0EnLo_RI 63
# define CP0EnLo_XI 62
/*
 * The R10000 family's uncached attribute occupies bits 63:62, the same two bits
 * a MIPS32 processor spends on read-inhibit and execute-inhibit. No processor
 * has both, so a TLB write reads the pair one way or the other depending on
 * which processor it is, and never both ways.
 */
# define CP0EnLo_UC 62
#else
# define CP0EnLo_RI 31
# define CP0EnLo_XI 30
#endif
    /*
     * Writable EntryLo bits beyond those the physical address mask names.
     * The R10000 family's uncached attribute sits there; zero elsewhere.
     */
    uint64_t CP0_EntryLo_extra_rw_bitmask;
    int32_t CP0_GlobalNumber;
#define CP0GN_VPId 0
/*
 * CP0 Register 4
 */
    target_ulong CP0_Context;
    int32_t CP0_MemoryMapID;
/*
 * CP0 Register 5
 */
    int32_t CP0_PageMask;
#define CP0PM_MASK 13
    /* The PageMask bits a write can set; the rest read back as zero. */
    int32_t CP0_PageMask_rw_bitmask;
    int32_t CP0_PageGrain_rw_bitmask;
    int32_t CP0_PageGrain;
#define CP0PG_RIE 31
#define CP0PG_XIE 30
#define CP0PG_ELPA 29
#define CP0PG_IEC 27
    target_ulong CP0_SegCtl0;
    target_ulong CP0_SegCtl1;
    target_ulong CP0_SegCtl2;
#define CP0SC_PA        9
#define CP0SC_PA_MASK   (0x7FULL << CP0SC_PA)
#define CP0SC_PA_1GMASK (0x7EULL << CP0SC_PA)
#define CP0SC_AM        4
#define CP0SC_AM_MASK   (0x7ULL << CP0SC_AM)
#define CP0SC_AM_UK     0ULL
#define CP0SC_AM_MK     1ULL
#define CP0SC_AM_MSK    2ULL
#define CP0SC_AM_MUSK   3ULL
#define CP0SC_AM_MUSUK  4ULL
#define CP0SC_AM_USK    5ULL
#define CP0SC_AM_UUSK   7ULL
#define CP0SC_EU        3
#define CP0SC_EU_MASK   (1ULL << CP0SC_EU)
#define CP0SC_C         0
#define CP0SC_C_MASK    (0x7ULL << CP0SC_C)
#define CP0SC_MASK      (CP0SC_C_MASK | CP0SC_EU_MASK | CP0SC_AM_MASK | \
                         CP0SC_PA_MASK)
#define CP0SC_1GMASK    (CP0SC_C_MASK | CP0SC_EU_MASK | CP0SC_AM_MASK | \
                         CP0SC_PA_1GMASK)
#define CP0SC0_MASK     (CP0SC_MASK | (CP0SC_MASK << 16))
#define CP0SC1_XAM      59
#define CP0SC1_XAM_MASK (0x7ULL << CP0SC1_XAM)
#define CP0SC1_MASK     (CP0SC_MASK | (CP0SC_MASK << 16) | CP0SC1_XAM_MASK)
#define CP0SC2_XR       56
#define CP0SC2_XR_MASK  (0xFFULL << CP0SC2_XR)
#define CP0SC2_MASK     (CP0SC_1GMASK | (CP0SC_1GMASK << 16) | CP0SC2_XR_MASK)
    target_ulong CP0_PWBase;
    target_ulong CP0_PWField;
#if defined(TARGET_MIPS64)
#define CP0PF_BDI  32    /* 37..32 */
#define CP0PF_GDI  24    /* 29..24 */
#define CP0PF_UDI  18    /* 23..18 */
#define CP0PF_MDI  12    /* 17..12 */
#define CP0PF_PTI  6     /* 11..6  */
#define CP0PF_PTEI 0     /*  5..0  */
#else
#define CP0PF_GDW  24    /* 29..24 */
#define CP0PF_UDW  18    /* 23..18 */
#define CP0PF_MDW  12    /* 17..12 */
#define CP0PF_PTW  6     /* 11..6  */
#define CP0PF_PTEW 0     /*  5..0  */
#endif
    target_ulong CP0_PWSize;
#if defined(TARGET_MIPS64)
#define CP0PS_BDW  32    /* 37..32 */
#endif
#define CP0PS_PS   30
#define CP0PS_GDW  24    /* 29..24 */
#define CP0PS_UDW  18    /* 23..18 */
#define CP0PS_MDW  12    /* 17..12 */
#define CP0PS_PTW  6     /* 11..6  */
#define CP0PS_PTEW 0     /*  5..0  */
/*
 * CP0 Register 6
 */
    int32_t CP0_Wired;
    int32_t CP0_PWCtl;
#define CP0PC_PWEN      31
#if defined(TARGET_MIPS64)
#define CP0PC_PWDIREXT  30
#define CP0PC_XK        28
#define CP0PC_XS        27
#define CP0PC_XU        26
#endif
#define CP0PC_DPH       7
#define CP0PC_HUGEPG    6
#define CP0PC_PSN       0     /*  5..0  */
    int32_t CP0_SRSConf0_rw_bitmask;
    int32_t CP0_SRSConf0;
#define CP0SRSC0_M      31
#define CP0SRSC0_SRS3   20
#define CP0SRSC0_SRS2   10
#define CP0SRSC0_SRS1   0
    int32_t CP0_SRSConf1_rw_bitmask;
    int32_t CP0_SRSConf1;
#define CP0SRSC1_M      31
#define CP0SRSC1_SRS6   20
#define CP0SRSC1_SRS5   10
#define CP0SRSC1_SRS4   0
    int32_t CP0_SRSConf2_rw_bitmask;
    int32_t CP0_SRSConf2;
#define CP0SRSC2_M      31
#define CP0SRSC2_SRS9   20
#define CP0SRSC2_SRS8   10
#define CP0SRSC2_SRS7   0
    int32_t CP0_SRSConf3_rw_bitmask;
    int32_t CP0_SRSConf3;
#define CP0SRSC3_M      31
#define CP0SRSC3_SRS12  20
#define CP0SRSC3_SRS11  10
#define CP0SRSC3_SRS10  0
    int32_t CP0_SRSConf4_rw_bitmask;
    int32_t CP0_SRSConf4;
#define CP0SRSC4_SRS15  20
#define CP0SRSC4_SRS14  10
#define CP0SRSC4_SRS13  0
/*
 * CP0 Register 7
 */
    int32_t CP0_HWREna;
/*
 * CP0 Register 8
 */
    target_ulong CP0_BadVAddr;
    uint32_t CP0_BadInstr;
    uint32_t CP0_BadInstrP;
    uint32_t CP0_BadInstrX;
/*
 * CP0 Register 9
 */
    int32_t CP0_Count;
#define CP0SAARI_TARGET 0    /*  5..0  */
#define CP0SAAR_BASE    12   /* 43..12 */
#define CP0SAAR_SIZE    1    /*  5..1  */
#define CP0SAAR_EN      0
/*
 * CP0 Register 10
 */
    target_ulong CP0_EntryHi;
#define CP0EnHi_EHINV 10
    target_ulong CP0_EntryHi_ASID_mask;
/*
 * CP0 Register 11
 */
    int32_t CP0_Compare;
    /*
     * The timer's own contribution to Cause.IP7: raised when Count reaches
     * Compare, cleared by a Compare write. Kept apart from Cause because on
     * the R10000 family the performance counters share IP[7], so the line
     * is the OR of this and the counters' overflow condition, and neither
     * source may clear what the other asserted.
     */
    uint32_t cp0_timer_ip7;
/*
 * CP0 Register 12
 */
    int32_t CP0_Status;
#define CP0St_CU3   31
/*
 * The R10000 family has no fourth coprocessor, because the MIPS IV extension
 * took that coprocessor's opcode. The bit becomes XX, which enables MIPS IV in
 * user mode and is ignored in kernel and supervisor mode.
 */
#define CP0St_XX    31
#define CP0St_CU2   30
#define CP0St_CU1   29
#define CP0St_CU0   28
#define CP0St_RP    27
#define CP0St_FR    26
#define CP0St_RE    25
#define CP0St_MX    24
#define CP0St_PX    23
#define CP0St_BEV   22
#define CP0St_TS    21
#define CP0St_SR    20
#define CP0St_NMI   19
#define CP0St_CH    18
#define CP0St_IM    8
#define CP0St_KX    7
#define CP0St_SX    6
#define CP0St_UX    5
#define CP0St_KSU   3
#define CP0St_ERL   2
#define CP0St_EXL   1
#define CP0St_IE    0
    int32_t CP0_IntCtl;
#define CP0IntCtl_IPTI 29
#define CP0IntCtl_IPPCI 26
#define CP0IntCtl_VS 5
    int32_t CP0_SRSCtl;
#define CP0SRSCtl_HSS 26
#define CP0SRSCtl_EICSS 18
#define CP0SRSCtl_ESS 12
#define CP0SRSCtl_PSS 6
#define CP0SRSCtl_CSS 0
    int32_t CP0_SRSMap;
#define CP0SRSMap_SSV7 28
#define CP0SRSMap_SSV6 24
#define CP0SRSMap_SSV5 20
#define CP0SRSMap_SSV4 16
#define CP0SRSMap_SSV3 12
#define CP0SRSMap_SSV2 8
#define CP0SRSMap_SSV1 4
#define CP0SRSMap_SSV0 0
/*
 * CP0 Register 13
 */
    int32_t CP0_Cause;
#define CP0Ca_BD   31
#define CP0Ca_TI   30
#define CP0Ca_CE   28
#define CP0Ca_DC   27
#define CP0Ca_PCI  26
#define CP0Ca_IV   23
#define CP0Ca_WP   22
#define CP0Ca_IP    8
#define CP0Ca_IP_mask 0x0000FF00
#define CP0Ca_EC    2
/*
 * CP0 Register 14
 */
    target_ulong CP0_EPC;
/*
 * CP0 Register 15
 */
    int32_t CP0_PRid;
    target_ulong CP0_EBase;
    target_ulong CP0_EBaseWG_rw_bitmask;
#define CP0EBase_WG 11
    target_ulong CP0_CMGCRBase;
/*
 * CP0 Register 16 (after Release 1)
 */
    int32_t CP0_Config0;
#define CP0C0_M    31
#define CP0C0_K23  28    /* 30..28 */
#define CP0C0_KU   25    /* 27..25 */
#define CP0C0_MDU  20
#define CP0C0_MM   18
#define CP0C0_BM   16
#define CP0C0_Impl 16    /* 24..16 */
#define CP0C0_BE   15
#define CP0C0_AT   13    /* 14..13 */
#define CP0C0_AR   10    /* 12..10 */
#define CP0C0_MT   7     /*  9..7  */
#define CP0C0_VI   3
#define CP0C0_K0   0     /*  2..0  */
#define CP0C0_AR_LENGTH 3
/*
 * CP0 Register 16 (before Release 1)
 */
#define CP0C0_Impl 16    /* 24..16 */
#define CP0C0_IC   9     /* 11..9 */
#define CP0C0_DC   6     /*  8..6 */
#define CP0C0_IB   5
#define CP0C0_DB   4
/*
 * CP0 Register 16 as the R10000 family lays it out, which is neither of the
 * above: the primary cache size fields move to the top of the register and the
 * secondary cache and system interface fields fill the middle. Bits 24..0 are
 * the reset mode bits the board drives, so their values describe the machine
 * rather than the processor. Only BE and K0 sit where the older layout puts
 * them.  Bits 25..22 are reserved and read as zero.
 */
#define CP0C0_R10K_IC 29 /* 31..29 primary instruction cache size */
#define CP0C0_R10K_DC 26 /* 28..26 primary data cache size */
#define CP0C0_R10K_SC 19 /* 21..19 processor to secondary cache clock divisor */
#define CP0C0_R10K_SS 16 /* 18..16 secondary cache size */
#define CP0C0_R10K_SK 14 /* secondary cache data error correction method */
#define CP0C0_R10K_SB 13 /* secondary cache block size */
#define CP0C0_R10K_EC 9  /* 12..9 processor to system interface clock divisor */
#define CP0C0_R10K_PM 7  /* 8..7 maximum outstanding processor requests */
#define CP0C0_R10K_PE 6  /* processor eliminate requests enabled */
#define CP0C0_R10K_CT 5  /* target of coherent requests */
#define CP0C0_R10K_DN 3  /* 4..3 processor device number */
    /* The Config bits a write preserves; it only ever sets the kseg0 field. */
    int32_t CP0_Config0_preserve_bitmask;
    int32_t CP0_Config1;
#define CP0C1_M    31
#define CP0C1_MMU  25    /* 30..25 */
#define CP0C1_IS   22    /* 24..22 */
#define CP0C1_IL   19    /* 21..19 */
#define CP0C1_IA   16    /* 18..16 */
#define CP0C1_DS   13    /* 15..13 */
#define CP0C1_DL   10    /* 12..10 */
#define CP0C1_DA   7     /*  9..7  */
#define CP0C1_C2   6
#define CP0C1_MD   5
#define CP0C1_PC   4
#define CP0C1_WR   3
#define CP0C1_CA   2
#define CP0C1_EP   1
#define CP0C1_FP   0
    int32_t CP0_Config2;
#define CP0C2_M    31
#define CP0C2_TU   28    /* 30..28 */
#define CP0C2_TS   24    /* 27..24 */
#define CP0C2_TL   20    /* 23..20 */
#define CP0C2_TA   16    /* 19..16 */
#define CP0C2_SU   12    /* 15..12 */
#define CP0C2_SS   8     /* 11..8  */
#define CP0C2_SL   4     /*  7..4  */
#define CP0C2_SA   0     /*  3..0  */
    int32_t CP0_Config3;
#define CP0C3_M            31
#define CP0C3_BPG          30
#define CP0C3_CMGCR        29
#define CP0C3_MSAP         28
#define CP0C3_BP           27
#define CP0C3_BI           26
#define CP0C3_SC           25
#define CP0C3_PW           24
#define CP0C3_VZ           23
#define CP0C3_IPLV         21    /* 22..21 */
#define CP0C3_MMAR         18    /* 20..18 */
#define CP0C3_MCU          17
#define CP0C3_ISA_ON_EXC   16
#define CP0C3_ISA          14    /* 15..14 */
#define CP0C3_ULRI         13
#define CP0C3_RXI          12
#define CP0C3_DSP2P        11
#define CP0C3_DSPP         10
#define CP0C3_CTXTC        9
#define CP0C3_ITL          8
#define CP0C3_LPA          7
#define CP0C3_VEIC         6
#define CP0C3_VInt         5
#define CP0C3_SP           4
#define CP0C3_CDMM         3
#define CP0C3_MT           2
#define CP0C3_SM           1
#define CP0C3_TL           0
    int32_t CP0_Config4;
    int32_t CP0_Config4_rw_bitmask;
#define CP0C4_M            31
#define CP0C4_IE           29    /* 30..29 */
#define CP0C4_AE           28
#define CP0C4_VTLBSizeExt  24    /* 27..24 */
#define CP0C4_KScrExist    16
#define CP0C4_MMUExtDef    14
#define CP0C4_FTLBPageSize 8     /* 12..8  */
/* bit layout if MMUExtDef=1 */
#define CP0C4_MMUSizeExt   0     /*  7..0  */
/* bit layout if MMUExtDef=2 */
#define CP0C4_FTLBWays     4     /*  7..4  */
#define CP0C4_FTLBSets     0     /*  3..0  */
    int32_t CP0_Config5;
    int32_t CP0_Config5_rw_bitmask;
#define CP0C5_M            31
#define CP0C5_K            30
#define CP0C5_CV           29
#define CP0C5_EVA          28
#define CP0C5_MSAEn        27
#define CP0C5_PMJ          23    /* 25..23 */
#define CP0C5_WR2          22
#define CP0C5_NMS          21
#define CP0C5_ULS          20
#define CP0C5_XPA          19
#define CP0C5_CRCP         18
#define CP0C5_MI           17
#define CP0C5_GI           15    /* 16..15 */
#define CP0C5_CA2          14
#define CP0C5_XNP          13
#define CP0C5_DEC          11
#define CP0C5_L2C          10
#define CP0C5_UFE          9
#define CP0C5_FRE          8
#define CP0C5_VP           7
#define CP0C5_SBRI         6
#define CP0C5_MVH          5
#define CP0C5_LLB          4
#define CP0C5_MRP          3
#define CP0C5_UFR          2
#define CP0C5_NFExists     0
    int32_t CP0_Config6;
    int32_t CP0_Config6_rw_bitmask;
#define CP0C6_BPPASS          31
#define CP0C6_KPOS            24
#define CP0C6_KE              23
#define CP0C6_VTLBONLY        22
#define CP0C6_LASX            21
#define CP0C6_SSEN            20
#define CP0C6_DISDRTIME       19
#define CP0C6_PIXNUEN         18
#define CP0C6_SCRAND          17
#define CP0C6_LLEXCEN         16
#define CP0C6_DISVC           15
#define CP0C6_VCLRU           14
#define CP0C6_DCLRU           13
#define CP0C6_PIXUEN          12
#define CP0C6_DISBLKLYEN      11
#define CP0C6_UMEMUALEN       10
#define CP0C6_SFBEN           8
#define CP0C6_FLTINT          7
#define CP0C6_VLTINT          6
#define CP0C6_DISBTB          5
#define CP0C6_STPREFCTL       2
#define CP0C6_INSTPREF        1
#define CP0C6_DATAPREF        0
    int32_t CP0_Config7;
    int64_t CP0_Config7_rw_bitmask;
#define CP0C7_WII          31
#define CP0C7_NAPCGEN       2
#define CP0C7_UNIMUEN       1
#define CP0C7_VFPUCGEN      0
    uint64_t CP0_LLAddr;
    uint64_t CP0_MAAR[MIPS_MAAR_MAX];
    int32_t CP0_MAARI;
    /* XXX: Maybe make LLAddr per-TC? */
/*
 * CP0 Register 17
 */
    target_ulong lladdr; /* LL virtual address compared against SC */
    target_ulong llval;
    uint64_t llval_wp;
    uint32_t llnewval_wp;
    uint64_t CP0_LLAddr_rw_bitmask;
    int CP0_LLAddr_shift;
    int32_t CP0_TagLo_rw_bitmask;
    int32_t CP0_ECC_rw_bitmask;
/*
 * CP0 Register 18
 */
    target_ulong CP0_WatchLo[8];
/*
 * CP0 Register 19
 */
    uint64_t CP0_WatchHi[8];
#define CP0WH_ASID 16
#define CP0WH_M    31
/*
 * The R10000 family's WatchLo layout: physical address bits 31:3 above two
 * enable bits, trap-on-load and trap-on-store, with bit 2 ignored on write
 * and read as zero. WatchHi carries physical address bits 39:32.
 */
#define CP0WL_R10K_MASK 0xfffffff8u
#define CP0WL_R10K_R    (1u << 1)
#define CP0WL_R10K_W    (1u << 0)
    /*
     * A Watch exception the R10000 postponed because EXL or ERL was set when
     * the watched doubleword was referenced. The latch is architecturally
     * invisible; it drains into a Watch exception when both bits clear, and
     * a system reset or an MTC0 to WatchLo empties it.
     */
    uint32_t CP0_R10K_DelayedWatch;
/*
 * CP0 Register 20
 */
    target_ulong CP0_XContext;
    int32_t CP0_Framemask;
    /*
     * The FrameMask bits this processor implements, zero where it has no such
     * register. r4k_fill_tlb() masks with this as well, so a processor without
     * the register masks nothing out of a TLB entry as it is written.
     */
    int32_t CP0_Framemask_rw_bitmask;
/*
 * CP0 Register 22, the R10000 family's Diagnostic register. Architecturally 64
 * bits wide, and not all of its fields lie in the low half: the instruction
 * TLB miss counter sits at bits 51:48. The model never moves that counter —
 * there is no instruction TLB behind it — but the register is stored at its
 * architectural width so a doubleword read is a read of the whole register
 * rather than a sign extension of half of it.
 */
    uint64_t CP0_Diagnostic;
#define CP0DI_BPOp    0  /*  1..0  branch prediction table operation */
#define CP0DI_BPIdx   3  /* 11..3  branch prediction table index */
#define CP0DI_BPState 14 /* 15..14 branch prediction table entry state */
#define CP0DI_BPMode 16  /* 17..16 branch prediction algorithm */
/*
 * The fields software writes. The manual's errata calls every field read-only
 * and then names the prediction algorithm as read-write, but the same section
 * gives a code sequence that writes the index and the operation to start a
 * prediction table access and writes the state for a table write, so those
 * three are software-written too. Everything else in the register reports what
 * the branch caches and the instruction TLB last did, and is left read-only.
 */
#define CP0DI_WRITABLE ((3 << CP0DI_BPOp) | (0x1ff << CP0DI_BPIdx) | \
                        (3 << CP0DI_BPState) | (3 << CP0DI_BPMode))
/*
 * The branch prediction table those fields operate on: 512 two-bit entries,
 * indexed by VA[11:3] of a branch, initialized in lines of eight entries
 * named by the upper six index bits. Only the register's table operations
 * touch it here — the model predicts no branches, so the entries hold what
 * the operations put there where real hardware's are rewritten by every
 * verified conditional branch.
 */
#define R10K_BPT_ENTRIES  512
#define R10K_BPT_LINE     8
    uint8_t r10k_bpt[R10K_BPT_ENTRIES];
/*
 * CP0 Register 23
 */
    int32_t CP0_Debug;
#define CP0DB_DBD  31
#define CP0DB_DM   30
#define CP0DB_LSNM 28
#define CP0DB_Doze 27
#define CP0DB_Halt 26
#define CP0DB_CNT  25
#define CP0DB_IBEP 24
#define CP0DB_DBEP 21
#define CP0DB_IEXI 20
#define CP0DB_VER  15
#define CP0DB_DEC  10
#define CP0DB_SSt  8
#define CP0DB_DINT 5
#define CP0DB_DIB  4
#define CP0DB_DDBS 3
#define CP0DB_DDBL 2
#define CP0DB_DBp  1
#define CP0DB_DSS  0
/*
 * CP0 Register 24
 */
    target_ulong CP0_DEPC;
/*
 * CP0 Register 25
 */
    /*
     * Register 25 holds a model-selected number of counters and control
     * registers: R10000 has two pairs and R14000 has four. Even selectors are
     * the control registers and odd ones the counters. The R10000 family
     * reaches them through its own move instructions, whose low bits land in
     * the same place.
     *
     * On the R10000 family a counter whose control register selects the
     * Cycles event and enables the current processor mode is live: its slot
     * holds the counter value minus the current cycle count, the way
     * CP0_Count is kept, and the value is reassembled on read. Every other
     * slot holds its value directly.
     */
    int32_t CP0_Performance[8];
/*
 * The R10000 family's performance control register layout: an event
 * number, an interrupt enable, and one count enable per processor mode.
 */
#define CP0PC_EVENT   5        /* 8..5 the event counted */
#define CP0PC_IE      (1 << 4) /* assert IP[7] while counter bit 31 is set */
#define CP0PC_U       (1 << 3) /* count in user mode, EXL and ERL clear */
#define CP0PC_S       (1 << 2) /* count in supervisor mode, EXL and ERL clear */
#define CP0PC_K       (1 << 1) /* count in kernel mode, EXL and ERL clear */
#define CP0PC_EXL     (1 << 0) /* count while EXL is set, ERL clear */
#define CP0PC_MASK    0x1ff
/*
 * CP0 Register 26
 */
    int32_t CP0_ErrCtl;
#define CP0EC_WST 29
#define CP0EC_SPR 28
#define CP0EC_ITC 26
/*
 * CP0 Register 27, the CacheErr register: read-only to software, loaded by
 * hardware when a Cache Error exception is taken. Nothing in the model
 * raises that exception yet, so the register holds its reset value of zero
 * until the cache-error delivery path exists to load it.
 */
    int32_t CP0_CacheErr;
/*
 * CP0 Register 28
 */
    uint64_t CP0_TagLo;
    int32_t CP0_DataLo;
/*
 * CP0 Register 29
 */
    int32_t CP0_TagHi;
    int32_t CP0_DataHi;

    /* R10000 external secondary-cache diagnostic state. */
    uint32_t r10k_scache_taglo[R10K_SCACHE_4M_TAGS];
    uint8_t r10k_scache_taghi[R10K_SCACHE_4M_TAGS];
    uint8_t r10k_scache_mru[R10K_SCACHE_4M_SETS];
    uint64_t r10k_scache_data[R10K_SCACHE_4M_DWORDS];
    uint16_t r10k_scache_ecc[R10K_SCACHE_4M_QUADWORDS];

    /*
     * R10000 primary-cache diagnostic state. The tag arrays keep each tag's
     * defined TagLo bits except the set-wide least-recently-used bit, which
     * lives in its own per-set array; the data arrays keep a predecoded
     * 36-bit instruction or a 32-bit word per entry with the parity bits
     * software staged through the ECC register beside them.
     */
    uint32_t r10k_icache_taglo[R10K_ICACHE_TAGS];
    uint8_t r10k_icache_taghi[R10K_ICACHE_TAGS];
    uint8_t r10k_icache_lru[R10K_ICACHE_SETS];
    uint64_t r10k_icache_data[R10K_PCACHE_WORDS];
    uint8_t r10k_icache_parity[R10K_PCACHE_WORDS];
    uint32_t r10k_dcache_taglo[R10K_DCACHE_TAGS];
    uint32_t r10k_dcache_taghi[R10K_DCACHE_TAGS];
    uint8_t r10k_dcache_lru[R10K_DCACHE_SETS];
    uint32_t r10k_dcache_data[R10K_PCACHE_WORDS];
    uint8_t r10k_dcache_parity[R10K_PCACHE_WORDS];
/*
 * CP0 Register 30
 */
    target_ulong CP0_ErrorEPC;
/*
 * CP0 Register 31
 */
    int32_t CP0_DESAVE;
    target_ulong CP0_KScratch[MIPS_KSCRATCH_NUM];
/*
 * Loongson CSR CPUCFG registers
 */
    uint32_t lcsr_cpucfg1;
#define CPUCFG1_FP     0
#define CPUCFG1_FPREV  1
#define CPUCFG1_MMI    4
#define CPUCFG1_MSA1   5
#define CPUCFG1_MSA2   6
#define CPUCFG1_LSLDR0 16
#define CPUCFG1_LSPERF 17
#define CPUCFG1_LSPERFX 18
#define CPUCFG1_LSSYNCI 19
#define CPUCFG1_LLEXC   20
#define CPUCFG1_SCRAND  21
#define CPUCFG1_MUALP   25
#define CPUCFG1_KMUALEN 26
#define CPUCFG1_ITLBT   27
#define CPUCFG1_SFBP    29
#define CPUCFG1_CDMAP   30
    uint32_t lcsr_cpucfg2;
#define CPUCFG2_LEXT1   0
#define CPUCFG2_LEXT2   1
#define CPUCFG2_LEXT3   2
#define CPUCFG2_LSPW    3
#define CPUCFG2_LCSRP   27
#define CPUCFG2_LDISBLIKELY 28

    /* We waste some space so we can handle shadow registers like TCs. */
    TCState tcs[MIPS_SHADOW_SET_MAX];
    CPUMIPSFPUContext fpus[MIPS_FPU_MAX];
    /* QEMU */
    int error_code;
#define EXCP_TLB_NOMATCH   0x1
#define EXCP_INST_NOTAVAIL 0x2 /* No valid instruction word for BadInstr */
    uint32_t hflags;    /* CPU State */
    /* TMASK defines different execution modes */
#define MIPS_HFLAG_TMASK  0x3F5807FF
#define MIPS_HFLAG_MODE   0x00007 /* execution modes                    */
    /*
     * The KSU flags must be the lowest bits in hflags. The flag order
     * must be the same as defined for CP0 Status. This allows to use
     * the bits as the value of mmu_idx.
     */
#define MIPS_HFLAG_KSU    0x00003 /* kernel/supervisor/user mode mask   */
#define MIPS_HFLAG_UM     0x00002 /* user mode flag                     */
#define MIPS_HFLAG_SM     0x00001 /* supervisor mode flag               */
#define MIPS_HFLAG_KM     0x00000 /* kernel mode flag                   */
#define MIPS_HFLAG_DM     0x00004 /* Debug mode                         */
#define MIPS_HFLAG_64     0x00008 /* 64-bit instructions enabled        */
#define MIPS_HFLAG_CP0    0x00010 /* CP0 enabled                        */
#define MIPS_HFLAG_FPU    0x00020 /* FPU enabled                        */
#define MIPS_HFLAG_F64    0x00040 /* 64-bit FPU enabled                 */
    /*
     * True if the MIPS IV COP1X instructions can be used.  This also
     * controls the non-COP1X instructions RECIP.S, RECIP.D, RSQRT.S
     * and RSQRT.D.
     */
#define MIPS_HFLAG_COP1X  0x00080 /* COP1X instructions enabled         */
#define MIPS_HFLAG_RE     0x00100 /* Reversed endianness                */
#define MIPS_HFLAG_AWRAP  0x00200 /* 32-bit compatibility address wrapping */
#define MIPS_HFLAG_M16    0x00400 /* MIPS16 mode flag                   */
#define MIPS_HFLAG_M16_SHIFT 10
    /*
     * If translation is interrupted between the branch instruction and
     * the delay slot, record what type of branch it is so that we can
     * resume translation properly.  It might be possible to reduce
     * this from three bits to two.
     */
#define MIPS_HFLAG_BMASK_BASE  0x803800
#define MIPS_HFLAG_B      0x00800 /* Unconditional branch               */
#define MIPS_HFLAG_BC     0x01000 /* Conditional branch                 */
#define MIPS_HFLAG_BL     0x01800 /* Likely branch                      */
#define MIPS_HFLAG_BR     0x02000 /* branch to register (can't link TB) */
    /* Extra flags about the current pending branch.  */
#define MIPS_HFLAG_BMASK_EXT 0x7C000
#define MIPS_HFLAG_B16    0x04000 /* branch instruction was 16 bits     */
#define MIPS_HFLAG_BDS16  0x08000 /* branch requires 16-bit delay slot  */
#define MIPS_HFLAG_BDS32  0x10000 /* branch requires 32-bit delay slot  */
#define MIPS_HFLAG_BDS_STRICT  0x20000 /* Strict delay slot size */
#define MIPS_HFLAG_BX     0x40000 /* branch exchanges execution mode    */
#define MIPS_HFLAG_BMASK  (MIPS_HFLAG_BMASK_BASE | MIPS_HFLAG_BMASK_EXT)
    /* MIPS DSP resources access. */
#define MIPS_HFLAG_DSP    0x080000   /* Enable access to DSP resources.    */
#define MIPS_HFLAG_DSP_R2 0x100000   /* Enable access to DSP R2 resources. */
#define MIPS_HFLAG_DSP_R3 0x20000000 /* Enable access to DSP R3 resources. */
    /* Extra flag about HWREna register. */
#define MIPS_HFLAG_HWRENA_ULR 0x200000 /* ULR bit from HWREna is set. */
#define MIPS_HFLAG_SBRI  0x400000 /* R6 SDBBP causes RI excpt. in user mode */
#define MIPS_HFLAG_FBNSLOT 0x800000 /* Forbidden slot                   */
#define MIPS_HFLAG_MSA   0x1000000
#define MIPS_HFLAG_FRE   0x2000000 /* FRE enabled */
#define MIPS_HFLAG_ELPA  0x4000000
#define MIPS_HFLAG_ITC_CACHE  0x8000000 /* CACHE instr. operates on ITC tag */
#define MIPS_HFLAG_ERL   0x10000000 /* error level flag */
#define MIPS_HFLAG_TB_MASK (MIPS_HFLAG_TMASK | MIPS_HFLAG_BMASK | \
                            MIPS_HFLAG_HWRENA_ULR)

#define TB_FLAG_MIPS_FIXADE  0x40000000
    target_ulong btarget;        /* Jump / branch target               */
    target_ulong bcond;          /* Branch condition (if needed)       */

    int SYNCI_Step; /* Address step size for SYNCI */
    int CCRes; /* Cycle count resolution/divisor */
    uint32_t CP0_Status_rw_bitmask; /* Read/write bits in CP0_Status */
    uint32_t CP0_TCStatus_rw_bitmask; /* Read/write bits in CP0_TCStatus */
    uint64_t insn_flags; /* Supported instruction set */
    MIPSCPUConfig cfg; /* Behaviours this processor selects */

    /* Fields up to this point are cleared by a CPU reset */
    struct {} end_reset_fields;

    /* Fields from here on are preserved across CPU reset. */
#if !defined(CONFIG_USER_ONLY)
    CPUMIPSTLBContext *tlb;
    qemu_irq irq[8];
    MemoryRegion *itc_tag; /* ITC Configuration Tags */

    /* Loongson IOCSR memory */
    struct {
        AddressSpace as;
        MemoryRegion mr;
    } iocsr;
#endif

    const mips_def_t *cpu_model;
    QEMUTimer *timer; /* Internal timer */
    /*
     * Deadline for the next IP[7] edge a live R10000 performance counter
     * will produce: crossing into bit 31, or wrapping back out of it.
     * NULL on every other processor.
     */
    QEMUTimer *r10k_perf_timer;
    target_ulong exception_base; /* ExceptionBase input to the core */
} CPUMIPSState;

/**
 * MIPSCPU:
 * @env: #CPUMIPSState
 * @clock: this CPU input clock (may be connected
 *         to an output clock from another device).
 *
 * A MIPS CPU.
 */
#define MIPS_DEBUG_BREAKPOINTS 8

/* A breakpoint planted by x-mips-debug-breakpoint. */
typedef struct MIPSDebugBreakpoint {
    uint64_t pc;
    uint16_t asid;
    uint8_t exccode;
    bool has_asid;
    bool has_exccode;
    bool used;
} MIPSDebugBreakpoint;

struct ArchCPU {
    CPUState parent_obj;

    CPUMIPSState env;

    Clock *clock;
    Clock *count_clock; /* CP0_Count clock */
    Clock *count_div; /* Divider for CP0_Count clock */

    CPUMIPSMVPContext *mvp;

    /*
     * BQL-protected device and board wiring drives the external NMI input,
     * which queues one exception on each rising edge.  The supported Bedrock
     * source uses qemu_irq_pulse(), so no asserted level persists at a reset
     * or migration boundary.  CPUState.interrupt_request owns and migrates an
     * edge that is waiting for exception entry.
     */
    bool nmi_level;

    target_ulong (*store_transaction)(MIPSCPU *cpu, MemoryRegion *mr,
                                      hwaddr offset,
                                      target_ulong value, unsigned size,
                                      void *opaque);
    void *store_transaction_opaque;

    /* Host debug state for the debug commands, not migrated. */
    MIPSDebugBreakpoint debug_bp[MIPS_DEBUG_BREAKPOINTS];
    bool debug_bp_skip;
    uint64_t debug_bp_skip_pc;

    /* Properties */
    bool is_big_endian;
};

/**
 * MIPSCPUClass:
 * @parent_realize: The parent class' realize handler.
 * @parent_phases: The parent class' reset phase handlers.
 *
 * A MIPS CPU model.
 */
struct MIPSCPUClass {
    CPUClass parent_class;

    DeviceRealize parent_realize;
    DeviceUnrealize parent_unrealize;
    ResettablePhases parent_phases;
    const struct mips_def_t *cpu_def;

    /* Used for the jazz board to modify mips_cpu_do_transaction_failed. */
    bool no_data_aborts;
};

void cpu_wrdsp(uint32_t rs, uint32_t mask_num, CPUMIPSState *env);
uint32_t cpu_rddsp(uint32_t mask_num, CPUMIPSState *env);

/*
 * MMU modes definitions. We carefully match the indices with our
 * hflags layout.
 */
#define MMU_KERNEL_IDX 0
#define MMU_SUPERVISOR_IDX 1
#define MMU_USER_IDX 2
#define MMU_ERL_IDX 3

/*
 * Every index a MIPS translation can be installed under: the three the
 * KSU field selects, plus the one an error-level reference uses. Nothing
 * this target fills reaches any other index, so a flush naming these
 * reaches every entry the target owns.
 */
#define MIPS_MMU_IDXMAP (BIT(MMU_KERNEL_IDX) | BIT(MMU_SUPERVISOR_IDX) | \
                         BIT(MMU_USER_IDX) | BIT(MMU_ERL_IDX))

static inline int hflags_mmu_index(uint32_t hflags)
{
    if (hflags & MIPS_HFLAG_ERL) {
        return MMU_ERL_IDX;
    } else {
        return hflags & MIPS_HFLAG_KSU;
    }
}

static inline int mips_env_mmu_index(const CPUMIPSState *env)
{
    return hflags_mmu_index(env->hflags);
}

/* Exceptions */
enum {
    EXCP_NONE          = -1,
    EXCP_RESET         = 0,
    EXCP_SRESET,
    EXCP_DSS,
    EXCP_DINT,
    EXCP_DDBL,
    EXCP_DDBS,
    EXCP_NMI,
    EXCP_MCHECK,
    EXCP_EXT_INTERRUPT, /* 8 */
    EXCP_DFWATCH,
    EXCP_DIB,
    EXCP_IWATCH,
    EXCP_AdEL,
    EXCP_AdES,
    EXCP_TLBF,
    EXCP_IBE,
    EXCP_DBp, /* 16 */
    EXCP_SYSCALL,
    EXCP_BREAK,
    EXCP_CpU,
    EXCP_RI,
    EXCP_OVERFLOW,
    EXCP_TRAP,
    EXCP_FPE,
    EXCP_DWATCH, /* 24 */
    EXCP_LTLBL,
    EXCP_TLBL,
    EXCP_TLBS,
    EXCP_DBE,
    EXCP_THREAD,
    EXCP_MDMX,
    EXCP_C2E,
    EXCP_CACHE, /* 32 */
    EXCP_DSPDIS,
    EXCP_MSADIS,
    EXCP_MSAFPE,
    EXCP_TLBXI,
    EXCP_TLBRI,
    EXCP_SEMIHOST,

    EXCP_LAST = EXCP_SEMIHOST,
};

/*
 * This is an internally generated WAKE request line.
 * It is driven by the CPU itself. Raised when the MT
 * block wants to wake a VPE from an inactive state and
 * cleared when VPE goes from active to inactive.
 */
#define CPU_INTERRUPT_WAKE CPU_INTERRUPT_TGT_INT_0

/*
 * A postponed R10000 Watch exception waiting for EXL and ERL to clear.
 * Raised when a watched reference is made under either bit, so the exec
 * loop revisits deliverability at each block boundary; delivered and
 * cleared by mips_cpu_exec_interrupt(), or cleared by the MTC0 to WatchLo
 * that empties the latch.
 */
#define CPU_INTERRUPT_DWATCH CPU_INTERRUPT_TGT_INT_1

/* An NMI input edge waits here until the execution loop takes the exception. */
#define CPU_INTERRUPT_NMI CPU_INTERRUPT_TGT_EXT_0

#define CPU_RESOLVING_TYPE TYPE_MIPS_CPU

bool cpu_type_supports_cps_smp(const char *cpu_type);
bool cpu_supports_isa(const CPUMIPSState *env, uint64_t isa_mask);
bool cpu_type_supports_isa(const char *cpu_type, uint64_t isa);

/* Check presence of MIPS-3D ASE */
static inline bool ase_3d_available(const CPUMIPSState *env)
{
    return env->active_fpu.fcr0 & (1 << FCR0_3D);
}

/* Check presence of MSA implementation */
static inline bool ase_msa_available(const CPUMIPSState *env)
{
    return env->CP0_Config3 & (1 << CP0C3_MSAP);
}

/* Check presence of Loongson CSR instructions */
static inline bool ase_lcsr_available(const CPUMIPSState *env)
{
    return env->lcsr_cpucfg2 & (1 << CPUCFG2_LCSRP);
}

/* Check presence of multi-threading ASE implementation */
static inline bool ase_mt_available(const CPUMIPSState *env)
{
    return env->CP0_Config3 & (1 << CP0C3_MT);
}

/**
 * cp0_cause_update: replace the named bits of CP0_Cause
 * @clear: bits to clear
 * @set: bits to set, applied after @clear
 *
 * Returns the value the update replaced.
 *
 * The fields of CP0_Cause belong to different threads: a device raises
 * and lowers IP2-IP7 from whichever thread runs the device model, the
 * count/compare timer owns TI, and the CPU itself writes BD, CE, EC and
 * the software interrupt lines. No one lock covers every writer, so an
 * update composes by touching only the bits it names rather than by
 * writing back a whole word it read earlier.
 */
static inline uint32_t cp0_cause_update(CPUMIPSState *env,
                                        uint32_t clear, uint32_t set)
{
    uint32_t old = qatomic_read(&env->CP0_Cause);

    for (;;) {
        uint32_t val = (old & ~clear) | set;
        uint32_t seen;

        if (val == old) {
            return old;
        }
        seen = qatomic_cmpxchg(&env->CP0_Cause, (int32_t)old, (int32_t)val);
        if (seen == old) {
            return old;
        }
        old = seen;
    }
}

static inline bool cpu_type_is_64bit(const char *cpu_type)
{
    return cpu_type_supports_isa(cpu_type, CPU_MIPS64);
}

void cpu_set_exception_base(int vp_index, target_ulong address);

/* addr.c */
uint64_t cpu_mips_kseg0_to_phys(void *opaque, uint64_t addr);
uint64_t cpu_mips_phys_to_kseg0(void *opaque, uint64_t addr);

uint64_t cpu_mips_kseg1_to_phys(void *opaque, uint64_t addr);
uint64_t cpu_mips_phys_to_kseg1(void *opaque, uint64_t addr);

#if !defined(CONFIG_USER_ONLY)

/* HW declaration specific to the MIPS target */
void cpu_mips_soft_irq(CPUMIPSState *env, int irq, int level);
void cpu_mips_irq_init_cpu(MIPSCPU *cpu);
void cpu_mips_clock_init(MIPSCPU *cpu);

#endif /* !CONFIG_USER_ONLY */

/* helper.c */
target_ulong exception_resume_pc(CPUMIPSState *env);

/* fpu.c */
void cpu_mips_restore_fp_status(CPUMIPSState *env);

/**
 * mips_cpu_create_with_clock:
 * @typename: a MIPS CPU type.
 * @cpu_refclk: this cpu input clock (an output clock of another device)
 * @is_big_endian: whether this CPU is configured in big endianness
 *
 * Instantiates a MIPS CPU, set the input clock of the CPU to @cpu_refclk,
 * then realizes the CPU.
 *
 * Returns: A #CPUState or %NULL if an error occurred.
 */
MIPSCPU *mips_cpu_create_with_clock(const char *cpu_type, Clock *cpu_refclk,
                                    bool is_big_endian);

#endif /* MIPS_CPU_H */
