/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Isolated Bedrock local-block MMIO contract. The empty machine provides an
 * address space only and does not represent an Origin 300.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define LB_PATH "/machine/peripheral/lb"
#define LB_BASE 0x11000000ULL
#define MACHINE "-M none -cpu R10000 -device sgi-bedrock-lb,id=lb," \
                "manufacturer=0x36,part-number=0x3012,revision=2," \
                "node-number=0x5a,region-size=0,request-region=7 -S"

#define REVISION        0x0000
#define CPU_PERMISSION  0x4000
#define CPU_OVERRIDE    0x4008
#define IO_PERMISSION   0x4010
#define SOFT_RESET      0x4018
#define REGION_PRESENT  0x4020
#define NODES_ABSENT    0x4028
#define ERROR_BITS      0x4040
#define ERROR_CLEAR     0x4048
#define ERROR_HEADER_1  0x4050
#define ERROR_HEADER_2  0x4058
#define ERROR_DATA      0x4060
#define DEBUG_SELECT    0x4100
#define RTC_LOCAL       0x4200
#define RTC_FILTER      0x4208
#define SCRATCH(n)      (0x8000 + (n) * 8)
#define SCRATCH_WZ(n)   (0x8040 + (n) * 8)
#define SCRATCH_RS(n)   (0x8058 + ((n) - 3) * 8)
#define VECTOR_PARAMS   0xc000
#define VECTOR_ROUTE    0xc008
#define VECTOR_DATA     0xc010
#define VECTOR_STATUS   0xc020
#define VECTOR_RETURN   0xc028
#define VECTOR_READ     0xc030
#define VECTOR_STATUS_CLEAR 0xc038

/* The one-bit type field is the model's reading of an open width. */
#define VECTOR_PARAMS_MASK (1ULL | (0x1fffffULL << 3) | (0xffULL << 32) | \
                            (0x7ffULL << 40))

static QTestState *lb_start(const char *extra)
{
    QTestState *s = qtest_initf(MACHINE " %s", extra);

    qtest_sysbus_mmio_map(s, LB_PATH, 0, LB_BASE);
    return s;
}

static void test_identity_and_permissions(void)
{
    QTestState *s = lb_start("");
    const uint64_t identity = (0x36ULL << 1) | (0x3012ULL << 12) |
                              (2ULL << 28) | (0x5aULL << 32);

    g_assert_cmphex(qtest_readq(s, LB_BASE + REVISION), ==, identity);
    qtest_writeq(s, LB_BASE + REVISION, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + REVISION), ==, identity);

    qtest_writeq(s, LB_BASE + CPU_PERMISSION, 0);
    qtest_writeq(s, LB_BASE + CPU_OVERRIDE, 0x1234);
    g_assert_cmphex(qtest_readq(s, LB_BASE + CPU_PERMISSION), ==, 0);
    qtest_writeq(s, LB_BASE + CPU_OVERRIDE, 0x53474972756c6573ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + CPU_PERMISSION), ==, 1ULL << 7);
    qtest_writeq(s, LB_BASE + IO_PERMISSION, 0xa55aa55aa55aa55aULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + IO_PERMISSION), ==,
                    0xa55aa55aa55aa55aULL);
    qtest_quit(s);
}

static void test_storage_masks_and_error(void)
{
    QTestState *s = lb_start("");
    const uint64_t rtc_local_mask = 1ULL | (0x3ffULL << 4) |
                                    (0x3ffULL << 16) | (1ULL << 28) |
                                    (1ULL << 32);
    const uint64_t rtc_filter_mask = 0x1fULL | (0xfffULL << 8) |
                                     (1ULL << 20) | (0x3ffULL << 24) |
                                     (0x3ffULL << 36) | (0x3ffULL << 48);

    qtest_irq_intercept_out_named(s, LB_PATH, "error");
    qtest_writeq(s, LB_BASE + REGION_PRESENT, 0x8000000000000001ULL);
    qtest_writeq(s, LB_BASE + NODES_ABSENT, 0xffff01238000ff7fULL);
    qtest_writeq(s, LB_BASE + RTC_LOCAL, UINT64_MAX);
    qtest_writeq(s, LB_BASE + RTC_FILTER, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + REGION_PRESENT), ==,
                    0x8000000000000001ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + NODES_ABSENT), ==,
                    0x80ff00238000807fULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + RTC_LOCAL), ==, rtc_local_mask);
    g_assert_cmphex(qtest_readq(s, LB_BASE + RTC_FILTER), ==, rtc_filter_mask);

    qtest_writeq(s, LB_BASE + ERROR_HEADER_1, 1ULL << 44);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_HEADER_1), ==, 1ULL << 44);
    qtest_writeq(s, LB_BASE + ERROR_HEADER_2, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_HEADER_2), ==,
                    (1ULL << 38) - 1);
    qtest_writeq(s, LB_BASE + ERROR_DATA, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_DATA), ==,
                    0x0123456789abcdefULL);
    qtest_writeq(s, LB_BASE + DEBUG_SELECT, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + DEBUG_SELECT), ==, 0x3f);
    qtest_writeq(s, LB_BASE + VECTOR_ROUTE, 0xfedcba9876543210ULL);
    qtest_writeq(s, LB_BASE + VECTOR_DATA, 0x1020304050607080ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_ROUTE), ==,
                    0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_DATA), ==,
                    0x1020304050607080ULL);
    qtest_writeq(s, LB_BASE + ERROR_BITS, (1ULL << 10) | 3);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_BITS), ==,
                    (1ULL << 10) | 3);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, LB_BASE + ERROR_CLEAR, 1);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_BITS), ==,
                    (1ULL << 10) | 2);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, LB_BASE + ERROR_CLEAR, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_BITS), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_quit(s);
}

