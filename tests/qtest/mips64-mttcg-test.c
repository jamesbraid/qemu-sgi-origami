/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Four vCPUs exercise the accelerator core on a machine that contains none of
 * this fork's devices.
 *
 * This is the machine-independent rung of the MTTCG ladder.  Every case runs
 * on malta, where the -smp CPUs are plain MIPS cores rather than a CPS
 * cluster, so all four come out of reset free-running at the boot vector and
 * nothing but the accelerator stands between the guest program and the host.
 * A failure here is evidence about target/mips and accel/tcg, not about the
 * SN0 machine model.
 *
 * Each case generates its own MIPS64 program and hands it to QEMU as a raw
 * -bios image: malta copies the flash image to the reset vector, so all four
 * CPUs start executing it with no board firmware in the way.  A CPU learns
 * which of the four it is from the CPUNum field of EBase, which QEMU fills in
 * from the vCPU index at reset.  Identity deliberately does not come from a
 * primitive under test, or a broken primitive makes the failure
 * unattributable.
 *
 * The guest spins are unbounded.  Each CPU publishes a progress word as it
 * enters each phase or iteration, so a stall can be read off the state dump,
 * and the only timeout is the host's polling deadline.  Every image also
 * carries an exception handler on the boot-exception vectors, so a guest that
 * faults reports Cause, BadVAddr and EPC instead of wandering off and looking
 * like a hang.
 *
 * The cases:
 *
 *   mttcg/rendezvous  LL/SC atomicity and cross-thread visibility.  Four CPUs
 *                     rendezvous, then contend for one word with lld/scd.
 *   tlb/shootdown     A vCPU's own tlbwi remap must be coherent with data a
 *                     peer just wrote.  Underneath: r4k_invalidate_tlb's
 *                     tlb_flush_page on the remapping vCPU.
 *   tlb/asid          ASID-tagged translation must follow EntryHi.ASID swaps.
 *                     Underneath: the flush on ASID change in the EntryHi
 *                     write path.
 *   smc/cross-cpu     A peer's store into code another vCPU is executing must
 *                     take effect and never yield a torn instruction.
 *
 * ABA: QEMU lowers SC to a compare-exchange against the value LL read, so it
 * cannot tell a word that never changed from one that changed and changed
 * back.  That is accepted QEMU-wide behaviour, and this test does not probe
 * it.  What the rendezvous case asserts is that the total is exact, which a
 * lost update breaks whether or not ABA is detectable.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define NR_CPUS             4

/*
 * State words live in low RAM, well clear of the flash the image is loaded
 * into, and are reached through KSEG1 so the guest never depends on cache
 * behaviour.  QEMU hands out zeroed RAM, which is what makes "still zero"
 * mean "not yet published" for the spins and starts counters at zero.
 */
#define STATE_PHYS          0x00100000
#define STATE_KSEG1_HI      0xa010

/* Words every case publishes, so the handler and the dump are case-agnostic. */
#define ALIVE_OFF           0x000
#define PROGRESS_OFF        0x020
#define DONE_OFF            0x040
#define EXC_MARK_OFF        0x060
#define EXC_CAUSE_OFF       0x080
#define EXC_VADDR_OFF       0x0a0
#define EXC_EPC_OFF         0x0c0

#define ALIVE_MAGIC         0x00a11e00ULL
#define DONE_MAGIC          0x00d04e00ULL
#define EXC_MAGIC           0x00ecc000ULL
#define FAULT_MAGIC         0x00fa0170ULL

/* Guest program image.  Loaded at the reset vector, so entry is offset 0. */
#define IMAGE_SIZE          0x1000
/*
 * Malta patches its board ID into the word at offset 0x10 of the image it
 * maps at the reset vector, so the entry branch jumps clear of that word and
 * of the boot-exception vectors below.
 */
#define VEC_TLB_REFILL      0x200
#define VEC_XTLB_REFILL     0x280
#define VEC_CACHE_ERROR     0x300
#define VEC_GENERAL         0x380
#define CODE_START          0x400

#define POLL_TRIES          30000
#define POLL_INTERVAL_US    1000

/* Instruction encodings, named as in target/mips/tcg/translate.c. */
#define OPC_SPECIAL         (0x00u << 26)
#define OPC_REGIMM          (0x01u << 26)
#define OPC_BEQ             (0x04u << 26)
#define OPC_BNE             (0x05u << 26)
#define OPC_ADDIU           (0x09u << 26)
#define OPC_SLTIU           (0x0bu << 26)
#define OPC_ANDI            (0x0cu << 26)
#define OPC_ORI             (0x0du << 26)
#define OPC_LUI             (0x0fu << 26)
#define OPC_DADDIU          (0x19u << 26)
#define OPC_SW              (0x2bu << 26)
#define OPC_LLD             (0x34u << 26)
#define OPC_LD              (0x37u << 26)
#define OPC_SCD             (0x3cu << 26)
#define OPC_SD              (0x3fu << 26)
#define OPC_SLL             (0x00u | OPC_SPECIAL)
#define OPC_JR              (0x08u | OPC_SPECIAL)
#define OPC_JALR            (0x09u | OPC_SPECIAL)
#define OPC_SYNC            (0x0fu | OPC_SPECIAL)
#define OPC_ADDU            (0x21u | OPC_SPECIAL)
#define OPC_AND             (0x24u | OPC_SPECIAL)
#define OPC_OR              (0x25u | OPC_SPECIAL)
#define OPC_SLTU            (0x2bu | OPC_SPECIAL)
#define OPC_SYNCI           (OPC_REGIMM | (0x1fu << 16))
#define OPC_CP0             (0x10u << 26)
#define OPC_MFC0            (OPC_CP0 | (0x00u << 21))
#define OPC_MTC0            (OPC_CP0 | (0x04u << 21))
#define OPC_C0              (OPC_CP0 | (0x10u << 21))
#define OPC_TLBWI           (OPC_C0 | 0x02u)
#define OPC_WAIT            (OPC_C0 | 0x20u)
/* JR.HB and JALR.HB are the same opcodes with hint 16 in bits 10:6. */
#define HINT_HB             (16u << 6)

/* General-purpose registers the guest programs use. */
#define R_ZERO              0
#define R_V0                2
#define R_T0                8
#define R_T1                9
#define R_T2                10
#define R_T3                11
#define R_T4                12
#define R_T5                13
#define R_T6                14
#define R_T7                15
#define R_S0                16
#define R_S1                17
#define R_S2                18
#define R_S3                19
#define R_S4                20
#define R_S5                21
#define R_T8                24
#define R_T9                25
#define R_K0                26
#define R_K1                27
#define R_RA                31

/* CP0 registers, by number and select. */
#define CP0_INDEX           0
#define CP0_ENTRYLO0        2
#define CP0_ENTRYLO1        3
#define CP0_PAGEMASK        5
#define CP0_BADVADDR        8
#define CP0_ENTRYHI         10
#define CP0_STATUS          12
#define CP0_CAUSE           13
#define CP0_EPC             14
#define CP0_EBASE           15
#define CP0_EBASE_SEL       1
#define CPUNUM_MASK         0x3ff

