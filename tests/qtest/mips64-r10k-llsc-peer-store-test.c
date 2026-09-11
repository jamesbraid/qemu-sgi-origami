/*
 * R10000 peer-CPU store load-linked tests.
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
 * This diagnostic covers system-mode translated stores executed by another
 * R10000 CPU.  User-mode execution, DMA, host writes, and stores from other
 * CPU models are not observed by the target-specific implementation.
 * The R4000 and R14000 cases preserve QEMU's existing same-block behavior;
 * they are regression controls, not claims about those processors.
 *
 * Run two CPUs on Malta so no SGI machine device participates.
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
#define DATA_HIGH_PHYS      UINT64_C(0x80100000)
#define DATA_KSEG0_HI       0x8010
#define DATA_KSEG1_HI       0xa010
#define XKPHYS_CACHED_HI    0xa800
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
#define NO_LINK_STORE_COUNT 128

#define OPC_SPECIAL         (0x00u << 26)
#define OPC_BEQ             (0x04u << 26)
#define OPC_ADDIU           (0x09u << 26)
#define OPC_ORI             (0x0du << 26)
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
#define R_T3                11
#define R_T4                12

typedef struct GuestImage {
    uint8_t bytes[IMAGE_SIZE];
    unsigned int pos;
} GuestImage;

typedef enum PeerStoreCase {
    PEER_STORE_OUTSIDE_BLOCK,
    PEER_STORE_SAME_BLOCK,
    PEER_STORE_ALIAS_SAME_BLOCK,
    PEER_STORE_GPA_ALIAS_SAME_BLOCK,
    PEER_STORE_ABA,
    PEER_STORE_NO_LINK_BURST,
} PeerStoreCase;

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

static void emit_ori(GuestImage *img, int rt, int rs, uint16_t imm)
{
    emit_i(img, OPC_ORI, rs, rt, imm);
}

static void emit_dsll(GuestImage *img, int rd, int rt, int sa)
{
    emit(img, OPC_SPECIAL | (rt << 16) | (rd << 11) | (sa << 6) | 0x38);
}

static void emit_dsll32(GuestImage *img, int rd, int rt)
{
    emit(img, OPC_SPECIAL | (rt << 16) | (rd << 11) | 0x3c);
}

static void emit_or(GuestImage *img, int rd, int rs, int rt)
{
    emit(img, OPC_SPECIAL | (rs << 21) | (rt << 16) | (rd << 11) | 0x25);
}

static void emit_mfc0_status(GuestImage *img, int rt)
{
    emit(img, 0x40006000 | (rt << 16));
}

static void emit_mtc0_status(GuestImage *img, int rt)
{
    emit(img, 0x40806000 | (rt << 16));
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

static void build_peer(GuestImage *img, PeerStoreCase store_case,
                       unsigned int *peer_done)
{
    unsigned int wait;
    unsigned int i;

    img->pos = PEER_OFF;
    emit_lui(img, R_T0, DATA_KSEG0_HI);
    wait = img->pos;
    emit_ld(img, R_T1, R_T0, COMMAND_OFF);
    emit_beq(img, R_T1, R_ZERO, wait);
    emit_nop(img);

    if (store_case == PEER_STORE_NO_LINK_BURST) {
        emit_addiu(img, R_T2, R_ZERO, PEER_VALUE);
        for (i = 0; i < NO_LINK_STORE_COUNT; i++) {
            emit_sd(img, R_T2, R_T0, SAME_BLOCK_OFF);
        }
    } else if (store_case == PEER_STORE_SAME_BLOCK ||
               store_case == PEER_STORE_ALIAS_SAME_BLOCK ||
               store_case == PEER_STORE_GPA_ALIAS_SAME_BLOCK) {
        emit_addiu(img, R_T2, R_ZERO, PEER_VALUE);
        if (store_case == PEER_STORE_ALIAS_SAME_BLOCK) {
            emit_lui(img, R_T3, DATA_KSEG1_HI);
            emit_sd(img, R_T2, R_T3, SAME_BLOCK_OFF);
        } else if (store_case == PEER_STORE_GPA_ALIAS_SAME_BLOCK) {
            /* Malta maps the same RAM at GPA 0 and GPA 0x80000000. */
            emit_mfc0_status(img, R_T3);
            emit_ori(img, R_T3, R_T3, 0x80); /* Status.KX */
            emit_mtc0_status(img, R_T3);
            emit_nop(img);
            emit_nop(img);
            emit_lui(img, R_T3, XKPHYS_CACHED_HI);
            emit_dsll32(img, R_T3, R_T3);
            emit_ori(img, R_T4, R_ZERO, DATA_HIGH_PHYS >> 16);
            emit_dsll(img, R_T4, R_T4, 16);
            emit_or(img, R_T3, R_T3, R_T4);
            emit_sd(img, R_T2, R_T3, SAME_BLOCK_OFF);
        } else {
            emit_sd(img, R_T2, R_T0, SAME_BLOCK_OFF);
        }
    } else if (store_case == PEER_STORE_ABA) {
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

static char *write_image(PeerStoreCase store_case, unsigned int *after_lld,
                         unsigned int *peer_done)
{
    g_autofree GuestImage *img = g_new0(GuestImage, 1);
    char *path = NULL;
    int fd;

    build_victim(img, after_lld);
    build_peer(img, store_case, peer_done);
    fd = g_file_open_tmp("mips64-r10k-peer-store-XXXXXX", &path, NULL);
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

static void wait_for_migration(QTestState *qts)
{
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(
            qts, "{'execute':'query-migrate'}");
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

static void run_peer_store(const char *cpu, PeerStoreCase store_case,
                           bool expect_success)
{
    g_autofree char *image = NULL;
    unsigned int after_lld, peer_done;
    uint64_t result, target;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    image = write_image(store_case, &after_lld, &peer_done);
    qts = qtest_initf(
        "-M malta -m 64M -smp 2 -cpu %s "
        "-device loader,file=%s,addr=0x%" PRIx64 ",force-raw=on "
        "-device loader,addr=0x%" PRIx64 ",cpu-num=0 "
        "-device loader,addr=0x%" PRIx64 ",cpu-num=1 "
        "-accel tcg,thread=multi -no-reboot -no-shutdown -S",
        cpu, image, CODE_PHYS, CODE_KSEG0 + VICTIM_OFF,
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
    if (store_case == PEER_STORE_SAME_BLOCK ||
        store_case == PEER_STORE_ALIAS_SAME_BLOCK ||
        store_case == PEER_STORE_GPA_ALIAS_SAME_BLOCK) {
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

    if (expect_success) {
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

static void test_r10k_outside_block_peer_store(void)
{
    run_peer_store("R10000", PEER_STORE_OUTSIDE_BLOCK, true);
}

static void test_r10k_same_block_peer_store(void)
{
    run_peer_store("R10000", PEER_STORE_SAME_BLOCK, false);
}

static void test_r10k_alias_same_block_peer_store(void)
{
    run_peer_store("R10000", PEER_STORE_ALIAS_SAME_BLOCK, false);
}

static void test_r10k_gpa_alias_same_block_peer_store(void)
{
    run_peer_store("R10000", PEER_STORE_GPA_ALIAS_SAME_BLOCK, false);
}

static void test_r10k_target_aba_peer_store(void)
{
    run_peer_store("R10000", PEER_STORE_ABA, false);
}

static void test_r4k_normal_llsc(void)
{
    run_peer_store("R4000", PEER_STORE_SAME_BLOCK, true);
}

static void test_r14k_normal_llsc(void)
{
    run_peer_store("R14000", PEER_STORE_SAME_BLOCK, true);
}

static void test_r10k_migration_drops_link(void)
{
    g_autofree char *directory = NULL;
    g_autofree char *image = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    unsigned int after_lld, peer_done;
    GError *error = NULL;
    QTestState *from, *to;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    image = write_image(PEER_STORE_OUTSIDE_BLOCK, &after_lld, &peer_done);
    directory = g_dir_make_tmp("mips64-r10k-llsc-migration-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);

    from = qtest_initf(
        "-M malta -m 64M -cpu R10000 "
        "-device loader,file=%s,addr=0x%" PRIx64 ",force-raw=on "
        "-device loader,addr=0x%" PRIx64 ",cpu-num=0 "
        "-accel tcg,thread=multi -no-reboot -no-shutdown -S",
        image, CODE_PHYS, CODE_KSEG0 + VICTIM_OFF);
    to = qtest_initf(
        "-M malta -m 64M -cpu R10000 "
        "-device loader,file=%s,addr=0x%" PRIx64 ",force-raw=on "
        "-device loader,addr=0x%" PRIx64 ",cpu-num=0 "
        "-accel tcg,thread=multi -no-reboot -no-shutdown -S "
        "-incoming defer",
        image, CODE_PHYS, CODE_KSEG0 + VICTIM_OFF);
    unlink(image);

    /* llval is not migrated, so zero keeps the value comparison eligible. */
    qtest_writeq(from, DATA_PHYS + TARGET_OFF, 0);
    add_breakpoint(from, 0, CODE_KSEG0 + after_lld);
    qtest_qmp_assert_success(from, "{'execute':'cont'}");
    qtest_qmp_eventwait(from, "STOP");
    g_assert_cmphex(qtest_readq(from, DATA_PHYS + VICTIM_DONE_OFF), ==, 0);
    remove_breakpoint(from, 0, CODE_KSEG0 + after_lld);
    qtest_qmp_assert_success(from, "{'execute':'cont'}");
    qtest_qmp_assert_success(from, "{'execute':'stop'}");

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_for_migration(from);

    qtest_writel(to, CODE_PHYS + after_lld, 0);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    wait_for_word(to, DATA_PHYS + VICTIM_DONE_OFF, VICTIM_DONE_MAGIC);
    g_assert_cmphex(qtest_readq(to, DATA_PHYS + SC_RESULT_OFF), ==, 0);
    g_assert_cmphex(qtest_readq(to, DATA_PHYS + TARGET_OFF), ==, 0);

    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_r10k_no_link_skips_slow_path(void)
{
    g_autofree char *image = NULL;
    g_autofree char *trace = NULL;
    g_autofree char *trace_path = NULL;
    unsigned int after_lld, peer_done;
    QTestState *qts;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    image = write_image(PEER_STORE_NO_LINK_BURST, &after_lld, &peer_done);
    fd = g_file_open_tmp("mips64-r10k-llsc-slow-path-XXXXXX", &trace_path,
                         NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    qts = qtest_initf(
        "-M malta -m 64M -cpu R10000 "
        "-device loader,file=%s,addr=0x%" PRIx64 ",force-raw=on "
        "-device loader,addr=0x%" PRIx64 ",cpu-num=0 "
        "-trace mips_r10k_llsc_peer_store_slow_path,file=%s "
        "-accel tcg -no-reboot -no-shutdown -S",
        image, CODE_PHYS, CODE_KSEG0 + PEER_OFF, trace_path);
    unlink(image);

    qtest_writeq(qts, DATA_PHYS + COMMAND_OFF, COMMAND_MAGIC);
    add_breakpoint(qts, 0, CODE_KSEG0 + peer_done);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    qtest_qmp_eventwait(qts, "STOP");
    g_assert_cmphex(qtest_readq(qts, DATA_PHYS + PEER_DONE_OFF), ==,
                    PEER_DONE_MAGIC);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, NULL));
    g_assert_null(strstr(trace, "mips_r10k_llsc_peer_store_slow_path"));
    g_assert_cmpint(g_unlink(trace_path), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/r10k-llsc-peer-store/outside-block",
                   test_r10k_outside_block_peer_store);
    qtest_add_func("/r10k-llsc-peer-store/same-block",
                   test_r10k_same_block_peer_store);
    qtest_add_func("/r10k-llsc-peer-store/alias-same-block",
                   test_r10k_alias_same_block_peer_store);
    qtest_add_func("/r10k-llsc-peer-store/gpa-alias-same-block",
                   test_r10k_gpa_alias_same_block_peer_store);
    qtest_add_func("/r10k-llsc-peer-store/target-aba",
                   test_r10k_target_aba_peer_store);
    qtest_add_func("/r10k-llsc-peer-store/r4000-normal",
                   test_r4k_normal_llsc);
    qtest_add_func("/r10k-llsc-peer-store/r14000-normal",
                   test_r14k_normal_llsc);
    qtest_add_func("/r10k-llsc-peer-store/migration-drops-link",
                   test_r10k_migration_drops_link);
    qtest_add_func("/r10k-llsc-peer-store/no-link-skips-slow-path",
                   test_r10k_no_link_skips_slow_path);
    return g_test_run();
}
