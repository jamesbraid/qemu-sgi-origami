/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone PCI head; the fixture does not represent an Origin 300. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

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

    static const uint8_t window_base[8] = { 0, 2, 4, 5, 6, 7, 8, 9 };
    unsigned n;

    g_assert_cmphex(reg_read(qts, 0), ==, 0x2d002049);
    g_assert_cmphex(reg_read(qts, 8) & (1U << 5), ==, 1U << 5);
    /* The firmware's layout: two 2 MB windows, then six of 1 MB. */
    for (n = 0; n < 8; n++) {
        g_assert_cmphex(reg_read(qts, 0x200 + n * 8), ==,
                        0x18001000 | window_base[n]);
    }
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0x0f00320f);
    g_assert_cmphex(reg_read(qts, 0x80), ==, 0x20000);
    g_assert_cmphex(reg_read(qts, 0xa0), ==, 0x20008);
    g_assert_cmphex(reg_read(qts, 0xc0), ==, 0x10000);
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
    /* Carried over from the Bridge: stored with their documented fields. */
    reg_write(qts, 0x70, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x70), ==, 0xffffff);
    reg_write(qts, 0xa0, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0xa0), ==, 0x3ff7f);
    reg_write(qts, 0xc0, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0xc0), ==, 0x1f03ff);
    reg_write(qts, 0x120, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x120), ==, 0xffffff);
    reg_write(qts, 0x280, 0x12345678);
    reg_write(qts, 0x288, 0x9abcdef0);
    g_assert_cmphex(reg_read(qts, 0x280), ==, 0x12345678);
    g_assert_cmphex(reg_read(qts, 0x288), ==, 0x9abcdef0);
    qtest_quit(qts);
}

/*
 * Revision B's translation-RAM table by chip revision: 1024 entries written
 * as doublewords from 0x1_0000 to 0x1_1fff, upper words read at each entry's
 * +4, lower words read from a separate view at 0x1_2000. SGI's xbg writes
 * ~i:i to the first 512 and reads the lower word at 0x1_2004.
 */
static void test_ate(void)
{
    QTestState *qts = head_start();
    const unsigned indices[] = { 0, 1, 127, 511, 512, 1023 };
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(indices); i++) {
        unsigned off = indices[i] * 8;
        uint64_t value = 0xabcd123456780021ULL + ((uint64_t)i << 12);

        qtest_writeq(qts, BASE + 0x10000 + off, value);
        /* Bits 63:48 are unresolved and not asserted; 47:32 must hold. */
        g_assert_cmphex(qtest_readl(qts, BASE + 0x10004 + off) & 0xffff, ==,
                        (value >> 32) & 0xffff);
        g_assert_cmphex(qtest_readl(qts, BASE + 0x12004 + off), ==,
                        (uint32_t)value);
    }

    /* The upper half of the table is its own storage, not a view. */
    qtest_writeq(qts, BASE + 0x10000, 0x0000000100000002ULL);
    qtest_writeq(qts, BASE + 0x11000, 0x0000000300000004ULL);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x10004), ==, 1);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x12004), ==, 2);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x11004), ==, 3);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x13004), ==, 4);

    /*
     * The view is read-only and the RAM takes only doubleword writes: a write
     * into the view, or a word write into the RAM, is an invalid address,
     * interrupt status bit 24, and changes no entry.
     */
    g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 0);
    qtest_writeq(qts, BASE + 0x12000, 0x0000000500000006ULL);
    g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 1U << 24);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x12004), ==, 2);
    reg_write(qts, 0x110, 0xffffffff);
    g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 0);
    qtest_writel(qts, BASE + 0x11004, 7);
    g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 1U << 24);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x11004), ==, 3);
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

static void test_config_doubleword(void)
{
    QTestState *qts = qtest_init("-M none -cpu R10000 -S "
        "-device sgi-xbridge-head,id=head "
        "-device sgi-bridge-dma-test,bus=head.0,addr=0");
    uint64_t header;

    qtest_sysbus_mmio_map(qts, HEAD, 0, BASE);
    /*
     * io8bt reads a device's configuration header as one 64-bit load and
     * compares it with the two 32-bit words: identity high, command and
     * status low. Both halves must be the same words a 32-bit read gets.
     */
    qtest_writel(qts, BASE + 0x20004, 6);
    header = qtest_readq(qts, BASE + 0x20000);
    g_assert_cmphex(header >> 32, ==, qtest_readl(qts, BASE + 0x20000));
    g_assert_cmphex(header >> 32, ==, 0x00051b36);
    g_assert_cmphex((uint32_t)header, ==, qtest_readl(qts, BASE + 0x20004));
    g_assert_cmphex((uint32_t)header & 0xffff, ==, 6);
    /* A doubleword write lands as two words, in the same order. */
    qtest_writeq(qts, BASE + 0x20010, 0x0100000000000000ULL);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x20010), ==, 0x01000000);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x20014), ==, 0);
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
        "-device sgi-xio-test-target,id=targeta "
        "-device sgi-xio-test-target,id=targetb "
        "-device sgi-xio-test-target,id=targetc "
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