/*
 * Status.ERL is bit 2.  Clearing it puts the low mapped segment under the TLB
 * instead of leaving it unmapped, which is what reset hands us.
 */
#define STATUS_ERL_CLEAR    (-5)

/*
 * EntryLo: PFN in bits 29:6, C (cache attribute) in 5:3, D bit 2, V bit 1,
 * G bit 0.  C = 2 is uncached, which is what shared data wants and which
 * costs nothing here because QEMU models no cache.  The entry is global only
 * when bit 0 is set in both halves.
 */
#define ENTRYLO_FLAGS_GLOBAL    0x17u
#define ENTRYLO_FLAGS_ASIDED    0x16u
#define ENTRYLO_GLOBAL(pfn)     (((pfn) << 6) | ENTRYLO_FLAGS_GLOBAL)
/* A global-but-invalid odd half, so the pair is global and only V maps. */
#define ENTRYLO_ODD_UNUSED      0x01u

typedef struct {
    uint8_t *code;
    unsigned int pos;       /* byte offset the next instruction lands at */
} GuestImage;

static void emit(GuestImage *img, uint32_t insn)
{
    g_assert_cmpuint(img->pos + 4, <=, IMAGE_SIZE);
    stl_be_p(img->code + img->pos, insn);
    img->pos += 4;
}

static void emit_immediate(GuestImage *img, uint32_t opc, int rs, int rt,
                           uint16_t imm)
{
    emit(img, opc | (rs << 21) | (rt << 16) | imm);
}

static void emit_lui(GuestImage *img, int rt, uint16_t imm)
{
    emit_immediate(img, OPC_LUI, R_ZERO, rt, imm);
}

static void emit_ori(GuestImage *img, int rt, int rs, uint16_t imm)
{
    emit_immediate(img, OPC_ORI, rs, rt, imm);
}

static void emit_andi(GuestImage *img, int rt, int rs, uint16_t imm)
{
    emit_immediate(img, OPC_ANDI, rs, rt, imm);
}

static void emit_addiu(GuestImage *img, int rt, int rs, int16_t imm)
{
    emit_immediate(img, OPC_ADDIU, rs, rt, (uint16_t)imm);
}

static void emit_daddiu(GuestImage *img, int rt, int rs, int16_t imm)
{
    emit_immediate(img, OPC_DADDIU, rs, rt, (uint16_t)imm);
}

static void emit_sltiu(GuestImage *img, int rt, int rs, uint16_t imm)
{
    emit_immediate(img, OPC_SLTIU, rs, rt, imm);
}

static void emit_ld(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_immediate(img, OPC_LD, base, rt, off);
}

static void emit_sd(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_immediate(img, OPC_SD, base, rt, off);
}

static void emit_sw(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_immediate(img, OPC_SW, base, rt, off);
}

static void emit_lld(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_immediate(img, OPC_LLD, base, rt, off);
}

static void emit_scd(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_immediate(img, OPC_SCD, base, rt, off);
}

static void emit_synci(GuestImage *img, int base, uint16_t off)
{
    emit(img, OPC_SYNCI | (base << 21) | off);
}

static void emit_sll(GuestImage *img, int rd, int rt, int sa)
{
    emit(img, OPC_SLL | (rt << 16) | (rd << 11) | (sa << 6));
}

static void emit_reg3(GuestImage *img, uint32_t opc, int rd, int rs, int rt)
{
    emit(img, opc | (rs << 21) | (rt << 16) | (rd << 11));
}

static void emit_addu(GuestImage *img, int rd, int rs, int rt)
{
    emit_reg3(img, OPC_ADDU, rd, rs, rt);
}

static void emit_and(GuestImage *img, int rd, int rs, int rt)
{
    emit_reg3(img, OPC_AND, rd, rs, rt);
}

static void emit_sltu(GuestImage *img, int rd, int rs, int rt)
{
    emit_reg3(img, OPC_SLTU, rd, rs, rt);
}

static void emit_jr_hb(GuestImage *img, int rs)
{
    emit(img, OPC_JR | (rs << 21) | HINT_HB);
}

static void emit_jalr(GuestImage *img, int rd, int rs)
{
    emit(img, OPC_JALR | (rs << 21) | (rd << 11));
}

static void emit_mfc0(GuestImage *img, int rt, int rd, int sel)
{
    emit(img, OPC_MFC0 | (rt << 16) | (rd << 11) | sel);
}

static void emit_mtc0(GuestImage *img, int rt, int rd, int sel)
{
    emit(img, OPC_MTC0 | (rt << 16) | (rd << 11) | sel);
}

static void emit_tlbwi(GuestImage *img)
{
    emit(img, OPC_TLBWI);
}

static void emit_sync(GuestImage *img)
{
    emit(img, OPC_SYNC);
}

static void emit_nop(GuestImage *img)
{
    emit_sll(img, R_ZERO, R_ZERO, 0);
}

/* EHB is SLL with rd = rt = 0 and sa = 3: an execution hazard barrier. */
static void emit_ehb(GuestImage *img)
{
    emit_sll(img, R_ZERO, R_ZERO, 3);
}

static void emit_wait(GuestImage *img)
{
    emit(img, OPC_WAIT);
}

/* Backward branches take a byte offset within the image and encode it. */
static uint16_t branch_offset(const GuestImage *img, unsigned int target)
{
    int delta = ((int)target - (int)(img->pos + 4)) / 4;

    g_assert_cmpint(delta, >=, -0x8000);
    g_assert_cmpint(delta, <=, 0x7fff);
    return (uint16_t)delta;
}

static void emit_beq(GuestImage *img, int rs, int rt, unsigned int target)
{
    emit_immediate(img, OPC_BEQ, rs, rt, branch_offset(img, target));
}

static void emit_bne(GuestImage *img, int rs, int rt, unsigned int target)
{
    emit_immediate(img, OPC_BNE, rs, rt, branch_offset(img, target));
}

static void emit_b(GuestImage *img, unsigned int target)
{
    emit_beq(img, R_ZERO, R_ZERO, target);
}

/*
 * Forward branches are emitted with a placeholder offset and patched once the
 * target is known.  Hand-counting a forward offset is the thing most likely to
 * go wrong in a program written this way, and a wrong one is a jump into the
 * middle of an unrelated sequence rather than an error anybody sees.
 */
static unsigned int emit_fwd(GuestImage *img, uint32_t opc, int rs, int rt)
{
    unsigned int at = img->pos;

    emit_immediate(img, opc, rs, rt, 0);
    return at;
}

static unsigned int emit_beq_fwd(GuestImage *img, int rs, int rt)
{
    return emit_fwd(img, OPC_BEQ, rs, rt);
}

static unsigned int emit_bne_fwd(GuestImage *img, int rs, int rt)
{
    return emit_fwd(img, OPC_BNE, rs, rt);
}

