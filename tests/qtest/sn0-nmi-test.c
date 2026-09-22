/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercise the external MIPS NMI input of the R10000 on the Origin 200, the
 * only machine this fork builds that carries it.  A tiny guest records the
 * architectural state seen at the boot exception vector, returns through
 * ErrorEPC, and acknowledges each completed entry from its main loop.  The
 * NMI line is driven straight on the CPU, so the test stays about
 * target/mips rather than the Hub's interrupt routing; the synthesized
 * firmware is off so the guest owns the reset vector and low memory.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"

#define IMAGE_SIZE          0x1000
#define NMI_HANDLER         0x480
#define MAIN_LOOP           0x500

#define STATE_PHYS          0x00100000
#define STATE_KSEG1_HI      0xa010
#define READY_OFF           0x00
#define COUNT_OFF           0x08
#define ACK_OFF             0x10
#define STATUS_OFF          0x18
#define ERROREPC_OFF        0x20

#define READY_MAGIC         0x1400
#define POLL_INTERVAL_US    1000

#define OPC_SPECIAL         (0x00u << 26)
#define OPC_BEQ             (0x04u << 26)
#define OPC_BNE             (0x05u << 26)
#define OPC_ADDIU           (0x09u << 26)
#define OPC_AND             (0x24u | OPC_SPECIAL)
#define OPC_LUI             (0x0fu << 26)
#define OPC_CP0             (0x10u << 26)
#define OPC_LD              (0x37u << 26)
#define OPC_SD              (0x3fu << 26)
#define OPC_MFC0            (OPC_CP0 | (0x00u << 21))
#define OPC_SYNC            (0x0fu | OPC_SPECIAL)
#define OPC_ERET            0x42000018u

#define R_ZERO              0
#define R_T0                8
#define R_T1                9
#define R_T2                10
#define R_T3                11

#define CP0_STATUS          12
#define CP0_ERROREPC        30
#define STATUS_SR           (1u << 20)
#define STATUS_NMI          (1u << 19)
#define STATUS_ERL          (1u << 2)
#define STATUS_BEV          (1u << 22)

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

static void emit_branch(GuestImage *img, uint32_t opcode, int rs, int rt,
                        unsigned int target)
{
    int32_t off = ((int32_t)target - (int32_t)(img->pos + 4)) / 4;

    emit(img, opcode | (rs << 21) | (rt << 16) | (off & 0xffff));
}

static void emit_lui(GuestImage *img, int rt, uint16_t imm)
{
    emit(img, OPC_LUI | (rt << 16) | imm);
}

static void emit_addiu(GuestImage *img, int rt, int rs, uint16_t imm)
{
    emit(img, OPC_ADDIU | (rs << 21) | (rt << 16) | imm);
}

static void emit_and(GuestImage *img, int rd, int rs, int rt)
{
    emit(img, OPC_AND | (rs << 21) | (rt << 16) | (rd << 11));
}

static void emit_ld(GuestImage *img, int rt, int base, uint16_t off)
{
    emit(img, OPC_LD | (base << 21) | (rt << 16) | off);
}

static void emit_sd(GuestImage *img, int rt, int base, uint16_t off)
{
    emit(img, OPC_SD | (base << 21) | (rt << 16) | off);
}

static void emit_mfc0(GuestImage *img, int rt, int rd)
{
    emit(img, OPC_MFC0 | (rt << 16) | (rd << 11));
}

