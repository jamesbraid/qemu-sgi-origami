/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Four vCPUs rendezvous and then contend for one word, on a machine that
 * contains none of this fork's devices.
 *
 * This is the machine-independent rung of the MTTCG ladder.  It runs on
 * malta, where the -smp CPUs are plain MIPS cores rather than a CPS cluster,
 * so every one of them comes out of reset free-running at the boot vector
 * and nothing but the accelerator stands between the guest program and the
 * host.  A failure here is evidence about target/mips and accel/tcg, not
 * about the SN0 machine model.
 *
 * The guest program is generated here and handed to QEMU as a raw -bios
 * image: malta copies the flash image to the reset vector, so all four CPUs
 * start executing it with no board firmware in the way.  A CPU learns which
 * of the four it is from the CPUNum field of EBase, which QEMU fills in from
 * the vCPU index at reset.  Identity deliberately does not come from LL/SC:
 * the primitive under test cannot also be the mechanism that decides who is
 * who, or a broken primitive makes the failure unattributable.
 *
 * The guest spins are unbounded.  Each CPU publishes a progress word as it
 * enters each phase, so a stall can be read off the state dump, and the only
 * timeout is the host's polling deadline.
 *
 * ABA: QEMU lowers SC to a compare-exchange against the value LL read, so it
 * cannot tell a word that never changed from one that changed and changed
 * back.  That is accepted QEMU-wide behaviour, and this test does not probe
 * it.  What it asserts is that the total is exact, which a lost update
 * breaks whether or not ABA is detectable.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define NR_CPUS             4

/*
 * State words live in low RAM, well clear of the flash the image is loaded
 * into, and are reached through KSEG1 so the guest never depends on cache
 * behaviour.  QEMU hands out zeroed RAM, which is what makes "still zero"
 * mean "not yet published" for the spins and starts the counter at zero.
 */
#define STATE_PHYS          0x00100000
#define STATE_KSEG1_HI      0xa010

#define ALIVE_OFF           0x000
#define SLOT_OFF            0x020
#define PROGRESS_OFF        0x040
#define DONE_OFF            0x060
#define COUNTER_OFF         0x080

#define ALIVE_MAGIC         0x00a11e00ULL
#define DONE_MAGIC          0x00d04e00ULL

/* The phase a CPU has published once its increments are all retired. */
#define PROGRESS_COUNTED    4

/* Guest program image.  Loaded at the reset vector, so entry is offset 0. */
#define IMAGE_SIZE          0x400
/*
 * Malta patches its board ID into the word at offset 0x10 of the image it
 * maps at the reset vector, so the program proper starts past that word.
 */
#define CODE_START          0x20

#define POLL_TRIES          30000
#define POLL_INTERVAL_US    1000

/* Instruction encodings, named as in target/mips/tcg/translate.c. */
#define OPC_SPECIAL         (0x00u << 26)
#define OPC_BEQ             (0x04u << 26)
#define OPC_BNE             (0x05u << 26)
#define OPC_ADDIU           (0x09u << 26)
#define OPC_SLTIU           (0x0bu << 26)
#define OPC_ANDI            (0x0cu << 26)
#define OPC_ORI             (0x0du << 26)
#define OPC_LUI             (0x0fu << 26)
#define OPC_DADDIU          (0x19u << 26)
#define OPC_LLD             (0x34u << 26)
#define OPC_LD              (0x37u << 26)
#define OPC_SCD             (0x3cu << 26)
#define OPC_SD              (0x3fu << 26)
#define OPC_SLL             (0x00u | OPC_SPECIAL)
#define OPC_ADDU            (0x21u | OPC_SPECIAL)
#define OPC_SYNC            (0x0fu | OPC_SPECIAL)
#define OPC_MFC0            (0x10u << 26)
#define OPC_WAIT            ((0x10u << 26) | (1u << 25) | 0x20u)

/* General-purpose registers the guest program uses. */
#define R_ZERO              0
#define R_T0                8
#define R_T1                9
#define R_T2                10
#define R_T3                11
#define R_T4                12
#define R_T5                13
#define R_T6                14
#define R_T7                15
#define R_T8                24
#define R_K0                26

/* CP0 register 15 select 1 is EBase, whose low 10 bits are CPUNum. */
#define CP0_EBASE           15
#define CP0_EBASE_SEL       1
#define CPUNUM_MASK         0x3ff

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

