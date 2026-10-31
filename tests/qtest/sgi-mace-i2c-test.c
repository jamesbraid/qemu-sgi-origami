/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-migrate.h"

#define BASE 0x1f000000ULL
#define I2C (BASE + 0x330000)
#define CONFIG I2C
#define CONTROL (I2C + 0x10)
#define DATA (I2C + 0x18)
#define EP "/machine/peripheral/endpoint"

static QTestState *start_type(const char *extra, unsigned mode,
                               const char *type)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 -nodefaults "
        "-device sgi-mace,id=mace,i2c-untimed-test-profile=on "
        "-device %s,id=endpoint,bus=i2c,address=0x44,mode=%u "
        MEMTX_TEST_DEVICE " %s", type, mode, extra);

    qtest_sysbus_mmio_map(s, "/machine/peripheral/mace", 0, BASE);
    qtest_irq_intercept_out_named(s, "/machine/peripheral/mace", "crime");
    return s;
}

static QTestState *start(const char *extra, unsigned mode)
{
    return start_type(extra, mode, "sgi-mace-i2c-test-endpoint");
}

static uint64_t observe(QTestState *s, const char *name)
{
    g_autoptr(QDict) reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        EP, name);

    return qdict_get_int(reply, "return");
}

static void address(QTestState *s, bool recv)
{
    qtest_writeq(s, CONTROL, 5);
    qtest_writeq(s, DATA, 0x88 | recv);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 5);
}

static void test_slots(void)
{
    QTestState *s = start("", 0);
    uint8_t bytes[8];
    uint64_t value;

    g_assert_cmphex(qtest_readq(s, CONFIG), ==, 0x30);
    g_assert_cmphex(qtest_readq(s, CONFIG + 8), ==, 0x30);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0);
    qtest_writeq(s, CONFIG + 8, 0xffff000000000002ULL);
    qtest_memread(s, CONFIG, bytes, sizeof(bytes));
    g_assert_cmphex(ldq_be_p(bytes), ==, 0x32);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0);
    qtest_writeq(s, CONTROL, 0xffff0000000000ffULL);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 6);
    for (unsigned slot = 0; slot < 4; slot++) {
        for (unsigned size = 1; size <= 4; size *= 2) {
            g_assert_cmpint(memtx_store(s, I2C + slot * 8, 0x89, size),
                            !=, MEMTX_OK);
            g_assert_cmpint(memtx_load(s, I2C + slot * 8, &value, size),
                            !=, MEMTX_OK);
        }
        for (unsigned offset = 1; offset < 8; offset++) {
            g_assert_cmpint(memtx_store(s, I2C + slot * 8 + offset, 0, 8),
                            !=, MEMTX_OK);
            g_assert_cmpint(memtx_load(s, I2C + slot * 8 + offset, &value, 8),
                            !=, MEMTX_OK);
        }
    }
    g_assert_cmpint(memtx_store(s, I2C + 0x20, 0, 8), !=, MEMTX_OK);
    g_assert_cmpint(memtx_load(s, I2C + 0x100, &value, 8), !=, MEMTX_OK);
    g_assert_cmpint(memtx_load(s, I2C + 0xfff8, &value, 8), !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, I2C + 0xfff8, 0x89, 8), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CONFIG), ==, 0x32);
    g_assert_cmpuint(observe(s, "starts"), ==, 0);
    for (unsigned i = 0; i < 16; i++) {
        g_assert_false(qtest_get_irq(s, i));
    }
    qtest_quit(s);
}

