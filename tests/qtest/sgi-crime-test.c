/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-migrate.h"

#define CRIME_PATH "/machine/peripheral/crime"
#define CRIME_BASE 0x14000000ULL
#define CRIME_ID (CRIME_BASE + 0x00)
#define CRIME_CONTROL (CRIME_BASE + 0x08)
#define CRIME_STATUS (CRIME_BASE + 0x10)
#define CRIME_ENABLE (CRIME_BASE + 0x18)
#define CRIME_SOFTWARE (CRIME_BASE + 0x20)
#define CRIME_HARDWARE (CRIME_BASE + 0x28)
#define CRIME_MEMORY_CONTROL (CRIME_BASE + 0x200)
#define CRIME_CPU_ERROR_STATUS (CRIME_BASE + 0x4048)
#define CRIME_BANK(i) (CRIME_BASE + 0x208 + 8 * (i))
#define BIT64(n) (1ULL << (n))
#define CRIME_FITTED_RAM (64ULL * 1024 * 1024)
#define CRIME_FULL_RAM 0x40000000ULL
#define CRIME_NO_ECC_RAM 0x80000000ULL

static QTestState *crime_start(const char *extra)
{
    QTestState *s = qtest_initf(MEMTX_TEST_DEVICE "-M none -cpu R10000 "
        "-S -device sgi-crime,id=crime,interface-revision=1 %s", extra);

    qtest_sysbus_mmio_map(s, CRIME_PATH, 0, CRIME_BASE);
    return s;
}

static QTestState *crime_start_with_ram(const char *extra)
{
    QTestState *s = qtest_initf(MEMTX_TEST_DEVICE "-M none -cpu R10000 "
        "-S -object memory-backend-ram,id=fit,size=64M "
        "-device sgi-crime,id=crime,interface-revision=1,"
        "ram=/objects/fit/fit[0] %s", extra);

    qtest_sysbus_mmio_map(s, CRIME_PATH, 0, CRIME_BASE);
    return s;
}

static void mace(QTestState *s, unsigned source, bool level)
{
    qtest_set_irq_in(s, CRIME_PATH, "mace", source, level);
}

static void assert_cpu_ip(QTestState *s, unsigned expected)
{
    for (unsigned output = 0; output < 5; output++) {
        g_assert_cmpint(qtest_get_irq(s, output), ==,
                        !!(expected & (1U << output)));
    }
}

static void test_reset_and_access(void)
{
    QTestState *s = crime_start("");
    uint64_t value;
    uint32_t narrow;

    g_assert_cmphex(qtest_readq(s, CRIME_ID), ==, 0xb1);
    g_assert_cmphex(qtest_readq(s, CRIME_CONTROL), ==, 0x120);
    g_assert_cmphex(qtest_readq(s, CRIME_ENABLE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_SOFTWARE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_HARDWARE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_STATUS), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_CONTROL), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_CPU_ERROR_STATUS), ==, 0);
    for (unsigned bank = 0; bank < 8; bank++) {
        g_assert_cmphex(qtest_readq(s, CRIME_BANK(bank)), ==, bank);
        g_assert_cmpuint(memtx_writeq(s, CRIME_BANK(bank), 0x100 | bank),
                         !=, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, CRIME_BANK(bank)), ==, bank);
    }
    g_assert_cmpuint(memtx_readl(s, CRIME_ID, &narrow), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readq(s, CRIME_ID + 4, &value), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writel(s, CRIME_ENABLE, 1U << 4), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writel(s, CRIME_SOFTWARE, 1U << 28),
                     !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_ENABLE + 4, BIT64(4)),
                     !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_ENABLE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_SOFTWARE), ==, 0);
    g_assert_cmpuint(memtx_writeq(s, CRIME_CONTROL, BIT64(9)), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_CONTROL, BIT64(11)), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_CONTROL), ==, 0x120);
    g_assert_cmpuint(memtx_writeq(s, CRIME_MEMORY_CONTROL, 1), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_MEMORY_CONTROL), ==, 0);
    g_assert_cmpuint(memtx_writeq(s, CRIME_HARDWARE, 1), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readq(s, CRIME_BASE + 0x48, &value), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_readq(s, CRIME_BASE + 0x100, &value), !=, MEMTX_OK);
    g_assert_cmpuint(memtx_writeq(s, CRIME_BASE + 0x100, 0), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_ENABLE), ==, 0);
    qtest_quit(s);
}

