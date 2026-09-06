/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone PCI head; the fixture does not represent an Origin 300. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define BASE 0x10000000ULL
#define HEAD "/machine/peripheral/head"

static QTestState *head_start(void)
{
    QTestState *qts = qtest_init("-M none -cpu R10000 -S "
                                "-device sgi-xbridge-head,id=head");

    qtest_sysbus_mmio_map(qts, HEAD, 0, BASE);
    return qts;
}

static uint32_t reg_read(QTestState *qts, unsigned off)
{
    return qtest_readl(qts, BASE + off + 4);
}

static void reg_write(QTestState *qts, unsigned off, uint32_t value)
{
    qtest_writel(qts, BASE + off + 4, value);
}

static void test_registers(void)
{
    QTestState *qts = head_start();

    g_assert_cmphex(reg_read(qts, 0), ==, 0x2d002049);
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0x0f00320f);
    g_assert_cmphex(reg_read(qts, 0x80), ==, 0x20000);
    reg_write(qts, 0x80, 0xa40025);
    g_assert_cmphex(reg_read(qts, 0x80), ==, 0xa00025);
    reg_write(qts, 0x80, 0xb00013);
    g_assert_cmphex(reg_read(qts, 0x80), ==, 0xb00013);
    reg_write(qts, 0x20, 0x0f00320e);
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0x0f00320e);
    reg_write(qts, 0x108, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x108), ==, 0x7ffefeff);
    reg_write(qts, 0x200, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x200), ==, 0x1eefffff);
    qtest_quit(qts);
}

static void test_ate(void)
{
    QTestState *qts = head_start();
    const unsigned indices[] = { 0, 127, 128, 511 };
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(indices); i++) {
        unsigned off = indices[i] * 8;
        uint64_t value = 0xabcd123456780021ULL + (i << 12);

        qtest_writeq(qts, BASE + 0x10000 + off, value);
        g_assert_cmphex(qtest_readl(qts, BASE + 0x10004 + off), ==,
                        (value >> 32) & 0xffff);
        g_assert_cmphex(qtest_readl(qts, BASE + 0x11004 + off), ==,
                        (uint32_t)value);
        qtest_writeq(qts, BASE + 0x11000 + off, 0);
        g_assert_cmphex(qtest_readl(qts, BASE + 0x11004 + off), ==,
                        (uint32_t)value);
    }
    qtest_quit(qts);
}

static void test_config_pio(void)
{
    QTestState *qts = qtest_init("-M none -cpu R10000 -S "
        "-device sgi-xbridge-head,id=head "
        "-device sgi-bridge-dma-test,bus=head.0,addr=0");
    uint8_t bytes[4] = { 0x67, 0x45, 0x23, 0x01 };
    uint8_t actual[4];

    qtest_sysbus_mmio_map(qts, HEAD, 0, BASE);
    /* An actual PCI device must answer, including mutable BAR decoding. */
    g_assert_cmphex(qtest_readl(qts, BASE + 0x20000), ==, 0x00051b36);
    qtest_writel(qts, BASE + 0x20010, 0x01000000);
    qtest_writel(qts, BASE + 0x20004, 6);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x20010), ==, 0x01000000);
    reg_write(qts, 0x200, 0x3010);
    qtest_memwrite(qts, BASE + 0x200000, bytes, sizeof(bytes));
    qtest_memread(qts, BASE + 0x200000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), bytes, sizeof(bytes));
    bytes[0] = 0x89;
    qtest_memwrite(qts, BASE + 0x200000, bytes, sizeof(bytes));
    qtest_memread(qts, BASE + 0x200000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), bytes, sizeof(bytes));
    qtest_quit(qts);
}

static void test_config_type1(void)
{
    QTestState *qts = qtest_init("-M none -cpu R10000 -S "
        "-device sgi-xbridge-head,id=head "
        "-device pci-bridge,id=down,bus=head.0,addr=1,chassis_nr=1 "
        "-device sgi-bridge-dma-test,bus=down,addr=2");

    qtest_sysbus_mmio_map(qts, HEAD, 0, BASE);
    qtest_writel(qts, BASE + 0x21018, 0x00010100);
    reg_write(qts, 0xc8, 0x00011000);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x28000), ==, 0x00051b36);
    qtest_writel(qts, BASE + 0x28010, 0x01200000);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x28010), ==, 0x01200000);
    reg_write(qts, 0xc8, 0x00011800);
    qtest_readl(qts, BASE + 0x28000);
    g_assert_cmphex(reg_read(qts, 0x170) & (1U << 11), ==, 1U << 11);
    reg_write(qts, 0x110, 1U << 6);
    g_assert_cmphex(reg_read(qts, 0x170), ==, 0);
    qtest_quit(qts);
}

