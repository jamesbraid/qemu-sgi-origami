/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-mips-emit.h"
#include "sgi-wait.h"

#define MAILBOX 0x1000
#define CODE 0x4000
#define LOOP_READS 64
#define READS (LOOP_READS + 2)
#define SUM 8
#define PATH 12
#define IRQ_COUNT 16
#define IRQ_CAUSE 20
#define IRQ_EPC 24
#define IRQ_VALUE 28
#define IRQ_STATUS 32
#define FIXTURE_PATH "/machine/peripheral/meta"

static char *image_path;

static void test_metadata(gconstpointer data)
{
    const char *options = data;
    g_autofree char *quoted = g_shell_quote(image_path);
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = qtest_initf("-M origin200 -m 128M -cpu R10000 -bios %s "
                      "-nic none -S -device tcg-io-recompile-fixture,id=meta %s",
                      quoted, options);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/meta',"
        "'property':'exercise','value':true}}");
    qtest_quit(qts);
}

static void control(QTestState *qts, const char *name)
{
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':%s,'value':true}}", FIXTURE_PATH, name);
}

static uint64_t counter(QTestState *qts, const char *name)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        FIXTURE_PATH, name);

    return qdict_get_int(reply, "return");
}

static bool has_metadata(QTestState *qts)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'metadata-present'}}", FIXTURE_PATH);

    return qdict_get_bool(reply, "return");
}

static void write_program(QTestState *qts, bool taken)
{
    uint32_t program[] = {
        mips_lui(8, 0xbe00),             /* t0: counting MMIO window */
        mips_addiu(16, 0, 0),            /* s0: checksum */
        mips_addiu(17, 0, LOOP_READS),    /* s1: loop count */
        mips_addiu(9, 0, -1),
        mips_mtc0(9, 11),                /* Compare: timer stays quiet */
        mips_mtc0(0, 13),                /* Cause: no software IRQ */
        mips_lui(9, 0x0040),
        mips_ori(9, 9, 0x0401),          /* BEV, external IRQ 2, IE */
        mips_mtc0(9, 12),
        0, 0, 0, 0,
        mips_lw(10, 8, 0),               /* loop: actual MMIO in mid-TB */
        mips_rtype(16, 10, 16, 0, 0x21), /* addu s0, s0, t2 */
        mips_addiu(17, 17, -1),
        mips_itype(5, 17, 0, -4),        /* bne s1, zero, loop */
        0,
        mips_itype(taken ? 4 : 5, 0, 0, 4),
        mips_lw(10, 8, 0),               /* delay slot: read 65 raises IRQ */
        mips_addiu(17, 0, 1),            /* untaken destination */
        mips_itype(4, 0, 0, 2),
        0,
        mips_addiu(17, 0, 2),            /* taken destination */
        mips_rtype(16, 10, 16, 0, 0x21),
        mips_lw(10, 8, 0),               /* read 66, after the handler */
        mips_rtype(16, 10, 16, 0, 0x21),
        mips_sw(16, 26, SUM),
        mips_sw(17, 26, PATH),
        mips_rtype(31, 0, 0, 0, 8),      /* jr ra */
        0,
    };
    uint8_t bytes[sizeof(program)];

    for (unsigned int i = 0; i < G_N_ELEMENTS(program); i++) {
        stl_be_p(bytes + i * 4, program[i]);
    }
    qtest_memwrite(qts, CODE, bytes, sizeof(bytes));
}

