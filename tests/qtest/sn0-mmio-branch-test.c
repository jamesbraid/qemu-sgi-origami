/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A MIPS64 branch executed from an MMIO-backed code page.
 *
 * An MMIO-first page caps a translation block at one instruction, so a
 * branch and its delay slot land in separate blocks and the pending-branch
 * state must survive the gap.  The promdata-window regression measured it
 * not surviving: a jr fell through past its delay slot.  Each case runs one
 * branch from such a page and asks whether the target was reached -- a
 * distinct word stored on each path, so the answer is a memory read, and
 * neither path needs a second branch to report.  The window is the
 * per-processor promdata filter, the one executable MMIO a qtest can reach
 * here; two RAM controls ride the same resume path without MMIO.
 *
 * Every configuration below takes the branch, so the drop needs a further
 * condition none of this reproduces.  The MMIO cases are therefore a
 * sentinel: a drop reports g_test_incomplete() rather than failing, since
 * the mechanism is unpinned, while any third outcome -- neither marker, or
 * a fault -- is a hard failure, because it is not the behaviour under
 * study.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

/*
 * The filter covers 32 KiB at physical 1 MiB; state words and RAM controls
 * sit clear of it and of each other.
 */
#define WINDOW_PHYS         0x00100000
#define WINDOW_KSEG1        0xa0100000

#define STATE_PHYS          0x00200000
#define STATE_KSEG1_HI      0xa020

/* Branch on the last word of a page, delay slot on the first of the next. */
#define CROSS_PHYS          0x00301ff4
#define CROSS_KSEG1         0xa0301ff4

/* An ordinary RAM page, nothing special about the offset. */
#define RAM_PHYS            0x00500000
#define RAM_KSEG1           0xa0500000

/* State words, eight bytes each, at STATE_PHYS. */
#define ALIVE_OFF           0x00
#define ENTERED_OFF         0x08
#define FELL_OFF            0x10
#define TARGET_OFF          0x18
#define TRAP_OFF            0x20
#define CAUSE_OFF           0x28
#define EPC_OFF             0x30
#define ITER_OFF            0x38
#define IRQ_OFF             0x40
#define CACHE_AS_RAM_OFF             0x48
#define PROBE_OFF           0x50
#define PROBE2_OFF          0x58

/* Markers.  Each fits an ori immediate, and RAM arrives zeroed. */
#define MARK_ALIVE          0x1111
#define MARK_ENTERED        0x2222
#define MARK_FELL           0x3333
#define MARK_TARGET         0x4444
#define MARK_TRAP           0x5555

/* ROM image.  Loaded at the reset vector, so entry is offset 0. */
#define IMAGE_SIZE          0x1000
/*
 * The machine patches the Origin 200 configuration block into the supplied
 * image at offset 0x60, so the entry branch jumps clear of it and of the
 * boot-exception vectors below.
 */
#define CONFIG_OFFSET       0x60
#define VEC_TLB_REFILL      0x200
#define VEC_XTLB_REFILL     0x280
#define VEC_CACHE_ERROR     0x300
#define VEC_GENERAL         0x380
#define CODE_START          0x400
#define HANDLER_START       0x500
#define ROM_BASE            0xbfc00000

#define POLL_TRIES          20000
#define POLL_INTERVAL_US    1000

/* Instruction encodings, named as in target/mips/tcg/translate.c. */
#define OPC_SPECIAL         (0x00u << 26)
#define OPC_J               (0x02u << 26)
#define OPC_BEQ             (0x04u << 26)
#define OPC_BNE             (0x05u << 26)
#define OPC_BLEZ            (0x06u << 26)
#define OPC_BGTZ            (0x07u << 26)
#define OPC_ADDIU           (0x09u << 26)
#define OPC_ANDI            (0x0cu << 26)
#define OPC_ORI             (0x0du << 26)
#define OPC_DADDIU          (0x19u << 26)
#define OPC_LW              (0x23u << 26)
#define OPC_LD              (0x37u << 26)
#define OPC_LUI             (0x0fu << 26)
#define OPC_SD              (0x3fu << 26)
#define OPC_SLL             (0x00u | OPC_SPECIAL)
#define OPC_JR              (0x08u | OPC_SPECIAL)
#define OPC_SYSCALL         (0x0cu | OPC_SPECIAL)
#define OPC_SUBU            (0x23u | OPC_SPECIAL)
#define OPC_AND             (0x24u | OPC_SPECIAL)
#define OPC_OR              (0x25u | OPC_SPECIAL)
#define OPC_DSLL32          (0x3cu | OPC_SPECIAL)
#define OPC_CP0             (0x10u << 26)
#define OPC_CP1             (0x11u << 26)
#define OPC_DMTC1           (OPC_CP1 | (0x05u << 21))
#define OPC_DMFC1           (OPC_CP1 | (0x01u << 21))
#define OPC_MFC0            (OPC_CP0 | (0x00u << 21))
#define OPC_MTC0            (OPC_CP0 | (0x04u << 21))
#define OPC_C0              (OPC_CP0 | (0x10u << 21))
#define OPC_ERET            (OPC_C0 | 0x18u)