static void emit_lld(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_immediate(img, OPC_LLD, base, rt, off);
}

static void emit_scd(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_immediate(img, OPC_SCD, base, rt, off);
}

static void emit_sll(GuestImage *img, int rd, int rt, int sa)
{
    emit(img, OPC_SLL | (rt << 16) | (rd << 11) | (sa << 6));
}

static void emit_addu(GuestImage *img, int rd, int rs, int rt)
{
    emit(img, OPC_ADDU | (rs << 21) | (rt << 16) | (rd << 11));
}

static void emit_mfc0(GuestImage *img, int rt, int rd, int sel)
{
    emit(img, OPC_MFC0 | (rt << 16) | (rd << 11) | sel);
}

static void emit_sync(GuestImage *img)
{
    emit(img, OPC_SYNC);
}

static void emit_nop(GuestImage *img)
{
    emit_sll(img, R_ZERO, R_ZERO, 0);
}

static void emit_wait(GuestImage *img)
{
    emit(img, OPC_WAIT);
}

/* Branches take a byte offset within the image and encode it relative. */
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

static void emit_li(GuestImage *img, int rt, uint64_t value)
{
    /* lui sign-extends, so only constants with bit 31 clear come out whole. */
    g_assert_cmpuint(value, <, 0x80000000ULL);
    emit_lui(img, rt, value >> 16);
    emit_ori(img, rt, rt, value & 0xffff);
}

/* Publish a word for this CPU and make it visible before anything after it. */
static void emit_publish(GuestImage *img, int rt, uint16_t off)
{
    emit_sd(img, rt, R_T2, off);
    emit_sync(img);
}

static void emit_progress(GuestImage *img, int phase)
{
    emit_addiu(img, R_T4, R_ZERO, phase);
    emit_publish(img, R_T4, PROGRESS_OFF);
}

static void write_guest_program(int fd, unsigned int increments)
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    unsigned int spin, retry, halt, i;
    ssize_t written;

    g_assert_cmpuint(increments, <=, 0x7fff);

    emit_b(&img, CODE_START);
    emit_nop(&img);
    img.pos = CODE_START;

    /* k0 = CPUNum, t0 = state base, t2 = this CPU's words. */
    emit_mfc0(&img, R_K0, CP0_EBASE, CP0_EBASE_SEL);
    emit_andi(&img, R_K0, R_K0, CPUNUM_MASK);
    emit_lui(&img, R_T0, STATE_KSEG1_HI);
    emit_sll(&img, R_T1, R_K0, 3);
    emit_addu(&img, R_T2, R_T0, R_T1);

    /* Phase 1: this CPU is running. */
    emit_li(&img, R_T3, ALIVE_MAGIC);
    emit_publish(&img, R_T3, ALIVE_OFF);
    emit_progress(&img, 1);

    /* Phase 2: claim slot CPUNum by writing CPUNum + 1 into it. */
    emit_addiu(&img, R_T3, R_K0, 1);
    emit_publish(&img, R_T3, SLOT_OFF);
    emit_progress(&img, 2);

    /* Wait until every slot has been claimed. */
    spin = img.pos;
    for (i = 0; i < NR_CPUS; i++) {
        emit_ld(&img, R_T5, R_T0, SLOT_OFF + 8 * i);
        emit_beq(&img, R_T5, R_ZERO, spin);
        emit_nop(&img);
    }
    emit_sync(&img);

    /* Phase 3: every CPU increments the one shared word, N times each. */
    emit_progress(&img, 3);
    emit_addiu(&img, R_T6, R_ZERO, increments);
    retry = img.pos;
    emit_lld(&img, R_T7, R_T0, COUNTER_OFF);
    emit_daddiu(&img, R_T7, R_T7, 1);
    emit_scd(&img, R_T7, R_T0, COUNTER_OFF);
    /* SC leaves 0 in its source register when the store did not happen. */
    emit_beq(&img, R_T7, R_ZERO, retry);
    emit_nop(&img);
    emit_addiu(&img, R_T6, R_T6, -1);
    emit_bne(&img, R_T6, R_ZERO, retry);
    emit_nop(&img);
    emit_sync(&img);

    /*
     * Phase 4: wait until every CPU has retired its increments, so that the
     * DONE words the host waits on can only appear once the count is final.
     * The test is "reached at least this phase" rather than "is at this
     * phase", because a CPU that leaves the rendezvous first goes on to
     * publish a later phase while the others are still watching it.
     */
    emit_progress(&img, PROGRESS_COUNTED);
    spin = img.pos;
    for (i = 0; i < NR_CPUS; i++) {
        emit_ld(&img, R_T5, R_T0, PROGRESS_OFF + 8 * i);
        emit_sltiu(&img, R_T8, R_T5, PROGRESS_COUNTED);
        emit_bne(&img, R_T8, R_ZERO, spin);
        emit_nop(&img);
    }
    emit_sync(&img);

    emit_progress(&img, PROGRESS_COUNTED + 1);
    emit_li(&img, R_T3, DONE_MAGIC);
    emit_publish(&img, R_T3, DONE_OFF);

    /*
     * Idle rather than spin, so four finished CPUs stop competing with the
     * host for cores.  A CPU woken by anything comes straight back here.
     */
    halt = img.pos;
    emit_wait(&img);
    emit_b(&img, halt);
    emit_nop(&img);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static uint64_t state_word(QTestState *qts, unsigned int off)
{
    return qtest_readq(qts, STATE_PHYS + off);
}