/*
 * The card's own scalar fields (address, count, seed, trigger) need
 * value-preserving placement through the slot's PIO window regardless of what
 * a test has bit 13 set to for its own byte-stream checks elsewhere, so clear
 * it here for the one write and restore whatever the caller had.
 */
static void card_write(QTestState *qts, unsigned reg, uint32_t value)
{
    uint32_t dev = reg_read(qts, 0x200);

    if (dev & (1U << 13)) {
        reg_write(qts, 0x200, dev & ~(1U << 13));
    }
    qtest_writel(qts, BASE + 0x200000 + reg, value);
    if (dev & (1U << 13)) {
        reg_write(qts, 0x200, dev);
    }
}

static uint32_t card_read(QTestState *qts, unsigned reg)
{
    uint32_t dev = reg_read(qts, 0x200);
    uint32_t value;

    if (dev & (1U << 13)) {
        reg_write(qts, 0x200, dev & ~(1U << 13));
    }
    value = qtest_readl(qts, BASE + 0x200000 + reg);
    if (dev & (1U << 13)) {
        reg_write(qts, 0x200, dev);
    }
    return value;
}

static void dma_fill(QTestState *qts, uint64_t address, uint8_t seed)
{
    card_write(qts, 0, address);
    card_write(qts, 0x18, address >> 32);
    card_write(qts, 4, 8);
    card_write(qts, 8, seed);
    g_assert_cmphex(card_read(qts, 4), ==, 8);
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
    reg_write(qts, 0x100, 0);
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
    /*
     * BAR 1 accepts narrow cycles; BAR 0's command registers require words.
     */
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

static void test_self_ordered_dma(void)
{
    static const uint8_t expected[] = { 0x40, 0x41, 0x42, 0x43,
                                        0x44, 0x45, 0x46, 0x47 };
    QTestState *qts = transport_start(
        "-global sgi-bridge-dma-test.byte-lane-self-ordered=on");
    uint8_t actual[sizeof(expected)];

    reg_write(qts, 0x80, 0xa00000);
    reg_write(qts, 0x200, 0x3010);
    dma_fill(qts, 0x80008000, 0x40);
    qtest_memread(qts, TARGET_A + SCRATCH, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_quit(qts);
}

static void test_self_ordered_pio(void)
{
    static const uint8_t expected[] = { 0xef, 0xcd, 0xab, 0x89,
                                        0x67, 0x45, 0x23, 0x01 };
    QTestState *qts = transport_start(
        "-global sgi-bridge-dma-test.byte-lane-self-ordered=on");

    qtest_writel(qts, BASE + 0x20014, 0x01200000);
    reg_write(qts, 0x200, 0x481012);
    for (unsigned i = 0; i < sizeof(expected); i++) {
        g_assert_cmphex(qtest_readb(qts, BASE + 0x200000 + i), ==,
                        expected[i]);
    }
    qtest_quit(qts);
}

static void test_self_ordered_pio_values(void)
{
    QTestState *qts = transport_start(
        "-global sgi-bridge-dma-test.byte-lane-self-ordered=on");
    static const uint8_t expected[] = { 0x88, 0x77, 0x66, 0x55,
                                        0x44, 0x33, 0x22, 0x11 };

    qtest_writel(qts, BASE + 0x20014, 0x01200000);
    reg_write(qts, 0x200, 0x483012);
    g_assert_cmphex(qtest_readq(qts, BASE + 0x200000), ==,
                    0x0123456789abcdefULL);
    qtest_writeq(qts, BASE + 0x200008, 0x1122334455667788ULL);
    for (unsigned i = 0; i < sizeof(expected); i++) {
        g_assert_cmphex(qtest_readb(qts, BASE + 0x200008 + i), ==,
                        expected[i]);
    }
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
    /*
     * PIO window 0 reaches the child; its PCI requester is upstream slot 1.
     */
    reg_write(qts, 0x200, 0x3012);
    reg_write(qts, 0x208, 0x80000);
    reg_write(qts, 0x80, 0xa00000);
    dma_fill(qts, 0x80008000, 0x50);
    g_assert_cmphex(qtest_readq(qts, TARGET_A + SCRATCH), ==,
                    0x5051525354555657ULL);
    qtest_quit(qts);
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
    qtest_irq_intercept_out_named(to, HEAD, "pci-intx");
    card_write(from, 0x10, 1);
    reg_write(from, 0x20, 0x0f00320e);
    reg_write(from, 0x80, 0xb00000);
    reg_write(from, 0x30, 0x000cabf4);
    reg_write(from, 0x38, 0x8000);
    reg_write(from, 0x148, 0x4885);
    reg_write(from, 0x108, 8);
    reg_write(from, 0x118, 8);
    qtest_set_irq_in(from, HEAD, NULL, 3, 1);
    qtest_writeq(from, BASE + 0x10ff8, 0x123456780021ULL);
    /*
     * Entry 1023, from the half of the table that migration version 2 adds.
     */
    qtest_writeq(from, BASE + 0x11ff8, 0x4321876500a1ULL);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    /* The source completes when its state is sent, not when it is loaded. */
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_true(qtest_get_irq(to, 0));
    card_write(to, 0x10, 0);
    g_assert_false(qtest_get_irq(to, 0));
    g_assert_cmphex(reg_read(to, 0x20), ==, 0x0f00320e);
    g_assert_cmphex(reg_read(to, 0x100) & 8, ==, 8);
    g_assert_cmphex(qtest_readl(to, BASE + 0x10ffc), ==, 0x1234);
    g_assert_cmphex(qtest_readl(to, BASE + 0x12ffc), ==, 0x56780021);
    g_assert_cmphex(qtest_readl(to, BASE + 0x11ffc), ==, 0x4321);
    g_assert_cmphex(qtest_readl(to, BASE + 0x13ffc), ==, 0x876500a1);
    qtest_set_irq_in(to, HEAD, NULL, 3, 0);
    g_assert_cmphex(qtest_readq(to, TARGET_C + SCRATCH), ==, 0x85);
    dma_fill(to, 0x80008000, 0x60);
    g_assert_cmphex(qtest_readq(to, TARGET_B + SCRATCH), ==,
                    0x6061626364656667ULL);
    card_write(to, 0x10, 1);
    qtest_system_reset(to);
    g_assert_false(qtest_get_irq(to, 0));
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
    reg_write(qts, 0x128, 0x93);
    reg_write(qts, 0x108, 1U << 24);
    /* Defined but deferred ranges are not invalid addresses. */
    reg_read(qts, 0x300);
    reg_read(qts, 0x400);
    qtest_readl(qts, BASE + 0x30004);
    qtest_readl(qts, BASE + 0xc00004);
    reg_write(qts, 0x400, 0);
    g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==, 0);
    qtest_writeq(qts, BASE + 0x20, 0xabcdef0123456789ULL);
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0x0f00320f);
    g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 1U << 24);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==, 0x193);
    /* The first access owns the record; a second one changes nothing. */
    g_assert_cmphex(reg_read(qts, 0x10), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x18), ==, 0x20);
    qtest_writeq(qts, TARGET_C + SCRATCH, 0);
    qtest_writeq(qts, BASE + 0x28, 0);
    g_assert_cmphex(reg_read(qts, 0x18), ==, 0x20);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==, 0);
    reg_write(qts, 0x110, 1);
    g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 1U << 24);
    reg_write(qts, 0x110, 8);
    g_assert_cmphex(reg_read(qts, 0x100), ==, 0);
    /* Cleared and rearmed: the next invalid access is a first one again. */
    qtest_writeq(qts, BASE + 0x30, 0);
    g_assert_cmphex(reg_read(qts, 0x18), ==, 0x30);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==, 0x193);
    reg_write(qts, 0xb0, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0xb0) & 3, ==, 0);
    reg_write(qts, 0x178, UINT32_MAX);
    g_assert_cmphex(reg_read(qts, 0x178), ==, 0);
    /* The acknowledge slot is eight bytes; the rest of its page is not. */
    reg_write(qts, 0x110, 8);
    qtest_readl(qts, BASE + 0x3000c);
    g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 1U << 24);
    g_assert_cmphex(reg_read(qts, 0x18), ==, 0x3000c);
    qtest_quit(qts);
}