#define R_ZERO              0
#define R_T0                8
#define R_T2                10
#define R_T1                9
#define R_T3                11
#define R_T4                12
#define R_T5                13
#define R_T6                14
#define R_T7                15
#define R_T8                24
#define R_T9                25
#define R_K0                26
#define R_K1                27

#define CP0_COUNT           9
#define CP0_COMPARE         11
#define CP0_STATUS          12
#define CP0_CAUSE           13
#define CP0_EPC             14

/* Status.ERL is bit 2; reset leaves it set. */
#define STATUS_ERL_CLEAR    (-5)
/* Status.KX is bit 7: without it the 64-bit physical segment is unusable. */
#define STATUS_KX           0x80
/* Status.CU1 is bit 29 and Status.FR bit 26: an unambiguous 64-bit FPU. */
#define STATUS_FPU_ON_HI    0x2400

/*
 * The filter answers from its private copy when FPR 0 carries this
 * cache-as-RAM bit, the accessing PC is inside a PROM range, and the
 * access is cached; an uncached reference always sees the RAM underneath.
 */
#define PROM_BOOT_STATUS_CACHE_AS_RAM        0x2

/*
 * XKPHYS at cache attribute 5, the window the kernel's own fetches used.
 * The attribute is load-bearing: the filter's private-copy test reads the
 * mips_uncached MemTxAttr derived from it.
 */
#define XKPHYS_CACHED_HI    0xa800

/* Cause.ExcCode, already shifted: a syscall is 8. */
#define CAUSE_EXCCODE_MASK  0x7c
#define CAUSE_EXCCODE_SYS   (8 << 2)

/* Status.IM7 is the timer's mask bit; Status.IE is the global enable. */
#define STATUS_TIMER_ON     0x8001
/*
 * Counts, not instructions: CP0_Count advances with virtual time, so this is
 * a period the loop below crosses many times over rather than a tick count.
 */
#define TIMER_PERIOD        0x4000
#define LOOP_ITERATIONS     1000

/*
 * The branch under test.  Three flavours, because the translator keeps
 * pending-branch state differently for each: an unconditional immediate
 * branch (MIPS_HFLAG_B), a conditional one (MIPS_HFLAG_BC), and a jump to
 * register (MIPS_HFLAG_BR, which is what the original failure was).
 */
typedef enum {
    BRANCH_JR,
    BRANCH_B,
    BRANCH_BEQ,
} BranchKind;

typedef struct {
    const char *name;
    BranchKind kind;
    uint32_t snippet_phys;      /* where the harness writes the snippet */
    uint32_t snippet_kseg1;     /* how the guest reaches it */
    bool mmio;                  /* the window is I/O-backed */
    bool loop;                  /* iterate, with the timer interrupt armed */
    bool racing;                /* more than one processor runs the program */
    bool xkphys;                /* reach the snippet through XKPHYS */
    bool cache_as_ram;                   /* arm the filter's private-copy condition */
    const char *smp;            /* -smp, which is what gates CF_PARALLEL */
    const char *accel;          /* the accelerator and its thread mode */
    const char *extra;          /* anything else the configuration needs */
} TestCase;

typedef struct {
    uint8_t *code;
    unsigned int pos;
    unsigned int size;
} CodeBuf;

static void emit(CodeBuf *buf, uint32_t insn)
{
    g_assert_cmpuint(buf->pos + 4, <=, buf->size);
    stl_be_p(buf->code + buf->pos, insn);
    buf->pos += 4;
}

static void emit_immediate(CodeBuf *buf, uint32_t opc, int rs, int rt,
                           uint16_t imm)
{
    emit(buf, opc | (rs << 21) | (rt << 16) | imm);
}

static void emit_lui(CodeBuf *buf, int rt, uint16_t imm)
{
    emit_immediate(buf, OPC_LUI, 0, rt, imm);
}

static void emit_ori(CodeBuf *buf, int rt, int rs, uint16_t imm)
{
    emit_immediate(buf, OPC_ORI, rs, rt, imm);
}

static void emit_addiu(CodeBuf *buf, int rt, int rs, int16_t imm)
{
    emit_immediate(buf, OPC_ADDIU, rs, rt, (uint16_t)imm);
}

static void emit_sd(CodeBuf *buf, int rt, int base, uint16_t off)
{
    emit_immediate(buf, OPC_SD, base, rt, off);
}

static void emit_and(CodeBuf *buf, int rd, int rs, int rt)
{
    emit(buf, OPC_AND | (rs << 21) | (rt << 16) | (rd << 11));
}

static void emit_or(CodeBuf *buf, int rd, int rs, int rt)
{
    emit(buf, OPC_OR | (rs << 21) | (rt << 16) | (rd << 11));
}