static void test_receive(void)
{
    for (unsigned polls = 0; polls <= 17; polls += polls ? 16 : 1) {
        QTestState *s = start("", 0);
        int64_t time = qtest_clock_set(s, 0);
        uint64_t value;

        address(s, true);
        qtest_writeq(s, CONTROL, 7);
        g_assert_cmpuint(observe(s, "cursor"), ==, 0);
        for (unsigned i = 0; i < polls; i++) {
            g_assert_cmphex(qtest_readq(s, CONTROL), ==, 7);
        }
        g_assert_cmpint(memtx_load(s, CONFIG, &value, 8), !=, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, DATA), ==, 0x96);
        g_assert_cmpuint(observe(s, "cursor"), ==, 1);
        g_assert_cmphex(qtest_readq(s, DATA), ==, 0x3d);
        g_assert_cmpuint(observe(s, "cursor"), ==, 2);
        qtest_writeq(s, CONTROL, 3);
        g_assert_cmphex(qtest_readq(s, CONTROL), ==, 2);
        g_assert_cmphex(observe(s, "events"), ==, 0x23345);
        g_assert_cmpuint(observe(s, "nacks"), ==, 1);
        g_assert_cmpuint(observe(s, "stops"), ==, 1);
        qtest_writeq(s, CONTROL, 0);
        g_assert_cmpuint(observe(s, "stops"), ==, 1);
        g_assert_cmpint(qtest_clock_set(s, 0), ==, time);
        qtest_quit(s);
    }
}

static void test_final_receive(void)
{
    QTestState *s = start("", 0);

    address(s, true);
    qtest_writeq(s, CONTROL, 3);
    g_assert_cmpuint(observe(s, "cursor"), ==, 0);
    g_assert_cmphex(qtest_readq(s, DATA), ==, 0x96);
    g_assert_cmphex(observe(s, "events"), ==, 0x2345);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 2);
    qtest_quit(s);
}

static void test_transmit(void)
{
    QTestState *s = start("", 0);

    address(s, false);
    qtest_writeq(s, DATA, 0x12);
    qtest_writeq(s, CONTROL, 5);
    qtest_writeq(s, DATA, 0x89);
    qtest_writeq(s, CONTROL, 1);
    qtest_writeq(s, DATA, 0x3d);
    g_assert_cmpuint(observe(s, "starts"), ==, 1);
    g_assert_cmpuint(observe(s, "writes"), ==, 3);
    g_assert_cmphex(observe(s, "payload"), ==, 0x12893d);
    g_assert_cmphex(observe(s, "events"), ==, 0x17775);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0);
    address(s, false);
    qtest_writeq(s, CONTROL, 4);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 4);
    g_assert_cmpuint(observe(s, "stops"), ==, 2);
    address(s, false);
    qtest_writeq(s, CONTROL, 0);
    g_assert_cmpuint(observe(s, "stops"), ==, 3);
    qtest_quit(s);
}

static void test_errors(void)
{
    QTestState *s = start("", 0);
    uint64_t value;

    address(s, false);
    qtest_writeq(s, CONTROL, 7);
    g_assert_cmpint(memtx_load(s, DATA, &value, 8), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x86);
    g_assert_cmpuint(observe(s, "cursor"), ==, 0);
    g_assert_cmphex(observe(s, "events"), ==, 0x15);
    qtest_writeq(s, CONTROL, 0);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x80);
    g_assert_cmpint(memtx_store(s, DATA, 0x88, 8), !=, MEMTX_OK);
    qtest_writeq(s, CONFIG, 1);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0);
    qtest_writeq(s, CONTROL, 3);
    g_assert_cmpint(memtx_load(s, DATA, &value, 8), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x82);
    qtest_writeq(s, CONFIG, 0);
    qtest_writeq(s, CONFIG, 1);
    g_assert_cmpint(memtx_store(s, CONFIG, 0x0e, 8), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CONFIG), ==, 0x31);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x80);
    qtest_quit(s);
}

