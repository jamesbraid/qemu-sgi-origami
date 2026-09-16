/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Identical-build migration for the Origin 300: the Bedrock node's
 * object graph now hangs off sgi-sn1-node (sn1-node.c) rather than the
 * old standalone SN1MachineState, so this proves migration still
 * completes and the moved devices still answer afterward -- the same
 * concern sn0-migration-test.c's own docstring names, applied here to
 * the node-types and Origin 300 steps' re-parenting instead of a
 * derived register value.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define PROM_SIZE UINT64_C(0x1686a8)

#define SPACE_HUB UINT64_C(0x10000000000)
#define HUB_LOCAL_REGISTERS (SPACE_HUB + UINT64_C(0x10000000))
#define BOARD_ID_LOCAL (HUB_LOCAL_REGISTERS + UINT64_C(0x4000000))
#define BOARD_ID_VALUE UINT64_C(0x8000)

#define L1_UART_BASE (SPACE_HUB + UINT64_C(0x30000000) + 0x80)
#define L1_UART_STATUS 0x28

static char *write_prom_image(const char *directory)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *prom = g_malloc0(PROM_SIZE);
    char *path = g_build_filename(directory, "prom.bin", NULL);

    g_assert_true(g_file_set_contents(path, (const char *)prom, PROM_SIZE,
                                      &error));
    g_assert_no_error(error);
    return path;
}

/*
 * Migrate and block until completed or failed, the way
 * sn0-migration-test.c's own migrate_stopped_vm does.
 */
static void migrate_stopped_vm(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);

    g_test_timer_start();
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(from,
            "{ 'execute': 'query-migrate' }");
        g_autofree char *status = g_strdup(qdict_get_str(ret, "status"));

        qobject_unref(ret);
        g_assert_cmpstr(status, !=, "failed");
        if (g_str_equal(status, "completed")) {
            break;
        }

        g_assert(g_test_timer_elapsed() < 30);
        g_usleep(1000);
    }
}

static void test_board_id_and_l1_uart_survive_migration(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-migration-XXXXXX",
                                                &error);
    g_autofree char *prom_path = write_prom_image(directory);
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *cmdline = NULL;
    g_autofree char *to_cmdline = NULL;
    QTestState *from, *to;
    uint64_t l1_status_before;

    g_assert_no_error(error);
    tmpfs = g_dir_make_tmp("sn1-migration-sock-XXXXXX", &error);
    g_assert_no_error(error);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    cmdline = g_strdup_printf(
        "-M origin300 -cpu R14000 -smp 2 -bios %s -display none -S",
        prom_path);
    from = qtest_init(cmdline);
    g_assert_cmphex(qtest_readq(from, BOARD_ID_LOCAL), ==, BOARD_ID_VALUE);
    l1_status_before = qtest_readq(from, L1_UART_BASE + L1_UART_STATUS);

    to_cmdline = g_strdup_printf("%s -incoming %s", cmdline, uri);
    to = qtest_init(to_cmdline);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readq(to, BOARD_ID_LOCAL), ==, BOARD_ID_VALUE);
    g_assert_cmphex(qtest_readq(to, L1_UART_BASE + L1_UART_STATUS), ==,
                    l1_status_before);

    qtest_quit(to);
    qtest_quit(from);
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn1/migration/board-id-and-l1-uart-survive",
                        NULL, test_board_id_and_l1_uart_survive_migration);
    return g_test_run();
}