static void emit_subu(CodeBuf *buf, int rd, int rs, int rt)
{
    emit(buf, OPC_SUBU | (rs << 21) | (rt << 16) | (rd << 11));
}

/* dsll32 rd, rt, 0 -- a plain 32-bit left shift of a 64-bit register. */
static void emit_dsll32(CodeBuf *buf, int rd, int rt)
{
    emit(buf, OPC_DSLL32 | (rt << 16) | (rd << 11));
}

static void emit_andi(CodeBuf *buf, int rt, int rs, uint16_t imm)
{
    emit_immediate(buf, OPC_ANDI, rs, rt, imm);
}

static void emit_daddiu(CodeBuf *buf, int rt, int rs, int16_t imm)
{
    emit_immediate(buf, OPC_DADDIU, rs, rt, (uint16_t)imm);
}

static void emit_ld(CodeBuf *buf, int rt, int base, uint16_t off)
{
    emit_immediate(buf, OPC_LD, base, rt, off);
}

static void emit_nop(CodeBuf *buf)
{
    emit(buf, OPC_SLL);
}

static void emit_eret(CodeBuf *buf)
{
    emit(buf, OPC_ERET);
}

static void emit_dmtc1(CodeBuf *buf, int rt, int fs)
{
    emit(buf, OPC_DMTC1 | (rt << 16) | (fs << 11));
}

static void emit_dmfc1(CodeBuf *buf, int rt, int fs)
{
    emit(buf, OPC_DMFC1 | (rt << 16) | (fs << 11));
}

static void emit_lw(CodeBuf *buf, int rt, int base, uint16_t off)
{
    emit_immediate(buf, OPC_LW, base, rt, off);
}

static void emit_jr(CodeBuf *buf, int rs)
{
    emit(buf, OPC_JR | (rs << 21));
}

/* beq rs, rt, <words> forward from the instruction after the delay slot. */
static void emit_beq(CodeBuf *buf, int rs, int rt, int16_t words)
{
    emit_immediate(buf, OPC_BEQ, rs, rt, (uint16_t)words);
}

/* bne rs, rt, <absolute image offset>, resolved against where we stand. */
static void emit_bne_to(CodeBuf *buf, int rs, int rt, unsigned int target)
{
    int words = ((int)target - (int)buf->pos - 4) / 4;

    emit_immediate(buf, OPC_BNE, rs, rt, (uint16_t)(int16_t)words);
}

static void emit_bgtz_to(CodeBuf *buf, int rs, unsigned int target)
{
    int words = ((int)target - (int)buf->pos - 4) / 4;

    emit_immediate(buf, OPC_BGTZ, rs, 0, (uint16_t)(int16_t)words);
}

static void emit_blez_to(CodeBuf *buf, int rs, unsigned int target)
{
    int words = ((int)target - (int)buf->pos - 4) / 4;

    emit_immediate(buf, OPC_BLEZ, rs, 0, (uint16_t)(int16_t)words);
}

static void emit_syscall(CodeBuf *buf)
{
    emit(buf, OPC_SYSCALL);
}

static void emit_mfc0(CodeBuf *buf, int rt, int rd)
{
    emit(buf, OPC_MFC0 | (rt << 16) | (rd << 11));
}

static void emit_mtc0(CodeBuf *buf, int rt, int rd)
{
    emit(buf, OPC_MTC0 | (rt << 16) | (rd << 11));
}

/* An absolute jump inside the ROM's own 256 MB region. */
static void emit_j(CodeBuf *buf, uint32_t target)
{
    uint32_t here = ROM_BASE + buf->pos;

    g_assert_cmphex(target & 0xf0000000u, ==, (here + 4) & 0xf0000000u);
    emit(buf, OPC_J | ((target >> 2) & 0x03ffffffu));
}

/* Load a sign-extendable 32-bit constant into a register. */
static void emit_load_addr(CodeBuf *buf, int rt, uint32_t addr)
{
    emit_lui(buf, rt, addr >> 16);
    if (addr & 0xffff) {
        emit_ori(buf, rt, rt, addr & 0xffff);
    }
}

/*
 * The snippet, fixed layout so only the branch word changes per flavour:
 *
 *   +0x00  publish "entered"
 *   +0x08  the branch under test, target +0x40
 *   +0x0c  its delay slot
 *   +0x10  publish "fell through" -- reached only on a dropped branch
 *   +0x18  syscall
 *   +0x40  publish "target reached"
 *   +0x48  syscall
 *
 * Neither path branches to report: a reporting branch would itself be the
 * thing under test.
 */
#define SNIPPET_TARGET_OFF  0x40
#define SNIPPET_MAX_WORDS   45

/*
 * The looping form runs three branches per iteration and gives a timer
 * interrupt the branch-to-delay-slot gap to land in.  Each branch has its
 * own dropped signature: "fell through", a counter past its limit, or a
 * short count.
 */
#define LOOP_EXIT_OFF       0x58
#define LOOP_WAIT_IRQ_OFF   0x88
#define LOOP_MMIO_SUCCESS_OFF 0xa8