#define TARGET_A 0x20000000ULL
#define TARGET_B 0x21000000ULL
#define TARGET_C 0x123400000000ULL
#define SCRATCH 0x8000

static QTestState *transport_start(const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors "
        "-device sgi-xtalk-test-target,id=targeta "
        "-device sgi-xtalk-test-target,id=targetb "
        "-device sgi-xtalk-test-target,id=targetc "
        "-device sgi-xbridge-head,id=head,"
        "target-0=/machine/peripheral/targeta/targeta.ram[0],"
        "target-a=/machine/peripheral/targeta/targeta.ram[0],"
        "target-b=/machine/peripheral/targetb/targetb.ram[0],"
        "target-c=/machine/unattached/system[0] "
        "-device sgi-bridge-dma-test,bus=head.0,addr=0,multifunction=on "
        "%s", extra);

    qtest_sysbus_mmio_map(qts, HEAD, 0, BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/targeta", 0, TARGET_A);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/targetb", 0, TARGET_B);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/targetc", 0, TARGET_C);
    qtest_writel(qts, BASE + 0x20010, 0x01000000);
    qtest_writel(qts, BASE + 0x20004, 6);
    reg_write(qts, 0x200, 0x83010);
    return qts;
}

static void card_write(QTestState *qts, unsigned reg, uint32_t value)
{
    uint8_t bytes[4] = { value, value >> 8, value >> 16, value >> 24 };

    qtest_memwrite(qts, BASE + 0x200000 + reg, bytes, sizeof(bytes));
}

static void dma_fill(QTestState *qts, uint64_t address, uint8_t seed)
{
    card_write(qts, 0, address);
    card_write(qts, 0x18, address >> 32);
    card_write(qts, 4, 8);
    card_write(qts, 8, seed);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x200004), ==, 0x08000000);
    card_write(qts, 0xc, 1);
}

static void test_dma_routes(void)
{
    QTestState *qts = transport_start("");
    const uint64_t rejected[] = { 0, 0x8000, 0x3fff8000, 0x40008000,
        0x100008000ULL, 0x0080000000008000ULL,
        0x0fffffffffff8000ULL, 0xd080000000008000ULL };
    unsigned i;

    qtest_writeq(qts, TARGET_A + SCRATCH, 0xfeedfacecafebeefULL);
    qtest_writeq(qts, TARGET_B + SCRATCH, 0x1122334455667788ULL);
    reg_write(qts, 0x80, 0xa00000);
    dma_fill(qts, 0x80008000, 0x10);
    g_assert_cmphex(qtest_readq(qts, TARGET_A + SCRATCH), ==,
                    0x1011121314151617ULL);
    g_assert_cmphex(qtest_readq(qts, TARGET_B + SCRATCH), ==,
                    0x1122334455667788ULL);
    dma_fill(qts, 0xb080000000008000ULL, 0x20);
    g_assert_cmphex(qtest_readq(qts, TARGET_B + SCRATCH), ==,
                    0x2021222324252627ULL);
    dma_fill(qts, 0xc080123400008000ULL, 0x30);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==,
                    0x3031323334353637ULL);
    qtest_writeq(qts, TARGET_C + SCRATCH, 0xabcdef0123456789ULL);
    reg_write(qts, 0x80, 0xc22468);
    dma_fill(qts, 0x80008000, 0x30);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==,
                    0x3031323334353637ULL);
    reg_write(qts, 0x80, 0xc20000);
    dma_fill(qts, 0x80008000, 0x10);
    g_assert_cmphex(qtest_readq(qts, TARGET_A + SCRATCH), ==,
                    0x1011121314151617ULL);
    reg_write(qts, 0x80, 0xa00000);
    card_write(qts, 0x14, 0x8000);
    card_write(qts, 0x1c, 0xa0800000);
    card_write(qts, 0, 0x8000);
    card_write(qts, 0x18, 0xb0800000);
    card_write(qts, 0xc, 2);
    g_assert_cmphex(qtest_readq(qts, TARGET_B + SCRATCH), ==,
                    0x1011121314151617ULL);
    for (i = 0; i < G_N_ELEMENTS(rejected); i++) {
        dma_fill(qts, rejected[i], 0x80);
        g_assert_cmphex(qtest_readq(qts, TARGET_A + SCRATCH), ==,
                        0x1011121314151617ULL);
        g_assert_cmphex(qtest_readq(qts, TARGET_B + SCRATCH), ==,
                        0x1011121314151617ULL);
        g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==,
                        0x3031323334353637ULL);
    }
    qtest_quit(qts);
}

