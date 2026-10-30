/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Test R10000 NMI, Status.RE logging and deterministic LL/SC link breaks
 * independent of Hub and XIO. Origin 200 is this fork's only R10000 machine;
 * firmware is off so each fragment owns the reset vector and low RAM.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "libqtest.h"
#include "sgi-migrate.h"
#include "sgi-mips-emit.h"
#include "sgi-test.h"
#include "sgi-wait.h"

/* Guest images share one fixed-size flash window and result area. */
#define IMAGE_SIZE          0x1000
#define STATE_PHYS          0x00100000
#define STATE_KSEG1_HI      0xa010
#define POLL_INTERVAL_US    1000
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
#define STATUS_TS           (1u << 21)
#define STATUS_KX           (1u << 7)

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
    SgiWait w;

    sgi_wait_start(&w, "guest state 0x%" PRIx64 " at 0x%" PRIx64,
                   expected, off);
    while (state_word(qts, off) != expected) {
        sgi_wait_pause(&w, POLL_INTERVAL_US);
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
    sgi_wait_migration(from, "completed");
    sgi_migrate_resume(to);

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
    SgiWait w;
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

    sgi_wait_start(&w, "guest done or fault");
    for (;;) {
        done = qtest_readq(qts, STATE_PHYS + SRE_DONE_OFF);
        fault = qtest_readq(qts, STATE_PHYS + SRE_FAULT_OFF);
        if (done || fault) {
            break;
        }
        sgi_wait_pause(&w, POLL_INTERVAL_US);
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
 * llsc: pins the R10000 family to QEMU's common LL/SC model. The link breaks
 * on exception entry (the handler returns with JR, not ERET) and on ERET, so
 * SC fails after either. An intervening load, store, SYNC, CACHE or PREF
 * leaves the link alone and SC succeeds, as on QEMU's other LL/SC targets,
 * although the R10000 User's Manual says SC fails after each of them.
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
    const char *path;
    const char *machine;
    const char *cpu;
    bool has_pref;
    uint32_t success_mask;
} CPUCase;

#define LLSC_COMMON_SUCCESS_MASK \
    ((1u << BREAK_LOAD) | (1u << BREAK_STORE) | (1u << BREAK_SYNC) | \
     (1u << BREAK_CACHE) | (1u << BREAK_PREF))

static const CPUCase cpu_cases[] = {
    {
        "/r10k-llsc/r10000", "origin200", "R10000", true,
        LLSC_COMMON_SUCCESS_MASK,
    },
    {
        "/r10k-llsc/r12000", "malta",
        "R12000,r12000-prid=0xe12,r12000-fpu-id=0x4567,"
        "r12000-reset-mode=0x198683,r12000-scache-bytes=1048576,"
        "r12000-scache-block-words=16",
        true, LLSC_COMMON_SUCCESS_MASK,
    },
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
    SgiWait w;

    sgi_wait_start(&w, "guest done or fault");
    for (;;) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + LLSC_DONE_OFF);
        uint64_t fault = qtest_readq(qts, STATE_PHYS + LLSC_FAULT_OFF);

        if (done || fault) {
            g_assert_cmphex(fault, ==, 0);
            g_assert_cmphex(done, ==, LLSC_DONE_MAGIC);
            return;
        }
        sgi_wait_pause(&w, POLL_INTERVAL_US);
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

    qts = qtest_initf("-M %s -m 64M "
                      "-cpu %s -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", cpu_case->machine,
                      cpu_case->cpu, image);
    unlink(image);
    wait_for_done(qts);

    for (LinkBreaker breaker = 0; breaker < BREAK_COUNT; breaker++) {
        uint64_t result = qtest_readq(qts, STATE_PHYS + RESULT_BASE +
                                      breaker * RESULT_STRIDE);
        uint64_t value = qtest_readq(qts, STATE_PHYS + RESULT_BASE +
                                     breaker * RESULT_STRIDE + 8);
        uint64_t expected_value;

        g_test_message("%s after %s", cpu_case->cpu,
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

/* A BREAK must clear either reservation before the general handler's SC. */
#define R16_LLSC_TARGET_REG        17
#define R16_LLSC_TARGET_ADDR       0x1000
#define R16_LLSC_BASELINE_RESULT   0x300
#define R16_LLSC_BASELINE_VALUE    0x308
#define R16_LLSC_HANDLER_STATUS   0x310
#define R16_LLSC_HANDLER_CAUSE    0x318
#define R16_LLSC_HANDLER_RESULT   0x320
#define R16_LLSC_HANDLER_VALUE    0x328
#define R16_LLSC_INITIAL_STATUS   0x330
#define R16_LLSC_DONE_OFF         0x338
#define R16_LLSC_FAULT_OFF        0x340
#define R16_LLSC_DONE_MAGIC       0x5343
#define OPC_LL                    (0x30u << 26)
#define OPC_SC                    (0x38u << 26)
#define OPC_BREAK                 (0x0du | OPC_SPECIAL)
#define R16_LLSC_CP0_CAUSE        13
#define R16_LLSC_CAUSE_BREAK      (9u << 2)
#define R16_LLSC_CAUSE_CODE_MASK  (0x1fu << 2)
#define R16_LLSC_STATUS_EXL       (1u << 1)

typedef struct R16000LLSCCase {
    const char *path;
    bool doubleword;
} R16000LLSCCase;

static const R16000LLSCCase r16000_llsc_cases[] = {
    { "/r16000/llsc/sc-after-break/word", false },
    { "/r16000/llsc/sc-after-break/doubleword", true },
};

static void emit_r16000_ll(GuestImage *img, bool doubleword, int rt, int off)
{
    uint32_t opcode = doubleword ? OPC_LLD : OPC_LL;

    emit(img, opcode | (R16_LLSC_TARGET_REG << 21) | (rt << 16) |
         (uint16_t)off);
}

static void emit_r16000_sc(GuestImage *img, bool doubleword, int rt, int off)
{
    uint32_t opcode = doubleword ? OPC_SCD : OPC_SC;

    emit(img, opcode | (R16_LLSC_TARGET_REG << 21) | (rt << 16) |
         (uint16_t)off);
}

static void emit_r16000_llsc_store(GuestImage *img, bool doubleword, int rt,
                                   int off)
{
    if (doubleword) {
        emit_sd(img, rt, R16_LLSC_TARGET_REG, off);
    } else {
        emit(img, MIPS_OPC_SW | (R16_LLSC_TARGET_REG << 21) |
             (rt << 16) | (uint16_t)off);
    }
}

static void emit_r16000_llsc_load(GuestImage *img, bool doubleword, int rt,
                                  int off)
{
    if (doubleword) {
        emit_ld(img, rt, R16_LLSC_TARGET_REG, off);
    } else {
        emit(img, MIPS_OPC_LW | (R16_LLSC_TARGET_REG << 21) |
             (rt << 16) | (uint16_t)off);
    }
}

static void r16000_llsc_write_guest_image(int fd, bool doubleword)
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
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_sd(&img, R_T0, R_S0, R16_LLSC_HANDLER_STATUS);
    emit_mfc0(&img, R_T0, R16_LLSC_CP0_CAUSE);
    emit_sd(&img, R_T0, R_S0, R16_LLSC_HANDLER_CAUSE);
    emit_r16000_sc(&img, doubleword, R_T1, 0);
    emit_sd(&img, R_T1, R_S0, R16_LLSC_HANDLER_RESULT);
    emit_r16000_llsc_load(&img, doubleword, R_T0, 0);
    emit_sd(&img, R_T0, R_S0, R16_LLSC_HANDLER_VALUE);
    emit_report_and_spin(&img, R16_LLSC_DONE_MAGIC, R16_LLSC_DONE_OFF);

    img.pos = CODE_START;
    emit_lui(&img, R_S0, STATE_KSEG1_HI);
    emit_lui(&img, R16_LLSC_TARGET_REG, 0x8000);
    emit_addiu(&img, R16_LLSC_TARGET_REG, R16_LLSC_TARGET_REG,
               R16_LLSC_TARGET_ADDR);
    emit_clear_status_bit(&img, STATUS_ERL_CLEAR);
    emit_clear_status_bit(&img, STATUS_EXL_CLEAR);
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_sd(&img, R_T0, R_S0, R16_LLSC_INITIAL_STATUS);

    emit_addiu(&img, R_T0, R_ZERO, OLD_VALUE);
    emit_r16000_llsc_store(&img, doubleword, R_T0, 0);
    emit_addiu(&img, R_T1, R_ZERO, NEW_VALUE);
    emit_r16000_ll(&img, doubleword, R_T2, 0);
    emit_r16000_sc(&img, doubleword, R_T1, 0);
    emit_sd(&img, R_T1, R_S0, R16_LLSC_BASELINE_RESULT);
    emit_r16000_llsc_load(&img, doubleword, R_T0, 0);
    emit_sd(&img, R_T0, R_S0, R16_LLSC_BASELINE_VALUE);

    emit_addiu(&img, R_T0, R_ZERO, OLD_VALUE);
    emit_r16000_llsc_store(&img, doubleword, R_T0, 0);
    emit_addiu(&img, R_T1, R_ZERO, NEW_VALUE);
    emit_r16000_ll(&img, doubleword, R_T2, 0);
    emit(&img, OPC_BREAK);
    emit_addiu(&img, R_T0, R_ZERO, FAULT_MAGIC);
    emit_sd(&img, R_T0, R_S0, R16_LLSC_FAULT_OFF);
    emit_report_and_spin(&img, R16_LLSC_DONE_MAGIC, R16_LLSC_DONE_OFF);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static void test_r16000_llsc_sc_in_exception(gconstpointer opaque)
{
    const R16000LLSCCase *test_case = opaque;
    g_autofree char *image = NULL;
    QTestState *qts;
    uint64_t status, cause;
    int fd;

    fd = g_file_open_tmp("mips64-r16000-llsc-exception-XXXXXX", &image,
                         NULL);
    g_assert_cmpint(fd, >=, 0);
    r16000_llsc_write_guest_image(fd, test_case->doubleword);
    close(fd);

    qts = qtest_initf("-M malta -m 64M -cpu %s -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", R16000_CPU, image);
    unlink(image);
    wait_for_state(qts, R16_LLSC_DONE_OFF, R16_LLSC_DONE_MAGIC);

    status = state_word(qts, R16_LLSC_INITIAL_STATUS);
    g_assert_cmphex(status & (STATUS_ERL | R16_LLSC_STATUS_EXL), ==, 0);
    g_assert_cmphex(status & STATUS_BEV, ==, STATUS_BEV);
    g_assert_cmphex(state_word(qts, R16_LLSC_BASELINE_RESULT), ==, 1);
    g_assert_cmphex(state_word(qts, R16_LLSC_BASELINE_VALUE), ==, NEW_VALUE);

    status = state_word(qts, R16_LLSC_HANDLER_STATUS);
    g_assert_cmphex(status & R16_LLSC_STATUS_EXL, ==,
                    R16_LLSC_STATUS_EXL);
    cause = state_word(qts, R16_LLSC_HANDLER_CAUSE);
    g_assert_cmphex(cause & R16_LLSC_CAUSE_CODE_MASK, ==,
                    R16_LLSC_CAUSE_BREAK);
    g_assert_cmphex(state_word(qts, R16_LLSC_HANDLER_RESULT), ==, 0);
    g_assert_cmphex(state_word(qts, R16_LLSC_HANDLER_VALUE), ==, OLD_VALUE);
    g_assert_cmphex(state_word(qts, R16_LLSC_FAULT_OFF), ==, 0);

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
    SgiWait w;

    fd = g_file_open_tmp("mips64-r10k-chain-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    chain_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", image);
    unlink(image);

    sgi_wait_start(&w, "guest done or fault");
    for (;;) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + CHAIN_DONE_OFF);
        uint64_t fault = qtest_readq(qts, STATE_PHYS + CHAIN_FAULT_OFF);

        if (done || fault) {
            g_assert_cmphex(fault, ==, 0);
            g_assert_cmphex(done, ==, (uint64_t)CHAIN_DONE_MAGIC);
            break;
        }
        sgi_wait_pause(&w, POLL_INTERVAL_US);
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
    SgiWait w;

    fd = g_file_open_tmp("mips64-r10k-pref-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    pref_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", image);
    unlink(image);

    sgi_wait_start(&w, "guest done or fault");
    for (;;) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + PREF_DONE_OFF);
        uint64_t fault = qtest_readq(qts, STATE_PHYS + PREF_FAULT_OFF);

        if (done || fault) {
            g_assert_cmphex(fault, ==, 0);
            g_assert_cmphex(done, ==, (uint64_t)PREF_DONE_MAGIC);
            break;
        }
        sgi_wait_pause(&w, POLL_INTERVAL_US);
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


static void emit_li64(GuestImage *img, int rt, uint64_t value)
{
    emit_ori(img, rt, R_ZERO, value >> 48);
    for (int shift = 32; shift >= 0; shift -= 16) {
        emit(img, OPC_SPECIAL | (rt << 16) | (rt << 11) | (16 << 6) | 0x38);
        emit_ori(img, rt, rt, value >> shift);
    }
}

static void test_unmapped_cpu_relative_destination(void)
{
    g_autofree char *path = NULL;
    uint8_t image[IMAGE_SIZE] = { 0 };
    uint8_t source[8] = { 0 };
    uint8_t dest[16] = { 0 };
    GuestImage img = { .code = image };
    QTestState *qts;
    int fd = g_file_open_tmp("r10k-cpu-relative-XXXXXX", &path, NULL);
    SgiWait w;

    g_assert_cmpint(fd, >=, 0);
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_ori(&img, R_T0, R_T0, 0x80);
    emit_mtc0(&img, R_T0, CP0_STATUS);
    for (int i = 0; i < 4; i++) {
        emit_nop(&img);
    }
    emit_li64(&img, R_T0, 0x9200000001000020ULL);
    emit_ld(&img, R_T1, R_T0, 0);
    emit(&img, OPC_SPECIAL | (R_T1 << 16) | (R_T1 << 11) | (3 << 6));
    emit_lui(&img, R_S0, 0xa018);
    emit(&img, OPC_SPECIAL | (R_S0 << 21) | (R_T1 << 16) |
               (R_S0 << 11) | 0x2d);
    emit_li64(&img, R_T0, 0x9000000000020000ULL);
    emit_jr(&img, R_T0);
    emit_nop(&img);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M -smp 2 -cpu R10000 "
                      "-bios %s -accel tcg,thread=single -S "
                      "-icount shift=2,align=off,sleep=off "
                      "-no-reboot -no-shutdown", path);
    unlink(path);

    /* The source is shared; the low Hub-local destination swaps bit 16. */
    stl_be_p(source, mips_branch(OPC_BEQ, R_ZERO, R_ZERO,
                                (0x1000 - 0x20004) / 4));
    qtest_memwrite(qts, 0x20000, source, sizeof(source));
    for (int cpu = 0; cpu < 2; cpu++) {
        stl_be_p(dest, mips_addiu(R_T2, R_ZERO, cpu + 1));
        stl_be_p(dest + 4, mips_sd(R_T2, R_S0, 0));
        stl_be_p(dest + 8, mips_branch(OPC_BEQ, R_ZERO, R_ZERO, -1));
        qtest_memwrite(qts, 0x1000 + cpu * 0x10000, dest, sizeof(dest));
    }
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&w, "guest done or fault");
    for (;;) {
        if (qtest_readq(qts, 0x180000) && qtest_readq(qts, 0x180008)) {
            break;
        }
        sgi_wait_pause(&w, POLL_INTERVAL_US);
    }
    g_assert_cmpuint(qtest_readq(qts, 0x180000), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, 0x180008), ==, 2);
    qtest_quit(qts);
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
    SgiWait w;

    fd = g_file_open_tmp("mips64-r10k-tlb-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    tlb_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", image);
    unlink(image);

    sgi_wait_start(&w, "guest done or fault");
    for (;;) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + TLB_DONE_OFF);
        uint64_t fault = qtest_readq(qts, STATE_PHYS + TLB_FAULT_OFF);

        if (done || fault) {
            g_assert_cmphex(fault, ==, 0);
            g_assert_cmphex(done, ==, (uint64_t)TLB_DONE_MAGIC);
            break;
        }
        sgi_wait_pause(&w, POLL_INTERVAL_US);
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

/*
 * tlb-replaced-half: an entry TLBWR replaces is gone, so a reference to
 * either of its pages takes TLB Refill, even through the half that was
 * marked invalid. TLB Invalid needs an entry that still matches (R10000
 * User's Manual version 2.0, chapter 17, TLB Refill and TLB Invalid
 * exceptions). With Wired at the last index, Random names that index for
 * every TLBWR, so the second write replaces the first.
 */
#define CP0_WIRED           6
#define OPC_TLBWR           0x42000006u
#define REPLACED_VPN2       0x00020000u
#define REPLACED_ODD_PAGE   0x00021000u
#define REPLACING_VPN2      0x00040000u

static void emit_tlb_write_random(GuestImage *img, uint32_t vpn2,
                                  uint32_t lo0, uint32_t lo1)
{
    emit_lui(img, R_T0, (uint16_t)(vpn2 >> 16));
    emit_ori(img, R_T0, R_T0, (uint16_t)(vpn2 & 0xffff));
    emit_mtc0(img, R_T0, CP0_ENTRYHI);
    emit_lui(img, R_T0, (uint16_t)(lo0 >> 16));
    emit_ori(img, R_T0, R_T0, (uint16_t)(lo0 & 0xffff));
    emit_mtc0(img, R_T0, CP0_ENTRYLO0);
    emit_lui(img, R_T0, (uint16_t)(lo1 >> 16));
    emit_ori(img, R_T0, R_T0, (uint16_t)(lo1 & 0xffff));
    emit_mtc0(img, R_T0, CP0_ENTRYLO1);
    emit_ehb(img);
    emit(img, OPC_TLBWR);
    emit_ehb(img);
}

static void tlb_replaced_half_write_guest_image(int fd)
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
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_addiu(&img, R_T1, R_ZERO, STATUS_ERL_CLEAR);
    emit_and(&img, R_T0, R_T0, R_T1);
    emit_mtc0(&img, R_T0, CP0_STATUS);
    emit_mtc0(&img, R_ZERO, CP0_PAGEMASK);
    emit_addiu(&img, R_T0, R_ZERO, 63);
    emit_mtc0(&img, R_T0, CP0_WIRED);
    emit_ehb(&img);

    /* Global, even page valid and uncached, odd page invalid. */
    emit_tlb_write_random(&img, REPLACED_VPN2, (0x200 << 6) | 0x17, 0x1);
    emit_tlb_write_random(&img, REPLACING_VPN2, (0x202 << 6) | 0x17,
                          (0x203 << 6) | 0x17);

    emit_lui(&img, R_T1, (uint16_t)(REPLACED_ODD_PAGE >> 16));
    emit_ori(&img, R_T1, R_T1, (uint16_t)(REPLACED_ODD_PAGE & 0xffff));
    emit_ld(&img, R_T3, R_T1, 0);

    emit_lui(&img, R_T0, STATE_KSEG1_HI);
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