static unsigned int emit_b_fwd(GuestImage *img)
{
    return emit_fwd(img, OPC_BEQ, R_ZERO, R_ZERO);
}

static void patch_fwd(GuestImage *img, unsigned int at)
{
    int delta = ((int)img->pos - (int)(at + 4)) / 4;
    uint32_t insn = ldl_be_p(img->code + at);

    g_assert_cmpint(delta, >=, 0);
    g_assert_cmpint(delta, <=, 0x7fff);
    stl_be_p(img->code + at, (insn & 0xffff0000u) | (uint32_t)delta);
}

static void emit_li(GuestImage *img, int rt, uint64_t value)
{
    /* lui sign-extends, so only constants with bit 31 clear come out whole. */
    g_assert_cmpuint(value, <, 0x80000000ULL);
    emit_lui(img, rt, value >> 16);
    emit_ori(img, rt, rt, value & 0xffff);
}

/*
 * Load an address in one of the 32-bit compatibility segments.  Here the sign
 * extension lui performs is the point rather than a hazard: KSEG1 0xa0300000
 * becomes 0xffffffffa0300000, which is the address the guest actually uses.
 */
static void emit_li_addr(GuestImage *img, int rt, uint32_t value)
{
    emit_lui(img, rt, value >> 16);
    if (value & 0xffff) {
        emit_ori(img, rt, rt, value & 0xffff);
    }
}

/* Publish a word for this CPU and make it visible before anything after it. */
static void emit_publish(GuestImage *img, int rt, uint16_t off)
{
    emit_sd(img, rt, R_T2, off);
    emit_sync(img);
}

/*
 * Spin until the word at off from the state base is at least as large as rt.
 * Scratches t8 and t9, so rt must not be either — deliberately not t3, which
 * an earlier version used and which is the register callers most often hold
 * the compared value in.
 */
static void emit_wait_ge(GuestImage *img, int rt, uint16_t off)
{
    unsigned int spin = img->pos;

    g_assert_cmpint(rt, !=, R_T8);
    g_assert_cmpint(rt, !=, R_T9);
    emit_ld(img, R_T8, R_T0, off);
    emit_sltu(img, R_T9, R_T8, rt);
    emit_bne(img, R_T9, R_ZERO, spin);
    emit_nop(img);
}

/* Idle rather than spin, so a finished CPU stops competing for host cores. */
static void emit_halt(GuestImage *img)
{
    unsigned int halt = img->pos;

    emit_wait(img);
    emit_b(img, halt);
    emit_nop(img);
}

static void emit_done_and_halt(GuestImage *img)
{
    emit_li(img, R_T3, DONE_MAGIC);
    emit_publish(img, R_T3, DONE_OFF);
    emit_halt(img);
}

/*
 * Every image carries this on the boot-exception vectors.  Without it a guest
 * that takes an unexpected TLB fault runs off into whatever follows and the
 * only symptom is the host's poll deadline expiring, which says nothing about
 * why.  k0 and k1 are the registers the architecture reserves for this, and
 * nothing here returns, so clobbering them is free.
 */
static void emit_exception_handler(GuestImage *img)
{
    emit_mfc0(img, R_K1, CP0_EBASE, CP0_EBASE_SEL);
    emit_andi(img, R_K1, R_K1, CPUNUM_MASK);
    emit_sll(img, R_K1, R_K1, 3);
    emit_lui(img, R_K0, STATE_KSEG1_HI);
    emit_addu(img, R_K1, R_K0, R_K1);

    emit_mfc0(img, R_K0, CP0_CAUSE, 0);
    emit_sd(img, R_K0, R_K1, EXC_CAUSE_OFF);
    emit_mfc0(img, R_K0, CP0_BADVADDR, 0);
    emit_sd(img, R_K0, R_K1, EXC_VADDR_OFF);
    emit_mfc0(img, R_K0, CP0_EPC, 0);
    emit_sd(img, R_K0, R_K1, EXC_EPC_OFF);
    emit_li(img, R_K0, EXC_MAGIC);
    emit_sd(img, R_K0, R_K1, EXC_MARK_OFF);
    emit_sync(img);
    emit_halt(img);
}

/*
 * Common entry: identify this CPU, park the state pointers, put the low
 * mapped segment under the TLB by clearing Status.ERL, fix the page size, and
 * say the CPU is running.
 *
 * Held across every case: k0 = CPUNum, t0 = state base, t2 = this CPU's words.
 */
static void emit_preamble(GuestImage *img)
{
    emit_mfc0(img, R_K0, CP0_EBASE, CP0_EBASE_SEL);
    emit_andi(img, R_K0, R_K0, CPUNUM_MASK);
    emit_lui(img, R_T0, STATE_KSEG1_HI);
    emit_sll(img, R_T1, R_K0, 3);
    emit_addu(img, R_T2, R_T0, R_T1);

    emit_mfc0(img, R_T3, CP0_STATUS, 0);
    emit_addiu(img, R_T4, R_ZERO, STATUS_ERL_CLEAR);
    emit_and(img, R_T3, R_T3, R_T4);
    emit_mtc0(img, R_T3, CP0_STATUS, 0);
    emit_ehb(img);

    emit_mtc0(img, R_ZERO, CP0_PAGEMASK, 0);
    emit_ehb(img);

    emit_li(img, R_T3, ALIVE_MAGIC);
    emit_publish(img, R_T3, ALIVE_OFF);
}

/*
 * Write one TLB entry: virtual page (and ASID) in vreg, even-half EntryLo in
 * loreg, odd half as given, at a fixed index.  Scratches t8 and t9, so vreg
 * and loreg must not be either.
 */
static void emit_tlb_write(GuestImage *img, int vreg, int loreg,
                           int index, unsigned int odd_half)
{
    g_assert_cmpint(vreg, !=, R_T8);
    g_assert_cmpint(vreg, !=, R_T9);
    g_assert_cmpint(loreg, !=, R_T8);
    g_assert_cmpint(loreg, !=, R_T9);

    emit_mtc0(img, vreg, CP0_ENTRYHI, 0);
    emit_ehb(img);
    emit_mtc0(img, loreg, CP0_ENTRYLO0, 0);
    emit_li(img, R_T8, odd_half);
    emit_mtc0(img, R_T8, CP0_ENTRYLO1, 0);
    emit_addiu(img, R_T9, R_ZERO, index);
    emit_mtc0(img, R_T9, CP0_INDEX, 0);
    emit_ehb(img);
    emit_tlbwi(img);
    emit_ehb(img);
}

static void write_guest_image(int fd, void (*build)(GuestImage *img))
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    ssize_t written;

    emit_b(&img, CODE_START);
    emit_nop(&img);

    /* The refill and cache-error vectors join the general one. */
    img.pos = VEC_TLB_REFILL;
    emit_b(&img, VEC_GENERAL);
    emit_nop(&img);
    img.pos = VEC_XTLB_REFILL;
    emit_b(&img, VEC_GENERAL);
    emit_nop(&img);
    img.pos = VEC_CACHE_ERROR;
    emit_b(&img, VEC_GENERAL);
    emit_nop(&img);

    img.pos = VEC_GENERAL;
    emit_exception_handler(&img);
    g_assert_cmpuint(img.pos, <=, CODE_START);

    img.pos = CODE_START;
    emit_preamble(&img);
    build(&img);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

