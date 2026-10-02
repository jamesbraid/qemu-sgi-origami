/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Test R10000 NMI, Status.RE logging and deterministic LL/SC link breaks
 * independent of Hub and XIO. Origin 200 is this fork's only R10000 machine;
 * firmware is off so each fragment owns the reset vector and low RAM.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-migrate.h"
#include "sgi-mips-emit.h"
#include "sgi-test.h"

/* Guest images share one fixed-size flash window and result area. */
#define IMAGE_SIZE          0x1000
#define STATE_PHYS          0x00100000
#define STATE_KSEG1_HI      0xa010
#define POLL_INTERVAL_US    1000
#define POLL_TRIES          10000
#define R16000_CPU "R16000,big-endian=on,r16000-experimental=on," \
    "r16000-reset-mode=0x3a005,r16000-scache-bytes=4194304"

/*
 * Boot-exception vectors, shared by the status-re and llsc fragments: both
 * install fault stubs so an unexpected exception reports loudly instead of
 * wandering off and looking like a hang.
 */
#define VEC_TLB_REFILL      0x200
#define VEC_XTLB_REFILL     0x280
#define VEC_CACHE_ERROR     0x300
#define VEC_GENERAL         0x380
#define FAULT_MAGIC         0x0bad

/* Instruction encodings, named as in target/mips/tcg/translate.c. */
#define OPC_SPECIAL         (0x00u << 26)
#define OPC_BEQ             (0x04u << 26)
#define OPC_BNE             (0x05u << 26)
#define OPC_SYNC            (0x0fu | OPC_SPECIAL)
#define OPC_ERET            0x42000018u

#define R_ZERO              0
#define R_T0                8
#define R_T1                9
#define R_T2                10
#define R_T3                11

#define CP0_STATUS          12

typedef struct GuestImage {
    uint8_t *code;
    unsigned int pos;
    unsigned int size;
} GuestImage;

static void emit(GuestImage *img, uint32_t insn)
{
    unsigned int size = img->size ? img->size : IMAGE_SIZE;
    g_assert_cmpuint(img->pos + 4, <=, size);
    stl_be_p(img->code + img->pos, insn);
    img->pos += 4;
}

static void emit_nop(GuestImage *img)
{
    emit(img, mips_nop());
}

static void emit_branch(GuestImage *img, uint32_t opcode, int rs, int rt,
                        unsigned int target)
{
    int32_t off = ((int32_t)target - (int32_t)(img->pos + 4)) / 4;

    emit(img, mips_branch(opcode, rs, rt, off));
}

static void emit_lui(GuestImage *img, int rt, uint16_t imm)
{
    emit(img, mips_lui(rt, imm));
}

static void emit_addiu(GuestImage *img, int rt, int rs, int16_t imm)
{
    emit(img, mips_addiu(rt, rs, imm));
}

static void emit_and(GuestImage *img, int rd, int rs, int rt)
{
    emit(img, mips_and(rd, rs, rt));
}

static void emit_ld(GuestImage *img, int rt, int base, int16_t off)
{
    emit(img, mips_ld(rt, base, off));
}

static void emit_sd(GuestImage *img, int rt, int base, int16_t off)
{
    emit(img, mips_sd(rt, base, off));
}

static void emit_mfc0(GuestImage *img, int rt, int rd)
{
    emit(img, mips_mfc0(rt, rd));
}

static void emit_mtc0(GuestImage *img, int rt, int rd)
{
    emit(img, mips_mtc0(rt, rd));
}

/* EHB is SLL $0, $0, 3: a hazard barrier on R2, a no-op before it. */
static void emit_ehb(GuestImage *img)
{
    emit(img, OPC_SPECIAL | (3 << 6));
}

/* Report a value and spin.  The branch needs a delay slot, hence the nop. */
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

/*
 * NMI: exercise the external MIPS NMI input. A tiny guest records the
 * architectural state seen at the boot exception vector, returns through
 * ErrorEPC, and acknowledges each completed entry from its main loop. The NMI
 * line is driven straight on the CPU, so the test stays about target/mips
 * rather than the Hub's interrupt routing.
 */

#define NMI_HANDLER         0x480
#define MAIN_LOOP           0x500

#define READY_OFF           0x00
#define COUNT_OFF           0x08
#define ACK_OFF             0x10
#define STATUS_OFF          0x18
#define ERROREPC_OFF        0x20

