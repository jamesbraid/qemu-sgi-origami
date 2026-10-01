/* SPDX-License-Identifier: GPL-2.0-or-later */
/* OHCI general transfer packet accounting and failed payload DMA. */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define OHCI 0xe0000000
#define RAM 0x100000
#define ED 0x100
#define TD_SETUP 0x110
#define TD_DATA 0x120
#define TD_TAIL 0x140
#define SETUP 0x200
#define DATA 0x300
#define TD_NOT_ACCESSED (15U << 28)
#define TD_DATA0 (2U << 24)
#define TD_DATA1 (3U << 24)
#define TD_IN (2U << 19)
#define TD_ROUND (1U << 18)

static uint32_t rd(QTestState *qts, unsigned off)
{
    uint8_t word[4];

    qtest_memread(qts, RAM + off, word, sizeof(word));
    return ldl_le_p(word);
}

static void wr(QTestState *qts, unsigned off, uint32_t value)
{
    uint8_t word[4];

    stl_le_p(word, value);
    qtest_memwrite(qts, RAM + off, word, sizeof(word));
}

static void td(QTestState *qts, unsigned off, uint32_t flags,
               uint32_t payload, unsigned next, unsigned len)
{
    wr(qts, off, TD_NOT_ACCESSED | flags);
    wr(qts, off + 4, payload);
    wr(qts, off + 8, RAM + next);
    wr(qts, off + 12, len ? payload + len - 1 : 0);
}

static void ed(QTestState *qts, unsigned off, unsigned head,
               unsigned tail, unsigned next)
{
    wr(qts, off, 8U << 16);
    wr(qts, off + 4, RAM + tail);
    wr(qts, off + 8, RAM + head);
    wr(qts, off + 12, next ? RAM + next : 0);
}

static QTestState *start_device(const char *trace_path, bool audio)
{
    QTestState *qts = qtest_initf(
        "-machine pc -m 64M -nodefaults "
        "-device pci-ohci,id=ohci,addr=04.0 "
        "%s %s%s",
        audio ? "-audiodev none,id=audio "
                "-device usb-audio,bus=ohci.0,port=1,audiodev=audio,multi=on" :
                "-device usb-kbd,bus=ohci.0,port=1",
        trace_path ? "-trace file=" : "", trace_path ? trace_path : "");
    uint8_t zero[4096] = { 0 };

    qtest_outl(qts, 0xcf8, 0x80002010);
    qtest_outl(qts, 0xcfc, OHCI);
    qtest_outl(qts, 0xcf8, 0x80002004);
    qtest_outw(qts, 0xcfc, 6);
    g_assert_cmphex(qtest_readl(qts, OHCI), ==, 0x10);
    qtest_memwrite(qts, RAM, zero, sizeof(zero));
    qtest_writel(qts, OHCI + 0x18, RAM);
    qtest_writel(qts, OHCI + 0x54, 1U << 8);
    qtest_writel(qts, OHCI + 0x54, 1U << 4);
    g_assert_cmphex(qtest_readl(qts, OHCI + 0x54) & 3, ==, 3);
    return qts;
}

static bool descriptor(QTestState *qts, unsigned length)
{
    uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, length, 0 };
    uint8_t data[32];
    const uint8_t prefix[] = { 18, 1, 0, 2, 0, 0, 0, 8 };
    unsigned received = MIN(length, 18);
    unsigned expected_toggle = 3 ^ (((received + 7) / 8) & 1);
    unsigned expected_carry = expected_toggle & 1;
    uint32_t head, flags;
    unsigned attempt;

    memset(data, 0xa5, sizeof(data));
    qtest_memwrite(qts, RAM + SETUP, setup, sizeof(setup));
    qtest_memwrite(qts, RAM + DATA, data, sizeof(data));
    ed(qts, ED, TD_SETUP, TD_TAIL, 0);
    td(qts, TD_SETUP, TD_DATA0, RAM + SETUP, TD_DATA, 8);
    td(qts, TD_DATA, TD_DATA1 | TD_IN | TD_ROUND,
       RAM + DATA, TD_TAIL, length);
    qtest_writel(qts, OHCI + 0x20, RAM + ED);
    qtest_writel(qts, OHCI + 8, 2);
    qtest_writel(qts, OHCI + 4, 0x90);
    for (attempt = 0; attempt < 64; attempt++) {
        qtest_clock_step_next(qts);
        if ((rd(qts, ED + 8) & ~15U) == RAM + TD_TAIL) {
            break;
        }
    }
    head = rd(qts, ED + 8);
    flags = rd(qts, TD_DATA);
    qtest_memread(qts, RAM + DATA, data, sizeof(data));
    g_test_message("length=%u MPS=8 EDhead=%08x carry=%u "
                   "TDflags=%08x toggle=%u cbp=%08x expected-toggle=%u "
                   "expected-carry=%u bytes=%02x%02x%02x%02x%02x%02x%02x%02x",
                   length, head, (head >> 1) & 1, flags,
                   (flags >> 24) & 3, rd(qts, TD_DATA + 4),
                   expected_toggle, expected_carry,
                   data[0], data[1], data[2], data[3],
                   data[4], data[5], data[6], data[7]);
    g_assert_cmpuint(attempt, <, 64);
    g_assert_cmphex(head & 1, ==, 0);
    g_assert_cmphex(flags >> 28, ==, 0);
    g_assert_cmphex(rd(qts, TD_DATA + 4), ==,
                    received == length ? 0 : RAM + DATA + received);
    g_assert_cmpmem(data, sizeof(prefix), prefix, sizeof(prefix));
    for (unsigned i = received; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }
    qtest_writel(qts, OHCI + 4, 0x80);
    return ((flags >> 24) & 3) == expected_toggle &&
           ((head >> 1) & 1) == expected_carry;
}