static unsigned int build_snippet(uint32_t *words, const TestCase *tc)
{
    uint8_t bytes[SNIPPET_MAX_WORDS * 4];
    CodeBuf buf = { .code = bytes, .pos = 0, .size = sizeof(bytes) };
    unsigned int n, i;

    emit_ori(&buf, R_T2, R_ZERO, MARK_ENTERED);
    emit_sd(&buf, R_T2, R_T0, ENTERED_OFF);

    switch (tc->kind) {
    case BRANCH_JR:
        /* t9 holds the target; the ROM entry put it there. */
        emit_jr(&buf, R_T9);
        break;
    case BRANCH_B:
        emit_beq(&buf, R_ZERO, R_ZERO,
                 (SNIPPET_TARGET_OFF - (int)buf.pos - 4) / 4);
        break;
    case BRANCH_BEQ:
        /* t4 and t5 both hold 1, so the condition is computed and true. */
        emit_beq(&buf, R_T4, R_T5,
                 (SNIPPET_TARGET_OFF - (int)buf.pos - 4) / 4);
        break;
    default:
        g_assert_not_reached();
    }
    emit_nop(&buf);

    emit_ori(&buf, R_T2, R_ZERO, MARK_FELL);
    emit_sd(&buf, R_T2, R_T0, FELL_OFF);
    emit_syscall(&buf);

    while (buf.pos < SNIPPET_TARGET_OFF) {
        emit_nop(&buf);
    }

    if (tc->loop) {
        emit_daddiu(&buf, R_T3, R_T3, 1);
        emit_sd(&buf, R_T3, R_T0, ITER_OFF);
        emit_beq(&buf, R_T3, R_T6,
                 (LOOP_EXIT_OFF - (int)buf.pos - 4) / 4);
        emit_nop(&buf);
        /* t8 holds the top of this snippet; the ROM entry jumped through it. */
        emit_jr(&buf, R_T8);
        emit_nop(&buf);
        g_assert_cmpuint(buf.pos, ==, LOOP_EXIT_OFF);
        if (tc->mmio) {
            /*
             * The measured loop can finish before the original deadline.
             * Refresh Compare here, then prove it stayed in the future: an
             * mfc0 Count ends its translation block, so host scheduling can
             * otherwise put the target behind Count before mtc0 Compare.
             */
            emit_mfc0(&buf, R_T2, CP0_COUNT);
            emit_addiu(&buf, R_T2, R_T2, TIMER_PERIOD);
            emit_mtc0(&buf, R_T2, CP0_COMPARE);
            emit_mfc0(&buf, R_T1, CP0_COUNT);
            emit_subu(&buf, R_T1, R_T2, R_T1);
            emit_bgtz_to(&buf, R_T1, LOOP_WAIT_IRQ_OFF);
            emit_nop(&buf);
            emit_blez_to(&buf, R_T1, LOOP_EXIT_OFF);
            emit_nop(&buf);
            emit_ori(&buf, R_T2, R_ZERO, MARK_FELL);
            emit_sd(&buf, R_T2, R_T0, FELL_OFF);
            emit_syscall(&buf);
            g_assert_cmpuint(buf.pos, ==, LOOP_WAIT_IRQ_OFF);
            /* Keep branching here until the timer path has run. */
            emit_ld(&buf, R_T2, R_T0, IRQ_OFF);
            emit_beq(&buf, R_T2, R_ZERO,
                     (LOOP_WAIT_IRQ_OFF - (int)buf.pos - 4) / 4);
            emit_nop(&buf);
            /*
             * A dropped wait branch can fall through with the pre-interrupt
             * zero still in t2.  Record that failure before a peer can hide
             * it by publishing the shared interrupt count.
             */
            emit_bne_to(&buf, R_T2, R_ZERO, LOOP_MMIO_SUCCESS_OFF);
            emit_nop(&buf);
            emit_ori(&buf, R_T2, R_ZERO, MARK_FELL);
            emit_sd(&buf, R_T2, R_T0, FELL_OFF);
            emit_syscall(&buf);
            g_assert_cmpuint(buf.pos, ==, LOOP_MMIO_SUCCESS_OFF);
        }
    }

    emit_ori(&buf, R_T2, R_ZERO, MARK_TARGET);
    emit_sd(&buf, R_T2, R_T0, TARGET_OFF);
    emit_syscall(&buf);

    n = buf.pos / 4;
    for (i = 0; i < n; i++) {
        words[i] = ldl_be_p(bytes + 4 * i);
    }
    return n;
}

/*
 * The ROM image: an entry that hands control to the snippet, and one handler
 * on every boot-exception vector.  The handler acknowledges a timer
 * interrupt and returns; anything else it records before parking in a loop
 * -- in ROM, where branches are not what is under test.
 */
#define FAULT_START         0x580

