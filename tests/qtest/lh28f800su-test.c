/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Sharp LH28F800SU x16 commands without an IP30 board adapter. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-migrate.h"

#define FLASH UINT64_C(0x50000000)
#define BLOCK (64 * 1024)
#define BSR(n) ((n) * BLOCK + 2)
#define GSR(n) ((n) * BLOCK + 4)
#define PROGRAM_NS 8000
#define ERASE_NS 700000000
#define PIN_VPP 0
#define PIN_WP 1
#define PIN_RP 2
#define CHIP_PATH "/machine/peripheral/flash-fixture/chip"

static QTestState *start(const char *extra)
{
    QTestState *qts = qtest_initf(
        MEMTX_TEST_DEVICE
        "-M malta -device lh28f800su-test-device,id=flash-fixture %s", extra);

    qtest_irq_intercept_out_named(qts, CHIP_PATH, "ready-busy");
    qtest_clock_step(qts, 400);
    return qts;
}

static void pin(QTestState *qts, int n, int level)
{
    qtest_set_irq_in(qts, CHIP_PATH, "pins", n, level);
}

static void command(QTestState *qts, uint64_t offset, uint16_t value)
{
    qtest_writew(qts, FLASH + offset, value);
}

static uint16_t word(QTestState *qts, uint64_t offset)
{
    return qtest_readw(qts, FLASH + offset);
}

static uint8_t status(QTestState *qts)
{
    command(qts, 0, 0x70);
    return word(qts, 0);
}

static uint8_t extended_status(QTestState *qts, uint64_t offset)
{
    command(qts, BLOCK + 0x80, 0x71);
    return word(qts, offset);
}

static void clear_status(QTestState *qts)
{
    command(qts, 0, 0x50);
    g_assert_cmphex(status(qts), ==, 0x80);
}

static void program(QTestState *qts, uint64_t offset, uint16_t value)
{
    command(qts, offset, 0x40);
    command(qts, offset, value);
}

static void array_mode(QTestState *qts)
{
    command(qts, 0, 0xff);
}

static void test_compatible(void)
{
    QTestState *qts = start("");
    const uint64_t target = BLOCK + 0x20;
    const uint64_t neighbor = 2 * BLOCK + 0x20;

    g_assert_cmphex(word(qts, target), ==, 0xffff);
    command(qts, 0, 0x90);
    g_assert_cmphex(word(qts, 0), ==, 0x00b0);
    g_assert_cmphex(word(qts, 2), ==, 0x66a8);
    array_mode(qts);

    /* WP high permits a stored lock bit to be bypassed. */
    program(qts, target, 0x1234);
    g_assert_cmphex(status(qts), ==, 0x00);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, PROGRAM_NS - 1);
    g_assert_cmphex(status(qts), ==, 0x00);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(status(qts), ==, 0x80);
    g_assert_true(qtest_get_irq(qts, 0));
    array_mode(qts);
    g_assert_cmphex(word(qts, target), ==, 0x1234);

    program(qts, neighbor, 0xabcd);
    qtest_clock_step(qts, PROGRAM_NS);
    array_mode(qts);
    g_assert_cmphex(word(qts, neighbor), ==, 0xabcd);

    program(qts, target, 0xffff);
    g_assert_cmphex(status(qts), ==, 0x90);
    clear_status(qts);
    array_mode(qts);
    g_assert_cmphex(word(qts, target), ==, 0x1234);

    /* SMT96107 p. 10 selects the erase block on the second cycle. */
    command(qts, 0x20, 0x20);
    command(qts, target, 0xd0);
    g_assert_cmphex(status(qts), ==, 0x00);
    qtest_clock_step(qts, 100);
    command(qts, target, 0xb0);
    g_assert_cmphex(status(qts), ==, 0xc0);
    g_assert_true(qtest_get_irq(qts, 0));
    array_mode(qts);
    g_assert_cmphex(word(qts, target), ==, 0x1234);
    qtest_clock_step(qts, ERASE_NS);
    g_assert_cmphex(word(qts, target), ==, 0x1234);
    command(qts, 0, 0xd0);
    g_assert_cmphex(status(qts), ==, 0x00);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, ERASE_NS);
    g_assert_cmphex(status(qts), ==, 0x80);
    g_assert_true(qtest_get_irq(qts, 0));
    array_mode(qts);
    g_assert_cmphex(word(qts, target), ==, 0xffff);
    g_assert_cmphex(word(qts, neighbor), ==, 0xabcd);

    program(qts, target, 0x7070);
    qtest_clock_step(qts, PROGRAM_NS);
    array_mode(qts);
    g_assert_cmphex(word(qts, target), ==, 0x7070);
    qtest_system_reset(qts);
    g_assert_cmphex(word(qts, target), ==, 0x7070);
    qtest_quit(qts);
}

