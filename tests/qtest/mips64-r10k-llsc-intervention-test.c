/*
 * R10000 external-intervention load-linked tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The MIPS R10000 Microprocessor User's Manual, version 2.0 of
 * January 29, 1997, page 27, requires SC to fail after an external
 * exclusive intervention or invalidate to the secondary-cache block that
 * contains the linked address.  Page 60 defines an asserted secondary-cache
 * block-size mode as 32 words; the R10000 CPU definition asserts that mode,
 * giving this model a 128-byte block.
 *
 * Run two R10000 CPUs on Malta so no SGI machine device participates.
 * A breakpoint stops the victim immediately after LLD.  The host removes it,
 * lets the victim spin on an integer branch, and stops the peer after its
 * writes.  Only then does the host replace the spin branch with a NOP and
 * allow the victim's SCD to retire.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define IMAGE_SIZE          0x1000
#define CODE_PHYS           UINT64_C(0x00200000)
#define CODE_KSEG0          UINT64_C(0xffffffff80200000)
#define VICTIM_OFF          0x400
#define PEER_OFF            0x800

#define DATA_PHYS           0x00100000
#define DATA_KSEG0_HI       0x8010
#define TARGET_OFF          0x000
#define SAME_BLOCK_OFF      0x078
#define COMMAND_OFF         0x100
#define PEER_DONE_OFF       0x108
#define SC_RESULT_OFF       0x110
#define VICTIM_DONE_OFF     0x118

#define OLD_VALUE           UINT64_C(0x11)
#define SC_VALUE            UINT64_C(0x22)
#define PEER_VALUE          UINT64_C(0x44)
#define COMMAND_MAGIC       UINT64_C(0x10)
#define PEER_DONE_MAGIC     UINT64_C(0x20)
#define VICTIM_DONE_MAGIC   UINT64_C(0x30)

#define POLL_TRIES          30000
#define POLL_INTERVAL_US    1000

#define OPC_SPECIAL         (0x00u << 26)
#define OPC_BEQ             (0x04u << 26)
#define OPC_ADDIU           (0x09u << 26)
#define OPC_LUI             (0x0fu << 26)
#define OPC_LLD             (0x34u << 26)
#define OPC_LD              (0x37u << 26)
#define OPC_SCD             (0x3cu << 26)
#define OPC_SD              (0x3fu << 26)
#define OPC_SYNC            (0x0fu | OPC_SPECIAL)
#define OPC_WAIT            0x42000020u

#define R_ZERO              0
#define R_T0                8
#define R_T1                9
#define R_T2                10

typedef struct GuestImage {
    uint8_t bytes[IMAGE_SIZE];
    unsigned int pos;
} GuestImage;

typedef enum Intervention {
    INTERVENTION_OUTSIDE_BLOCK,
    INTERVENTION_SAME_BLOCK,
    INTERVENTION_ABA,
} Intervention;

static void emit(GuestImage *img, uint32_t insn)
{
    g_assert_cmpuint(img->pos + 4, <=, sizeof(img->bytes));
    stl_be_p(img->bytes + img->pos, insn);
    img->pos += 4;
}

static void emit_i(GuestImage *img, uint32_t op, int rs, int rt, uint16_t imm)
{
    emit(img, op | (rs << 21) | (rt << 16) | imm);
}

static void emit_addiu(GuestImage *img, int rt, int rs, int16_t imm)
{
    emit_i(img, OPC_ADDIU, rs, rt, imm);
}

static void emit_lui(GuestImage *img, int rt, uint16_t imm)
{
    emit_i(img, OPC_LUI, R_ZERO, rt, imm);
}

static void emit_ld(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_i(img, OPC_LD, base, rt, off);
}

static void emit_sd(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_i(img, OPC_SD, base, rt, off);
}

static void emit_lld(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_i(img, OPC_LLD, base, rt, off);
}

static void emit_scd(GuestImage *img, int rt, int base, uint16_t off)
{
    emit_i(img, OPC_SCD, base, rt, off);
}

static void emit_beq(GuestImage *img, int rs, int rt, unsigned int dest)
{
    int delta = ((int)dest - (int)img->pos - 4) / 4;

    g_assert_cmpint(delta, >=, INT16_MIN);
    g_assert_cmpint(delta, <=, INT16_MAX);
    emit_i(img, OPC_BEQ, rs, rt, delta);
}

static void emit_nop(GuestImage *img)
{
    emit(img, 0);
}

static void emit_halt(GuestImage *img)
{
    unsigned int halt = img->pos;

    emit(img, OPC_WAIT);
    emit_beq(img, R_ZERO, R_ZERO, halt);
    emit_nop(img);
}

static void build_victim(GuestImage *img, unsigned int *after_lld)
{
    img->pos = VICTIM_OFF;
    emit_lui(img, R_T0, DATA_KSEG0_HI);
    emit_lld(img, R_T1, R_T0, TARGET_OFF);
    *after_lld = img->pos;

    /* The host replaces this branch after the peer has finished. */
    emit_beq(img, R_ZERO, R_ZERO, *after_lld);
    emit_nop(img);

    emit_addiu(img, R_T2, R_ZERO, SC_VALUE);
    emit_scd(img, R_T2, R_T0, TARGET_OFF);
    emit_sd(img, R_T2, R_T0, SC_RESULT_OFF);
    emit_addiu(img, R_T2, R_ZERO, VICTIM_DONE_MAGIC);
    emit_sd(img, R_T2, R_T0, VICTIM_DONE_OFF);
    emit(img, OPC_SYNC);
    emit_halt(img);
}