static void test_r10k_tlb_replaced_half(void)
{
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;
    SgiWait w;

    fd = g_file_open_tmp("mips64-r10k-tlb-replaced-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    tlb_replaced_half_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", image);
    unlink(image);

    /* The handler stores EPC last: a nonzero EPC means a whole record. */
    sgi_wait_start(&w, "guest fault or done");
    for (;;) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + TLB_DONE_OFF);
        uint64_t epc = qtest_readq(qts, STATE_PHYS + TLB_FAULT_OFF + 24);

        if (done || epc) {
            break;
        }
        sgi_wait_pause(&w, POLL_INTERVAL_US);
    }

    /* The 32-bit refill vector, TLBL, at the replaced entry's odd page. */
    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + TLB_FAULT_OFF), ==, 1);
    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + TLB_FAULT_OFF + 8) &
                    0x7c, ==, 2 << 2);
    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + TLB_FAULT_OFF + 16), ==,
                    REPLACED_ODD_PAGE);

    qtest_quit(qts);
}

#define COUNT_IMAGE_SIZE    0x1000
#define COUNT_DONE_OFF      0x60
#define COUNT_FAULT_OFF     0x68
#define COUNT_START_OFF     0x70
#define COUNT_END_OFF       0x78
#define COUNT_DONE_MAGIC    0x434e544d /* "CNTM" */
#define CP0_COUNT           9

static void count_write_guest_image(int fd)
{
    uint8_t *image = g_malloc0(COUNT_IMAGE_SIZE);
    GuestImage img = { .code = image, .pos = 0, .size = COUNT_IMAGE_SIZE };
    ssize_t written;

    img.pos = VEC_TLB_REFILL;
    emit_report_and_spin(&img, FAULT_MAGIC, COUNT_FAULT_OFF);
    img.pos = VEC_XTLB_REFILL;
    emit_report_and_spin(&img, FAULT_MAGIC, COUNT_FAULT_OFF);
    img.pos = VEC_CACHE_ERROR;
    emit_report_and_spin(&img, FAULT_MAGIC, COUNT_FAULT_OFF);
    img.pos = VEC_GENERAL;
    emit_report_and_spin(&img, FAULT_MAGIC, COUNT_FAULT_OFF);

    img.pos = 0;
    /* Clear ERL */
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_addiu(&img, R_T1, R_ZERO, STATUS_ERL_CLEAR);
    emit_and(&img, R_T0, R_T0, R_T1);
    emit_mtc0(&img, R_T0, CP0_STATUS);
    emit_ehb(&img);

    /* Read initial Count into S0 (16) */
    emit_mfc0(&img, 16, CP0_COUNT);

    /* Loop 100 iterations reading count */
    emit_addiu(&img, R_T1, R_ZERO, 100);
    unsigned int loop_start = img.pos;
    emit_mfc0(&img, R_T0, CP0_COUNT);
    emit_addiu(&img, R_T1, R_T1, -1);
    emit_branch(&img, OPC_BNE, R_T1, R_ZERO, loop_start);
    emit_nop(&img);

    /* Read final Count into S1 (17) */
    emit_mfc0(&img, 17, CP0_COUNT);

    emit_lui(&img, R_T0, STATE_KSEG1_HI);
    emit_sd(&img, 16, R_T0, COUNT_START_OFF);
    emit_sd(&img, 17, R_T0, COUNT_END_OFF);

    emit_lui(&img, R_T2, (uint16_t)(COUNT_DONE_MAGIC >> 16));
    emit_addiu(&img, R_T2, R_T2, (int16_t)(COUNT_DONE_MAGIC & 0xffff));
    emit_sd(&img, R_T2, R_T0, COUNT_DONE_OFF);
    emit(&img, OPC_SYNC);

    unsigned int spin = img.pos;
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, spin);
    emit_nop(&img);

    written = write(fd, image, COUNT_IMAGE_SIZE);
    g_assert_cmpint(written, ==, COUNT_IMAGE_SIZE);
    g_free(image);
}

static void test_r10k_count_chaining(void)
{
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;
    SgiWait w;

    fd = g_file_open_tmp("mips64-r10k-count-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    count_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", image);
    unlink(image);

    sgi_wait_start(&w, "guest done or fault");
    for (;;) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + COUNT_DONE_OFF);
        uint64_t fault = qtest_readq(qts, STATE_PHYS + COUNT_FAULT_OFF);

        if (done || fault) {
            g_assert_cmphex(fault, ==, 0);
            g_assert_cmphex(done, ==, (uint64_t)COUNT_DONE_MAGIC);
            break;
        }
        sgi_wait_pause(&w, POLL_INTERVAL_US);
    }

    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + COUNT_DONE_OFF), ==,
                    (uint64_t)COUNT_DONE_MAGIC);
    uint32_t start = (uint32_t)qtest_readq(qts, STATE_PHYS + COUNT_START_OFF);
    uint32_t end = (uint32_t)qtest_readq(qts, STATE_PHYS + COUNT_END_OFF);
    g_assert_cmpuint(end, >, start);

    qtest_quit(qts);
}

#define R_S0                16

#define COUNT_INT_IMAGE_SIZE    0x1000
#define COUNT_INT_DONE_OFF      0xa0
#define COUNT_INT_FAULT_OFF     0xa8
#define COUNT_INT_EPC_OFF       0xb0
#define COUNT_INT_CANARY_OFF    0xb8
#define COUNT_INT_CAUSE_OFF     0xc0
#define COUNT_INT_DONE_MAGIC    0x434e5449 /* "CNTI" */
#define CP0_COMPARE             11
#define CP0_CAUSE               13
#define CP0_EPC                 14
#define COUNT_INT_CODE_START    0x500

static void count_interrupt_write_guest_image(int fd, uint32_t *canary_pc)
{
    uint8_t *image = g_malloc0(COUNT_INT_IMAGE_SIZE);
    GuestImage img = { .code = image, .pos = 0, .size = COUNT_INT_IMAGE_SIZE };
    ssize_t written;

    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, COUNT_INT_CODE_START);
    emit_nop(&img);

    img.pos = VEC_TLB_REFILL;
    emit_report_and_spin(&img, FAULT_MAGIC, COUNT_INT_FAULT_OFF);
    img.pos = VEC_XTLB_REFILL;
    emit_report_and_spin(&img, FAULT_MAGIC, COUNT_INT_FAULT_OFF);
    img.pos = VEC_CACHE_ERROR;
    emit_report_and_spin(&img, FAULT_MAGIC, COUNT_INT_FAULT_OFF);

    /*
     * General exception / interrupt vector:
     * When Count reaches Compare, timer interrupt IP7 fires.
     * With Status.IE=1, Status.IM7=1, Status.EXL=0, Status.ERL=0,
     * the interrupt vectors here.
     * We record EPC, Cause, and canary $s0.
     */
    img.pos = VEC_GENERAL;
    emit_mfc0(&img, R_T0, CP0_EPC);
    emit_mfc0(&img, R_T2, CP0_CAUSE);
    emit_lui(&img, R_T3, STATE_KSEG1_HI);
    emit_sd(&img, R_T0, R_T3, COUNT_INT_EPC_OFF);
    emit_sd(&img, R_S0, R_T3, COUNT_INT_CANARY_OFF);
    emit_sd(&img, R_T2, R_T3, COUNT_INT_CAUSE_OFF);

    /* Acknowledge/clear the timer interrupt */
    emit_mtc0(&img, R_ZERO, CP0_COMPARE);

    /* Signal completion */
    emit_lui(&img, R_T0, (uint16_t)(COUNT_INT_DONE_MAGIC >> 16));
    emit_addiu(&img, R_T0, R_T0, (int16_t)(COUNT_INT_DONE_MAGIC & 0xffff));
    emit_sd(&img, R_T0, R_T3, COUNT_INT_DONE_OFF);
    emit(&img, OPC_SYNC);

    unsigned int hspin = img.pos;
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, hspin);
    emit_nop(&img);

    /* Dedicated interrupt vector when Cause.IV == 1 */
    img.pos = 0x400;
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, VEC_GENERAL);
    emit_nop(&img);

    img.pos = COUNT_INT_CODE_START;

    /* Base address for memory stores: STATE_KSEG1_HI */
    emit_lui(&img, R_T3, STATE_KSEG1_HI);

    /* Initialize canary register $s0 to 0 */
    emit_addiu(&img, R_S0, R_ZERO, 0);

    /* Clear ERL in Status */
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_addiu(&img, R_T1, R_ZERO, STATUS_ERL_CLEAR);
    emit_and(&img, R_T0, R_T0, R_T1);
    emit_mtc0(&img, R_T0, CP0_STATUS);
    emit_ehb(&img);

    /*
     * Reset Count to 0 and arm Compare for 20,000,000 ticks (200 ms).
     * Writing Count directly establishes timer_adjacent_pending and
     * guarantees Compare is in the future.
     */
    emit_mtc0(&img, R_ZERO, CP0_COUNT);
    emit_ehb(&img);
    emit_lui(&img, R_T2, 0x0131);
    emit_ori(&img, R_T2, R_T2, 0x2d00); /* 20,000,000 ticks = 200 ms */
    emit_mtc0(&img, R_T2, CP0_COMPARE);
    emit_ehb(&img);

    /* Enable interrupts: IE (bit 0) and IM7 (bit 15) -> Status |= 0x8001 */
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_lui(&img, R_T1, 0);
    emit_ori(&img, R_T1, R_T1, 0x8001);
    emit_or(&img, R_T0, R_T0, R_T1);
    emit_mtc0(&img, R_T0, CP0_STATUS);
    emit_ehb(&img);

    /*
     * Polling loop:
     * Set canary $s0 = 0 before mfc0.
     * Read Count. When Count reaches Compare, helper raises timer IP7.
     * With DISAS_EXIT, execution exits immediately at the mfc0 boundary.
     * The interrupt is taken before the next instruction (addiu $s0, 0x1111)
     * runs, leaving $s0 == 0 and EPC == address of addiu.
     */
    unsigned int poll_loop = img.pos;
    emit_addiu(&img, R_S0, R_ZERO, 0);
    emit_mfc0(&img, R_T0, CP0_COUNT);
    *canary_pc = 0xbfc00000u + img.pos;
    emit_addiu(&img, R_S0, R_ZERO, 0x1111);
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, poll_loop);
    emit_nop(&img);

    written = write(fd, image, COUNT_INT_IMAGE_SIZE);
    g_assert_cmpint(written, ==, COUNT_INT_IMAGE_SIZE);
    g_free(image);
}

static void test_r10k_count_interrupt_delivery(void)
{
    g_autofree char *image = NULL;
    QTestState *qts;
    uint32_t expected_epc = 0;
    int fd;
    SgiWait w;

    fd = g_file_open_tmp("mips64-r10k-count-int-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    count_interrupt_write_guest_image(fd, &expected_epc);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", image);
    unlink(image);

    sgi_wait_start(&w, "guest done or fault");
    for (;;) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + COUNT_INT_DONE_OFF);
        uint64_t fault = qtest_readq(qts, STATE_PHYS + COUNT_INT_FAULT_OFF);

        if (done || fault) {
            g_assert_cmphex(fault, ==, 0);
            g_assert_cmphex(done, ==, (uint64_t)COUNT_INT_DONE_MAGIC);
            break;
        }
        sgi_wait_pause(&w, POLL_INTERVAL_US);
    }

    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + COUNT_INT_FAULT_OFF), ==, 0);
    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + COUNT_INT_DONE_OFF), ==,
                    (uint64_t)COUNT_INT_DONE_MAGIC);

    /* Canary register $s0 must be 0: instruction after mfc0 did NOT execute */
    uint64_t canary = qtest_readq(qts, STATE_PHYS + COUNT_INT_CANARY_OFF);
    g_assert_cmphex(canary, ==, 0);

    /* EPC must point to the instruction immediately following mfc0 Count */
    uint64_t epc = qtest_readq(qts, STATE_PHYS + COUNT_INT_EPC_OFF);
    g_assert_cmphex((uint32_t)epc, ==, expected_epc);

    /* Cause must have IP7 (bit 15) set and ExcCode == 0 (Interrupt) */
    uint64_t cause_raw = qtest_readq(qts, STATE_PHYS + COUNT_INT_CAUSE_OFF);
    uint32_t cause = (uint32_t)cause_raw;
    g_assert_cmphex(cause & (1u << 15), ==, (1u << 15));
    g_assert_cmphex(cause & 0x7cu, ==, 0);

    qtest_quit(qts);
}


#define FPU_IMAGE_SIZE      0x1000
#define FPU_DONE_OFF        0x80
#define FPU_FAULT_OFF       0x88
#define FPU_FCR31_PASS_OFF  0x90
#define FPU_FCR31_DIV0_OFF  0x98
#define FPU_DONE_MAGIC      0x4650554d /* "FPUM" */

#define OPC_CFC1            0x44400000u
#define OPC_CTC1            0x44c00000u
#define OPC_MTC1            0x44800000u
#define OPC_ADD_S           0x46000000u
#define OPC_DIV_S           0x46000003u

static void emit_cfc1(GuestImage *img, int rt, int fs)
{
    emit(img, OPC_CFC1 | (rt << 16) | (fs << 11));
}

static void emit_ctc1(GuestImage *img, int rt, int fs)
{
    emit(img, OPC_CTC1 | (rt << 16) | (fs << 11));
}

static void emit_mtc1(GuestImage *img, int rt, int fs)
{
    emit(img, OPC_MTC1 | (rt << 16) | (fs << 11));
}

static void emit_add_s(GuestImage *img, int fd, int fs, int ft)
{
    emit(img, OPC_ADD_S | (ft << 16) | (fs << 11) | (fd << 6));
}

static void emit_div_s(GuestImage *img, int fd, int fs, int ft)
{
    emit(img, OPC_DIV_S | (ft << 16) | (fs << 11) | (fd << 6));
}

static void fpu_write_guest_image(int fd)
{
    uint8_t *image = g_malloc0(FPU_IMAGE_SIZE);
    GuestImage img = { .code = image, .pos = 0, .size = FPU_IMAGE_SIZE };
    ssize_t written;

    img.pos = VEC_TLB_REFILL;
    emit_report_and_spin(&img, FAULT_MAGIC, FPU_FAULT_OFF);
    img.pos = VEC_XTLB_REFILL;
    emit_report_and_spin(&img, FAULT_MAGIC, FPU_FAULT_OFF);
    img.pos = VEC_CACHE_ERROR;
    emit_report_and_spin(&img, FAULT_MAGIC, FPU_FAULT_OFF);
    img.pos = VEC_GENERAL;
    emit_report_and_spin(&img, FAULT_MAGIC, FPU_FAULT_OFF);

    img.pos = 0;
    /* Clear ERL and enable CU1 */
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_addiu(&img, R_T1, R_ZERO, STATUS_ERL_CLEAR);
    emit_and(&img, R_T0, R_T0, R_T1);
    emit_lui(&img, R_T1, 0x2000); /* CU1 */
    emit(&img, 0x25u | (R_T0 << 21) | (R_T1 << 16) | (R_T0 << 11)); /* or */
    emit_mtc0(&img, R_T0, CP0_STATUS);
    emit_ehb(&img);

    /* Reset FCR31 to 0 */
    emit_ctc1(&img, R_ZERO, 31);

    /* Load 1.0f (0x3f800000) and 2.0f (0x40000000) into FPRs */
    emit_lui(&img, R_T0, 0x3f80);
    emit_mtc1(&img, R_T0, 0); /* $f0 = 1.0f */
    emit_lui(&img, R_T1, 0x4000);
    emit_mtc1(&img, R_T1, 1); /* $f1 = 2.0f */

    /* Loop 50 iterations of non-exceptional FP additions */
    emit_addiu(&img, R_T2, R_ZERO, 50);
    unsigned int loop_start = img.pos;
    emit_add_s(&img, 2, 0, 1); /* $f2 = $f0 + $f1 */
    emit_addiu(&img, R_T2, R_T2, -1);
    emit_branch(&img, OPC_BNE, R_T2, R_ZERO, loop_start);
    emit_nop(&img);

    /* Read FCR31: should have 0 flags/causes */
    emit_cfc1(&img, 16, 31); /* S0 */

    /* Now perform 1.0f / 0.0f -> divide by zero */
    emit_mtc1(&img, R_ZERO, 1); /* $f1 = 0.0f */
    emit_div_s(&img, 2, 0, 1);  /* $f2 = 1.0f / 0.0f */
    emit_cfc1(&img, 17, 31); /* S1 */

    emit_lui(&img, R_T0, STATE_KSEG1_HI);
    emit_sd(&img, 16, R_T0, FPU_FCR31_PASS_OFF);
    emit_sd(&img, 17, R_T0, FPU_FCR31_DIV0_OFF);

    emit_lui(&img, R_T2, (uint16_t)(FPU_DONE_MAGIC >> 16));
    emit_addiu(&img, R_T2, R_T2, (int16_t)(FPU_DONE_MAGIC & 0xffff));
    emit_sd(&img, R_T2, R_T0, FPU_DONE_OFF);
    emit(&img, OPC_SYNC);

    unsigned int spin = img.pos;
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, spin);
    emit_nop(&img);

    written = write(fd, image, FPU_IMAGE_SIZE);
    g_assert_cmpint(written, ==, FPU_IMAGE_SIZE);
    g_free(image);
}

static void test_r10k_fpu_fast_path(void)
{
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;
    SgiWait w;

    fd = g_file_open_tmp("mips64-r10k-fpu-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    fpu_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", image);
    unlink(image);

    sgi_wait_start(&w, "guest done or fault");
    for (;;) {
        uint64_t done = qtest_readq(qts, STATE_PHYS + FPU_DONE_OFF);
        uint64_t fault = qtest_readq(qts, STATE_PHYS + FPU_FAULT_OFF);

        if (done || fault) {
            g_assert_cmphex(fault, ==, 0);
            g_assert_cmphex(done, ==, (uint64_t)FPU_DONE_MAGIC);
            break;
        }
        sgi_wait_pause(&w, POLL_INTERVAL_US);
    }

    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + FPU_DONE_OFF), ==,
                    (uint64_t)FPU_DONE_MAGIC);
    uint32_t fcr31_pass =
        (uint32_t)qtest_readq(qts, STATE_PHYS + FPU_FCR31_PASS_OFF);
    uint32_t fcr31_div0 =
        (uint32_t)qtest_readq(qts, STATE_PHYS + FPU_FCR31_DIV0_OFF);

    /* Common non-exceptional path: Cause (17:12) and Flag (6:2) are zero */
    g_assert_cmphex(fcr31_pass & 0x0003f07cu, ==, 0);

    /* Divide by zero sets Flag bit 5 (0x20) and Cause bit 15 (0x8000) */
    g_assert_cmphex(fcr31_div0 & 0x8020u, ==, 0x8020u);

    qtest_quit(qts);
}

