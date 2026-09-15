/*
 * R10000 performance-counter and secondary-cache tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The R10000 is the only CPU this fork builds a machine for, so every case
 * runs on the Origin 200 or Origin 2000 with the synthesized firmware off,
 * leaving the guest fragment in sole possession of the reset vector and low
 * memory.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define IMAGE_SIZE          0x1000
#define CODE_START          0x400
#define MAIN_LOOP           0x480
#define CACHE_START         0x900
#define CACHE_LOOP          0xa00
#define CACHE_READ          0xa40
#define CACHE_DONE          0xb00

#define STATE_PHYS          0x00100000
#define STATE_KSEG1_HI      0xa010
#define READY_OFF           0x00
#define COUNT_OFF           0x08
#define CACHE_READY_OFF     0x120
#define CACHE_COMMAND_OFF   0x128
#define CACHE_DONE_OFF      0x130
#define CACHE_TAGLO_OFF     0x138
#define CACHE_TAGHI_OFF     0x140
#define CACHE_TAG_STRIDE    0x10
#define CACHE_DATA_LO_OFF   0x168
#define CACHE_DATA_HI_OFF   0x170
#define CACHE_ECC_OFF       0x178
#define CACHE_OTHER_LO_OFF  0x180
#define CACHE_OTHER_HI_OFF  0x188
#define CACHE_OTHER_ECC_OFF 0x190
#define READY_MAGIC         0x1400
#define CACHE_READY_MAGIC   0x14ca
#define CACHE_DONE_MAGIC    0x14cb
#define INITIAL_COUNT       0x12340000u
#define CACHE_TAGLO         0x1234c5aau
#define CACHE_TAGHI         0x0000000du
#define CACHE_DATA_LO       0x2468ace0u
#define CACHE_DATA_HI       0x13579bdfu
#define CACHE_ECC           0x2aau
#define CACHE_OTHER_LO      0x10293847u
#define CACHE_OTHER_HI      0x5a6b7c8du
#define CACHE_OTHER_ECC     0x155u
#define CACHE_SENTINEL      0xffffffffu
#define CACHE_ECC_SENTINEL  0x3ffu
#define POLL_TRIES          20000
#define POLL_INTERVAL_US    1000
#define ANALYZE_SCRIPT      "scripts/analyze-migration.py"
#define STREAM_ADVANCE      (1u << 26)

#define OPC_SPECIAL         (0x00u << 26)
#define OPC_BEQ             (0x04u << 26)
#define OPC_BNE             (0x05u << 26)
#define OPC_ADDIU           (0x09u << 26)
#define OPC_ORI             (0x0du << 26)
#define OPC_CACHE           (0x2fu << 26)
#define OPC_AND             (0x24u | OPC_SPECIAL)
#define OPC_LUI             (0x0fu << 26)
#define OPC_CP0             (0x10u << 26)
#define OPC_LD              (0x37u << 26)
#define OPC_SD              (0x3fu << 26)
#define OPC_MFC0            (OPC_CP0 | (0x00u << 21))
#define OPC_MTC0            (OPC_CP0 | (0x04u << 21))
#define OPC_SYNC            (0x0fu | OPC_SPECIAL)

#define R_ZERO              0
#define R_T0                8
#define R_T1                9
#define R_T2                10
#define R_T3                11

#define CP0_STATUS          12
#define CP0_PERFORMANCE     25
#define CP0_ERRCTL          26
#define CP0_TAGLO           28
#define CP0_TAGHI           29
#define PERF_KERNEL         (1u << 1)

typedef struct {
    const char *cpu;
    unsigned int control_sel;
    unsigned int counter_sel;
} TestConfig;

typedef struct {
    uint8_t *code;
    unsigned int pos;
} GuestImage;

typedef struct {
    uint16_t address_hi;
    uint32_t taglo;
    uint32_t taghi;
} CacheTagProbe;

static const TestConfig configurations[] = {
    { "R10000", 0, 1 },
};

static const CacheTagProbe r10000_cache_probe = {
    .address_hi = 0xa000,
    .taglo = CACHE_TAGLO,
    .taghi = CACHE_TAGHI,
};

static void emit(GuestImage *img, uint32_t insn)
{
    g_assert_cmpuint(img->pos + 4, <=, IMAGE_SIZE);
    stl_be_p(img->code + img->pos, insn);
    img->pos += 4;
}

static void emit_branch(GuestImage *img, unsigned int target)
{
    int32_t off = ((int32_t)target - (int32_t)(img->pos + 4)) / 4;

    emit(img, OPC_BEQ | (off & 0xffff));
}

static void emit_bne(GuestImage *img, int rs, int rt, unsigned int target)
{
    int32_t off = ((int32_t)target - (int32_t)(img->pos + 4)) / 4;

    emit(img, OPC_BNE | (rs << 21) | (rt << 16) | (off & 0xffff));
}

static void emit_mfc0(GuestImage *img, int rt, int rd, int sel)
{
    emit(img, OPC_MFC0 | (rt << 16) | (rd << 11) | sel);
}

static void emit_mtc0(GuestImage *img, int rt, int rd, int sel)
{
    emit(img, OPC_MTC0 | (rt << 16) | (rd << 11) | sel);
}

static void emit_load_u32(GuestImage *img, int rt, uint32_t value)
{
    emit(img, OPC_LUI | (rt << 16) | (value >> 16));
    emit(img, OPC_ORI | (rt << 21) | (rt << 16) | (value & 0xffff));
}

static void emit_cache(GuestImage *img, int operation, int base, uint16_t off)
{
    emit(img, OPC_CACHE | (base << 21) | (operation << 16) | off);
}

static void write_guest_image(int fd, const TestConfig *config)
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    ssize_t written;

    /* Branch clear of the low words so seeded board data cannot matter. */
    emit_branch(&img, CODE_START);
    emit(&img, 0);

    img.pos = CODE_START;
    emit_mfc0(&img, R_T0, CP0_STATUS, 0);
    emit(&img, OPC_ADDIU | (R_ZERO << 21) | (R_T1 << 16) | 0xfffb);
    emit(&img, OPC_AND | (R_T0 << 21) | (R_T1 << 16) | (R_T0 << 11));
    emit_mtc0(&img, R_T0, CP0_STATUS, 0);
    emit(&img, 0);

    emit(&img, OPC_LUI | (R_T0 << 16) | (INITIAL_COUNT >> 16));
    emit(&img, OPC_ORI | (R_T0 << 21) | (R_T0 << 16) |
               (INITIAL_COUNT & 0xffff));
    emit_mtc0(&img, R_T0, CP0_PERFORMANCE, config->counter_sel);
    emit(&img, OPC_ADDIU | (R_ZERO << 21) | (R_T0 << 16) | PERF_KERNEL);
    emit_mtc0(&img, R_T0, CP0_PERFORMANCE, config->control_sel);
    emit(&img, OPC_LUI | (R_T2 << 16) | STATE_KSEG1_HI);
    emit(&img, OPC_ADDIU | (R_ZERO << 21) | (R_T0 << 16) | READY_MAGIC);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | READY_OFF);

    img.pos = MAIN_LOOP;
    emit_mfc0(&img, R_T0, CP0_PERFORMANCE, config->counter_sel);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | COUNT_OFF);
    emit(&img, OPC_SYNC);
    emit_branch(&img, MAIN_LOOP);
    emit(&img, 0);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static void write_cache_guest_image(int fd, const CacheTagProbe *probes,
                                    size_t nprobes)
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    ssize_t written;
    size_t i;

    g_assert_cmpuint(nprobes, >, 0);

    /* Branch clear of the low words so seeded board data cannot matter. */
    emit_branch(&img, CACHE_START);
    emit(&img, 0);

    img.pos = CACHE_START;
    emit(&img, OPC_LUI | (R_T2 << 16) | STATE_KSEG1_HI);
    for (i = 0; i < nprobes; i++) {
        emit(&img, OPC_LUI | (R_T1 << 16) | probes[i].address_hi);
        emit_load_u32(&img, R_T0, probes[i].taglo);
        emit_mtc0(&img, R_T0, CP0_TAGLO, 0);
        emit_load_u32(&img, R_T0, probes[i].taghi);
        emit_mtc0(&img, R_T0, CP0_TAGHI, 0);
        emit_cache(&img, 0x0b, R_T1, 0);
    }
    /* Fill the quadword's other half first; the store below pads over it. */
    emit_load_u32(&img, R_T0, CACHE_OTHER_LO);
    emit_mtc0(&img, R_T0, CP0_TAGLO, 0);
    emit_load_u32(&img, R_T0, CACHE_OTHER_HI);
    emit_mtc0(&img, R_T0, CP0_TAGHI, 0);
    emit_load_u32(&img, R_T0, CACHE_OTHER_ECC);
    emit_mtc0(&img, R_T0, CP0_ERRCTL, 0);
    emit_cache(&img, 0x1f, R_T1, 0);
    emit_load_u32(&img, R_T0, CACHE_DATA_LO);
    emit_mtc0(&img, R_T0, CP0_TAGLO, 0);
    emit_load_u32(&img, R_T0, CACHE_DATA_HI);
    emit_mtc0(&img, R_T0, CP0_TAGHI, 0);
    emit_load_u32(&img, R_T0, CACHE_ECC);
    emit_mtc0(&img, R_T0, CP0_ERRCTL, 0);
    emit_cache(&img, 0x1f, R_T1, 8);
    emit_load_u32(&img, R_T0, CACHE_READY_MAGIC);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_READY_OFF);
    emit(&img, OPC_SYNC);

    img.pos = CACHE_LOOP;
    emit(&img, OPC_LD | (R_T2 << 21) | (R_T0 << 16) |
               CACHE_COMMAND_OFF);
    emit_bne(&img, R_T0, R_ZERO, CACHE_READ);
    emit(&img, 0);
    emit_branch(&img, CACHE_LOOP);
    emit(&img, 0);

    img.pos = CACHE_READ;
    for (i = 0; i < nprobes; i++) {
        emit(&img, OPC_LUI | (R_T1 << 16) | probes[i].address_hi);
        emit_mtc0(&img, R_ZERO, CP0_TAGLO, 0);
        emit_mtc0(&img, R_ZERO, CP0_TAGHI, 0);
        emit_cache(&img, 0x07, R_T1, 0);
        emit_mfc0(&img, R_T0, CP0_TAGLO, 0);
        emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) |
                   (CACHE_TAGLO_OFF + i * CACHE_TAG_STRIDE));
        emit_mfc0(&img, R_T0, CP0_TAGHI, 0);
        emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) |
                   (CACHE_TAGHI_OFF + i * CACHE_TAG_STRIDE));
    }
    emit_mtc0(&img, R_ZERO, CP0_TAGLO, 0);
    emit_mtc0(&img, R_ZERO, CP0_TAGHI, 0);
    emit_mtc0(&img, R_ZERO, CP0_ERRCTL, 0);
    emit_cache(&img, 0x1b, R_T1, 8);
    emit_mfc0(&img, R_T0, CP0_TAGLO, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_DATA_LO_OFF);
    emit_mfc0(&img, R_T0, CP0_TAGHI, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_DATA_HI_OFF);
    emit_mfc0(&img, R_T0, CP0_ERRCTL, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_ECC_OFF);
    emit_load_u32(&img, R_T0, CACHE_SENTINEL);
    emit_mtc0(&img, R_T0, CP0_TAGLO, 0);
    emit_mtc0(&img, R_T0, CP0_TAGHI, 0);
    emit_load_u32(&img, R_T0, CACHE_ECC_SENTINEL);
    emit_mtc0(&img, R_T0, CP0_ERRCTL, 0);
    emit_cache(&img, 0x1b, R_T1, 0);
    emit_mfc0(&img, R_T0, CP0_TAGLO, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_OTHER_LO_OFF);
    emit_mfc0(&img, R_T0, CP0_TAGHI, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_OTHER_HI_OFF);
    emit_mfc0(&img, R_T0, CP0_ERRCTL, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_OTHER_ECC_OFF);
    emit_load_u32(&img, R_T0, CACHE_DONE_MAGIC);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_DONE_OFF);
    emit(&img, OPC_SYNC);

    img.pos = CACHE_DONE;
    emit_branch(&img, CACHE_DONE);
    emit(&img, 0);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static uint64_t state_word(QTestState *qts, uint64_t off)
{
    return qtest_readq(qts, STATE_PHYS + off);
}

