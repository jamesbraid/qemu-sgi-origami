/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercise the R10000's deterministic load-linked link breakers on the
 * Origin 200, the only machine this fork builds that carries the R10000.
 * The processor requires SC to fail if an LL/SC sequence contains another
 * load or store, SYNC, CACHE, PREF, an exception, or ERET.  The synthesized
 * firmware is off so the fragment owns the reset vector and low memory.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"

#define IMAGE_SIZE          0x1000
#define VEC_TLB_REFILL      0x200
#define VEC_XTLB_REFILL     0x280
#define VEC_CACHE_ERROR     0x300
#define VEC_GENERAL         0x380

#define STATE_PHYS          0x00100000
#define STATE_KSEG1_HI      0xa010
#define RESULT_BASE         0x00
#define RESULT_STRIDE       0x10
#define TARGET_OFF          0x100
#define OTHER_OFF           0x108
#define DONE_OFF            0x1f0
#define EXC_COUNT_OFF       0x1f8
#define FAULT_OFF           0x200

#define OLD_VALUE           0x11
#define NEW_VALUE           0x22
#define OTHER_VALUE         0x44
#define DONE_MAGIC          0x10c5
#define FAULT_MAGIC         0x0bad

#define POLL_TRIES          10000
#define POLL_INTERVAL_US    1000

#define OPC_SPECIAL         (0x00u << 26)
#define OPC_BEQ             (0x04u << 26)
#define OPC_BNE             (0x05u << 26)
#define OPC_ADDIU           (0x09u << 26)
#define OPC_LUI             (0x0fu << 26)
#define OPC_CP0             (0x10u << 26)
#define OPC_CACHE           (0x2fu << 26)
#define OPC_PREF            (0x33u << 26)
#define OPC_LLD             (0x34u << 26)
#define OPC_LD              (0x37u << 26)
#define OPC_SCD             (0x3cu << 26)
#define OPC_SD              (0x3fu << 26)
#define OPC_JR              (0x08u | OPC_SPECIAL)
#define OPC_SYSCALL         (0x0cu | OPC_SPECIAL)
#define OPC_SYNC            (0x0fu | OPC_SPECIAL)
#define OPC_AND             (0x24u | OPC_SPECIAL)
#define OPC_MFC0            (OPC_CP0 | (0x00u << 21))
#define OPC_MTC0            (OPC_CP0 | (0x04u << 21))
#define OPC_ERET            0x42000018u

#define R_ZERO              0
#define R_T0                8
#define R_T1                9
#define R_T2                10
#define R_T3                11
#define R_S0                16
#define R_S6                22
#define R_S7                23
#define R_K0                26

#define CP0_STATUS          12
#define CP0_EPC             14
#define STATUS_ERL_CLEAR    (-5)
#define STATUS_EXL_CLEAR    (-3)

typedef enum LinkBreaker {
    BREAK_LOAD,
    BREAK_STORE,
    BREAK_SYNC,
    BREAK_CACHE,
    BREAK_PREF,
    BREAK_EXCEPTION,
    BREAK_ERET,
    BREAK_COUNT,
} LinkBreaker;

typedef struct GuestImage {
    uint8_t *code;
    unsigned int pos;
} GuestImage;

typedef struct CPUCase {
    const char *name;
    bool has_pref;
    uint32_t success_mask;
} CPUCase;

static const CPUCase cpu_cases[] = {
    { "R10000", true, 0 },
};

static const char *const breaker_names[] = {
    [BREAK_LOAD] = "load",
    [BREAK_STORE] = "store",
    [BREAK_SYNC] = "sync",
    [BREAK_CACHE] = "cache",
    [BREAK_PREF] = "pref",
    [BREAK_EXCEPTION] = "exception",
    [BREAK_ERET] = "eret",
};

static void emit(GuestImage *img, uint32_t insn)
{
    g_assert_cmpuint(img->pos + 4, <=, IMAGE_SIZE);
    stl_be_p(img->code + img->pos, insn);
    img->pos += 4;
}

static void emit_nop(GuestImage *img)
{
    emit(img, 0);
}

static void emit_branch(GuestImage *img, uint32_t opc, int rs, int rt,
                        unsigned int target)
{
    int32_t off = ((int32_t)target - (int32_t)(img->pos + 4)) / 4;

    emit(img, opc | (rs << 21) | (rt << 16) | (off & 0xffff));
}