/* ---------------------------------------------------------------- */
/* Secondary-cache capacity selected by board reset mode            */
/* ---------------------------------------------------------------- */

/*
 * R10000 Config0 bits 24:0 as an SN0 board drives them, but with SS = 5
 * (16 MB, the largest encoding) in place of the 1 MB the CPU definition
 * defaults to. Index operations at the top of that range must land in
 * arrays sized for it: a model that sizes from the definition but bounds
 * by the mode bits writes far past the end of its arrays.
 */
#define SCACHE_MODE_16M         0x015a785
#define SCACHE_TOP_TAG_ADDR     0xffffffff807fff81ULL /* last set, way 1 */
#define SCACHE_TOP_DATA_ADDR    0xffffffff807ffff9ULL /* last quad, way 1 */
#define SCACHE_TAGLO            0x5a5a4c00u
#define SCACHE_TAGHI            0x80000005u
#define SCACHE_DATA_LO          0x12345678u
#define SCACHE_DATA_HI          0x7abcdef0u
#define SCACHE_ECC              0x155u

#define SCACHE_CONFIG_OFF       0x00
#define SCACHE_TAGLO_OFF        0x08
#define SCACHE_TAGHI_OFF        0x10
#define SCACHE_DATA_LO_OFF      0x18
#define SCACHE_DATA_HI_OFF      0x20
#define SCACHE_ECC_OFF          0x28

#define CP0_CONFIG              16
#define CP0_ECC                 26
#define CP0_TAGLO               28
#define CP0_TAGHI               29

#define CACHE_S_INDEX_LOAD_TAG  0x07
#define CACHE_S_INDEX_STORE_TAG 0x0b
#define CACHE_S_INDEX_LOAD_DATA 0x1b
#define CACHE_S_INDEX_STORE_DATA 0x1f

static void emit_cache(GuestImage *img, int op, int base)
{
    emit(img, OPC_CACHE | (base << 21) | (op << 16));
}

static void emit_li32(GuestImage *img, int rt, uint32_t value)
{
    emit_lui(img, rt, value >> 16);
    emit_ori(img, rt, rt, value & 0xffff);
}

#define SOFT_READY_OFF          0x40
#define SOFT_DONE_OFF           0x48
#define SOFT_STATUS_OFF         0x50
#define SOFT_ERROREPC_OFF       0x58
#define SOFT_CONFIG_OFF         0x60
#define SOFT_WATCHLO_OFF        0x68
#define SOFT_WATCHHI_OFF        0x70
#define SOFT_EXPECT_CONFIG_OFF  0x78
#define SOFT_EXPECT_WATCHLO_OFF 0x80
#define SOFT_EXPECT_WATCHHI_OFF 0x88
#define SOFT_INITIAL_CONFIG_OFF 0x90
#define SOFT_INITIAL_STATUS_OFF 0x98
#define SOFT_SEEDED_STATUS_OFF  0xa0
#define SOFT_DONE_MAGIC         0x5352
#define SOFT_HANDLER            0x700
#define SOFT_SETUP              0x500
#define SOFT_CONFIG_K0          3
#define SOFT_WATCHLO            0x12345000u
#define SOFT_WATCHHI            0x30u
#define CP0_WATCHLO             18
#define CP0_WATCHHI             19
#define STATUS_KSU              (3u << 3)

static void emit_andi(GuestImage *img, int rt, int rs, uint16_t imm)
{
    emit(img, MIPS_OPC_ANDI | (rs << 21) | (rt << 16) | imm);
}

static unsigned int soft_reset_write_guest_image(int fd)
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    unsigned int park_pc;
    ssize_t written;

    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, CODE_START);
    emit_nop(&img);

    img.pos = CODE_START;
    emit_lui(&img, R_T2, STATE_KSEG1_HI);
    emit_ld(&img, R_T0, R_T2, SOFT_READY_OFF);
    emit_addiu(&img, R_T1, R_ZERO, READY_MAGIC);
    emit_branch(&img, OPC_BNE, R_T0, R_T1, SOFT_SETUP);
    emit_nop(&img);
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, SOFT_HANDLER);
    emit_nop(&img);

    img.pos = SOFT_SETUP;
    emit_lui(&img, R_T2, STATE_KSEG1_HI);
    emit_mfc0(&img, R_T0, CP0_CONFIG);
    emit_sd(&img, R_T0, R_T2, SOFT_INITIAL_CONFIG_OFF);
    emit_andi(&img, R_T1, R_T0, 0xfff8);
    emit_ori(&img, R_T1, R_T1, SOFT_CONFIG_K0);
    emit_mtc0(&img, R_T1, CP0_CONFIG);
    emit_mfc0(&img, R_T0, CP0_CONFIG);
    emit_sd(&img, R_T0, R_T2, SOFT_EXPECT_CONFIG_OFF);

    emit_li32(&img, R_T0, SOFT_WATCHLO);
    emit_mtc0(&img, R_T0, CP0_WATCHLO);
    emit_li32(&img, R_T0, SOFT_WATCHHI);
    emit_mtc0(&img, R_T0, CP0_WATCHHI);
    emit_mfc0(&img, R_T0, CP0_WATCHLO);
    emit_sd(&img, R_T0, R_T2, SOFT_EXPECT_WATCHLO_OFF);
    emit_mfc0(&img, R_T0, CP0_WATCHHI);
    emit_sd(&img, R_T0, R_T2, SOFT_EXPECT_WATCHHI_OFF);

    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_sd(&img, R_T0, R_T2, SOFT_INITIAL_STATUS_OFF);
    emit_li32(&img, R_T1, ~(STATUS_SR | STATUS_ERL | STATUS_BEV));
    emit_and(&img, R_T1, R_T0, R_T1);
    emit_li32(&img, R_T2, STATUS_NMI | STATUS_TS | STATUS_KX);
    emit_or(&img, R_T1, R_T1, R_T2);
    emit_mtc0(&img, R_T1, CP0_STATUS);
    emit_lui(&img, R_T2, STATE_KSEG1_HI);
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_sd(&img, R_T0, R_T2, SOFT_SEEDED_STATUS_OFF);

    emit_addiu(&img, R_T0, R_ZERO, READY_MAGIC);
    emit_sd(&img, R_T0, R_T2, SOFT_READY_OFF);
    park_pc = img.pos;
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, park_pc);
    emit_nop(&img);

    img.pos = SOFT_HANDLER;
    emit_lui(&img, R_T2, STATE_KSEG1_HI);
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_sd(&img, R_T0, R_T2, SOFT_STATUS_OFF);
    emit_mfc0(&img, R_T0, CP0_ERROREPC);
    emit_sd(&img, R_T0, R_T2, SOFT_ERROREPC_OFF);
    emit_mfc0(&img, R_T0, CP0_CONFIG);
    emit_sd(&img, R_T0, R_T2, SOFT_CONFIG_OFF);
    emit_mfc0(&img, R_T0, CP0_WATCHLO);
    emit_sd(&img, R_T0, R_T2, SOFT_WATCHLO_OFF);
    emit_mfc0(&img, R_T0, CP0_WATCHHI);
    emit_sd(&img, R_T0, R_T2, SOFT_WATCHHI_OFF);
    emit_addiu(&img, R_T0, R_ZERO, SOFT_DONE_MAGIC);
    emit_sd(&img, R_T0, R_T2, SOFT_DONE_OFF);
    emit(&img, OPC_ERET);
    emit_nop(&img);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
    return park_pc;
}

static void test_r10k_soft_reset(gconstpointer opaque)
{
    const char *cpu = opaque;
    g_autofree char *image = NULL;
    QTestState *qts;
    uint64_t status, config, watchlo, watchhi, error_epc;
    uint64_t initial_config;
    unsigned int park_pc;
    int fd;

    if (!qtest_has_accel("tcg") ||
        !qtest_has_device("mips-cpu-exception-fixture")) {
        g_test_skip("MIPS CPU exception fixture requires TCG");
        return;
    }

    fd = g_file_open_tmp("mips64-r10k-soft-reset-image-XXXXXX", &image,
                         NULL);
    g_assert_cmpint(fd, >=, 0);
    park_pc = soft_reset_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M malta -m 64M -cpu %s -bios %s -accel tcg "
                      "-device mips-cpu-exception-fixture,id=reset-fixture,"
                      "cpu-index=0 -no-reboot -no-shutdown", cpu, image);
    unlink(image);
    wait_for_state(qts, SOFT_READY_OFF, READY_MAGIC);
    g_usleep(10 * POLL_INTERVAL_US);

    qtest_set_irq_in(qts, "/machine/peripheral/reset-fixture", "soft-reset",
                     0, 1);
    qtest_set_irq_in(qts, "/machine/peripheral/reset-fixture", "soft-reset",
                     0, 0);
    wait_for_state(qts, SOFT_DONE_OFF, SOFT_DONE_MAGIC);

    status = state_word(qts, SOFT_STATUS_OFF);
    g_assert_cmphex(status & (STATUS_SR | STATUS_NMI | STATUS_ERL |
                              STATUS_BEV | STATUS_TS),
                    ==, STATUS_SR | STATUS_ERL | STATUS_BEV);
    g_assert_cmphex(status & STATUS_KX, ==, STATUS_KX);
    g_assert_cmphex(state_word(qts, SOFT_SEEDED_STATUS_OFF) & STATUS_KX,
                    ==, STATUS_KX);
    error_epc = state_word(qts, SOFT_ERROREPC_OFF);
    g_assert_cmphex(error_epc, ==,
                    0xffffffffbfc00000ULL + park_pc);

    status = state_word(qts, SOFT_INITIAL_STATUS_OFF);
    g_assert_cmphex(status & STATUS_KSU, ==, 0);
    status = state_word(qts, SOFT_SEEDED_STATUS_OFF);
    g_assert_cmphex(status & (STATUS_SR | STATUS_ERL | STATUS_BEV), ==, 0);
    g_assert_cmphex(status & (STATUS_NMI | STATUS_TS), ==,
                    STATUS_NMI | STATUS_TS);
    g_assert_cmphex(status & STATUS_KSU, ==, 0);

    config = state_word(qts, SOFT_CONFIG_OFF);
    initial_config = state_word(qts, SOFT_INITIAL_CONFIG_OFF);
    g_assert_cmphex(config, ==,
                    state_word(qts, SOFT_EXPECT_CONFIG_OFF));
    g_assert_cmphex(initial_config & 7, ==, 5);
    g_assert_cmphex(config & 7, ==, SOFT_CONFIG_K0);
    watchlo = state_word(qts, SOFT_WATCHLO_OFF);
    watchhi = state_word(qts, SOFT_WATCHHI_OFF);
    g_assert_cmphex(watchlo, ==,
                    state_word(qts, SOFT_EXPECT_WATCHLO_OFF));
    g_assert_cmphex(watchhi, ==,
                    state_word(qts, SOFT_EXPECT_WATCHHI_OFF));
    g_assert_cmphex(watchlo, ==, SOFT_WATCHLO);
    g_assert_cmphex(watchhi, ==, SOFT_WATCHHI);
    g_assert_cmphex(watchlo & 7, ==, 0);

    qtest_quit(qts);
}

#define CACHEERR_VECTOR_PC_OFF       0x100
#define CACHEERR_READY_OFF           0x108
#define CACHEERR_ENTRY_COUNT_OFF     0x110
#define CACHEERR_STATUS_OFF          0x118
#define CACHEERR_ERROREPC_OFF        0x120
#define CACHEERR_EPC_OFF             0x128
#define CACHEERR_CAUSE_OFF           0x130
#define CACHEERR_CAUSE_HW_IP_MASK    0xfc00ULL
#define CACHEERR_INITIAL_STATUS_OFF  0x138
#define CACHEERR_SEEDED_STATUS_OFF   0x140
#define CACHEERR_INITIAL_ERROREPC_OFF 0x148
#define CACHEERR_INITIAL_EPC_OFF     0x150
#define CACHEERR_INITIAL_CAUSE_OFF   0x158
#define CACHEERR_HEARTBEAT_OFF       0x160
#define CACHEERR_READY_MAGIC         0x4345
#define CACHEERR_EPC_CANARY          0x1234
#define CACHEERR_ERROREPC_CANARY     0x5678
#define CACHEERR_HANDLER             0x700
#define CACHEERR_SETUP               0x500
#define CACHEERR_CP0_CAUSE           13
#define CACHEERR_CP0_EPC             14
#define CACHEERR_STATUS_EXL          (1u << 1)
#define CACHEERR_STATUS_IM           (0xffu << 8)

static unsigned int cacheerr_write_guest_image(int fd)
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    unsigned int park_pc;
    ssize_t written;

    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, CODE_START);
    emit_nop(&img);

    /* BEV CacheErr enters at BFC00300, in the uncached KSeg1 boot window. */
    img.pos = VEC_CACHE_ERROR;
    emit(&img, (0x03u << 26) |
         (((0x0fc00000u | CACHEERR_HANDLER) >> 2) & 0x03ffffff));
    emit_nop(&img);

    img.pos = CODE_START;
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, CACHEERR_SETUP);
    emit_nop(&img);

    img.pos = CACHEERR_SETUP;
    emit_lui(&img, R_T2, STATE_KSEG1_HI);
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_sd(&img, R_T0, R_T2, CACHEERR_INITIAL_STATUS_OFF);
    emit_li32(&img, R_T1, ~(STATUS_ERL | CACHEERR_STATUS_EXL |
                            CACHEERR_STATUS_IM | STATUS_KSU));
    emit_and(&img, R_T1, R_T0, R_T1);
    emit_li32(&img, R_T2, STATUS_BEV);
    emit_or(&img, R_T1, R_T1, R_T2);
    emit_mtc0(&img, R_T1, CP0_STATUS);

    emit_lui(&img, R_T2, STATE_KSEG1_HI);
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_sd(&img, R_T0, R_T2, CACHEERR_SEEDED_STATUS_OFF);
    emit_li32(&img, R_T0, CACHEERR_EPC_CANARY);
    emit_mtc0(&img, R_T0, CACHEERR_CP0_EPC);
    emit_li32(&img, R_T0, CACHEERR_ERROREPC_CANARY);
    emit_mtc0(&img, R_T0, CP0_ERROREPC);
    emit_mfc0(&img, R_T0, CP0_ERROREPC);
    emit_sd(&img, R_T0, R_T2, CACHEERR_INITIAL_ERROREPC_OFF);
    emit_mfc0(&img, R_T0, CACHEERR_CP0_EPC);
    emit_sd(&img, R_T0, R_T2, CACHEERR_INITIAL_EPC_OFF);
    emit_mfc0(&img, R_T0, CACHEERR_CP0_CAUSE);
    emit_sd(&img, R_T0, R_T2, CACHEERR_INITIAL_CAUSE_OFF);
    emit_addiu(&img, R_T0, R_ZERO, READY_MAGIC);
    emit_sd(&img, R_T0, R_T2, CACHEERR_READY_OFF);
    park_pc = img.pos;
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, park_pc);
    emit_nop(&img);

    img.pos = CACHEERR_HANDLER;
    emit_lui(&img, R_T2, STATE_KSEG1_HI);
    emit_sd(&img, 31, R_T2, CACHEERR_VECTOR_PC_OFF);
    emit_ld(&img, R_T0, R_T2, CACHEERR_ENTRY_COUNT_OFF);
    emit_addiu(&img, R_T0, R_T0, 1);
    emit_sd(&img, R_T0, R_T2, CACHEERR_ENTRY_COUNT_OFF);
    emit_addiu(&img, R_T0, R_ZERO, CACHEERR_READY_MAGIC);
    emit_sd(&img, R_T0, R_T2, CACHEERR_READY_OFF);
    {
        unsigned int snapshot_pc = img.pos;

        emit_mfc0(&img, R_T0, CP0_STATUS);
        emit_sd(&img, R_T0, R_T2, CACHEERR_STATUS_OFF);
        emit_mfc0(&img, R_T0, CP0_ERROREPC);
        emit_sd(&img, R_T0, R_T2, CACHEERR_ERROREPC_OFF);
        emit_mfc0(&img, R_T0, CACHEERR_CP0_EPC);
        emit_sd(&img, R_T0, R_T2, CACHEERR_EPC_OFF);
        emit_mfc0(&img, R_T0, CACHEERR_CP0_CAUSE);
        emit_sd(&img, R_T0, R_T2, CACHEERR_CAUSE_OFF);
        emit(&img, OPC_SYNC);
        emit_ld(&img, R_T0, R_T2, CACHEERR_HEARTBEAT_OFF);
        emit_addiu(&img, R_T0, R_T0, 1);
        emit_sd(&img, R_T0, R_T2, CACHEERR_HEARTBEAT_OFF);
        emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, snapshot_pc);
        emit_nop(&img);
    }

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
    return park_pc;
}

static void wait_for_cache_error_completion(QTestState *qts,
                                            uint64_t expected)
{
    SgiWait w;

    sgi_wait_start(&w, "CacheErr fixture completion count %" PRIu64,
                   expected);
    while (qtest_get_irq_raise_count(qts, 0) < expected) {
        sgi_wait_pause(&w, POLL_INTERVAL_US);
    }
}

static uint64_t wait_for_cache_error_heartbeat(QTestState *qts,
                                               uint64_t previous)
{
    SgiWait w;
    uint64_t heartbeat;

    sgi_wait_start(&w, "CacheErr handler heartbeat after %" PRIu64,
                   previous);
    do {
        heartbeat = state_word(qts, CACHEERR_HEARTBEAT_OFF);
        if (heartbeat == previous) {
            sgi_wait_pause(&w, POLL_INTERVAL_US);
        }
    } while (heartbeat == previous);
    return heartbeat;
}