static void test_undefined_registers(void)
{
    QTestState *qts = transport_start("");
    static const unsigned undefined[] = { 0x78, 0x88, 0xe0, 0xf8, 0x2a0,
                                          0x2f8 };
    static const unsigned defined[] = { 0x70, 0x80, 0xd8, 0x168, 0x170,
                                        0x278, 0x298 };
    unsigned i;

    reg_write(qts, 0x30, 0x000c1234);
    reg_write(qts, 0x38, 0x8000);
    reg_write(qts, 0x128, 0);
    reg_write(qts, 0x108, 1U << 24);
    /* The sanity diagnostic's provoke: a 32-bit store to a reserved slot. */
    qtest_writel(qts, BASE + 0x7c, 0x5555);
    g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 1U << 24);
    g_assert_cmphex(reg_read(qts, 0x18), ==, 0x7c);
    g_assert_cmphex(qtest_readq(qts, TARGET_C + SCRATCH), ==, 0x100);
    /* Every reserved slot below 0x300, read or written, is one. */
    for (i = 0; i < ARRAY_SIZE(undefined); i++) {
        reg_write(qts, 0x110, 8);
        qtest_readl(qts, BASE + undefined[i] + 4);
        g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 1U << 24);
        g_assert_cmphex(reg_read(qts, 0x18), ==, undefined[i] + 4);
        reg_write(qts, 0x110, 8);
        qtest_writel(qts, BASE + undefined[i] + 4, 0);
        g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 1U << 24);
    }
    /* The defined slots around them are not. */
    reg_write(qts, 0x110, 8);
    for (i = 0; i < ARRAY_SIZE(defined); i++) {
        reg_read(qts, defined[i]);
        g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 0);
    }
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

