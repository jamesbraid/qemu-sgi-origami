/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test sgi-command-port.c with a fixture-only device: selector 0x1 takes
 * zero arguments, 0x2 takes three, and 0x3 takes eleven, the largest known
 * InfiniteReality packet.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"

#define CP_PATH "/machine/peripheral/cp"
#define CP_BASE 0x10000000ULL
#define MACHINE "-M none -cpu R10000 -device sgi-command-port-test,id=cp -S"

#define REG_PUSH               0x00
#define REG_CREDIT             0x08
#define REG_KICK               0x10
#define REG_PACKET_COUNT       0x18
#define REG_LAST_SELECTOR      0x20
#define REG_LAST_ARGC          0x28
#define REG_LAST_ARGS          0x30
#define REG_UNSUPPORTED_COUNT  0x130
#define REG_LAST_UNSUPPORTED   0x138

static QTestState *cp_start(const char *extra)
{
    QTestState *s = qtest_initf(MACHINE " %s", extra);

    qtest_sysbus_mmio_map(s, CP_PATH, 0, CP_BASE);
    return s;
}

static void test_framing(void)
{
    QTestState *s = cp_start("");

    /* A zero-argument selector completes as its own packet. */
    qtest_writel(s, CP_BASE + REG_PUSH, 0x1);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_PACKET_COUNT), ==, 1);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_SELECTOR), ==, 0x1);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_ARGC), ==, 0);

    /* A three-argument selector, one word per write. */
    qtest_writel(s, CP_BASE + REG_PUSH, 0x2);
    qtest_writel(s, CP_BASE + REG_PUSH, 0x10);
    qtest_writel(s, CP_BASE + REG_PUSH, 0x20);
    qtest_writel(s, CP_BASE + REG_PUSH, 0x30);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_PACKET_COUNT), ==, 2);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_SELECTOR), ==, 0x2);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_ARGC), ==, 3);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_ARGS + 0 * 8), ==, 0x10);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_ARGS + 1 * 8), ==, 0x20);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_ARGS + 2 * 8), ==, 0x30);
    qtest_quit(s);
}

/* A 64-bit store pushes two words, high half first. */
static void test_wide_push(void)
{
    QTestState *s = cp_start("");

    qtest_writeq(s, CP_BASE + REG_PUSH, (0x2ULL << 32) | 0x11);
    qtest_writeq(s, CP_BASE + REG_PUSH, (0x22ULL << 32) | 0x33);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_PACKET_COUNT), ==, 1);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_SELECTOR), ==, 0x2);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_ARGS + 0 * 8), ==, 0x11);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_ARGS + 1 * 8), ==, 0x22);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_ARGS + 2 * 8), ==, 0x33);
    qtest_quit(s);
}

/*
 * Interleave unrelated register accesses with eleven single-word payload
 * writes; they must preserve mid-packet parser state.
 */
static void test_split_payload(void)
{
    QTestState *s = cp_start("");

    qtest_writel(s, CP_BASE + REG_PUSH, 0x3);
    for (unsigned i = 0; i < 11; i++) {
        qtest_writel(s, CP_BASE + REG_PUSH, 0x100 + i);
        qtest_writeq(s, CP_BASE + REG_KICK, i);
        g_assert_cmphex(qtest_readq(s, CP_BASE + REG_KICK), ==, i);
        g_assert_cmphex(qtest_readq(s, CP_BASE + REG_CREDIT), ==, 0);
        g_assert_cmphex(qtest_readq(s, CP_BASE + REG_PACKET_COUNT),
                        ==, i == 10 ? 1 : 0);
    }
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_SELECTOR), ==, 0x3);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_ARGC), ==, 11);
    for (unsigned i = 0; i < 11; i++) {
        g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_ARGS + i * 8),
                        ==, 0x100 + i);
    }
    qtest_quit(s);
}

/*
 * Report an unknown selector once; parse the next word as a new selector.
 */
static void test_unsupported_selectors(void)
{
    QTestState *s = cp_start("");

    qtest_writel(s, CP_BASE + REG_PUSH, 0xdead);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_UNSUPPORTED_COUNT), ==, 1);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_LAST_UNSUPPORTED), ==, 0xdead);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_PACKET_COUNT), ==, 0);

    qtest_writel(s, CP_BASE + REG_PUSH, 0x1);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_PACKET_COUNT), ==, 1);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_UNSUPPORTED_COUNT), ==, 1);
    qtest_quit(s);
}

/*
 * Return the owner callback's settable value for credit/occupancy; the
 * command-port object must not impose its own policy.
 */
static void test_credit_policy(void)
{
    QTestState *s = cp_start("");

    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_CREDIT), ==, 0);
    qtest_writeq(s, CP_BASE + REG_CREDIT, 0x7fe);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_CREDIT), ==, 0x7fe);
    qtest_writeq(s, CP_BASE + REG_CREDIT, 0);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_CREDIT), ==, 0);
    qtest_quit(s);
}