/* ---------------------------------------------------------------- */
/* Case: mttcg/rendezvous                                           */
/* ---------------------------------------------------------------- */

#define RV_SLOT_OFF         0x100
#define RV_COUNTER_OFF      0x120

/* The phase a CPU has published once its increments are all retired. */
#define RV_PROGRESS_COUNTED 4

static unsigned int rv_increments;

static void emit_progress(GuestImage *img, int phase)
{
    emit_addiu(img, R_T4, R_ZERO, phase);
    emit_publish(img, R_T4, PROGRESS_OFF);
}

static void build_rendezvous(GuestImage *img)
{
    unsigned int spin, retry, i;

    g_assert_cmpuint(rv_increments, <=, 0x7fff);

    emit_progress(img, 1);

    /* Claim slot CPUNum by writing CPUNum + 1 into it. */
    emit_addiu(img, R_T3, R_K0, 1);
    emit_publish(img, R_T3, RV_SLOT_OFF);
    emit_progress(img, 2);

    /* Wait until every slot has been claimed. */
    spin = img->pos;
    for (i = 0; i < NR_CPUS; i++) {
        emit_ld(img, R_T5, R_T0, RV_SLOT_OFF + 8 * i);
        emit_beq(img, R_T5, R_ZERO, spin);
        emit_nop(img);
    }
    emit_sync(img);

    /* Every CPU increments the one shared word, N times each. */
    emit_progress(img, 3);
    emit_addiu(img, R_T6, R_ZERO, rv_increments);
    retry = img->pos;
    emit_lld(img, R_T7, R_T0, RV_COUNTER_OFF);
    emit_daddiu(img, R_T7, R_T7, 1);
    emit_scd(img, R_T7, R_T0, RV_COUNTER_OFF);
    /* SC leaves 0 in its source register when the store did not happen. */
    emit_beq(img, R_T7, R_ZERO, retry);
    emit_nop(img);
    emit_addiu(img, R_T6, R_T6, -1);
    emit_bne(img, R_T6, R_ZERO, retry);
    emit_nop(img);
    emit_sync(img);

    /*
     * Wait until every CPU has retired its increments, so that the DONE words
     * the host waits on can only appear once the count is final.  The test is
     * "reached at least this phase" rather than "is at this phase", because a
     * CPU that leaves the rendezvous first goes on to publish a later phase
     * while the others are still watching it.
     */
    emit_progress(img, RV_PROGRESS_COUNTED);
    spin = img->pos;
    for (i = 0; i < NR_CPUS; i++) {
        emit_ld(img, R_T5, R_T0, PROGRESS_OFF + 8 * i);
        emit_sltiu(img, R_T8, R_T5, RV_PROGRESS_COUNTED);
        emit_bne(img, R_T8, R_ZERO, spin);
        emit_nop(img);
    }
    emit_sync(img);

    emit_progress(img, RV_PROGRESS_COUNTED + 1);
    emit_done_and_halt(img);
}

/* ---------------------------------------------------------------- */
/* Case: tlb/shootdown                                              */
/* ---------------------------------------------------------------- */

#define SD_MAILBOX_OFF      0x100
#define SD_ACK_OFF          0x120
#define SD_FAULT_OFF        0x140
#define SD_SEEN_OFF         0x160
#define SD_FGEN_OFF         0x180

/* Per-CPU virtual page, 8 KiB apart so each CPU owns one even/odd pair. */
#define SD_VBASE            0x00400000
/* The two physical pages the mapping ping-pongs between. */
#define SD_PAGE0_PFN        0x200
#define SD_PAGE1_PFN        0x201
#define SD_PAGE0_KSEG1_HI   0xa020
#define SD_TLB_INDEX        1
/*
 * Generations.  Modest on purpose so the whole binary stays far inside the
 * 60 s qtest ceiling; a soak harness scales repetition externally rather than
 * by raising this.
 */
#define SD_GENERATIONS      256

static void build_tlb_shootdown(GuestImage *img)
{
    unsigned int loop, leader_skip, page0, joined, ack_skip, no_fault, i;
    unsigned int fault_done;

    /* s1 = this CPU's virtual page. */
    emit_li(img, R_S1, SD_VBASE);
    emit_sll(img, R_T3, R_K0, 13);
    emit_addu(img, R_S1, R_S1, R_T3);

    /* Start mapped at page 0, which is generation 0's page. */
    emit_li(img, R_S2, ENTRYLO_GLOBAL(SD_PAGE0_PFN));
    emit_tlb_write(img, R_S1, R_S2, SD_TLB_INDEX, ENTRYLO_ODD_UNUSED);

    /* s0 = generation, counting from 1. */
    emit_addiu(img, R_S0, R_ZERO, 1);

    loop = img->pos;
    emit_publish(img, R_S0, PROGRESS_OFF);

    /*
     * CPU 0 publishes generation g: it writes g's pattern into the page the
     * mapping is about to move to, which is the page nobody is reading, then
     * announces g.  It only gets here once every CPU has acknowledged g-1, so
     * the page it writes is not one a peer still has mapped.
     */
    leader_skip = emit_bne_fwd(img, R_K0, R_ZERO);
    emit_nop(img);

    emit_andi(img, R_T3, R_S0, 1);
    page0 = emit_beq_fwd(img, R_T3, R_ZERO);
    emit_nop(img);
    emit_lui(img, R_T4, SD_PAGE0_KSEG1_HI);
    emit_ori(img, R_T4, R_T4, 0x1000);
    joined = emit_b_fwd(img);
    emit_nop(img);
    patch_fwd(img, page0);
    emit_lui(img, R_T4, SD_PAGE0_KSEG1_HI);
    patch_fwd(img, joined);

    /* The pattern is the generation number, so staleness names itself. */
    emit_sd(img, R_S0, R_T4, 0);
    emit_sync(img);
    emit_sd(img, R_S0, R_T0, SD_MAILBOX_OFF);
    emit_sync(img);

    patch_fwd(img, leader_skip);

    /* Everyone waits for generation g to be announced. */
    emit_wait_ge(img, R_S0, SD_MAILBOX_OFF);
    emit_sync(img);

    /* Remap this CPU's V to generation g's page with its own tlbwi. */
    emit_andi(img, R_T3, R_S0, 1);
    page0 = emit_beq_fwd(img, R_T3, R_ZERO);
    emit_nop(img);
    emit_li(img, R_S2, ENTRYLO_GLOBAL(SD_PAGE1_PFN));
    joined = emit_b_fwd(img);
    emit_nop(img);
    patch_fwd(img, page0);
    emit_li(img, R_S2, ENTRYLO_GLOBAL(SD_PAGE0_PFN));
    patch_fwd(img, joined);
    emit_tlb_write(img, R_S1, R_S2, SD_TLB_INDEX, ENTRYLO_ODD_UNUSED);

    /* Read through V.  Anything but generation g is a stale translation. */
    emit_ld(img, R_T7, R_S1, 0);
    no_fault = emit_beq_fwd(img, R_T7, R_S0);
    emit_nop(img);

    /* Record the first fault only, with what was read and when. */
    emit_ld(img, R_T3, R_T2, SD_FAULT_OFF);
    fault_done = emit_bne_fwd(img, R_T3, R_ZERO);
    emit_nop(img);
    emit_sd(img, R_T7, R_T2, SD_SEEN_OFF);
    emit_sd(img, R_S0, R_T2, SD_FGEN_OFF);
    emit_li(img, R_T3, FAULT_MAGIC);
    emit_sd(img, R_T3, R_T2, SD_FAULT_OFF);
    emit_sync(img);
    patch_fwd(img, fault_done);
    patch_fwd(img, no_fault);

    emit_publish(img, R_S0, SD_ACK_OFF);

    /* CPU 0 waits for all four before it may touch the other page again. */
    ack_skip = emit_bne_fwd(img, R_K0, R_ZERO);
    emit_nop(img);
    for (i = 0; i < NR_CPUS; i++) {
        emit_wait_ge(img, R_S0, SD_ACK_OFF + 8 * i);
    }
    patch_fwd(img, ack_skip);

    emit_addiu(img, R_S0, R_S0, 1);
    emit_addiu(img, R_T3, R_ZERO, SD_GENERATIONS);
    emit_sltu(img, R_T4, R_T3, R_S0);
    emit_beq(img, R_T4, R_ZERO, loop);
    emit_nop(img);

    emit_done_and_halt(img);
}