static void write_guest_image(int fd)
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    ssize_t written;

    /* Branch clear of the low words so seeded board data cannot matter. */
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, CODE_START);
    emit_nop(&img);

    img.pos = CODE_START;
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_lui(&img, R_T1, STATUS_NMI >> 16);
    emit_and(&img, R_T2, R_T0, R_T1);
    emit_branch(&img, OPC_BNE, R_T2, R_ZERO, NMI_HANDLER);
    emit_nop(&img);
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, MAIN_LOOP);
    emit_nop(&img);

    img.pos = NMI_HANDLER;
    emit_lui(&img, R_T2, STATE_KSEG1_HI);
    emit_sd(&img, R_T0, R_T2, STATUS_OFF);
    emit_mfc0(&img, R_T1, CP0_ERROREPC);
    emit_sd(&img, R_T1, R_T2, ERROREPC_OFF);
    emit_ld(&img, R_T3, R_T2, COUNT_OFF);
    emit_addiu(&img, R_T3, R_T3, 1);
    emit_sd(&img, R_T3, R_T2, COUNT_OFF);
    emit(&img, OPC_SYNC);
    emit(&img, OPC_ERET);
    emit_nop(&img);

    img.pos = MAIN_LOOP;
    emit_lui(&img, R_T2, STATE_KSEG1_HI);
    emit_addiu(&img, R_T3, R_ZERO, READY_MAGIC);
    emit_sd(&img, R_T3, R_T2, READY_OFF);
    emit_ld(&img, R_T3, R_T2, COUNT_OFF);
    emit_sd(&img, R_T3, R_T2, ACK_OFF);
    emit(&img, OPC_SYNC);
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, MAIN_LOOP);
    emit_nop(&img);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static uint64_t state_word(QTestState *qts, uint64_t off)
{
    return qtest_readq(qts, STATE_PHYS + off);
}

static void wait_for_state(QTestState *qts, uint64_t off, uint64_t expected)
{
    /*
     * A generous host-schedule margin, not a guest timing bound: under a
     * loaded runner the guest still needs real wall-clock time to reach
     * the next state. A guest state machine that never advances still
     * fails, just not on host contention alone.
     */
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

    while (state_word(qts, off) != expected) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(POLL_INTERVAL_US);
    }
}

static char *cpu_qom_path(QTestState *qts)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    QList *cpus = qdict_get_qlist(response, "return");
    QDict *cpu = qobject_to(QDict, qlist_peek(cpus));
    char *path = g_strdup(qdict_get_str(cpu, "qom-path"));

    qobject_unref(response);
    return path;
}

static void wait_for_migration(QTestState *from)
{
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(from,
                                             "{'execute':'query-migrate'}");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");

        if (!strcmp(status, "completed")) {
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(POLL_INTERVAL_US);
    }
}

static void test_nmi_entry_and_retrigger(void)
{
    g_autofree char *image = NULL;
    g_autofree char *cpu_path = NULL;
    QTestState *qts;
    uint64_t status, error_epc;
    int fd;

    fd = g_file_open_tmp("sn0-nmi-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", image);
    unlink(image);
    wait_for_state(qts, READY_OFF, READY_MAGIC);
    cpu_path = cpu_qom_path(qts);

    qtest_set_irq_in(qts, cpu_path, "nmi", 0, 1);
    wait_for_state(qts, ACK_OFF, 1);
    status = state_word(qts, STATUS_OFF);
    g_assert_cmphex(status & (STATUS_SR | STATUS_NMI | STATUS_ERL | STATUS_BEV),
                    ==, STATUS_SR | STATUS_NMI | STATUS_ERL | STATUS_BEV);
    error_epc = state_word(qts, ERROREPC_OFF);
    g_assert_cmphex(error_epc, >=, 0xffffffffbfc00000ULL + MAIN_LOOP);
    g_assert_cmphex(error_epc, <, 0xffffffffbfc00000ULL + MAIN_LOOP + 0x20);

    /* A held assertion is one event; lowering it rearms the next edge. */
    qtest_set_irq_in(qts, cpu_path, "nmi", 0, 1);
    g_usleep(10 * POLL_INTERVAL_US);
    g_assert_cmphex(state_word(qts, COUNT_OFF), ==, 1);
    qtest_set_irq_in(qts, cpu_path, "nmi", 0, 0);
    g_assert_cmphex(state_word(qts, COUNT_OFF), ==, 1);
    qtest_set_irq_in(qts, cpu_path, "nmi", 0, 1);
    wait_for_state(qts, ACK_OFF, 2);
    qtest_set_irq_in(qts, cpu_path, "nmi", 0, 0);

    qtest_quit(qts);
}