#define READY_MAGIC         0x1400

#define CP0_ERROREPC        30
#define STATUS_SR           (1u << 20)
#define STATUS_NMI          (1u << 19)
#define STATUS_ERL          (1u << 2)
#define STATUS_BEV          (1u << 22)

static void nmi_write_guest_image(int fd)
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
     * loaded runner the guest still needs real wall-clock time to reach the
     * next state. A guest state machine that never advances still fails, just
     * not on host contention alone.
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

static void run_nmi_entry_and_retrigger(const char *machine,
                                        const char *cpu)
{
    g_autofree char *image = NULL;
    g_autofree char *cpu_path = NULL;
    QTestState *qts;
    uint64_t status, error_epc;
    int fd;

    fd = g_file_open_tmp("mips64-r10k-nmi-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    nmi_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M %s -m 64M "
                      "-cpu %s -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", machine, cpu, image);
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

static void test_nmi_entry_and_retrigger(void)
{
    run_nmi_entry_and_retrigger("origin200", "R10000");
}

static void test_r16000_nmi_entry_and_retrigger(void)
{
    run_nmi_entry_and_retrigger("malta", R16000_CPU);
}

static void run_nmi_reset_rearms(const char *machine, const char *cpu)
{
    g_autofree char *image = NULL;
    g_autofree char *cpu_path = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("mips64-r10k-nmi-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    nmi_write_guest_image(fd);
    close(fd);
    qts = qtest_initf("-M %s -m 64M "
                      "-cpu %s -bios %s -accel tcg "
                      "-no-shutdown", machine, cpu, image);
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
     * pass on the pre-reset guest's own write and go on to fire the next NMI
     * before the real reset has run, catching the CPU still at the reset
     * vector instead of parked in the post-reset MAIN_LOOP. cont resumes only
     * once the reset above is known to have been applied.
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

static void test_nmi_reset_rearms(void)
{
    run_nmi_reset_rearms("origin200", "R10000");
}

static void test_r16000_nmi_reset_rearms(void)
{
    run_nmi_reset_rearms("malta", R16000_CPU);
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

    fd = g_file_open_tmp("mips64-r10k-nmi-image-XXXXXX", &image, &error);
    g_assert_no_error(error);
    nmi_write_guest_image(fd);
    close(fd);
    directory = g_dir_make_tmp("mips64-r10k-nmi-migration-XXXXXX", &error);
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
    sgi_wait_migration_status(from, "completed", 60);
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

/*
 * status-re: Status.RE promises reverse-endian user-mode loads and stores
 * (user's manual version 2.0, section 14.10). The model stores the bit and
 * implements none of the semantics, so raising it must be loud rather than
 * silent: helper_mtc0_status() logs a LOG_UNIMP line when a guest sets RE on
 * an R10000-family processor. The case boots a fragment that sets RE from
 * kernel mode and reads the -d unimp log back; the line must appear.
 */

#define SRE_DONE_OFF        0x00
#define SRE_FAULT_OFF       0x08
#define SRE_DONE_MAGIC      0x0d04

#define OPC_ORI             (0x0du << 26)
#define OPC_OR              (0x25u | OPC_SPECIAL)

#define STATUS_RE_HI        0x0200  /* bit 25, as a LUI immediate */

static void emit_or(GuestImage *img, int rd, int rs, int rt)
{
    emit(img, OPC_OR | (rs << 21) | (rt << 16) | (rd << 11));
}

/*
 * Any exception says so instead of wandering off and looking like a hang. The
 * fragment runs entirely in kernel mode from flash, so nothing should fault;
 * the stub exists to make it diagnosable if something does.
 */
static void sre_emit_fault_stub(GuestImage *img)
{
    emit_report_and_spin(img, FAULT_MAGIC, SRE_FAULT_OFF);
}

static void sre_write_guest_image(int fd)
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    ssize_t written;

    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, CODE_START);
    emit_nop(&img);

    img.pos = VEC_TLB_REFILL;
    sre_emit_fault_stub(&img);
    img.pos = VEC_XTLB_REFILL;
    sre_emit_fault_stub(&img);
    img.pos = VEC_CACHE_ERROR;
    sre_emit_fault_stub(&img);
    img.pos = VEC_GENERAL;
    sre_emit_fault_stub(&img);

    /*
     * Read Status, raise RE (bit 25), write it back. Reset state - ERL and
     * BEV set - is preserved, so the fragment stays in kernel mode and the
     * write is an ordinary read-modify-write of a live register, the shape a
     * real guest's write would have.
     */
    img.pos = CODE_START;
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_lui(&img, R_T1, STATUS_RE_HI);
    emit_or(&img, R_T0, R_T0, R_T1);
    emit_mtc0(&img, R_T0, CP0_STATUS);
    emit_ehb(&img);
    emit_report_and_spin(&img, SRE_DONE_MAGIC, SRE_DONE_OFF);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

/*
 * Boot the fragment on the R10000 with the unimp log written to a file, wait
 * for the guest's report, and return the log contents.
 */
static char *run_fragment(void)
{
    g_autofree char *image = NULL;
    g_autofree char *log_path = NULL;
    QTestState *qts;
    unsigned int tries;
    uint64_t done = 0, fault = 0;
    char *log = NULL;
    int fd;

    fd = g_file_open_tmp("mips64-r10k-status-re-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    sre_write_guest_image(fd);
    close(fd);

    fd = g_file_open_tmp("mips64-r10k-status-re-log-XXXXXX", &log_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-d unimp -D %s -no-reboot -no-shutdown",
                      image, log_path);
    unlink(image);

    for (tries = 0; tries < POLL_TRIES; tries++) {
        done = qtest_readq(qts, STATE_PHYS + SRE_DONE_OFF);
        fault = qtest_readq(qts, STATE_PHYS + SRE_FAULT_OFF);
        if (done || fault) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(fault, ==, 0);
    g_assert_cmphex(done, ==, SRE_DONE_MAGIC);

    /* Quitting flushes the log file before it is read. */
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    unlink(log_path);
    return log;
}

static void test_r10000_logs_unimp(void)
{
    g_autofree char *log = run_fragment();

    g_assert_nonnull(strstr(log, "Status.RE"));
}

/*
 * llsc: the R10000's deterministic load-linked link breakers. The processor
 * requires SC to fail if an LL/SC sequence contains another load or store,
 * SYNC, CACHE, PREF, an exception, or ERET.
 */

#define RESULT_BASE         0x00
#define RESULT_STRIDE       0x10
#define TARGET_OFF          0x100
#define OTHER_OFF           0x108
#define LLSC_DONE_OFF       0x1f0
#define EXC_COUNT_OFF       0x1f8
#define LLSC_FAULT_OFF      0x200

#define OLD_VALUE           0x11
#define NEW_VALUE           0x22
#define OTHER_VALUE         0x44
#define LLSC_DONE_MAGIC     0x10c5

#define OPC_CACHE           (0x2fu << 26)
#define OPC_PREF            (0x33u << 26)
#define OPC_LLD             (0x34u << 26)
#define OPC_SCD             (0x3cu << 26)
#define OPC_JR              (0x08u | OPC_SPECIAL)
#define OPC_SYSCALL         (0x0cu | OPC_SPECIAL)

#define R_S0                16
#define R_S6                22
#define R_S7                23
#define R_K0                26

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

static void emit_lld(GuestImage *img, int rt, int base, int16_t off)
{
    emit(img, OPC_LLD | (base << 21) | (rt << 16) | (uint16_t)off);
}

static void emit_scd(GuestImage *img, int rt, int base, int16_t off)
{
    emit(img, OPC_SCD | (base << 21) | (rt << 16) | (uint16_t)off);
}

static void llsc_emit_fault_stub(GuestImage *img)
{
    emit_report_and_spin(img, FAULT_MAGIC, LLSC_FAULT_OFF);
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

static void llsc_write_guest_image(int fd, bool has_pref)
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    ssize_t written;

    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, CODE_START);
    emit_nop(&img);

    img.pos = VEC_TLB_REFILL;
    llsc_emit_fault_stub(&img);
    img.pos = VEC_XTLB_REFILL;
    llsc_emit_fault_stub(&img);
    img.pos = VEC_CACHE_ERROR;
    llsc_emit_fault_stub(&img);
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
    emit_report_and_spin(&img, LLSC_DONE_MAGIC, LLSC_DONE_OFF);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static void wait_for_done(QTestState *qts)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + LLSC_DONE_OFF);
        uint64_t fault = qtest_readq(qts, STATE_PHYS + LLSC_FAULT_OFF);

        if (done || fault) {
            g_assert_cmphex(fault, ==, 0);
            g_assert_cmphex(done, ==, LLSC_DONE_MAGIC);
            return;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + LLSC_DONE_OFF), ==,
                    LLSC_DONE_MAGIC);
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

    fd = g_file_open_tmp("mips64-r10k-llsc-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    llsc_write_guest_image(fd, cpu_case->has_pref);
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

/* ---------------------------------------------------------------- */
/* Indirect branch cache and unmapped cross-page chaining           */
/* ---------------------------------------------------------------- */

#define CHAIN_IMAGE_SIZE        0x2000
#define CHAIN_DONE_MAGIC        0x5afe1000
#define CHAIN_PAGE1_TARGET      0x1020
#define CHAIN_PAGE0_TARGET      0x200

#define CHAIN_SUB_A_OFF         0x300
#define CHAIN_SUB_B_OFF         0x320
#define CHAIN_SUB_C_OFF         0x340

#define CHAIN_DONE_OFF          0x380
#define CHAIN_COUNT_A_OFF       0x388
#define CHAIN_COUNT_B_OFF       0x390
#define CHAIN_COUNT_C_OFF       0x398
#define CHAIN_FAULT_OFF         0x3a0

#define OPC_JALR                (0x09u | OPC_SPECIAL)
#define R_A0                    4
#define R_A1                    5
#define R_S1                    17
#define R_S2                    18
#define R_RA                    31

static void emit_jr(GuestImage *img, int rs)
{
    emit(img, OPC_JR | (rs << 21));
}

static void emit_jalr(GuestImage *img, int rd, int rs)
{
    emit(img, OPC_JALR | (rs << 21) | (rd << 11));
}

static void chain_write_guest_image(int fd)
{
    uint8_t *image = g_malloc0(CHAIN_IMAGE_SIZE);
    GuestImage img = { .code = image, .pos = 0, .size = CHAIN_IMAGE_SIZE };
    ssize_t written;

    /* Fault handlers at standard vectors */
    img.pos = VEC_TLB_REFILL;
    emit_report_and_spin(&img, FAULT_MAGIC, CHAIN_FAULT_OFF);
    img.pos = VEC_XTLB_REFILL;
    emit_report_and_spin(&img, FAULT_MAGIC, CHAIN_FAULT_OFF);
    img.pos = VEC_CACHE_ERROR;
    emit_report_and_spin(&img, FAULT_MAGIC, CHAIN_FAULT_OFF);
    img.pos = VEC_GENERAL;
    emit_report_and_spin(&img, FAULT_MAGIC, CHAIN_FAULT_OFF);

    /*
     * 1. Subroutines in Page 0:
     * sub_a at 0x300: $s0++
     * sub_b at 0x320: $s1++
     * sub_c at 0x340: $s2++
     */
    img.pos = CHAIN_SUB_A_OFF;
    emit_addiu(&img, R_S0, R_S0, 1);
    emit_jr(&img, R_RA);
    emit_nop(&img);

    img.pos = CHAIN_SUB_B_OFF;
    emit_addiu(&img, R_S1, R_S1, 1);
    emit_jr(&img, R_RA);
    emit_nop(&img);

    img.pos = CHAIN_SUB_C_OFF;
    emit_addiu(&img, R_S2, R_S2, 1);
    emit_jr(&img, R_RA);
    emit_nop(&img);

    /*
     * 2. Page 1 target at 0x1020:
     * Reached via cross-page branch from Page 0 in unmapped KSEG1.
     * Branches BACK to Page 0 at CHAIN_PAGE0_TARGET (0x200).
     */
    img.pos = CHAIN_PAGE1_TARGET;
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, CHAIN_PAGE0_TARGET);
    emit_nop(&img);

    /*
     * 3. Reset Vector at 0x000:
     * Clears counters ($s0, $s1, $s2 = 0).
     * Branches forward across page boundary to Page 1 at CHAIN_PAGE1_TARGET.
     */
    img.pos = 0;
    emit_addiu(&img, R_S0, R_ZERO, 0);
    emit_addiu(&img, R_S1, R_ZERO, 0);
    emit_addiu(&img, R_S2, R_ZERO, 0);
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, CHAIN_PAGE1_TARGET);
    emit_nop(&img);

    /*
     * 4. Page 0 loop body at CHAIN_PAGE0_TARGET (0x200):
     * Returned from Page 1 via cross-page goto_tb.
     *
     * Loop 1: Call sub_a 50 times (repeated returns with same target).
     * Loop 2: Call sub_a, sub_b, sub_c 50 times (alternating indirect targets).
     */
    img.pos = CHAIN_PAGE0_TARGET;

    emit_lui(&img, R_A0, 0xbfc0);
    emit_addiu(&img, R_A0, R_A0, CHAIN_SUB_A_OFF);

    emit_lui(&img, R_A1, 0xbfc0);
    emit_addiu(&img, R_A1, R_A1, CHAIN_SUB_B_OFF);

    emit_lui(&img, R_T1, 0xbfc0);
    emit_addiu(&img, R_T1, R_T1, CHAIN_SUB_C_OFF);

    /* Loop 1: 50 calls to sub_a */
    emit_addiu(&img, R_T2, R_ZERO, 50);
    unsigned int loop1_start = img.pos;
    emit_jalr(&img, R_RA, R_A0);
    emit_nop(&img);
    emit_addiu(&img, R_T2, R_T2, -1);
    emit_branch(&img, OPC_BNE, R_T2, R_ZERO, loop1_start);
    emit_nop(&img);

    /* Loop 2: 50 iterations calling sub_a, sub_b, sub_c in order */
    emit_addiu(&img, R_T2, R_ZERO, 50);
    unsigned int loop2_start = img.pos;
    emit_jalr(&img, R_RA, R_A0);
    emit_nop(&img);
    emit_jalr(&img, R_RA, R_A1);
    emit_nop(&img);
    emit_jalr(&img, R_RA, R_T1);
    emit_nop(&img);
    emit_addiu(&img, R_T2, R_T2, -1);
    emit_branch(&img, OPC_BNE, R_T2, R_ZERO, loop2_start);
    emit_nop(&img);

    /* Write final results to STATE_PHYS */
    emit_lui(&img, R_T3, STATE_KSEG1_HI);
    emit_sd(&img, R_S0, R_T3, CHAIN_COUNT_A_OFF);
    emit_sd(&img, R_S1, R_T3, CHAIN_COUNT_B_OFF);
    emit_sd(&img, R_S2, R_T3, CHAIN_COUNT_C_OFF);

    /* Publish DONE magic and spin */
    emit_lui(&img, R_T2, (uint16_t)(CHAIN_DONE_MAGIC >> 16));
    emit_addiu(&img, R_T2, R_T2, (int16_t)(CHAIN_DONE_MAGIC & 0xffff));
    emit_sd(&img, R_T2, R_T3, CHAIN_DONE_OFF);
    emit(&img, OPC_SYNC);

    unsigned int spin = img.pos;
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, spin);
    emit_nop(&img);

    written = write(fd, image, CHAIN_IMAGE_SIZE);
    g_assert_cmpint(written, ==, CHAIN_IMAGE_SIZE);
    g_free(image);
}

static void test_indirect_and_unmapped_chaining(void)
{
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("mips64-r10k-chain-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    chain_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", image);
    unlink(image);

    for (int i = 0; i < POLL_TRIES; i++) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + CHAIN_DONE_OFF);
        uint64_t fault = qtest_readq(qts, STATE_PHYS + CHAIN_FAULT_OFF);

        if (done || fault) {
            g_assert_cmphex(fault, ==, 0);
            g_assert_cmphex(done, ==, (uint64_t)CHAIN_DONE_MAGIC);
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }

    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + CHAIN_DONE_OFF), ==,
                    (uint64_t)CHAIN_DONE_MAGIC);
    g_assert_cmpuint(qtest_readq(qts, STATE_PHYS + CHAIN_COUNT_A_OFF), ==, 100);
    g_assert_cmpuint(qtest_readq(qts, STATE_PHYS + CHAIN_COUNT_B_OFF), ==, 50);
    g_assert_cmpuint(qtest_readq(qts, STATE_PHYS + CHAIN_COUNT_C_OFF), ==, 50);

    qtest_quit(qts);
}