static void assert_cache_tags(QTestState *qts, const CacheTagProbe *probes,
                              size_t nprobes)
{
    size_t i;

    for (i = 0; i < nprobes; i++) {
        g_assert_cmphex(state_word(qts, CACHE_TAGLO_OFF +
                                   i * CACHE_TAG_STRIDE), ==,
                        probes[i].taglo);
        g_assert_cmphex(state_word(qts, CACHE_TAGHI_OFF +
                                   i * CACHE_TAG_STRIDE), ==,
                        probes[i].taghi);
    }
}

static uint64_t wait_for_change(QTestState *qts, uint64_t old)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        uint64_t value = state_word(qts, COUNT_OFF);

        if (value != old) {
            return value;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_not_reached();
}

static uint32_t wait_for_advance(QTestState *qts, uint32_t initial,
                                 uint32_t advance)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        uint32_t value = state_word(qts, COUNT_OFF);

        if (value - initial >= advance) {
            return value;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_not_reached();
}

static void wait_for_ready(QTestState *qts)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (state_word(qts, READY_OFF) == READY_MAGIC) {
            return;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_not_reached();
}

static void wait_for_state(QTestState *qts, uint64_t off, uint64_t expected)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (state_word(qts, off) == expected) {
            return;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(state_word(qts, off), ==, expected);
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
        g_usleep(1000);
    }
}