static void test_access_and_commands(void)
{
    QTestState *qts = start("");
    uint8_t byte;
    uint16_t half;
    uint32_t wide;
    uint64_t quad;
    const uint16_t unsupported[] = { 0x77, 0xa7, 0xee };

    g_assert_cmpuint(memtx_readb(qts, FLASH, &byte), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeb(qts, FLASH, 0xff), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readw(qts, FLASH + 1, &half), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writew(qts, FLASH + 1, 0xff), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readl(qts, FLASH, &wide), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writel(qts, FLASH, 0xffffffff), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readq(qts, FLASH, &quad), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(qts, FLASH, UINT64_MAX), !=, MEMTX_OK);

    for (unsigned i = 0; i < ARRAY_SIZE(unsupported); i++) {
        g_assert_cmpuint(memtx_writew(qts, FLASH, unsupported[i]), !=,
                         MEMTX_OK);
    }
    g_assert_cmphex(word(qts, 0), ==, 0xffff);
    qtest_quit(qts);
}

static void test_extended_addresses_and_upload(void)
{
    QTestState *qts = start("");
    uint16_t half;

    g_assert_cmphex(extended_status(qts, BSR(0)), ==, 0x80);
    g_assert_cmphex(word(qts, GSR(0)), ==, 0x86);
    g_assert_cmphex(word(qts, BSR(1)), ==, 0x80);
    g_assert_cmphex(word(qts, GSR(15)), ==, 0x86);
    g_assert_cmphex(word(qts, BSR(15)), ==, 0x80);
    g_assert_cmpuint(memtx_readw(qts, FLASH, &half), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readw(qts, FLASH + 6, &half), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readw(qts, FLASH + BSR(15) + 4, &half),
                     !=, MEMTX_OK);

    command(qts, BLOCK + 0x80, 0x97);
    g_assert_cmphex(word(qts, BSR(0)), ==, 0x80);
    command(qts, 2 * BLOCK + 0x80, 0xd0);
    g_assert_cmphex(word(qts, BSR(0)), ==, 0xc0);
    g_assert_cmphex(word(qts, BSR(1)), ==, 0x80);
    g_assert_cmphex(word(qts, GSR(15)), ==, 0x86);

    pin(qts, PIN_RP, 0);
    pin(qts, PIN_RP, 1);
    qtest_clock_step(qts, 400);
    g_assert_cmphex(extended_status(qts, BSR(0)), ==, 0x80);
    command(qts, 0, 0x97);
    g_assert_cmpuint(memtx_writew(qts, FLASH, 0x1212), !=, MEMTX_OK);
    g_assert_cmphex(word(qts, BSR(0)), ==, 0x80);
    g_assert_cmphex(word(qts, GSR(0)), ==, 0x86);
    g_assert_cmpuint(memtx_writew(qts, FLASH, 0xd0d0), !=, MEMTX_OK);
    g_assert_cmphex(word(qts, BSR(0)), ==, 0x80);

    qtest_quit(qts);
}