static void test_nack(void)
{
    QTestState *s = start("", 4);
    uint64_t value;

    address(s, false);
    qtest_writeq(s, DATA, 0x96);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x25);
    qtest_writeq(s, DATA, 0x3d);
    g_assert_cmpuint(observe(s, "writes"), ==, 2);
    qtest_writeq(s, CONTROL, 0);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x20);
    qtest_writeq(s, CONTROL, 5);
    qtest_writeq(s, DATA, 0xa1);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x25);
    qtest_writeq(s, CONTROL, 7);
    g_assert_cmpint(memtx_load(s, DATA, &value, 8), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0xa6);
    g_assert_cmpuint(observe(s, "cursor"), ==, 0);
    qtest_quit(s);
}

static void test_reset_edges(void)
{
    QTestState *s = start("", 0);

    qtest_writeq(s, CONFIG, 3);
    g_assert_cmphex(qtest_readq(s, CONFIG), ==, 0x33);
    address(s, true);
    qtest_writeq(s, CONTROL, 7);
    g_assert_cmphex(qtest_readq(s, DATA), ==, 0x96);
    qtest_writeq(s, CONTROL, 5);
    qtest_writeq(s, CONFIG, 3);
    g_assert_cmpuint(observe(s, "stops"), ==, 0);
    qtest_writeq(s, CONFIG + 8, 2);
    g_assert_cmpuint(observe(s, "stops"), ==, 0);
    qtest_writeq(s, CONFIG + 8, 3);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0);
    g_assert_cmphex(qtest_readq(s, CONFIG), ==, 0x33);
    g_assert_cmphex(observe(s, "events"), ==, 0x2345);
    address(s, false);
    qtest_writeq(s, DATA, 0x3d);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0);
    g_assert_cmphex(qtest_readq(s, CONFIG), ==, 0x30);
    g_assert_cmpuint(observe(s, "payload"), ==, 0x3d);
    g_assert_cmpuint(observe(s, "stops"), ==, 2);
    qtest_quit(s);
}

static void test_force_idle_receive(void)
{
    QTestState *s = start("", 0);

    address(s, true);
    qtest_writeq(s, CONTROL, 7);
    g_assert_cmphex(qtest_readq(s, DATA), ==, 0x96);
    qtest_writeq(s, CONTROL, 5);
    qtest_writeq(s, CONTROL, 5);
    g_assert_cmpuint(observe(s, "stops"), ==, 0);
    qtest_writeq(s, CONTROL, 4);
    g_assert_cmphex(observe(s, "events"), ==, 0x2345);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 4);
    qtest_quit(s);
}

static void test_negative_api(void)
{
    QTestState *s = start("", 6);

    address(s, false);
    g_assert_cmpint(memtx_store(s, DATA, 0x3d, 8), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x84);
    g_assert_cmpuint(observe(s, "writes"), ==, 1);
    g_assert_cmpuint(observe(s, "stops"), ==, 1);
    qtest_quit(s);
}

static void test_empty_bus(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -nodefaults "
        "-device sgi-mace,id=mace,i2c-untimed-test-profile=on "
        MEMTX_TEST_DEVICE);
    uint64_t value;

    qtest_sysbus_mmio_map(s, "/machine/peripheral/mace", 0, BASE);
    qtest_writeq(s, CONTROL, 5);
    qtest_writeq(s, DATA, 0x89);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x25);
    qtest_writeq(s, CONTROL, 7);
    g_assert_cmpint(memtx_load(s, DATA, &value, 8), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0xa6);
    qtest_quit(s);
}