static void build_peer(GuestImage *img, Intervention intervention,
                       unsigned int *peer_done)
{
    unsigned int wait;

    img->pos = PEER_OFF;
    emit_lui(img, R_T0, DATA_KSEG0_HI);
    wait = img->pos;
    emit_ld(img, R_T1, R_T0, COMMAND_OFF);
    emit_beq(img, R_T1, R_ZERO, wait);
    emit_nop(img);

    if (intervention == INTERVENTION_SAME_BLOCK) {
        emit_addiu(img, R_T2, R_ZERO, PEER_VALUE);
        emit_sd(img, R_T2, R_T0, SAME_BLOCK_OFF);
    } else if (intervention == INTERVENTION_ABA) {
        emit_addiu(img, R_T2, R_ZERO, SC_VALUE);
        emit_sd(img, R_T2, R_T0, TARGET_OFF);
        emit_addiu(img, R_T2, R_ZERO, OLD_VALUE);
        emit_sd(img, R_T2, R_T0, TARGET_OFF);
    }
    emit_addiu(img, R_T2, R_ZERO, PEER_DONE_MAGIC);
    emit_sd(img, R_T2, R_T0, PEER_DONE_OFF);
    emit(img, OPC_SYNC);
    *peer_done = img->pos;
    emit_halt(img);
}

static char *write_image(Intervention intervention, unsigned int *after_lld,
                         unsigned int *peer_done)
{
    g_autofree GuestImage *img = g_new0(GuestImage, 1);
    char *path = NULL;
    int fd;

    build_victim(img, after_lld);
    build_peer(img, intervention, peer_done);
    fd = g_file_open_tmp("mips64-r10k-intervention-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, img->bytes, sizeof(img->bytes)), ==,
                    sizeof(img->bytes));
    close(fd);
    return path;
}

static void add_breakpoint(QTestState *qts, unsigned int cpu, uint64_t pc)
{
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'x-mips-debug-breakpoint', 'arguments': "
        "{ 'cpu-index': %u, 'pc': %" PRIu64 " } }", cpu, pc);
}

static void remove_breakpoint(QTestState *qts, unsigned int cpu, uint64_t pc)
{
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'x-mips-debug-breakpoint', 'arguments': "
        "{ 'cpu-index': %u, 'pc': %" PRIu64 ", 'remove': true } }",
        cpu, pc);
}

