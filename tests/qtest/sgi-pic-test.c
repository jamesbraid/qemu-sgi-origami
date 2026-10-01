/* SPDX-License-Identifier: BSD-3-Clause */
/* PIC revision 3 compatibility rules, with synthetic PCI fixture identities. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include "hw/pci/pci_regs.h"

#define PIC "/machine/peripheral/pic"
#define BASE 0x100000000000ULL
#define HEAD(n) (BASE + (n) * 0x800000ULL)
#define TARGET 0x20000000ULL
#define CARD(n) (BASE + (n ? 0xc0000000ULL : 0x40000000ULL) + 0x01000000)

static uint32_t pic_requests(QTestState *qts, unsigned head, unsigned pin,
                             bool set)
{
    g_autofree char *path = g_strdup_printf(PIC "/head-%u", head);
    g_autofree char *property = g_strdup_printf("x-test-%s-requests-%u",
                                                set ? "set" : "clear", pin);
    QDict *reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, property);
    uint32_t count;

    g_assert_true(qdict_haskey(reply, "return"));
    count = qdict_get_int(reply, "return");

    qobject_unref(reply);
    return count;
}

static QTestState *pic_start(bool cards, const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors "
        "-device sgi-xio-test-target,id=fabric,size=65536 "
        "-device sgi-pic,id=pic,test-fixture=on,"
        "target-0=/machine/peripheral/fabric/fabric.ram[0] %s %s",
        cards ?
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3500 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1,"
        "vendor-id=0xfeed,device-id=0x3501" : "", extra);

    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, TARGET);
    return qts;
}

static void test_heads_and_widths(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-width-XXXXXX", &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log_contents = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    extra = g_strdup_printf("-D %s", log_path);
    qts = pic_start(false, extra);

    g_assert_cmphex(qtest_readq(qts, HEAD(0)), ==, 0x3d102024);
    g_assert_cmphex(qtest_readq(qts, HEAD(1)), ==, 0x3d112024);
    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    qtest_writeq(qts, HEAD(1) + 0x108, 0x80);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x108), ==, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x108), ==, 0x80);
    qtest_writeb(qts, HEAD(0) + 0x108, 0xff);
    qtest_writew(qts, HEAD(0) + 0x108, 0xffff);
    qtest_writel(qts, HEAD(0) + 0x108, 0xffffffff);
    qtest_writel(qts, HEAD(0) + 0x10c, 0xffffffff);
    g_assert_cmphex(qtest_readb(qts, HEAD(0) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readw(qts, HEAD(0) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x108), ==, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x108), ==, 0x80);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio write offset 0x108 size 1"));
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio write offset 0x108 size 2"));
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio write offset 0x108 size 4"));
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio write offset 0x10c size 4"));
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio read offset 0x108 size 1"));
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio read offset 0x108 size 2"));
    g_assert_nonnull(strstr(log_contents,
                            "origin=widget-mmio read offset 0x108 size 4"));
    unlink(log_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_configuration(void)
{
    QTestState *qts = pic_start(true, "");

    /* These IDs belong only to the explicitly selected synthetic fixture. */
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x20000), ==, 0x3502feed);
    g_assert_cmphex(qtest_readl(qts, HEAD(1) + 0x20000), ==, 0x3503feed);
    qtest_writel(qts, HEAD(0) + 0x20004, PCI_COMMAND_MEMORY);
    qtest_writel(qts, HEAD(0) + 0x20010, 0x01000000);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x20004) &
                    PCI_COMMAND_MEMORY, ==, PCI_COMMAND_MEMORY);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x20010), ==, 0x01000000);
    g_assert_cmphex(qtest_readl(qts, HEAD(1) + 0x20010), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x21000), ==, 0x3500feed);
    g_assert_cmphex(qtest_readl(qts, HEAD(1) + 0x21000), ==, 0x3501feed);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x22000), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x21100), ==, 0xffffffff);
    g_assert_cmphex(qtest_readb(qts, HEAD(0) + 0x21000), ==, 0x35);
    g_assert_cmphex(qtest_readb(qts, HEAD(0) + 0x21003), ==, 0xed);
    qtest_writeb(qts, HEAD(0) + 0x21007, 5);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x21004) & 7, ==, 5);
    qtest_writew(qts, HEAD(0) + 0x21006, 6);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x21004) & 7, ==, 6);
    qtest_writel(qts, HEAD(0) + 0x21004, 7);
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x21004) & 7, ==, 7);
    qtest_writeq(qts, HEAD(0) + 0xc8, (2ULL << 16) | (3 << 11));
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x28000), ==, 0xffffffff);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0xc8), ==, 0);
    qtest_writeq(qts, HEAD(0) + 0x08, 0xffff);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x08), ==, 0);
    qtest_writeq(qts, HEAD(0) + 0x600, 0xffff);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x600), ==, 0);
    qtest_quit(qts);
}