static void test_migration(void)
{
    for (unsigned state = 0; state < 3; state++) {
        g_autofree char *dir = g_dir_make_tmp("mace-i2c-migration-XXXXXX",
                                               NULL);
        g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
        g_autofree char *uri = g_strdup_printf("unix:%s", socket);
        QTestState *from = start("", 0);
        QTestState *to = start("-incoming defer", 0);

        qtest_writeq(from, CONFIG, 3);
        if (state) {
            address(from, state == 2);
            if (state == 2) {
                qtest_writeq(from, CONTROL, 7);
                g_assert_cmphex(qtest_readq(from, DATA), ==, 0x96);
                /* CONTROL changes cannot lose a pending receive ACK. */
                qtest_writeq(from, CONTROL, 5);
            } else {
                qtest_writeq(from, DATA, 0x12);
            }
        }
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        g_assert_cmpuint(observe(to, "starts"), ==, state != 0);
        g_assert_cmpuint(observe(to, "stops"), ==, 0);
        g_assert_cmpuint(observe(to, "nacks"), ==, 0);
        if (state == 2) {
            g_assert_cmpuint(observe(to, "cursor"), ==, 1);
            qtest_writeq(to, CONTROL, 7);
            g_assert_cmphex(qtest_readq(to, DATA), ==, 0x3d);
            qtest_writeq(to, CONTROL, 3);
            g_assert_cmphex(observe(to, "events"), ==, 0x23345);
        } else if (state == 1) {
            qtest_writeq(to, CONTROL, 1);
            qtest_writeq(to, DATA, 0x3d);
            g_assert_cmphex(observe(to, "payload"), ==, 0x123d);
            g_assert_cmphex(observe(to, "events"), ==, 0x1775);
        } else {
            g_assert_cmphex(qtest_readq(to, CONFIG), ==, 0x33);
            address(to, true);
            qtest_writeq(to, CONTROL, 3);
            g_assert_cmphex(qtest_readq(to, DATA), ==, 0x96);
        }
        qtest_quit(to);
        qtest_quit(from);
        g_unlink(socket);
        g_rmdir(dir);
    }
}

static void test_unrealize(void)
{
    QTestState *s = start("", 0);

    address(s, true);
    qtest_writeq(s, CONTROL, 7);
    g_assert_cmphex(qtest_readq(s, DATA), ==, 0x96);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/mace',"
        "'property':'realized','value':false}}");
    g_assert_cmphex(observe(s, "events"), ==, 0x2345);
    g_assert_cmpuint(observe(s, "stops"), ==, 1);
    qtest_quit(s);
}

static void test_callback_reset(void)
{
    for (unsigned mode = 7; mode <= 8; mode++) {
        QTestState *s = start("", mode);

        qtest_writeq(s, CONFIG, 3);
        address(s, mode == 7);
        if (mode == 7) {
            qtest_writeq(s, CONTROL, 7);
            g_assert_cmphex(qtest_readq(s, DATA), ==, 0x96);
            g_assert_cmphex(observe(s, "events"), ==, 0x2345);
            g_assert_cmphex(observe(s, "busy-seen"), ==, 0x10);
        } else {
            qtest_writeq(s, DATA, 0x3d);
            g_assert_cmphex(observe(s, "events"), ==, 0x175);
            g_assert_cmphex(observe(s, "payload"), ==, 0x3d);
        }
        g_assert_cmpuint(observe(s, "recursive-events"), ==, 0);
        g_assert_cmpuint(observe(s, "stops"), ==, 1);
        g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0);
        g_assert_cmphex(qtest_readq(s, CONFIG), ==, 0x30);
        qtest_quit(s);
    }
}

static void test_reject_endpoint(void)
{
    for (unsigned mode = 1; mode <= 3; mode++) {
        g_autofree char *args = g_strdup_printf(
            "-M none -nodefaults -device sgi-mace,id=mace,"
            "i2c-untimed-test-profile=on -device sgi-mace-i2c-test-endpoint,"
            "bus=i2c,address=0x44,mode=%u", mode);
        QTestState *s = qtest_init_ext("QTEST_QEMU_BINARY", args, NULL, false);

        qtest_set_expected_status(s, 1);
        qtest_wait_qemu(s);
        qtest_quit(s);
    }
}