static void emit_lui(GuestImage *img, int rt, uint16_t imm)
{
    emit(img, OPC_LUI | (rt << 16) | imm);
}

static void emit_addiu(GuestImage *img, int rt, int rs, int16_t imm)
{
    emit(img, OPC_ADDIU | (rs << 21) | (rt << 16) | (uint16_t)imm);
}

static void emit_and(GuestImage *img, int rd, int rs, int rt)
{
    emit(img, OPC_AND | (rs << 21) | (rt << 16) | (rd << 11));
}

static void emit_ld(GuestImage *img, int rt, int base, int16_t off)
{
    emit(img, OPC_LD | (base << 21) | (rt << 16) | (uint16_t)off);
}

static void emit_lld(GuestImage *img, int rt, int base, int16_t off)
{
    emit(img, OPC_LLD | (base << 21) | (rt << 16) | (uint16_t)off);
}

static void emit_sd(GuestImage *img, int rt, int base, int16_t off)
{
    emit(img, OPC_SD | (base << 21) | (rt << 16) | (uint16_t)off);
}

static void emit_scd(GuestImage *img, int rt, int base, int16_t off)
{
    emit(img, OPC_SCD | (base << 21) | (rt << 16) | (uint16_t)off);
}

static void emit_mfc0(GuestImage *img, int rt, int rd)
{
    emit(img, OPC_MFC0 | (rt << 16) | (rd << 11));
}

static void emit_mtc0(GuestImage *img, int rt, int rd)
{
    emit(img, OPC_MTC0 | (rt << 16) | (rd << 11));
}

static void emit_ehb(GuestImage *img)
{
    emit(img, OPC_SPECIAL | (3 << 6));
}

static void emit_report_and_spin(GuestImage *img, uint16_t magic,
                                 int16_t off)
{
    unsigned int spin;

    emit_lui(img, R_T2, STATE_KSEG1_HI);
    emit_addiu(img, R_T3, R_ZERO, magic);
    emit_sd(img, R_T3, R_T2, off);
    emit(img, OPC_SYNC);
    spin = img->pos;
    emit_branch(img, OPC_BEQ, R_ZERO, R_ZERO, spin);
    emit_nop(img);
}

static void emit_fault_stub(GuestImage *img)
{
    emit_report_and_spin(img, FAULT_MAGIC, FAULT_OFF);
}

static void emit_general_handler(GuestImage *img)
{
    unsigned int eret_path = VEC_GENERAL + 0x1c;

    emit_addiu(img, R_S6, R_S6, 1);
    emit_branch(img, OPC_BNE, R_S7, R_ZERO, eret_path);
    emit_nop(img);
    emit_mfc0(img, R_K0, CP0_EPC);
    emit_addiu(img, R_K0, R_K0, 4);
    emit(img, OPC_JR | (R_K0 << 21));
    emit_nop(img);

    g_assert_cmpuint(img->pos, ==, eret_path);
    emit_mfc0(img, R_K0, CP0_EPC);
    emit_addiu(img, R_K0, R_K0, 4);
    emit_mtc0(img, R_K0, CP0_EPC);
    emit_ehb(img);
    emit(img, OPC_ERET);
    emit_nop(img);
}

static void emit_sequence_start(GuestImage *img)
{
    emit_addiu(img, R_T0, R_ZERO, OLD_VALUE);
    emit_sd(img, R_T0, R_S0, TARGET_OFF);
    emit_addiu(img, R_T1, R_ZERO, NEW_VALUE);
    emit_lld(img, R_T2, R_S0, TARGET_OFF);
}

static void emit_sequence_result(GuestImage *img, LinkBreaker breaker)
{
    int16_t result = RESULT_BASE + breaker * RESULT_STRIDE;

    emit_scd(img, R_T1, R_S0, TARGET_OFF);
    emit_sd(img, R_T1, R_S0, result);
    emit_ld(img, R_T3, R_S0, TARGET_OFF);
    emit_sd(img, R_T3, R_S0, result + 8);
}

static void emit_clear_status_bit(GuestImage *img, int16_t mask)
{
    emit_mfc0(img, R_T0, CP0_STATUS);
    emit_addiu(img, R_T1, R_ZERO, mask);
    emit_and(img, R_T0, R_T0, R_T1);
    emit_mtc0(img, R_T0, CP0_STATUS);
    emit_ehb(img);
}