#define PREF_IMAGE_SIZE     0x1000
#define PREF_DONE_OFF       0x20
#define PREF_FAULT_OFF      0x28
#define PREF_DONE_MAGIC     0x50524546 /* "PREF" */

static void pref_write_guest_image(int fd)
{
    uint8_t *image = g_malloc0(PREF_IMAGE_SIZE);
    GuestImage img = { .code = image, .pos = 0, .size = PREF_IMAGE_SIZE };
    ssize_t written;

    /* Fault handlers at standard vectors */
    img.pos = VEC_TLB_REFILL;
    emit_report_and_spin(&img, FAULT_MAGIC, PREF_FAULT_OFF);
    img.pos = VEC_XTLB_REFILL;
    emit_report_and_spin(&img, FAULT_MAGIC, PREF_FAULT_OFF);
    img.pos = VEC_CACHE_ERROR;
    emit_report_and_spin(&img, FAULT_MAGIC, PREF_FAULT_OFF);
    img.pos = VEC_GENERAL;
    emit_report_and_spin(&img, FAULT_MAGIC, PREF_FAULT_OFF);

    img.pos = 0;
    /* Clear ERL and enable CU1 for PREFX (COP1X) */
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_addiu(&img, R_T1, R_ZERO, STATUS_ERL_CLEAR);
    emit_and(&img, R_T0, R_T0, R_T1);
    emit_lui(&img, R_T1, 0x2000); /* Status.CU1 (bit 29) */
    emit(&img, 0x25u | (R_T0 << 21) | (R_T1 << 16) | (R_T0 << 11)); /* or */
    emit_mtc0(&img, R_T0, CP0_STATUS);
    emit_ehb(&img);

    emit_lui(&img, R_T0, STATE_KSEG1_HI);
    emit_addiu(&img, R_T1, R_ZERO, 0x100);

    /* Test PREF and PREFX with all 32 hints and both offset directions. */
    for (int hint = 0; hint < 32; hint++) {
        emit(&img, OPC_PREF | (R_T0 << 21) | (hint << 16) | 0x0040);
        emit(&img, OPC_PREF | (R_T0 << 21) | (hint << 16) | (uint16_t)-64);
        uint32_t prefx = (0x13u << 26) | (R_T0 << 21) | (R_T1 << 16) |
                         (hint << 11) | 0x0fu;
        emit(&img, prefx);
    }

    emit_lui(&img, R_T2, (uint16_t)(PREF_DONE_MAGIC >> 16));
    emit_addiu(&img, R_T2, R_T2, (int16_t)(PREF_DONE_MAGIC & 0xffff));
    emit_sd(&img, R_T2, R_T0, PREF_DONE_OFF);
    emit(&img, OPC_SYNC);

    unsigned int spin = img.pos;
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, spin);
    emit_nop(&img);

    written = write(fd, image, PREF_IMAGE_SIZE);
    g_assert_cmpint(written, ==, PREF_IMAGE_SIZE);
    g_free(image);
}

