/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test Origin 2000 node PROM storage and AMD-style flash commands. */
/*
 * Each node has one byte-wide pflash_cfi02 part and a separate backing file.
 * System reset restores read-array mode; read-only backing makes mutations
 * logged no-ops. sh4 r2d is the only other in-tree machine with this part;
 * checking these behaviors there required a second emulator.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define FLASH_SIZE (1024 * 1024)
#define FLASH_BASE UINT64_C(0x1fc00000)
#define PROBE 0x8000

static char *directory;
static char *input;
static char *stores[4];

/* The part's identity and command set (hw/mips/sgi/sn0.c, sn0_flash). */
#define FLASH_MANUFACTURER_ID 0x01
#define SECTOR_SIZE (64 * 1024)
#define CMD_AUTOSELECT 0x90
#define CMD_PROGRAM 0xa0
#define CMD_ERASE 0x80
#define CMD_SECTOR_ERASE 0x30
#define CMD_CHIP_ERASE 0x10
#define CMD_RESET 0xf0
#define CMD_CFI_QUERY 0x98
#define CFI_QUERY_ADDR 0x55
#define STATUS_DQ6 0x40

static void flash_write(QTestState *qts, unsigned int node, uint32_t offset,
                        uint8_t value)
{
    g_assert_true(qtest_memwrite_cpu(qts, node * 2, FLASH_BASE + offset,
                                    &value, sizeof(value)));
}

static uint8_t flash_read(QTestState *qts, unsigned int node, uint32_t offset)
{
    uint8_t value;

    g_assert_true(qtest_memread_cpu(qts, node * 2, FLASH_BASE + offset,
                                   &value, sizeof(value)));
    return value;
}

static void flash_unlock(QTestState *qts, unsigned int node, uint8_t command)
{
    flash_write(qts, node, 0x555, 0xaa);
    flash_write(qts, node, 0x2aa, 0x55);
    flash_write(qts, node, 0x555, command);
}

static void flash_program(QTestState *qts, unsigned int node, uint32_t offset,
                          uint8_t value)
{
    flash_unlock(qts, node, 0xa0);
    flash_write(qts, node, offset, value);
}

static void flash_erase_prefix(QTestState *qts, unsigned int node)
{
    flash_unlock(qts, node, CMD_ERASE);
    flash_write(qts, node, 0x555, 0xaa);
    flash_write(qts, node, 0x2aa, 0x55);
}

static void flash_sector_erase(QTestState *qts, unsigned int node,
                               uint32_t offset)
{
    flash_erase_prefix(qts, node);
    flash_write(qts, node, offset, CMD_SECTOR_ERASE);
}

static void flash_chip_erase(QTestState *qts, unsigned int node)
{
    flash_erase_prefix(qts, node);
    flash_write(qts, node, 0x555, CMD_CHIP_ERASE);
}

/*
 * An erase in progress toggles DQ6 on every read, so two reads in a row agree
 * only once it has finished. The part publishes its own deadlines: a sector
 * erase waits 50 us for further sector commands and then 512 us per sector, a
 * chip erase 4096 ms. Step the virtual clock past the one asked for. This
 * machine's real-time counter ticks throughout, so stepping to the next timer
 * deadline arrives at that counter rather than at the flash.
 */
#define SECTOR_ERASE_NS (1ULL * 1000 * 1000)
#define CHIP_ERASE_NS (5ULL * 1000 * 1000 * 1000)

static void wait_for_completion(QTestState *qts, unsigned int node,
                                uint32_t offset, uint64_t nanoseconds)
{
    qtest_clock_step(qts, nanoseconds);
    g_assert_cmphex(flash_read(qts, node, offset), ==,
                    flash_read(qts, node, offset));
}

static void prepare_stores(void)
{
    g_autofree char *bytes = g_malloc(FLASH_SIZE);

    memset(bytes, 0xff, FLASH_SIZE);
    /* A recognizable input byte catches any machine-side reseeding. */
    bytes[0x100] = 0x69;
    g_assert_true(g_file_set_contents(input, bytes, FLASH_SIZE, NULL));
    for (unsigned int i = 0; i < G_N_ELEMENTS(stores); i++) {
        g_assert_true(g_file_set_contents(stores[i], bytes, FLASH_SIZE, NULL));
    }
}