static void test_runtime_admission(void)
{
    QTestState *s = start("", 0);
    uint64_t value;
    g_autoptr(QDict) reply = NULL;

    address(s, true);
    qtest_writeq(s, CONTROL, 7);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'profile-available','value':false}}", EP);
    g_assert_cmpint(memtx_load(s, DATA, &value, 8), !=, MEMTX_OK);
    g_assert_cmpuint(observe(s, "cursor"), ==, 0);
    g_assert_cmpuint(observe(s, "stops"), ==, 0);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x87);
    reply = qtest_qmp(s,
        "{'execute':'device_add','arguments':{'driver':"
        "'sgi-mace-i2c-test-endpoint','id':'late','bus':'i2c','address':69}}");
    g_assert_true(qdict_haskey(reply, "error"));
    qtest_writeq(s, CONTROL, 0);
    g_assert_cmpuint(observe(s, "stops"), ==, 1);
    g_assert_cmpint(memtx_load(s, CONFIG, &value, 8), !=, MEMTX_OK);
    qtest_quit(s);
}

static void test_incompatible_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("mace-i2c-reject-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start("", 0);
    QTestState *to = start("-incoming defer", 4);

    address(from, true);
    qtest_writeq(from, CONTROL, 7);
    g_assert_cmphex(qtest_readq(from, DATA), ==, 0x96);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    qtest_wait_qemu(to);
    g_assert_cmphex(qtest_readq(from, CONTROL), ==, 7);
    g_assert_cmpuint(observe(from, "cursor"), ==, 1);
    g_assert_cmpuint(observe(from, "stops"), ==, 0);
    g_assert_cmpuint(observe(from, "nacks"), ==, 0);
    qtest_writeq(from, CONTROL, 3);
    g_assert_cmphex(observe(from, "events"), ==, 0x2345);
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(dir);
}

static void test_unqualified_source_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("mace-i2c-source-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start("", 0);
    QTestState *to = start("-incoming defer", 0);

    address(from, true);
    qtest_writeq(from, CONTROL, 7);
    g_assert_cmphex(qtest_readq(from, DATA), ==, 0x96);
    qtest_qmp_assert_success(from,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'profile-available','value':false}}", EP);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(from, "failed");
    g_assert_cmphex(qtest_readq(from, CONTROL), ==, 7);
    g_assert_cmpuint(observe(from, "cursor"), ==, 1);
    g_assert_cmpuint(observe(from, "stops"), ==, 0);
    g_assert_cmpuint(observe(from, "nacks"), ==, 0);
    qtest_qmp_assert_success(from,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'profile-available','value':true}}", EP);
    g_assert_cmphex(qtest_readq(from, DATA), ==, 0x3d);
    qtest_writeq(from, CONTROL, 3);
    g_assert_cmphex(observe(from, "events"), ==, 0x23345);
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(dir);
}

static void test_default_disabled(void)
{
    QTestState *s = qtest_init("-M none -nodefaults -device sgi-mace,id=mace");
    g_autoptr(QDict) reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':"
        "'/machine/peripheral/mace/i2c-controller','property':'realized'}}");

    g_assert_true(qdict_haskey(reply, "error"));
    qtest_quit(s);
}

static void test_pending_ack_transmit_control(void)
{
    QTestState *s = start("", 0);
    uint64_t value;

    address(s, true);
    qtest_writeq(s, CONTROL, 7);
    g_assert_cmphex(qtest_readq(s, DATA), ==, 0x96);
    qtest_writeq(s, CONTROL, 1);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 1);
    g_assert_cmpuint(observe(s, "stops"), ==, 0);
    g_assert_cmpuint(observe(s, "nacks"), ==, 0);
    g_assert_cmpint(memtx_store(s, CONFIG, 0x0c, 8), !=, MEMTX_OK);
    g_assert_cmpuint(observe(s, "stops"), ==, 0);
    g_assert_cmpint(memtx_load(s, DATA, &value, 8), !=, MEMTX_OK);
    g_assert_cmpuint(observe(s, "cursor"), ==, 1);
    qtest_writeq(s, CONTROL, 4);
    g_assert_cmphex(observe(s, "events"), ==, 0x2345);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x84);
    qtest_quit(s);
}