/* ---------------------------------------------------------------- */
/* Case: tlb/asid                                                   */
/* ---------------------------------------------------------------- */

#define AS_FAULT_OFF        0x100
#define AS_SEEN_OFF         0x120
#define AS_WANT_OFF         0x140
#define AS_ITER_OFF         0x160

#define AS_VBASE            0x00500000
/* Per-CPU physical pair: A at 0x280000 + cpu*0x2000, B one page after it. */
#define AS_PAGE_A_PFN       0x280
#define AS_PAGE_A_KSEG1_HI  0xa028
#define AS_PATTERN1_HI      0x1111
#define AS_PATTERN2_HI      0x2222
#define AS_ASID1            1
#define AS_ASID2            2
#define AS_INDEX1           1
#define AS_INDEX2           2
#define AS_ITERATIONS       512

/*
 * One half of the loop: select an ASID, read this CPU's V, and require the
 * pattern belonging to that ASID's page.  Records the first mismatch with
 * what was read, what was wanted, and which iteration.
 */
static void emit_asid_check(GuestImage *img, unsigned int asid, int wantreg)
{
    unsigned int ok, fault_done;

    emit_ori(img, R_T3, R_S1, asid);
    emit_mtc0(img, R_T3, CP0_ENTRYHI, 0);
    emit_ehb(img);
    emit_ld(img, R_T7, R_S1, 0);
    ok = emit_beq_fwd(img, R_T7, wantreg);
    emit_nop(img);

    emit_ld(img, R_T3, R_T2, AS_FAULT_OFF);
    fault_done = emit_bne_fwd(img, R_T3, R_ZERO);
    emit_nop(img);
    emit_sd(img, R_T7, R_T2, AS_SEEN_OFF);
    emit_sd(img, wantreg, R_T2, AS_WANT_OFF);
    emit_sd(img, R_S0, R_T2, AS_ITER_OFF);
    emit_li(img, R_T3, FAULT_MAGIC);
    emit_sd(img, R_T3, R_T2, AS_FAULT_OFF);
    emit_sync(img);
    patch_fwd(img, fault_done);
    patch_fwd(img, ok);
}

static void build_tlb_asid(GuestImage *img)
{
    unsigned int loop;

    /* s1 = this CPU's virtual page; t3 keeps the per-CPU byte offset. */
    emit_li(img, R_S1, AS_VBASE);
    emit_sll(img, R_T3, R_K0, 13);
    emit_addu(img, R_S1, R_S1, R_T3);

    /* s2 = KSEG1 of page A, s3 = KSEG1 of page B, both this CPU's own. */
    emit_lui(img, R_S2, AS_PAGE_A_KSEG1_HI);
    emit_addu(img, R_S2, R_S2, R_T3);
    emit_addiu(img, R_S3, R_S2, 0x1000);

    /* s4 = pattern for ASID 1's page, s5 = pattern for ASID 2's page. */
    emit_lui(img, R_S4, AS_PATTERN1_HI);
    emit_addu(img, R_S4, R_S4, R_K0);
    emit_lui(img, R_S5, AS_PATTERN2_HI);
    emit_addu(img, R_S5, R_S5, R_K0);

    emit_sd(img, R_S4, R_S2, 0);
    emit_sd(img, R_S5, R_S3, 0);
    emit_sync(img);

    /*
     * Two non-global entries for the same virtual page at different indices.
     * PFN of A is AS_PAGE_A_PFN + 2*CPUNum, and B is the page after it, so
     * each CPU has its own pair and a stale read cannot come from a peer.
     */
    emit_addiu(img, R_T4, R_ZERO, AS_PAGE_A_PFN);
    emit_sll(img, R_T5, R_K0, 1);
    emit_addu(img, R_T4, R_T4, R_T5);
    emit_sll(img, R_T6, R_T4, 6);
    emit_ori(img, R_T6, R_T6, ENTRYLO_FLAGS_ASIDED);
    emit_ori(img, R_T3, R_S1, AS_ASID1);
    emit_tlb_write(img, R_T3, R_T6, AS_INDEX1, 0);

    emit_addiu(img, R_T4, R_ZERO, AS_PAGE_A_PFN + 1);
    emit_sll(img, R_T5, R_K0, 1);
    emit_addu(img, R_T4, R_T4, R_T5);
    emit_sll(img, R_T6, R_T4, 6);
    emit_ori(img, R_T6, R_T6, ENTRYLO_FLAGS_ASIDED);
    emit_ori(img, R_T3, R_S1, AS_ASID2);
    emit_tlb_write(img, R_T3, R_T6, AS_INDEX2, 0);

    emit_addiu(img, R_S0, R_ZERO, 0);
    loop = img->pos;
    emit_publish(img, R_S0, PROGRESS_OFF);
    emit_asid_check(img, AS_ASID1, R_S4);
    emit_asid_check(img, AS_ASID2, R_S5);
    emit_addiu(img, R_S0, R_S0, 1);
    emit_addiu(img, R_T3, R_ZERO, AS_ITERATIONS);
    emit_sltu(img, R_T4, R_S0, R_T3);
    emit_bne(img, R_T4, R_ZERO, loop);
    emit_nop(img);

    emit_done_and_halt(img);
}

