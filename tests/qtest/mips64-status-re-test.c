/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The R10000's Status.RE bit promises reverse-endian user-mode loads and
 * stores (user's manual version 2.0, section 14.10).  The model stores the
 * bit and implements none of the semantics, so raising it has to be loud
 * rather than silent: helper_mtc0_status() logs a LOG_UNIMP line when a
 * guest sets RE on an R10000-family processor.
 *
 * Each case boots a fragment that sets RE from kernel mode and reads the
 * -d unimp log back.  On the R10000 the line must appear.  On the R4000 -
 * whose Status mask also leaves bit 25 writable, and which the bare-metal
 * ROM suite uses as its calibration model - it must not, pinning both the
 * processor gate and that other models keep today's silent behaviour.
 *
 * Malta rather than the Origin 200, for the same reason the MTTCG ladder
 * starts there: the property under test lives in target/mips, and a
 * machine that contains none of this fork's devices keeps the evidence
 * about the CPU model alone.  Malta also takes -cpu, which the R4000 leg
 * of the test needs.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

/*
 * The image is loaded as the boot flash, so offsets are from the reset
 * vector.  Boot-exception vectors stay in flash because the fragment never
 * clears Status.BEV.  Malta patches its board ID into the word at offset
 * 0x10, so entry branches over it.
 */
#define IMAGE_SIZE          0x1000
#define VEC_TLB_REFILL      0x200
#define VEC_XTLB_REFILL     0x280
#define VEC_CACHE_ERROR     0x300
#define VEC_GENERAL         0x380
#define CODE_START          0x400

/*
 * Result words in low RAM, reached uncached through KSEG1 so nothing
 * depends on cache state.  QEMU hands out zeroed RAM, so "still zero"
 * means "not yet reported".
 */
#define STATE_PHYS          0x00100000
#define STATE_KSEG1_HI      0xa010
#define DONE_OFF            0x00
#define FAULT_OFF           0x08

#define DONE_MAGIC          0x0d04
#define FAULT_MAGIC         0x0bad

#define POLL_TRIES          10000
#define POLL_INTERVAL_US    1000

/* Instruction encodings, named as in target/mips/tcg/translate.c. */
#define OPC_SPECIAL         (0x00u << 26)
#define OPC_BEQ             (0x04u << 26)
#define OPC_ADDIU           (0x09u << 26)
#define OPC_ORI             (0x0du << 26)
#define OPC_LUI             (0x0fu << 26)
#define OPC_CP0             (0x10u << 26)
#define OPC_SD              (0x3fu << 26)
#define OPC_OR              (0x25u | OPC_SPECIAL)
#define OPC_SYNC            (0x0fu | OPC_SPECIAL)
#define OPC_MFC0            (OPC_CP0 | (0x00u << 21))
#define OPC_MTC0            (OPC_CP0 | (0x04u << 21))

#define R_ZERO              0
#define R_T0                8
#define R_T1                9
#define R_T2                10
#define R_T3                11

#define CP0_STATUS          12
#define STATUS_RE_HI        0x0200  /* bit 25, as a LUI immediate */

typedef struct {
    uint8_t *code;
    unsigned int pos;
} GuestImage;

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

/* EHB is SLL $0, $0, 3: a hazard barrier on R2, a no-op before it. */
static void emit_ehb(GuestImage *img)
{
    emit(img, OPC_SPECIAL | (3 << 6));
}

static void emit_b(GuestImage *img, unsigned int target)
{
    int32_t off = ((int32_t)target - (int32_t)(img->pos + 4)) / 4;

    emit(img, OPC_BEQ | (off & 0xffff));
}

static void emit_lui(GuestImage *img, int rt, uint16_t imm)
{
    emit(img, OPC_LUI | (rt << 16) | imm);
}

static void emit_addiu(GuestImage *img, int rt, int rs, uint16_t imm)
{
    emit(img, OPC_ADDIU | (rs << 21) | (rt << 16) | imm);
}

static void emit_or(GuestImage *img, int rd, int rs, int rt)
{
    emit(img, OPC_OR | (rs << 21) | (rt << 16) | (rd << 11));
}

static void emit_sd(GuestImage *img, int rt, int base, uint16_t off)
{
    emit(img, OPC_SD | (base << 21) | (rt << 16) | off);
}

static void emit_mfc0(GuestImage *img, int rt, int rd)
{
    emit(img, OPC_MFC0 | (rt << 16) | (rd << 11));
}

static void emit_mtc0(GuestImage *img, int rt, int rd)
{
    emit(img, OPC_MTC0 | (rt << 16) | (rd << 11));
}

static void emit_sync(GuestImage *img)
{
    emit(img, OPC_SYNC);
}

/* Report a value and spin.  The b needs a delay slot, hence the nop. */
static void emit_report_and_spin(GuestImage *img, uint16_t magic,
                                 uint16_t off)
{
    unsigned int spin;

    emit_lui(img, R_T2, STATE_KSEG1_HI);
    emit_addiu(img, R_T3, R_ZERO, magic);
    emit_sd(img, R_T3, R_T2, off);
    emit_sync(img);
    spin = img->pos;
    emit_b(img, spin);
    emit_nop(img);
}

/*
 * Any exception says so instead of wandering off and looking like a hang.
 * The fragment runs entirely in kernel mode from flash, so nothing should
 * fault; the stub exists to make it diagnosable if something does.
 */
static void emit_fault_stub(GuestImage *img)
{
    emit_report_and_spin(img, FAULT_MAGIC, FAULT_OFF);
}

static void write_guest_image(int fd)
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    ssize_t written;

    emit_b(&img, CODE_START);
    emit_nop(&img);

    img.pos = VEC_TLB_REFILL;
    emit_fault_stub(&img);
    img.pos = VEC_XTLB_REFILL;
    emit_fault_stub(&img);
    img.pos = VEC_CACHE_ERROR;
    emit_fault_stub(&img);
    img.pos = VEC_GENERAL;
    emit_fault_stub(&img);

    /*
     * Read Status, raise RE (bit 25), write it back.  Reset state - ERL
     * and BEV set - is preserved, so the fragment stays in kernel mode
     * and the write is an ordinary read-modify-write of a live register,
     * the shape a real guest's write would have.
     */
    img.pos = CODE_START;
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_lui(&img, R_T1, STATUS_RE_HI);
    emit_or(&img, R_T0, R_T0, R_T1);
    emit_mtc0(&img, R_T0, CP0_STATUS);
    emit_ehb(&img);
    emit_report_and_spin(&img, DONE_MAGIC, DONE_OFF);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

/*
 * Boot the fragment on the named CPU with the unimp log captured to a
 * file, wait for the guest's report, and return the log contents.
 */
static char *run_fragment(const char *cpu)
{
    g_autofree char *image = NULL;
    g_autofree char *log_path = NULL;
    QTestState *qts;
    unsigned int tries;
    uint64_t done = 0, fault = 0;
    char *log = NULL;
    int fd;

    fd = g_file_open_tmp("mips64-status-re-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    write_guest_image(fd);
    close(fd);

    fd = g_file_open_tmp("mips64-status-re-log-XXXXXX", &log_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf("-M malta -m 64M -cpu %s -bios %s -accel tcg "
                      "-d unimp -D %s -no-reboot -no-shutdown",
                      cpu, image, log_path);
    unlink(image);

    for (tries = 0; tries < POLL_TRIES; tries++) {
        done = qtest_readq(qts, STATE_PHYS + DONE_OFF);
        fault = qtest_readq(qts, STATE_PHYS + FAULT_OFF);
        if (done || fault) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(fault, ==, 0);
    g_assert_cmphex(done, ==, DONE_MAGIC);

    /* Quitting flushes the log file before it is read. */
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    unlink(log_path);
    return log;
}

static void test_r10000_logs_unimp(void)
{
    g_autofree char *log = run_fragment("R10000");

    g_assert_nonnull(strstr(log, "Status.RE"));
}

static void test_r4000_stays_silent(void)
{
    g_autofree char *log = run_fragment("R4000");

    g_assert_null(strstr(log, "Status.RE"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("status-re/r10000-logs-unimp", test_r10000_logs_unimp);
    qtest_add_func("status-re/r4000-stays-silent", test_r4000_stays_silent);

    return g_test_run();
}
