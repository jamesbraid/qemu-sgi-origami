/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The composed XBridge: crossbar at widget 0, heads at 0xe and 0xf, one host
 * port. The host sits on a non-default port so the property is exercised,
 * not fixed routing hidden behind it. The fixture is not an Origin 300.
 */
#include "qemu/osdep.h"
#include "libqtest.h"

#define CHIP "/machine/peripheral/xbridge"
#define HOST_PORT 0xb

/* The chip's crosstalk space: sixteen 16 TB widget slots. */
#define BASE 0x100000000000ULL
#define WIDGET(n) (BASE + ((uint64_t)(n) << 44))
#define FACE 0x1000000ULL

/* Where packets to the host land. */
#define FABRIC 0x20000000ULL
#define SCRATCH 0x8000

#define XBOW_ID 0x2d000001
#define HEAD_ID 0x2d002049

static QTestState *chip_start(const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors "
        "-device sgi-xtalk-test-target,id=fabric "
        "-device sgi-xbridge,id=xbridge,host-port=0x%x,"
        "fabric=/machine/peripheral/fabric/fabric.ram[0] %s",
        HOST_PORT, extra);

    qtest_sysbus_mmio_map(qts, CHIP, 0, BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, FABRIC);
    return qts;
}

static uint32_t reg_read(QTestState *qts, unsigned widget, unsigned off)
{
    return qtest_readl(qts, WIDGET(widget) + off + 4);
}

static void reg_write(QTestState *qts, unsigned widget, unsigned off,
                      uint32_t value)
{
    qtest_writel(qts, WIDGET(widget) + off + 4, value);
}

static void test_widgets(void)
{
    QTestState *qts = chip_start("");
    unsigned n;

    g_assert_cmphex(reg_read(qts, 0, 0), ==, XBOW_ID);
    g_assert_cmphex(reg_read(qts, 0xe, 0), ==, HEAD_ID);
    g_assert_cmphex(reg_read(qts, 0xf, 0), ==, HEAD_ID);
    /* The crossbar sees the host on its port, not on the default one. */
    g_assert_cmphex((reg_read(qts, 0, 8) >> 6) & 0xf, ==, HOST_PORT);
    for (n = 8; n <= 0xf; n++) {
        uint32_t aux = reg_read(qts, 0, 0x100 + (n - 8) * 0x40 + 0x38);
        bool present = n == HOST_PORT || n == 0xe || n == 0xf;

        g_assert_cmpuint(!!(aux & (1U << 5)), ==, present);
    }
    /* Empty ports and space beyond a face answer nothing. */
    g_assert_cmphex(qtest_readl(qts, WIDGET(8) + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, WIDGET(HOST_PORT) + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, WIDGET(0xe) + FACE + 4), ==, 0);
    qtest_quit(qts);
}

static void test_fixed_destinations(void)
{
    QTestState *qts = chip_start("");

    /* A head's widget-ID field is its source identity, not its address. */
    reg_write(qts, 0xe, 0x20, 0x0f003209);
    g_assert_cmphex(reg_read(qts, 0xe, 0x20), ==, 0x0f003219);
    g_assert_cmphex(reg_read(qts, 0xe, 0), ==, HEAD_ID);
    g_assert_cmphex(reg_read(qts, 0xf, 0x20), ==, 0x0f00320f);
    g_assert_cmphex(qtest_readl(qts, WIDGET(9) + 4), ==, 0);
    /* Each head keeps its own state; a write to one is unseen at the other. */
    reg_write(qts, 0xf, 0x28, 0x12345);
    g_assert_cmphex(reg_read(qts, 0xf, 0x28), ==, 0x12345);
    g_assert_cmphex(reg_read(qts, 0xe, 0x28), ==, 0xfffff);
    qtest_quit(qts);
}