static void report_state(QTestState *qts)
{
    unsigned int i;

    for (i = 0; i < NR_CPUS; i++) {
        g_test_message("cpu %u: alive 0x%" PRIx64 " slot %" PRIu64
                       " progress %" PRIu64 " done 0x%" PRIx64, i,
                       state_word(qts, ALIVE_OFF + 8 * i),
                       state_word(qts, SLOT_OFF + 8 * i),
                       state_word(qts, PROGRESS_OFF + 8 * i),
                       state_word(qts, DONE_OFF + 8 * i));
    }
    g_test_message("shared counter %" PRIu64, state_word(qts, COUNTER_OFF));
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

static bool state_as_expected(QTestState *qts, unsigned int increments)
{
    unsigned int i;

    for (i = 0; i < NR_CPUS; i++) {
        if (state_word(qts, ALIVE_OFF + 8 * i) != ALIVE_MAGIC ||
            state_word(qts, SLOT_OFF + 8 * i) != i + 1) {
            return false;
        }
    }
    return state_word(qts, COUNTER_OFF) == (uint64_t)NR_CPUS * increments;
}

static void run_rendezvous(const char *accel, unsigned int increments)
{
    g_autofree char *image = NULL;
    QTestState *qts;
    unsigned int tries, i;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("mips64-mttcg-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    write_guest_program(fd, increments);
    close(fd);

    /*
     * The accelerator named here wins over the -accel qtest libqtest appends,
     * because QEMU takes the first one on the command line that initialises.
     * An explicit R2 CPU is required: EBase is MIPS64R2 and malta's default
     * 20Kc is R1.  5KEc has no coherency manager, so malta builds plain CPUs
     * rather than a CPS cluster and all four run from reset.
     */
    qts = qtest_initf("-M malta -m 128M -smp %d -cpu 5KEc -bios %s "
                      "-accel %s -no-reboot -no-shutdown",
                      NR_CPUS, image, accel);
    unlink(image);

    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (all_cpus_done(qts)) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }

    if (tries == POLL_TRIES || !state_as_expected(qts, increments)) {
        report_state(qts);
    }

    g_assert_cmpuint(tries, <, POLL_TRIES);
    for (i = 0; i < NR_CPUS; i++) {
        g_assert_cmphex(state_word(qts, ALIVE_OFF + 8 * i), ==, ALIVE_MAGIC);
        /* Slot i is claimed by CPUNum i, so this is not a permutation claim. */
        g_assert_cmpuint(state_word(qts, SLOT_OFF + 8 * i), ==, i + 1);
        g_assert_cmphex(state_word(qts, DONE_OFF + 8 * i), ==, DONE_MAGIC);
    }
    g_assert_cmpuint(state_word(qts, COUNTER_OFF), ==,
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("mttcg/rendezvous/thread-multi", NULL,
                        test_rendezvous_thread_multi);
    qtest_add_data_func("mttcg/rendezvous/round-robin", NULL,
                        test_rendezvous_round_robin);
    return g_test_run();
}