static void test_r10k_prefetch_hints(void)
{
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("mips64-r10k-pref-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    pref_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", image);
    unlink(image);

    for (int i = 0; i < POLL_TRIES; i++) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + PREF_DONE_OFF);
        uint64_t fault = qtest_readq(qts, STATE_PHYS + PREF_FAULT_OFF);

        if (done || fault) {
            g_assert_cmphex(fault, ==, 0);
            g_assert_cmphex(done, ==, (uint64_t)PREF_DONE_MAGIC);
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }

    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + PREF_DONE_OFF), ==,
                    (uint64_t)PREF_DONE_MAGIC);

    qtest_quit(qts);
}

#define TLB_IMAGE_SIZE      0x1000
#define TLB_DONE_OFF        0x30
#define TLB_FAULT_OFF       0x38
#define TLB_PAGE0_OFF       0x40
#define TLB_PAGE1_OFF       0x48
#define TLB_PAGE2_OFF       0x50
#define TLB_PAGE3_OFF       0x58
#define TLB_DONE_MAGIC      0x544c424d /* "TLBM" */

#define CP0_INDEX           0
#define CP0_ENTRYLO0        2
#define CP0_ENTRYLO1        3
#define CP0_PAGEMASK        5
#define CP0_ENTRYHI         10
#define OPC_TLBWI           0x42000002u

