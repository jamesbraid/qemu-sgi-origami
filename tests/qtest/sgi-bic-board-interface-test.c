/* SPDX-License-Identifier: BSD-3-Clause */
/* Synthetic BIC inputs; no firmware or physical controller bus. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"

#define BASE 0x10000000ULL
#define PATH "/machine/peripheral/bic"
#define ID 0x112233445566c788ULL
#define OTHER_ID 0xaabbccddeeffc900ULL
#define ID_OFFSET 0x34000000ULL
#define LATCH_OFFSET 0x35000000ULL
#define TX_ERROR 1

static QTestState *start(uint64_t identity, bool initial, uint16_t latch,
                         const char *extra)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 -S "
        "-device sgi-bic-board-interface-fixture,id=bic,identity=0x%" PRIx64
        ",initial-latch-set=%s,initial-latch=%u %s",
        identity, initial ? "on" : "off", latch, extra);

    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    return s;
}

/* Submit remote accesses and expose their transaction results. */
static uint64_t remote(QTestState *s, uint64_t offset, unsigned size,
                       bool write, uint64_t value, unsigned result)
{
    qtest_writeq(s, BASE, offset);
    qtest_writeq(s, BASE + 8, size);
    qtest_writeq(s, BASE + 16, value);
    qtest_writeq(s, BASE + 24, write);
    g_assert_cmpuint(qtest_readq(s, BASE + 32), ==, result);
    return qtest_readq(s, BASE + 16);
}

static void byte(QTestState *s, unsigned position, bool high, uint8_t value,
                 bool completed, unsigned result)
{
    qtest_writeq(s, BASE + 40, value | ((uint64_t)high << 8) |
                  ((uint64_t)completed << 9) | ((uint64_t)position << 16));
    g_assert_cmpuint(qtest_readq(s, BASE + 48), ==, result);
}

static void latch(QTestState *s, uint16_t expected)
{
    g_assert_cmphex(remote(s, LATCH_OFFSET, 2, false, 0, 0), ==, expected);
}

static void unknown(QTestState *s)
{
    remote(s, LATCH_OFFSET, 2, false, 0, TX_ERROR);
}

static void test_controller_operations(void)
{
    QTestState *s = start(ID, true, 0xabcd, "");
    const uint64_t invalid[] = {
        UINT64_C(1) << 32 | 0x008c1301,
        0x018c1301, 0x008e1301, 0x008c1401,
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(invalid); i++) {
        qtest_writeq(s, BASE + 56, invalid[i]);
        g_assert_cmpuint(qtest_readq(s, BASE + 48), ==, TX_ERROR);
        latch(s, 0xabcd);
    }
    qtest_writeq(s, BASE + 56, 0x008c1302);
    g_assert_cmpuint(qtest_readq(s, BASE + 48), ==, 0);
    latch(s, 0x02cd);
    qtest_writeq(s, BASE + 56, 0x008c1241);
    g_assert_cmpuint(qtest_readq(s, BASE + 48), ==, 0);
    latch(s, 0x0241);
    qtest_quit(s);
}

static void test_identity(void)
{
    const uint64_t ids[] = { ID, OTHER_ID };

    for (unsigned i = 0; i < G_N_ELEMENTS(ids); i++) {
        QTestState *s = start(ids[i], false, 0, "");
        uint64_t value = remote(s, ID_OFFSET, 8, false, 0, 0);

        g_assert_cmphex(value, ==, ids[i]);
        g_assert_cmphex(value & 0xf000, ==, 0xc000);
        qtest_quit(s);
    }
}

static void test_unknown(void)
{
    QTestState *s = start(ID, false, 0, "");

    unknown(s);
    byte(s, 0, true, 0, false, TX_ERROR);
    unknown(s);
    byte(s, 0, true, 0, true, 0);
    unknown(s);
    byte(s, 0, false, 0, true, 0);
    latch(s, 0);
    qtest_qmp_assert_success(s, "{'execute': 'system_reset'}");
    unknown(s);
    byte(s, 0, false, 0xff, true, 0);
    unknown(s);
    byte(s, 0, true, 0xff, true, 0);
    latch(s, (1023 << 6) | 63);
    qtest_quit(s);
}

static void test_initial(void)
{
    QTestState *s = start(ID, true, 0x1234, "");

    latch(s, 0x1234);
    byte(s, 0, true, 0xab, true, 0);
    latch(s, 0xab34);
    byte(s, 0, false, 0xcd, false, TX_ERROR);
    latch(s, 0xab34);
    byte(s, 0, false, 0xcd, true, 0);
    latch(s, 0xabcd);
    byte(s, 0, true, 0xef, false, TX_ERROR);
    latch(s, 0xabcd);
    qtest_qmp_assert_success(s, "{'execute': 'system_reset'}");
    latch(s, 0x1234);
    byte(s, 0, false, 0x56, true, 0);
    latch(s, 0x1256);
    byte(s, 0, true, 0x78, true, 0);
    latch(s, 0x7856);
    qtest_quit(s);
}