static void test_byte_order_and_width(void)
{
    QTestState *s = lb_start("");
    const uint8_t bytes[8] = { 1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    uint8_t actual[8];
    uint64_t addr = LB_BASE + CPU_PERMISSION;

    qtest_memwrite(s, addr, bytes, sizeof(bytes));
    g_assert_cmphex(qtest_readq(s, addr), ==, 0x0123456789abcdefULL);
    qtest_memread(s, addr, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), bytes, sizeof(bytes));
    qtest_writeb(s, addr + 7, 0);
    qtest_writew(s, addr + 6, 0);
    qtest_writel(s, addr + 4, 0);
    qtest_writeq(s, addr + 1, 0);
    g_assert_cmphex(qtest_readq(s, addr), ==, 0x0123456789abcdefULL);
    qtest_quit(s);
}

static void test_scratch_aliases_and_reset(void)
{
    QTestState *s = lb_start("");

    qtest_irq_intercept_out_named(s, LB_PATH, "soft-reset");
    for (unsigned i = 0; i < 5; i++) {
        qtest_writeq(s, LB_BASE + SCRATCH(i), 0x100 + i);
    }
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeq(s, LB_BASE + SCRATCH(i), 0);
        qtest_writeq(s, LB_BASE + SCRATCH_WZ(i), 0xdeadbeef + i);
        qtest_writeq(s, LB_BASE + SCRATCH_WZ(i), 0xcafebabe + i);
        g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(i)), ==,
                        0xdeadbeef + i);
    }
    for (unsigned i = 3; i < 5; i++) {
        qtest_writeq(s, LB_BASE + SCRATCH(i), UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(i)), ==, 1);
        qtest_writeq(s, LB_BASE + SCRATCH(i), 0);
        g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH_RS(i)), ==, 0);
        g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH_RS(i)), ==, 1);
        qtest_writeq(s, LB_BASE + SCRATCH(i), 0);
        g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH_RS(i)), ==, 0);
    }

    qtest_writeq(s, LB_BASE + ERROR_BITS, 4);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(0)), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(1)), ==, 0xdeadbef0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(2)), ==, 0xdeadbef1);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_BITS), ==, 4);

    qtest_writeq(s, LB_BASE + SCRATCH(0), 0x99);
    qtest_writeq(s, LB_BASE + SOFT_RESET, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(s, 0), ==, 1);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(0)), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(1)), ==, 0xdeadbef0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_BITS), ==, 4);
    qtest_quit(s);
}

/*
 * The vector registers as the datasheet's checklist states them: parameters
 * keep their fields with busy reading clear, a status-clear write is
 * accepted, and route and data are plain storage. No peer can answer a
 * launch on a one-node machine, so the launch is logged as unimplemented
 * and status, return route and read data hold no reply.
 */