static void wait_for_word(QTestState *qts, uint64_t addr, uint64_t expected)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (qtest_readq(qts, addr) == expected) {
            return;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(qtest_readq(qts, addr), ==, expected);
}

static void run_intervention(Intervention intervention)
{
    g_autofree char *image = NULL;
    unsigned int after_lld, peer_done;
    uint64_t result, target;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    image = write_image(intervention, &after_lld, &peer_done);
    qts = qtest_initf(
        "-M malta -m 64M -smp 2 -cpu R10000 "
        "-device loader,file=%s,addr=0x%" PRIx64 ",force-raw=on "
        "-device loader,addr=0x%" PRIx64 ",cpu-num=0 "
        "-device loader,addr=0x%" PRIx64 ",cpu-num=1 "
        "-accel tcg,thread=multi -no-reboot -no-shutdown -S",
        image, CODE_PHYS, CODE_KSEG0 + VICTIM_OFF,
        CODE_KSEG0 + PEER_OFF);
    unlink(image);

    qtest_writeq(qts, DATA_PHYS + TARGET_OFF, OLD_VALUE);
    add_breakpoint(qts, 0, CODE_KSEG0 + after_lld);
    add_breakpoint(qts, 1, CODE_KSEG0 + peer_done);

    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    qtest_qmp_eventwait(qts, "STOP");

    /* Release the peer only after CPU0 has established its link. */
    qtest_writeq(qts, DATA_PHYS + COMMAND_OFF, COMMAND_MAGIC);
    remove_breakpoint(qts, 0, CODE_KSEG0 + after_lld);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    qtest_qmp_eventwait(qts, "STOP");

    g_assert_cmphex(qtest_readq(qts, DATA_PHYS + PEER_DONE_OFF), ==,
                    PEER_DONE_MAGIC);
    g_assert_cmphex(qtest_readq(qts, DATA_PHYS + VICTIM_DONE_OFF), ==, 0);
    if (intervention == INTERVENTION_SAME_BLOCK) {
        g_assert_cmphex(qtest_readq(qts, DATA_PHYS + SAME_BLOCK_OFF), ==,
                        PEER_VALUE);
    } else {
        g_assert_cmphex(qtest_readq(qts, DATA_PHYS + TARGET_OFF), ==,
                        OLD_VALUE);
    }

    qtest_writel(qts, CODE_PHYS + after_lld, 0);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    wait_for_word(qts, DATA_PHYS + VICTIM_DONE_OFF, VICTIM_DONE_MAGIC);
    result = qtest_readq(qts, DATA_PHYS + SC_RESULT_OFF);
    target = qtest_readq(qts, DATA_PHYS + TARGET_OFF);
    qtest_quit(qts);

    if (intervention == INTERVENTION_OUTSIDE_BLOCK) {
        if (result != 1 || target != SC_VALUE) {
            g_test_message("SCD result=0x%" PRIx64 ", target=0x%" PRIx64,
                           result, target);
            g_test_fail();
        }
    } else if (result != 0 || target != OLD_VALUE) {
        g_test_message("SCD result=0x%" PRIx64 ", target=0x%" PRIx64,
                       result, target);
        g_test_fail();
    }
}

static void test_outside_block_intervention(void)
{
    run_intervention(INTERVENTION_OUTSIDE_BLOCK);
}

static void test_same_block_intervention(void)
{
    run_intervention(INTERVENTION_SAME_BLOCK);
}

static void test_target_aba_intervention(void)
{
    run_intervention(INTERVENTION_ABA);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/r10k-llsc-intervention/outside-block-peer-write",
                   test_outside_block_intervention);
    qtest_add_func("/r10k-llsc-intervention/same-block-peer-write",
                   test_same_block_intervention);
    qtest_add_func("/r10k-llsc-intervention/target-aba-peer-write",
                   test_target_aba_intervention);
    return g_test_run();
}