static void test_r16000_cache_error_entry_and_nested_suppression(void)
{
    static const char fixture_path[] = "/machine/peripheral/exception-fixture";
    g_autofree char *image = NULL;
    QTestState *qts;
    uint64_t initial_status, seeded_status, status;
    uint64_t first_error_epc, first_epc, first_cause, first_status;
    uint64_t heartbeat;
    unsigned int park_pc;
    int fd;

    if (!qtest_has_accel("tcg") ||
        !qtest_has_device("mips-cpu-exception-fixture")) {
        g_test_skip("MIPS CPU exception fixture requires TCG");
        return;
    }

    fd = g_file_open_tmp("mips64-r16000-cache-error-image-XXXXXX",
                         &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    park_pc = cacheerr_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M malta -m 64M -cpu %s -bios %s -accel tcg "
                      "-device mips-cpu-exception-fixture,id=exception-fixture,"
                      "cpu-index=0 -no-reboot -no-shutdown",
                      R16000_CPU, image);
    unlink(image);
    qtest_irq_intercept_out_named(qts, fixture_path, "cache-error-complete");
    wait_for_state(qts, CACHEERR_READY_OFF, READY_MAGIC);
    g_usleep(10 * POLL_INTERVAL_US);

    initial_status = state_word(qts, CACHEERR_INITIAL_STATUS_OFF);
    seeded_status = state_word(qts, CACHEERR_SEEDED_STATUS_OFF);
    g_assert_cmphex(initial_status & STATUS_KSU, ==, 0);
    g_assert_cmphex(seeded_status & (STATUS_ERL | CACHEERR_STATUS_EXL), ==, 0);
    g_assert_cmphex(seeded_status & STATUS_BEV, ==, STATUS_BEV);
    g_assert_cmphex(state_word(qts, CACHEERR_INITIAL_EPC_OFF), ==,
                    CACHEERR_EPC_CANARY);
    g_assert_cmphex(state_word(qts, CACHEERR_INITIAL_ERROREPC_OFF), ==,
                    CACHEERR_ERROREPC_CANARY);

    qtest_set_irq_in(qts, fixture_path, "cache-error", 0, 1);
    qtest_set_irq_in(qts, fixture_path, "cache-error", 0, 0);
    wait_for_cache_error_completion(qts, 1);
    wait_for_state(qts, CACHEERR_READY_OFF, CACHEERR_READY_MAGIC);
    wait_for_cache_error_heartbeat(qts, 0);

    g_assert_cmphex(state_word(qts, CACHEERR_VECTOR_PC_OFF), ==,
                    0xffffffffbfc00308ULL);
    g_assert_cmphex(state_word(qts, CACHEERR_ENTRY_COUNT_OFF), ==, 1);
    status = state_word(qts, CACHEERR_STATUS_OFF);
    g_assert_cmphex(status & (STATUS_ERL | STATUS_BEV), ==,
                    STATUS_ERL | STATUS_BEV);
    g_assert_cmphex(status & CACHEERR_STATUS_EXL, ==,
                    seeded_status & CACHEERR_STATUS_EXL);
    g_assert_cmphex(state_word(qts, CACHEERR_ERROREPC_OFF), ==,
                    0xffffffffbfc00000ULL + park_pc);
    g_assert_cmphex(state_word(qts, CACHEERR_EPC_OFF), ==,
                    state_word(qts, CACHEERR_INITIAL_EPC_OFF));
    g_assert_cmphex(state_word(qts, CACHEERR_CAUSE_OFF) &
                    ~CACHEERR_CAUSE_HW_IP_MASK, ==,
                    state_word(qts, CACHEERR_INITIAL_CAUSE_OFF) &
                    ~CACHEERR_CAUSE_HW_IP_MASK);
    first_error_epc = state_word(qts, CACHEERR_ERROREPC_OFF);
    first_epc = state_word(qts, CACHEERR_EPC_OFF);
    first_cause = state_word(qts, CACHEERR_CAUSE_OFF);
    first_status = status;

    qtest_set_irq_in(qts, fixture_path, "cache-error", 0, 1);
    qtest_set_irq_in(qts, fixture_path, "cache-error", 0, 0);
    wait_for_cache_error_completion(qts, 2);
    heartbeat = state_word(qts, CACHEERR_HEARTBEAT_OFF);
    wait_for_cache_error_heartbeat(qts, heartbeat);

    g_assert_cmphex(state_word(qts, CACHEERR_ENTRY_COUNT_OFF), ==, 1);
    g_assert_cmphex(state_word(qts, CACHEERR_STATUS_OFF), ==, first_status);
    g_assert_cmphex(state_word(qts, CACHEERR_ERROREPC_OFF), ==,
                    first_error_epc);
    g_assert_cmphex(state_word(qts, CACHEERR_EPC_OFF), ==, first_epc);
    /* Asynchronous Malta IRQs can change hardware IP bits between reads. */
    g_assert_cmphex(state_word(qts, CACHEERR_CAUSE_OFF) &
                    ~CACHEERR_CAUSE_HW_IP_MASK, ==,
                    first_cause & ~CACHEERR_CAUSE_HW_IP_MASK);

    qtest_quit(qts);
}

static void scache_write_guest_image(int fd)
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
    llsc_emit_fault_stub(&img);

    img.pos = CODE_START;
    emit_lui(&img, R_S0, STATE_KSEG1_HI);
    emit_mfc0(&img, R_T0, CP0_CONFIG);
    emit_sd(&img, R_T0, R_S0, SCACHE_CONFIG_OFF);

    emit_li64(&img, R_S1, SCACHE_TOP_TAG_ADDR);
    emit_li32(&img, R_T0, SCACHE_TAGLO);
    emit_mtc0(&img, R_T0, CP0_TAGLO);
    emit_li32(&img, R_T0, SCACHE_TAGHI);
    emit_mtc0(&img, R_T0, CP0_TAGHI);
    emit_cache(&img, CACHE_S_INDEX_STORE_TAG, R_S1);
    emit_mtc0(&img, R_ZERO, CP0_TAGLO);
    emit_mtc0(&img, R_ZERO, CP0_TAGHI);
    emit_cache(&img, CACHE_S_INDEX_LOAD_TAG, R_S1);
    emit_mfc0(&img, R_T0, CP0_TAGLO);
    emit_sd(&img, R_T0, R_S0, SCACHE_TAGLO_OFF);
    emit_mfc0(&img, R_T0, CP0_TAGHI);
    emit_sd(&img, R_T0, R_S0, SCACHE_TAGHI_OFF);

    emit_li64(&img, R_S1, SCACHE_TOP_DATA_ADDR);
    emit_li32(&img, R_T0, SCACHE_DATA_LO);
    emit_mtc0(&img, R_T0, CP0_TAGLO);
    emit_li32(&img, R_T0, SCACHE_DATA_HI);
    emit_mtc0(&img, R_T0, CP0_TAGHI);
    emit_addiu(&img, R_T0, R_ZERO, SCACHE_ECC);
    emit_mtc0(&img, R_T0, CP0_ECC);
    emit_cache(&img, CACHE_S_INDEX_STORE_DATA, R_S1);
    emit_mtc0(&img, R_ZERO, CP0_TAGLO);
    emit_mtc0(&img, R_ZERO, CP0_TAGHI);
    emit_mtc0(&img, R_ZERO, CP0_ECC);
    emit_cache(&img, CACHE_S_INDEX_LOAD_DATA, R_S1);
    emit_mfc0(&img, R_T0, CP0_TAGLO);
    emit_sd(&img, R_T0, R_S0, SCACHE_DATA_LO_OFF);
    emit_mfc0(&img, R_T0, CP0_TAGHI);
    emit_sd(&img, R_T0, R_S0, SCACHE_DATA_HI_OFF);
    emit_mfc0(&img, R_T0, CP0_ECC);
    emit_sd(&img, R_T0, R_S0, SCACHE_ECC_OFF);

    emit_report_and_spin(&img, LLSC_DONE_MAGIC, LLSC_DONE_OFF);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static void test_r10k_scache_reset_mode_capacity(void)
{
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("mips64-r10k-scache-image-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    scache_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M malta -m 64M "
                      "-cpu R10000,reset-mode=0x%x -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", SCACHE_MODE_16M, image);
    unlink(image);
    wait_for_done(qts);

    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + SCACHE_CONFIG_OFF) &
                    0x01ffffff, ==, SCACHE_MODE_16M);
    g_assert_cmphex((uint32_t)qtest_readq(qts, STATE_PHYS + SCACHE_TAGLO_OFF),
                    ==, SCACHE_TAGLO);
    g_assert_cmphex((uint32_t)qtest_readq(qts, STATE_PHYS + SCACHE_TAGHI_OFF),
                    ==, SCACHE_TAGHI);
    g_assert_cmphex((uint32_t)qtest_readq(qts,
                                          STATE_PHYS + SCACHE_DATA_LO_OFF),
                    ==, SCACHE_DATA_LO);
    g_assert_cmphex((uint32_t)qtest_readq(qts,
                                          STATE_PHYS + SCACHE_DATA_HI_OFF),
                    ==, SCACHE_DATA_HI);
    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + SCACHE_ECC_OFF), ==,
                    SCACHE_ECC);
    qtest_quit(qts);
}

/*
 * The line size picks the secondary-cache set an index operation names.
 * With 128-byte lines an address and the one 64 bytes above it share a
 * set, so the second Index Store Tag replaces the first; with 64-byte
 * lines they are neighbouring sets and each keeps its own tag. The model
 * works this geometry out once per processor, so both widths are pinned.
 */
#define SCACHE_LINE_A_ADDR      0xffffffff80001000ULL /* way 0 */
#define SCACHE_LINE_TAG_A       0x5a5a4c00u
#define SCACHE_LINE_TAG_B       0x2a5a0c00u
#define SCACHE_LINE_A_OFF       0x30
#define SCACHE_LINE_B_OFF       0x38

typedef struct ScacheLineCase {
    const char *path;
    const char *cpu;
    uint32_t expect_a;
    /* The second index address; zero for SCACHE_LINE_A_ADDR + 64. */
    uint64_t second;
} ScacheLineCase;