static QTestState *start_stores_with(const char *machine,
                                     const char *drive_options,
                                     const char *extra)
{
    g_autoptr(GString) args = g_string_new(NULL);

    g_string_printf(args, "-M %s", machine);

    for (unsigned int i = 0; i < G_N_ELEMENTS(stores); i++) {
        g_string_append_printf(args,
            " -drive if=pflash,index=%u,file=%s,format=raw%s", i, stores[i],
            drive_options);
    }
    g_string_append_printf(args, " %s", extra);
    return qtest_init(args->str);
}

static QTestState *start_stores(const char *machine, const char *extra)
{
    return start_stores_with(machine, "", extra);
}

/*
 * Stores filled with a pattern no erase or program could leave behind, so an
 * unchanged read means the mutation really was refused rather than landing on
 * bytes that already held the value.
 */
static void prepare_patterned_stores(void)
{
    g_autofree char *bytes = g_malloc(FLASH_SIZE);

    for (size_t i = 0; i < FLASH_SIZE; i++) {
        bytes[i] = (char)(0x5a + i);
    }
    g_assert_true(g_file_set_contents(input, bytes, FLASH_SIZE, NULL));
    for (unsigned int i = 0; i < G_N_ELEMENTS(stores); i++) {
        g_assert_true(g_file_set_contents(stores[i], bytes, FLASH_SIZE, NULL));
    }
}

static char *make_log_path(const char *stem)
{
    g_autofree char *path = g_strdup_printf("%s/%s.log", directory, stem);
    int fd = g_open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return g_steal_pointer(&path);
}

/*
 * Whether one node's chip is mapped read-array, as the machine itself reports
 * it: a part in read-array answers fetches from its own storage and the flat
 * view calls that romd; any command mode takes the mapping away.
 */
static void assert_flash_read_array(QTestState *qts, unsigned int node,
                                    bool read_array)
{
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *name = g_strdup_printf(": sn0.node%u.flash", node);
    g_autofree char *line = NULL;
    const char *hit;
    const char *start;

    /* The chip itself, not the controller views whose names extend it. */
    for (hit = strstr(mtree, name); hit; hit = strstr(hit + 1, name)) {
        if (hit[strlen(name)] != '.') {
            break;
        }
    }
    g_assert_nonnull(hit);
    for (start = hit; start > mtree && start[-1] != '\n'; start--) {
        continue;
    }
    line = g_strndup(start, (size_t)(hit - start));
    g_assert_cmpint(strstr(line, "romd") != NULL, ==, read_array);
}

static void assert_file_byte(const char *path, uint32_t offset, uint8_t value)
{
    g_autofree char *bytes = NULL;
    gsize length;

    g_assert_true(g_file_get_contents(path, &bytes, &length, NULL));
    g_assert_cmpuint(length, ==, FLASH_SIZE);
    g_assert_cmphex((uint8_t)bytes[offset], ==, value);
}

static void test_writable_stores(gconstpointer data)
{
    QTestState *qts;
    const char *machine = data;

    prepare_stores();
    qts = start_stores(machine, "");
    for (unsigned int node = 0; node < G_N_ELEMENTS(stores); node++) {
        g_assert_cmphex(flash_read(qts, node, 0x100), ==, 0x69);
        flash_program(qts, node, PROBE, 0xe0 | node);
        for (unsigned int other = 0; other < G_N_ELEMENTS(stores); other++) {
            g_assert_cmphex(flash_read(qts, other, PROBE), ==,
                            other <= node ? (0xe0 | other) : 0xff);
        }
    }
    qtest_system_reset(qts);
    for (unsigned int node = 0; node < G_N_ELEMENTS(stores); node++) {
        g_assert_cmphex(flash_read(qts, node, PROBE), ==, 0xe0 | node);
    }
    qtest_quit(qts);

    /* Backing files are exportable raw images after the run has stopped. */
    for (unsigned int node = 0; node < G_N_ELEMENTS(stores); node++) {
        assert_file_byte(stores[node], PROBE, 0xe0 | node);
        assert_file_byte(stores[node], 0x100, 0x69);
    }
    assert_file_byte(input, PROBE, 0xff);
    assert_file_byte(input, 0x100, 0x69);
    qts = start_stores(machine, "");
    for (unsigned int node = 0; node < G_N_ELEMENTS(stores); node++) {
        g_assert_cmphex(flash_read(qts, node, PROBE), ==, 0xe0 | node);
    }
    qtest_quit(qts);
}