static void test_slot_bounds(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init("-M none -cpu R10000 -S "
            "-device sgi-pic,id=pic,test-fixture=on "
            "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=5");

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*slot 5 function 0 not available*reserved*");
}

static void test_unresolved_default_identity(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init("-M none -cpu R10000 -S "
            "-device sgi-pic,id=pic");

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*PIC revision 3 PCI device 0 identity is"
                              " unresolved*test-fixture=on is only for"
                              " synthetic register tests*");
}

static void test_type1_configuration(void)
{
    QTestState *qts = qtest_init("-M none -cpu R10000 -S "
        "-device sgi-pic,id=pic,test-fixture=on "
        "-device pci-bridge,id=down,bus=pic-head-0.0,addr=4,chassis_nr=1 "
        "-device sgi-bridge-dma-test,bus=down,addr=3,"
        "vendor-id=0xfeed,device-id=0x3504 "
        "-device sgi-bridge-dma-test,bus=down,addr=4,"
        "vendor-id=0xfeed,device-id=0x3505");

    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    qtest_writel(qts, HEAD(0) + 0x24018, 0x00020200);
    qtest_writeq(qts, HEAD(0) + 0xc8, (2ULL << 16) | (3 << 11));
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x28000), ==, 0x3504feed);
    qtest_writeq(qts, HEAD(0) + 0xc8, (2ULL << 16) | (4 << 11));
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x28000), ==, 0x3505feed);
    qtest_writeq(qts, HEAD(0) + 0xc8, (2ULL << 16) | (5 << 11));
    g_assert_cmphex(qtest_readl(qts, HEAD(0) + 0x28000), ==, 0xffffffff);
    qtest_writeq(qts, HEAD(1) + 0xc8, (2ULL << 16) | (3 << 11));
    g_assert_cmphex(qtest_readl(qts, HEAD(1) + 0x28000), ==, 0xffffffff);
    qtest_quit(qts);
}