static void scache_line_write_guest_image(int fd, uint64_t second)
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
    llsc_emit_fault_stub(&img);

    img.pos = CODE_START;
    emit_lui(&img, R_S0, STATE_KSEG1_HI);
    emit_mtc0(&img, R_ZERO, CP0_TAGHI);
    emit_li64(&img, R_S1, SCACHE_LINE_A_ADDR);
    emit_li32(&img, R_T0, SCACHE_LINE_TAG_A);
    emit_mtc0(&img, R_T0, CP0_TAGLO);
    emit_cache(&img, CACHE_S_INDEX_STORE_TAG, R_S1);
    emit_li64(&img, R_S2, second ? second : SCACHE_LINE_A_ADDR + 64);
    emit_li32(&img, R_T0, SCACHE_LINE_TAG_B);
    emit_mtc0(&img, R_T0, CP0_TAGLO);
    emit_cache(&img, CACHE_S_INDEX_STORE_TAG, R_S2);

    emit_mtc0(&img, R_ZERO, CP0_TAGLO);
    emit_cache(&img, CACHE_S_INDEX_LOAD_TAG, R_S1);
    emit_mfc0(&img, R_T0, CP0_TAGLO);
    emit_sd(&img, R_T0, R_S0, SCACHE_LINE_A_OFF);
    emit_mtc0(&img, R_ZERO, CP0_TAGLO);
    emit_cache(&img, CACHE_S_INDEX_LOAD_TAG, R_S2);
    emit_mfc0(&img, R_T0, CP0_TAGLO);
    emit_sd(&img, R_T0, R_S0, SCACHE_LINE_B_OFF);

    emit_report_and_spin(&img, LLSC_DONE_MAGIC, LLSC_DONE_OFF);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static void test_r10k_scache_line_size(gconstpointer opaque)
{
    const ScacheLineCase *c = opaque;
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("mips64-r10k-scache-line-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    scache_line_write_guest_image(fd, c->second);
    close(fd);

    qts = qtest_initf("-M malta -m 64M -cpu %s -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", c->cpu, image);
    unlink(image);
    wait_for_done(qts);

    g_assert_cmphex((uint32_t)qtest_readq(qts,
                                          STATE_PHYS + SCACHE_LINE_A_OFF),
                    ==, c->expect_a);
    g_assert_cmphex((uint32_t)qtest_readq(qts,
                                          STATE_PHYS + SCACHE_LINE_B_OFF),
                    ==, SCACHE_LINE_TAG_B);
    qtest_quit(qts);
}

static const ScacheLineCase scache_line_cases[] = {
    { "/r10k/scache/line-size/128", "R10000", SCACHE_LINE_TAG_B },
    {
        "/r10k/scache/line-size/64",
        "R12000,r12000-prid=0xe12,r12000-fpu-id=0x4567,"
        "r12000-reset-mode=0x198683,r12000-scache-bytes=1048576,"
        "r12000-scache-block-words=16",
        SCACHE_LINE_TAG_A,
    },
    /*
     * The R10000's capacity follows Config SS from its reset mode, here
     * 2 MiB against 1 MiB. Way 0's sets 0x20 and 0x1020 are two sets of a
     * 2 MiB cache, and the same one of a 1 MiB cache, which wraps at
     * 0x1000; only a cache sized from SS keeps both tags.
     */
    {
        "/r10k/scache/capacity/2m", "R10000,reset-mode=0x12a785",
        SCACHE_LINE_TAG_A, SCACHE_LINE_A_ADDR + 0x80000,
    },
    {
        "/r10k/scache/capacity/1m", "R10000,reset-mode=0x11a785",
        SCACHE_LINE_TAG_B, SCACHE_LINE_A_ADDR + 0x80000,
    },
};

/*
 * tlb-asid: an ASID change must leave behind no translation the processor
 * would not make under the new ASID, while global entries and the unmapped
 * segments keep translating. QEMU's own TLB caches translations per mode,
 * not per ASID, so each case below is one a stale cached translation would
 * get wrong. Every load lands in a result slot; a recording handler at
 * every exception vector logs each fault and resumes after the faulting
 * instruction, so one guest walks all the cases. The address space
 * identifier, global bit and TLB instruction semantics come from the
 * R10000 User's Manual version 2.0, chapter 14 (EntryHi, EntryLo, Status,
 * PageMask, Wired) and the TLB instruction descriptions.
 */
#define ASID_IMAGE_SIZE      0x2000
#define ASID_MAIN            0x400
#define ASID_DONE_OFF        0x00
#define ASID_FCOUNT_OFF      0x08
#define ASID_RESULT_OFF      0x100
#define ASID_FAULT_OFF       0x400
#define ASID_FAULT_STRIDE    0x20
#define ASID_DONE_MAGIC      0x41534944 /* "ASID" */
#define ASID_SENTINEL        0x0dea

#define OPC_TLBP             0x42000008u
#define OPC_TLBR             0x42000001u
#define CP0_BADVADDR         8
#define R_V0                 2
#define R_S3                 19
#define R_S4                 20
#define R_T9                 25
#define R_K1                 27
#define STATUS_UX            (1u << 5)
#define STATUS_SX            (1u << 6)
#define STATUS_KX            (1u << 7)

/*
 * EntryLo: cacheable noncoherent (C=3), dirty, valid; global adds G. Not
 * uncached: a mapped uncached reference carries the uncached attribute out
 * to a window Malta does not decode.
 */
#define ASID_LO(pfn)         (((uint32_t)(pfn) << 6) | 0x1e)
#define ASID_LO_G(pfn)       (ASID_LO(pfn) | 1)
#define ASID_PAGEMASK_1M     0x001fe000u

/* Physical pages, each holding its own tag in its first doubleword. */
#define ASID_P1              0x200000u  /* ASID 1 at VA_A */
#define ASID_P2              0x202000u  /* ASID 2 at VA_A */
#define ASID_P3              0x204000u  /* global at VA_G, xuseg at VA_X */
#define ASID_P4              0x206000u  /* ASID 1 at VA_A after remap */
#define ASID_P5              0x208000u  /* ASID 2 at VA_B, then replaced */
#define ASID_P6              0x20a000u  /* ASID 1 code at VA_CODE */
#define ASID_P7              0x20c000u  /* ASID 2 code at VA_CODE */
#define ASID_P8              0x20e000u  /* ASID 1 and 2 user code */
#define ASID_BIG1            0x400000u  /* ASID 1 1 MiB pair at VA_BIG */
#define ASID_BIG2            0x600000u  /* ASID 2 1 MiB pair at VA_BIG */

#define ASID_TAG_P1          0x1111
#define ASID_TAG_P2          0x2222
#define ASID_TAG_P3          0x3333
#define ASID_TAG_P4          0x4444
#define ASID_TAG_P5          0x5555
#define ASID_TAG_STORE       0x7777
#define ASID_TAG_BIG1        0x1000
#define ASID_TAG_BIG2        0x2000

#define ASID_VA_A            0x00020000u
#define ASID_VA_B            0x00060000u
#define ASID_VA_G            0x00040000u
#define ASID_VA_CODE         0x00100000u
#define ASID_VA_USER         0x00300000u
#define ASID_VA_BIG          0x01000000u
#define ASID_VA_X            0x0000000100000000ULL  /* xuseg, needs UX */
#define ASID_VA_K            0xc000000000010000ULL  /* xkseg, needs KX */
#define ASID_VA_S            0x4000000000010000ULL  /* xsseg, needs SX */
/* xkphys, cacheable noncoherent: kernel mode needs KX, no TLB entry */
#define ASID_VA_PHYS         (0x9800000000000000ULL | ASID_P1)
#define ASID_BIG_PAGES       300  /* more than the model lists per ASID */
#define ASID_SMALL_PAGES     100
#define ASID_CODE_ROUNDS     8

enum {
    ASID_R_A1, ASID_R_A2, ASID_R_A1_AGAIN,
    ASID_R_G_A1, ASID_R_G_A2,
    ASID_R_A1_AFTER_STORE, ASID_R_P2_PHYS, ASID_R_LOOP_SUM,
    ASID_R_REMAP_A1, ASID_R_REMAP_A2,
    ASID_R_REPLACED, ASID_R_WIRED_A1, ASID_R_WIRED_A2,
    ASID_R_TLBP_HIT, ASID_R_TLBP_MISS, ASID_R_TLBR, ASID_R_NO_ENTRY,
    ASID_R_BIG_A1, ASID_R_BIG_A2, ASID_R_BIG_A1_AGAIN,
    ASID_R_SMALL_A2, ASID_R_SMALL_A1, ASID_R_SMALL_A2_AGAIN,
    ASID_R_SX_SET, ASID_R_SX_CLEAR,
    ASID_R_XKPHYS_SET, ASID_R_XKPHYS_CLEAR,
    ASID_R_UX_SET, ASID_R_UX_CLEAR, ASID_R_UX_RESET,
    ASID_R_KX_SET, ASID_R_KX_CLEAR, ASID_R_KX_RESET,
    ASID_R_CODE_A1, ASID_R_CODE_A2, ASID_R_CODE_LOOP,
    ASID_R_USER_A1, ASID_R_USER_A2,
    ASID_R_COUNT
};

static uint32_t mips_dmfc0(int rt, int rd)
{
    return MIPS_OPC_CP0 | (0x01u << 21) | (rt << 16) | (rd << 11);
}

/*
 * Log the vector, Cause, BadVAddr and EPC, then resume after EPC. At the
 * general vector a system call is not logged: it is how the user-mode leg
 * comes back, to the kernel address s7 holds.
 */
static void emit_asid_fault_handler(GuestImage *img, uint16_t vec_id)
{
    if (vec_id == 4) {
        unsigned int branch, record;

        emit_mfc0(img, R_K0, CP0_CAUSE);
        emit(img, mips_andi(R_K0, R_K0, 0x7c));
        emit_addiu(img, R_K1, R_ZERO, 8 << 2);
        branch = img->pos;
        emit_nop(img);
        emit_nop(img);
        emit(img, mips_dmtc0(R_S7, CP0_EPC));
        emit_mfc0(img, R_K0, CP0_STATUS);
        emit_addiu(img, R_K1, R_ZERO, ~0x18);
        emit_and(img, R_K0, R_K0, R_K1);
        emit_mtc0(img, R_K0, CP0_STATUS);
        emit(img, OPC_ERET);
        record = img->pos;
        img->pos = branch;
        emit_branch(img, OPC_BNE, R_K0, R_K1, record);
        img->pos = record;
    }
    emit_lui(img, R_K0, STATE_KSEG1_HI);
    emit_ld(img, R_K1, R_K0, ASID_FCOUNT_OFF);
    emit(img, mips_daddiu(R_K1, R_K1, 1));
    emit_sd(img, R_K1, R_K0, ASID_FCOUNT_OFF);
    emit(img, mips_dsll(R_K1, R_K1, 5));
    emit(img, mips_daddu(R_K1, R_K1, R_K0));
    emit_addiu(img, R_K0, R_ZERO, vec_id);
    emit_sd(img, R_K0, R_K1, ASID_FAULT_OFF - ASID_FAULT_STRIDE);
    emit_mfc0(img, R_K0, CP0_CAUSE);
    emit_sd(img, R_K0, R_K1, ASID_FAULT_OFF - ASID_FAULT_STRIDE + 8);
    emit(img, mips_dmfc0(R_K0, CP0_BADVADDR));
    emit_sd(img, R_K0, R_K1, ASID_FAULT_OFF - ASID_FAULT_STRIDE + 16);
    emit(img, mips_dmfc0(R_K0, CP0_EPC));
    emit_sd(img, R_K0, R_K1, ASID_FAULT_OFF - ASID_FAULT_STRIDE + 24);
    emit(img, mips_daddiu(R_K0, R_K0, 4));
    emit(img, mips_dmtc0(R_K0, CP0_EPC));
    emit_ehb(img);
    emit(img, OPC_ERET);
    emit_nop(img);
}

static void emit_asid_result(GuestImage *img, int rt, int slot)
{
    emit_lui(img, R_T9, STATE_KSEG1_HI);
    emit_sd(img, rt, R_T9, ASID_RESULT_OFF + 8 * slot);
}

/* EntryHi with a zero page number: only its ASID matters here. */
static void emit_asid_set(GuestImage *img, uint16_t asid)
{
    emit_addiu(img, R_T0, R_ZERO, asid);
    emit_mtc0(img, R_T0, CP0_ENTRYHI);
    emit_ehb(img);
}

/* Load the doubleword at @va into @rt, which first holds the sentinel. */
static void emit_asid_load(GuestImage *img, int rt, uint64_t va)
{
    emit_addiu(img, rt, R_ZERO, ASID_SENTINEL);
    emit_li64(img, R_T1, va);
    emit_ld(img, rt, R_T1, 0);
}

static void emit_asid_poke(GuestImage *img, uint32_t phys, uint32_t value)
{
    emit_li64(img, R_T0, 0xffffffffa0000000ULL | phys);
    emit_li32(img, R_T1, value);
    emit_sd(img, R_T1, R_T0, 0);
}

static void emit_asid_poke_word(GuestImage *img, uint32_t phys,
                                uint32_t insn)
{
    emit_li64(img, R_T0, 0xffffffffa0000000ULL | phys);
    emit_li32(img, R_T1, insn);
    emit(img, mips_sw(R_T1, R_T0, 0));
}

static void emit_asid_tlb_entry(GuestImage *img, int index, uint64_t entryhi,
                                uint32_t lo0, uint32_t lo1,
                                uint32_t pagemask)
{
    emit_li32(img, R_T0, pagemask);
    emit_mtc0(img, R_T0, CP0_PAGEMASK);
    emit_li64(img, R_T0, entryhi);
    emit(img, mips_dmtc0(R_T0, CP0_ENTRYHI));
    emit_li32(img, R_T0, lo0);
    emit_mtc0(img, R_T0, CP0_ENTRYLO0);
    emit_li32(img, R_T0, lo1);
    emit_mtc0(img, R_T0, CP0_ENTRYLO1);
    emit_addiu(img, R_T0, R_ZERO, index);
    emit_mtc0(img, R_T0, CP0_INDEX);
    emit_ehb(img);
    emit(img, OPC_TLBWI);
    emit_ehb(img);
}

static void emit_asid_status(GuestImage *img, bool set, uint32_t bits)
{
    emit_mfc0(img, R_T0, CP0_STATUS);
    if (set) {
        emit_ori(img, R_T0, R_T0, bits);
    } else {
        emit_addiu(img, R_T1, R_ZERO, (int16_t)~bits);
        emit_and(img, R_T0, R_T0, R_T1);
    }
    emit_mtc0(img, R_T0, CP0_STATUS);
    emit_ehb(img);
}

/* Tag the first doubleword of @count pages from @phys with base + i. */
static void emit_asid_tag_pages(GuestImage *img, uint32_t phys, int count,
                                int16_t base)
{
    unsigned int loop;

    emit_li64(img, R_T0, 0xffffffffa0000000ULL | phys);
    emit_addiu(img, R_T1, R_ZERO, 0);
    emit_addiu(img, R_T2, R_ZERO, count);
    loop = img->pos;
    emit(img, mips_daddiu(R_T3, R_T1, base));
    emit_sd(img, R_T3, R_T0, 0);
    emit(img, mips_daddiu(R_T0, R_T0, 0x1000));
    emit(img, mips_daddiu(R_T1, R_T1, 1));
    emit_branch(img, OPC_BNE, R_T1, R_T2, loop);
    emit_nop(img);
}

/*
 * Sum the first doubleword of @count pages from @va into result @slot,
 * last page first: a run longer than QEMU's TLB leaves its last pages
 * resident, so a stale one is met before anything evicts it.
 */
static void emit_asid_sum_pages(GuestImage *img, uint64_t va, int count,
                                int slot)
{
    unsigned int loop;

    emit_li64(img, R_T0, va + (uint64_t)(count - 1) * 0x1000);
    emit_addiu(img, R_T1, R_ZERO, 0);
    emit_addiu(img, R_T2, R_ZERO, count);
    emit_addiu(img, R_S0, R_ZERO, 0);
    loop = img->pos;
    emit_ld(img, R_T3, R_T0, 0);
    emit(img, mips_daddu(R_S0, R_S0, R_T3));
    emit(img, mips_daddiu(R_T0, R_T0, -0x1000));
    emit(img, mips_daddiu(R_T1, R_T1, 1));
    emit_branch(img, OPC_BNE, R_T1, R_T2, loop);
    emit_nop(img);
    emit_asid_result(img, R_S0, slot);
}

static void emit_asid_call_code(GuestImage *img)
{
    emit_li64(img, R_T1, ASID_VA_CODE);
    emit_jalr(img, R_RA, R_T1);
    emit_nop(img);
}

static void asid_write_guest_image(int fd)
{
    uint8_t *image = g_malloc0(ASID_IMAGE_SIZE);
    GuestImage img = { .code = image, .pos = 0, .size = ASID_IMAGE_SIZE };
    unsigned int loop;
    ssize_t written;

    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, ASID_MAIN);
    emit_nop(&img);
    img.pos = VEC_TLB_REFILL;
    emit_asid_fault_handler(&img, 1);
    img.pos = VEC_XTLB_REFILL;
    emit_asid_fault_handler(&img, 2);
    img.pos = VEC_CACHE_ERROR;
    emit_asid_fault_handler(&img, 3);
    img.pos = VEC_GENERAL;
    emit_asid_fault_handler(&img, 4);
    g_assert_cmpuint(img.pos, <=, ASID_MAIN);

    img.pos = ASID_MAIN;
    emit_mfc0(&img, R_T0, CP0_STATUS);
    emit_addiu(&img, R_T1, R_ZERO, STATUS_ERL_CLEAR);
    emit_and(&img, R_T0, R_T0, R_T1);
    emit_mtc0(&img, R_T0, CP0_STATUS);
    emit_mtc0(&img, R_ZERO, CP0_WIRED);
    emit_ehb(&img);

    emit_asid_poke(&img, ASID_P1, ASID_TAG_P1);
    emit_asid_poke(&img, ASID_P2, ASID_TAG_P2);
    emit_asid_poke(&img, ASID_P3, ASID_TAG_P3);
    emit_asid_poke(&img, ASID_P4, ASID_TAG_P4);
    emit_asid_poke(&img, ASID_P5, ASID_TAG_P5);
    /* At VA_CODE: addiu v0, zero, ASID; jr ra; nop. */
    emit_asid_poke_word(&img, ASID_P6, mips_addiu(R_V0, R_ZERO, 1));
    emit_asid_poke_word(&img, ASID_P6 + 4, OPC_JR | (R_RA << 21));
    emit_asid_poke_word(&img, ASID_P6 + 8, 0);
    emit_asid_poke_word(&img, ASID_P7, mips_addiu(R_V0, R_ZERO, 2));
    emit_asid_poke_word(&img, ASID_P7 + 4, OPC_JR | (R_RA << 21));
    emit_asid_poke_word(&img, ASID_P7 + 8, 0);
    emit_asid_tag_pages(&img, ASID_BIG1, ASID_BIG_PAGES, ASID_TAG_BIG1);
    emit_asid_tag_pages(&img, ASID_BIG2, ASID_BIG_PAGES, ASID_TAG_BIG2);

    emit_asid_tlb_entry(&img, 0, ASID_VA_A | 1, ASID_LO(ASID_P1 >> 12), 0, 0);
    emit_asid_tlb_entry(&img, 1, ASID_VA_A | 2, ASID_LO(ASID_P2 >> 12), 0, 0);
    emit_asid_tlb_entry(&img, 2, ASID_VA_G | 1, ASID_LO_G(ASID_P3 >> 12),
                        ASID_LO_G((ASID_P3 >> 12) + 1), 0);

    /*
     * One straight run, so one translation block: a write to EntryHi must
     * take effect for the very next load, with no branch in between.
     */
    emit_asid_set(&img, 1);
    emit_asid_load(&img, R_S0, ASID_VA_A);
    emit_asid_set(&img, 2);
    emit_asid_load(&img, R_S1, ASID_VA_A);
    emit_asid_set(&img, 1);
    emit_asid_load(&img, R_S2, ASID_VA_A);
    emit_asid_load(&img, R_S3, ASID_VA_G);
    emit_asid_set(&img, 2);
    emit_asid_load(&img, R_S4, ASID_VA_G);
    emit_asid_result(&img, R_S0, ASID_R_A1);
    emit_asid_result(&img, R_S1, ASID_R_A2);
    emit_asid_result(&img, R_S2, ASID_R_A1_AGAIN);
    emit_asid_result(&img, R_S3, ASID_R_G_A1);
    emit_asid_result(&img, R_S4, ASID_R_G_A2);

    /* A store under ASID 2 reaches only ASID 2's page. */
    emit_li32(&img, R_T2, ASID_TAG_STORE);
    emit_li64(&img, R_T1, ASID_VA_A);
    emit_sd(&img, R_T2, R_T1, 0);
    emit_asid_set(&img, 1);
    emit_asid_load(&img, R_S0, ASID_VA_A);
    emit_asid_result(&img, R_S0, ASID_R_A1_AFTER_STORE);
    emit_asid_load(&img, R_S0, 0xffffffffa0000000ULL | ASID_P2);
    emit_asid_result(&img, R_S0, ASID_R_P2_PHYS);

    /* Alternate many times, so the loop's own translations are reused. */
    emit_addiu(&img, R_S0, R_ZERO, 0);
    emit_addiu(&img, R_S1, R_ZERO, 16);
    loop = img.pos;
    emit_asid_set(&img, 1);
    emit_asid_load(&img, R_T2, ASID_VA_A);
    emit(&img, mips_daddu(R_S0, R_S0, R_T2));
    emit_asid_set(&img, 2);
    emit_asid_load(&img, R_T2, ASID_VA_A);
    emit(&img, mips_daddu(R_S0, R_S0, R_T2));
    emit_addiu(&img, R_S1, R_S1, -1);
    emit_branch(&img, OPC_BNE, R_S1, R_ZERO, loop);
    emit_nop(&img);
    emit_asid_result(&img, R_S0, ASID_R_LOOP_SUM);

    /* Remap ASID 1's entry while ASID 2's translation is the cached one. */
    emit_asid_tlb_entry(&img, 0, ASID_VA_A | 1, ASID_LO(ASID_P4 >> 12), 0, 0);
    emit_asid_set(&img, 2);
    emit_asid_load(&img, R_S1, ASID_VA_A);
    emit_asid_set(&img, 1);
    emit_asid_load(&img, R_S0, ASID_VA_A);
    emit_asid_result(&img, R_S0, ASID_R_REMAP_A1);
    emit_asid_result(&img, R_S1, ASID_R_REMAP_A2);

    /*
     * A random write that replaces another ASID's entry leaves nothing
     * behind: with Wired at the last index, Random names that index.
     * Fault 1 is the refill when ASID 2 reaches for its page again.
     */
    emit_addiu(&img, R_T0, R_ZERO, 63);
    emit_mtc0(&img, R_T0, CP0_WIRED);
    emit_asid_tlb_entry(&img, 63, ASID_VA_B | 2, ASID_LO(ASID_P5 >> 12),
                        0, 0);
    emit_asid_load(&img, R_S0, ASID_VA_B);
    emit_asid_set(&img, 1);
    emit_li64(&img, R_T0, 0x00080000 | 1);
    emit(&img, mips_dmtc0(R_T0, CP0_ENTRYHI));
    emit_li32(&img, R_T0, ASID_LO(ASID_P3 >> 12));
    emit_mtc0(&img, R_T0, CP0_ENTRYLO0);
    emit_mtc0(&img, R_ZERO, CP0_ENTRYLO1);
    emit_ehb(&img);
    emit(&img, OPC_TLBWR);
    emit_ehb(&img);
    emit_asid_set(&img, 2);
    emit_asid_load(&img, R_S0, ASID_VA_B);
    emit_asid_result(&img, R_S0, ASID_R_REPLACED);

    /* Random writes leave the wired entries alone. */
    emit_addiu(&img, R_T0, R_ZERO, 10);
    emit_mtc0(&img, R_T0, CP0_WIRED);
    for (int i = 0; i < 4; i++) {
        emit_li64(&img, R_T0, (0x00200000 + 0x2000 * i) | 1);
        emit(&img, mips_dmtc0(R_T0, CP0_ENTRYHI));
        emit_li32(&img, R_T0, ASID_LO(ASID_P3 >> 12));
        emit_mtc0(&img, R_T0, CP0_ENTRYLO0);
        emit_ehb(&img);
        emit(&img, OPC_TLBWR);
        emit_ehb(&img);
    }
    emit_asid_set(&img, 1);
    emit_asid_load(&img, R_S0, ASID_VA_A);
    emit_asid_set(&img, 2);
    emit_asid_load(&img, R_S1, ASID_VA_A);
    emit_asid_result(&img, R_S0, ASID_R_WIRED_A1);
    emit_asid_result(&img, R_S1, ASID_R_WIRED_A2);
    emit_mtc0(&img, R_ZERO, CP0_WIRED);

    /* TLBP matches by ASID. */
    emit_li64(&img, R_T0, ASID_VA_A | 2);
    emit(&img, mips_dmtc0(R_T0, CP0_ENTRYHI));
    emit_ehb(&img);
    emit(&img, OPC_TLBP);
    emit_ehb(&img);
    emit_mfc0(&img, R_S0, CP0_INDEX);
    emit_asid_result(&img, R_S0, ASID_R_TLBP_HIT);
    emit_li64(&img, R_T0, ASID_VA_A | 3);
    emit(&img, mips_dmtc0(R_T0, CP0_ENTRYHI));
    emit_ehb(&img);
    emit(&img, OPC_TLBP);
    emit_ehb(&img);
    emit_mfc0(&img, R_S0, CP0_INDEX);
    emit_asid_result(&img, R_S0, ASID_R_TLBP_MISS);

    /* TLBR changes the ASID as an EntryHi write does. */
    emit_asid_set(&img, 1);
    emit_asid_load(&img, R_S0, ASID_VA_A);
    emit_addiu(&img, R_T0, R_ZERO, 1);
    emit_mtc0(&img, R_T0, CP0_INDEX);
    emit_ehb(&img);
    emit(&img, OPC_TLBR);
    emit_ehb(&img);
    emit_asid_load(&img, R_S0, ASID_VA_A);
    emit_asid_result(&img, R_S0, ASID_R_TLBR);

    /* An ASID with no entry refills: fault 2. */
    emit_asid_set(&img, 3);
    emit_asid_load(&img, R_S0, ASID_VA_A);
    emit_asid_result(&img, R_S0, ASID_R_NO_ENTRY);

    /*
     * 1 MiB pages, first over more pages than the model lists per ASID,
     * then over fewer.
     */
    emit_asid_tlb_entry(&img, 4, ASID_VA_BIG | 1, ASID_LO(ASID_BIG1 >> 12),
                        ASID_LO((ASID_BIG1 >> 12) + 0x100), ASID_PAGEMASK_1M);
    emit_asid_tlb_entry(&img, 5, ASID_VA_BIG | 2, ASID_LO(ASID_BIG2 >> 12),
                        ASID_LO((ASID_BIG2 >> 12) + 0x100), ASID_PAGEMASK_1M);
    emit_mtc0(&img, R_ZERO, CP0_PAGEMASK);
    emit_asid_set(&img, 1);
    emit_asid_sum_pages(&img, ASID_VA_BIG, ASID_BIG_PAGES, ASID_R_BIG_A1);
    emit_asid_set(&img, 2);
    emit_asid_sum_pages(&img, ASID_VA_BIG, ASID_BIG_PAGES, ASID_R_BIG_A2);
    emit_asid_set(&img, 1);
    emit_asid_sum_pages(&img, ASID_VA_BIG, ASID_BIG_PAGES,
                        ASID_R_BIG_A1_AGAIN);
    emit_asid_set(&img, 2);
    emit_asid_sum_pages(&img, ASID_VA_BIG, ASID_SMALL_PAGES,
                        ASID_R_SMALL_A2);
    emit_asid_set(&img, 1);
    emit_asid_sum_pages(&img, ASID_VA_BIG, ASID_SMALL_PAGES,
                        ASID_R_SMALL_A1);
    emit_asid_set(&img, 2);
    emit_asid_sum_pages(&img, ASID_VA_BIG, ASID_SMALL_PAGES,
                        ASID_R_SMALL_A2_AGAIN);

    /*
     * Status SX reaches xsseg, and KX reaches xkphys in kernel mode. Each
     * load with its enable clear is an address error: faults 3 and 4. This
     * runs before the xkseg cases below, which leave a KX-dependent page
     * cached in the kernel index and would hide a missing xkphys record.
     */
    emit_asid_tlb_entry(&img, 12, ASID_VA_S | 1, ASID_LO(ASID_P3 >> 12), 0, 0);
    emit_asid_set(&img, 1);
    emit_asid_status(&img, true, STATUS_SX);
    emit_asid_load(&img, R_S0, ASID_VA_S);
    emit_asid_status(&img, false, STATUS_SX);
    emit_asid_load(&img, R_S1, ASID_VA_S);
    emit_asid_result(&img, R_S0, ASID_R_SX_SET);
    emit_asid_result(&img, R_S1, ASID_R_SX_CLEAR);
    emit_asid_status(&img, true, STATUS_KX);
    emit_asid_load(&img, R_S0, ASID_VA_PHYS);
    emit_asid_status(&img, false, STATUS_KX);
    emit_asid_load(&img, R_S1, ASID_VA_PHYS);
    emit_asid_result(&img, R_S0, ASID_R_XKPHYS_SET);
    emit_asid_result(&img, R_S1, ASID_R_XKPHYS_CLEAR);

    /*
     * Status UX reaches xuseg above 2 Gbytes and KX reaches xkseg. Each
     * load with its enable clear is an address error: faults 5 and 6.
     */
    emit_asid_tlb_entry(&img, 6, ASID_VA_X | 1, ASID_LO(ASID_P3 >> 12), 0, 0);
    emit_asid_tlb_entry(&img, 7, ASID_VA_K | 1, ASID_LO_G(ASID_P1 >> 12),
                        ASID_LO_G((ASID_P1 >> 12) + 1), 0);
    emit_asid_set(&img, 1);
    emit_asid_status(&img, true, STATUS_UX);
    emit_asid_load(&img, R_S0, ASID_VA_X);
    emit_asid_status(&img, false, STATUS_UX);
    emit_asid_load(&img, R_S1, ASID_VA_X);
    emit_asid_status(&img, true, STATUS_UX);
    emit_asid_load(&img, R_S2, ASID_VA_X);
    emit_asid_result(&img, R_S0, ASID_R_UX_SET);
    emit_asid_result(&img, R_S1, ASID_R_UX_CLEAR);
    emit_asid_result(&img, R_S2, ASID_R_UX_RESET);
    emit_asid_status(&img, true, STATUS_KX);
    emit_asid_load(&img, R_S0, ASID_VA_K);
    emit_asid_status(&img, false, STATUS_KX);
    emit_asid_load(&img, R_S1, ASID_VA_K);
    emit_asid_status(&img, true, STATUS_KX);
    emit_asid_load(&img, R_S2, ASID_VA_K);
    emit_asid_result(&img, R_S0, ASID_R_KX_SET);
    emit_asid_result(&img, R_S1, ASID_R_KX_CLEAR);
    emit_asid_result(&img, R_S2, ASID_R_KX_RESET);

    /* Code fetched through one virtual page under two ASIDs. */
    emit_asid_tlb_entry(&img, 8, ASID_VA_CODE | 1, ASID_LO(ASID_P6 >> 12),
                        0, 0);
    emit_asid_tlb_entry(&img, 9, ASID_VA_CODE | 2, ASID_LO(ASID_P7 >> 12),
                        0, 0);
    emit_asid_set(&img, 1);
    emit_addiu(&img, R_V0, R_ZERO, ASID_SENTINEL);
    emit_asid_call_code(&img);
    emit_asid_result(&img, R_V0, ASID_R_CODE_A1);
    emit_asid_set(&img, 2);
    emit_addiu(&img, R_V0, R_ZERO, ASID_SENTINEL);
    emit_asid_call_code(&img);
    emit_asid_result(&img, R_V0, ASID_R_CODE_A2);
    emit_addiu(&img, R_S0, R_ZERO, 0);
    emit_addiu(&img, R_S1, R_ZERO, ASID_CODE_ROUNDS);
    loop = img.pos;
    emit_asid_set(&img, 1);
    emit_asid_call_code(&img);
    emit(&img, mips_daddu(R_S0, R_S0, R_V0));
    emit_asid_set(&img, 2);
    emit_asid_call_code(&img);
    emit(&img, mips_daddu(R_S0, R_S0, R_V0));
    emit_addiu(&img, R_S1, R_S1, -1);
    emit_branch(&img, OPC_BNE, R_S1, R_ZERO, loop);
    emit_nop(&img);
    emit_asid_result(&img, R_S0, ASID_R_CODE_LOOP);

    /*
     * The kernel and user mode cache the same page separately: an ASID
     * change must discard both. User code at VA_USER loads the low word at
     * a0 into v1 and returns through a system call.
     */
    emit_asid_poke_word(&img, ASID_P8, mips_lw(3, R_A0, 4));
    emit_asid_poke_word(&img, ASID_P8 + 4, OPC_SYSCALL);
    emit_asid_poke_word(&img, ASID_P8 + 8, 0);
    emit_asid_tlb_entry(&img, 10, ASID_VA_USER | 1, ASID_LO(ASID_P8 >> 12),
                        0, 0);
    emit_asid_tlb_entry(&img, 11, ASID_VA_USER | 2, ASID_LO(ASID_P8 >> 12),
                        0, 0);
    for (int asid = 1; asid <= 2; asid++) {
        unsigned int cont_li;

        emit_asid_set(&img, asid);
        emit_asid_load(&img, R_S0, ASID_VA_A);
        emit_li64(&img, R_A0, ASID_VA_A);
        emit_addiu(&img, 3, R_ZERO, ASID_SENTINEL);
        cont_li = img.pos;
        emit_nop(&img);
        emit_nop(&img);
        emit_li64(&img, R_T0, ASID_VA_USER);
        emit(&img, mips_dmtc0(R_T0, CP0_EPC));
        emit_mfc0(&img, R_T0, CP0_STATUS);
        emit_ori(&img, R_T0, R_T0, 0x12); /* KSU user, EXL */
        emit_mtc0(&img, R_T0, CP0_STATUS);
        emit_ehb(&img);
        emit(&img, OPC_ERET);
        {
            unsigned int cont = img.pos;

            img.pos = cont_li;
            emit_li32(&img, R_S7, 0xbfc00000u + cont);
            img.pos = cont;
        }
        emit_asid_result(&img, 3, asid == 1 ? ASID_R_USER_A1 :
                                              ASID_R_USER_A2);
    }

    emit_lui(&img, R_T0, STATE_KSEG1_HI);
    emit_li32(&img, R_T2, ASID_DONE_MAGIC);
    emit_sd(&img, R_T2, R_T0, ASID_DONE_OFF);
    emit(&img, OPC_SYNC);
    loop = img.pos;
    emit_branch(&img, OPC_BEQ, R_ZERO, R_ZERO, loop);
    emit_nop(&img);

    written = write(fd, image, ASID_IMAGE_SIZE);
    g_assert_cmpint(written, ==, ASID_IMAGE_SIZE);
    g_free(image);
}