static void test_descriptor(void)
{
    QTestState *qts = start_device(NULL, false);
    unsigned failures = 0;

    failures += !descriptor(qts, 8);
    failures += !descriptor(qts, 16);
    failures += !descriptor(qts, 18);
    failures += !descriptor(qts, 32);
    qtest_quit(qts);
    g_assert_cmpuint(failures, ==, 0);
}

static void test_long_descriptor(void)
{
    QTestState *qts = start_device(NULL, true);
    uint8_t setup[8] = { 0x80, 6, 0, 2, 0, 0, 0, 2 };
    uint8_t data[512];
    unsigned total_packets = 0;

    memset(data, 0xa5, sizeof(data));
    qtest_memwrite(qts, RAM + SETUP, setup, sizeof(setup));
    ed(qts, ED, 0x400, 0x490, 0);
    for (unsigned i = 0; i < 3; i++) {
        unsigned first = 0x400 + i * 0x30;
        unsigned buffer = 0x600 + i * sizeof(data);

        qtest_memwrite(qts, RAM + buffer, data, sizeof(data));
        td(qts, first, TD_DATA0, RAM + SETUP, first + 0x10, 8);
        td(qts, first + 0x10, TD_DATA1 | TD_IN | TD_ROUND,
           RAM + buffer, first + 0x20, sizeof(data));
        td(qts, first + 0x20, TD_DATA1 | (1U << 19),
           0, first + 0x30, 0);
    }
    qtest_writel(qts, OHCI + 0x20, RAM + ED);
    qtest_writel(qts, OHCI + 8, 2);
    qtest_writel(qts, OHCI + 4, 0x90);
    qtest_clock_step_next(qts);
    g_assert_cmphex(qtest_readl(qts, OHCI + 0xc) & 0x10, ==, 0);
    g_assert_cmphex(rd(qts, ED + 8) & ~15U, ==, RAM + 0x490);
    g_assert_cmphex(rd(qts, ED + 8) & 3, ==, 0);
    for (unsigned i = 0; i < 3; i++) {
        unsigned first = 0x400 + i * 0x30;
        unsigned buffer = 0x600 + i * sizeof(data);
        unsigned received, packets;
        uint32_t flags = rd(qts, first + 0x10);

        qtest_memread(qts, RAM + buffer, data, sizeof(data));
        received = lduw_le_p(data + 2);
        packets = received / 8 + 1;
        total_packets += packets + 2;
        g_test_message("configuration %u bytes=%u MPS=8 packets=%u "
                       "TDflags=%08x cbp=%08x",
                       i, received, packets, flags, rd(qts, first + 0x14));
        g_assert_cmphex(data[0], ==, 9);
        g_assert_cmphex(data[1], ==, 2);
        g_assert_cmpuint(received, <, sizeof(data));
        g_assert_cmphex(rd(qts, first) >> 28, ==, 0);
        g_assert_cmphex(flags >> 28, ==, 0);
        g_assert_cmphex(rd(qts, first + 0x20) >> 28, ==, 0);
        g_assert_cmphex((flags >> 24) & 3, ==, 3 ^ (packets & 1));
        g_assert_cmphex(rd(qts, first + 0x14), ==, RAM + buffer + received);
        for (unsigned j = received; j < sizeof(data); j++) {
            g_assert_cmphex(data[j], ==, 0xa5);
        }
    }
    /* Nine TDs require more packets than the descriptor loop limit. */
    g_assert_cmpuint(total_packets, >, 34);
    qtest_quit(qts);
}