#define OPC_ORI             (0x0du << 26)

static void emit_ori(GuestImage *img, int rt, int rs, uint16_t imm)
{
    emit(img, OPC_ORI | (rs << 21) | (rt << 16) | imm);
}

static void emit_fault_handler(GuestImage *img, uint16_t vec_id)
{
    emit_lui(img, R_T2, STATE_KSEG1_HI);
    emit_addiu(img, R_T3, R_ZERO, vec_id);
    emit_sd(img, R_T3, R_T2, TLB_FAULT_OFF);
    emit_mfc0(img, R_T3, 13); /* CP0_CAUSE */
    emit_sd(img, R_T3, R_T2, TLB_FAULT_OFF + 8);
    emit_mfc0(img, R_T3, 8);  /* CP0_BADVADDR */
    emit_sd(img, R_T3, R_T2, TLB_FAULT_OFF + 16);
    emit_mfc0(img, R_T3, 14); /* CP0_EPC */
    emit_sd(img, R_T3, R_T2, TLB_FAULT_OFF + 24);
    emit(img, OPC_SYNC);
    unsigned int spin = img->pos;
    emit_branch(img, OPC_BEQ, R_ZERO, R_ZERO, spin);
    emit_nop(img);
}

static void emit_tlb_write_entry(GuestImage *img, int index, uint32_t vpn2,
                                 uint32_t pfn0, uint32_t pfn1)
{
    emit_lui(img, R_T0, (uint16_t)(vpn2 >> 16));
    emit_ori(img, R_T0, R_T0, (uint16_t)(vpn2 & 0xffff));
    emit_mtc0(img, R_T0, CP0_ENTRYHI);
    emit_ehb(img);

    uint32_t lo0 = (pfn0 << 6) | 0x17u;
    emit_lui(img, R_T0, (uint16_t)(lo0 >> 16));
    emit_ori(img, R_T0, R_T0, (uint16_t)(lo0 & 0xffff));
    emit_mtc0(img, R_T0, CP0_ENTRYLO0);

    uint32_t lo1 = (pfn1 << 6) | 0x17u;
    emit_lui(img, R_T0, (uint16_t)(lo1 >> 16));
    emit_ori(img, R_T0, R_T0, (uint16_t)(lo1 & 0xffff));
    emit_mtc0(img, R_T0, CP0_ENTRYLO1);

    emit_addiu(img, R_T0, R_ZERO, index);
    emit_mtc0(img, R_T0, CP0_INDEX);
    emit_ehb(img);

    emit(img, OPC_TLBWI);
    emit_ehb(img);
}