static void test_kick_register(void)
{
    QTestState *s = cp_start("");

    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_KICK), ==, 0);
    qtest_writeq(s, CP_BASE + REG_KICK, 0x700);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_KICK), ==, 0x700);
    qtest_quit(s);
}

/*
 * Counters and last completed or unsupported word fields are guest read-only;
 * writes must log a guest error.
 */
static void test_read_only_writes(void)
{
    g_autofree char *log =
        g_strdup_printf("%s/sgi-command-port-readonly-XXXXXX.log",
                        g_get_tmp_dir());
    g_autofree char *extra = NULL;
    g_autofree char *found = NULL;
    static const uint64_t offsets[] = {
        REG_PACKET_COUNT, REG_LAST_SELECTOR, REG_LAST_ARGC,
        REG_UNSUPPORTED_COUNT, REG_LAST_UNSUPPORTED,
    };
    QTestState *s;
    int fd = g_mkstemp(log);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = g_strdup_printf("-d guest_errors -D %s", log);
    s = cp_start(extra);
    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
        qtest_writeq(s, CP_BASE + offsets[i], UINT64_MAX);
    }
    qtest_quit(s);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
        g_autofree char *line = g_strdup_printf(
            "write to read-only register 0x%" PRIx64, offsets[i]);

        g_assert_nonnull(strstr(found, line));
    }
    g_assert_cmpint(unlink(log), ==, 0);
}

/*
 * Reset clears registers and mid-packet parser state; ten remaining words
 * must not finish a pre-reset eleven-argument packet.
 */
static void test_reset(void)
{
    QTestState *s = cp_start("");

    qtest_writel(s, CP_BASE + REG_PUSH, 0x3);
    qtest_writel(s, CP_BASE + REG_PUSH, 0xaa);
    qtest_writeq(s, CP_BASE + REG_KICK, 0x700);
    qtest_writeq(s, CP_BASE + REG_CREDIT, 0x7fe);

    qtest_system_reset(s);

    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_KICK), ==, 0);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_CREDIT), ==, 0);
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_PACKET_COUNT), ==, 0);

    /*
     * Use 0x100 + i to avoid fixture selectors 0x1, 0x2 and 0x3. Values
     *      * 0..9 would restart framing: 1 takes no args and 2 consumes 3,
     *        4, 5.
     */
    for (unsigned i = 0; i < 10; i++) {
        qtest_writel(s, CP_BASE + REG_PUSH, 0x100 + i);
    }
    g_assert_cmphex(qtest_readq(s, CP_BASE + REG_PACKET_COUNT), ==, 0);
    qtest_quit(s);
}

/*
 * Migrate after the selector and two of three arguments; completing the
 * packet on the destination checks parser state as well as registers.
 */
static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sgi-command-port-migration-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = cp_start("");
    to = cp_start("-incoming defer");

    qtest_writel(from, CP_BASE + REG_PUSH, 0x2);
    qtest_writel(from, CP_BASE + REG_PUSH, 0x10);
    qtest_writel(from, CP_BASE + REG_PUSH, 0x20);
    qtest_writeq(from, CP_BASE + REG_KICK, 0x700);
    qtest_writeq(from, CP_BASE + REG_CREDIT, 0x7fe);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);

    g_assert_cmphex(qtest_readq(to, CP_BASE + REG_KICK), ==, 0x700);
    g_assert_cmphex(qtest_readq(to, CP_BASE + REG_CREDIT), ==, 0x7fe);
    g_assert_cmphex(qtest_readq(to, CP_BASE + REG_PACKET_COUNT), ==, 0);

    qtest_writel(to, CP_BASE + REG_PUSH, 0x30);
    g_assert_cmphex(qtest_readq(to, CP_BASE + REG_PACKET_COUNT), ==, 1);
    g_assert_cmphex(qtest_readq(to, CP_BASE + REG_LAST_SELECTOR), ==, 0x2);
    g_assert_cmphex(qtest_readq(to, CP_BASE + REG_LAST_ARGS + 0 * 8), ==, 0x10);
    g_assert_cmphex(qtest_readq(to, CP_BASE + REG_LAST_ARGS + 1 * 8), ==, 0x20);
    g_assert_cmphex(qtest_readq(to, CP_BASE + REG_LAST_ARGS + 2 * 8), ==, 0x30);

    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sgi-command-port/framing", test_framing);
    qtest_add_func("sgi-command-port/wide-push", test_wide_push);
    qtest_add_func("sgi-command-port/split-payload", test_split_payload);
    qtest_add_func("sgi-command-port/unsupported-selectors",
                   test_unsupported_selectors);
    qtest_add_func("sgi-command-port/credit-policy", test_credit_policy);
    qtest_add_func("sgi-command-port/kick-register", test_kick_register);
    qtest_add_func("sgi-command-port/read-only-writes",
                   test_read_only_writes);
    qtest_add_func("sgi-command-port/reset", test_reset);
    qtest_add_func("sgi-command-port/migration", test_migration);
    return g_test_run();
}