static void test_interrupt_packets(void)
{
    QTestState *qts = transport_start("");

    reg_write(qts, 0x30, 0x000cabf4);
    reg_write(qts, 0x38, 0x8000);
    reg_write(qts, 0x148, 0x4885);
    qtest_writeq(qts, TARGET_C + SCRATCH, 0x123456789abcdef0ULL);
    qtest_set_irq_in(qts, HEAD, NULL, 3, 1);
    g_assert_cmphex(reg_read(qts, 0x100) & 8, ==, 8);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==,
                    0x123456789abcdef0ULL);
    reg_write(qts, 0x108, 8);
    qtest_set_irq_in(qts, HEAD, NULL, 3, 0);
    qtest_set_irq_in(qts, HEAD, NULL, 3, 1);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==, 0x185);
    reg_write(qts, 0x118, 8);
    qtest_set_irq_in(qts, HEAD, NULL, 3, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==, 0x85);
    reg_write(qts, 0x1d8, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==, 0x85);
    reg_write(qts, 0x198, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==, 0x185);
    reg_write(qts, 0x118, 0);
    qtest_set_irq_in(qts, HEAD, NULL, 3, 1);
    qtest_writeq(qts, TARGET_C + SCRATCH, 0xaaaaaaaa55555555ULL);
    qtest_set_irq_in(qts, HEAD, NULL, 3, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==,
                    0xaaaaaaaa55555555ULL);
    qtest_quit(qts);
}

static void test_byte_lanes(void)
{
    QTestState *qts = transport_start("");
    uint8_t actual[8];
    const uint8_t swapped[] = { 0x43, 0x42, 0x41, 0x40,
                                0x47, 0x46, 0x45, 0x44 };

    reg_write(qts, 0x80, 0xa00000);
    reg_write(qts, 0x200, 0x3010);
    dma_fill(qts, 0x80008000, 0x40);
    qtest_memread(qts, TARGET_A + SCRATCH, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), swapped, sizeof(swapped));
    reg_write(qts, 0x200, 0x83010);
    dma_fill(qts, 0xa000000000008000ULL, 0x40);
    qtest_memread(qts, TARGET_A + SCRATCH, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), swapped, sizeof(swapped));
    /* BAR 1 accepts narrow cycles; BAR 0's command registers require words. */
    qtest_writel(qts, BASE + 0x20014, 0x01200000);
    reg_write(qts, 0x200, 0x481012);
    qtest_writeq(qts, BASE + 0x200008, 0x1122334455667788ULL);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x200008), ==, 0x11);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x20000f), ==, 0x88);
    reg_write(qts, 0x200, 0x483012);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x200008), ==, 0x88);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x20000f), ==, 0x11);
    qtest_quit(qts);
}

static void test_upstream_requester(void)
{
    QTestState *qts = transport_start(
        "-device pci-bridge,id=down,bus=head.0,addr=1,chassis_nr=1 "
        "-device sgi-bridge-dma-test,bus=down,addr=0x1f");

    qtest_writel(qts, BASE + 0x21018, 0x00010100);
    qtest_writel(qts, BASE + 0x21020, 0x01f00100);
    qtest_writel(qts, BASE + 0x21004, 6);
    reg_write(qts, 0xc8, 0x0001f800);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x28000), ==, 0x00051b36);
    qtest_writel(qts, BASE + 0x28010, 0x01200000);
    qtest_writel(qts, BASE + 0x28004, 6);
    /* PIO window 0 reaches the child; its PCI requester is upstream slot 1. */
    reg_write(qts, 0x200, 0x3012);
    reg_write(qts, 0x208, 0x80000);
    reg_write(qts, 0x80, 0xa00000);
    dma_fill(qts, 0x80008000, 0x50);
    g_assert_cmphex(qtest_readq(qts, TARGET_A + SCRATCH), ==,
                    0x5051525354555657ULL);
    qtest_quit(qts);
}