/* ---------------------------------------------------------------- */
/* Case: smc/cross-cpu                                              */
/* ---------------------------------------------------------------- */

#define SMC_READY_OFF       0x100
#define SMC_ROUND_OFF       0x108
#define SMC_EXPECT_OFF      0x110
#define SMC_PREV_OFF        0x118
#define SMC_ACK_OFF         0x120
#define SMC_SEEN_OFF        0x128
#define SMC_STALE_OFF       0x130
#define SMC_FAULT_OFF       0x138
#define SMC_BADK_OFF        0x140
#define SMC_BADR_OFF        0x148

/*
 * Two patch sites in different physical pages, so alternating between them
 * exercises two translations rather than one.  Executed through KSEG0 and
 * patched through KSEG1: the patcher's store therefore goes straight to
 * memory with no dirty line to write back, and the executor is the one that
 * invalidates its instruction stream, which is the handshake the
 * architecture asks for.
 */
#define SMC_SITE0_KSEG0     0x80300000
#define SMC_SITE0_KSEG1     0xa0300000
#define SMC_SITE1_KSEG0     0x80302000
#define SMC_SITE1_KSEG1     0xa0302000

#define SMC_ROUNDS          64
#define SMC_K_BASE          0x1000

#define SMC_EXECUTOR        0
#define SMC_PATCHER         1

/*
 * The two instructions the site is built from, spelled with the same opcode
 * macros the emitters use so the site cannot drift from the encodings the
 * rest of the file agrees on.  The patched one is "ori v0, zero, K", of which
 * only the top half is fixed; K is OR-ed in at patch time.
 */
#define SMC_ORI_V0_HI       ((OPC_ORI | ((uint32_t)R_V0 << 16)) >> 16)
#define SMC_JR_RA           (OPC_JR | ((uint32_t)R_RA << 21))

/* Build "ori v0, zero, K" for the K held in kreg, leaving it in dst. */
static void emit_build_patch_word(GuestImage *img, int dst, int kreg)
{
    emit_lui(img, dst, SMC_ORI_V0_HI);
    emit_reg3(img, OPC_OR, dst, dst, kreg);
}

static void build_smc_cross_cpu(GuestImage *img)
{
    unsigned int not_patcher, not_executor, loop, site0, joined;
    unsigned int retry, got_current, not_stale, fault_done;

    /*
     * CPUs other than the executor and the patcher take no part.  They still
     * publish, so the host can confirm all four booted and neither of the two
     * that matter is silently standing in for another.
     */
    emit_addiu(img, R_T3, R_ZERO, SMC_PATCHER);
    not_patcher = emit_bne_fwd(img, R_K0, R_T3);
    emit_nop(img);

    /* ---- patcher ---- */

    /* Lay down both sites: ori v0, zero, K_BASE / jr ra / nop. */
    emit_li(img, R_S0, SMC_K_BASE);
    emit_build_patch_word(img, R_T4, R_S0);
    emit_li(img, R_T5, SMC_JR_RA);
    emit_li_addr(img, R_S2, SMC_SITE0_KSEG1);
    emit_sw(img, R_T4, R_S2, 0);
    emit_sw(img, R_T5, R_S2, 4);
    emit_sw(img, R_ZERO, R_S2, 8);
    emit_li_addr(img, R_S3, SMC_SITE1_KSEG1);
    emit_sw(img, R_T4, R_S3, 0);
    emit_sw(img, R_T5, R_S3, 4);
    emit_sw(img, R_ZERO, R_S3, 8);
    emit_sync(img);

    emit_li(img, R_T3, DONE_MAGIC);
    emit_sd(img, R_T3, R_T0, SMC_READY_OFF);
    emit_sync(img);

    /* s0 = round, s1 = previous K. */
    emit_li(img, R_S1, SMC_K_BASE);
    emit_addiu(img, R_S0, R_ZERO, 1);

    loop = img->pos;
    emit_publish(img, R_S0, PROGRESS_OFF);

    /* Alternate sites: odd rounds patch site 1, even rounds site 0. */
    emit_andi(img, R_T3, R_S0, 1);
    site0 = emit_beq_fwd(img, R_T3, R_ZERO);
    emit_nop(img);
    emit_reg3(img, OPC_OR, R_T6, R_S3, R_ZERO);
    joined = emit_b_fwd(img);
    emit_nop(img);
    patch_fwd(img, site0);
    emit_reg3(img, OPC_OR, R_T6, R_S2, R_ZERO);
    patch_fwd(img, joined);

    /* K for this round, then the instruction that materialises it. */
    emit_addiu(img, R_T7, R_ZERO, SMC_K_BASE);
    emit_addu(img, R_T7, R_T7, R_S0);
    emit_build_patch_word(img, R_T4, R_T7);
    emit_sw(img, R_T4, R_T6, 0);
    emit_sync(img);

    /* Announce what to expect before announcing that the round is open. */
    emit_sd(img, R_S1, R_T0, SMC_PREV_OFF);
    emit_sd(img, R_T7, R_T0, SMC_EXPECT_OFF);
    emit_sync(img);
    emit_sd(img, R_S0, R_T0, SMC_ROUND_OFF);
    emit_sync(img);

    emit_wait_ge(img, R_S0, SMC_ACK_OFF);

    emit_reg3(img, OPC_OR, R_S1, R_T7, R_ZERO);
    emit_addiu(img, R_S0, R_S0, 1);
    emit_addiu(img, R_T3, R_ZERO, SMC_ROUNDS);
    emit_sltu(img, R_T4, R_T3, R_S0);
    emit_beq(img, R_T4, R_ZERO, loop);
    emit_nop(img);
    emit_done_and_halt(img);

    patch_fwd(img, not_patcher);

    emit_addiu(img, R_T3, R_ZERO, SMC_EXECUTOR);
    not_executor = emit_bne_fwd(img, R_K0, R_T3);
    emit_nop(img);

    /* ---- executor ---- */

    emit_li(img, R_T3, DONE_MAGIC);
    emit_wait_ge(img, R_T3, SMC_READY_OFF);
    emit_sync(img);

    emit_li_addr(img, R_S2, SMC_SITE0_KSEG0);
    emit_li_addr(img, R_S3, SMC_SITE1_KSEG0);
    emit_addiu(img, R_S0, R_ZERO, 1);

    loop = img->pos;
    emit_publish(img, R_S0, PROGRESS_OFF);
    emit_wait_ge(img, R_S0, SMC_ROUND_OFF);
    emit_sync(img);

    emit_ld(img, R_S4, R_T0, SMC_EXPECT_OFF);
    emit_ld(img, R_S5, R_T0, SMC_PREV_OFF);

    emit_andi(img, R_T3, R_S0, 1);
    site0 = emit_beq_fwd(img, R_T3, R_ZERO);
    emit_nop(img);
    emit_reg3(img, OPC_OR, R_S1, R_S3, R_ZERO);
    joined = emit_b_fwd(img);
    emit_nop(img);
    patch_fwd(img, site0);
    emit_reg3(img, OPC_OR, R_S1, R_S2, R_ZERO);
    patch_fwd(img, joined);

    /*
     * synci writes back and invalidates the line the patch landed in, and the
     * hazard-clearing jump keeps the following fetch from being satisfied out
     * of anything synci was meant to discard.
     *
     * Neither does what its name says here, and that is worth knowing rather
     * than assuming: QEMU models no cache, and its JR decoding passes hint 0
     * whatever the encoding holds, so jr.hb runs as a plain jump.  What makes
     * cross-CPU patching land under TCG is elsewhere — the patcher's store
     * invalidates the translated block, and synci ends this one.  The
     * handshake is written out anyway because a guest that leaves it out is
     * riding undefined behaviour on hardware, and this case would then be
     * asserting something about QEMU that no real machine promises.
     */
    retry = img->pos;
    emit_synci(img, R_S1, 0);
    emit_lui(img, R_T9, 0xbfc0);
    /* Filled in below once the address after the hazard barrier is known. */
    {
        unsigned int ori_at = img->pos;
        unsigned int after;

        emit_ori(img, R_T9, R_T9, 0);
        emit_jr_hb(img, R_T9);
        emit_nop(img);
        after = img->pos;
        stl_be_p(img->code + ori_at,
                 (ldl_be_p(img->code + ori_at) & 0xffff0000u) | after);
    }

    emit_jalr(img, R_RA, R_S1);
    emit_nop(img);

    /* v0 now holds whatever the patched instruction materialised. */
    got_current = emit_beq_fwd(img, R_V0, R_S4);
    emit_nop(img);
    not_stale = emit_bne_fwd(img, R_V0, R_S5);
    emit_nop(img);

    /* The previous round's value is tolerated; count it and look again. */
    emit_ld(img, R_T3, R_T0, SMC_STALE_OFF);
    emit_daddiu(img, R_T3, R_T3, 1);
    emit_sd(img, R_T3, R_T0, SMC_STALE_OFF);
    emit_sync(img);
    emit_b(img, retry);
    emit_nop(img);

    /* Anything else is a torn or invented instruction. */
    patch_fwd(img, not_stale);
    emit_ld(img, R_T3, R_T0, SMC_FAULT_OFF);
    fault_done = emit_bne_fwd(img, R_T3, R_ZERO);
    emit_nop(img);
    emit_sd(img, R_V0, R_T0, SMC_BADK_OFF);
    emit_sd(img, R_S0, R_T0, SMC_BADR_OFF);
    emit_li(img, R_T3, FAULT_MAGIC);
    emit_sd(img, R_T3, R_T0, SMC_FAULT_OFF);
    emit_sync(img);
    patch_fwd(img, fault_done);
    /*
     * Release the patcher before stopping.  Without this it waits on an
     * acknowledgement that is never coming, never reaches DONE, and the host
     * reports a deadline it could have reported as the fault it actually is.
     */
    emit_addiu(img, R_T3, R_ZERO, SMC_ROUNDS);
    emit_sd(img, R_T3, R_T0, SMC_ACK_OFF);
    emit_sync(img);
    emit_done_and_halt(img);

    patch_fwd(img, got_current);
    emit_sd(img, R_V0, R_T0, SMC_SEEN_OFF);
    emit_sync(img);
    emit_sd(img, R_S0, R_T0, SMC_ACK_OFF);
    emit_sync(img);

    emit_addiu(img, R_S0, R_S0, 1);
    emit_addiu(img, R_T3, R_ZERO, SMC_ROUNDS);
    emit_sltu(img, R_T4, R_T3, R_S0);
    emit_beq(img, R_T4, R_ZERO, loop);
    emit_nop(img);
    emit_done_and_halt(img);

    patch_fwd(img, not_executor);
    emit_done_and_halt(img);
}