static void test_raw_pci_interrupts(void)
{
    QTestState *qts = transport_start(
        "-device sgi-bridge-dma-test,bus=head.0,addr=0.1 "
        "-device sgi-bridge-dma-test,bus=head.0,addr=2,interrupt-pin=3");
    uint8_t one[4] = { 1 };

    qtest_writel(qts, BASE + 0x20110, 0x01200000);
    qtest_writel(qts, BASE + 0x20104, 6);
    qtest_irq_intercept_out_named(qts, HEAD, "pci-intx");
    card_write(qts, 0x10, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(reg_read(qts, 0x100) & 0xff, ==, 0);
    reg_write(qts, 0x200, 0x83012);
    card_write(qts, 0x10, 1);
    reg_write(qts, 0x200, 0x83010);
    card_write(qts, 0x10, 0);
    g_assert_true(qtest_get_irq(qts, 0));
    reg_write(qts, 0x200, 0x83012);
    card_write(qts, 0x10, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(reg_read(qts, 0x100) & 0xff, ==, 0);
    /* Slot 2's pin C is output 10, told apart from slot 0's pin A. */
    qtest_writel(qts, BASE + 0x22010, 0x02000000);
    qtest_writel(qts, BASE + 0x22004, 6);
    reg_write(qts, 0x210, 0x83020);
    qtest_memwrite(qts, BASE + 0x600000 + 0x10, one, sizeof(one));
    g_assert_true(qtest_get_irq(qts, 10));
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 8));
    qtest_quit(qts);
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
        /*
         * ATE target zero is valid; high address bits belong to the target.
         */
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

/*
 * Slot 1's own scalar fields, same reasoning as card_write()/card_read():
 * value-preserving placement for configuration, regardless of what the test
 * has slot 1's own bit 13 set to.
 */
static void tx_write(QTestState *qts, unsigned reg, uint32_t value)
{
    uint32_t dev = reg_read(qts, 0x208);

    if (dev & (1U << 13)) {
        reg_write(qts, 0x208, dev & ~(1U << 13));
    }
    qtest_writel(qts, BASE + 0x400000 + reg, value);
    if (dev & (1U << 13)) {
        reg_write(qts, 0x208, dev);
    }
}

static uint32_t tx_read(QTestState *qts, unsigned reg)
{
    uint32_t dev = reg_read(qts, 0x208);
    uint32_t value;

    if (dev & (1U << 13)) {
        reg_write(qts, 0x208, dev & ~(1U << 13));
    }
    value = qtest_readl(qts, BASE + 0x400000 + reg);
    if (dev & (1U << 13)) {
        reg_write(qts, 0x208, dev);
    }
    return value;
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
    /* The flush range completes; the rest of local space gets no response. */
    g_assert_cmphex(tx_run(qts, 0x3fff8000), ==, 0);
    g_assert_cmphex(tx_run(qts, 0x00008000), ==, 0xdead0002);
    /*
     * Index 512 now has storage (a valid entry reads back), but the page
     * mapper uses only 512 entries, so it still faults.
     */
    ate_write(qts, 512, 0x8a21);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x12004 + 512 * 8), ==, 0x8a21);
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
    /* Refused by the model's rule, not a claim about the chip. */
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