static void test_extended_operation_status(void)
{
    QTestState *qts = start("");

    pin(qts, PIN_VPP, 0);
    program(qts, 0x40, 0x1234);
    g_assert_cmphex(status(qts), ==, 0x98);
    g_assert_cmphex(extended_status(qts, GSR(0)), ==, 0xa6);
    g_assert_cmphex(word(qts, BSR(0)), ==, 0xa4);
    clear_status(qts);
    g_assert_cmphex(extended_status(qts, GSR(0)), ==, 0x86);
    g_assert_cmphex(word(qts, BSR(0)), ==, 0x80);

    pin(qts, PIN_VPP, 1);
    pin(qts, PIN_WP, 0);
    program(qts, BLOCK + 0x40, 0x1234);
    g_assert_cmphex(status(qts), ==, 0x90);
    g_assert_cmphex(extended_status(qts, GSR(0)), ==, 0xa6);
    g_assert_cmphex(word(qts, BSR(1)), ==, 0xa0);
    g_assert_cmphex(word(qts, BSR(0)), ==, 0x80);
    clear_status(qts);

    command(qts, 0, 0x97);
    command(qts, 0, 0xd0);
    g_assert_cmphex(extended_status(qts, BSR(0)), ==, 0xc0);
    program(qts, 0x40, 0x1234);
    g_assert_cmphex(status(qts), ==, 0x00);
    g_assert_cmphex(extended_status(qts, GSR(0)), ==, 0x06);
    g_assert_cmphex(word(qts, BSR(0)), ==, 0x40);
    g_assert_cmphex(word(qts, BSR(1)), ==, 0x80);
    g_assert_cmpuint(memtx_writew(qts, FLASH, 0x9797), !=, MEMTX_OK);
    qtest_clock_step(qts, PROGRAM_NS);
    g_assert_cmphex(word(qts, GSR(0)), ==, 0x86);
    g_assert_cmphex(word(qts, BSR(0)), ==, 0xc0);
    array_mode(qts);
    g_assert_cmphex(word(qts, 0x40), ==, 0x1234);

    pin(qts, PIN_RP, 0);
    pin(qts, PIN_RP, 1);
    qtest_clock_step(qts, 400);
    g_assert_cmphex(extended_status(qts, BSR(0)), ==, 0x80);
    program(qts, 0x42, 0x5678);
    qtest_clock_step(qts, PROGRAM_NS);
    array_mode(qts);
    g_assert_cmphex(word(qts, 0x42), ==, 0x5678);
    g_assert_cmphex(word(qts, 0x40), ==, 0x1234);
    program(qts, BLOCK + 0x40, 0x1234);
    g_assert_cmphex(status(qts), ==, 0x90);
    g_assert_cmphex(extended_status(qts, GSR(0)), ==, 0xa6);
    g_assert_cmphex(word(qts, BSR(1)), ==, 0xa0);
    clear_status(qts);
    array_mode(qts);
    g_assert_cmphex(word(qts, BLOCK + 0x40), ==, 0xffff);

    command(qts, 0, 0x20);
    command(qts, 0x40, 0xd0);
    g_assert_cmphex(extended_status(qts, GSR(0)), ==, 0x06);
    g_assert_cmphex(word(qts, BSR(0)), ==, 0x00);
    command(qts, 0, 0xb0);
    g_assert_cmphex(extended_status(qts, GSR(0)), ==, 0xc6);
    g_assert_cmphex(word(qts, BSR(0)), ==, 0x80);
    command(qts, 0, 0xd0);
    g_assert_cmphex(extended_status(qts, GSR(0)), ==, 0x06);
    qtest_clock_step(qts, ERASE_NS);
    g_assert_cmphex(word(qts, GSR(0)), ==, 0x86);
    g_assert_cmphex(word(qts, BSR(0)), ==, 0x80);
    array_mode(qts);
    g_assert_cmphex(word(qts, 0x40), ==, 0xffff);

    qtest_quit(qts);
}