/* ---------------------------------------------------------------- */
/* Host side                                                        */
/* ---------------------------------------------------------------- */

static uint64_t state_word(QTestState *qts, unsigned int off)
{
    return qtest_readq(qts, STATE_PHYS + off);
}

static void report_common(QTestState *qts)
{
    unsigned int i;

    for (i = 0; i < NR_CPUS; i++) {
        g_test_message("cpu %u: alive 0x%" PRIx64 " progress %" PRIu64
                       " done 0x%" PRIx64 " exc 0x%" PRIx64
                       " cause 0x%" PRIx64 " badvaddr 0x%" PRIx64
                       " epc 0x%" PRIx64, i,
                       state_word(qts, ALIVE_OFF + 8 * i),
                       state_word(qts, PROGRESS_OFF + 8 * i),
                       state_word(qts, DONE_OFF + 8 * i),
                       state_word(qts, EXC_MARK_OFF + 8 * i),
                       state_word(qts, EXC_CAUSE_OFF + 8 * i),
                       state_word(qts, EXC_VADDR_OFF + 8 * i),
                       state_word(qts, EXC_EPC_OFF + 8 * i));
    }
}

static bool all_cpus_done(QTestState *qts)
{
    unsigned int i;

    for (i = 0; i < NR_CPUS; i++) {
        if (state_word(qts, DONE_OFF + 8 * i) != DONE_MAGIC) {
            return false;
        }
    }
    return true;
}

/*
 * The accelerator named here wins over the -accel qtest libqtest appends,
 * because QEMU takes the first one on the command line that initialises.
 * An explicit R2 CPU is required: EBase is MIPS64R2 and malta's default 20Kc
 * is R1.  5KEc has no coherency manager, so malta builds plain CPUs rather
 * than a CPS cluster and all four run from reset.
 */