static void test_unsupported_registers(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-reg-XXXXXX", &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log_contents = NULL;
    QTestState *qts;
    const char *at;
    unsigned count = 0;
    unsigned i;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors -D %s "
        "-device sgi-pic,id=pic,test-fixture=on", log_path);
    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    qtest_writeq(qts, HEAD(0) + 0x08, 0x55);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x08), ==, 0);
    qtest_writeq(qts, HEAD(0) + 0x600, 0x66);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x600), ==, 0);
    qtest_writeb(qts, HEAD(0) + 0x108, 0x77);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x40000000), ==, 0);
    for (i = 0; i < 40; i++) {
        qtest_writeq(qts, HEAD(0) + 0x600, 0x100 + i);
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log_contents,
                            "head 0 origin=widget-mmio write offset 0x8"
                            " size 8 value 0x55"));
    g_assert_nonnull(strstr(log_contents,
                            "head 0 origin=widget-mmio read offset 0x8"
                            " size 8"));
    g_assert_nonnull(strstr(log_contents,
                            "head 0 origin=widget-mmio write offset 0x600"
                            " size 8 value 0x66"));
    g_assert_nonnull(strstr(log_contents,
                            "head 0 origin=widget-mmio write offset 0x108"
                            " size 1 value 0x77"));
    g_assert_nonnull(strstr(log_contents,
                            "head 0 origin=pci-memory-window read offset"
                            " 0x40000000 size 8"));
    for (at = log_contents; (at = strstr(at, "further unsupported accesses"
                                      " suppressed")); at++) {
        count++;
    }
    g_assert_cmpuint(count, ==, 1);
    g_assert_null(strstr(log_contents, "size 8 value 0x127"));
    unlink(log_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_pin_and_reset(void)
{
    QTestState *qts = pic_start(false, "");

    /* Packet data is a declared model rule, not a measured PIC format. */
    qtest_writeq(qts, HEAD(0) + 0x130, 0x1234000000001000ULL);
    qtest_writeq(qts, HEAD(1) + 0x168, 0x5678000000002000ULL);
    qtest_writeq(qts, HEAD(0) + 0x80, 0x1234);
    qtest_writeq(qts, HEAD(0) + 0xc8, (2ULL << 16) | (3 << 11));
    qtest_writeq(qts, HEAD(0) + 0x18000, 0xfeedface);
    qtest_writeq(qts, HEAD(1) + 0x19000, 0xcafebabe);
    qtest_writeq(qts, HEAD(1) + 0x80, 0x1234);
    qtest_writeq(qts, HEAD(1) + 0xc8, (3ULL << 16) | (4 << 11));
    qtest_set_irq_in(qts, PIC "/head-0", NULL, 0, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 1);
    g_assert_cmpuint(pic_requests(qts, 0, 0, true), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x100), ==, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0);
    qtest_writeq(qts, HEAD(0) + 0x108, 1);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x11234);
    g_assert_cmpuint(pic_requests(qts, 0, 0, true), ==, 1);
    qtest_set_irq_in(qts, PIC "/head-1", NULL, 7, 1);
    qtest_writeq(qts, HEAD(1) + 0x108, 0x80);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x2000), ==, 0x15678);
    g_assert_cmpuint(pic_requests(qts, 1, 7, true), ==, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x100), ==, 0x80);
    qtest_writeq(qts, HEAD(0) + 0x110, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 1);
    g_assert_cmpuint(pic_requests(qts, 0, 0, true), ==, 2);
    qtest_writeq(qts, TARGET + 0x1000, 0x55);
    qtest_writeq(qts, HEAD(0) + 0x130, 0x1234000100001000ULL);
    qtest_writeq(qts, HEAD(0) + 0x110, 1);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x55);
    g_assert_cmpuint(pic_requests(qts, 0, 0, true), ==, 3);
    qtest_writeq(qts, HEAD(0) + 0x130, 0x1234000000001000ULL);
    qtest_writeq(qts, HEAD(0) + 0x118, 1);
    qtest_set_irq_in(qts, PIC "/head-0", NULL, 0, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x1000), ==, 0x1234);
    g_assert_cmpuint(pic_requests(qts, 0, 0, false), ==, 1);
    qtest_writeq(qts, HEAD(1) + 0x118, 0x80);
    qtest_set_irq_in(qts, PIC "/head-1", NULL, 7, 0);
    g_assert_cmphex(qtest_readq(qts, TARGET + 0x2000), ==, 0x5678);
    g_assert_cmpuint(pic_requests(qts, 1, 7, false), ==, 1);
    qtest_writeq(qts, HEAD(0) + 0x110, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 0);
    qtest_set_irq_in(qts, PIC "/head-0", NULL, 0, 1);
    qtest_set_irq_in(qts, PIC "/head-1", NULL, 7, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 1);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x100), ==, 0x80);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x130), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x80), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0xc8), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x18000), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x108), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x100), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x80), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0xc8), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x19000), ==, 0);
    qtest_quit(qts);
}

static void test_remaining_pin_edges(void)
{
    static const struct {
        unsigned head;
        unsigned pin;
        uint64_t target_offset;
        uint16_t level;
    } cases[] = {
        { 0, 7, 0x3000, 0x3456 },
        { 1, 0, 0x4000, 0x789a },
    };
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        unsigned head = cases[i].head;
        unsigned other = 1 - head;
        unsigned pin = cases[i].pin;
        uint64_t bit = 1U << pin;
        uint64_t target = TARGET + cases[i].target_offset;
        g_autofree char *path = g_strdup_printf(PIC "/head-%u", head);
        QTestState *qts = pic_start(false, "");

        qtest_writeq(qts, HEAD(head) + 0x130 + pin * 8,
                     ((uint64_t)cases[i].level << 48) |
                     cases[i].target_offset);
        qtest_set_irq_in(qts, path, NULL, pin, 1);
        g_assert_cmphex(qtest_readq(qts, HEAD(head) + 0x100), ==, bit);
        g_assert_cmphex(qtest_readq(qts, HEAD(other) + 0x100), ==, 0);
        g_assert_cmpuint(pic_requests(qts, head, pin, true), ==, 0);
        g_assert_cmpuint(pic_requests(qts, other, pin, true), ==, 0);
        qtest_writeq(qts, HEAD(head) + 0x108, bit);
        g_assert_cmphex(qtest_readq(qts, target), ==,
                        0x10000 | cases[i].level);
        g_assert_cmpuint(pic_requests(qts, head, pin, true), ==, 1);
        g_assert_cmpuint(pic_requests(qts, other, pin, true), ==, 0);
        qtest_set_irq_in(qts, path, NULL, pin, 1);
        g_assert_cmpuint(pic_requests(qts, head, pin, true), ==, 1);
        qtest_writeq(qts, HEAD(head) + 0x110, bit);
        g_assert_cmpuint(pic_requests(qts, head, pin, true), ==, 2);
        g_assert_cmphex(qtest_readq(qts, target), ==,
                        0x10000 | cases[i].level);
        qtest_writeq(qts, HEAD(head) + 0x118, bit);
        qtest_set_irq_in(qts, path, NULL, pin, 0);
        g_assert_cmphex(qtest_readq(qts, target), ==, cases[i].level);
        g_assert_cmpuint(pic_requests(qts, head, pin, false), ==, 1);
        g_assert_cmpuint(pic_requests(qts, other, pin, false), ==, 0);
        g_assert_cmphex(qtest_readq(qts, HEAD(other) + 0x108), ==, 0);
        qtest_quit(qts);
    }
}