static void test_protection_and_reset(void)
{
    QTestState *qts = start("");
    const uint64_t unlocked = 0x40;
    const uint64_t locked = BLOCK + 0x40;
    const uint64_t interrupted = 0x60;
    uint16_t half;

    pin(qts, PIN_VPP, 0);
    program(qts, unlocked, 0x1234);
    g_assert_cmphex(status(qts), ==, 0x98);
    clear_status(qts);
    array_mode(qts);
    g_assert_cmphex(word(qts, unlocked), ==, 0xffff);

    pin(qts, PIN_VPP, 1);
    pin(qts, PIN_WP, 0);
    program(qts, locked, 0x1234);
    g_assert_cmphex(status(qts), ==, 0x90);
    clear_status(qts);
    array_mode(qts);
    g_assert_cmphex(word(qts, locked), ==, 0xffff);

    /* Supply loss aborts an operation atomically in this model. */
    program(qts, unlocked, 0x1234);
    g_assert_cmphex(status(qts), ==, 0x00);
    qtest_clock_step(qts, 100);
    pin(qts, PIN_VPP, 0);
    g_assert_cmphex(status(qts), ==, 0x98);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, PROGRAM_NS);
    array_mode(qts);
    g_assert_cmphex(word(qts, unlocked), ==, 0xffff);
    clear_status(qts);
    pin(qts, PIN_VPP, 1);

    program(qts, unlocked, 0x7070);
    qtest_clock_step(qts, PROGRAM_NS);
    command(qts, unlocked, 0x20);
    command(qts, unlocked, 0xd0);
    qtest_clock_step(qts, 100);
    pin(qts, PIN_VPP, 0);
    g_assert_cmphex(status(qts), ==, 0xa8);
    qtest_clock_step(qts, ERASE_NS);
    array_mode(qts);
    g_assert_cmphex(word(qts, unlocked), ==, 0x7070);
    clear_status(qts);
    pin(qts, PIN_VPP, 1);

    program(qts, interrupted, 0x1234);
    g_assert_cmphex(status(qts), ==, 0x00);
    pin(qts, PIN_RP, 0);
    g_assert_cmpuint(memtx_readw(qts, FLASH + interrupted, &half), !=,
                     MEMTX_OK);
    g_assert_cmpuint(memtx_writew(qts, FLASH, 0xff), !=, MEMTX_OK);
    g_assert_true(qtest_get_irq(qts, 0));
    pin(qts, PIN_RP, 1);
    qtest_clock_step(qts, 399);
    g_assert_cmpuint(memtx_readw(qts, FLASH + interrupted, &half), !=,
                     MEMTX_OK);
    g_assert_cmpuint(memtx_writew(qts, FLASH, 0xff), !=, MEMTX_OK);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(word(qts, interrupted), ==, 0xffff);
    qtest_clock_step(qts, PROGRAM_NS);
    g_assert_cmphex(word(qts, interrupted), ==, 0xffff);

    program(qts, interrupted, 0x7070);
    qtest_clock_step(qts, PROGRAM_NS);
    array_mode(qts);
    g_assert_cmphex(word(qts, interrupted), ==, 0x7070);
    qtest_quit(qts);
}