static void test_vector_registers(void)
{
    g_autofree char *log = g_strdup_printf("%s/sn1-lb-vector-XXXXXX.log",
                                           g_get_tmp_dir());
    g_autofree char *extra = NULL;
    g_autofree char *found = NULL;
    QTestState *s;
    int fd = g_mkstemp(log);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = g_strdup_printf("-d unimp,guest_errors -D %s", log);
    s = lb_start(extra);

    qtest_writeq(s, LB_BASE + VECTOR_ROUTE, 0xfedcba9876543210ULL);
    qtest_writeq(s, LB_BASE + VECTOR_DATA, 0x1020304050607080ULL);
    qtest_writeq(s, LB_BASE + VECTOR_PARAMS, UINT64_MAX);
    /* Bit 1 reading 0 is the model's one-bit type, not a datasheet fact. */
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_PARAMS), ==,
                    VECTOR_PARAMS_MASK);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_ROUTE), ==,
                    0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_DATA), ==,
                    0x1020304050607080ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_STATUS), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_RETURN), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_READ), ==, 0);
    qtest_writeq(s, LB_BASE + VECTOR_STATUS_CLEAR, UINT64_MAX);
    /* Read-clear is the family precedent and open here; status never sets. */
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_STATUS_CLEAR), ==, 0);
    qtest_writeq(s, LB_BASE + VECTOR_STATUS, UINT64_MAX);
    qtest_writeq(s, LB_BASE + VECTOR_RETURN, UINT64_MAX);
    qtest_writeq(s, LB_BASE + VECTOR_READ, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_STATUS), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_RETURN), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_READ), ==, 0);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_nonnull(strstr(found, "vector launch"));
    g_assert_null(strstr(found, "unsupported write at 0xc000"));
    g_assert_null(strstr(found, "unsupported read at 0xc020"));
    g_assert_null(strstr(found, "unsupported read at 0xc028"));
    g_assert_null(strstr(found, "unsupported read at 0xc030"));
    g_assert_null(strstr(found, "unsupported write at 0xc038"));
    g_assert_null(strstr(found, "unsupported read at 0xc038"));
    g_assert_null(strstr(found, "read from write-only register 0xc038"));
    g_assert_nonnull(strstr(found, "write to read-only register 0xc020"));
    g_assert_nonnull(strstr(found, "write to read-only register 0xc028"));
    g_assert_nonnull(strstr(found, "write to read-only register 0xc030"));
    g_assert_cmpint(unlink(log), ==, 0);
}

static void wait_for_migration(QTestState *from)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

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

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-lb-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = lb_start("");
    to = lb_start("-incoming defer");
    qtest_writeq(from, LB_BASE + CPU_PERMISSION, 0x123456789abcdef0ULL);
    qtest_writeq(from, LB_BASE + REGION_PRESENT, 0x55aa55aa55aa55aaULL);
    qtest_writeq(from, LB_BASE + SCRATCH(2), 0x0ead0040);
    qtest_writeq(from, LB_BASE + ERROR_BITS, 1ULL << 9);
    qtest_writeq(from, LB_BASE + DEBUG_SELECT, 0x2a);
    qtest_writeq(from, LB_BASE + VECTOR_ROUTE, 0x123456789abcdef0ULL);
    qtest_writeq(from, LB_BASE + VECTOR_DATA, 0xfedcba9876543210ULL);
    qtest_writeq(from, LB_BASE + VECTOR_PARAMS, 0x5a5a5a5a5a5a5a5aULL);
    qtest_irq_intercept_out_named(to, LB_PATH, "error");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    wait_for_migration(from);
    g_assert_cmphex(qtest_readq(to, LB_BASE + CPU_PERMISSION), ==,
                    0x123456789abcdef0ULL);
    g_assert_cmphex(qtest_readq(to, LB_BASE + REGION_PRESENT), ==,
                    0x55aa55aa55aa55aaULL);
    g_assert_cmphex(qtest_readq(to, LB_BASE + SCRATCH(2)), ==, 0x0ead0040);
    g_assert_cmphex(qtest_readq(to, LB_BASE + ERROR_BITS), ==, 1ULL << 9);
    g_assert_cmphex(qtest_readq(to, LB_BASE + DEBUG_SELECT), ==, 0x2a);
    g_assert_cmphex(qtest_readq(to, LB_BASE + VECTOR_ROUTE), ==,
                    0x123456789abcdef0ULL);
    g_assert_cmphex(qtest_readq(to, LB_BASE + VECTOR_DATA), ==,
                    0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(to, LB_BASE + VECTOR_PARAMS), ==,
                    0x5a5a5a5a5a5a5a5aULL & VECTOR_PARAMS_MASK);
    g_assert_true(qtest_get_irq(to, 0));
    qtest_writeq(to, LB_BASE + ERROR_CLEAR, 1ULL << 9);
    g_assert_false(qtest_get_irq(to, 0));
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-lb/identity-permissions",
                   test_identity_and_permissions);
    qtest_add_func("sn1/bedrock-lb/storage-masks-error",
                   test_storage_masks_and_error);
    qtest_add_func("sn1/bedrock-lb/byte-order-width",
                   test_byte_order_and_width);
    qtest_add_func("sn1/bedrock-lb/scratch-reset",
                   test_scratch_aliases_and_reset);
    qtest_add_func("sn1/bedrock-lb/vector-registers", test_vector_registers);
    qtest_add_func("sn1/bedrock-lb/migration", test_migration);
    return g_test_run();
}
