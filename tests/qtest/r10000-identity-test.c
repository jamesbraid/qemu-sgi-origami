/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CP0 identity using original synthetic instructions, without a guest image. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-migrate.h"

#define MAILBOX 0x1000

static char *image_path;

static QTestState *start(const char *machine, const char *cpu,
                        const char *extra)
{
    g_autofree char *quoted = g_shell_quote(image_path);

    return qtest_initf("-M %s -m 128M -cpu %s -bios %s "
                       "-nic none -accel tcg -S %s",
                       machine, cpu, quoted, extra);
}

static void read_identity(QTestState *qts, uint32_t expected)
{
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    /* Clear completion while stopped to require a new executed CP0 read. */
    qtest_writel(qts, MAILBOX + 8, 0);
    qtest_writel(qts, MAILBOX + 12, 1);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    while (!qtest_readl(qts, MAILBOX + 8)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, MAILBOX), ==, expected);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 4), ==, expected);
}

static void test_identity(gconstpointer data)
{
    const char *machine = data;
    uint32_t expected = !strcmp(machine, "octane") ? 0x927 : 0x926;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start(machine, "R10000", "");

    read_identity(qts, expected);
    for (unsigned int i = 0; i < 2; i++) {
        qtest_system_reset(qts);
        read_identity(qts, expected);
    }
    qtest_quit(qts);
}

static void test_invalid_identity(void)
{
    const char *profiles[] = {
        "R10000,r10000-prid=0xe27",
        "R10000,r10000-prid=0x10927",
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(profiles); i++) {
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", "malta", "-cpu", profiles[i],
            "-display", "none", "-S", NULL,
        };
        g_autofree char *error_output = NULL;
        int status;

        g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                                   NULL, &error_output, &status, NULL));
        g_assert_false(g_spawn_check_exit_status(status, NULL));
        g_assert_nonnull(strstr(error_output, "requires implementation 0x09"));
    }
}

static void test_immutable_identity(void)
{
    QTestState *qts;
    g_autoptr(QDict) cpus = NULL;
    g_autoptr(QDict) reply = NULL;
    QDict *cpu, *error;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start("malta", "R10000", "");
    cpus = qtest_qmp(qts, "{'execute':'query-cpus-fast'}");
    cpu = qobject_to(QDict, qlist_peek(qdict_get_qlist(cpus, "return")));
    reply = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'r10000-prid','value':2343}}",
        qdict_get_str(cpu, "qom-path"));
    error = qdict_get_qdict(reply, "error");

    g_assert_nonnull(error);
    g_assert_nonnull(strstr(qdict_get_str(error, "desc"),
                            "cannot change after CPU realization"));
    read_identity(qts, 0x926);
    qtest_quit(qts);
}

static void test_migration(gconstpointer data)
{
    bool mismatch = GPOINTER_TO_INT(data);
    g_autofree char *dir = g_dir_make_tmp("r10000-identity-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "state.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *incoming = g_strdup_printf("-incoming %s", uri);
    const char *machine = mismatch ? "malta" : "octane";
    QTestState *from, *to;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        g_assert_cmpint(g_rmdir(dir), ==, 0);
        return;
    }
    from = start(machine, "R10000,r10000-prid=0x927", "");
    to = start(machine, "R10000", incoming);

    read_identity(from, 0x927);
    if (mismatch) {
        int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

        qtest_set_expected_status(to, 1);
        qtest_qmp_assert_success(from,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        while (qtest_probe_child(to)) {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
        }
    } else {
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration_status(to, "completed", 30);
        read_identity(to, 0x927);
        qtest_system_reset(to);
        read_identity(to, 0x927);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    /* Each loop reads PRId, attempts a write, then publishes both results. */
    static const uint32_t code[] = {
        0x3c1aa000,             /* lui k0, 0xa000 */
        0x375a1000,             /* ori k0, k0, 0x1000 */
        0x8f48000c,             /* lw t0, 12(k0): wait for a fresh request */
        0x1100fffe,             /* beq t0, zero, request loop */
        0,                     /* delay slot */
        0xaf40000c,             /* sw zero, 12(k0): consume request */
        0x40087800,             /* mfc0 t0, PRId */
        0xaf480000,             /* sw t0, 0(k0) */
        0x2409ffff,             /* addiu t1, zero, -1 */
        0x40897800,             /* mtc0 t1, PRId */
        0, 0, 0, 0,           /* CP0 write hazard spacing */
        0x40087800,             /* mfc0 t0, PRId */
        0xaf480004,             /* sw t0, 4(k0) */
        0x24080001,             /* addiu t0, zero, 1 */
        0xaf480008,             /* sw t0, 8(k0) */
        0x1000ffef,             /* branch to request loop */
        0,                     /* delay slot */
    };
    uint8_t image[0x2000] = { 0 };
    int fd, result;

    g_test_init(&argc, &argv, NULL);
    for (unsigned int i = 0; i < G_N_ELEMENTS(code); i++) {
        stl_be_p(image + i * 4, code[i]);
    }
    fd = g_file_open_tmp("r10000-identity-code-XXXXXX", &image_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    close(fd);
    qtest_add_data_func("/r10000/identity/octane", "octane", test_identity);
    if (qtest_has_machine("malta")) {
        qtest_add_data_func("/r10000/identity/default", "malta", test_identity);
        qtest_add_func("/r10000/identity/invalid", test_invalid_identity);
        qtest_add_func("/r10000/identity/immutable", test_immutable_identity);
        qtest_add_data_func("/r10000/identity/migration-mismatch",
                            GINT_TO_POINTER(1), test_migration);
    }
    qtest_add_data_func("/r10000/identity/migration",
                        GINT_TO_POINTER(0), test_migration);
    result = g_test_run();
    unlink(image_path);
    g_free(image_path);
    return result;
}