static void write_guest_image(int fd, bool has_pref)
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    ssize_t written;

    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, CODE_START);
    emit_nop(&img);

    img.pos = VEC_TLB_REFILL;
    emit_fault_stub(&img);
    img.pos = VEC_XTLB_REFILL;
    emit_fault_stub(&img);
    img.pos = VEC_CACHE_ERROR;
    emit_fault_stub(&img);
    img.pos = VEC_GENERAL;
    emit_general_handler(&img);

    img.pos = CODE_START;
    emit_lui(&img, R_S0, STATE_KSEG1_HI);
    emit_clear_status_bit(&img, STATUS_ERL_CLEAR);
    emit_addiu(&img, R_T0, R_ZERO, OTHER_VALUE);
    emit_sd(&img, R_T0, R_S0, OTHER_OFF);

    emit_sequence_start(&img);
    emit_ld(&img, R_T3, R_S0, OTHER_OFF);
    emit_sequence_result(&img, BREAK_LOAD);

    emit_sequence_start(&img);
    emit_sd(&img, R_T0, R_S0, OTHER_OFF);
    emit_sequence_result(&img, BREAK_STORE);

    emit_sequence_start(&img);
    emit(&img, OPC_SYNC);
    emit_sequence_result(&img, BREAK_SYNC);

    emit_sequence_start(&img);
    emit(&img, OPC_CACHE | (R_S0 << 21) | TARGET_OFF);
    emit_sequence_result(&img, BREAK_CACHE);

    emit_sequence_start(&img);
    if (has_pref) {
        emit(&img, OPC_PREF | (R_S0 << 21) | TARGET_OFF);
    } else {
        emit_nop(&img);
    }
    emit_sequence_result(&img, BREAK_PREF);

    emit_sequence_start(&img);
    emit_addiu(&img, R_S7, R_ZERO, 0);
    emit(&img, OPC_SYSCALL);
    emit_sequence_result(&img, BREAK_EXCEPTION);
    emit_clear_status_bit(&img, STATUS_EXL_CLEAR);

    emit_sequence_start(&img);
    emit_addiu(&img, R_S7, R_ZERO, 1);
    emit(&img, OPC_SYSCALL);
    emit_sequence_result(&img, BREAK_ERET);

    emit_sd(&img, R_S6, R_S0, EXC_COUNT_OFF);
    emit_report_and_spin(&img, DONE_MAGIC, DONE_OFF);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static void wait_for_done(QTestState *qts)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + DONE_OFF);
        uint64_t fault = qtest_readq(qts, STATE_PHYS + FAULT_OFF);

        if (done || fault) {
            g_assert_cmphex(fault, ==, 0);
            g_assert_cmphex(done, ==, DONE_MAGIC);
            return;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + DONE_OFF), ==, DONE_MAGIC);
}

static void test_cpu(gconstpointer opaque)
{
    const CPUCase *cpu_case = opaque;
    g_autofree char *image = NULL;
    QTestState *qts;
    uint32_t invalid_mask = 0;
    uint32_t observed_success_mask = 0;
    uint32_t wrong_value_mask = 0;
    int fd;

    fd = g_file_open_tmp("sn0-r10k-llsc-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    write_guest_image(fd, cpu_case->has_pref);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu %s -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", cpu_case->name, image);
    unlink(image);
    wait_for_done(qts);

    for (LinkBreaker breaker = 0; breaker < BREAK_COUNT; breaker++) {
        uint64_t result = qtest_readq(qts, STATE_PHYS + RESULT_BASE +
                                      breaker * RESULT_STRIDE);
        uint64_t value = qtest_readq(qts, STATE_PHYS + RESULT_BASE +
                                     breaker * RESULT_STRIDE + 8);
        uint64_t expected_value;

        g_test_message("%s after %s", cpu_case->name,
                       breaker_names[breaker]);
        if (result > 1) {
            invalid_mask |= 1u << breaker;
        }
        if (result == 1) {
            observed_success_mask |= 1u << breaker;
        }
        expected_value = result == 1 ? NEW_VALUE : OLD_VALUE;
        if (value != expected_value) {
            wrong_value_mask |= 1u << breaker;
        }
    }
    g_assert_cmphex(invalid_mask, ==, 0);
    g_assert_cmphex(wrong_value_mask, ==, 0);
    g_assert_cmphex(observed_success_mask, ==, cpu_case->success_mask);
    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + EXC_COUNT_OFF), ==, 2);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_data_func("/r10k-llsc/r10000", &cpu_cases[0], test_cpu);
    return g_test_run();
}