static void test_direct_dma(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-direct-XXXXXX", &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log_contents = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    uint8_t bytes[8];
    uint8_t unchanged[8];
    unsigned i;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    extra = g_strdup_printf("-D %s", log_path);
    qts = pic_start(true, extra);
    for (i = 0; i < 2; i++) {
        qtest_writel(qts, HEAD(i) + 0x21010, 0x01000000);
        qtest_writel(qts, HEAD(i) + 0x21004, 6);
        qtest_writeq(qts, HEAD(i) + 0x80, 0);
        qtest_writel(qts, CARD(i), 0x80001000 + i * 0x1000);
        qtest_writel(qts, CARD(i) + 4, 8);
        qtest_writel(qts, CARD(i) + 8, i ? 0x10 : 0x01);
        qtest_writel(qts, CARD(i) + 0xc, 1);
    }
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 1 + i);
    }
    qtest_memread(qts, TARGET + 0x2000, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x10 + i);
    }
    qtest_writeq(qts, HEAD(0) + 0x200, 1U << 19);
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x200), ==, 1U << 19);
    qtest_writel(qts, CARD(0), 0x80001000);
    qtest_writel(qts, CARD(0) + 4, 8);
    qtest_writel(qts, CARD(0) + 8, 0x21);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x21 + (i ^ 3));
    }
    qtest_writel(qts, CARD(0), 0x80000ffc);
    qtest_writel(qts, CARD(0) + 8, 0x31);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0xffc, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x31 + (i ^ 3));
    }
    qtest_writeq(qts, HEAD(0) + 0x200, (1U << 19) | (1U << 20));
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x200), ==,
                    (1U << 19) | (1U << 20));
    qtest_memread(qts, TARGET + 0x1000, unchanged, sizeof(unchanged));
    qtest_writeq(qts, HEAD(0) + 0x80, 0xfULL << 20);
    qtest_writel(qts, CARD(0), 0x80001000);
    qtest_writel(qts, CARD(0) + 8, 0x41);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, bytes, sizeof(bytes));
    g_assert_cmpmem(bytes, sizeof(bytes), unchanged, sizeof(unchanged));
    qtest_writeq(qts, HEAD(0) + 0x80, 0);
    qtest_writeq(qts, HEAD(0) + 0x200, 0);
    qtest_writel(qts, CARD(0), 0x8000fffc);
    qtest_writel(qts, CARD(0) + 8, 0x60);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0xfffc, bytes, 4);
    for (i = 0; i < 4; i++) {
        g_assert_cmpuint(bytes[i], ==, 0x60 + i);
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log_contents,
                            "head 0 invalid DMA target f address 0x1000"
                            " from slot 1"));
    unlink(log_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_direct_dma_head_targets(void)
{
    QTestState *qts = qtest_init("-M none -cpu R10000 -S "
        "-device sgi-xio-test-target,id=near,size=65536 "
        "-device sgi-xio-test-target,id=far,size=65536 "
        "-device sgi-pic,id=pic,test-fixture=on,"
        "target-0=/machine/peripheral/near/near.ram[0],"
        "target-1=/machine/unattached/system[0] "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=1");
    uint8_t near[8];
    uint8_t far[8];
    const uint8_t zeros[8] = { 0 };
    unsigned i;

    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/near", 0, TARGET);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/far", 0, 0x80000000);
    for (i = 0; i < 2; i++) {
        qtest_writel(qts, HEAD(i) + 0x21010, 0x01000000);
        qtest_writel(qts, HEAD(i) + 0x21004, PCI_COMMAND_MEMORY |
                     PCI_COMMAND_MASTER);
        qtest_writel(qts, CARD(i) + 4, 8);
    }
    qtest_writeq(qts, HEAD(0) + 0x80, 0);
    qtest_writeq(qts, HEAD(1) + 0x80, (1ULL << 20) | 1);
    qtest_writel(qts, CARD(0), 0x80001000);
    qtest_writel(qts, CARD(0) + 8, 0x11);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_writel(qts, CARD(1), 0x80002000);
    qtest_writel(qts, CARD(1) + 8, 0x21);
    qtest_writel(qts, CARD(1) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, near, sizeof(near));
    qtest_memread(qts, 0x80002000, far, sizeof(far));
    for (i = 0; i < sizeof(near); i++) {
        g_assert_cmpuint(near[i], ==, 0x11 + i);
        g_assert_cmpuint(far[i], ==, 0x21 + i);
    }
    qtest_memread(qts, 0x80001000, far, sizeof(far));
    g_assert_cmpmem(far, sizeof(far), zeros, sizeof(zeros));
    qtest_memread(qts, TARGET + 0x2000, near, sizeof(near));
    g_assert_cmpmem(near, sizeof(near), zeros, sizeof(zeros));
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x80), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x80), ==,
                    (1ULL << 20) | 1);

    qtest_writeq(qts, HEAD(0) + 0x80, (1ULL << 20) | 1);
    qtest_writel(qts, CARD(0), 0x80003000);
    qtest_writel(qts, CARD(0) + 8, 0x31);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, TARGET + 0x1000, near, sizeof(near));
    qtest_memread(qts, 0x80003000, far, sizeof(far));
    for (i = 0; i < sizeof(near); i++) {
        g_assert_cmpuint(near[i], ==, 0x11 + i);
        g_assert_cmpuint(far[i], ==, 0x31 + i);
    }
    qtest_memread(qts, TARGET + 0x3000, near, sizeof(near));
    g_assert_cmpmem(near, sizeof(near), zeros, sizeof(zeros));
    g_assert_cmphex(qtest_readq(qts, HEAD(1) + 0x80), ==,
                    (1ULL << 20) | 1);
    qtest_writel(qts, CARD(1) + 8, 0x41);
    qtest_writel(qts, CARD(1) + 0xc, 1);
    qtest_memread(qts, 0x80002000, far, sizeof(far));
    for (i = 0; i < sizeof(far); i++) {
        g_assert_cmpuint(far[i], ==, 0x41 + i);
    }
    qtest_quit(qts);
}