static void test_address_nack(void)
{
    QTestState *s = start("", 5);

    qtest_writeq(s, CONTROL, 5);
    qtest_writeq(s, DATA, 0x88);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x25);
    g_assert_cmpuint(observe(s, "starts"), ==, 1);
    g_assert_cmpuint(observe(s, "writes"), ==, 0);
    g_assert_cmpint(memtx_store(s, DATA, 0x3d, 8), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0xa4);
    g_assert_cmpuint(observe(s, "writes"), ==, 0);
    g_assert_cmpuint(observe(s, "stops"), ==, 0);
    qtest_quit(s);
}

static void test_write_only_receive(void)
{
    QTestState *s = start_type("", 0, "sgi-mace-i2c-test-write-only");
    uint64_t value;

    address(s, true);
    qtest_writeq(s, CONTROL, 7);
    g_assert_cmpint(memtx_load(s, DATA, &value, 8), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x86);
    g_assert_cmpuint(observe(s, "cursor"), ==, 0);
    g_assert_cmphex(observe(s, "events"), ==, 0x25);
    qtest_quit(s);
}

static void test_read_only_transmit(void)
{
    QTestState *s = start_type("", 0, "sgi-mace-i2c-test-read-only");

    address(s, false);
    g_assert_cmpint(memtx_store(s, DATA, 0x96, 8), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x84);
    g_assert_cmpuint(observe(s, "writes"), ==, 0);
    g_assert_cmphex(observe(s, "events"), ==, 0x15);
    qtest_quit(s);
}

static bool observe_bool(QTestState *s, const char *name)
{
    g_autoptr(QDict) reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        EP, name);

    return qdict_get_bool(reply, "return");
}

static void termination_reset(unsigned mode)
{
    QTestState *s = start("", mode);

    qtest_writeq(s, CONFIG, 2);
    address(s, true);
    qtest_writeq(s, CONTROL, 7);
    g_assert_cmphex(qtest_readq(s, DATA), ==, 0x96);
    qtest_writeq(s, CONFIG, 3);
    g_assert_cmphex(qtest_readq(s, CONFIG), ==, 0x30);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0);
    g_assert_cmphex(observe(s, "events"), ==, 0x2345);
    g_assert_cmpuint(observe(s, "nacks"), ==, 1);
    g_assert_cmpuint(observe(s, "stops"), ==, 1);
    g_assert_cmpuint(observe(s, "recursive-events"), ==, 0);
    g_assert_cmpuint(observe(s, "cursor"), ==, 1);
    qtest_quit(s);
}

static void test_reset_during_nack(void)
{
    termination_reset(9);
}

static void test_reset_during_stop(void)
{
    termination_reset(10);
}

static void test_child_unrealize(void)
{
    QTestState *s = start("", 0);
    uint64_t value;

    address(s, true);
    qtest_writeq(s, CONTROL, 7);
    g_assert_cmphex(qtest_readq(s, DATA), ==, 0x96);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':"
        "'/machine/peripheral/mace/i2c-controller',"
        "'property':'realized','value':false}}");
    g_assert_cmphex(observe(s, "events"), ==, 0x2345);
    g_assert_cmpuint(observe(s, "bus-owner"), ==, UINT32_MAX);
    g_assert_false(observe_bool(s, "bus-address-valid"));
    g_assert_cmpint(memtx_load(s, CONTROL, &value, 8), !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, DATA, 0x88, 8), !=, MEMTX_OK);
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/mace',"
        "'property':'realized','value':false}}");
    g_assert_false(observe_bool(s, "controller-mapped"));
    g_assert_cmpuint(observe(s, "callbacks-after-unrealize"), ==, 0);
    g_assert_cmphex(observe(s, "events"), ==, 0x2345);
    g_assert_cmpuint(observe(s, "cursor"), ==, 1);
    qtest_quit(s);
}