static void test_execution(gconstpointer data)
{
    bool taken = GPOINTER_TO_INT(data);
    g_autofree char *quoted = g_shell_quote(image_path);
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = qtest_initf("-M malta -m 64M -cpu R10000 -bios %s -nic none "
        "-S -accel tcg,thread=single -icount shift=3,align=off,sleep=off "
        "-device tcg-io-recompile-fixture,id=meta,"
        "mmio-base=0x1e000000,irq-after=%u", quoted, LOOP_READS + 1);
    write_program(qts, taken);
    for (unsigned int round = 0; round < 2; round++) {
        SgiWait wait;

        control(qts, "reset-io");
        control(qts, "flush");
        g_assert_false(has_metadata(qts));
        qtest_memset(qts, MAILBOX, 0, 64);
        qtest_writel(qts, MAILBOX, 0xa0000000 | CODE);
        qtest_qmp_assert_success(qts, "{'execute':'cont'}");
        sgi_wait_start(&wait, "icount MMIO completion, round %u", round);
        while (!qtest_readl(qts, MAILBOX + 4)) {
            sgi_wait_pause(&wait, 1000);
        }
        qtest_qmp_assert_success(qts, "{'execute':'stop'}");
        qtest_qmp_eventwait(qts, "STOP");
        g_assert_true(has_metadata(qts));
        g_assert_cmpuint(counter(qts, "reads"), ==, READS);
        g_assert_cmpuint(counter(qts, "acks"), ==, 1);
        g_assert_cmpuint(qtest_readl(qts, MAILBOX + SUM), ==,
                          READS * (READS + 1) / 2);
        g_assert_cmpuint(qtest_readl(qts, MAILBOX + PATH), ==, taken ? 2 : 1);
        g_assert_cmpuint(qtest_readl(qts, MAILBOX + IRQ_COUNT), ==, 1);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + IRQ_CAUSE) & 0x8000047c,
                         ==, 0x400); /* external IRQ 2, no exception or BD */
        g_assert_cmphex(qtest_readl(qts, MAILBOX + IRQ_EPC), ==,
                         0xa0000000 | (CODE + 4 * (taken ? 23 : 20)));
        g_assert_cmpuint(qtest_readl(qts, MAILBOX + IRQ_VALUE), ==,
                          LOOP_READS + 1);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + IRQ_STATUS) & 3, ==, 3);
    }
    qtest_quit(qts);
}

#define STRESS_STOP 0x40
#define STRESS_READY 0x44
#define STRESS_WRITES 0x48
#define STRESS_READER_DONE 0x4c
#define STRESS_WRITER_DONE 0x50
#define STRESS_COUNT 0x54
#define STRESS_SUM 0x58
#define STRESS_MAX_READS 0x100000u
#define STRESS_WRITER_CODE 0x8000

static void get_tb_counters(QTestState *qts, unsigned int *invalidations,
                            unsigned int *flushes)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'tb-counters'}}", FIXTURE_PATH);

    g_assert_cmpint(sscanf(qdict_get_str(reply, "return"), "%u %u",
                           invalidations, flushes), ==, 2);
}

static void start_cpu(QTestState *qts, const char *fixture, uint32_t code)
{
    g_autofree char *context = g_strdup_printf("0xffffffffa000%04x "
                                               "0xffffffffa0001000", code);

    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'ram-context','value':%s}}", fixture, context);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'test-halted','value':false}}", fixture);
}