static void test_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("lh28f800su-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");
    const uint64_t target = BLOCK + 0x60;

    program(from, target, 0x1234);
    qtest_clock_step(from, PROGRAM_NS);
    array_mode(from);
    g_assert_cmphex(word(from, target), ==, 0x1234);
    command(from, 0, 0x90);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(word(to, 0), ==, 0x00b0);
    array_mode(to);
    g_assert_cmphex(word(to, target), ==, 0x1234);
    pin(to, PIN_WP, 0);
    program(to, target, 0x0000);
    g_assert_cmphex(status(to), ==, 0x90);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_migration_program(gconstpointer opaque)
{
    int phase = GPOINTER_TO_INT(opaque);
    g_autofree char *dir = g_dir_make_tmp("lh28f800su-program-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");
    const uint64_t target = BLOCK + 0xa0;

    command(from, target, 0x40);
    if (phase != 1) {
        command(from, target, 0x1234);
    }
    if (phase == 0) {
        qtest_clock_step(from, PROGRAM_NS);
        array_mode(from);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    if (phase == 0) {
        /* The destination must read array data before a mode command. */
        g_assert_cmphex(word(to, target), ==, 0x1234);
    } else {
        if (phase == 1) {
            command(to, target, 0x1234);
        }
        g_assert_cmphex(status(to), ==, 0x00);
        g_assert_false(qtest_get_irq(to, 0));
        qtest_clock_step(to, PROGRAM_NS);
        g_assert_cmphex(status(to), ==, 0x80);
        g_assert_true(qtest_get_irq(to, 0));
        array_mode(to);
        g_assert_cmphex(word(to, target), ==, 0x1234);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_migration_erase(gconstpointer opaque)
{
    bool suspend = GPOINTER_TO_INT(opaque);
    g_autofree char *dir = g_dir_make_tmp("lh28f800su-erase-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");
    const uint64_t target = BLOCK + 0x80;

    program(from, target, 0x1234);
    qtest_clock_step(from, PROGRAM_NS);
    command(from, 0, 0x20);
    command(from, target, 0xd0);
    g_assert_cmphex(status(from), ==, 0x00);
    if (suspend) {
        qtest_clock_step(from, 100);
        command(from, 0, 0xb0);
        g_assert_cmphex(status(from), ==, 0xc0);
    }

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(status(to), ==, suspend ? 0xc0 : 0x00);
    if (suspend) {
        qtest_clock_step(to, ERASE_NS);
        g_assert_cmphex(status(to), ==, 0xc0);
        command(to, 0, 0xd0);
        g_assert_cmphex(status(to), ==, 0x00);
    }
    qtest_clock_step(to, ERASE_NS);
    g_assert_cmphex(status(to), ==, 0x80);
    array_mode(to);
    g_assert_cmphex(word(to, target), ==, 0xffff);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_migration_extended(gconstpointer opaque)
{
    bool pending = GPOINTER_TO_INT(opaque);
    g_autofree char *dir = g_dir_make_tmp("lh28f800su-status-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");

    g_assert_cmphex(extended_status(from, BSR(0)), ==, 0x80);
    command(from, 0, 0x97);
    if (!pending) {
        command(from, 0, 0xd0);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(word(to, BSR(0)), ==, pending ? 0x80 : 0xc0);
    g_assert_cmphex(word(to, GSR(15)), ==, 0x86);
    if (pending) {
        command(to, BLOCK + 0x40, 0xd0);
        g_assert_cmphex(word(to, BSR(0)), ==, 0xc0);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/lh28f800su/compatible-x16", test_compatible);
    qtest_add_func("/lh28f800su/access-commands", test_access_and_commands);
    qtest_add_func("/lh28f800su/extended-addresses-upload",
                   test_extended_addresses_and_upload);
    qtest_add_func("/lh28f800su/extended-operation-status",
                   test_extended_operation_status);
    qtest_add_func("/lh28f800su/protection-reset", test_protection_and_reset);
    qtest_add_func("/lh28f800su/migration", test_migration);
    g_test_add_data_func("/lh28f800su/migration-array",
                         GINT_TO_POINTER(0), test_migration_program);
    g_test_add_data_func("/lh28f800su/migration-setup",
                         GINT_TO_POINTER(1), test_migration_program);
    g_test_add_data_func("/lh28f800su/migration-program-busy",
                         GINT_TO_POINTER(2), test_migration_program);
    g_test_add_data_func("/lh28f800su/migration-busy",
                         GINT_TO_POINTER(false), test_migration_erase);
    g_test_add_data_func("/lh28f800su/migration-suspended",
                         GINT_TO_POINTER(true), test_migration_erase);
    g_test_add_data_func("/lh28f800su/migration-extended-uploaded",
                         GINT_TO_POINTER(false), test_migration_extended);
    g_test_add_data_func("/lh28f800su/migration-extended-setup",
                         GINT_TO_POINTER(true), test_migration_extended);
    return g_test_run();
}