static QTestState *boot_case(const char *accel, void (*build)(GuestImage *img))
{
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("mips64-mttcg-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    write_guest_image(fd, build);
    close(fd);

    qts = qtest_initf("-M malta -m 128M -smp %d -cpu 5KEc -bios %s "
                      "-accel %s -no-reboot -no-shutdown",
                      NR_CPUS, image, accel);
    unlink(image);
    return qts;
}

/* Poll until every CPU is done.  Returns false if the deadline expired. */
static bool wait_for_done(QTestState *qts)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (all_cpus_done(qts)) {
            return true;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    return false;
}

/* Every CPU booted, none of them faulted, all of them finished. */
static void assert_common(QTestState *qts)
{
    unsigned int i;

    for (i = 0; i < NR_CPUS; i++) {
        g_assert_cmphex(state_word(qts, ALIVE_OFF + 8 * i), ==, ALIVE_MAGIC);
        g_assert_cmphex(state_word(qts, EXC_MARK_OFF + 8 * i), ==, 0);
        g_assert_cmphex(state_word(qts, DONE_OFF + 8 * i), ==, DONE_MAGIC);
    }
}

static bool rendezvous_state_ok(QTestState *qts)
{
    unsigned int i;

    for (i = 0; i < NR_CPUS; i++) {
        if (state_word(qts, ALIVE_OFF + 8 * i) != ALIVE_MAGIC ||
            state_word(qts, EXC_MARK_OFF + 8 * i) != 0 ||
            state_word(qts, RV_SLOT_OFF + 8 * i) != i + 1) {
            return false;
        }
    }
    return state_word(qts, RV_COUNTER_OFF) ==
           (uint64_t)NR_CPUS * rv_increments;
}

static void run_rendezvous(const char *accel, unsigned int increments)
{
    QTestState *qts;
    unsigned int i;
    bool done;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    rv_increments = increments;
    qts = boot_case(accel, build_rendezvous);
    done = wait_for_done(qts);

    if (!done || !rendezvous_state_ok(qts)) {
        report_common(qts);
        for (i = 0; i < NR_CPUS; i++) {
            g_test_message("cpu %u: slot %" PRIu64, i,
                           state_word(qts, RV_SLOT_OFF + 8 * i));
        }
        g_test_message("shared counter %" PRIu64,
                       state_word(qts, RV_COUNTER_OFF));
    }

    g_assert_true(done);
    assert_common(qts);
    for (i = 0; i < NR_CPUS; i++) {
        /* Slot i is claimed by CPUNum i, so this is not a permutation claim. */
        g_assert_cmpuint(state_word(qts, RV_SLOT_OFF + 8 * i), ==, i + 1);
    }
    g_assert_cmpuint(state_word(qts, RV_COUNTER_OFF), ==,
                     (uint64_t)NR_CPUS * increments);

    qtest_quit(qts);
}

static void test_rendezvous_thread_multi(const void *data)
{
    run_rendezvous("tcg,thread=multi", 10000);
}

static void test_rendezvous_round_robin(const void *data)
{
    /*
     * The same program under the default single-threaded TCG, which
     * interleaves the CPUs instead of running them at once.  Fewer
     * increments, because this one is a deterministic baseline showing the
     * program is scheduler-agnostic rather than a contention test.
     */
    run_rendezvous("tcg", 500);
}

static void test_tlb_shootdown(const void *data)
{
    QTestState *qts;
    unsigned int i;
    bool done, faulted;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    qts = boot_case("tcg,thread=multi", build_tlb_shootdown);
    done = wait_for_done(qts);

    faulted = false;
    for (i = 0; i < NR_CPUS; i++) {
        faulted |= state_word(qts, SD_FAULT_OFF + 8 * i) != 0;
    }
    if (!done || faulted) {
        report_common(qts);
        g_test_message("mailbox %" PRIu64, state_word(qts, SD_MAILBOX_OFF));
        for (i = 0; i < NR_CPUS; i++) {
            g_test_message("cpu %u: ack %" PRIu64 " fault 0x%" PRIx64
                           " read %" PRIu64 " in generation %" PRIu64, i,
                           state_word(qts, SD_ACK_OFF + 8 * i),
                           state_word(qts, SD_FAULT_OFF + 8 * i),
                           state_word(qts, SD_SEEN_OFF + 8 * i),
                           state_word(qts, SD_FGEN_OFF + 8 * i));
        }
    }

    g_assert_true(done);
    assert_common(qts);
    for (i = 0; i < NR_CPUS; i++) {
        g_assert_cmphex(state_word(qts, SD_FAULT_OFF + 8 * i), ==, 0);
        g_assert_cmpuint(state_word(qts, SD_ACK_OFF + 8 * i), ==,
                         SD_GENERATIONS);
    }

    qtest_quit(qts);
}

static void test_tlb_asid(const void *data)
{
    QTestState *qts;
    unsigned int i;
    bool done, faulted;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    qts = boot_case("tcg,thread=multi", build_tlb_asid);
    done = wait_for_done(qts);

    faulted = false;
    for (i = 0; i < NR_CPUS; i++) {
        faulted |= state_word(qts, AS_FAULT_OFF + 8 * i) != 0;
    }
    if (!done || faulted) {
        report_common(qts);
        for (i = 0; i < NR_CPUS; i++) {
            g_test_message("cpu %u: fault 0x%" PRIx64 " read 0x%" PRIx64
                           " wanting 0x%" PRIx64 " on iteration %" PRIu64, i,
                           state_word(qts, AS_FAULT_OFF + 8 * i),
                           state_word(qts, AS_SEEN_OFF + 8 * i),
                           state_word(qts, AS_WANT_OFF + 8 * i),
                           state_word(qts, AS_ITER_OFF + 8 * i));
        }
    }

    g_assert_true(done);
    assert_common(qts);
    for (i = 0; i < NR_CPUS; i++) {
        g_assert_cmphex(state_word(qts, AS_FAULT_OFF + 8 * i), ==, 0);
    }

    qtest_quit(qts);
}

static void test_smc_cross_cpu(const void *data)
{
    QTestState *qts;
    bool done;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    qts = boot_case("tcg,thread=multi", build_smc_cross_cpu);
    done = wait_for_done(qts);

    if (!done || state_word(qts, SMC_FAULT_OFF) != 0) {
        report_common(qts);
        g_test_message("round %" PRIu64 " ack %" PRIu64 " expect %" PRIu64
                       " seen %" PRIu64 " stale %" PRIu64,
                       state_word(qts, SMC_ROUND_OFF),
                       state_word(qts, SMC_ACK_OFF),
                       state_word(qts, SMC_EXPECT_OFF),
                       state_word(qts, SMC_SEEN_OFF),
                       state_word(qts, SMC_STALE_OFF));
        g_test_message("executor read 0x%" PRIx64 " in round %" PRIu64,
                       state_word(qts, SMC_BADK_OFF),
                       state_word(qts, SMC_BADR_OFF));
    }

    g_assert_true(done);
    assert_common(qts);
    /* Never a value outside {previous, current}: no torn instruction. */
    g_assert_cmphex(state_word(qts, SMC_FAULT_OFF), ==, 0);
    /* Every round's patch arrived, in its own round. */
    g_assert_cmpuint(state_word(qts, SMC_ACK_OFF), ==, SMC_ROUNDS);
    g_assert_cmpuint(state_word(qts, SMC_SEEN_OFF), ==,
                     SMC_K_BASE + SMC_ROUNDS);

    if (state_word(qts, SMC_STALE_OFF) != 0) {
        g_test_message("executor saw the previous round's value %" PRIu64
                       " times", state_word(qts, SMC_STALE_OFF));
    }

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("mttcg/rendezvous/thread-multi", NULL,
                        test_rendezvous_thread_multi);
    qtest_add_data_func("mttcg/rendezvous/round-robin", NULL,
                        test_rendezvous_round_robin);
    qtest_add_data_func("mttcg/tlb/shootdown", NULL, test_tlb_shootdown);
    qtest_add_data_func("mttcg/tlb/asid", NULL, test_tlb_asid);
    qtest_add_data_func("mttcg/smc/cross-cpu", NULL, test_smc_cross_cpu);
    return g_test_run();
}