static void build_rom(uint8_t *image, const TestCase *tc)
{
    CodeBuf buf = { .code = image, .pos = 0, .size = IMAGE_SIZE };
    unsigned int vec;
    static const unsigned int vectors[] = {
        VEC_TLB_REFILL, VEC_XTLB_REFILL, VEC_CACHE_ERROR, VEC_GENERAL,
    };
    unsigned int i;

    emit_j(&buf, ROM_BASE + CODE_START);
    emit_nop(&buf);
    g_assert_cmpuint(buf.pos, <=, CONFIG_OFFSET);

    for (i = 0; i < ARRAY_SIZE(vectors); i++) {
        vec = vectors[i];
        buf.pos = vec;
        emit_j(&buf, ROM_BASE + HANDLER_START);
        emit_nop(&buf);
    }

    buf.pos = CODE_START;
    /*
     * Clear Status.ERL.  Nothing here uses a mapped segment, but reset
     * leaves ERL set, and an interrupt cannot be taken while it is.
     */
    emit_mfc0(&buf, R_T1, CP0_STATUS);
    emit_addiu(&buf, R_T7, R_ZERO, STATUS_ERL_CLEAR);
    emit_and(&buf, R_T1, R_T1, R_T7);
    if (tc->xkphys) {
        emit_ori(&buf, R_T1, R_T1, STATUS_KX);
    }
    emit_mtc0(&buf, R_T1, CP0_STATUS);

    emit_load_addr(&buf, R_T0, STATE_KSEG1_HI << 16);
    emit_ori(&buf, R_T2, R_ZERO, MARK_ALIVE);
    emit_sd(&buf, R_T2, R_T0, ALIVE_OFF);

    if (tc->loop) {
        /* Arm the timer, then let it through. */
        emit_mfc0(&buf, R_T1, CP0_COUNT);
        emit_addiu(&buf, R_T1, R_T1, TIMER_PERIOD);
        emit_mtc0(&buf, R_T1, CP0_COMPARE);
        emit_mfc0(&buf, R_T1, CP0_STATUS);
        emit_ori(&buf, R_T1, R_T1, STATUS_TIMER_ON);
        emit_mtc0(&buf, R_T1, CP0_STATUS);
        emit_ori(&buf, R_T3, R_ZERO, 0);
        emit_ori(&buf, R_T6, R_ZERO, LOOP_ITERATIONS);
    }

    if (tc->kind == BRANCH_BEQ) {
        emit_ori(&buf, R_T4, R_ZERO, 1);
        emit_ori(&buf, R_T5, R_ZERO, 1);
    }

    if (tc->cache_as_ram) {
        /*
         * Arm the private copy from code already inside the boot flash:
         * both halves of the filter's test are then satisfied, and the
         * only open question is which PC the filter reads at the first
         * fetch into the window -- the fetched address or the jumper's.
         */
        emit_mfc0(&buf, R_T1, CP0_STATUS);
        emit_lui(&buf, R_T7, STATUS_FPU_ON_HI);
        emit_or(&buf, R_T1, R_T1, R_T7);
        emit_mtc0(&buf, R_T1, CP0_STATUS);
        emit_ori(&buf, R_T7, R_ZERO, PROM_BOOT_STATUS_CACHE_AS_RAM);
        emit_dmtc1(&buf, R_T7, 0);
        /*
         * Read it back and publish it.  A dmtc1 that the processor ignored
         * would leave every assertion below passing for the wrong reason.
         */
        emit_dmfc1(&buf, R_T7, 0);
        emit_sd(&buf, R_T7, R_T0, CACHE_AS_RAM_OFF);
        /*
         * Probe the window as data from inside the flash: the uncached
         * kseg1 read must see the snippet in RAM, the cached kseg0 read
         * must see the zeroed private copy.  Same instant, same bit, same
         * region resolving two ways -- the whole question.
         */
        emit_load_addr(&buf, R_T1, tc->snippet_kseg1);
        emit_lw(&buf, R_T7, R_T1, 0);
        emit_sd(&buf, R_T7, R_T0, PROBE_OFF);
        emit_load_addr(&buf, R_T1, tc->snippet_phys | 0x80000000);
        emit_lw(&buf, R_T7, R_T1, 0);
        emit_sd(&buf, R_T7, R_T0, PROBE2_OFF);
    }

    if (tc->xkphys) {
        /* t8 = 0xa800000000000000 | snippet_phys, built in halves. */
        emit_lui(&buf, R_T8, XKPHYS_CACHED_HI);
        emit_dsll32(&buf, R_T8, R_T8);
        emit_load_addr(&buf, R_T7, tc->snippet_phys);
        emit_or(&buf, R_T8, R_T8, R_T7);
        if (tc->kind == BRANCH_JR) {
            emit_daddiu(&buf, R_T9, R_T8, SNIPPET_TARGET_OFF);
        }
    } else {
        if (tc->kind == BRANCH_JR) {
            emit_load_addr(&buf, R_T9,
                           tc->snippet_kseg1 + SNIPPET_TARGET_OFF);
        }
        emit_load_addr(&buf, R_T8, tc->snippet_kseg1);
    }
    emit_jr(&buf, R_T8);
    emit_nop(&buf);
    g_assert_cmpuint(buf.pos, <=, HANDLER_START);

    buf.pos = HANDLER_START;
    emit_mfc0(&buf, R_K1, CP0_CAUSE);
    emit_andi(&buf, R_K0, R_K1, CAUSE_EXCCODE_MASK);
    emit_bne_to(&buf, R_K0, R_ZERO, FAULT_START);
    emit_nop(&buf);
    /* A timer interrupt: rearm, count it, and resume where it landed. */
    emit_mfc0(&buf, R_K1, CP0_COUNT);
    emit_addiu(&buf, R_K1, R_K1, TIMER_PERIOD);
    emit_mtc0(&buf, R_K1, CP0_COMPARE);
    emit_load_addr(&buf, R_K0, STATE_KSEG1_HI << 16);
    emit_ld(&buf, R_K1, R_K0, IRQ_OFF);
    emit_daddiu(&buf, R_K1, R_K1, 1);
    emit_sd(&buf, R_K1, R_K0, IRQ_OFF);
    emit_eret(&buf);
    g_assert_cmpuint(buf.pos, <=, FAULT_START);

    buf.pos = FAULT_START;
    emit_load_addr(&buf, R_K0, STATE_KSEG1_HI << 16);
    emit_mfc0(&buf, R_K1, CP0_CAUSE);
    emit_sd(&buf, R_K1, R_K0, CAUSE_OFF);
    emit_mfc0(&buf, R_K1, CP0_EPC);
    emit_sd(&buf, R_K1, R_K0, EPC_OFF);
    emit_ori(&buf, R_K1, R_ZERO, MARK_TRAP);
    emit_sd(&buf, R_K1, R_K0, TRAP_OFF);
    /* park: b park */
    emit_beq(&buf, R_ZERO, R_ZERO, -1);
    emit_nop(&buf);
}