static void wait_migration(QTestState *qts)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) reply = qtest_qmp(qts, "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(qdict_get_qdict(reply, "return"),
                                          "status");
        if (!strcmp(status, "completed")) {
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void test_migration_reset(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-xbridge-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from = transport_start("");
    QTestState *to = transport_start("-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    reg_write(from, 0x20, 0x0f00320e);
    reg_write(from, 0x80, 0xb00000);
    reg_write(from, 0x30, 0x000cabf4);
    reg_write(from, 0x38, 0x8000);
    reg_write(from, 0x148, 0x4885);
    reg_write(from, 0x108, 8);
    reg_write(from, 0x118, 8);
    qtest_set_irq_in(from, HEAD, NULL, 3, 1);
    qtest_writeq(from, BASE + 0x10ff8, 0x123456780021ULL);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    g_assert_cmphex(reg_read(to, 0x20), ==, 0x0f00320e);
    g_assert_cmphex(reg_read(to, 0x100) & 8, ==, 8);
    g_assert_cmphex(qtest_readl(to, BASE + 0x10ffc), ==, 0x1234);
    g_assert_cmphex(qtest_readl(to, BASE + 0x11ffc), ==, 0x56780021);
    qtest_set_irq_in(to, HEAD, NULL, 3, 0);
    g_assert_cmphex(qtest_readq(to, TARGET_C + SCRATCH), ==, 0x85);
    dma_fill(to, 0x80008000, 0x60);
    g_assert_cmphex(qtest_readq(to, TARGET_B + SCRATCH), ==,
                    0x6061626364656667ULL);
    qtest_system_reset(to);
    g_assert_cmphex(reg_read(to, 0x20), ==, 0x0f00320f);
    g_assert_cmphex(reg_read(to, 0x80), ==, 0x20000);
    g_assert_cmphex(reg_read(to, 0x108), ==, 0);
    g_assert_cmphex(reg_read(to, 0x100), ==, 0);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_access_errors(void)
{
    QTestState *qts = transport_start("");

    reg_write(qts, 0x30, 0x000c1234);
    reg_write(qts, 0x38, 0x8000);
    reg_write(qts, 0x108, 1U << 24);
    qtest_writeq(qts, BASE + 0x20, 0xabcdef0123456789ULL);
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0x0f00320f);
    g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 1U << 24);
    reg_write(qts, 0x110, 1);
    g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 1U << 24);
    reg_write(qts, 0xb0, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0xb0) & 3, ==, 0);
    reg_write(qts, 0x178, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x178), ==, 0);
    qtest_quit(qts);
}

static void test_requester_bounds(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = transport_start(
            "-device sgi-bridge-dma-test,bus=head.0,addr=8");

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*slot 8 function 0 not available*reserved*");
}

static void test_notifier_rejection(void)
{
    for (unsigned flags = 1; flags <= 7; flags++) {
        QTestState *qts = qtest_initf(
            "-M none -cpu R10000 -S -device sgi-xbridge-head,id=head "
            "-device sgi-iommu-notifier-test,id=notifier,flags=%u,"
            "iommu=" HEAD "/xbridge.dma-0[0]", flags);
        QDict *reply = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{"
            "'path':'/machine/peripheral/notifier','property':'result'}}");

        g_assert_cmpint(qdict_get_int(reply, "return"), ==, -EINVAL);
        qobject_unref(reply);
        qtest_quit(qts);
    }
}

static void ate_write(QTestState *qts, unsigned index, uint64_t value)
{
    qtest_writeq(qts, BASE + 0x10000 + index * 8, value);
}

