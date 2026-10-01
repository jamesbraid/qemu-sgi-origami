/*
 * Machine 'none' tests.
 *
 * Copyright (c) 2018 Red Hat Inc.
 *
 * Authors:
 *  Igor Mammedov <imammedo@redhat.com>,
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include "qemu/cutils.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-migrate.h"
#include "qobject/qdict.h"


struct arch2cpu {
    const char *arch;
    const char *cpu_model;
};

static struct arch2cpu cpus_map[] = {
    /* tested targets list */
    { "arm", "cortex-a15" },
    { "aarch64", "cortex-a57" },
    { "avr", "avr6-avr-cpu" },
    { "x86_64", "qemu64,apic-id=0" },
    { "i386", "qemu32,apic-id=0" },
    { "alpha", "ev67" },
    { "m68k", "m5206" },
    { "microblaze", "any" },
    { "mips", "4Kc" },
    { "mipsel", "I7200" },
    { "mips64", "20Kc" },
    { "mips64el", "I6500" },
    { "or1k", "or1200" },
    { "ppc", "604" },
    { "ppc64", "power11_v2.0" },
    { "s390x", "qemu" },
    { "sh4", "sh7750r" },
    { "sh4eb", "sh7751r" },
    { "sparc", "LEON2" },
    { "sparc64", "Fujitsu Sparc64" },
    { "tricore", "tc1796" },
    { "xtensa", "dc233c" },
    { "xtensaeb", "fsf" },
    { "hppa", "pa-7300lc" },
    { "riscv64", "rv64" },
    { "riscv32", "rv32" },
    { "rx", "rx62n" },
    { "loongarch64", "la464"},
};

static const char *get_cpu_model_by_arch(const char *arch)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(cpus_map); i++) {
        if (!strcmp(arch, cpus_map[i].arch)) {
            return cpus_map[i].cpu_model;
        }
    }
    return NULL;
}

static void test_machine_cpu_cli(void)
{
    QDict *response;
    const char *arch = qtest_get_arch();
    const char *cpu_model = get_cpu_model_by_arch(arch);
    QTestState *qts;

    if (!cpu_model) {
        fprintf(stderr, "WARNING: cpu name for target '%s' isn't defined,"
                " add it to cpus_map\n", arch);
        return; /* TODO: die here to force all targets have a test */
    }
    qts = qtest_initf("-machine none -cpu \"%s\"", cpu_model);

    response = qtest_qmp(qts, "{ 'execute': 'quit' }");
    g_assert(qdict_haskey(response, "return"));
    qobject_unref(response);

    qtest_quit(qts);
}

static void test_memory_transaction_result(void)
{
    const char *cpu_model = get_cpu_model_by_arch(qtest_get_arch());
    QTestState *qts;
    uint64_t value;
    uint32_t word;
    uint64_t unassigned = 0x10000000000ULL;

    if (!cpu_model || !qtest_has_device("memory-transaction-test")) {
        g_test_skip("Memory transaction fixture or CPU unavailable");
        return;
    }
    qts = qtest_initf(MEMTX_TEST_DEVICE "-machine none -cpu \"%s\"", cpu_model);
    g_assert_cmpuint(memtx_readq(qts, unassigned, &value), ==,
                     MEMTX_DECODE_ERROR);
    g_assert_cmphex(value, ==, 0);
    g_assert_cmpuint(memtx_writeq(qts, unassigned, UINT64_MAX), ==,
                     MEMTX_DECODE_ERROR);
    g_assert_cmphex(qtest_readq(qts, unassigned), ==, 0);
    g_assert_cmpuint(memtx_readl(qts, unassigned, &word), ==,
                     MEMTX_DECODE_ERROR);
    g_assert_cmphex(word, ==, 0);
    g_assert_cmpuint(memtx_writel(qts, unassigned, UINT32_MAX), ==,
                     MEMTX_DECODE_ERROR);
    qtest_quit(qts);
}

static void test_transaction_fixture(gconstpointer data)
{
    const char *cpu_model = get_cpu_model_by_arch(qtest_get_arch());
    QTestState *qts;
    QDict *response;
    const char *bad[] = {
        "read -1 0 3", "read 99 0 4", "read -2 0 4",
        "write -1 0 xyz", "write -1 0 001122", "read -1 0 4 extra",
    };

    if (!cpu_model || !qtest_has_device("memory-transaction-test")) {
        g_test_skip("Memory transaction fixture or CPU unavailable");
        return;
    }
    qts = qtest_initf("-machine none -m 1M -cpu \"%s\" "
                      "-device memory-transaction-test,id=memtx %s", cpu_model,
                      (const char *)data);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/memtx',"
        "'property':'request','value':'read -1 0 8'}}");
    response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/memtx',"
        "'property':'result'}}");
    g_assert_cmpstr(qdict_get_str(response, "return"), ==,
                    "0x00000000 0000000000000000");
    qobject_unref(response);

    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/memtx',"
        "'property':'request','value':'write 0 0xffffc 1122334455667788'}}");
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/memtx',"
        "'property':'request','value':'read 0 0xffffc 8'}}");
    response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/memtx',"
        "'property':'result'}}");
    g_assert_cmpstr(qdict_get_str(response, "return"), ==,
                    "0x00000002 1122334400000000");
    qobject_unref(response);
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        response = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{"
            "'path':'/machine/peripheral/memtx',"
            "'property':'request','value':%s}}", bad[i]);
        g_assert(qdict_haskey(response, "error"));
        qobject_unref(response);
    }
    /* Unaligned RAM exercises all typed widths in the target byte order. */
    for (unsigned size = 1; size <= 8; size *= 2) {
        uint64_t expected = UINT64_C(0x8877665544332211) >> (8 * (8 - size));
        uint64_t value;
        uint8_t bytes[8];

        g_assert_cmpuint(memtx_write(qts, 3, expected, size), ==, MEMTX_OK);
        g_assert_cmpuint(memtx_read(qts, 3, &value, size), ==, MEMTX_OK);
        g_assert_cmphex(value, ==, expected);
        qtest_memread(qts, 3, bytes, size);
        for (unsigned i = 0; i < size; i++) {
            unsigned shift = 8 * (qtest_big_endian(qts) ? size - 1 - i : i);

            g_assert_cmphex(bytes[i], ==, (expected >> shift) & 0xff);
        }
    }
    qtest_quit(qts);
}