static void test_active_counter(const void *opaque)
{
    const TestConfig *config = opaque;
    g_autofree char *image = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *source_args = NULL;
    g_autofree char *dest_args = NULL;
    GError *error = NULL;
    QTestState *from, *to;
    uint32_t before, after, delta;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("sn0-r10k-perf-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_guest_image(fd, config);
    close(fd);
    directory = g_dir_make_tmp("sn0-r10k-migration-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);

    source_args = g_strdup_printf(
        "-M origin200,synthesized-firmware=off -m 64M -cpu %s -bios %s "
        "-accel tcg -no-reboot -no-shutdown", config->cpu, image);
    dest_args = g_strdup_printf(
        "-M origin200,synthesized-firmware=off -m 64M -cpu %s -bios %s "
        "-accel tcg -no-reboot -no-shutdown -incoming defer",
        config->cpu, image);
    from = qtest_init(source_args);
    to = qtest_init(dest_args);
    wait_for_ready(from);
    before = wait_for_change(from, INITIAL_COUNT);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    before = state_word(from, COUNT_OFF);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    wait_for_migration(from);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    after = wait_for_change(to, before);
    delta = after - before;
    g_test_message("%s before=%" PRIu32 " after=%" PRIu32
                   " delta=%" PRIu32, config->cpu, before, after, delta);
    g_assert_cmpuint(delta, >, 0);
    g_assert_cmpuint(delta, <, 1u << 30);

    qtest_quit(from);
    qtest_quit(to);
    unlink(image);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_stream_representation(const void *opaque)
{
    const TestConfig *config = opaque;
    g_autofree char *image = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *stream = NULL;
    g_autofree char *state = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *source_args = NULL;
    g_autofree char *json = NULL;
    GError *error = NULL;
    QTestState *from;
    const char *python = g_getenv("PYTHON");
    int fd, state_fd, pid, wstatus;

    if (!python) {
        g_test_skip("PYTHON variable not set");
        return;
    }

    fd = g_file_open_tmp("sn0-r10k-perf-XXXXXX", &image, &error);
    g_assert_no_error(error);
    write_guest_image(fd, config);
    close(fd);
    directory = g_dir_make_tmp("sn0-r10k-stream-XXXXXX", &error);
    g_assert_no_error(error);
    stream = g_build_filename(directory, "migration", NULL);
    state = g_build_filename(directory, "state.json", NULL);
    uri = g_strdup_printf("exec:cat > %s", stream);
    source_args = g_strdup_printf(
        "-M origin200,synthesized-firmware=off -m 64M -cpu %s -bios %s "
        "-accel tcg -no-reboot -no-shutdown", config->cpu, image);
    from = qtest_init(source_args);
    wait_for_ready(from);
    /* Let the counter advance far enough that the stream carries a value. */
    wait_for_advance(from, INITIAL_COUNT, STREAM_ADVANCE);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    wait_for_migration(from);
    qtest_quit(from);

    state_fd = open(state, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    g_assert_cmpint(state_fd, >=, 0);
    pid = fork();
    g_assert_cmpint(pid, >=, 0);
    if (!pid) {
        dup2(state_fd, STDOUT_FILENO);
        close(state_fd);
        execl(python, python, ANALYZE_SCRIPT, "-f", stream,
              "-d", "state", NULL);
        _exit(127);
    }
    close(state_fd);
    g_assert_cmpint(waitpid(pid, &wstatus, 0), ==, pid);
    g_assert_true(WIFEXITED(wstatus));
    g_assert_cmpint(WEXITSTATUS(wstatus), ==, 0);
    g_assert_true(g_file_get_contents(state, &json, NULL, &error));
    g_assert_no_error(error);

    g_assert_null(strstr(json, "\"cpu/r14k-performance\""));
    g_assert_null(strstr(json, "\"cpu/r14k-scache\""));

    unlink(image);
    unlink(stream);
    unlink(state);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_r10000_scache_migration(void)
{
    g_autofree char *image = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *source_args = NULL;
    g_autofree char *dest_args = NULL;
    GError *error = NULL;
    QTestState *from, *to;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("sn0-r10k-scache-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_cache_guest_image(fd, &r10000_cache_probe, 1);
    close(fd);
    directory = g_dir_make_tmp("sn0-r10k-scache-migration-XXXXXX",
                               &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    source_args = g_strdup_printf(
        "-M origin2000,synthesized-firmware=off -m 256M -smp 8 "
        "-cpu R10000 -bios %s -accel tcg,thread=single -nodefaults "
        "-no-reboot -no-shutdown", image);
    dest_args = g_strdup_printf(
        "-M origin2000,synthesized-firmware=off -m 256M -smp 8 "
        "-cpu R10000 -bios %s -accel tcg,thread=single -nodefaults "
        "-no-reboot -no-shutdown -incoming defer", image);
    from = qtest_init(source_args);
    to = qtest_init(dest_args);
    wait_for_state(from, CACHE_READY_OFF, CACHE_READY_MAGIC);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    wait_for_migration(from);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    qtest_writeq(to, STATE_PHYS + CACHE_COMMAND_OFF, 1);
    wait_for_state(to, CACHE_DONE_OFF, CACHE_DONE_MAGIC);

    assert_cache_tags(to, &r10000_cache_probe, 1);
    g_assert_cmphex(state_word(to, CACHE_DATA_LO_OFF), ==, CACHE_DATA_LO);
    g_assert_cmphex(state_word(to, CACHE_DATA_HI_OFF), ==, CACHE_DATA_HI);
    g_assert_cmphex(state_word(to, CACHE_ECC_OFF), ==, CACHE_ECC);

    qtest_quit(from);
    qtest_quit(to);
    unlink(image);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

/*
 * Index Store Data (S) writes the whole quadword: the doubleword the address
 * names comes from TagHi and TagLo, the other is padded with zeroes, and the
 * check bits from ECC[9:0] cover the result. Fill the other half first, store
 * the addressed half, and read the other half back: it must be zero and carry
 * the second store's check bits, not the first's data. The read goes through
 * all-ones sentinels so a load that did nothing cannot pass as a zero pad.
 */
static void test_r10000_scache_data_padding(void)
{
    g_autofree char *image = NULL;
    g_autofree char *args = NULL;
    GError *error = NULL;
    QTestState *qts;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("sn0-r10k-scache-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_cache_guest_image(fd, &r10000_cache_probe, 1);
    close(fd);
    args = g_strdup_printf(
        "-M origin200,synthesized-firmware=off -m 64M -cpu R10000 -bios %s "
        "-accel tcg -nodefaults -no-reboot -no-shutdown", image);
    qts = qtest_init(args);
    wait_for_state(qts, CACHE_READY_OFF, CACHE_READY_MAGIC);
    qtest_writeq(qts, STATE_PHYS + CACHE_COMMAND_OFF, 1);
    wait_for_state(qts, CACHE_DONE_OFF, CACHE_DONE_MAGIC);

    g_assert_cmphex(state_word(qts, CACHE_DATA_LO_OFF), ==, CACHE_DATA_LO);
    g_assert_cmphex(state_word(qts, CACHE_DATA_HI_OFF), ==, CACHE_DATA_HI);
    g_assert_cmphex(state_word(qts, CACHE_ECC_OFF), ==, CACHE_ECC);
    g_assert_cmphex(state_word(qts, CACHE_OTHER_LO_OFF), ==, 0);
    g_assert_cmphex(state_word(qts, CACHE_OTHER_HI_OFF), ==, 0);
    g_assert_cmphex(state_word(qts, CACHE_OTHER_ECC_OFF), ==, CACHE_ECC);

    qtest_quit(qts);
    unlink(image);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("sn0/r10k-perf-migration/r10000",
                        &configurations[0], test_active_counter);
    qtest_add_data_func("sn0/r10k-perf-stream/r10000",
                        &configurations[0], test_stream_representation);
    qtest_add_func("sn0/r10000-scache-migration/stock",
                   test_r10000_scache_migration);
    qtest_add_func("sn0/r10000-scache-data/padding",
                   test_r10000_scache_data_padding);
    return g_test_run();
}