/* The widget's space above the face: PCI memory by two aliases, PCI I/O. */
#define MEM32 0x40000000ULL
#define MEM64 0x80000000ULL
#define PCI_IO 0x100000000ULL

static void test_space_above_face(void)
{
    QTestState *qts = transport_start("");

    /*
     * The card's length register through the 32-bit alias, value kept. Read
     * back through the direct PIO window with its own bit 13 clear, so that
     * window's placement correction also nets to value-preserved (a
     * full-width access) rather than the byte-stream mirror transport_start's
     * shared bit-13-set default would apply here.
     */
    qtest_writel(qts, BASE + MEM32 + 0x01000004, 0x10);
    g_assert_cmphex(qtest_readl(qts, BASE + MEM32 + 0x01000004), ==, 0x10);
    reg_write(qts, 0x200, 0x81010);
    g_assert_cmphex(qtest_readl(qts, BASE + 0x200004), ==, 0x10);
    reg_write(qts, 0x200, 0x83010);
    /* Its wide scratch through the 64-bit alias, read back the same way. */
    qtest_writel(qts, BASE + 0x20014, 0x01200000);
    qtest_writeq(qts, BASE + MEM64 + 0x01200008, 0x1122334455667788ULL);
    g_assert_cmphex(qtest_readq(qts, BASE + MEM64 + 0x01200008), ==,
                    0x1122334455667788ULL);
    /*
     * Seen through a direct 64-bit window the bytes sit mirrored within
     * eight: the model's reading of the alias selecting the width.
     */
    reg_write(qts, 0x200, 0x483012);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x200008), ==, 0x88);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x20000f), ==, 0x11);
    /* With control bit 22 clear a byte store lands at offset ^ 3. */
    qtest_writeb(qts, BASE + MEM32 + 0x0120000c, 0xaa);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x20000f), ==, 0xaa);
    /* Setting it moves the memory placement to the offset addressed. */
    reg_write(qts, 0x20, 0x0f00320f | (1U << 22));
    qtest_writeb(qts, BASE + MEM32 + 0x0120000c, 0xbb);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x20000c), ==, 0xbb);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x20000f), ==, 0xaa);
    qtest_writeb(qts, BASE + MEM64 + 0x0120000d, 0xcc);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x20000d), ==, 0xcc);
    /* PCI I/O space through its range; bit 22 does not touch it. */
    qtest_writel(qts, BASE + 0x20018, 0x1000);
    qtest_writel(qts, BASE + 0x20004, 7);
    qtest_writeb(qts, BASE + PCI_IO + 0x1004, 0xdd);
    reg_write(qts, 0x200, 0x82000);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x201007), ==, 0xdd);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x201004), ==, 0);
    /* Bit 23 moves the I/O placement. */
    reg_write(qts, 0x20, 0x0f00320f | (1U << 23));
    qtest_writeb(qts, BASE + PCI_IO + 0x1004, 0xee);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x201004), ==, 0xee);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x201007), ==, 0xdd);
    /*
     * Outside every range: an undefined address, recorded as the widget sees
     * it, reaching no PCI space.
     */
    reg_write(qts, 0x110, 8);
    qtest_writel(qts, BASE + 0x01200008, 0x99999999);
    g_assert_cmphex(reg_read(qts, 0x100) & (1U << 24), ==, 1U << 24);
    g_assert_cmphex(reg_read(qts, 0x10), ==, 0);
    g_assert_cmphex(reg_read(qts, 0x18), ==, 0x01200008);
    reg_write(qts, 0x110, 8);
    qtest_readl(qts, BASE + 0xc0000004);
    g_assert_cmphex(reg_read(qts, 0x18), ==, 0xc0000004);
    reg_write(qts, 0x110, 8);
    qtest_readl(qts, BASE + 0x200000004ULL);
    g_assert_cmphex(reg_read(qts, 0x10), ==, 2);
    g_assert_cmphex(reg_read(qts, 0x18), ==, 4);
    reg_write(qts, 0x200, 0x483012);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x200008), ==, 0x88);
    qtest_quit(qts);
}

/*
 * QEMU's optional, synthetic board-identity responder at the bridge-half NIC
 * register. The XBridge specification calls this register unused in XBridge
 * mode.
 */
#define BRIDGE_NIC 0xb0

#define NIC_LEVEL (1u << 0)
#define NIC_DONE  (1u << 1)
#define NIC_PULSE(n)  ((uint32_t)(n) << 10)
#define NIC_SAMPLE(n) ((uint32_t)(n) << 2)