static uint64_t state_word(QTestState *qts, unsigned int off)
{
    return qtest_readq(qts, STATE_PHYS + off);
}

static void report_state(QTestState *qts)
{
    g_test_message("alive 0x%" PRIx64 " entered 0x%" PRIx64
                   " fell 0x%" PRIx64 " target 0x%" PRIx64
                   " trap 0x%" PRIx64 " cause 0x%" PRIx64 " epc 0x%" PRIx64
                   " iterations %" PRIu64 " interrupts %" PRIu64
                   " cache_as_ram 0x%" PRIx64 " probe 0x%" PRIx64
                   " probe2 0x%" PRIx64,
                   state_word(qts, ALIVE_OFF), state_word(qts, ENTERED_OFF),
                   state_word(qts, FELL_OFF), state_word(qts, TARGET_OFF),
                   state_word(qts, TRAP_OFF), state_word(qts, CAUSE_OFF),
                   state_word(qts, EPC_OFF), state_word(qts, ITER_OFF),
                   state_word(qts, IRQ_OFF), state_word(qts, CACHE_AS_RAM_OFF),
                   state_word(qts, PROBE_OFF), state_word(qts, PROBE2_OFF));
}

/*
 * The window must be I/O-backed or the case passes while measuring
 * nothing; read it off the machine's own flat view rather than assuming.
 */
static void assert_window_is_mmio(QTestState *qts)
{
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    const char *hit = strstr(mtree, "promdata-filter");
    const char *line_start;
    g_autofree char *line = NULL;
    const char *line_end;

    g_assert_nonnull(hit);
    for (line_start = hit; line_start > mtree && line_start[-1] != '\n';
         line_start--) {
        continue;
    }
    line_end = strchr(line_start, '\n');
    line = g_strndup(line_start, line_end ? (size_t)(line_end - line_start)
                                          : strlen(line_start));
    g_assert_nonnull(strstr(line, "i/o"));
    g_assert_nonnull(strstr(line, "0000000000100000-"));
}