static void test_migration(void)
{
    g_autofree char *state = g_build_filename(directory, "migration.bin", NULL);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    g_autofree char *incoming = g_strdup_printf("-incoming %s", uri);
    QTestState *from, *to;
    int64_t deadline;

    prepare_stores();
    from = start_stores("origin2000", "");
    flash_program(from, 3, PROBE, 0x93);
    flash_unlock(from, 0, 0xa0);
    flash_unlock(from, 1, 0x90);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(from,
            "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(qdict_get_qdict(response, "return"),
                                          "status");

        g_assert_cmpstr(status, !=, "failed");
        if (!strcmp(status, "completed")) {
            break;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    /* The destination uses the same backing files after the source closes. */
    qtest_quit(from);
    to = start_stores("origin2000", incoming);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(to,
            "{'execute':'query-status'}");

        if (qdict_get_bool(qdict_get_qdict(response, "return"), "running")) {
            break;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    /* Resume an unfinished program command and a different node's ID mode. */
    flash_write(to, 0, PROBE, 0x60);
    g_assert_cmphex(flash_read(to, 0, PROBE), ==, 0x60);
    g_assert_cmphex(flash_read(to, 1, 0), ==, 1);
    g_assert_cmphex(flash_read(to, 3, PROBE), ==, 0x93);
    qtest_system_reset(to);
    g_assert_cmphex(flash_read(to, 1, 0), ==, 0xff);
    g_assert_cmphex(flash_read(to, 0, PROBE), ==, 0x60);
    qtest_quit(to);
    assert_file_byte(stores[0], PROBE, 0x60);
    assert_file_byte(stores[3], PROBE, 0x93);
    assert_file_byte(input, PROBE, 0xff);
    unlink(state);
}

/*
 * A system reset must return the chip to read-array mapping. Data reads
 * answer identically either way, so the observable difference is the mapping
 * mode itself: read it from the machine's own flat view rather than inferring
 * it from data.
 */
static void test_reset_to_read_array(void)
{
    QTestState *qts;

    prepare_stores();
    qts = start_stores("origin2000", "");

    assert_flash_read_array(qts, 0, true);

    /* Leave read-array mapping by entering autoselect. */
    flash_unlock(qts, 0, CMD_AUTOSELECT);
    g_assert_cmphex(flash_read(qts, 0, 0), ==, FLASH_MANUFACTURER_ID);
    assert_flash_read_array(qts, 0, false);

    qtest_system_reset(qts);
    assert_flash_read_array(qts, 0, true);
    qtest_quit(qts);
}

/*
 * A read-only backing store makes every mutation a no-op. The device used to
 * swallow them without a word, which reads from the guest side exactly like a
 * chip that programmed fine, so the diagnostic is the only way to tell an
 * ignored write from a successful one. Attempt each of the three mutations
 * and require both the complaint and the untouched bytes.
 */
static void test_readonly_mutations_diagnosed(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree char *before = NULL;
    g_autofree char *after = NULL;
    g_autofree char *arguments = NULL;
    gsize before_length, after_length;
    uint8_t byte0, byte1;
    QTestState *qts;

    prepare_patterned_stores();
    log = make_log_path("readonly-mutations");
    arguments = g_strdup_printf("-d guest_errors -D %s", log);
    g_assert_true(g_file_get_contents(stores[0], &before, &before_length,
                                      NULL));
    qts = start_stores_with("origin2000", ",readonly=on", arguments);

    byte0 = flash_read(qts, 0, 0);
    byte1 = flash_read(qts, 0, SECTOR_SIZE);

    /*
     * The pattern, not an erased chip: an unchanged read must mean something
     */
    g_assert_cmphex(byte0, !=, 0xff);

    flash_program(qts, 0, 0, 0x12);
    g_assert_cmphex(flash_read(qts, 0, 0), ==, byte0);

    /* Sector erase, run to completion so reads return data again. */
    flash_sector_erase(qts, 0, 0);
    wait_for_completion(qts, 0, 0, SECTOR_ERASE_NS);
    g_assert_cmphex(flash_read(qts, 0, 0), ==, byte0);

    flash_chip_erase(qts, 0);
    wait_for_completion(qts, 0, 0, CHIP_ERASE_NS);
    g_assert_cmphex(flash_read(qts, 0, 0), ==, byte0);
    g_assert_cmphex(flash_read(qts, 0, SECTOR_SIZE), ==, byte1);

    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_nonnull(strstr(found, "word program ignored: backing store is "
                                   "read-only"));
    g_assert_nonnull(strstr(found, "sector erase ignored: backing store is "
                                   "read-only"));
    g_assert_nonnull(strstr(found, "chip erase ignored: backing store is "
                                   "read-only"));
    g_assert_cmpint(unlink(log), ==, 0);

    /* The backing file itself is byte-for-byte what we wrote. */
    g_assert_true(g_file_get_contents(stores[0], &after, &after_length, NULL));
    g_assert_cmpuint(after_length, ==, before_length);
    g_assert_cmpint(memcmp(before, after, before_length), ==, 0);
}

/*
 * The error belongs to the mutation, not to the command decoder. Query,
 * autoselect and reset never reach a mutation, so a read-only backend must
 * leave the log empty for them. Every message is flushed as it is written, so
 * the log can be read back while the machine is still up: program afterwards
 * and the same instance proves the backend really was read-only, rather than
 * leaving the silence to be explained by a writable drive.
 */
static void test_readonly_queries_silent(void)
{
    g_autofree char *log = NULL;
    g_autofree char *quiet = NULL;
    g_autofree char *noisy = NULL;
    g_autofree char *arguments = NULL;
    gsize length = 0;
    QTestState *qts;

    prepare_patterned_stores();
    log = make_log_path("readonly-queries");
    arguments = g_strdup_printf("-d guest_errors -D %s", log);
    qts = start_stores_with("origin2000", ",readonly=on", arguments);

    flash_unlock(qts, 0, CMD_AUTOSELECT);
    g_assert_cmphex(flash_read(qts, 0, 0), ==, FLASH_MANUFACTURER_ID);
    flash_write(qts, 0, CFI_QUERY_ADDR, CMD_CFI_QUERY);
    g_assert_cmphex(flash_read(qts, 0, 0x10), ==, 'Q');
    flash_write(qts, 0, 0, CMD_RESET);
    flash_write(qts, 0, 0, CMD_RESET);

    g_assert_true(g_file_get_contents(log, &quiet, &length, NULL));
    g_assert_cmpuint(length, ==, 0);

    flash_program(qts, 0, 0, 0x12);
    g_assert_true(g_file_get_contents(log, &noisy, NULL, NULL));
    g_assert_nonnull(strstr(noisy, "word program ignored: backing store is "
                                   "read-only"));

    qtest_quit(qts);
    g_assert_cmpint(unlink(log), ==, 0);
}

static void test_launch_selection(gconstpointer data)
{
    unsigned int which = GPOINTER_TO_UINT(data);
    unsigned int count = which == 1 ? 1 : 4;
    g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
    const char *base[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000", "-display", "none",
        "-S", "-nodefaults", "-qmp", "stdio", NULL,
    };
    const char commands[] = "{\"execute\":\"qmp_capabilities\"}\n"
                            "{\"execute\":\"quit\"}\n";
    struct sigaction ignore = { .sa_handler = SIG_IGN }, previous;
    char error_output[4096] = { 0 };
    GPid pid;
    int input_fd, error_fd, status;
    ssize_t length;

    prepare_stores();
    for (unsigned int i = 0; base[i]; i++) {
        g_ptr_array_add(args, g_strdup(base[i]));
    }
    for (unsigned int i = 0; i < count; i++) {
        g_ptr_array_add(args, g_strdup("-drive"));
        g_ptr_array_add(args, g_strdup_printf(
            "if=pflash,index=%u,file=%s,format=raw", i, stores[i]));
    }
    if (which == 2) {
        g_ptr_array_add(args, g_strdup("-bios"));
        g_ptr_array_add(args, g_strdup(input));
    } else if (which == 3) {
        /* The block layer rounds file lengths up to 512-byte sectors. */
        g_assert_cmpint(truncate(stores[2], FLASH_SIZE - 512), ==, 0);
    }
    g_ptr_array_add(args, NULL);
    g_assert_true(g_spawn_async_with_pipes(NULL, (char **)args->pdata, NULL,
                    G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL,
                    NULL, NULL, &pid, &input_fd, NULL, &error_fd, NULL));
    sigemptyset(&ignore.sa_mask);
    g_assert_cmpint(sigaction(SIGPIPE, &ignore, &previous), ==, 0);
    length = qemu_write_full(input_fd, commands, sizeof(commands) - 1);
    g_assert_true(length == sizeof(commands) - 1 ||
                  (length == -1 && errno == EPIPE));
    g_assert_cmpint(sigaction(SIGPIPE, &previous, NULL), ==, 0);
    close(input_fd);
    g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
    length = read(error_fd, error_output, sizeof(error_output) - 1);
    g_assert_cmpint(length, >=, 0);
    close(error_fd);
    g_spawn_close_pid(pid);
    g_test_message("launch output: %s", error_output);
    g_assert_true(WIFEXITED(status));
    /*
     * which == 1 and which == 2 used to be refused under Origin 2000's own
     * rules ("requires all four node pflash drives", "cannot combine -bios
     * with node pflash drives"). The one-builder step's unified rule (-bios
     * fills every node's flash, per-node pflash overrides) accepts both:
     * which == 1's three drive-less nodes come up with blank flash, the same
     * as a node would with neither -bios nor a drive of its own; which == 2's
     * four drives leave nothing for -bios to fill, the same as passing -bios
     * alongside a full population on every other product. which == 3's
     * wrong-sized backing file is still refused. That is the generic pflash
     * device's own size check, not an Origin 2000 rule.
     */
    if (which == 3) {
        g_assert_cmpint(WEXITSTATUS(status), !=, 0);
        g_assert_nonnull(strstr(error_output, "requires 1048576 bytes"));
    } else {
        g_assert_cmpint(WEXITSTATUS(status), ==, 0);
    }
}

int main(int argc, char **argv)
{
    int result;

    g_test_init(&argc, &argv, NULL);
    directory = g_dir_make_tmp("origin2000-pflash-XXXXXX", NULL);
    g_assert_nonnull(directory);
    input = g_build_filename(directory, "input.bin", NULL);
    for (unsigned int i = 0; i < G_N_ELEMENTS(stores); i++) {
        stores[i] = g_strdup_printf("%s/node%u.bin", directory, i + 1);
    }
    qtest_add_data_func("sn0/origin2000/pflash/writable-stores",
                        "origin2000", test_writable_stores);
    /*
     * The reordered-stores variant ran the same assertions against a fixture
     * machine that built its nodes in an order different from the
     * descriptor's, to catch a store keyed to the wrong physical node. That
     * fixture depended on the deleted per-product descriptor
     * (SN0ModelComposition) and its physical-slot indirection; format 2 has
     * no such indirection left to get wrong; node array index and descriptor
     * node index are the same number everywhere the builder reads, so there
     * is no second order left to test against.
     */
    qtest_add_func("sn0/origin2000/pflash/migration", test_migration);
    qtest_add_func("sn0/origin2000/pflash/reset-to-read-array",
                   test_reset_to_read_array);
    qtest_add_func("sn0/origin2000/pflash/readonly-mutations-diagnosed",
                   test_readonly_mutations_diagnosed);
    qtest_add_func("sn0/origin2000/pflash/readonly-queries-silent",
                   test_readonly_queries_silent);
    qtest_add_data_func("sn0/origin2000/pflash/normal-launch",
                        GUINT_TO_POINTER(0), test_launch_selection);
    qtest_add_data_func("sn0/origin2000/pflash/partial-drives",
                        GUINT_TO_POINTER(1), test_launch_selection);
    qtest_add_data_func("sn0/origin2000/pflash/bios-with-full-drives",
                        GUINT_TO_POINTER(2), test_launch_selection);
    qtest_add_data_func("sn0/origin2000/pflash/reject-wrong-size",
                        GUINT_TO_POINTER(3), test_launch_selection);
    result = g_test_run();

    unlink(input);
    g_free(input);
    for (unsigned int i = 0; i < G_N_ELEMENTS(stores); i++) {
        unlink(stores[i]);
        g_free(stores[i]);
    }
    g_assert_cmpint(rmdir(directory), ==, 0);
    g_free(directory);
    return result;
}