static void test_direct_dma_end(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-dma-XXXXXX", &error);
    g_autofree char *log_path = NULL;
    g_autofree char *log_contents = NULL;
    QTestState *qts;
    uint8_t sentinel[4] = { 0xa5, 0xa5, 0xa5, 0xa5 };
    uint8_t after[4];
    uint8_t bytes[4];
    unsigned i;

    g_assert_no_error(error);
    log_path = g_build_filename(dir, "qemu.log", NULL);
    qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors -D %s "
        "-device sgi-xio-test-target,id=last,size=4096 "
        "-device sgi-xio-test-target,id=outside,size=4096 "
        "-device sgi-pic,id=pic,test-fixture=on,"
        "target-0=/machine/unattached/system[0] "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1",
        log_path);

    qtest_sysbus_mmio_map(qts, PIC, 0, BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/last", 0, 0x7ffff000);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/outside", 0, 0x80000000);
    qtest_memwrite(qts, 0x80000000, sentinel, sizeof(sentinel));
    qtest_writel(qts, HEAD(0) + 0x21010, 0x01000000);
    qtest_writel(qts, HEAD(0) + 0x21004, 6);
    qtest_writeq(qts, HEAD(0) + 0x80, 0);
    qtest_writel(qts, CARD(0), 0xffffffff);
    qtest_writel(qts, CARD(0) + 4, 1);
    qtest_writel(qts, CARD(0) + 8, 0x5a);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    g_assert_cmphex(qtest_readb(qts, 0x7fffffff), ==, 0x5a);

    /* QEMU splits this at the IOMMU boundary; the first four bytes land. */
    qtest_writel(qts, CARD(0), 0xfffffffc);
    qtest_writel(qts, CARD(0) + 4, 8);
    qtest_writel(qts, CARD(0) + 8, 0x90);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, 0x7ffffffc, bytes, sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x90 + i);
    }
    qtest_memread(qts, 0x80000000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), sentinel, sizeof(sentinel));

    qtest_writel(qts, CARD(0) + 0x18, 1);
    qtest_writel(qts, CARD(0), 0x80000000);
    qtest_writel(qts, CARD(0) + 4, 1);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_writel(qts, CARD(0) + 0x18, 0);
    qtest_writel(qts, CARD(0), 0x40000000);
    qtest_writel(qts, CARD(0) + 0xc, 1);
    qtest_memread(qts, 0x80000000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), sentinel, sizeof(sentinel));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log_contents,
                            "sgi-pic: head 0 unsupported DMA 0x100000000"));
    g_assert_nonnull(strstr(log_contents,
                            "sgi-pic: head 0 unsupported DMA 0x180000000"));
    g_assert_nonnull(strstr(log_contents,
                            "sgi-pic: head 0 unsupported DMA 0x40000000"));
    unlink(log_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_pending_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = pic_start(false, "");
    QTestState *to = pic_start(false, "-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_writeq(from, HEAD(0) + 0x130, 0x1234000000001000ULL);
    qtest_writeq(from, HEAD(0) + 0x108, 1);
    qtest_writeq(from, HEAD(0) + 0x118, 1);
    qtest_set_irq_in(from, PIC "/head-0", NULL, 0, 1);
    g_assert_cmphex(qtest_readq(from, TARGET + 0x1000), ==, 0x11234);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x100), ==, 1);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x108), ==, 1);
    qtest_writeq(to, TARGET + 0x1000, 0);
    qtest_writeq(to, HEAD(0) + 0x110, 1);
    g_assert_cmphex(qtest_readq(to, HEAD(0) + 0x100), ==, 1);
    g_assert_cmphex(qtest_readq(to, TARGET + 0x1000), ==, 0x11234);
    qtest_set_irq_in(to, PIC "/head-0", NULL, 0, 0);
    g_assert_cmphex(qtest_readq(to, TARGET + 0x1000), ==, 0x1234);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_unwired_pci_interrupt(void)
{
    QTestState *qts = pic_start(true, "");

    qtest_writel(qts, HEAD(0) + 0x21010, 0x01000000);
    qtest_writel(qts, HEAD(0) + 0x21004, 6);
    qtest_irq_intercept_out_named(qts, PIC "/head-0", "pci-intx");
    qtest_writel(qts, CARD(0) + 0x10, 1);
    g_assert_true(qtest_get_irq(qts, 4));
    /* The machine must connect that output to one of eight PIC pin inputs. */
    g_assert_cmphex(qtest_readq(qts, HEAD(0) + 0x100), ==, 0);
    qtest_writel(qts, CARD(0) + 0x10, 0);
    g_assert_false(qtest_get_irq(qts, 4));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi-pic/heads-and-widths", test_heads_and_widths);
    qtest_add_func("/sgi-pic/configuration", test_configuration);
    qtest_add_func("/sgi-pic/slot-bounds", test_slot_bounds);
    qtest_add_func("/sgi-pic/unresolved-default-identity",
                   test_unresolved_default_identity);
    qtest_add_func("/sgi-pic/type1-configuration", test_type1_configuration);
    qtest_add_func("/sgi-pic/unsupported-registers",
                   test_unsupported_registers);
    qtest_add_func("/sgi-pic/pin-and-reset", test_pin_and_reset);
    qtest_add_func("/sgi-pic/remaining-pin-edges",
                   test_remaining_pin_edges);
    qtest_add_func("/sgi-pic/direct-dma", test_direct_dma);
    qtest_add_func("/sgi-pic/direct-dma-head-targets",
                   test_direct_dma_head_targets);
    qtest_add_func("/sgi-pic/direct-dma-end", test_direct_dma_end);
    qtest_add_func("/sgi-pic/pending-migration", test_pending_migration);
    qtest_add_func("/sgi-pic/unwired-pci-interrupt",
                   test_unwired_pci_interrupt);
    return g_test_run();
}