static void assert_bus_state(QTestState *s, bool owned)
{
    g_assert_cmpuint(observe(s, "bus-owner"), ==, owned ? 0 : UINT32_MAX);
    g_assert_cmpint(observe_bool(s, "bus-address-valid"), ==, owned);
}

static void test_address_nack_release(void)
{
    QTestState *s = start("", 0);

    qtest_writeq(s, CONTROL, 1);
    qtest_writeq(s, DATA, 0xa0);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x20);
    assert_bus_state(s, false);
    g_assert_cmpuint(observe(s, "address-attempts"), ==, 1);
    g_assert_cmpuint(observe(s, "starts"), ==, 0);
    qtest_writeq(s, DATA, 0x88);
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0);
    assert_bus_state(s, false);
    g_assert_cmpuint(observe(s, "address-attempts"), ==, 2);
    g_assert_cmphex(observe(s, "events"), ==, 0x15);
    qtest_quit(s);
}

static void test_address_nack_release_force_idle(void)
{
    QTestState *s = start("", 0);

    qtest_writeq(s, CONTROL, 1);
    qtest_writeq(s, DATA, 0xa0);
    assert_bus_state(s, false);
    for (unsigned i = 0; i < 17; i++) {
        g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x20);
        qtest_writeq(s, CONTROL, 0);
    }
    g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x20);
    assert_bus_state(s, false);
    g_assert_cmpuint(observe(s, "address-attempts"), ==, 1);
    g_assert_cmpuint(observe(s, "starts"), ==, 0);
    g_assert_cmpuint(observe(s, "stops"), ==, 0);
    qtest_quit(s);
}

static void test_address_nack_hold(void)
{
    static const unsigned polls[] = { 0, 1, 17 };

    for (unsigned p = 0; p < ARRAY_SIZE(polls); p++) {
        QTestState *s = start("", 0);

        qtest_writeq(s, CONTROL, 5);
        qtest_writeq(s, DATA, 0xa0);
        for (unsigned i = 0; i < polls[p]; i++) {
            g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x25);
        }
        qtest_writeq(s, CONTROL, 7);
        qtest_writeq(s, CONTROL, 1);
        g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x21);
        assert_bus_state(s, true);
        g_assert_cmpuint(observe(s, "address-attempts"), ==, 1);
        g_assert_cmpuint(observe(s, "starts"), ==, 0);
        qtest_writeq(s, CONTROL, 4);
        qtest_writeq(s, CONTROL, 4);
        g_assert_cmphex(qtest_readq(s, CONTROL), ==, 0x24);
        assert_bus_state(s, false);
        g_assert_cmpuint(observe(s, "address-attempts"), ==, 1);
        qtest_writeq(s, CONTROL, 5);
        qtest_writeq(s, DATA, 0x88);
        g_assert_cmphex(qtest_readq(s, CONTROL), ==, 5);
        assert_bus_state(s, true);
        g_assert_cmpuint(observe(s, "address-attempts"), ==, 2);
        g_assert_cmpuint(observe(s, "starts"), ==, 1);
        g_assert_cmpuint(observe(s, "stops"), ==, 0);
        qtest_writeq(s, CONTROL, 0);
        g_assert_cmphex(observe(s, "events"), ==, 0x15);
        qtest_quit(s);
    }
}

static void test_address_nack_hold_payload(void)
{
    for (unsigned read = 0; read < 2; read++) {
        QTestState *s = start("", 0);
        uint64_t value;

        qtest_writeq(s, CONTROL, 5);
        qtest_writeq(s, DATA, 0xa0);
        assert_bus_state(s, true);
        if (read) {
            qtest_writeq(s, CONTROL, 7);
            g_assert_cmpint(memtx_load(s, DATA, &value, 8), !=, MEMTX_OK);
        } else {
            g_assert_cmpint(memtx_store(s, DATA, 0x88, 8), !=, MEMTX_OK);
        }
        g_assert_cmphex(qtest_readq(s, CONTROL), ==, read ? 0xa6 : 0xa4);
        assert_bus_state(s, false);
        g_assert_cmpuint(observe(s, "address-attempts"), ==, 1);
        g_assert_cmpuint(observe(s, "starts"), ==, 0);
        g_assert_cmpuint(observe(s, "writes"), ==, 0);
        g_assert_cmpuint(observe(s, "cursor"), ==, 0);
        g_assert_cmpuint(observe(s, "events"), ==, 0);
        qtest_quit(s);
    }
}

