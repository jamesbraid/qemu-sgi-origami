/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Writable node PROMs use test-owned bytes and the existing flash commands. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define FLASH_SIZE (1024 * 1024)
#define FLASH_BASE UINT64_C(0x1fc00000)
#define PROBE 0x8000

static char *directory;
static char *input;
static char *stores[4];

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

static QTestState *start_stores(const char *machine, const char *extra)
{
    g_autoptr(GString) args = g_string_new(NULL);

    g_string_printf(args, "-M %s", machine);

    for (unsigned int i = 0; i < G_N_ELEMENTS(stores); i++) {
        g_string_append_printf(args,
            " -drive if=pflash,index=%u,file=%s,format=raw", i, stores[i]);
    }
    g_string_append_printf(args, " %s", extra);
    return qtest_init(args->str);
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
    if (which) {
        g_assert_cmpint(WEXITSTATUS(status), !=, 0);
        if (which == 1) {
            g_assert_nonnull(strstr(error_output, "all four node pflash"));
        } else if (which == 2) {
            g_assert_nonnull(strstr(error_output, "cannot combine -bios"));
        } else {
            g_assert_nonnull(strstr(error_output, "requires 1048576 bytes"));
        }
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
    qtest_add_data_func("sn0/origin2000/pflash/reordered-stores",
                        "origin2000-model-test", test_writable_stores);
    qtest_add_func("sn0/origin2000/pflash/migration", test_migration);
    qtest_add_data_func("sn0/origin2000/pflash/normal-launch",
                        GUINT_TO_POINTER(0), test_launch_selection);
    qtest_add_data_func("sn0/origin2000/pflash/reject-partial",
                        GUINT_TO_POINTER(1), test_launch_selection);
    qtest_add_data_func("sn0/origin2000/pflash/reject-bios-conflict",
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