/*
 * The placeholder identity hw/mips/sn1.c cites.
 */
#define PLACEHOLDER_PART "NOT-OBSERVED"
#define PLACEHOLDER_FAMILY 0x0b
#define PLACEHOLDER_SERIAL 0x060504030201ULL

/*
 * head_start() above already covers the no-board-part case this fixture also
 * needs; only the board-part-supplied case is new here.
 */
static QTestState *head_start_with_board_part(const char *board_part)
{
    g_autofree char *args = g_strdup_printf(
        "-M none -cpu R10000 -S "
        "-device sgi-xbridge-head,id=head,board-part=%s", board_part);
    QTestState *qts = qtest_init(args);

    qtest_sysbus_mmio_map(qts, HEAD, 0, BASE);
    return qts;
}

/*
 * One bit slot, mainline's widths: a long reset, a short write-one/read, a
 * middling write-zero.
 */
static bool nic_slot(QTestState *qts, unsigned int pulse, unsigned int sample)
{
    reg_write(qts, BRIDGE_NIC, NIC_PULSE(pulse) | NIC_SAMPLE(sample));

    uint32_t nic = reg_read(qts, BRIDGE_NIC);

    g_assert_cmphex(nic & NIC_DONE, ==, NIC_DONE);
    return nic & NIC_LEVEL;
}

#define NIC_RESET(q)      nic_slot((q), 520, 65)
#define NIC_READ(q)       nic_slot((q), 6, 13)
#define NIC_WRITE_ONE(q)  nic_slot((q), 6, 13)
#define NIC_WRITE_ZERO(q) nic_slot((q), 80, 30)

static void nic_write_byte(QTestState *qts, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
}