static uint64_t asid_result(QTestState *qts, int slot)
{
    return qtest_readq(qts, STATE_PHYS + ASID_RESULT_OFF + 8 * slot);
}

static void asid_check_fault(QTestState *qts, int n, uint64_t vec,
                             uint64_t exccode, uint64_t badvaddr)
{
    uint64_t rec = STATE_PHYS + ASID_FAULT_OFF + ASID_FAULT_STRIDE * (n - 1);

    g_assert_cmpuint(qtest_readq(qts, rec), ==, vec);
    g_assert_cmpuint((qtest_readq(qts, rec + 8) >> 2) & 0x1f, ==, exccode);
    g_assert_cmphex(qtest_readq(qts, rec + 16), ==, badvaddr);
}

static uint64_t asid_page_sum(int pages, int base)
{
    return (uint64_t)pages * (pages - 1) / 2 + (uint64_t)pages * base;
}

typedef struct AsidCase {
    const char *path;
    const char *machine;
    const char *cpu;
} AsidCase;

/* Origin 200 carries only the R10000; Malta takes the R14000. */
static const AsidCase asid_cases[] = {
    { "/sn0/tcg/tlb-asid/r10000", "origin200", "R10000" },
    { "/r10k/tlb-asid/r14000", "malta", "R14000" },
};

static void test_r10k_tlb_asid(gconstpointer opaque)
{
    const AsidCase *c = opaque;
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd;
    SgiWait w;

    fd = g_file_open_tmp("mips64-r10k-tlb-asid-XXXXXX", &image, NULL);
    g_assert_cmpint(fd, >=, 0);
    asid_write_guest_image(fd);
    close(fd);

    qts = qtest_initf("-M %s -m 64M -cpu %s -bios %s -accel tcg "
                      "-no-reboot -no-shutdown", c->machine, c->cpu, image);
    unlink(image);

    sgi_wait_start(&w, "guest done");
    while (qtest_readq(qts, STATE_PHYS + ASID_DONE_OFF) == 0) {
        sgi_wait_pause(&w, POLL_INTERVAL_US);
    }
    g_assert_cmphex(qtest_readq(qts, STATE_PHYS + ASID_DONE_OFF), ==,
                    ASID_DONE_MAGIC);

    g_assert_cmphex(asid_result(qts, ASID_R_A1), ==, ASID_TAG_P1);
    g_assert_cmphex(asid_result(qts, ASID_R_A2), ==, ASID_TAG_P2);
    g_assert_cmphex(asid_result(qts, ASID_R_A1_AGAIN), ==, ASID_TAG_P1);
    g_assert_cmphex(asid_result(qts, ASID_R_G_A1), ==, ASID_TAG_P3);
    g_assert_cmphex(asid_result(qts, ASID_R_G_A2), ==, ASID_TAG_P3);
    g_assert_cmphex(asid_result(qts, ASID_R_A1_AFTER_STORE), ==, ASID_TAG_P1);
    g_assert_cmphex(asid_result(qts, ASID_R_P2_PHYS), ==, ASID_TAG_STORE);
    g_assert_cmphex(asid_result(qts, ASID_R_LOOP_SUM), ==,
                    16 * (ASID_TAG_P1 + ASID_TAG_STORE));
    g_assert_cmphex(asid_result(qts, ASID_R_REMAP_A1), ==, ASID_TAG_P4);
    g_assert_cmphex(asid_result(qts, ASID_R_REMAP_A2), ==, ASID_TAG_STORE);
    g_assert_cmphex(asid_result(qts, ASID_R_REPLACED), ==, ASID_SENTINEL);
    g_assert_cmphex(asid_result(qts, ASID_R_WIRED_A1), ==, ASID_TAG_P4);
    g_assert_cmphex(asid_result(qts, ASID_R_WIRED_A2), ==, ASID_TAG_STORE);
    g_assert_cmphex(asid_result(qts, ASID_R_TLBP_HIT), ==, 1);
    g_assert_cmphex(asid_result(qts, ASID_R_TLBP_MISS) & 0x80000000u, !=, 0);
    g_assert_cmphex(asid_result(qts, ASID_R_TLBR), ==, ASID_TAG_STORE);
    g_assert_cmphex(asid_result(qts, ASID_R_NO_ENTRY), ==, ASID_SENTINEL);
    g_assert_cmpuint(asid_result(qts, ASID_R_BIG_A1), ==,
                     asid_page_sum(ASID_BIG_PAGES, ASID_TAG_BIG1));
    g_assert_cmpuint(asid_result(qts, ASID_R_BIG_A2), ==,
                     asid_page_sum(ASID_BIG_PAGES, ASID_TAG_BIG2));
    g_assert_cmpuint(asid_result(qts, ASID_R_BIG_A1_AGAIN), ==,
                     asid_page_sum(ASID_BIG_PAGES, ASID_TAG_BIG1));
    g_assert_cmpuint(asid_result(qts, ASID_R_SMALL_A2), ==,
                     asid_page_sum(ASID_SMALL_PAGES, ASID_TAG_BIG2));
    g_assert_cmpuint(asid_result(qts, ASID_R_SMALL_A1), ==,
                     asid_page_sum(ASID_SMALL_PAGES, ASID_TAG_BIG1));
    g_assert_cmpuint(asid_result(qts, ASID_R_SMALL_A2_AGAIN), ==,
                     asid_page_sum(ASID_SMALL_PAGES, ASID_TAG_BIG2));
    g_assert_cmphex(asid_result(qts, ASID_R_SX_SET), ==, ASID_TAG_P3);
    g_assert_cmphex(asid_result(qts, ASID_R_SX_CLEAR), ==, ASID_SENTINEL);
    g_assert_cmphex(asid_result(qts, ASID_R_XKPHYS_SET), ==, ASID_TAG_P1);
    g_assert_cmphex(asid_result(qts, ASID_R_XKPHYS_CLEAR), ==, ASID_SENTINEL);
    g_assert_cmphex(asid_result(qts, ASID_R_UX_SET), ==, ASID_TAG_P3);
    g_assert_cmphex(asid_result(qts, ASID_R_UX_CLEAR), ==, ASID_SENTINEL);
    g_assert_cmphex(asid_result(qts, ASID_R_UX_RESET), ==, ASID_TAG_P3);
    g_assert_cmphex(asid_result(qts, ASID_R_KX_SET), ==, ASID_TAG_P1);
    g_assert_cmphex(asid_result(qts, ASID_R_KX_CLEAR), ==, ASID_SENTINEL);
    g_assert_cmphex(asid_result(qts, ASID_R_KX_RESET), ==, ASID_TAG_P1);
    g_assert_cmphex(asid_result(qts, ASID_R_CODE_A1), ==, 1);
    g_assert_cmphex(asid_result(qts, ASID_R_CODE_A2), ==, 2);
    g_assert_cmphex(asid_result(qts, ASID_R_CODE_LOOP), ==,
                    ASID_CODE_ROUNDS * (1 + 2));
    g_assert_cmphex(asid_result(qts, ASID_R_USER_A1), ==, ASID_TAG_P4);
    g_assert_cmphex(asid_result(qts, ASID_R_USER_A2), ==, ASID_TAG_STORE);

    /* Refill vector, TLBL; general vector, AdEL. */
    g_assert_cmpuint(qtest_readq(qts, STATE_PHYS + ASID_FCOUNT_OFF), ==, 6);
    asid_check_fault(qts, 1, 1, 2, ASID_VA_B);
    asid_check_fault(qts, 2, 1, 2, ASID_VA_A);
    asid_check_fault(qts, 3, 4, 4, ASID_VA_S);
    asid_check_fault(qts, 4, 4, 4, ASID_VA_PHYS);
    asid_check_fault(qts, 5, 4, 4, ASID_VA_X);
    asid_check_fault(qts, 6, 4, 4, ASID_VA_K);

    qtest_quit(qts);
}

/* R12000A fixtures test capture, never a physical secondary ECC detector. */
#define A_STATE 0x100000
#define A_FIXTURE "/machine/peripheral/a-fixture"

typedef struct ACPU {
    QTestState *qts;
    uint32_t next;
    char *image;
} ACPU;

static char *a_cpu_options(const char *variant, unsigned int ec,
                            unsigned int sc)
{
    uint32_t mode = (0x011a8603 & ~((15u << 9) | (7u << 19))) |
                    (ec << 9) | (sc << 19);

    return g_strdup_printf("R12000,r12000-prid=0xe12,r12000-fpu-id=0x4567,"
        "r12000-reset-mode=0x%x,r12000-scache-bytes=2097152,"
        "r12000-scache-block-words=16%s%s", mode,
        variant ? ",r12000-variant=" : "", variant ? variant : "");
}

static void a_wait_entry(ACPU *a, unsigned int old)
{
    SgiWait wait;

    qtest_qmp_assert_success(a->qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "R12000A synthetic reset entry");
    while (qtest_readl(a->qts, A_STATE + 8) == old) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(a->qts, "{'execute':'stop'}");
    qtest_qmp_eventwait(a->qts, "STOP");
}