static void test_unsupported(void)
{
    QTestState *s = start(ID, true, 0, "");
    const unsigned widths[] = { 1, 2, 4, 8 };

    byte(s, 1, true, 0xff, true, TX_ERROR);
    byte(s, 1, false, 0xff, true, TX_ERROR);
    latch(s, 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(widths); i++) {
        unsigned w = widths[i];

        if (w != 8) {
            remote(s, ID_OFFSET, w, false, 0, TX_ERROR);
        }
        if (w != 2) {
            remote(s, LATCH_OFFSET, w, false, 0, TX_ERROR);
        }
        remote(s, ID_OFFSET, w, true, ~0ULL, TX_ERROR);
        remote(s, LATCH_OFFSET, w, true, ~0ULL, TX_ERROR);
        remote(s, ID_OFFSET + 1, w, false, 0, TX_ERROR);
        remote(s, 0, w, false, 0, TX_ERROR);
    }
    latch(s, 0);
    g_assert_cmphex(remote(s, ID_OFFSET, 8, false, 0, 0), ==, ID);
    qtest_quit(s);
}

static void test_migration_validity(void)
{
    for (unsigned valid = 0; valid < 4; valid++) {
        GError *error = NULL;
        g_autofree char *dir = g_dir_make_tmp("bic-migration-XXXXXX", &error);
        g_autofree char *socket = NULL, *uri = NULL;
        QTestState *from, *to;

        g_assert_no_error(error);
        socket = g_build_filename(dir, "migration.sock", NULL);
        uri = g_strdup_printf("unix:%s", socket);
        from = start(ID, false, 0, "");
        to = start(ID, false, 0, "-incoming defer");
        if (valid & 1) {
            byte(from, 0, true, 0xab, true, 0);
        }
        if (valid & 2) {
            byte(from, 0, false, 0xcd, true, 0);
        }
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration_status(to, "completed", 30);
        if (valid == 3) {
            latch(to, 0xabcd);
        } else {
            unknown(to);
        }
        byte(to, 0, true, 0xef, false, TX_ERROR);
        if (!(valid & 1)) {
            byte(to, 0, true, 0x12, true, 0);
        }
        if (!(valid & 2)) {
            byte(to, 0, false, 0x34, true, 0);
        }
        latch(to, ((valid & 1) ? 0xab00 : 0x1200) |
                  ((valid & 2) ? 0xcd : 0x34));
        qtest_qmp_assert_success(to, "{'execute':'system_reset'}");
        unknown(to);
        qtest_quit(to);
        qtest_quit(from);
        unlink(socket);
        g_assert_cmpint(rmdir(dir), ==, 0);
    }
}

static void test_migration_initial(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("bic-initial-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start(ID, true, 0x1234, "");
    to = start(ID, true, 0x1234, "-incoming defer");
    byte(from, 0, true, 0xab, true, 0);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    latch(to, 0xab34);
    byte(to, 0, false, 0xcd, true, 0);
    latch(to, 0xabcd);
    qtest_qmp_assert_success(to, "{'execute':'system_reset'}");
    latch(to, 0x1234);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_migration_mismatch(const void *opaque)
{
    unsigned kind = GPOINTER_TO_UINT(opaque);
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("bic-mismatch-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start(ID, true, 0x1234, "");
    to = start(kind == 0 ? OTHER_ID : ID, kind != 2,
               kind == 1 ? 0x5678 : 0x1234, "-incoming defer");
    qtest_set_expected_status(to, EXIT_FAILURE);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    qtest_wait_qemu(to);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sgi/bic/controller-operations", test_controller_operations);
    qtest_add_func("/bic/identity", test_identity);
    qtest_add_func("/bic/unknown", test_unknown);
    qtest_add_func("/bic/initial", test_initial);
    qtest_add_func("/bic/unsupported", test_unsupported);
    qtest_add_func("/bic/migration/validity", test_migration_validity);
    qtest_add_func("/bic/migration/initial", test_migration_initial);
    qtest_add_data_func("/bic/migration/identity-mismatch",
                        GUINT_TO_POINTER(0), test_migration_mismatch);
    qtest_add_data_func("/bic/migration/initial-word-mismatch",
                        GUINT_TO_POINTER(1), test_migration_mismatch);
    qtest_add_data_func("/bic/migration/initial-presence-mismatch",
                        GUINT_TO_POINTER(2), test_migration_mismatch);
    return g_test_run();
}