static void test_callback_unrealize(void)
{
    for (unsigned mode = 11; mode <= 12; mode++) {
        QTestState *s = start("", mode);

        address(s, true);
        qtest_writeq(s, CONTROL, 7);
        g_assert_cmphex(qtest_readq(s, DATA), ==, 0x96);
        assert_bus_state(s, true);
        g_assert_cmphex(qtest_readq(s, CONTROL), ==, 7);
        g_assert_cmpuint(observe(s, "refused-teardowns"), ==, 1);
        g_assert_cmpuint(observe(s, "nacks"), ==, 0);
        g_assert_cmpuint(observe(s, "stops"), ==, 0);
        g_assert_cmpuint(observe(s, "recursive-events"), ==, 0);
        qtest_writeq(s, CONTROL, 0);
        g_assert_cmphex(observe(s, "events"), ==, 0x2345);
        g_assert_cmpuint(observe(s, "callbacks-after-unrealize"), ==, 0);
        qtest_quit(s);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("mace/i2c/slots", test_slots);
    qtest_add_func("mace/i2c/receive", test_receive);
    qtest_add_func("mace/i2c/final-receive", test_final_receive);
    qtest_add_func("mace/i2c/transmit", test_transmit);
    qtest_add_func("mace/i2c/errors", test_errors);
    qtest_add_func("mace/i2c/nack", test_nack);
    qtest_add_func("mace/i2c/reset-edges", test_reset_edges);
    qtest_add_func("mace/i2c/force-idle-receive", test_force_idle_receive);
    qtest_add_func("mace/i2c/negative-api", test_negative_api);
    qtest_add_func("mace/i2c/empty-bus", test_empty_bus);
    qtest_add_func("mace/i2c/migration", test_migration);
    qtest_add_func("mace/i2c/unrealize", test_unrealize);
    qtest_add_func("mace/i2c/callback-reset", test_callback_reset);
    qtest_add_func("mace/i2c/reject-endpoint", test_reject_endpoint);
    qtest_add_func("mace/i2c/runtime-admission", test_runtime_admission);
    qtest_add_func("mace/i2c/incompatible-migration",
                    test_incompatible_migration);
    qtest_add_func("mace/i2c/unqualified-source-migration",
                    test_unqualified_source_migration);
    qtest_add_func("mace/i2c/default-disabled", test_default_disabled);
    qtest_add_func("mace/i2c/pending-ack-transmit-control",
                    test_pending_ack_transmit_control);
    qtest_add_func("mace/i2c/address-nack", test_address_nack);
    qtest_add_func("mace/i2c/write-only-receive", test_write_only_receive);
    qtest_add_func("mace/i2c/read-only-transmit", test_read_only_transmit);
    qtest_add_func("mace/i2c/reset-during-nack", test_reset_during_nack);
    qtest_add_func("mace/i2c/reset-during-stop", test_reset_during_stop);
    qtest_add_func("mace/i2c/child-unrealize", test_child_unrealize);
    qtest_add_func("mace/i2c/address-nack-release", test_address_nack_release);
    qtest_add_func("mace/i2c/address-nack-release-force-idle",
                    test_address_nack_release_force_idle);
    qtest_add_func("mace/i2c/address-nack-hold", test_address_nack_hold);
    qtest_add_func("mace/i2c/address-nack-hold-payload",
                    test_address_nack_hold_payload);
    qtest_add_func("mace/i2c/callback-unrealize", test_callback_unrealize);
    return g_test_run();
}