static ACPU a_start(const char *cpu, const char *extra, bool run)
{
    static const uint32_t code[] = {
        0x3c1aa010, 0x40088000, 0xaf480020, /* mailbox and Config */
        0x8f480008, 0x25080001, 0xaf480008, /* entry count */
        0x8f5b0000, 0x1360fffe, 0, 0xaf400000, /* request */
        0x0360f809, 0, 0x24080001, 0xaf480004, 0x1000fff7, 0,
    };
    static const uint32_t fault[] = {
        0x40086800, 0xaf48007c, 0x24080001, 0xaf480004, 0x1000ffff, 0,
    };
    ACPU a = { .next = 0x200000 };
    uint8_t image[IMAGE_SIZE] = { 0 };
    g_autofree char *quoted = NULL;
    int fd;

    stl_be_p(image, 0x0bf00010); /* Skip Malta's board word at offset 0x10. */
    for (unsigned int i = 0; i < ARRAY_SIZE(code); i++) {
        stl_be_p(image + 0x40 + i * 4, code[i]);
    }
    for (unsigned int vec = 0x200; vec <= 0x380; vec += 0x80) {
        for (unsigned int i = 0; i < ARRAY_SIZE(fault); i++) {
            stl_be_p(image + vec + i * 4, fault[i]);
        }
    }
    fd = g_file_open_tmp("r12000a-cpu-XXXXXX", &a.image, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    close(fd);
    quoted = g_shell_quote(a.image);
    a.qts = qtest_initf("-M malta -m 64M -cpu %s -bios %s -nic none "
        "-accel tcg -S -device mips-cpu-exception-fixture,id=a-fixture %s",
        cpu, quoted, extra);
    if (run) {
        a_wait_entry(&a, 0);
    }
    return a;
}

static void a_quit(ACPU *a)
{
    qtest_quit(a->qts);
    g_unlink(a->image);
    g_free(a->image);
}

static void a_execute(ACPU *a, const uint32_t *words, size_t count)
{
    uint8_t bytes[256];
    SgiWait wait;

    g_assert_cmpuint((count + 2) * 4, <=, sizeof(bytes));
    for (unsigned int i = 0; i < count; i++) {
        stl_be_p(bytes + i * 4, words[i]);
    }
    stl_be_p(bytes + count * 4, 0x03e00008);
    stl_be_p(bytes + count * 4 + 4, 0);
    qtest_memwrite(a->qts, a->next, bytes, (count + 2) * 4);
    qtest_writel(a->qts, A_STATE + 4, 0);
    qtest_writel(a->qts, A_STATE + 0x7c, 0);
    qtest_writel(a->qts, A_STATE, 0xa0000000 | a->next);
    a->next += 0x1000;
    qtest_qmp_assert_success(a->qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "R12000A synthetic command");
    while (!qtest_readl(a->qts, A_STATE + 4)) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(a->qts, "{'execute':'stop'}");
    qtest_qmp_eventwait(a->qts, "STOP");
    g_assert_cmphex(qtest_readl(a->qts, A_STATE + 0x7c) & 0x7c, ==, 0);
}

static void a_write(ACPU *a, unsigned int reg, unsigned int sel, uint32_t value)
{
    uint32_t code[] = {
        mips_lui(R_T0, value >> 16), mips_ori(R_T0, R_T0, value),
        mips_mtc0(R_T0, reg) | sel, 0, 0, 0, 0,
    };

    a_execute(a, code, ARRAY_SIZE(code));
}

static void a_snapshot(ACPU *a, uint32_t out[7])
{
    static const uint32_t code[] = {
        0x4008c800, 0xaf480040, 0x4008c801, 0xaf480044,
        0x4008c802, 0xaf480048, 0x4008c803, 0xaf48004c,
        0x40086800, 0xaf480050, 0x40088000, 0xaf480054,
        0x40084800, 0xaf480058, 0x4028c800, 0xff480060,
    };

    a_execute(a, code, ARRAY_SIZE(code));
    for (unsigned int i = 0; i < 7; i++) {
        out[i] = qtest_readl(a->qts, A_STATE + 0x40 + i * 4);
    }
}

static uint32_t a_syndrome(ACPU *a)
{
    uint32_t out[7];

    a_snapshot(a, out);
    return out[0] >> 23;
}

static bool a_captured(QTestState *qts, const char *path)
{
    g_autoptr(QDict) value = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'syndrome-captured'}}", path);

    g_assert_null(qdict_get(value, "error"));
    return qdict_get_bool(value, "return");
}

static void a_report(QTestState *qts, const char *path, unsigned int origin,
                      unsigned int error, unsigned int cacheop,
                      unsigned int syndrome, bool valid)
{
    g_autofree char *report = g_strdup_printf("%u %u %u %u", origin, error,
                                             cacheop, syndrome);
    g_autoptr(QDict) result = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'secondary-report','value':%s}}", path, report);

    if (valid) {
        g_assert_null(qdict_get(result, "error"));
    } else {
        g_assert_nonnull(qdict_get_qdict(result, "error"));
    }
}

static void a_expect_rejected(const char *cpu, const char *reason)
{
    const char *args[] = { qtest_qemu_binary(NULL), "-M", "malta", "-cpu", cpu,
        "-display", "none", "-S", NULL };
    g_autofree char *error = NULL;
    int status;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                               NULL, &error, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    g_assert_nonnull(strstr(error, reason));
}

static void test_r12000a_variant(void)
{
    static const char *variants[] = { NULL, "r12000", "vr12000a" };
    static const char *fields[] = {
        "r12000-prid=0xe12", "r12000-fpu-id=0x4567",
        "r12000-reset-mode=0x11a8603", "r12000-scache-bytes=2097152",
        "r12000-scache-block-words=16",
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(variants); i++) {
        g_autofree char *cpu = a_cpu_options(variants[i], 3, 3);
        ACPU a = a_start(cpu, "", true);
        g_autofree char *path = cpu_qom_path(a.qts);
        g_autoptr(QDict) value = qtest_qmp(a.qts,
            "{'execute':'qom-get','arguments':{'path':%s,"
            "'property':'r12000-variant'}}", path);
        g_autoptr(QDict) changed = qtest_qmp(a.qts,
            "{'execute':'qom-set','arguments':{'path':%s,"
            "'property':'r12000-variant','value':'vr12000a'}}", path);

        g_assert_cmpstr(qdict_get_str(value, "return"), ==,
                        variants[i] ? variants[i] : "r12000");
        g_assert_nonnull(qdict_get_qdict(changed, "error"));
        a_quit(&a);
    }
    for (unsigned int missing = 0; missing < ARRAY_SIZE(fields); missing++) {
        GString *cpu = g_string_new("R12000,r12000-variant=vr12000a");

        for (unsigned int i = 0; i < ARRAY_SIZE(fields); i++) {
            if (i != missing) {
                g_string_append_printf(cpu, ",%s", fields[i]);
            }
        }
        a_expect_rejected(cpu->str, "R12000 requires");
        g_string_free(cpu, true);
    }
    a_expect_rejected("R12000,r12000-variant=400mhz", "requires r12000");
    a_expect_rejected("R10000,r12000-variant=vr12000a", "not found");
}

static void test_r12000a_clock_codes(void)
{
    for (unsigned int a = 0; a < 2; a++) {
        for (unsigned int ec = 0; ec < 16; ec++) {
            g_autofree char *cpu = a_cpu_options(a ? "vr12000a" : NULL, ec, 3);

            if (ec < 3 || ec > (a ? 12 : 11)) {
                a_expect_rejected(cpu, "reserved encoding");
            } else {
                ACPU instance = a_start(cpu, "", true);
                uint32_t config = qtest_readl(instance.qts, A_STATE + 0x20);

                g_assert_cmpuint((config >> 9) & 15, ==, ec);
                a_write(&instance, 16, 0, 0xffffffff);
                uint32_t out[7];
                a_snapshot(&instance, out);
                g_assert_cmphex(out[5] & ~7u, ==, config & ~7u);
                qtest_system_reset(instance.qts);
                a_wait_entry(&instance, 1);
                g_assert_cmphex(qtest_readl(instance.qts, A_STATE + 0x20), ==,
                                config);
                a_quit(&instance);
            }
        }
        for (unsigned int sc = 0; sc < 8; sc++) {
            g_autofree char *cpu = a_cpu_options(a ? "vr12000a" : NULL, 3, sc);

            if (sc < 2 || sc == 6) {
                a_expect_rejected(cpu, "reserved encoding");
            } else {
                ACPU instance = a_start(cpu, "", true);

                /* SC=7 remains a silicon-test encoding, not a factory fit. */
                g_assert_cmpuint((qtest_readl(instance.qts, A_STATE + 0x20)
                                  >> 19) & 7, ==, sc);
                a_quit(&instance);
            }
        }
    }
}

static void test_r12000a_capture(void)
{
    static const struct {
        unsigned int path, error, cacheop, syndrome;
    } ignore[] = {
        { 0, 0, 0, 0 }, { 1, 0, 0, 0x1ff },
        { 0, 1, 1, 0x123 }, { 1, 2, 1, 0x42 },
        { 2, 1, 0, 0x55 }, { 2, 2, 0, 0x77 },
    };
    g_autofree char *cpu = a_cpu_options("vr12000a", 3, 3);
    ACPU a = a_start(cpu, "", true);

    g_assert_cmpuint(a_syndrome(&a), ==, 0);
    g_assert_false(a_captured(a.qts, A_FIXTURE));
    a_report(a.qts, A_FIXTURE, 0, 0, 0, 0, true);
    g_assert_false(a_captured(a.qts, A_FIXTURE));
    a_report(a.qts, A_FIXTURE, 0, 1, 0, 0, true);
    g_assert_true(a_captured(a.qts, A_FIXTURE));
    g_assert_cmpuint(a_syndrome(&a), ==, 0);
    for (unsigned int bit = 0; bit < 9; bit++) {
        a_report(a.qts, A_FIXTURE, bit & 1, bit & 1 ? 1 : 2, 0,
                 1u << bit, true);
        g_assert_cmpuint(a_syndrome(&a), ==, 1u << bit);
    }
    a_report(a.qts, A_FIXTURE, 0, 1, 0, 0x155, true);
    for (unsigned int i = 0; i < ARRAY_SIZE(ignore); i++) {
        a_report(a.qts, A_FIXTURE, ignore[i].path, ignore[i].error,
                 ignore[i].cacheop, ignore[i].syndrome, true);
        g_assert_cmpuint(a_syndrome(&a), ==, 0x155);
        /* Nondestructive polling. */
        g_assert_cmpuint(a_syndrome(&a), ==, 0x155);
    }
    a_report(a.qts, A_FIXTURE, 1, 2, 0, 0x1ff, true);
    g_assert_cmpuint(a_syndrome(&a), ==, 0x1ff);
    a_report(a.qts, A_FIXTURE, 0, 1, 0, 0, true);
    g_assert_cmpuint(a_syndrome(&a), ==, 0);
    g_assert_true(a_captured(a.qts, A_FIXTURE));
    a_report(a.qts, A_FIXTURE, 1, 1, 0, 0x123, true);
    a_report(a.qts, A_FIXTURE, 0, 2, 0, 2, true);
    /* The last qualifying event wins in reported order. */
    g_assert_cmpuint(a_syndrome(&a), ==, 2);
    a_report(a.qts, A_FIXTURE, 3, 1, 0, 1, false);
    a_report(a.qts, A_FIXTURE, 0, 3, 0, 1, false);
    a_report(a.qts, A_FIXTURE, 0, 1, 2, 1, false);
    a_report(a.qts, A_FIXTURE, 0, 1, 0, 512, false);
    g_assert_cmpuint(a_syndrome(&a), ==, 2);
    g_assert_true(a_captured(a.qts, A_FIXTURE));
    a_quit(&a);
}

static void test_r12000a_control_isolation(void)
{
    static const uint32_t upper[] = { 0, 0xff800000, 0x55000000, 0x00800000 };
    static const uint32_t lower[] = { 0, 0x10, 0x3e, 0x3ff };
    g_autofree char *cpu = a_cpu_options("vr12000a", 3, 3);
    ACPU a = a_start(cpu, "", true);
    uint32_t out[7];

    a_write(&a, 26, 0, 0xffffffff); /* CP0 ECC is not a syndrome input. */
    g_assert_cmpuint(a_syndrome(&a), ==, 0);
    a_write(&a, 25, 0, 0xff800010);
    g_assert_cmpuint(a_syndrome(&a), ==, 0);
    g_assert_false(a_captured(a.qts, A_FIXTURE));
    a_write(&a, 25, 1, 0x12345678);
    /* Common unsupported event storage, without adding an event. */
    a_write(&a, 25, 2, 0x3f);
    a_write(&a, 25, 3, 0x22334455);
    a_report(a.qts, A_FIXTURE, 0, 1, 0, 0x100, true);
    for (unsigned int i = 0; i < ARRAY_SIZE(upper); i++) {
        a_write(&a, 25, 0, upper[i] | lower[i]);
        a_snapshot(&a, out);
        g_assert_cmphex(out[0], ==, 0x80000000 | lower[i]);
        /* Preserve the common four-pair model's unsupported DMFC0 response. */
        g_assert_cmphex(qtest_readq(a.qts, A_STATE + 0x60), ==, UINT64_MAX);
        g_assert_cmphex(out[1], ==, 0x12345678);
        g_assert_cmphex(out[2], ==, 0x3f);
        g_assert_cmphex(out[3], ==, 0x22334455);
        /* Syndrome bit 31 is independent of overflow IP7. */
        g_assert_cmphex(out[4] & 0x8000, ==, 0);
    }
    for (unsigned int ctr = 1; ctr < 4; ctr++) {
        uint32_t code[] = {
            mips_mfc0(R_T0, 25) | (ctr * 2), 0xaf480070,
            mips_mfc0(R_T0, 25) | (ctr * 2 + 1), 0xaf480074,
        };

        a_write(&a, 25, ctr * 2, 0xff80003f);
        a_write(&a, 25, ctr * 2 + 1, 0x12345678 + ctr);
        a_execute(&a, code, ARRAY_SIZE(code));
        g_assert_cmphex(qtest_readl(a.qts, A_STATE + 0x70), ==, 0x3f);
        g_assert_cmphex(qtest_readl(a.qts, A_STATE + 0x74), ==,
                        0x12345678 + ctr);
    }
    a_write(&a, 26, 0, 0);
    g_assert_cmpuint(a_syndrome(&a), ==, 0x100);
    a_quit(&a);
    g_clear_pointer(&cpu, g_free);
    cpu = a_cpu_options(NULL, 3, 3);
    a = a_start(cpu, "", true);
    a_report(a.qts, A_FIXTURE, 0, 1, 0, 0x1ff, false);
    a_write(&a, 25, 0, 0xff800010);
    a_snapshot(&a, out);
    g_assert_cmphex(out[0], ==, 0x10);
    a_quit(&a);
}

static void test_r12000a_resets(void)
{
    g_autofree char *cpu = a_cpu_options("vr12000a", 3, 3);
    ACPU a = a_start(cpu, "", true);
    g_autofree char *path = cpu_qom_path(a.qts);

    a_report(a.qts, A_FIXTURE, 1, 2, 0, 0x1a5, true);
    for (unsigned int kind = 0; kind < 4; kind++) {
        if (kind == 0) {
            qtest_system_reset(a.qts);
        } else {
            const char *wire = kind == 1 ? "cold-reset" :
                               kind == 2 ? "soft-reset" : "nmi";
            const char *owner = kind == 3 ? path : A_FIXTURE;

            qtest_set_irq_in(a.qts, owner, wire, 0, 1);
            qtest_set_irq_in(a.qts, owner, wire, 0, 0);
        }
        a_wait_entry(&a, kind + 1);
        g_assert_cmpuint(a_syndrome(&a), ==, 0x1a5);
        g_assert_true(a_captured(a.qts, A_FIXTURE));
    }
    a_quit(&a);
    a = a_start(cpu, "", true);
    g_assert_cmpuint(a_syndrome(&a), ==, 0);
    g_assert_false(a_captured(a.qts, A_FIXTURE));
    a_quit(&a);
}

static unsigned int a_private_syndrome(QTestState *qts, const char *path)
{
    g_autoptr(QDict) value = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'secondary-syndrome'}}", path);

    unsigned int result;

    g_assert_null(qdict_get(value, "error"));
    g_assert_cmpint(qemu_strtoui(qdict_get_str(value, "return"), NULL, 10,
                                &result), ==, 0);
    return result;
}

static void test_r12000a_instance_isolation(void)
{
    static const char second[] = "/machine/peripheral/a-second";
    g_autofree char *cpu = a_cpu_options("vr12000a", 3, 3);
    ACPU a = a_start(cpu, "-smp 2 -device mips-cpu-exception-fixture,"
                          "id=a-second,cpu-index=1", false);

    /* Two paused CPU instances in one process catch process-global state. */
    a_report(a.qts, A_FIXTURE, 0, 1, 0, 0x1aa, true);
    g_assert_cmpuint(a_private_syndrome(a.qts, A_FIXTURE), ==, 0x1aa);
    g_assert_cmpuint(a_private_syndrome(a.qts, second), ==, 0);
    g_assert_false(a_captured(a.qts, second));
    a_report(a.qts, second, 1, 2, 0, 0x55, true);
    g_assert_cmpuint(a_private_syndrome(a.qts, A_FIXTURE), ==, 0x1aa);
    g_assert_cmpuint(a_private_syndrome(a.qts, second), ==, 0x55);
    a_report(a.qts, A_FIXTURE, 0, 2, 0, 0, true);
    g_assert_cmpuint(a_private_syndrome(a.qts, A_FIXTURE), ==, 0);
    g_assert_true(a_captured(a.qts, A_FIXTURE));
    g_assert_cmpuint(a_private_syndrome(a.qts, second), ==, 0x55);
    a_quit(&a);
}

static void a_set_halted(QTestState *qts, const char *owner, bool halted)
{
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'test-halted','value':%i}}", owner, halted);
}

static char *a_translation_key(QTestState *qts, const char *owner)
{
    g_autoptr(QDict) result = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'translation-key'}}", owner);

    g_assert_null(qdict_get(result, "error"));
    return g_strdup(qdict_get_str(result, "return"));
}

static void test_r12000a_mixed_execution(gconstpointer opaque)
{
    static const char second[] = "/machine/peripheral/a-mixed";
    static const uint32_t code[] = {
        0x4008c800, 0xaf480000, 0x24080001, 0xaf480004, 0x1000ffff, 0,
    };
    bool a_first = GPOINTER_TO_INT(opaque);
    g_autofree char *cpu = a_cpu_options(NULL, 3, 3);
    ACPU a = a_start(cpu, "-smp 1,maxcpus=2 "
        "-device mips-cpu-exception-fixture,id=a-mixed,"
        "create-r12000-variant=vr12000a", false);
    uint8_t bytes[sizeof(code)];
    g_autoptr(QDict) variant = qtest_qmp(a.qts,
        "{'execute':'qom-get','arguments':"
        "{'path':'/machine/peripheral/a-mixed/variant-cpu',"
        "'property':'r12000-variant'}}");
    char *keys[2];

    g_assert_cmpstr(qdict_get_str(variant, "return"), ==, "vr12000a");
    for (unsigned int i = 0; i < ARRAY_SIZE(code); i++) {
        stl_be_p(bytes + i * 4, code[i]);
    }
    qtest_memwrite(a.qts, 0x200000, bytes, sizeof(bytes));
    a_report(a.qts, second, 0, 1, 0, 0x155, true);
    for (unsigned int i = 0; i < 2; i++) {
        const char *owner = i ? second : A_FIXTURE;
        g_autofree char *context = g_strdup_printf(
            "0xffffffffa0200000 0x%" PRIx64,
            UINT64_C(0xffffffffa0100000) + i * 0x10000);

        a_set_halted(a.qts, owner, true);
        qtest_qmp_assert_success(a.qts,
            "{'execute':'qom-set','arguments':{'path':%s,"
            "'property':'ram-context','value':%s}}", owner, context);
        keys[i] = a_translation_key(a.qts, owner);
    }
    /* Same PC, architectural flags, cluster and TCG flags, in one process. */
    g_assert_cmpstr(keys[0], ==, keys[1]);
    g_free(keys[0]);
    g_free(keys[1]);
    for (unsigned int turn = 0; turn < 2; turn++) {
        bool selected_a = turn == 0 ? a_first : !a_first;
        const char *owner = selected_a ? second : A_FIXTURE;
        uint32_t mailbox = A_STATE + (selected_a ? 0x10000 : 0);
        uint32_t expected = selected_a ? 0xaa800000 : 0;
        SgiWait wait;

        qtest_writel(a.qts, mailbox, 0xdeadbeef);
        qtest_writel(a.qts, mailbox + 4, 0);
        a_set_halted(a.qts, owner, false);
        qtest_qmp_assert_success(a.qts, "{'execute':'cont'}");
        sgi_wait_start(&wait, "mixed-variant shared-PC control read");
        while (!qtest_readl(a.qts, mailbox + 4)) {
            sgi_wait_pause(&wait, 1000);
        }
        qtest_qmp_assert_success(a.qts, "{'execute':'stop'}");
        qtest_qmp_eventwait(a.qts, "STOP");
        a_set_halted(a.qts, owner, true);
        g_assert_cmphex(qtest_readl(a.qts, mailbox), ==, expected);
    }
    a_quit(&a);
}