static void test_mace_level_routing(void)
{
    const struct {
        unsigned source;
        unsigned output;
    } cases[] = {
        { 0, 1 }, /* IP3 */
        { 1, 1 },
        { 2, 1 },
        { 3, 0 }, /* IP2 */
        { 4, 0 }, /* IP2 */
        { 5, 0 },
        { 8, 1 }, /* IP3 */
        { 9, 1 },
        { 10, 1 },
        { 11, 1 },
        { 12, 1 },
        { 13, 1 },
        { 14, 1 },
        { 15, 1 },
        { 6, 3 }, /* IP5 */
        { 7, 4 }, /* IP6 */
    };
    QTestState *s = crime_start("");

    qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        uint64_t bit = BIT64(cases[i].source);

        mace(s, cases[i].source, true);
        g_assert_cmphex(qtest_readq(s, CRIME_HARDWARE), ==, bit);
        g_assert_cmphex(qtest_readq(s, CRIME_STATUS), ==, bit);
        assert_cpu_ip(s, 0);
        qtest_writeq(s, CRIME_ENABLE, bit);
        assert_cpu_ip(s, 1U << cases[i].output);
        qtest_writeq(s, CRIME_ENABLE, 0);
        assert_cpu_ip(s, 0);
        g_assert_cmphex(qtest_readq(s, CRIME_HARDWARE), ==, bit);
        mace(s, cases[i].source, false);
        g_assert_cmphex(qtest_readq(s, CRIME_STATUS), ==, 0);
    }
    qtest_writeq(s, CRIME_ENABLE, BIT64(8) | BIT64(9));
    mace(s, 8, true);
    mace(s, 9, true);
    assert_cpu_ip(s, 1U << 1);
    mace(s, 8, false);
    assert_cpu_ip(s, 1U << 1);
    mace(s, 9, false);
    assert_cpu_ip(s, 0);
    qtest_quit(s);
}

static void test_software_interrupts(void)
{
    QTestState *s = crime_start("");

    qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
    qtest_writeq(s, CRIME_SOFTWARE, BIT64(28) | BIT64(29));
    g_assert_cmphex(qtest_readq(s, CRIME_STATUS), ==,
                    BIT64(28) | BIT64(29));
    assert_cpu_ip(s, 0);
    qtest_writeq(s, CRIME_ENABLE, BIT64(28) | BIT64(29));
    assert_cpu_ip(s, (1U << 2) | (1U << 3));
    g_assert_cmpuint(memtx_writeq(s, CRIME_ENABLE, BIT64(27)),
                     !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_ENABLE), ==,
                    BIT64(28) | BIT64(29));
    qtest_writeq(s, CRIME_SOFTWARE, BIT64(29));
    assert_cpu_ip(s, 1U << 3);
    g_assert_cmpuint(memtx_writeq(s, CRIME_SOFTWARE, BIT64(27)),
                     !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, CRIME_SOFTWARE), ==, BIT64(29));
    qtest_writeq(s, CRIME_SOFTWARE, 0);
    assert_cpu_ip(s, 0);
    qtest_quit(s);
}

static void test_reset_pending_irq(void)
{
    QTestState *s = crime_start("");

    qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
    qtest_writeq(s, CRIME_ENABLE, BIT64(4) | BIT64(28));
    qtest_writeq(s, CRIME_SOFTWARE, BIT64(28));
    mace(s, 4, true);
    assert_cpu_ip(s, (1U << 0) | (1U << 2));
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, CRIME_ENABLE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_SOFTWARE), ==, 0);
    g_assert_cmphex(qtest_readq(s, CRIME_HARDWARE), ==, BIT64(4));
    assert_cpu_ip(s, 0);
    mace(s, 4, false);
    g_assert_cmphex(qtest_readq(s, CRIME_HARDWARE), ==, 0);
    qtest_quit(s);
}

static void test_pending_irq_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("crime-irq-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = crime_start("");
    to = crime_start("-incoming defer");
    qtest_irq_intercept_out_named(to, CRIME_PATH, "cpu-ip");
    qtest_writeq(from, CRIME_ENABLE, BIT64(4) | BIT64(29));
    qtest_writeq(from, CRIME_SOFTWARE, BIT64(29));
    mace(from, 4, true);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readq(to, CRIME_HARDWARE), ==, BIT64(4));
    g_assert_cmphex(qtest_readq(to, CRIME_SOFTWARE), ==, BIT64(29));
    assert_cpu_ip(to, (1U << 0) | (1U << 3));
    mace(to, 4, false);
    assert_cpu_ip(to, 1U << 3);
    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(directory);
}