static void test_nmi_reset_rearms(void)
{
    g_autofree char *image = NULL;
    g_autofree char *cpu_path = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn0-nmi-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    write_guest_image(fd);
    close(fd);
    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-no-shutdown", image);
    unlink(image);
    wait_for_state(qts, READY_OFF, READY_MAGIC);
    cpu_path = cpu_qom_path(qts);

    qtest_set_irq_in(qts, cpu_path, "nmi", 0, 1);
    wait_for_state(qts, ACK_OFF, 1);

    /*
     * QMP system_reset only requests a reset; QEMU applies it at the next
     * main-loop turn, after this command's reply. Stop the vCPU first so it
     * cannot keep spinning through the old MAIN_LOOP and rewriting READY_OFF
     * back to READY_MAGIC in that gap: a poll landing in that window would
     * pass on the pre-reset guest's own write and go on to fire the next
     * NMI before the real reset has run, catching the CPU still at the
     * reset vector instead of parked in the post-reset MAIN_LOOP. cont
     * resumes only once the reset above is known to have been applied.
     */
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    qtest_writeq(qts, STATE_PHYS + READY_OFF, 0);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_state(qts, READY_OFF, READY_MAGIC);
    qtest_set_irq_in(qts, cpu_path, "nmi", 0, 1);
    wait_for_state(qts, ACK_OFF, 2);
    qtest_set_irq_in(qts, cpu_path, "nmi", 0, 0);

    qtest_quit(qts);
}

static void test_nmi_latch_migration(void)
{
    g_autofree char *image = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *source_args = NULL;
    g_autofree char *dest_args = NULL;
    g_autofree char *source_path = NULL;
    g_autofree char *dest_path = NULL;
    GError *error = NULL;
    QTestState *from, *to;
    int fd;

    fd = g_file_open_tmp("sn0-nmi-image-XXXXXX", &image, &error);
    g_assert_no_error(error);
    write_guest_image(fd);
    close(fd);
    directory = g_dir_make_tmp("sn0-nmi-migration-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    source_args = g_strdup_printf(
        "-M origin200 -m 64M -cpu R10000 -bios %s "
        "-accel tcg -no-reboot -no-shutdown", image);
    dest_args = g_strdup_printf(
        "-M origin200 -m 64M -cpu R10000 -bios %s "
        "-accel tcg -no-reboot -no-shutdown -incoming defer", image);
    from = qtest_init(source_args);
    to = qtest_init(dest_args);
    unlink(image);
    wait_for_state(from, READY_OFF, READY_MAGIC);
    source_path = cpu_qom_path(from);
    dest_path = cpu_qom_path(to);
    qtest_set_irq_in(from, source_path, "nmi", 0, 1);
    wait_for_state(from, ACK_OFF, 1);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    wait_for_migration(from);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");

    qtest_set_irq_in(to, dest_path, "nmi", 0, 1);
    g_usleep(10 * POLL_INTERVAL_US);
    g_assert_cmphex(state_word(to, COUNT_OFF), ==, 1);
    qtest_set_irq_in(to, dest_path, "nmi", 0, 0);
    qtest_set_irq_in(to, dest_path, "nmi", 0, 1);
    wait_for_state(to, ACK_OFF, 2);
    qtest_set_irq_in(to, dest_path, "nmi", 0, 0);

    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/nmi/entry-and-retrigger",
                   test_nmi_entry_and_retrigger);
    qtest_add_func("sn0/nmi/reset-rearms", test_nmi_reset_rearms);
    qtest_add_func("sn0/nmi/latch-migration", test_nmi_latch_migration);
    return g_test_run();
}
