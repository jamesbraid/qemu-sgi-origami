/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The composed XBridge: crossbar at widget 0, heads at 0xe and 0xf, one host
 * port. The host sits on a non-default port so the property is exercised,
 * not fixed routing hidden behind it. The fixture is not an Origin 300.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define CHIP "/machine/peripheral/xbridge"
#define HOST_PORT 0xb

/* The chip's crosstalk space: sixteen 16 TB widget slots. */
#define BASE 0x100000000000ULL
#define WIDGET(n) (BASE + ((uint64_t)(n) << 44))
#define FACE 0x1000000ULL

/* The chip's own views of widgets 0, 0xe and 0xf, mapped apart. */
#define VIEW(i) (0x2000000000000ULL + ((uint64_t)(i) << 44))

/* Where packets to the host land. */
#define FABRIC 0x20000000ULL
#define SCRATCH 0x8000

#define XBOW_ID 0x2d000001
#define HEAD_ID 0x2d002049
#define CARD_ID 0x00051b36

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
    /* Empty ports answer nothing. */
    g_assert_cmphex(qtest_readl(qts, WIDGET(8) + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, WIDGET(HOST_PORT) + 4), ==, 0);
    /* Past its face a head answers itself: an undefined address, recorded. */
    g_assert_cmphex(qtest_readl(qts, WIDGET(0xe) + FACE + 4), ==, 0);
    g_assert_cmphex(reg_read(qts, 0xe, 0x100) & (1U << 24), ==, 1U << 24);
    g_assert_cmphex(reg_read(qts, 0xe, 0x18), ==, FACE + 4);
    g_assert_cmphex(reg_read(qts, 0xf, 0x100) & (1U << 24), ==, 0);
    qtest_quit(qts);
}