static uint64_t a_clock_period(QTestState *qts, const char *path)
{
    g_autoptr(QDict) value = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'qtest-clock-period'}}", path);

    g_assert_null(qdict_get(value, "error"));
    return qdict_get_int(value, "return");
}

static void test_r12000a_count_clock(void)
{
    uint64_t baseline = 0;

    for (unsigned int i = 0; i < 3; i++) {
        g_autofree char *cpu = a_cpu_options(i ? "vr12000a" : NULL,
                                            i == 2 ? 12 : 3, 3);
        QTestState *qts = qtest_initf("-M malta -m 64M -cpu %s -S", cpu);
        g_autofree char *path = cpu_qom_path(qts);
        g_autofree char *input = g_strdup_printf("%s/clk-in", path);
        g_autofree char *count = g_strdup_printf("%s/clk-count", path);
        uint64_t period = a_clock_period(qts, input);

        g_assert_cmpuint(period, >, 0);
        g_assert_cmpuint(a_clock_period(qts, count), ==, period * 2);
        if (i) {
            g_assert_cmpuint(period, ==, baseline);
        }
        baseline = period;
        qtest_quit(qts);
    }
}

static void a_wait_rejected(ACPU *a)
{
    SgiWait wait;

    sgi_wait_start(&wait, "rejected R12000 variant migration");
    while (qtest_probe_child(a->qts)) {
        sgi_wait_pause(&wait, 1000);
    }
}

static void test_r12000a_migration(void)
{
    for (unsigned int state = 0; state < 4; state++) {
        g_autofree char *cpu = a_cpu_options(state ? "vr12000a" : NULL, 3, 3);
        g_autofree char *dir = g_dir_make_tmp("r12000a-migrate-XXXXXX", NULL);
        g_autofree char *socket = g_build_filename(dir, "state.sock", NULL);
        g_autofree char *uri = g_strdup_printf("unix:%s", socket);
        g_autofree char *incoming = g_strdup_printf("-incoming %s", uri);
        ACPU from = a_start(cpu, "", true);
        ACPU to = a_start(cpu, incoming, false);
        uint32_t syndrome = state == 3 ? 0x155 : 0;
        uint32_t out[7];

        if (state >= 2) {
            a_report(from.qts, A_FIXTURE, 1, 1, 0, syndrome, true);
        }
        a_write(&from, 16, 0, 7);
        a_write(&from, 25, 0, 0x10);
        a_write(&from, 25, 1, 0x12345678);
        sgi_migrate_stopped(from.qts, uri);
        sgi_wait_migration(to.qts, "completed");
        a_snapshot(&to, out);
        g_assert_cmphex(out[0], ==, (syndrome << 23) | 0x10);
        g_assert_cmphex(out[1], ==, 0x12345678);
        g_assert_cmphex(out[4] & 0x8000, ==, 0);
        g_assert_cmpuint(out[5] & 7, ==, 7);
        g_assert_cmpint(a_captured(to.qts, A_FIXTURE), ==, state >= 2);
        qtest_system_reset(to.qts);
        a_wait_entry(&to, 1);
        a_snapshot(&to, out);
        g_assert_cmphex(out[0], ==, syndrome << 23);
        g_assert_cmphex(out[1], ==, 0);
        g_assert_cmpint(a_captured(to.qts, A_FIXTURE), ==, state >= 2);
        a_quit(&from);
        a_quit(&to);
        g_unlink(socket);
        g_assert_cmpint(g_rmdir(dir), ==, 0);
    }
}

static void test_r12000a_migration_mismatch(void)
{
    for (unsigned int kind = 0; kind < 5; kind++) {
        g_autofree char *from_cpu = kind >= 3 ? g_strdup("R10000") :
            a_cpu_options(kind == 1 ? NULL : "vr12000a", 3, 3);
        g_autofree char *to_cpu = a_cpu_options(
            kind == 0 || kind == 4 ? NULL : "vr12000a", kind == 2 ? 4 : 3, 3);
        g_autofree char *dir = g_dir_make_tmp("r12000a-refuse-XXXXXX", NULL);
        g_autofree char *socket = g_build_filename(dir, "state.sock", NULL);
        g_autofree char *uri = g_strdup_printf("unix:%s", socket);
        g_autofree char *incoming = g_strdup_printf("-incoming %s", uri);
        ACPU from = a_start(from_cpu, "", false);
        ACPU to = a_start(to_cpu, incoming, false);

        qtest_set_expected_status(to.qts, 1);
        qtest_qmp_assert_success(from.qts,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        a_wait_rejected(&to);
        a_quit(&from);
        a_quit(&to);
        g_unlink(socket);
        g_assert_cmpint(g_rmdir(dir), ==, 0);
    }
}

/* Locate the exact same-build subsection, avoiding its JSON description. */
static size_t a_profile_offset(const uint8_t *data, size_t len, size_t *header)
{
    static const char name[] = "cpu/r12000-profile";
    size_t found = 0;
    unsigned int hits = 0;

    for (size_t i = 2; i + sizeof(name) - 1 + 4 + 24 <= len; i++) {
        if (data[i - 2] == 5 && data[i - 1] == sizeof(name) - 1 &&
            !memcmp(data + i, name, sizeof(name) - 1) &&
            ldl_be_p(data + i + sizeof(name) - 1) == 1) {
            found = i;
            hits++;
        }
    }
    g_assert_cmpuint(hits, ==, 1);
    *header = found - 2;
    return found + sizeof(name) - 1 + 4;
}

static void test_r12000a_migration_malformed(void)
{
    g_autofree char *cpu = a_cpu_options("vr12000a", 3, 3);
    g_autofree char *dir = g_dir_make_tmp("r12000a-stream-XXXXXX", NULL);
    g_autofree char *stream = g_build_filename(dir, "baseline", NULL);
    g_autofree char *quoted = g_shell_quote(stream);
    g_autofree char *uri = g_strdup_printf("exec:cat > %s", quoted);
    g_autofree char *contents = NULL;
    ACPU from = a_start(cpu, "", true);
    gsize length;
    size_t body, header;

    a_report(from.qts, A_FIXTURE, 0, 2, 0, 0x155, true);
    sgi_migrate_stopped(from.qts, uri);
    a_quit(&from);
    g_assert_true(g_file_get_contents(stream, &contents, &length, NULL));
    body = a_profile_offset((uint8_t *)contents, length, &header);
    g_assert_cmphex(ldl_be_p(contents + body + 8), ==, 0x011a8603);
    g_assert_cmpuint((uint8_t)contents[body + 20], ==, 1);
    g_assert_cmphex(lduw_be_p(contents + body + 21), ==, 0x155);
    g_assert_cmpuint((uint8_t)contents[body + 23], ==, 1);
    for (unsigned int kind = 0; kind < 6; kind++) {
        g_autofree char *file = g_strdup_printf("%s/input-%u", dir, kind);
        g_autofree char *copy = g_memdup2(contents, length);
        g_autofree char *input = g_shell_quote(file);
        g_autofree char *read_uri = g_strdup_printf("exec:cat %s", input);
        ACPU to;
        size_t size = length;

        switch (kind) {
        case 1:
            copy[body + 20] = 2; /* Unknown variant. */
            break;
        case 2:
            stw_be_p(copy + body + 21, 0x200);
            break;
        case 3:
            copy[body + 23] = 2; /* Not a boolean byte. */
            break;
        case 4:
            copy[body + 23] = 0; /* Initial state cannot be nonzero. */
            break;
        case 5:
            memmove(copy + header, copy + body + 24, length - body - 24);
            size -= body + 24 - header;
            break;
        default:
            break;
        }
        g_assert_true(g_file_set_contents(file, copy, size, NULL));
        to = a_start(cpu, "-incoming defer", false);
        if (kind) {
            qtest_set_expected_status(to.qts, 1);
        }
        if (kind) {
            qtest_qmp_send(to.qts,
                "{'execute':'migrate-incoming',"
                "'arguments':{'uri':%s}}", read_uri);
            a_wait_rejected(&to);
        } else {
            qtest_qmp_assert_success(to.qts,
                "{'execute':'migrate-incoming',"
                "'arguments':{'uri':%s}}", read_uri);
            sgi_wait_migration(to.qts, "completed");
            g_assert_cmpuint(a_syndrome(&to), ==, 0x155);
            g_assert_true(a_captured(to.qts, A_FIXTURE));
        }
        a_quit(&to);
        g_unlink(file);
    }
    g_unlink(stream);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/* Strong same-build byte signatures locate registers, not mirrored metadata. */
static void a_register_offsets(const uint8_t *data, size_t length,
                               uint32_t config, size_t *prid, size_t *fcr0)
{
    unsigned int hits = 0;

    for (size_t i = 0; i + 28 <= length; i++) {
        if (ldl_be_p(data + i) == 0xe12 &&
            ldq_be_p(data + i + 4) == UINT64_C(0xffffffff80000000) &&
            ldq_be_p(data + i + 12) == 0 &&
            ldl_be_p(data + i + 20) == config) {
            *prid = i;
            hits++;
        }
    }
    g_assert_cmpuint(hits, ==, 1);
    hits = 0;
    for (size_t i = 512; i + 8 < *prid; i++) {
        static const uint8_t zero_fprs[512];

        if (ldl_be_p(data + i) == 0x4567 && ldl_be_p(data + i + 4) == 0 &&
            !memcmp(data + i - 512, zero_fprs, sizeof(zero_fprs))) {
            *fcr0 = i;
            hits++;
        }
    }
    g_assert_cmpuint(hits, ==, 1);
}

static char *a_incoming_terminal(QTestState *qts)
{
    SgiWait wait;

    sgi_wait_start(&wait, "incoming register-image validation");
    for (;;) {
        g_autoptr(QDict) reply = qtest_qmp(qts, "{'execute':'query-migrate'}");
        QDict *result = qdict_get_qdict(reply, "return");
        const char *status = qdict_get_str(result, "status");

        if (!strcmp(status, "completed") || !strcmp(status, "failed")) {
            return g_strdup(status);
        }
        sgi_wait_pause(&wait, 1000);
    }
}

static void test_r12000a_migration_register_image(void)
{
    g_autofree char *cpu = a_cpu_options("vr12000a", 3, 3);
    g_autofree char *dir = g_dir_make_tmp("r12000a-registers-XXXXXX", NULL);
    g_autofree char *stream = g_build_filename(dir, "baseline", NULL);
    g_autofree char *quoted = g_shell_quote(stream);
    g_autofree char *uri = g_strdup_printf("exec:cat > %s", quoted);
    g_autofree char *contents = NULL;
    ACPU from = a_start(cpu, "", true);
    uint32_t out[7];
    gsize length;
    size_t prid = 0, fcr0 = 0, body, header;

    a_report(from.qts, A_FIXTURE, 0, 2, 0, 0x155, true);
    a_write(&from, 16, 0, 7);
    a_write(&from, 25, 0, 0x3ff);
    a_snapshot(&from, out);
    sgi_migrate_stopped(from.qts, uri);
    a_quit(&from);
    g_assert_true(g_file_get_contents(stream, &contents, &length, NULL));
    a_register_offsets((uint8_t *)contents, length, out[5], &prid, &fcr0);
    body = a_profile_offset((uint8_t *)contents, length, &header);
    /* Baseline, PRId, FCR0, then each read-only Config position 3..31. */
    for (unsigned int kind = 0; kind < 32; kind++) {
        g_autofree char *file = g_strdup_printf("%s/input-%u", dir, kind);
        g_autofree char *copy = g_memdup2(contents, length);
        g_autofree char *input = g_shell_quote(file);
        g_autofree char *read_uri = g_strdup_printf("exec:cat %s", input);
        g_autofree char *status = NULL;
        bool rejected;
        ACPU to;

        if (kind == 1) {
            stl_be_p(copy + prid, 0xe13);
        } else if (kind == 2) {
            stl_be_p(copy + fcr0, 0x4566);
        } else if (kind >= 3) {
            stl_be_p(copy + prid + 20, out[5] ^ (1u << kind));
        }
        g_assert_cmpmem(copy + body, 24, contents + body, 24);
        g_assert_true(g_file_set_contents(file, copy, length, NULL));
        to = a_start(cpu, "-incoming defer", false);
        a_report(to.qts, A_FIXTURE, 1, 1, 0, 0x1aa, true);
        qtest_qmp_assert_success(to.qts,
            "{'execute':'migrate-incoming','arguments':"
            "{'uri':%s,'exit-on-error':false}}", read_uri);
        status = a_incoming_terminal(to.qts);
        rejected = !strcmp(status, "failed");
        if (rejected) {
            /* Rejection must not publish the valid mirrored capture state. */
            g_assert_cmpuint(a_private_syndrome(to.qts, A_FIXTURE), ==, 0x1aa);
            g_assert_true(a_captured(to.qts, A_FIXTURE));
        } else {
            g_assert_cmpuint(a_private_syndrome(to.qts, A_FIXTURE), ==, 0x155);
        }
        a_quit(&to);
        g_unlink(file);
        g_test_message("register-image case %u: %s", kind, status);
        g_assert_cmpint(rejected, ==, kind != 0);
    }
    g_unlink(stream);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/r12000a/variant", test_r12000a_variant);
    qtest_add_func("/r12000a/clock-codes", test_r12000a_clock_codes);
    qtest_add_func("/r12000a/capture-interface", test_r12000a_capture);
    qtest_add_func("/r12000a/control-isolation",
                   test_r12000a_control_isolation);
    qtest_add_func("/r12000a/resets", test_r12000a_resets);
    qtest_add_func("/r12000a/instance-isolation",
                   test_r12000a_instance_isolation);
    qtest_add_func("/r12000a/count-clock", test_r12000a_count_clock);
    qtest_add_data_func("/r12000a/mixed-common-first", GINT_TO_POINTER(0),
                        test_r12000a_mixed_execution);
    qtest_add_data_func("/r12000a/mixed-a-first", GINT_TO_POINTER(1),
                        test_r12000a_mixed_execution);
    qtest_add_func("/r12000a/migration-register-image",
                   test_r12000a_migration_register_image);
    qtest_add_func("/r12000a/migration", test_r12000a_migration);
    qtest_add_func("/r12000a/migration-mismatch",
                   test_r12000a_migration_mismatch);
    qtest_add_func("/r12000a/migration-malformed",
                   test_r12000a_migration_malformed);

    qtest_add_func("/sn0/nmi/entry-and-retrigger",
                   test_nmi_entry_and_retrigger);
    qtest_add_func("/sn0/nmi/reset-rearms", test_nmi_reset_rearms);
    qtest_add_func("/r16000/nmi/entry-and-retrigger",
                   test_r16000_nmi_entry_and_retrigger);
    qtest_add_func("/r16000/nmi/cold-reset-control",
                   test_r16000_nmi_reset_rearms);
    qtest_add_func("/r16000/cache-error/entry-and-nested-suppression",
                   test_r16000_cache_error_entry_and_nested_suppression);
    qtest_add_func("/sn0/nmi/latch-migration", test_nmi_latch_migration);
    qtest_add_func("/status-re/r10000-logs-unimp", test_r10000_logs_unimp);
    for (unsigned int i = 0; i < ARRAY_SIZE(cpu_cases); i++) {
        g_test_add_data_func(cpu_cases[i].path, &cpu_cases[i], test_cpu);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(r16000_llsc_cases); i++) {
        qtest_add_data_func(r16000_llsc_cases[i].path,
                             &r16000_llsc_cases[i],
                             test_r16000_llsc_sc_in_exception);
    }
    qtest_add_func("/sn0/tcg/indirect-and-unmapped-chaining",
                   test_indirect_and_unmapped_chaining);
    qtest_add_func("/sn0/tcg/unmapped-cpu-relative-destination",
                   test_unmapped_cpu_relative_destination);
    qtest_add_func("/sn0/tcg/prefetch-hints",
                   test_r10k_prefetch_hints);
    qtest_add_func("/sn0/tcg/tlb-interleaved",
                   test_r10k_tlb_interleaved);
    qtest_add_func("/sn0/tcg/tlb-replaced-half",
                   test_r10k_tlb_replaced_half);
    for (unsigned int i = 0; i < ARRAY_SIZE(asid_cases); i++) {
        qtest_add_data_func(asid_cases[i].path, &asid_cases[i],
                            test_r10k_tlb_asid);
    }
    qtest_add_func("/sn0/tcg/count-chaining",
                   test_r10k_count_chaining);
    qtest_add_func("/sn0/tcg/count-interrupt-delivery",
                   test_r10k_count_interrupt_delivery);
    qtest_add_func("/sn0/tcg/fpu-fast-path",
                   test_r10k_fpu_fast_path);
    qtest_add_func("/r10k/scache/reset-mode-capacity",
                   test_r10k_scache_reset_mode_capacity);
    qtest_add_data_func("/r10k/soft-reset/retains-config-watch-state",
                        "R10000,reset-mode=0x3a005",
                        test_r10k_soft_reset);
    qtest_add_data_func("/r16000/soft-reset/retains-config-watch-state",
                        R16000_CPU, test_r10k_soft_reset);
    for (unsigned int i = 0; i < ARRAY_SIZE(scache_line_cases); i++) {
        g_test_add_data_func(scache_line_cases[i].path,
                             &scache_line_cases[i],
                             test_r10k_scache_line_size);
    }

    return g_test_run();
}