static void tlb_write_guest_image(int fd)
{
    uint8_t *image = g_malloc0(TLB_IMAGE_SIZE);
    GuestImage img = { .code = image, .pos = 0, .size = TLB_IMAGE_SIZE };
    ssize_t written;

    img.pos = VEC_TLB_REFILL;
    emit_fault_handler(&img, 1);
    img.pos = VEC_XTLB_REFILL;
    emit_fault_handler(&img, 2);
    img.pos = VEC_CACHE_ERROR;
    emit_fault_handler(&img, 3);
    img.pos = VEC_GENERAL;
    emit_fault_handler(&img, 4);

    img.pos = 0;
    /* Clear ERL so low segment is TLB-mapped */
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_addiu(&img, R_T1, R_ZERO, STATUS_ERL_CLEAR);
    emit_and(&img, R_T0, R_T0, R_T1);
    emit_mtc0(&img, R_T0, CP0_STATUS);
    emit_ehb(&img);

    emit_mtc0(&img, R_ZERO, CP0_PAGEMASK);
    emit_ehb(&img);

    /* Program 4 TLB entries with different VPNs (hash buckets 16, 1, 17, 2) */
    emit_tlb_write_entry(&img, 0, 0x00020000, 0x200, 0x201);
    emit_tlb_write_entry(&img, 1, 0x00040000, 0x202, 0x203);
    emit_tlb_write_entry(&img, 2, 0x00060000, 0x204, 0x205);
    emit_tlb_write_entry(&img, 3, 0x00080000, 0x206, 0x207);

    /* A0 = 0x20000, A1 = 0x40000, A2 = 0x60000, A3 = 0x80000 */
    emit_lui(&img, 4, 0x0002);
    emit_lui(&img, 5, 0x0004);
    emit_lui(&img, 6, 0x0006);
    emit_lui(&img, 7, 0x0008);

    /* Iteration counter in T1 = 50 */
    emit_addiu(&img, R_T1, R_ZERO, 50);
    emit_addiu(&img, R_T2, R_ZERO, 0x1234);

    unsigned int loop_start = img.pos;
    /* Interleaved writes across all 4 mapped pages */
    emit_sd(&img, R_T2, 4, 0);
    emit_sd(&img, R_T2, 5, 0);
    emit_sd(&img, R_T2, 6, 0);
    emit_sd(&img, R_T2, 7, 0);

    /* Interleaved reads across all 4 mapped pages */
    emit_ld(&img, 16, 4, 0); /* S0 */
    emit_ld(&img, 17, 5, 0); /* S1 */
    emit_ld(&img, 18, 6, 0); /* S2 */
    emit_ld(&img, 19, 7, 0); /* S3 */

    emit_addiu(&img, R_T2, R_T2, 1);
    emit_addiu(&img, R_T1, R_T1, -1);
    emit_branch(&img, OPC_BNE, R_T1, R_ZERO, loop_start);
    emit_nop(&img);

    /* Write final read values to STATE_PHYS */
    emit_lui(&img, R_T0, STATE_KSEG1_HI);
    emit_sd(&img, 16, R_T0, TLB_PAGE0_OFF);
    emit_sd(&img, 17, R_T0, TLB_PAGE1_OFF);
    emit_sd(&img, 18, R_T0, TLB_PAGE2_OFF);
    emit_sd(&img, 19, R_T0, TLB_PAGE3_OFF);

    emit_lui(&img, R_T2, (uint16_t)(TLB_DONE_MAGIC >> 16));
    emit_addiu(&img, R_T2, R_T2, (int16_t)(TLB_DONE_MAGIC & 0xffff));
    emit_sd(&img, R_T2, R_T0, TLB_DONE_OFF);
    emit(&img, OPC_SYNC);

    unsigned int spin = img.pos;
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, spin);
    emit_nop(&img);

    written = write(fd, image, TLB_IMAGE_SIZE);
    g_assert_cmpint(written, ==, TLB_IMAGE_SIZE);
    g_free(image);
}

