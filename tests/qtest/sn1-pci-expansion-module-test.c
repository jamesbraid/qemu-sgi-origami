/* SPDX-License-Identifier: BSD-3-Clause */
/* Synthetic placement and head pairing, without a compute NI data route. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/misc/iommu-testdev.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-migrate.h"

#define CHASSIS "/machine/peripheral/wrap/board"
#define FIXTURE "-M none -cpu R10000 -S " \
    "-device sgi-board-test-fixture,id=wrap," \
    "board-type=sgi-origin300-pci-expansion-module"
#define POPULATION ",bedrock-revision=2,xbridge-revision=2," \
    "placement=42,node-number=7,bus-prefix=synthetic-"
#define PAIRING ",chip1-e-port=8,chip1-f-port=9," \
    "chip2-e-port=0xc,chip2-f-port=0xd"
#define REGISTERS 0x10000000ULL
#define PIO 0x100000000ULL
#define EMPTY_MEMORY 0x20000000ULL
#define VIEW(i) (0x2000000000000ULL + ((uint64_t)(i) << 44))
#define HEAD_ID 0x2d002049
#define XBOW_ID 0x2d000001

static QTestState *chassis_start(const char *options, const char *extra)
{
    QTestState *qts = qtest_initf(FIXTURE "%s %s", options, extra);
    unsigned i;

    qtest_sysbus_mmio_map(qts, CHASSIS, 0, REGISTERS);
    qtest_sysbus_mmio_map(qts, CHASSIS, 1, PIO);
    qtest_sysbus_mmio_map(qts, CHASSIS, 2, EMPTY_MEMORY);
    for (i = 0; i < 8; i++) {
        qtest_sysbus_mmio_map(qts, CHASSIS, 3 + i, VIEW(i));
    }
    return qts;
}

static int64_t qom_number(QTestState *qts, const char *path,
                          const char *property)
{
    QDict *reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, property);
    int64_t result = qdict_get_int(reply, "return");

    qobject_unref(reply);
    return result;
}

static char *qom_string(QTestState *qts, const char *path, const char *property)
{
    QDict *reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, property);
    char *result = g_strdup(qdict_get_str(reply, "return"));

    qobject_unref(reply);
    return result;
}

static char *ram_blocks(QTestState *qts)
{
    QDict *reply = qtest_qmp(qts,
        "{'execute':'human-monitor-command',"
        "'arguments':{'command-line':'info ramblock'}}");
    char *result = g_strdup(qdict_get_str(reply, "return"));

    qobject_unref(reply);
    return result;
}

static void test_population(void)
{
    QTestState *baseline = qtest_init("-M none -cpu R10000 -S");
    g_autofree char *before = ram_blocks(baseline);
    QTestState *qts = chassis_start(POPULATION PAIRING, "");
    g_autofree char *after = ram_blocks(qts);
    g_autofree char *memory = qom_string(qts, CHASSIS "/bic", "node-memory");
    g_autofree char *uart = qom_string(qts, CHASSIS "/bic", "l1-uart");
    QDict *before_cpus = qtest_qmp(baseline,
                                  "{'execute':'query-cpus-fast'}");
    QDict *reply = qtest_qmp(qts, "{'execute':'query-cpus-fast'}");
    unsigned bank;

    g_assert_cmpuint(qlist_size(qdict_get_qlist(reply, "return")), ==,
                    qlist_size(qdict_get_qlist(before_cpus, "return")));
    qobject_unref(before_cpus);
    qobject_unref(reply);
    g_assert_cmpstr(before, ==, after);
    g_assert_cmpstr(memory, ==, "");
    g_assert_cmpstr(uart, ==, "");
    g_assert_cmpint(qom_number(qts, CHASSIS "/bic", "processor-count"), ==, 0);
    g_assert_cmpint(qom_number(qts, CHASSIS "/bic", "processor-mask"), ==, 0);
    g_assert_cmpint(qom_number(qts, CHASSIS "/bic", "installed-bank-mask"),
                    ==, 0);
    for (bank = 0; bank < 8; bank++) {
        g_autofree char *name = g_strdup_printf("installed-bank%u-mib", bank);

        g_assert_cmpint(qom_number(qts, CHASSIS "/bic", name), ==, 0);
    }
    reply = qtest_qmp(qts, "{'execute':'qom-list-types',"
        "'arguments':{'implements':'sgi-sn1-fabric'}}");
    {
        const QListEntry *entry;
        bool found = false;

        QLIST_FOREACH_ENTRY(qdict_get_qlist(reply, "return"), entry) {
            QDict *type = qobject_to(QDict, qlist_entry_obj(entry));

            found |= g_str_equal(qdict_get_str(type, "name"),
                                 "sgi-origin300-pci-expansion-module");
        }
        g_assert_true(found);
    }
    qobject_unref(reply);
    qtest_quit(qts);
    qtest_quit(baseline);
}

static void test_register_views(void)
{
    QTestState *qts = chassis_start(POPULATION PAIRING, "");
    const unsigned widget[] = { 0xe, 0xf, 8, 9, 0xc, 0xd };
    unsigned i;

    g_assert_cmphex(qtest_readl(qts, PIO + 4), ==, XBOW_ID);
    for (i = 0; i < 6; i++) {
        uint64_t face = PIO + ((uint64_t)widget[i] << 24);

        g_assert_cmphex(qtest_readl(qts, face + 4), ==, HEAD_ID);
        g_assert_cmphex(qtest_readl(qts, VIEW(i) + 4), ==, HEAD_ID);
        qtest_writel(qts, face + 0x2c, 0x12340 + i);
        qtest_writel(qts, face + 0x4c, 0x123400 + i);
    }
    for (i = 0; i < 6; i++) {
        g_assert_cmphex(qtest_readl(qts, VIEW(i) + 0x2c), ==, 0x12340 + i);
        g_assert_cmphex(qtest_readl(qts, VIEW(i) + 0x4c), ==, 0x123400 + i);
        g_assert_cmphex(qtest_readl(qts, VIEW(i) + 0x20000), ==, UINT32_MAX);
    }
    g_assert_cmphex(qtest_readl(qts, VIEW(6) + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, VIEW(7) + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PIO + (0xbULL << 24) + 4), ==, 0);
    qtest_quit(qts);
}

static void test_explicit_pairing(void)
{
    QTestState *qts = chassis_start(POPULATION
        ",chip1-e-port=0xd,chip1-f-port=0xc,chip2-e-port=9,chip2-f-port=8", "");
    const unsigned widget[] = { 0xd, 0xc, 9, 8 };
    unsigned i;

    for (i = 0; i < 4; i++) {
        qtest_writel(qts, VIEW(2 + i) + 0x2c, 0x45670 + i);
    }
    for (i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(qts, PIO + ((uint64_t)widget[i] << 24) +
                                   0x2c), ==, 0x45670 + i);
    }
    qtest_quit(qts);
}

static void test_failed_dma(void)
{
    QTestState *qts = chassis_start(POPULATION PAIRING,
        "-device iommu-testdev,bus=synthetic-u0-head-e.0,addr=1 "
        "-device iommu-testdev,bus=synthetic-u0-head-f.0,addr=1 "
        "-device iommu-testdev,bus=synthetic-u1-head-e.0,addr=1 "
        "-device iommu-testdev,bus=synthetic-u1-head-f.0,addr=1 "
        "-device iommu-testdev,bus=synthetic-u2-head-e.0,addr=1 "
        "-device iommu-testdev,bus=synthetic-u2-head-f.0,addr=1");
    unsigned i;

    qtest_writeq(qts, EMPTY_MEMORY + 0x8000, 0x123456789abcdef0ULL);
    g_assert_cmphex(qtest_readq(qts, EMPTY_MEMORY + 0x8000), ==, 0);
    for (i = 0; i < 6; i++) {
        uint64_t head = VIEW(i), card = head + 0x400000;

        qtest_writel(qts, head + 0x21010, 0x01200000);
        qtest_writel(qts, head + 0x21004, 6);
        qtest_writel(qts, head + 0x20c, 0x81012);
        qtest_writel(qts, head + 0x84, 0xa << 20);
        qtest_writel(qts, card + ITD_REG_DMA_GVA_LO, 0x80008000);
        qtest_writel(qts, card + ITD_REG_DMA_GVA_HI, 0);
        qtest_writel(qts, card + ITD_REG_DMA_LEN, 8);
        qtest_writel(qts, card + ITD_REG_DMA_DBELL, ITD_DMA_DBELL_ARM);
        qtest_readl(qts, card + ITD_REG_DMA_TRIGGERING);
        g_assert_cmphex(qtest_readl(qts, card + ITD_REG_DMA_RESULT), ==,
                        ITD_DMA_ERR_TX_FAIL);
    }
    qtest_quit(qts);
}

static void test_reset(void)
{
    QTestState *qts = chassis_start(POPULATION PAIRING, "");
    unsigned i;

    for (i = 0; i < 6; i++) {
        qtest_writel(qts, VIEW(i) + 0x24, 0x0f003208 + i);
        qtest_writel(qts, VIEW(i) + 0x4c, 0x123400 + i);
    }
    qtest_system_reset(qts);
    for (i = 0; i < 6; i++) {
        g_assert_cmphex(qtest_readl(qts, VIEW(i) + 0x24) & 0xf, ==, 0xf);
        g_assert_cmphex(qtest_readl(qts, VIEW(i) + 0x4c), ==, 0x03ff1810);
    }
    g_assert_cmpint(qom_number(qts, CHASSIS "/bic", "processor-count"), ==, 0);
    g_assert_cmpint(qom_number(qts, CHASSIS "/bic", "installed-bank-mask"),
                    ==, 0);
    qtest_quit(qts);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("pci-expansion-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from = chassis_start(POPULATION PAIRING, "");
    QTestState *to = chassis_start(POPULATION PAIRING, "-incoming defer");
    unsigned i;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    for (i = 0; i < 6; i++) {
        qtest_writel(from, VIEW(i) + 0x24, 0x0f003208 + i);
        qtest_writel(from, VIEW(i) + 0x2c, 0x12340 + i);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    for (i = 0; i < 6; i++) {
        g_assert_cmphex(qtest_readl(to, VIEW(i) + 0x24) & 0xf, ==, 8 + i);
        g_assert_cmphex(qtest_readl(to, VIEW(i) + 0x2c), ==, 0x12340 + i);
    }
    g_assert_cmphex(qtest_readl(to, VIEW(6) + 4), ==, 0);
    g_assert_cmphex(qtest_readl(to, VIEW(7) + 4), ==, 0);
    g_assert_cmpint(qom_number(to, CHASSIS "/bic", "installed-bank-mask"),
                    ==, 0);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    rmdir(dir);
}

static void test_invalid_revision(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init(FIXTURE
            ",bedrock-revision=1,xbridge-revision=2,placement=42,node-number=7,"
            "bus-prefix=synthetic-" PAIRING);

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*explicit inferred revision 2*");
}

static void test_missing_revision(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init(FIXTURE PAIRING);

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*explicit inferred revision 2*");
}

static void test_missing_placement(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init(FIXTURE
            ",bedrock-revision=2,xbridge-revision=2,bus-prefix=synthetic-"
            PAIRING);

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*explicit placement and node number below 128*");
}

static void test_missing_pairing(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init(FIXTURE POPULATION);

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*needs an explicit U0 port*");
}

static void test_duplicate_pairing(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init(FIXTURE POPULATION
            ",chip1-e-port=8,chip1-f-port=9,chip2-e-port=0xc,chip2-f-port=8");

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*head links share U0 port 8*");
}

static void test_unwired_port(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init(FIXTURE POPULATION
            ",chip1-e-port=8,chip1-f-port=9,chip2-e-port=0xc,chip2-f-port=0xb");

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*needs an explicit U0 port from 8, 9, c or d*");
}

static void test_invalid_placement(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init(FIXTURE
            ",bedrock-revision=2,xbridge-revision=2,placement=128,node-number=7,"
            "bus-prefix=synthetic-" PAIRING);

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*explicit placement and node number below 128*");
}

static void test_missing_bus_prefix(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init(FIXTURE
            ",bedrock-revision=2,xbridge-revision=2,placement=42,node-number=7"
            PAIRING);

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*needs a bus-prefix for its six buses*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn1/pci-expansion/population", test_population);
    qtest_add_func("/sn1/pci-expansion/register-views", test_register_views);
    qtest_add_func("/sn1/pci-expansion/explicit-pairing",
                   test_explicit_pairing);
    qtest_add_func("/sn1/pci-expansion/failed-dma", test_failed_dma);
    qtest_add_func("/sn1/pci-expansion/reset", test_reset);
    qtest_add_func("/sn1/pci-expansion/migration", test_migration);
    qtest_add_func("/sn1/pci-expansion/invalid-revision",
                   test_invalid_revision);
    qtest_add_func("/sn1/pci-expansion/missing-revision",
                   test_missing_revision);
    qtest_add_func("/sn1/pci-expansion/missing-placement",
                   test_missing_placement);
    qtest_add_func("/sn1/pci-expansion/missing-pairing", test_missing_pairing);
    qtest_add_func("/sn1/pci-expansion/duplicate-pairing",
                   test_duplicate_pairing);
    qtest_add_func("/sn1/pci-expansion/unwired-port", test_unwired_port);
    qtest_add_func("/sn1/pci-expansion/invalid-placement",
                   test_invalid_placement);
    qtest_add_func("/sn1/pci-expansion/missing-bus-prefix",
                   test_missing_bus_prefix);
    return g_test_run();
}