static void test_space_above_face(void)
{
    QTestState *qts = chip_start(
        "-device sgi-bridge-dma-test,bus=head-e.0,addr=0");

    /* A card's register through head E's 32-bit alias, from the host. */
    qtest_writel(qts, WIDGET(0xe) + 0x20010, 0x01000000);
    qtest_writel(qts, WIDGET(0xe) + 0x20004, 6);
    qtest_writel(qts, WIDGET(0xe) + 0x40000000 + 0x01000004, 0x10);
    g_assert_cmphex(qtest_readl(qts, WIDGET(0xe) + 0x40000000 + 0x01000004),
                    ==, 0x10);
    g_assert_cmphex(reg_read(qts, 0xe, 0x100) & (1U << 24), ==, 0);
    /*
     * The same address at head F is its own PCI memory, decoded but empty:
     * no undefined-address error there, and E's register is not what it
     * reaches. What an unclaimed PCI cycle returns is not asserted.
     */
    qtest_writel(qts, WIDGET(0xf) + 0x40000000 + 0x01000004, 0x20);
    g_assert_cmphex(reg_read(qts, 0xf, 0x100) & (1U << 24), ==, 0);
    g_assert_cmphex(qtest_readl(qts, WIDGET(0xe) + 0x40000000 + 0x01000004),
                    ==, 0x10);
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

static void test_warm_reset_reaches_the_heads(void)
{
    QTestState *qts = chip_start("");

    reg_write(qts, 0xe, 0x28, 0x12345);
    reg_write(qts, 0xf, 0x28, 0x23456);
    reg_write(qts, 0, 0x20, 0xfe);
    /* Head E's link fence excludes the host; head F's admits it. */
    reg_write(qts, 0, 0x80 + (0xe - 8) * 8, 0xff & ~(1U << (HOST_PORT - 8)));
    qtest_set_irq_in(qts, CHIP, "warm-reset", HOST_PORT - 8, 1);
    qtest_set_irq_in(qts, CHIP, "warm-reset", HOST_PORT - 8, 0);
    g_assert_cmphex(reg_read(qts, 0, 0x20), ==, 0x80);
    g_assert_cmphex(reg_read(qts, 0xf, 0x28), ==, 0xfffff);
    g_assert_cmphex(reg_read(qts, 0xe, 0x28), ==, 0x12345);
    qtest_quit(qts);
}

static uint32_t config_read(QTestState *qts, uint64_t widget, unsigned slot)
{
    return qtest_readl(qts, widget + 0x20000 + slot * 0x1000);
}

static void test_widget_views(void)
{
    QTestState *qts = chip_start(
        "-device sgi-bridge-dma-test,bus=head-e.0,addr=1 "
        "-device sgi-bridge-dma-test,bus=head-f.0,addr=2");
    unsigned i;

    for (i = 0; i < 3; i++) {
        qtest_sysbus_mmio_map(qts, CHIP, 1 + i, VIEW(i));
    }
    g_assert_cmphex(qtest_readl(qts, VIEW(0) + 4), ==, XBOW_ID);
    g_assert_cmphex((qtest_readl(qts, VIEW(0) + 8 + 4) >> 6) & 0xf, ==,
                    HOST_PORT);
    g_assert_cmphex(qtest_readl(qts, VIEW(1) + 4), ==, HEAD_ID);
    g_assert_cmphex(qtest_readl(qts, VIEW(2) + 4), ==, HEAD_ID);
    /* A view is the widget's whole slot: past the face the head answers. */
    g_assert_cmphex(qtest_readl(qts, VIEW(1) + FACE + 4), ==, 0);
    g_assert_cmphex(reg_read(qts, 0xe, 0x100) & (1U << 24), ==, 1U << 24);
    /* A view and the full space show the same widget. */
    qtest_writel(qts, VIEW(2) + 0x28 + 4, 0x12345);
    g_assert_cmphex(reg_read(qts, 0xf, 0x28), ==, 0x12345);
    g_assert_cmphex(reg_read(qts, 0xe, 0x28), ==, 0xfffff);
    /* Each head's PCI bus is its own. */
    g_assert_cmphex(config_read(qts, VIEW(1), 1), ==, CARD_ID);
    g_assert_cmphex(config_read(qts, VIEW(1), 2), ==, 0xffffffff);
    g_assert_cmphex(config_read(qts, VIEW(2), 1), ==, 0xffffffff);
    g_assert_cmphex(config_read(qts, VIEW(2), 2), ==, CARD_ID);
    qtest_quit(qts);
}

static void card_setup(QTestState *qts, unsigned widget)
{
    qtest_writel(qts, WIDGET(widget) + 0x20010, 0x01000000);
    qtest_writel(qts, WIDGET(widget) + 0x20004, 6);
    reg_write(qts, widget, 0x200, 0x83010);
}

static void test_head_raw_sources(void)
{
    static const struct {
        const char *group;
        unsigned widget;
        unsigned line;
    } head[2] = {
        { "head-e-pci-intx", 0xe, 0 },
        { "head-f-pci-intx", 0xf, 2 },
    };
    unsigned i;

    /* A session intercepts one group, so one session per head. */
    for (i = 0; i < 2; i++) {
        QTestState *qts = chip_start(
            "-device sgi-bridge-dma-test,bus=head-e.0,addr=0 "
            "-device sgi-bridge-dma-test,bus=head-f.0,addr=0,interrupt-pin=3");
        unsigned other = i ^ 1;

        card_setup(qts, 0xe);
        card_setup(qts, 0xf);
        qtest_irq_intercept_out_named(qts, CHIP, head[i].group);
        /* The other head's card is not in this group. */
        card_write(qts, head[other].widget, 0x10, 1);
        g_assert_cmpuint(qtest_get_irq_raise_count(qts, head[other].line), ==,
                         0);
        card_write(qts, head[i].widget, 0x10, 1);
        g_assert_true(qtest_get_irq(qts, head[i].line));
        card_write(qts, head[i].widget, 0x10, 0);
        g_assert_false(qtest_get_irq(qts, head[i].line));
        qtest_quit(qts);
    }
}

static void test_head_pins(void)
{
    QTestState *qts = chip_start("");

    /* A chip-level pin is that head's own: its status bit, nobody else's. */
    qtest_set_irq_in(qts, CHIP, "head-e-pin", 3, 1);
    g_assert_cmphex(reg_read(qts, 0xe, 0x100) & 0xff, ==, 8);
    g_assert_cmphex(reg_read(qts, 0xf, 0x100) & 0xff, ==, 0);
    qtest_set_irq_in(qts, CHIP, "head-e-pin", 3, 0);
    g_assert_cmphex(reg_read(qts, 0xe, 0x100) & 0xff, ==, 0);
    /* And its rising edge sends that head's packet. */
    reg_write(qts, 0xf, 0x30, HOST_PORT << 16);
    reg_write(qts, 0xf, 0x38, SCRATCH);
    reg_write(qts, 0xf, 0x148, 0x85);
    reg_write(qts, 0xf, 0x108, 8);
    qtest_set_irq_in(qts, CHIP, "head-f-pin", 3, 1);
    g_assert_cmphex(reg_read(qts, 0xf, 0x100) & 0xff, ==, 8);
    g_assert_cmphex(qtest_readq(qts, FABRIC + SCRATCH), ==, 0x185);
    qtest_quit(qts);
}

static bool chip_has_property(QTestState *qts, const char *name)
{
    QDict *resp = qtest_qmp(qts, "{'execute': 'qom-list', "
                            "'arguments': {'path': %s}}", CHIP);
    const QListEntry *e;
    bool found = false;

    for (e = qlist_first(qdict_get_qlist(resp, "return")); e;
         e = qlist_next(e)) {
        if (!strcmp(qdict_get_str(qobject_to(QDict, e->value), "name"),
                    name)) {
            found = true;
        }
    }
    qobject_unref(resp);
    return found;
}

static void test_warm_reset_external_ports(void)
{
    QTestState *qts = chip_start("");

    /* Six lines each way: the heads' lines stay inside the chip. */
    g_assert_true(chip_has_property(qts, "warm-reset[5]"));
    g_assert_false(chip_has_property(qts, "warm-reset[6]"));
    g_assert_true(chip_has_property(qts, "warm-reset-out[5]"));
    g_assert_false(chip_has_property(qts, "warm-reset-out[6]"));
    qtest_irq_intercept_out_named(qts, CHIP, "warm-reset-out");
    qtest_set_irq_in(qts, CHIP, "warm-reset", HOST_PORT - 8, 1);
    qtest_set_irq_in(qts, CHIP, "warm-reset", HOST_PORT - 8, 0);
    /* Every other admitted external port goes out; the source does not. */
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 9 - 8), ==, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, HOST_PORT - 8), ==, 0);
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
    qtest_add_func("/sn1/xbridge-chip/space-above-face",
                   test_space_above_face);
    qtest_add_func("/sn1/xbridge-chip/fixed-destinations",
                   test_fixed_destinations);
    qtest_add_func("/sn1/xbridge-chip/head-interrupts-reach-the-host",
                   test_head_interrupts_reach_the_host);
    qtest_add_func("/sn1/xbridge-chip/head-dma-reaches-the-host",
                   test_head_dma_reaches_the_host);
    qtest_add_func("/sn1/xbridge-chip/warm-reset-reaches-the-heads",
                   test_warm_reset_reaches_the_heads);
    qtest_add_func("/sn1/xbridge-chip/widget-views", test_widget_views);
    qtest_add_func("/sn1/xbridge-chip/head-raw-sources",
                   test_head_raw_sources);
    qtest_add_func("/sn1/xbridge-chip/head-pins", test_head_pins);
    qtest_add_func("/sn1/xbridge-chip/warm-reset-external-ports",
                   test_warm_reset_external_ports);
    qtest_add_func("/sn1/xbridge-chip/realize-checks", test_realize_checks);
    qtest_add_func("/sn1/xbridge-chip/realize-needs-fabric",
                   test_realize_needs_fabric);
    return g_test_run();
}
