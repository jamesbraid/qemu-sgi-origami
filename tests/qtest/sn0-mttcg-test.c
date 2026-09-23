/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MIPS CPUs exercise the accelerator core on a machine that contains none of
 * this fork's devices.
 *
 * These exercise target/mips and accel/tcg on the R10000, the only CPU this
 * fork builds a machine for.  Each case runs on the Origin 200 with the
 * synthesized firmware off, so the -smp CPUs come out of reset free-running
 * at the boot vector with the guest program in sole possession of the reset
 * vector and low memory.  A failure here is evidence about the CPU and the
 * accelerator, not about the rest of the SN0 machine model.
 *
 * Each case generates its own MIPS64 program and hands it to QEMU as a raw
 * -bios image loaded at the reset vector.  Multi-CPU cases get each CPU's
 * identity from the Hub-local processor number the PI register reports, not
 * from a primitive under test, or a broken primitive makes the failure
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
 *   tlb/shootdown     A vCPU's own tlbwi remap must be coherent with data a
 *                     peer just wrote.  Underneath: r4k_invalidate_tlb's
 *                     tlb_flush_page on the remapping vCPU.
 *   tlb/asid          ASID-tagged translation must follow EntryHi.ASID swaps.
 *                     Underneath: the flush on ASID change in the EntryHi
 *                     write path.
 *   tlb/random-global A global random write must discard an older non-global
 *                     shadow for the same virtual page.
 *   interrupt/masked-line-stays-off-the-lock
 *                     A pending line the guest has masked must leave the
 *                     execution loop lock-free.  Underneath: the flag drop in
 *                     mips_cpu_exec_interrupt() and the re-assert every
 *                     own-CPU unmask makes.
 *
 * LL/SC atomicity and cross-thread visibility used to be checked here too,
 * with two CPUs rendezvousing then contending for one word with lld/scd.
 * tests/functional/mips64/test_sn0_smp.py's
 * test_four_cpu_two_node_rendezvous_thread_multi asserts the same claim on
 * a real machine, so the duplicate qtest was dropped.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-mips-emit.h"
#include "sgi-test.h"

#define NR_CPUS             2

/*
 * State words live in low RAM, well clear of the flash the image is loaded
 * into, and are reached through KSEG1 so the guest never depends on cache
 * behaviour.  QEMU hands out zeroed RAM, which is what makes "still zero"
 * mean "not yet published" for the spins and starts counters at zero.
 *
 * Clear of physical 1 MiB as well, where this machine overlays a 32 KiB
 * per-processor filter on RAM for the firmware's private data.  A state
 * word there is an I/O dispatch rather than a RAM access, so every read
 * and write of it takes the big lock -- which the cases below count.
 */
#define STATE_PHYS          0x00180000
#define STATE_KSEG1_HI      0xa018

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
 * The entry branch jumps clear of the low words and the boot-exception
 * vectors below, so nothing a board might seed there matters.
 */
#define VEC_TLB_REFILL      0x200
#define VEC_XTLB_REFILL     0x280
#define VEC_CACHE_ERROR     0x300
#define VEC_GENERAL         0x380

#define POLL_TRIES          30000
#define POLL_INTERVAL_US    1000

/* Instruction encodings, named as in target/mips/tcg/translate.c. */
#define OPC_SPECIAL         (0x00u << 26)
#define OPC_BEQ             (0x04u << 26)
#define OPC_BNE             (0x05u << 26)
#define OPC_ANDI            (0x0cu << 26)
#define OPC_ORI             (0x0du << 26)
#define OPC_LW              (0x23u << 26)
#define OPC_SW              (0x2bu << 26)
#define OPC_SLL             (0x00u | OPC_SPECIAL)
#define OPC_SYNC            (0x0fu | OPC_SPECIAL)
#define OPC_ADDU            (0x21u | OPC_SPECIAL)
#define OPC_OR              (0x25u | OPC_SPECIAL)
#define OPC_SLTU            (0x2bu | OPC_SPECIAL)
#define OPC_CP0             (0x10u << 26)
#define OPC_MFC0            (OPC_CP0 | (0x00u << 21))
#define OPC_MTC0            (OPC_CP0 | (0x04u << 21))
#define OPC_C0              (OPC_CP0 | (0x10u << 21))
#define OPC_TLBWI           (OPC_C0 | 0x02u)
#define OPC_TLBWR           (OPC_C0 | 0x06u)

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
#define CP0_WIRED           6
#define CP0_BADVADDR        8
#define CP0_COUNT           9
#define CP0_ENTRYHI         10
#define CP0_COMPARE         11
#define CP0_STATUS          12
#define CP0_CAUSE           13
#define CP0_EPC             14