static void test_ram_aliases_and_holes(void)
{
    QTestState *s = crime_start_with_ram("");
    const uint64_t bases[] = { 0, CRIME_FULL_RAM, CRIME_NO_ECC_RAM };
    const uint64_t pattern[] = {
        0x8e1a2b3c4d5e6f70ULL,
        0x1534a5c6d7e8f901ULL,
        0xfedcba9876543210ULL,
    };
    uint8_t byte;

    /* No-ECC is a data alias here; ECC checking is not modeled. */
    for (unsigned source = 0; source < ARRAY_SIZE(bases); source++) {
        qtest_writeq(s, bases[source] + 0x100, pattern[source]);
        for (unsigned view = 0; view < ARRAY_SIZE(bases); view++) {
            g_assert_cmphex(qtest_readq(s, bases[view] + 0x100), ==,
                            pattern[source]);
        }
    }
    for (unsigned source = 0; source < ARRAY_SIZE(bases); source++) {
        qtest_writeb(s, bases[source] + CRIME_FITTED_RAM - 1,
                     0xa0 + source);
        for (unsigned view = 0; view < ARRAY_SIZE(bases); view++) {
            g_assert_cmphex(qtest_readb(s, bases[view] +
                                            CRIME_FITTED_RAM - 1), ==,
                            0xa0 + source);
        }
    }
    for (unsigned view = 0; view < ARRAY_SIZE(bases); view++) {
        g_assert_cmpuint(memtx_readb(s, bases[view] + CRIME_FITTED_RAM,
                                      &byte), !=, MEMTX_OK);
        g_assert_cmpuint(memtx_writeb(s, bases[view] + CRIME_FITTED_RAM,
                                       0xff), !=, MEMTX_OK);
    }
    g_assert_cmpuint(memtx_readb(s, 0x20000000, &byte), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readb(s, CRIME_FITTED_RAM - 1), ==, 0xa2);
    for (unsigned bank = 0; bank < 8; bank++) {
        g_assert_cmphex(qtest_readq(s, CRIME_BANK(bank)), ==, bank);
        g_assert_cmpuint(memtx_writeq(s, CRIME_BANK(bank), 0x100 | bank),
                         !=, MEMTX_OK);
    }
    qtest_quit(s);
}

static void test_ram_alias_mapping_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("crime-ram-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    uint8_t byte;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = crime_start_with_ram("");
    to = crime_start_with_ram("-incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    /* -M none does not register the backing RAM for content migration. */
    qtest_writeq(to, CRIME_FULL_RAM + 0x200, 0x9264af731d0be5c8ULL);
    g_assert_cmphex(qtest_readq(to, 0x200), ==,
                    0x9264af731d0be5c8ULL);
    g_assert_cmphex(qtest_readq(to, CRIME_NO_ECC_RAM + 0x200), ==,
                    0x9264af731d0be5c8ULL);
    g_assert_cmpuint(memtx_readb(to, CRIME_FULL_RAM + CRIME_FITTED_RAM,
                                  &byte), !=, MEMTX_OK);
    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(directory);
}

static void test_ram_unsupported_fit(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*requires a 64 MiB RAM region*");
        return;
    }
    s = qtest_init(MEMTX_TEST_DEVICE "-M none -cpu R10000 -S "
                   "-object memory-backend-ram,id=fit,size=128M "
                   "-device sgi-crime,id=crime,interface-revision=1,"
                   "ram=/objects/fit/fit[0]");
    qtest_quit(s);
}

static void test_ram_readonly_fit(void)
{
    GError *error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *path = NULL;
    QTestState *s;
    int fd;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*requires writable RAM*");
        return;
    }
    directory = g_dir_make_tmp("crime-rom-XXXXXX", &error);
    g_assert_no_error(error);
    path = g_build_filename(directory, "backing", NULL);
    fd = g_open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, CRIME_FITTED_RAM), ==, 0);
    close(fd);
    s = qtest_initf(MEMTX_TEST_DEVICE "-M none -cpu R10000 -S "
                    "-object memory-backend-file,id=rom,size=64M,"
                    "mem-path=%s,readonly=on,rom=on "
                    "-device sgi-crime,id=crime,interface-revision=1,"
                    "ram=/objects/rom/rom[0]", path);
    qtest_quit(s);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/crime/reset-access", test_reset_and_access);
    qtest_add_func("/sgi/crime/mace-level-routing", test_mace_level_routing);
    qtest_add_func("/sgi/crime/software-interrupts", test_software_interrupts);
    qtest_add_func("/sgi/crime/reset-pending-irq", test_reset_pending_irq);
    qtest_add_func("/sgi/crime/migrate-pending-irq",
                   test_pending_irq_migration);
    qtest_add_func("/sgi/crime/ram-aliases-holes",
                   test_ram_aliases_and_holes);
    qtest_add_func("/sgi/crime/ram-alias-migration",
                   test_ram_alias_mapping_migration);
    qtest_add_func("/sgi/crime/ram-unsupported-fit",
                   test_ram_unsupported_fit);
    qtest_add_func("/sgi/crime/ram-readonly-fit",
                   test_ram_readonly_fit);
    return g_test_run();
}