static void test_r10k_tlb_interleaved(void)
{
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("mips64-r10k-tlb-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    tlb_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", image);
    unlink(image);

    for (int i = 0; i < POLL_TRIES; i++) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + TLB_DONE_OFF);
        uint64_t fault = qtest_readq(qts, STATE_PHYS + TLB_FAULT_OFF);

        if (done || fault) {
            g_assert_cmphex(fault, ==, 0);
            g_assert_cmphex(done, ==, (uint64_t)TLB_DONE_MAGIC);
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }

    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + TLB_DONE_OFF), ==,
                    (uint64_t)TLB_DONE_MAGIC);
    uint64_t expected = 0x1234 + 49;
    g_assert_cmpuint(qtest_readq(qts, STATE_PHYS + TLB_PAGE0_OFF),
                     ==, expected);
    g_assert_cmpuint(qtest_readq(qts, STATE_PHYS + TLB_PAGE1_OFF),
                     ==, expected);
    g_assert_cmpuint(qtest_readq(qts, STATE_PHYS + TLB_PAGE2_OFF),
                     ==, expected);
    g_assert_cmpuint(qtest_readq(qts, STATE_PHYS + TLB_PAGE3_OFF),
                     ==, expected);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sn0/nmi/entry-and-retrigger",
                   test_nmi_entry_and_retrigger);
    qtest_add_func("/sn0/nmi/reset-rearms", test_nmi_reset_rearms);
    qtest_add_func("/r16000/nmi/entry-and-retrigger",
                   test_r16000_nmi_entry_and_retrigger);
    qtest_add_func("/r16000/nmi/cold-reset-control",
                   test_r16000_nmi_reset_rearms);
    qtest_add_func("/sn0/nmi/latch-migration", test_nmi_latch_migration);
    qtest_add_func("/status-re/r10000-logs-unimp", test_r10000_logs_unimp);
    g_test_add_data_func("/r10k-llsc/r10000", &cpu_cases[0], test_cpu);
    qtest_add_func("/sn0/tcg/indirect-and-unmapped-chaining",
                   test_indirect_and_unmapped_chaining);
    qtest_add_func("/sn0/tcg/prefetch-hints",
                   test_r10k_prefetch_hints);
    qtest_add_func("/sn0/tcg/tlb-interleaved",
                   test_r10k_tlb_interleaved);

    return g_test_run();
}