static QTestState *start_case(const TestCase *tc)
{
    uint8_t image[IMAGE_SIZE] = { 0 };
    uint32_t snippet[SNIPPET_MAX_WORDS];
    g_autofree char *path = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    unsigned int words, i;
    int fd;

    build_rom(image, tc);
    words = build_snippet(snippet, tc);

    fd = g_file_open_tmp("sn0-mmio-branch-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);

    /*
     * The accelerator named here wins over the -accel qtest libqtest
     * appends, because QEMU takes the first one that initialises.  One
     * processor runs, so there is one writer for every state word;
     * -smp maxcpus is separately what decides CF_PARALLEL under MTTCG.
     * Firmware of the machine's own is off: the supplied image is the
     * whole program.
     */
    qts = qtest_initf("-M origin200,synthesized-firmware=off -m 64M %s "
                      "-cpu R10000 -bios %s -accel %s %s "
                      "-no-reboot -no-shutdown -S",
                      tc->smp, path, tc->accel, tc->extra ? tc->extra : "");
    g_assert_cmpint(unlink(path), ==, 0);

    if (tc->mmio) {
        assert_window_is_mmio(qts);
    }

    /*
     * Writes land in the node RAM the filter forwards to, which is the same
     * RAM the guest's fetches read back through it.
     */
    for (i = 0; i < words; i++) {
        qtest_writel(qts, tc->snippet_phys + 4 * i, snippet[i]);
    }

    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    return qts;
}

/* Poll until the handler reports.  False if the deadline expired. */
static bool wait_for_trap(QTestState *qts)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (state_word(qts, TRAP_OFF) == MARK_TRAP) {
            return true;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    return false;
}

static void run_case(const void *data)
{
    const TestCase *tc = data;
    QTestState *qts;
    uint64_t fell, target;
    bool trapped;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    qts = start_case(tc);
    trapped = wait_for_trap(qts);
    if (!trapped) {
        report_state(qts);
    }
    g_assert_true(trapped);

    g_assert_cmphex(state_word(qts, ALIVE_OFF), ==, MARK_ALIVE);

    if (tc->cache_as_ram) {
        /*
         * Three preconditions, or the case is not asking its question:
         * the bit is set, the cached PROM-PC read sees the zeroed private
         * copy, and the uncached read sees the RAM underneath.
         */
        report_state(qts);
        g_assert_cmphex(state_word(qts, CACHE_AS_RAM_OFF), ==, PROM_BOOT_STATUS_CACHE_AS_RAM);
        g_assert_cmphex(state_word(qts, PROBE2_OFF), ==, 0);
        g_assert_cmphex(state_word(qts, PROBE_OFF), ==,
                        OPC_ORI | (R_T2 << 16) | MARK_ENTERED);
    }

    /*
     * Reached only by fetching the window's first instruction; under an
     * armed bit, a stale PC would have fetched the zeroed copy -- a nop --
     * and this marker would never publish.
     */
    g_assert_cmphex(state_word(qts, ENTERED_OFF), ==, MARK_ENTERED);

    fell = state_word(qts, FELL_OFF);
    target = state_word(qts, TARGET_OFF);

    if (target != MARK_TARGET && fell != MARK_FELL) {
        /* Neither path ran to its marker: not the defect under study. */
        report_state(qts);
        g_assert_not_reached();
    }

    if (target == MARK_TARGET) {
        g_assert_cmphex(fell, ==, 0);
        if (tc->loop) {
            /*
             * A short count is a dropped loop-back jump.  With two
             * processors the counters are sums, but a dropped branch from
             * either still publishes the fall-through word.  MMIO loops
             * wait for an interrupt; RAM loops are the control.
             */
            report_state(qts);
            if (!tc->racing) {
                g_assert_cmpuint(state_word(qts, ITER_OFF), ==,
                                 LOOP_ITERATIONS);
            }
            if (tc->mmio) {
                g_assert_cmpuint(state_word(qts, IRQ_OFF), >, 0);
            }
            g_assert_cmphex(state_word(qts, FELL_OFF), ==, 0);
        }
        qtest_quit(qts);
        return;
    }

    report_state(qts);
    if (tc->mmio) {
        /*
         * The watched-for drop, reported rather than failed: the mechanism
         * is unpinned and a failure would overclaim.
         */
        g_test_incomplete("branch executed from MMIO-backed code was dropped");
        qtest_quit(qts);
        return;
    }
    g_assert_cmphex(target, ==, MARK_TARGET);
}

#define SMP_PLAIN       "-smp 1"
#define SMP_PARALLEL    "-smp 1,maxcpus=2"
#define SMP_TWO         "-smp 2"
#define ACCEL_SINGLE    "tcg,thread=single"
#define ACCEL_MULTI     "tcg,thread=multi"

static const TestCase cases[] = {
    {
        .name = "mmio-branch/ram/jr",
        .kind = BRANCH_JR,
        .snippet_phys = RAM_PHYS, .snippet_kseg1 = RAM_KSEG1,
        .smp = SMP_PLAIN, .accel = ACCEL_SINGLE,
    }, {
        .name = "mmio-branch/ram/page-cross-jr",
        .kind = BRANCH_JR,
        .snippet_phys = CROSS_PHYS, .snippet_kseg1 = CROSS_KSEG1,
        .smp = SMP_PLAIN, .accel = ACCEL_SINGLE,
    }, {
        .name = "mmio-branch/ram/page-cross-b",
        .kind = BRANCH_B,
        .snippet_phys = CROSS_PHYS, .snippet_kseg1 = CROSS_KSEG1,
        .smp = SMP_PLAIN, .accel = ACCEL_SINGLE,
    }, {
        .name = "mmio-branch/ram/loop",
        .kind = BRANCH_JR, .loop = true,
        .snippet_phys = RAM_PHYS, .snippet_kseg1 = RAM_KSEG1,
        .smp = SMP_PLAIN, .accel = ACCEL_SINGLE,
    }, {
        .name = "mmio-branch/mmio/jr",
        .kind = BRANCH_JR, .mmio = true,
        .snippet_phys = WINDOW_PHYS, .snippet_kseg1 = WINDOW_KSEG1,
        .smp = SMP_PLAIN, .accel = ACCEL_SINGLE,
    }, {
        .name = "mmio-branch/mmio/b",
        .kind = BRANCH_B, .mmio = true,
        .snippet_phys = WINDOW_PHYS, .snippet_kseg1 = WINDOW_KSEG1,
        .smp = SMP_PLAIN, .accel = ACCEL_SINGLE,
    }, {
        .name = "mmio-branch/mmio/beq",
        .kind = BRANCH_BEQ, .mmio = true,
        .snippet_phys = WINDOW_PHYS, .snippet_kseg1 = WINDOW_KSEG1,
        .smp = SMP_PLAIN, .accel = ACCEL_SINGLE,
    }, {
        /* The measured failure had chaining off: goto_ptr, not goto_tb. */
        .name = "mmio-branch/mmio/jr-nochain",
        .kind = BRANCH_JR, .mmio = true,
        .snippet_phys = WINDOW_PHYS, .snippet_kseg1 = WINDOW_KSEG1,
        .smp = SMP_PLAIN, .accel = ACCEL_SINGLE, .extra = "-d nochain",
    }, {
        /* CF_PARALLEL: MTTCG sets it from maxcpus, not running count. */
        .name = "mmio-branch/mmio/jr-parallel",
        .kind = BRANCH_JR, .mmio = true,
        .snippet_phys = WINDOW_PHYS, .snippet_kseg1 = WINDOW_KSEG1,
        .smp = SMP_PARALLEL, .accel = ACCEL_MULTI,
    }, {
        .name = "mmio-branch/mmio/jr-parallel-nochain",
        .kind = BRANCH_JR, .mmio = true,
        .snippet_phys = WINDOW_PHYS, .snippet_kseg1 = WINDOW_KSEG1,
        .smp = SMP_PARALLEL, .accel = ACCEL_MULTI, .extra = "-d nochain",
    }, {
        .name = "mmio-branch/mmio/loop",
        .kind = BRANCH_JR, .mmio = true, .loop = true,
        .snippet_phys = WINDOW_PHYS, .snippet_kseg1 = WINDOW_KSEG1,
        .smp = SMP_PLAIN, .accel = ACCEL_SINGLE,
    }, {
        /* The kernel's own address form: XKPHYS, not the 32-bit segment. */
        .name = "mmio-branch/mmio/jr-xkphys",
        .kind = BRANCH_JR, .mmio = true, .xkphys = true,
        .snippet_phys = WINDOW_PHYS, .snippet_kseg1 = WINDOW_KSEG1,
        .smp = SMP_PLAIN, .accel = ACCEL_SINGLE,
    }, {
        /*
         * The private copy armed: a stale PC at the first window fetch
         * would read the zeroed copy -- a nop -- and never publish
         * "entered".  Passing measures that the PC is not stale.
         */
        .name = "mmio-branch/mmio/jr-cache-as-ram-armed",
        .kind = BRANCH_JR, .mmio = true, .cache_as_ram = true,
        .snippet_phys = WINDOW_PHYS, .snippet_kseg1 = WINDOW_KSEG1,
        .smp = SMP_PLAIN, .accel = ACCEL_SINGLE,
    }, {
        /*
         * Both processors in the window at once, the measured failure's
         * shape; counters are sums, the fall-through word is the watch.
         */
        .name = "mmio-branch/mmio/loop-smp2",
        .kind = BRANCH_JR, .mmio = true, .loop = true, .racing = true,
        .snippet_phys = WINDOW_PHYS, .snippet_kseg1 = WINDOW_KSEG1,
        .smp = SMP_TWO, .accel = ACCEL_MULTI, .extra = "-d nochain",
    }, {
        .name = "mmio-branch/mmio/loop-parallel-nochain",
        .kind = BRANCH_JR, .mmio = true, .loop = true,
        .snippet_phys = WINDOW_PHYS, .snippet_kseg1 = WINDOW_KSEG1,
        .smp = SMP_PARALLEL, .accel = ACCEL_MULTI, .extra = "-d nochain",
    }, {
        /* Every condition this test can carry, at once. */
        .name = "mmio-branch/mmio/loop-xkphys-smp2",
        .kind = BRANCH_JR, .mmio = true, .loop = true, .racing = true,
        .xkphys = true,
        .snippet_phys = WINDOW_PHYS, .snippet_kseg1 = WINDOW_KSEG1,
        .smp = SMP_TWO, .accel = ACCEL_MULTI, .extra = "-d nochain",
    },
};

int main(int argc, char **argv)
{
    unsigned int i;

    g_test_init(&argc, &argv, NULL);
    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        qtest_add_data_func(cases[i].name, &cases[i], run_case);
    }
    return g_test_run();
}