static uint8_t nic_read_byte(QTestState *qts)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (NIC_READ(qts)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

static uint16_t nic_crc(uint16_t crc, const uint8_t *data, unsigned int len)
{
    unsigned int i, bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}
#define NIC_CRC_RESIDUE 0xb001

static uint8_t nic_crc8(uint8_t crc, uint64_t rom)
{
    unsigned int byte, bit;

    for (byte = 0; byte < 8; byte++) {
        crc ^= rom >> (byte * 8);
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }
    return crc;
}

/* Drive a search and return the identity the part hands back. */
static uint64_t nic_search_for_identity(QTestState *qts)
{
    uint64_t rom = 0;
    unsigned int i;

    nic_write_byte(qts, 0xf0);
    for (i = 0; i < 64; i++) {
        bool bit = NIC_READ(qts);
        bool complement = NIC_READ(qts);

        g_assert_cmpint(complement, ==, !bit);
        if (bit) {
            rom |= 1ULL << i;
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
    return rom;
}

/*
 * The scanning reader: find "name" within the first `len` bytes and copy
 * everything up to the next ';' delimiter. Unlike the fixed-span
 * positional reader (sn0-bridge-test.c's partnum_from_page(), not reused
 * here), this never collects the delimiter itself or reads past what it was
 * told to scan.
 */
static bool page_field(const uint8_t *page, unsigned int len,
                       const char *name, char *out)
{
    unsigned int pos = 0, n = strlen(name);

    while (pos < len) {
        unsigned int i;

        if (pos + n <= len && memcmp(&page[pos], name, n) == 0) {
            unsigned int j = 0;

            for (i = pos + n; i < len && page[i] != ';'; i++) {
                out[j++] = page[i];
            }
            out[j] = 0;
            return true;
        }
        for (i = pos; i < len && page[i] != ';'; i++) {
            /* skip to the next delimiter */
        }
        if (i >= len) {
            return false;
        }
        pos = i + 1;
    }
    return false;
}

/*
 * Check QEMU's placeholder record and its CRC. This says nothing about the
 * identity or one-wire routing of a physical Origin 300 IO-8 board.
 */
static void test_placeholder_identity_reads_back(void)
{
    QTestState *qts = head_start_with_board_part(PLACEHOLDER_PART);
    uint64_t rom;
    uint8_t exchange[6];
    uint8_t page[34];
    char part[13];
    unsigned int i;

    g_assert_false(NIC_RESET(qts));         /* pulled low: the part answers */

    rom = nic_search_for_identity(qts);
    g_assert_cmphex(rom & 0xff, ==, PLACEHOLDER_FAMILY);
    g_assert_cmphex((rom >> 8) & 0xffffffffffffULL, ==, PLACEHOLDER_SERIAL);
    g_assert_cmphex(nic_crc8(0, rom), ==, 0);

    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, 0xcc);              /* one part on bus: skip ROM */

    exchange[0] = 0xa5;                     /* read memory */
    exchange[1] = 0x00;
    exchange[2] = 0x00;
    for (i = 0; i < 3; i++) {
        nic_write_byte(qts, exchange[i]);
    }
    for (i = 3; i < 6; i++) {
        exchange[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(nic_crc(0, exchange, 6), ==, NIC_CRC_RESIDUE);

    for (i = 0; i < sizeof(page); i++) {
        page[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(nic_crc(0, page, sizeof(page)), ==, NIC_CRC_RESIDUE);

    g_assert_cmpmem(&page[6], 5, "Part:", 5);
    g_assert_cmpint(page[11 + 12], ==, ';');
    /*
     * Scan the field by name over page 1 alone (30 bytes, short of its own
     * CRC), exactly as sn0-bridge-test.c's page_field() does: never
     * collecting the ';' delimiter, unlike the fixed-span positional reader
     * that field also has to satisfy on a real driver.
     */
    g_assert_true(page_field(page, 30, "Part:", part));
    g_assert_cmpstr(part, ==, PLACEHOLDER_PART);

    qtest_quit(qts);
}

/*
 * A head a machine gives no board-part keeps BRIDGE_NIC exactly as inert as
 * the rev-A contract requires: no board behind that widget (like the empty
 * option slot at widget 14) must not gain a responder.
 */
static void test_no_board_stays_inert(void)
{
    QTestState *qts = head_start();

    g_assert_cmphex(reg_read(qts, BRIDGE_NIC), ==, 0);
    reg_write(qts, BRIDGE_NIC, NIC_PULSE(520) | NIC_SAMPLE(65));
    /* No onewire_bus for this head: the write is dropped, same as before. */
    g_assert_cmphex(reg_read(qts, BRIDGE_NIC), ==, 0);

    qtest_quit(qts);
}

/*
 * The XBridge head's own error interrupt on the Origin 300, provoked the way
 * the Origin 200 PROM's bridge_sanity provokes it: a 32-bit store to a
 * register slot the map does not define, with the error interrupt enabled and
 * the destination registers naming the Bedrock's interrupt-pending modify
 * register. The IP35 PROM's diagnostic expects the same status bit and
 * pending bit; which slot it stores to is not observed. What this file pins
 * is the machine path from the head's status bit to the processor interface's
 * pending bit, for both heads; the head's own error contract belongs to this
 * file's own tests above.
 */
#define MACHINE_ARGS "-M origin300 -cpu R14000 -smp 2"

#define IO_WIDGET(w) (SPACE_IO + (uint64_t)(w) * UINT64_C(0x1000000))
#define BEDROCK_LOCAL (SPACE_IO + UINT64_C(0x01000000))

#define XBRIDGE_HOST_PORT 0xa
#define XBRIDGE_ERROR_ADDRESS_LOWER 0x18
#define XBRIDGE_INT_UPPER 0x30
#define XBRIDGE_INT_LOWER 0x38
#define XBRIDGE_INT_STATUS 0x100
#define XBRIDGE_INT_ENABLE 0x108
#define XBRIDGE_INT_RESET 0x110
#define XBRIDGE_HOST_ERROR 0x128
#define XBRIDGE_INVALID_ADDRESS (1U << 24)
#define XBRIDGE_RESET_INVALID_ADDRESS (1U << 3)
/* A slot the register map does not define; the Origin 200 PROM's choice. */
#define XBRIDGE_UNDEFINED_SLOT 0x78

/*
 * Where an interrupt packet lands: the node's hub-register window with the
 * register flag at crosstalk address bit 47, which the head takes from bit 15
 * of the destination-upper register.
 */
#define PI_REMOTE_WINDOW 0x1800000
#define PI_PENDING_MODIFY 0x90
#define PI_PENDING_0 0x98
#define DESTINATION_UPPER ((XBRIDGE_HOST_PORT << 16) | (1U << 15))

static char *prom_path;

static char *create_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("sn1-xbridge-host-error-prom-XXXXXX.bin", &path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static uint32_t widget_reg_read(QTestState *qts, unsigned widget,
                                uint64_t offset)
{
    return qtest_readl(qts, IO_WIDGET(widget) + offset + 4);
}

static void widget_reg_write(QTestState *qts, unsigned widget,
                             uint64_t offset, uint32_t value)
{
    qtest_writel(qts, IO_WIDGET(widget) + offset + 4, value);
}

static void test_error_reaches_pending(void)
{
    g_autofree char *prom = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(MACHINE_ARGS " -bios %s", prom);
    static const struct {
        unsigned widget;
        unsigned bit;
    } head[2] = { { 0xe, 5 }, { 0xf, 6 } };
    unsigned i;

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==, 0);
    for (i = 0; i < 2; i++) {
        unsigned w = head[i].widget;

        widget_reg_write(qts, w, XBRIDGE_INT_UPPER, DESTINATION_UPPER);
        widget_reg_write(qts, w, XBRIDGE_INT_LOWER,
                         PI_REMOTE_WINDOW + PI_PENDING_MODIFY);
        widget_reg_write(qts, w, XBRIDGE_HOST_ERROR, head[i].bit);
        widget_reg_write(qts, w, XBRIDGE_INT_ENABLE, XBRIDGE_INVALID_ADDRESS);
        g_assert_cmphex(widget_reg_read(qts, w, XBRIDGE_INT_STATUS) &
                        XBRIDGE_INVALID_ADDRESS, ==, 0);
        /* The provoke: a store the head must post and report, not fault. */
        widget_reg_write(qts, w, XBRIDGE_UNDEFINED_SLOT, 0);
        g_assert_cmphex(widget_reg_read(qts, w, XBRIDGE_INT_STATUS) &
                        XBRIDGE_INVALID_ADDRESS, ==, XBRIDGE_INVALID_ADDRESS);
        g_assert_cmphex(widget_reg_read(qts, w, XBRIDGE_ERROR_ADDRESS_LOWER),
                        ==, XBRIDGE_UNDEFINED_SLOT + 4);
        /* And the interrupt reaches the pending bit the host field names. */
        g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0) &
                        (1ULL << head[i].bit), ==, 1ULL << head[i].bit);
        widget_reg_write(qts, w, XBRIDGE_INT_RESET,
                         XBRIDGE_RESET_INVALID_ADDRESS);
        g_assert_cmphex(widget_reg_read(qts, w, XBRIDGE_INT_STATUS) &
                        XBRIDGE_INVALID_ADDRESS, ==, 0);
    }
    /* Each head named its own bit; neither named the other's. */
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0) &
                    ((1ULL << 5) | (1ULL << 6)), ==, (1ULL << 5) | (1ULL << 6));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn1/xbridge/registers", test_registers);
    qtest_add_func("/sn1/xbridge/ate", test_ate);
    qtest_add_func("/sn1/xbridge/config-pio", test_config_pio);
    qtest_add_func("/sn1/xbridge/config-doubleword", test_config_doubleword);
    qtest_add_func("/sn1/xbridge/config-type1", test_config_type1);
    qtest_add_func("/sn1/xbridge/dma-routes", test_dma_routes);
    qtest_add_func("/sn1/xbridge/interrupt-packets", test_interrupt_packets);
    qtest_add_func("/sn1/xbridge/byte-lanes", test_byte_lanes);
    qtest_add_func("/sn1/xbridge/self-ordered-dma", test_self_ordered_dma);
    qtest_add_func("/sn1/xbridge/self-ordered-pio", test_self_ordered_pio);
    qtest_add_func("/sn1/xbridge/self-ordered-pio-values",
                   test_self_ordered_pio_values);
    qtest_add_func("/sn1/xbridge/upstream-requester", test_upstream_requester);
    qtest_add_func("/sn1/xbridge/migration-reset", test_migration_reset);
    qtest_add_func("/sn1/xbridge/access-errors", test_access_errors);
    qtest_add_func("/sn1/xbridge/undefined-registers",
                   test_undefined_registers);
    qtest_add_func("/sn1/xbridge/requester-bounds", test_requester_bounds);
    qtest_add_func("/sn1/xbridge/raw-pci-interrupts", test_raw_pci_interrupts);
    qtest_add_func("/sn1/xbridge/notifier-rejection", test_notifier_rejection);
    qtest_add_func("/sn1/xbridge/page-dma", test_page_dma);
    qtest_add_func("/sn1/xbridge/page-boundaries", test_page_boundaries);
    qtest_add_func("/sn1/xbridge/page-faults", test_page_faults);
    qtest_add_func("/sn1/xbridge/space-above-face", test_space_above_face);
    qtest_add_func("/sn1/xbridge/onewire/placeholder-identity-reads-back",
                   test_placeholder_identity_reads_back);
    qtest_add_func("/sn1/xbridge/onewire/no-board-stays-inert",
                   test_no_board_stays_inert);
    prom_path = create_prom();
    qtest_add_func("origin300/xbridge/error-reaches-pending",
                   test_error_reaches_pending);
    ret = g_test_run();
    unlink(prom_path);
    g_free(prom_path);
    return ret;
}