static void test_payload_dma(void)
{
    g_autofree char *trace_path = g_strdup_printf("ohci-plain-pci-%u.trace",
                                                 getpid());
    g_autofree char *trace = NULL;
    QTestState *qts = start_device(trace_path, false);
    uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 8, 0 };
    QDict *response;
    uint32_t saved[4], interrupt, head, flags, later_head, later_flags;
    uint32_t frame, hcca_frame;
    unsigned packets = 0;
    const char *cursor;

    qtest_memwrite(qts, RAM + SETUP, setup, sizeof(setup));
    ed(qts, ED, TD_SETUP, TD_TAIL, 0x400);
    /* The payload is unassigned on this 64 MiB PC, outside BAR0 and ROM. */
    td(qts, TD_SETUP, TD_DATA0, 0x40002000, TD_TAIL, 8);
    ed(qts, 0x400, 0x410, 0x420, 0);
    td(qts, 0x410, TD_DATA0, RAM + SETUP, 0x420, 8);
    for (unsigned i = 0; i < 4; i++) {
        saved[i] = rd(qts, TD_SETUP + 4 * i);
    }
    response = qtest_qmp(qts, "{ 'execute': 'trace-event-set-state', "
                              "'arguments': { "
                              "'name': 'usb_ohci_td_packet_status', "
                              "'enable': true } }");
    g_assert_true(qdict_haskey(response, "return"));
    qobject_unref(response);
    qtest_writel(qts, OHCI + 0x20, RAM + ED);
    qtest_writel(qts, OHCI + 8, 2);
    qtest_writel(qts, OHCI + 4, 0x90);
    frame = qtest_readl(qts, OHCI + 0x3c);
    hcca_frame = rd(qts, 0x80);
    qtest_clock_step_next(qts);
    interrupt = qtest_readl(qts, OHCI + 0xc);
    head = rd(qts, ED + 8);
    flags = rd(qts, TD_SETUP);
    later_head = rd(qts, 0x408);
    later_flags = rd(qts, 0x410);
    g_test_message("fatal setup: interrupt=%08x UE=%u EDhead=%08x "
                   "TD=%08x/%08x/%08x/%08x saved=%08x/%08x/%08x/%08x "
                   "later-head=%08x later-TD=%08x HcControl=%08x",
                   interrupt, !!(interrupt & 0x10), head,
                   flags, rd(qts, TD_SETUP + 4), rd(qts, TD_SETUP + 8),
                   rd(qts, TD_SETUP + 12), saved[0], saved[1], saved[2],
                   saved[3], later_head, later_flags,
                   qtest_readl(qts, OHCI + 4));
    g_assert_cmphex(qtest_readl(qts, OHCI + 0x3c), ==, frame);
    g_assert_cmphex(rd(qts, 0x80), ==, hcca_frame);
    g_assert_cmphex(rd(qts, 0x84), ==, 0);
    qtest_clock_step(qts, 10000000);
    g_assert_cmphex(qtest_readl(qts, OHCI + 0x3c), ==, frame);
    g_assert_cmphex(rd(qts, 0x80), ==, hcca_frame);
    g_assert_cmphex(rd(qts, ED + 8), ==, RAM + TD_SETUP);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, NULL));
    cursor = trace;
    while ((cursor = strstr(cursor, "usb_ohci_td_packet_status"))) {
        packets++;
        cursor++;
    }
    g_test_message("USB submissions=%u retained trace=%s", packets, trace_path);
    /* UE distinguishes a real failed DMA transaction from a silent read. */
    g_assert_cmphex(interrupt & 0x10, ==, 0x10);
    g_assert_cmpuint(packets, ==, 0);
    g_assert_cmphex(later_head, ==, RAM + 0x410);
    g_assert_cmphex(later_flags >> 28, ==, 15);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ohci/plain-pci/descriptor", test_descriptor);
    qtest_add_func("/ohci/plain-pci/payload-dma", test_payload_dma);
    if (qtest_has_device("usb-audio")) {
        qtest_add_func("/ohci/plain-pci/long-descriptor", test_long_descriptor);
    }
    return g_test_run();
}