static void test_page_dma(void)
{
    QTestState *qts = transport_start("");
    const unsigned indices[] = { 0, 127, 128, 511 };

    for (unsigned shift = 12; shift <= 14; shift += 2) {
        reg_write(qts, 0x20, 0x0f00320f | (shift == 14 ? 1U << 21 : 0));
        for (unsigned i = 0; i < G_N_ELEMENTS(indices); i++) {
            ate_write(qts, indices[i], 0x8a21);
            qtest_writeq(qts, TARGET_A + 0x8238, 0);
            dma_fill(qts, 0x40000238 + ((uint64_t)indices[i] << shift),
                     0x20 + i);
            g_assert_cmphex(qtest_readq(qts, TARGET_A + 0x8238), ==,
                0x2021222324252627ULL + 0x0101010101010101ULL * i);
        }
        /* ATE target zero is valid; high address bits belong to the target. */
        ate_write(qts, 0, 0x8021);
        dma_fill(qts, 0x40000000, 0x30);
        g_assert_cmphex(qtest_readq(qts, TARGET_A + 0x8000), ==,
                       0x3031323334353637ULL);
        ate_write(qts, 0, 0xabcd123400008ce1ULL);
        dma_fill(qts, 0x40000000, 0x40);
        g_assert_cmphex(qtest_readq(qts, TARGET_C + 0x8000), ==,
                       0x4041424344454647ULL);
        /* In 16 KB mode descriptor bits 13:12 do not change the page. */
        if (shift == 14) {
            ate_write(qts, 0, 0xba21);
            dma_fill(qts, 0x40001238, 0x50);
            g_assert_cmphex(qtest_readq(qts, TARGET_A + 0x9238), ==,
                           0x5051525354555657ULL);
        }
    }
    qtest_quit(qts);
}

static void test_page_boundaries(void)
{
    QTestState *qts = transport_start("");

    for (unsigned shift = 12; shift <= 14; shift += 2) {
        uint32_t size = 1U << shift;

        reg_write(qts, 0x20, 0x0f00320f | (shift == 14 ? 1U << 21 : 0));
        ate_write(qts, 0, 0x8a21);
        ate_write(qts, 1, 0x8b01);
        dma_fill(qts, 0x40000000 + size - 4, 0x20);
        g_assert_cmphex(qtest_readl(qts, TARGET_A + 0x8000 + size - 4), ==,
                       0x20212223);
        g_assert_cmphex(qtest_readl(qts, TARGET_B + 0x8000), ==, 0x27262524);
        /* The next access must use the rewritten target, base and swap. */
        ate_write(qts, 1, 0xca21);
        dma_fill(qts, 0x40000000 + size, 0x30);
        g_assert_cmphex(qtest_readq(qts, TARGET_A + 0xc000), ==,
                       0x3031323334353637ULL);
        card_write(qts, 0x14, 0x40000000 + size);
        card_write(qts, 0x1c, 0);
        card_write(qts, 0, 0x8000);
        card_write(qts, 0x18, 0xb0800000);
        card_write(qts, 0xc, 2);
        g_assert_cmphex(qtest_readq(qts, TARGET_B + 0x8000), ==,
                       0x3031323334353637ULL);
    }
    ate_write(qts, 1, 0x8a21);
    ate_write(qts, 4, 0x8b21);
    reg_write(qts, 0x20, 0x0f00320f);
    dma_fill(qts, 0x40004000, 0x60);
    reg_write(qts, 0x20, 0x0f20320f);
    dma_fill(qts, 0x40004000, 0x70);
    g_assert_cmphex(qtest_readq(qts, TARGET_B + 0x8000), ==,
                   0x6061626364656667ULL);
    g_assert_cmphex(qtest_readq(qts, TARGET_A + 0x8000), ==,
                   0x7071727374757677ULL);
    qtest_quit(qts);
}

static void tx_write(QTestState *qts, unsigned reg, uint32_t value)
{
    uint8_t bytes[4] = { value, value >> 8, value >> 16, value >> 24 };

    qtest_memwrite(qts, BASE + 0x400000 + reg, bytes, sizeof(bytes));
}