static void test_transaction_validation(gconstpointer data)
{
    const char *cpu_model = get_cpu_model_by_arch(qtest_get_arch());
    bool unrealized = GPOINTER_TO_INT(data);
    QTestState *qts;
    QDict *response;

    if (!cpu_model || !qtest_has_device("memory-transaction-test")) {
        g_test_skip("Memory transaction fixture or CPU unavailable");
        return;
    }
    qts = qtest_initf(MEMTX_TEST_DEVICE
                      "-machine none -m 1M -cpu \"%s\"", cpu_model);
    qtest_writeq(qts, 0, UINT64_C(0x1122334455667788));
    if (unrealized) {
        qtest_qmp_assert_success(qts,
            "{'execute':'qom-set','arguments':{'path':%s,"
            "'property':'realized','value':false}}", MEMTX_TEST_PATH);
    }
    response = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'request','value':%s}}", MEMTX_TEST_PATH,
        unrealized ? "write -1 0 0000000000000000" :
                     "write -1 0xfffffffffffffffc 0000000000000000");
    g_assert(qdict_haskey(response, "error"));
    qobject_unref(response);
    g_assert_cmphex(qtest_readq(qts, 0), ==, UINT64_C(0x1122334455667788));
    qtest_quit(qts);
}

static void test_transaction_scratch(void)
{
    const char *cpu_model = get_cpu_model_by_arch(qtest_get_arch());
    g_autofree char *dir = NULL;
    g_autofree char *state = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    QDict *response;

    if (!cpu_model || !qtest_has_device("memory-transaction-test")) {
        g_test_skip("Memory transaction fixture or CPU unavailable");
        return;
    }
    from = qtest_initf(MEMTX_TEST_DEVICE
                       "-machine none -m 1M -cpu \"%s\"", cpu_model);
    to = qtest_initf(MEMTX_TEST_DEVICE
                     "-machine none -m 1M -cpu \"%s\" -incoming defer",
                     cpu_model);
    g_assert_cmpuint(memtx_writeb(from, 0, 0x55), ==, MEMTX_OK);
    g_assert_cmpuint(memtx_writeb(to, 0, 0xaa), ==, MEMTX_OK);
    qtest_system_reset(from);
    response = qtest_qmp(from,
        "{'execute':'qom-get','arguments':{'path':%s,'property':'result'}}",
        MEMTX_TEST_PATH);
    g_assert_cmpstr(qdict_get_str(response, "return"), ==, "0x00000000 55");
    qobject_unref(response);

    dir = g_dir_make_tmp("memory-transaction-XXXXXX", NULL);
    g_assert_nonnull(dir);
    state = g_build_filename(dir, "state", NULL);
    quoted = g_shell_quote(state);
    uri = g_strdup_printf("exec:cat >%s", quoted);
    sgi_migrate_stopped(from, uri);
    g_free(uri);
    uri = g_strdup_printf("exec:cat %s", quoted);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readb(to, 0), ==, 0x55);
    response = qtest_qmp(to,
        "{'execute':'qom-get','arguments':{'path':%s,'property':'result'}}",
        MEMTX_TEST_PATH);
    g_assert_cmpstr(qdict_get_str(response, "return"), ==, "0x00000000 aa");
    qobject_unref(response);
    qtest_qmp_assert_success(to,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'realized','value':false}}", MEMTX_TEST_PATH);
    response = qtest_qmp(to,
        "{'execute':'qom-get','arguments':{'path':%s,'property':'result'}}",
        MEMTX_TEST_PATH);
    g_assert(qdict_haskey(response, "error"));
    qobject_unref(response);
    qtest_quit(to);
    qtest_quit(from);
    g_assert_cmpint(unlink(state), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("machine/none/cpu_option", test_machine_cpu_cli);
    qtest_add_func("machine/none/memory_transaction_result",
                   test_memory_transaction_result);

    qtest_add_data_func("machine/none/transaction_fixture", "",
                        test_transaction_fixture);

    if (qtest_has_accel("tcg")) {
        qtest_add_data_func("machine/none/transaction_fixture_tcg",
                            "-accel tcg -S", test_transaction_fixture);
    }

    qtest_add_data_func("machine/none/transaction_overflow",
                        GINT_TO_POINTER(false), test_transaction_validation);
    qtest_add_data_func("machine/none/transaction_unrealized",
                        GINT_TO_POINTER(true), test_transaction_validation);
    qtest_add_func("machine/none/transaction_scratch",
                   test_transaction_scratch);

    return g_test_run();
}