static void test_concurrent(void)
{
    /* CPU1 rewrites reader[7], the counting load itself, with the same word. */
    const uint32_t reader[] = {
        mips_lui(8, 0xbe00), mips_addiu(16, 0, 0), mips_addiu(17, 0, 0),
        mips_lui(18, STRESS_MAX_READS >> 16),
        mips_lw(11, 26, STRESS_READY), mips_itype(4, 11, 0, -2), 0,
        mips_lw(10, 8, 0),
        mips_rtype(16, 10, 16, 0, 0x2d), /* daddu checksum, checksum, read */
        mips_daddiu(17, 17, 1),
        mips_lw(11, 26, STRESS_STOP), mips_itype(5, 11, 0, 3), 0,
        mips_itype(5, 17, 18, -7), 0,
        mips_sd(16, 26, STRESS_SUM), mips_sw(17, 26, STRESS_COUNT),
        mips_addiu(11, 0, 1), mips_sw(11, 26, STRESS_READER_DONE),
        mips_itype(4, 0, 0, -1), 0,
    };
    const uint32_t writer[] = {
        mips_lui(8, 0xa000), mips_ori(8, 8, CODE + 7 * 4),
        mips_lui(9, mips_lw(10, 8, 0) >> 16),
        mips_ori(9, 9, mips_lw(10, 8, 0)),
        mips_addiu(10, 0, 0), mips_addiu(11, 0, 1),
        mips_sw(11, 26, STRESS_READY),
        mips_sw(9, 8, 0), mips_sync(), mips_addiu(10, 10, 1),
        mips_sw(10, 26, STRESS_WRITES),
        mips_lw(11, 26, STRESS_STOP), mips_itype(4, 11, 0, -6), 0,
        mips_addiu(11, 0, 1), mips_sw(11, 26, STRESS_WRITER_DONE),
        mips_itype(4, 0, 0, -1), 0,
    };
    g_autofree char *quoted = g_shell_quote(image_path);
    uint8_t bytes[sizeof(reader)];
    unsigned int invalidations, flushes;
    uint64_t reads;
    QTestState *qts;
    SgiWait wait;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = qtest_initf("-M origin200 -m 128M -cpu R10000 -smp 2 -bios %s "
        "-nic none -S -accel tcg,thread=multi "
        "-device tcg-io-recompile-fixture,id=meta,mmio-base=0x1e000000 "
        "-device mips-cpu-exception-fixture,id=reader,cpu-index=0 "
        "-device mips-cpu-exception-fixture,id=writer,cpu-index=1", quoted);
    {
        g_autoptr(QDict) reply = qtest_qmp(qts,
                                           "{'execute':'query-cpus-fast'}");
        const QListEntry *entry;
        int64_t threads[2];
        unsigned int count = 0;

        QLIST_FOREACH_ENTRY(qdict_get_qlist(reply, "return"), entry) {
            g_assert_cmpuint(count, <, 2);
            threads[count++] = qdict_get_int(qobject_to(QDict,
                qlist_entry_obj(entry)), "thread-id");
        }
        g_assert_cmpuint(count, ==, 2);
        g_assert_cmpint(threads[0], !=, threads[1]);
    }
    for (unsigned int i = 0; i < G_N_ELEMENTS(reader); i++) {
        stl_be_p(bytes + i * 4, reader[i]);
    }
    qtest_memwrite(qts, CODE, bytes, sizeof(reader));
    for (unsigned int i = 0; i < G_N_ELEMENTS(writer); i++) {
        stl_be_p(bytes + i * 4, writer[i]);
    }
    qtest_memwrite(qts, STRESS_WRITER_CODE, bytes, sizeof(writer));
    qtest_memset(qts, MAILBOX, 0, 0x80);
    start_cpu(qts, "/machine/peripheral/reader", CODE);
    start_cpu(qts, "/machine/peripheral/writer", STRESS_WRITER_CODE);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    for (unsigned int phase = 0; phase < 4; phase++) {
        unsigned int old_invalidations;
        uint32_t old_writes = qtest_readl(qts, MAILBOX + STRESS_WRITES);
        uint64_t old_reads = counter(qts, "reads");

        get_tb_counters(qts, &old_invalidations, &flushes);
        sgi_wait_start(&wait, "concurrent MMIO/invalidation phase %u", phase);
        do {
            g_assert_cmpuint(qtest_readl(qts,
                MAILBOX + STRESS_READER_DONE), ==, 0);
            get_tb_counters(qts, &invalidations, &flushes);
            reads = counter(qts, "reads");
            if (reads >= old_reads + 128 &&
                invalidations >= old_invalidations + 16 &&
                qtest_readl(qts, MAILBOX + STRESS_WRITES) > old_writes) {
                break;
            }
            sgi_wait_pause(&wait, 1000);
        } while (true);
        g_test_message("phase %u: reads=%" PRIu64 ", peer writes=%u, "
                       "TB invalidations=%u", phase, reads,
                       qtest_readl(qts, MAILBOX + STRESS_WRITES),
                       invalidations);
        control(qts, "queue-flush");
        unsigned int old_flushes = flushes;

        sgi_wait_start(&wait, "exclusive full TB flush, phase %u", phase);
        do {
            get_tb_counters(qts, &invalidations, &flushes);
            if (flushes > old_flushes) {
                g_test_message("phase %u: completed full TB flush %u",
                               phase, flushes);
                break;
            }
            sgi_wait_pause(&wait, 1000);
        } while (true);
    }
    reads = counter(qts, "reads");
    sgi_wait_start(&wait, "MMIO progress after final exclusive flush");
    while (counter(qts, "reads") < reads + 128) {
        g_assert_cmpuint(qtest_readl(qts,
            MAILBOX + STRESS_READER_DONE), ==, 0);
        sgi_wait_pause(&wait, 1000);
    }
    qtest_writel(qts, MAILBOX + STRESS_STOP, 1);
    sgi_wait_start(&wait, "both concurrent payloads stopped");
    while (!qtest_readl(qts, MAILBOX + STRESS_READER_DONE) ||
           !qtest_readl(qts, MAILBOX + STRESS_WRITER_DONE)) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    qtest_qmp_eventwait(qts, "STOP");
    reads = counter(qts, "reads");
    g_assert_cmpuint(reads, ==, qtest_readl(qts, MAILBOX + STRESS_COUNT));
    g_assert_cmpuint(reads, <, STRESS_MAX_READS);
    g_assert_cmpuint(qtest_readq(qts, MAILBOX + STRESS_SUM), ==,
                      reads * (reads + 1) / 2);
    g_assert_true(has_metadata(qts));
    g_test_message("completed %" PRIu64 " MMIO reads, checksum=%" PRIu64,
                   reads, qtest_readq(qts, MAILBOX + STRESS_SUM));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_autofree char *directory = NULL;
    /* The dispatcher and exception handler are original synthetic MIPS. */
    const uint32_t dispatcher[] = {
        mips_lui(26, 0xa000), mips_ori(26, 26, MAILBOX),
        mips_lw(8, 26, 0), mips_itype(4, 8, 0, -2), 0,
        mips_sw(0, 26, 0), mips_rtype(8, 0, 31, 0, 9), 0,
        mips_addiu(8, 0, 1), mips_sw(8, 26, 4),
        mips_itype(4, 0, 0, -9), 0,
    };
    const uint32_t handler[] = {
        mips_mfc0(27, 13), mips_sw(27, 26, IRQ_CAUSE),
        mips_mfc0(27, 14), mips_sw(27, 26, IRQ_EPC),
        mips_sw(10, 26, IRQ_VALUE),
        mips_mfc0(27, 12), mips_sw(27, 26, IRQ_STATUS),
        mips_lw(27, 26, IRQ_COUNT), mips_addiu(27, 27, 1),
        mips_sw(27, 26, IRQ_COUNT),
        mips_lui(27, 0xbe00), mips_sw(0, 27, 4), /* acknowledge IRQ */
        0x42000018, 0, /* eret */
    };
    uint8_t image[4096] = { 0 };

    g_test_init(&argc, &argv, NULL);
    for (unsigned int i = 0; i < G_N_ELEMENTS(dispatcher); i++) {
        stl_be_p(image + i * 4, dispatcher[i]);
    }
    for (unsigned int i = 0; i < G_N_ELEMENTS(handler); i++) {
        stl_be_p(image + 0x380 + i * 4, handler[i]);
    }
    directory = g_dir_make_tmp("tcg-io-recompile-XXXXXX", NULL);
    g_assert_nonnull(directory);
    image_path = g_build_filename(directory, "unused.bin", NULL);
    g_assert_true(g_file_set_contents(image_path, (char *)image,
                                      sizeof(image), NULL));
    qtest_add_data_func("/tcg/io-recompile/metadata/single",
                        "-accel tcg,thread=single", test_metadata);
    qtest_add_data_func("/tcg/io-recompile/metadata/multicpu",
                        "-smp 2 -accel tcg,thread=multi", test_metadata);
    qtest_add_data_func("/tcg/io-recompile/metadata/icount",
                        "-accel tcg,thread=single "
                        "-icount shift=3,align=off,sleep=off", test_metadata);
    qtest_add_data_func("/tcg/io-recompile/execution/taken-delay-irq",
                        GINT_TO_POINTER(1), test_execution);
    qtest_add_data_func("/tcg/io-recompile/execution/untaken-delay-irq",
                        GINT_TO_POINTER(0), test_execution);
    qtest_add_func("/tcg/io-recompile/execution/concurrent-invalidate-flush",
                    test_concurrent);
    int result = g_test_run();

    g_assert_cmpint(unlink(image_path), ==, 0);
    g_assert_cmpint(rmdir(directory), ==, 0);
    g_free(image_path);
    return result;
}