static void test_head_interrupts_reach_the_host(void)
{
    QTestState *qts = chip_start("");

    qtest_writeq(qts, FABRIC + SCRATCH, 0x123456789abcdef0ULL);
    reg_write(qts, 0xe, 0x30, HOST_PORT << 16);
    reg_write(qts, 0xe, 0x38, SCRATCH);
    reg_write(qts, 0xe, 0x148, 0x85);
    reg_write(qts, 0xe, 0x108, 8);
    qtest_set_irq_in(qts, CHIP "/head-e", NULL, 3, 1);
    g_assert_cmphex(qtest_readq(qts, FABRIC + SCRATCH), ==, 0x185);
    /* Aimed at another widget, the packet reaches nothing. */
    qtest_writeq(qts, FABRIC + SCRATCH, 0);
    reg_write(qts, 0xf, 0x30, 0xe << 16);
    reg_write(qts, 0xf, 0x38, SCRATCH);
    reg_write(qts, 0xf, 0x148, 0x85);
    reg_write(qts, 0xf, 0x108, 8);
    qtest_set_irq_in(qts, CHIP "/head-f", NULL, 3, 1);
    g_assert_cmphex(qtest_readq(qts, FABRIC + SCRATCH), ==, 0);
    qtest_quit(qts);
}

static void card_write(QTestState *qts, unsigned widget, unsigned reg,
                       uint32_t value)
{
    uint8_t bytes[4] = { value, value >> 8, value >> 16, value >> 24 };

    qtest_memwrite(qts, WIDGET(widget) + 0x200000 + reg, bytes, sizeof(bytes));
}

static void test_head_dma_reaches_the_host(void)
{
    QTestState *qts = chip_start(
        "-device sgi-bridge-dma-test,bus=head-e.0,addr=0");

    qtest_writel(qts, WIDGET(0xe) + 0x20010, 0x01000000);
    qtest_writel(qts, WIDGET(0xe) + 0x20004, 6);
    reg_write(qts, 0xe, 0x200, 0x83010);
    qtest_writeq(qts, FABRIC + SCRATCH, 0xfeedfacecafebeefULL);
    /* The direct window aimed at the host port lands in the fabric. */
    reg_write(qts, 0xe, 0x80, HOST_PORT << 20);
    card_write(qts, 0xe, 0, 0x80008000);
    card_write(qts, 0xe, 0x18, 0);
    card_write(qts, 0xe, 4, 8);
    card_write(qts, 0xe, 8, 0x10);
    card_write(qts, 0xe, 0xc, 1);
    g_assert_cmphex(qtest_readq(qts, FABRIC + SCRATCH), ==,
                    0x1011121314151617ULL);
    /* Aimed at the other head, nothing moves. */
    reg_write(qts, 0xe, 0x80, 0xf << 20);
    card_write(qts, 0xe, 8, 0x20);
    card_write(qts, 0xe, 0xc, 1);
    g_assert_cmphex(qtest_readq(qts, FABRIC + SCRATCH), ==,
                    0x1011121314151617ULL);
    qtest_quit(qts);
}

static void test_realize_checks(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init("-M none -cpu R10000 -S "
            "-device sgi-xtalk-test-target,id=fabric "
            "-device sgi-xbridge,id=xbridge,host-port=0xe,"
            "fabric=/machine/peripheral/fabric/fabric.ram[0]");

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*host port must be 8 to 0xd*");
}

static void test_realize_needs_fabric(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init("-M none -cpu R10000 -S "
            "-device sgi-xbridge,id=xbridge,host-port=0xa");

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*needs the fabric link*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn1/xbridge-chip/widgets", test_widgets);
    qtest_add_func("/sn1/xbridge-chip/fixed-destinations",
                   test_fixed_destinations);
    qtest_add_func("/sn1/xbridge-chip/head-interrupts-reach-the-host",
                   test_head_interrupts_reach_the_host);
    qtest_add_func("/sn1/xbridge-chip/head-dma-reaches-the-host",
                   test_head_dma_reaches_the_host);
    qtest_add_func("/sn1/xbridge-chip/realize-checks", test_realize_checks);
    qtest_add_func("/sn1/xbridge-chip/realize-needs-fabric",
                   test_realize_needs_fabric);
    return g_test_run();
}