static uint32_t tx_read(QTestState *qts, unsigned reg)
{
    uint8_t bytes[4];

    qtest_memread(qts, BASE + 0x400000 + reg, bytes, sizeof(bytes));
    return bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint32_t tx_run(QTestState *qts, uint32_t address)
{
    tx_write(qts, 4, address);
    tx_write(qts, 0xc, 8);
    tx_write(qts, 0x1c, TARGET_A + 0x8000);
    tx_write(qts, 0x14, 1);
    tx_read(qts, 0);
    return tx_read(qts, 0x10);
}

static void test_page_faults(void)
{
    QTestState *qts = transport_start(
        "-device iommu-testdev,bus=head.0,addr=1");
    const uint32_t faults[] = { 0x40000000, 0x40200000, 0x407ffff8 };
    const uint32_t unresolved[] = { 0x40800000, 0x60000000, 0x7ffffff8 };

    qtest_writel(qts, BASE + 0x21010, 0x01200000);
    qtest_writel(qts, BASE + 0x21004, 6);
    reg_write(qts, 0x208, 0x83012);
    reg_write(qts, 0x80, 0xa00000);
    g_assert_cmphex(tx_run(qts, 0x80008000), ==, 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(faults); i++) {
        qtest_writeq(qts, TARGET_A + 0x8000, 0x1122334455667788ULL);
        g_assert_cmphex(tx_run(qts, faults[i]), ==, 0xdead0002);
        g_assert_cmphex(reg_read(qts, 0x100) & (1U << 30), ==, 1U << 30);
        g_assert_cmphex(reg_read(qts, 0x170) & (1U << 30), ==, 1U << 30);
        reg_write(qts, 0x110, 2);
        g_assert_cmphex(reg_read(qts, 0x100) & (1U << 30), ==, 0);
        g_assert_cmphex(reg_read(qts, 0x170) & (1U << 30), ==, 0);
        g_assert_cmphex(qtest_readq(qts, TARGET_A + 0x8000), ==,
                       0x1122334455667788ULL);
    }
    ate_write(qts, 0, 0x8a21);
    g_assert_cmphex(tx_run(qts, 0x40000000), ==, 0);
    ate_write(qts, 0, 0x8a20);
    g_assert_cmphex(tx_run(qts, 0x40000000), ==, 0xdead0002);
    reg_write(qts, 0x110, 2);
    ate_write(qts, 0, 0x8a21);
    qtest_writeq(qts, TARGET_A + 0x8000, 0x1122334455667788ULL);
    for (unsigned i = 0; i < G_N_ELEMENTS(unresolved); i++) {
        g_assert_cmphex(tx_run(qts, unresolved[i]), ==, 0xdead0002);
        g_assert_cmphex(qtest_readq(qts, TARGET_A + 0x8000), ==,
                       0x1122334455667788ULL);
    }
    ate_write(qts, 0, 0x8d21);
    g_assert_cmphex(tx_run(qts, 0x40000000), ==, 0xdead0002);
    g_assert_cmphex(qtest_readq(qts, TARGET_A + 0x8000), ==,
                   0x1122334455667788ULL);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn1/xbridge/registers", test_registers);
    qtest_add_func("/sn1/xbridge/ate", test_ate);
    qtest_add_func("/sn1/xbridge/config-pio", test_config_pio);
    qtest_add_func("/sn1/xbridge/config-type1", test_config_type1);
    qtest_add_func("/sn1/xbridge/dma-routes", test_dma_routes);
    qtest_add_func("/sn1/xbridge/interrupt-packets", test_interrupt_packets);
    qtest_add_func("/sn1/xbridge/byte-lanes", test_byte_lanes);
    qtest_add_func("/sn1/xbridge/upstream-requester", test_upstream_requester);
    qtest_add_func("/sn1/xbridge/migration-reset", test_migration_reset);
    qtest_add_func("/sn1/xbridge/access-errors", test_access_errors);
    qtest_add_func("/sn1/xbridge/requester-bounds", test_requester_bounds);
    qtest_add_func("/sn1/xbridge/notifier-rejection", test_notifier_rejection);
    qtest_add_func("/sn1/xbridge/page-dma", test_page_dma);
    qtest_add_func("/sn1/xbridge/page-boundaries", test_page_boundaries);
    qtest_add_func("/sn1/xbridge/page-faults", test_page_faults);
    return g_test_run();
}