/*
 * Status.ERL is bit 2.  Clearing it puts the low mapped segment under the TLB
 * instead of leaving it unmapped, which is what reset hands us.
 */
#define STATUS_ERL_CLEAR    (-5)

/* Status.KX (bit 7) enables the 64-bit kernel segments the Hub read below uses. */
#define STATUS_KX           0x80u

/*
 * The Hub-local processor number, read uncached through XKPHYS.  Bit 0 is 0
 * for the node's CPU A and 1 for CPU B; with two CPUs on one node that is the
 * per-CPU identity the multi-CPU cases hold.
 */
#define SN0_PI_CPU_NUM      0x9200000001000020ULL

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
    emit(img, mips_lui(rt, imm));
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
    emit(img, mips_addiu(rt, rs, imm));
}

static void emit_ld(GuestImage *img, int rt, int base, uint16_t off)
{
    emit(img, mips_ld(rt, base, off));
}

static void emit_sd(GuestImage *img, int rt, int base, uint16_t off)
{
    emit(img, mips_sd(rt, base, off));
}

static void emit_sw(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_immediate(img, OPC_SW, base, rt, off);
}

static void emit_lw(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_immediate(img, OPC_LW, base, rt, off);
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
    emit(img, mips_and(rd, rs, rt));
}

static void emit_sltu(GuestImage *img, int rd, int rs, int rt)
{
    emit_reg3(img, OPC_SLTU, rd, rs, rt);
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

static void emit_tlbwr(GuestImage *img)
{
    emit(img, OPC_TLBWR);
}

static void emit_sync(GuestImage *img)
{
    emit(img, OPC_SYNC);
}

static void emit_nop(GuestImage *img)
{
    emit(img, mips_nop());
}

/* EHB is SLL with rd = rt = 0 and sa = 3: an execution hazard barrier. */
static void emit_ehb(GuestImage *img)
{
    emit_sll(img, R_ZERO, R_ZERO, 3);
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

/* DSLL: shift a doubleword left by sa (0-31), SPECIAL function 0x38. */
static void emit_dsll(GuestImage *img, int rd, int rt, int sa)
{
    emit(img, OPC_SPECIAL | (rt << 16) | (rd << 11) | (sa << 6) | 0x38u);
}

/* Load a full 64-bit constant, one 16-bit chunk at a time from the top. */
static void emit_li64(GuestImage *img, int rt, uint64_t value)
{
    emit_ori(img, rt, R_ZERO, (uint16_t)(value >> 48));
    for (int shift = 32; ; shift -= 16) {
        emit_dsll(img, rt, rt, 16);
        emit_ori(img, rt, rt, (uint16_t)(value >> shift));
        if (shift == 0) {
            break;
        }
    }
}

/* Read this processor's Hub-local number into dst; clobbers scratch. */
static void emit_cpu_number(GuestImage *img, int dst, int scratch)
{
    emit_li64(img, scratch, SN0_PI_CPU_NUM);
    emit_ld(img, dst, scratch, 0);
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

static void emit_spin(GuestImage *img)
{
    unsigned int spin = img->pos;

    emit_b(img, spin);
    emit_nop(img);
}

static void emit_done_and_halt(GuestImage *img)
{
    emit_li(img, R_T3, DONE_MAGIC);
    emit_publish(img, R_T3, DONE_OFF);
    emit_spin(img);
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
    emit_cpu_number(img, R_K1, R_K0);
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
    emit_spin(img);
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
    /* Clear ERL so the low segment is mapped; set KX so the Hub read works. */
    emit_mfc0(img, R_T3, CP0_STATUS, 0);
    emit_addiu(img, R_T4, R_ZERO, STATUS_ERL_CLEAR);
    emit_and(img, R_T3, R_T3, R_T4);
    emit_ori(img, R_T3, R_T3, STATUS_KX);
    emit_mtc0(img, R_T3, CP0_STATUS, 0);
    emit_ehb(img);

    emit_cpu_number(img, R_K0, R_T4);
    emit_lui(img, R_T0, STATE_KSEG1_HI);
    emit_sll(img, R_T1, R_K0, 3);
    emit_addu(img, R_T2, R_T0, R_T1);

    emit_mtc0(img, R_ZERO, CP0_PAGEMASK, 0);
    emit_ehb(img);

    emit_li(img, R_T3, ALIVE_MAGIC);
    emit_publish(img, R_T3, ALIVE_OFF);
}

static void emit_r10k_exception_handler(GuestImage *img)
{
    emit_lui(img, R_K1, STATE_KSEG1_HI);
    emit_mfc0(img, R_K0, CP0_CAUSE, 0);
    emit_sd(img, R_K0, R_K1, EXC_CAUSE_OFF);
    emit_mfc0(img, R_K0, CP0_BADVADDR, 0);
    emit_sd(img, R_K0, R_K1, EXC_VADDR_OFF);
    emit_mfc0(img, R_K0, CP0_EPC, 0);
    emit_sd(img, R_K0, R_K1, EXC_EPC_OFF);
    emit_li(img, R_K0, EXC_MAGIC);
    emit_sd(img, R_K0, R_K1, EXC_MARK_OFF);
    emit_sync(img);
    emit_spin(img);
}

static void emit_r10k_preamble(GuestImage *img)
{
    /* R10000 predates EBase; this image runs on one CPU and needs no ID. */
    emit_addiu(img, R_K0, R_ZERO, 0);
    emit_lui(img, R_T0, STATE_KSEG1_HI);
    emit_reg3(img, OPC_OR, R_T2, R_T0, R_ZERO);

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

/* Write one TLB entry through Random, with the same register contract. */
static void emit_tlb_random_write(GuestImage *img, int vreg, int loreg,
                                  unsigned int odd_half)
{
    g_assert_cmpint(vreg, !=, R_T8);
    g_assert_cmpint(loreg, !=, R_T8);

    emit_mtc0(img, vreg, CP0_ENTRYHI, 0);
    emit_ehb(img);
    emit_mtc0(img, loreg, CP0_ENTRYLO0, 0);
    emit_li(img, R_T8, odd_half);
    emit_mtc0(img, R_T8, CP0_ENTRYLO1, 0);
    emit_ehb(img);
    emit_tlbwr(img);
    emit_ehb(img);
}

static void write_guest_image_with_setup(int fd,
                                         void (*exception)(GuestImage *img),
                                         void (*preamble)(GuestImage *img),
                                         void (*build)(GuestImage *img))
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
    exception(&img);
    g_assert_cmpuint(img.pos, <=, CODE_START);

    img.pos = CODE_START;
    preamble(&img);
    build(&img);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static void write_guest_image(int fd, void (*build)(GuestImage *img))
{
    write_guest_image_with_setup(fd, emit_exception_handler, emit_preamble,
                                 build);
}

static void write_r10k_guest_image(int fd, void (*build)(GuestImage *img))
{
    write_guest_image_with_setup(fd, emit_r10k_exception_handler,
                                 emit_r10k_preamble, build);
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
/* Case: tlb/random-global                                          */
/* ---------------------------------------------------------------- */

#define RG_FAULT_OFF        0x100
#define RG_SEEN_OFF         0x120
#define RG_INITIAL_OFF      0x140

#define RG_VBASE            0x00600000
#define RG_OTHER_VBASE      0x00800000
#define RG_FINAL_VBASE      0x00a00000
#define RG_OLD_PFN          0x300
#define RG_NEW_PFN          0x302
#define RG_OTHER_PFN        0x304
#define RG_FINAL_PFN        0x306
#define RG_OLD_KSEG1        0xa0300000
#define RG_NEW_KSEG1        0xa0302000
#define RG_OLD_PATTERN      0x1111
#define RG_NEW_PATTERN      0x2222
#define RG_OLD_ASID         1
#define RG_NEW_ASID         2
#define RG_RANDOM_INDEX     63
#define RG_ENTRYLO_ASIDED(pfn)  (((pfn) << 6) | (3 << 3) | 0x6)
#define RG_ENTRYLO_GLOBAL(pfn)  (((pfn) << 6) | (3 << 3) | 0x7)

static void build_tlb_random_global(GuestImage *img)
{
    unsigned int correct;

    emit_li_addr(img, R_S2, RG_OLD_KSEG1);
    emit_li_addr(img, R_S3, RG_NEW_KSEG1);
    emit_li(img, R_S4, RG_OLD_PATTERN);
    emit_li(img, R_S5, RG_NEW_PATTERN);
    emit_sw(img, R_S4, R_S2, 0);
    emit_sw(img, R_S5, R_S3, 0);
    emit_sync(img);

    /* Restrict Random to one slot so each write displaces its predecessor. */
    emit_addiu(img, R_T3, R_ZERO, RG_RANDOM_INDEX);
    emit_mtc0(img, R_T3, CP0_WIRED, 0);
    emit_ehb(img);

    /* Cache an ASID 1 translation, then displace it into a shadow slot. */
    emit_li(img, R_S1, RG_VBASE);
    emit_li(img, R_S0, RG_ENTRYLO_ASIDED(RG_OLD_PFN));
    emit_ori(img, R_T3, R_S1, RG_OLD_ASID);
    emit_tlb_random_write(img, R_T3, R_S0,
                          RG_ENTRYLO_ASIDED(RG_OLD_PFN + 1));
    emit_lw(img, R_T7, R_S1, 0);
    emit_sd(img, R_T7, R_T2, RG_INITIAL_OFF);

    emit_li(img, R_T3, RG_OTHER_VBASE | RG_OLD_ASID);
    emit_li(img, R_T4, RG_ENTRYLO_ASIDED(RG_OTHER_PFN));
    emit_tlb_random_write(img, R_T3, R_T4,
                          RG_ENTRYLO_ASIDED(RG_OTHER_PFN + 1));

    /* A global entry for the same page overlaps the shadow for every ASID. */
    emit_li(img, R_T3, RG_VBASE | RG_NEW_ASID);
    emit_li(img, R_T4, RG_ENTRYLO_GLOBAL(RG_NEW_PFN));
    emit_tlb_random_write(img, R_T3, R_T4,
                          RG_ENTRYLO_GLOBAL(RG_NEW_PFN + 1));

    /*
     * Displace the global entry before reading through ASID 1.  A surviving
     * older shadow then precedes it in QEMU's lookup order and reappears as
     * the stale translation this test catches.
     */
    emit_li(img, R_T3, RG_FINAL_VBASE | RG_NEW_ASID);
    emit_li(img, R_T4, RG_ENTRYLO_ASIDED(RG_FINAL_PFN));
    emit_tlb_random_write(img, R_T3, R_T4,
                          RG_ENTRYLO_ASIDED(RG_FINAL_PFN + 1));

    emit_li(img, R_T3, RG_VBASE | RG_OLD_ASID);
    emit_mtc0(img, R_T3, CP0_ENTRYHI, 0);
    emit_ehb(img);
    emit_lw(img, R_T7, R_S1, 0);
    emit_sd(img, R_T7, R_T2, RG_SEEN_OFF);
    correct = emit_beq_fwd(img, R_T7, R_S5);
    emit_nop(img);
    emit_li(img, R_T3, FAULT_MAGIC);
    emit_sd(img, R_T3, R_T2, RG_FAULT_OFF);
    emit_sync(img);
    patch_fwd(img, correct);

    emit_li(img, R_T3, DONE_MAGIC);
    emit_publish(img, R_T3, DONE_OFF);
    emit_spin(img);
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
 */
static QTestState *boot_cpu_case(const char *accel, const char *cpu,
                                 int nr_cpus,
                                 void (*writer)(int fd,
                                                void (*build)(GuestImage *img)),
                                 void (*build)(GuestImage *img))
{
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn0-mttcg-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    writer(fd, build);
    close(fd);

    qts = qtest_initf("-M origin200 -m 128M "
                      "-smp %d -cpu %s -bios %s "
                      "-accel %s -no-reboot -no-shutdown",
                      nr_cpus, cpu, image, accel);
    unlink(image);
    return qts;
}

static QTestState *boot_case(const char *accel, void (*build)(GuestImage *img))
{
    return boot_cpu_case(accel, "R10000", NR_CPUS, write_guest_image, build);
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

static bool wait_for_cpu0_done(QTestState *qts)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (state_word(qts, DONE_OFF) == DONE_MAGIC) {
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

/*
 * A pending interrupt the guest has masked, and the cost of carrying it.
 *
 * Status bits 15:8 are the interrupt mask, bit 0 is IE, and bits 4:1 are KSU,
 * ERL and EXL; the guest below clears all of those and sets IE alone, which
 * is a kernel running with every line masked.  The count/compare timer is the
 * line it leaves standing: Compare is zero out of reset and the counter
 * reaches it every wrap, so an IRIX whose clock comes from the Hub carries
 * IP7 from its first wrap to shutdown without ever taking or lowering it.
 *
 * What is pinned: carrying that line must not put the execution loop on the
 * big lock.  The loop is lock-free while cpu->interrupt_request is zero and
 * acquires on every pass while it is not, so a model that leaves the flag up
 * under a masked line pays an acquire for each pass -- and eight processors
 * doing that do not boot.  Each pass here is an mtc0 to Status, which ends
 * its translation block, so the loop is re-entered once per iteration and the
 * acquisitions are countable.
 *
 * False pass to watch for: a phase that never re-enters the loop counts
 * nothing whatever the model does.  The first phase runs the identical loop
 * with the line parked ahead, so the two counts differ only by the standing
 * line, and the case fails if the second phase does not see IP7 in Cause.
 */
#define MI_GO_OFF           0x100
#define MI_CAUSE_OFF        0x120
#define MI_STATUS_OFF       0x140

/*
 * Passes each phase makes. Large enough that the harness's own polling of
 * the state words, which also takes the lock, stays in the noise.
 */
#define MI_PASSES           100000u

#define STATUS_IM7          0x8000u
#define CAUSE_IP7           0x8000u
/* Cause bits 6:2: zero is an interrupt rather than a fault. */
#define CAUSE_EXCCODE       0x7cu
/* Status bits the guest drives itself: the mask, KSU, ERL, EXL and IE. */
#define STATUS_KEEP_HI      0xffffu
#define STATUS_KEEP_LO      0x00e0u

/* A quarter of the counter's range: far enough ahead to stay unreached. */
#define MI_COMPARE_AHEAD    0x4000u

static void emit_status_passes(GuestImage *img, int status, unsigned int n)
{
    unsigned int loop;

    emit_li(img, R_T7, n);
    loop = img->pos;
    emit_mtc0(img, status, CP0_STATUS, 0);
    emit_addiu(img, R_T7, R_T7, -1);
    emit_bne(img, R_T7, R_ZERO, loop);
    emit_nop(img);
}

static void build_masked_interrupt(GuestImage *img)
{
    unsigned int spin;

    /* s0: every line masked, IE set, EXL and ERL clear. */
    emit_mfc0(img, R_T3, CP0_STATUS, 0);
    emit_lui(img, R_T4, STATUS_KEEP_HI);
    emit_ori(img, R_T4, R_T4, STATUS_KEEP_LO);
    emit_and(img, R_S0, R_T3, R_T4);
    emit_ori(img, R_S0, R_S0, 1);
    emit_mtc0(img, R_S0, CP0_STATUS, 0);
    emit_ehb(img);
    emit_publish(img, R_S0, MI_STATUS_OFF);

    /* Park the timer's compare ahead, so the first phase carries no line. */
    emit_mfc0(img, R_T5, CP0_COUNT, 0);
    emit_lui(img, R_T6, MI_COMPARE_AHEAD);
    emit_addu(img, R_T5, R_T5, R_T6);
    emit_mtc0(img, R_T5, CP0_COMPARE, 0);
    emit_ehb(img);

    emit_li(img, R_T3, 1);
    emit_publish(img, R_T3, PROGRESS_OFF);
    emit_li(img, R_T3, 1);
    emit_wait_ge(img, R_T3, MI_GO_OFF);

    emit_status_passes(img, R_S0, MI_PASSES);

    emit_li(img, R_T3, 2);
    emit_publish(img, R_T3, PROGRESS_OFF);
    emit_li(img, R_T3, 2);
    emit_wait_ge(img, R_T3, MI_GO_OFF);

    /* Compare at the counter matches at once, and nothing lowers IP7. */
    emit_mfc0(img, R_T5, CP0_COUNT, 0);
    emit_mtc0(img, R_T5, CP0_COMPARE, 0);
    emit_ehb(img);
    spin = img->pos;
    emit_mfc0(img, R_T6, CP0_CAUSE, 0);
    emit_andi(img, R_T7, R_T6, CAUSE_IP7);
    emit_beq(img, R_T7, R_ZERO, spin);
    emit_nop(img);
    emit_publish(img, R_T6, MI_CAUSE_OFF);

    emit_status_passes(img, R_S0, MI_PASSES);

    emit_li(img, R_T3, DONE_MAGIC);
    emit_publish(img, R_T3, DONE_OFF);

    /* Unmasking must deliver it here, not at the next edge on the line. */
    emit_ori(img, R_S0, R_S0, STATUS_IM7);
    emit_mtc0(img, R_S0, CP0_STATUS, 0);
    emit_ehb(img);
    emit_spin(img);
}

/* Big-lock acquisitions the profiler has counted since it was last reset. */
static uint64_t bql_acquisitions(QTestState *qts)
{
    g_autofree char *report = qtest_hmp(qts, "info sync-profile 200");
    g_auto(GStrv) lines = g_strsplit(report, "\n", -1);
    uint64_t total = 0;

    for (unsigned int i = 0; lines[i]; i++) {
        g_auto(GStrv) split = g_strsplit_set(lines[i], " \t", -1);
        const char *field[8];
        unsigned int n = 0;

        for (unsigned int j = 0; split[j] && n < G_N_ELEMENTS(field); j++) {
            if (split[j][0]) {
                field[n++] = split[j];
            }
        }
        /* "BQL mutex  <object>  <call site>  <wait>  <count>  <mean>" */
        if (n == 7 && g_str_equal(field[0], "BQL") &&
            g_str_equal(field[1], "mutex")) {
            total += g_ascii_strtoull(field[5], NULL, 10);
        }
    }
    return total;
}

static bool wait_for_progress(QTestState *qts, uint64_t want)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (state_word(qts, PROGRESS_OFF) >= want) {
            return true;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    return false;
}

static void test_masked_interrupt_stays_off_the_lock(const void *data)
{
    uint64_t clear_locks, masked_locks;
    QTestState *qts;
    bool done;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    qts = boot_cpu_case("tcg", "R10000", 1, write_r10k_guest_image,
                        build_masked_interrupt);
    g_free(qtest_hmp(qts, "sync-profile on"));

    g_assert_true(wait_for_progress(qts, 1));
    g_free(qtest_hmp(qts, "sync-profile reset"));
    qtest_writeq(qts, STATE_PHYS + MI_GO_OFF, 1);
    g_assert_true(wait_for_progress(qts, 2));
    clear_locks = bql_acquisitions(qts);

    g_free(qtest_hmp(qts, "sync-profile reset"));
    qtest_writeq(qts, STATE_PHYS + MI_GO_OFF, 2);
    done = wait_for_cpu0_done(qts);
    masked_locks = bql_acquisitions(qts);

    g_test_message("big-lock acquisitions over %u passes: %" PRIu64
                   " with no line, %" PRIu64 " with a masked line",
                   MI_PASSES, clear_locks, masked_locks);
    if (!done) {
        g_test_message("alive 0x%" PRIx64 " progress 0x%" PRIx64
                       " exception 0x%" PRIx64,
                       state_word(qts, ALIVE_OFF),
                       state_word(qts, PROGRESS_OFF),
                       state_word(qts, EXC_MARK_OFF));
    }

    g_assert_true(done);
    g_assert_cmphex(state_word(qts, MI_STATUS_OFF) & STATUS_IM7, ==, 0);
    g_assert_cmphex(state_word(qts, MI_CAUSE_OFF) & CAUSE_IP7, ==, CAUSE_IP7);
    g_assert_cmpuint(masked_locks, <, clear_locks + MI_PASSES / 10);

    /* The line the phases carried is still deliverable on the unmask. */
    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        if (state_word(qts, EXC_MARK_OFF) == EXC_MAGIC) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(state_word(qts, EXC_MARK_OFF), ==, EXC_MAGIC);
    g_assert_cmphex(state_word(qts, EXC_CAUSE_OFF) & CAUSE_IP7, ==, CAUSE_IP7);
    g_assert_cmphex(state_word(qts, EXC_CAUSE_OFF) & CAUSE_EXCCODE, ==, 0);

    qtest_quit(qts);
}

/*
 * The re-assert at ERET.  A line held off by EXL alone has had its flag
 * dropped by the passes above; clearing EXL with ERET must deliver it at
 * the instruction ERET returns to, not at the next edge on the line.
 * Nothing here lowers or re-raises IP7, so a missed re-assert leaves the
 * guest running the continuation with the interrupt never taken.
 */
#define UE_CAUSE_OFF        0x120
#define UE_STATUS_OFF       0x140
#define UE_CONT_OFF         0x160
#define STATUS_EXL          0x2u
#define IMAGE_KSEG1_BASE    0xbfc00000u

static unsigned int ue_cont_pos;

static void emit_eret(GuestImage *img)
{
    emit(img, 0x42000018);
}

static void build_unmask_by_eret(GuestImage *img)
{
    unsigned int spin;

    /* s0: IM7 open, IE set, EXL set -- EXL alone holds the line off. */
    emit_mfc0(img, R_T3, CP0_STATUS, 0);
    emit_lui(img, R_T4, STATUS_KEEP_HI);
    emit_ori(img, R_T4, R_T4, STATUS_KEEP_LO);
    emit_and(img, R_S0, R_T3, R_T4);
    emit_ori(img, R_S0, R_S0, STATUS_IM7 | STATUS_EXL | 1);
    emit_mtc0(img, R_S0, CP0_STATUS, 0);
    emit_ehb(img);
    emit_publish(img, R_S0, UE_STATUS_OFF);

    /* Compare at the counter matches at once; IP7 stands and EXL holds it. */
    emit_mfc0(img, R_T5, CP0_COUNT, 0);
    emit_mtc0(img, R_T5, CP0_COMPARE, 0);
    emit_ehb(img);
    spin = img->pos;
    emit_mfc0(img, R_T6, CP0_CAUSE, 0);
    emit_andi(img, R_T7, R_T6, CAUSE_IP7);
    emit_beq(img, R_T7, R_ZERO, spin);
    emit_nop(img);
    emit_publish(img, R_T6, UE_CAUSE_OFF);

    /* Passes through the loop with the line standing and masked by EXL. */
    emit_status_passes(img, R_S0, MI_PASSES / 10);

    /* EPC is the continuation: two words of address, mtc0, ehb, eret. */
    ue_cont_pos = img->pos + 5 * 4;
    emit_li_addr(img, R_T5, IMAGE_KSEG1_BASE + ue_cont_pos);
    emit_mtc0(img, R_T5, CP0_EPC, 0);
    emit_ehb(img);
    emit_eret(img);
    g_assert_cmpuint(img->pos, ==, ue_cont_pos);

    /* Reached only if the ERET delivered nothing. */
    emit_li(img, R_T3, 1);
    emit_publish(img, R_T3, UE_CONT_OFF);
    emit_done_and_halt(img);
}

static void test_unmask_by_eret_delivers(const void *data)
{
    QTestState *qts;
    unsigned int tries;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    qts = boot_cpu_case("tcg", "R10000", 1, write_r10k_guest_image,
                        build_unmask_by_eret);
    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (state_word(qts, EXC_MARK_OFF) == EXC_MAGIC ||
            state_word(qts, DONE_OFF) == DONE_MAGIC) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }

    g_assert_cmphex(state_word(qts, UE_STATUS_OFF) &
                    (STATUS_IM7 | STATUS_EXL | 1), ==,
                    STATUS_IM7 | STATUS_EXL | 1);
    g_assert_cmphex(state_word(qts, UE_CAUSE_OFF) & CAUSE_IP7, ==, CAUSE_IP7);
    /* Taken at the instruction ERET returns to: the continuation never runs. */
    g_assert_cmphex(state_word(qts, UE_CONT_OFF), ==, 0);
    g_assert_cmphex(state_word(qts, EXC_MARK_OFF), ==, EXC_MAGIC);
    g_assert_cmphex(state_word(qts, EXC_CAUSE_OFF) & CAUSE_IP7, ==, CAUSE_IP7);
    g_assert_cmphex(state_word(qts, EXC_CAUSE_OFF) & CAUSE_EXCCODE, ==, 0);
    g_assert_cmphex(state_word(qts, EXC_EPC_OFF), ==,
                    0xffffffff00000000ULL | IMAGE_KSEG1_BASE | ue_cont_pos);

    qtest_quit(qts);
}

static void test_tlb_random_global(const void *data)
{
    QTestState *qts;
    bool done;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    qts = boot_cpu_case("tcg", "R10000", 1, write_r10k_guest_image,
                        build_tlb_random_global);
    done = wait_for_cpu0_done(qts);

    if (!done || state_word(qts, RG_FAULT_OFF) != 0) {
        g_test_message("alive 0x%" PRIx64 " done 0x%" PRIx64
                       " exception 0x%" PRIx64,
                       state_word(qts, ALIVE_OFF),
                       state_word(qts, DONE_OFF),
                       state_word(qts, EXC_MARK_OFF));
        g_test_message("fault 0x%" PRIx64 " initial 0x%" PRIx64
                       " final 0x%" PRIx64,
                       state_word(qts, RG_FAULT_OFF),
                       state_word(qts, RG_INITIAL_OFF),
                       state_word(qts, RG_SEEN_OFF));
    }

    g_assert_true(done);
    g_assert_cmphex(state_word(qts, ALIVE_OFF), ==, ALIVE_MAGIC);
    g_assert_cmphex(state_word(qts, EXC_MARK_OFF), ==, 0);
    g_assert_cmphex(state_word(qts, DONE_OFF), ==, DONE_MAGIC);
    g_assert_cmphex(state_word(qts, RG_FAULT_OFF), ==, 0);
    g_assert_cmphex(state_word(qts, RG_INITIAL_OFF), ==, RG_OLD_PATTERN);
    g_assert_cmphex(state_word(qts, RG_SEEN_OFF), ==, RG_NEW_PATTERN);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("mttcg/tlb/shootdown", NULL, test_tlb_shootdown);
    qtest_add_data_func("mttcg/tlb/asid", NULL, test_tlb_asid);
    qtest_add_data_func("mttcg/tlb/random-global", NULL,
                        test_tlb_random_global);
    qtest_add_data_func("mttcg/interrupt/masked-line-stays-off-the-lock", NULL,
                        test_masked_interrupt_stays_off_the_lock);
    qtest_add_data_func("mttcg/interrupt/unmask-by-eret-delivers", NULL,
                        test_unmask_by_eret_delivers);
    return g_test_run();
}
