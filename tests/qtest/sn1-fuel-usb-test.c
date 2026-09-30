/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fuel USB transfers through the onboard OHCI controller and XBridge DMA. */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"

#define XBRIDGE (SPACE_IO + 0x0f000000)
#define OHCI (XBRIDGE + 0x900000)
#define CONFIG (XBRIDGE + 0x25000)
#define DMA_PHYS 0x100000
#define DMA_IOVA 0x40000000
#define ED 0x100
#define TD_SETUP 0x110
#define TD_DATA 0x120
#define TD_STATUS 0x130
#define TD_TAIL 0x140
#define SETUP 0x200
#define DATA 0x300
#define PORT(n) (0x54 + 4 * ((n) - 1))
#define PORT_CCS 1
#define PORT_PES 2
#define PORT_CSC (1U << 16)
#define PORT_PRSC (1U << 20)
#define TD_NOT_ACCESSED (15U << 28)
#define TD_DATA0 (2U << 24)
#define TD_DATA1 (3U << 24)
#define TD_IN (2U << 19)
#define TD_OUT (1U << 19)
#define TD_ROUND (1U << 18)

static uint32_t dma_read(QTestState *qts, unsigned offset)
{
    uint8_t word[4];

    qtest_memread(qts, DMA_PHYS + offset, word, sizeof(word));
    return ldl_le_p(word);
}

static void dma_write(QTestState *qts, unsigned offset, uint32_t value)
{
    uint8_t word[4];

    stl_le_p(word, value);
    qtest_memwrite(qts, DMA_PHYS + offset, word, sizeof(word));
}

static void write_td(QTestState *qts, unsigned offset, uint32_t flags,
                     uint32_t buffer, uint32_t next, unsigned length)
{
    dma_write(qts, offset, TD_NOT_ACCESSED | flags);
    dma_write(qts, offset + 4, length ? DMA_IOVA + buffer : 0);
    dma_write(qts, offset + 8, DMA_IOVA + next);
    dma_write(qts, offset + 12, length ? DMA_IOVA + buffer + length - 1 : 0);
}

/* OpenHCI 1.0a sections 4.2 and 4.3: little-endian ED and general TDs. */
static void get_descriptor(QTestState *qts, unsigned length)
{
    uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, length, 0 };
    uint8_t data[32];
    unsigned received = MIN(length, 18);
    /* usb_desc_device() advertises USB 2.0 with the MS OS descriptor. */
    static const uint8_t prefix[] = { 18, 1, 0, 2, 0, 0, 0, 8 };
    unsigned attempt;

    memset(data, 0xa5, sizeof(data));
    qtest_memwrite(qts, DMA_PHYS + SETUP, setup, sizeof(setup));
    qtest_memwrite(qts, DMA_PHYS + DATA, data, sizeof(data));
    dma_write(qts, ED, 8U << 16);
    dma_write(qts, ED + 4, DMA_IOVA + TD_TAIL);
    dma_write(qts, ED + 8, DMA_IOVA + TD_SETUP);
    dma_write(qts, ED + 12, 0);
    write_td(qts, TD_SETUP, TD_DATA0, SETUP, TD_DATA, 8);
    write_td(qts, TD_DATA, TD_DATA1 | TD_IN | TD_ROUND,
             DATA, TD_STATUS, length);
    write_td(qts, TD_STATUS, TD_DATA1 | TD_OUT, 0, TD_TAIL, 0);
    g_assert_cmphex(dma_read(qts, ED + 8), ==, DMA_IOVA + TD_SETUP);
    qtest_writel(qts, OHCI + 0x20, DMA_IOVA + ED);
    qtest_writel(qts, OHCI + 0x08, 2); /* ControlListFilled */
    /* Operational, control list enabled. */
    qtest_writel(qts, OHCI + 0x04, 0x90);

    for (attempt = 0; attempt < 64; attempt++) {
        qtest_clock_step_next(qts);
        if ((dma_read(qts, ED + 8) & ~15U) == DMA_IOVA + TD_TAIL) {
            break;
        }
    }
    if (attempt == 64) {
        g_test_message("ED head=%08x setup=%08x data=%08x status=%08x "
                       "HcControl=%08x HcCommandStatus=%08x "
                       "HcInterruptStatus=%08x HcFmNumber=%08x",
                       dma_read(qts, ED + 8), dma_read(qts, TD_SETUP),
                       dma_read(qts, TD_DATA), dma_read(qts, TD_STATUS),
                       qtest_readl(qts, OHCI + 4),
                       qtest_readl(qts, OHCI + 8),
                       qtest_readl(qts, OHCI + 12),
                       qtest_readl(qts, OHCI + 0x3c));
    }
    g_assert_cmpuint(attempt, <, 64);
    g_assert_cmphex(dma_read(qts, ED + 8) & 1, ==, 0);
    for (unsigned offset = TD_SETUP; offset <= TD_STATUS; offset += 16) {
        uint32_t remaining = offset == TD_DATA && received < length ?
                             DMA_IOVA + DATA + received : 0;

        g_assert_cmphex(dma_read(qts, offset) >> 28, ==, 0);
        g_assert_cmphex(dma_read(qts, offset + 4), ==, remaining);
    }
    qtest_writel(qts, OHCI + 0x04, 0x80);
    qtest_memread(qts, DMA_PHYS + DATA, data, sizeof(data));
    g_test_message("descriptor: %02x %02x %02x %02x %02x %02x %02x %02x",
                   data[0], data[1], data[2], data[3],
                   data[4], data[5], data[6], data[7]);
    g_assert_cmpmem(data, sizeof(prefix), prefix, sizeof(prefix));
    /* DATA1 toggles once per packet, including the final short packet. */
    g_assert_cmphex((dma_read(qts, TD_DATA) >> 24) & 3, ==,
                    3 ^ (((received + 7) / 8) & 1));
    if (received == 18) {
        /* Native QEMU usb-kbd declares one configuration. */
        g_assert_cmphex(data[17], ==, 1);
    }
    for (unsigned i = received; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }
}

typedef enum PayloadFault {
    DMA_NONE,
    DMA_SETUP,
    DMA_OUT,
    DMA_IN,
} PayloadFault;

typedef struct FuelUSBTest {
    const char *name;
    unsigned port;
    PayloadFault fault;
    bool periodic;
} FuelUSBTest;

/* Only payload page 2 lacks an XBridge ATE; descriptors stay in page 0. */
static void test_payload_dma(QTestState *qts, PayloadFault fault, bool periodic)
{
    bool input = fault == DMA_IN;
    unsigned failing = input ? TD_DATA : TD_SETUP;
    uint32_t flags = input ? TD_DATA1 | TD_IN :
                     fault == DMA_OUT ? TD_DATA1 | TD_OUT : TD_DATA0;
    uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 8, 0 };
    uint32_t saved[4];
    uint32_t frame, hcca_frame;
    QDict *response;

    qtest_memwrite(qts, DMA_PHYS + SETUP, setup, sizeof(setup));
    if (input) {
        /* Finish SETUP before selecting an inaccessible IN destination. */
        dma_write(qts, ED, 8U << 16);
        dma_write(qts, ED + 4, DMA_IOVA + TD_TAIL);
        dma_write(qts, ED + 8, DMA_IOVA + TD_SETUP);
        dma_write(qts, ED + 12, 0);
        write_td(qts, TD_SETUP, TD_DATA0, SETUP, TD_TAIL, 8);
        qtest_writel(qts, OHCI + 0x20, DMA_IOVA + ED);
        qtest_writel(qts, OHCI + 0x08, 2);
        qtest_writel(qts, OHCI + 0x04, 0x90);
        for (unsigned i = 0; i < 64; i++) {
            qtest_clock_step_next(qts);
            if ((dma_read(qts, ED + 8) & ~15U) == DMA_IOVA + TD_TAIL) {
                break;
            }
        }
        g_assert_cmphex(dma_read(qts, ED + 8) & ~15U,
                        ==, DMA_IOVA + TD_TAIL);
        qtest_writel(qts, OHCI + 0x04, 0x80);
    }

    /* Reachable later TD, next ED, control list and bulk list. */
    dma_write(qts, ED, 8U << 16);
    dma_write(qts, ED + 4, DMA_IOVA + TD_TAIL);
    dma_write(qts, ED + 8, DMA_IOVA + failing);
    dma_write(qts, ED + 12, DMA_IOVA + 0x400);
    write_td(qts, failing, flags, 0x2000, TD_STATUS, 8);
    write_td(qts, TD_STATUS, TD_DATA0, SETUP, TD_TAIL, 8);
    for (unsigned ed = 0x400; ed <= 0x600; ed += 0x100) {
        dma_write(qts, ed, 8U << 16);
        dma_write(qts, ed + 4, DMA_IOVA + ed + 0x20);
        dma_write(qts, ed + 8, DMA_IOVA + ed + 0x10);
        dma_write(qts, ed + 12, 0);
        write_td(qts, ed + 0x10, TD_DATA0, SETUP, ed + 0x20, 8);
    }
    for (unsigned i = 0; i < 4; i++) {
        saved[i] = dma_read(qts, failing + 4 * i);
    }
    dma_write(qts, 0x84, 0); /* HCCA done head */
    qtest_writel(qts, OHCI + 0x0c, 0xffffffff);
    response = qtest_qmp(qts, "{ 'execute': 'trace-event-set-state', "
                              "'arguments': { "
                              "'name': 'usb_ohci_td_packet_status', "
                              "'enable': true } }");
    g_assert_true(qdict_haskey(response, "return"));
    qobject_unref(response);
    if (periodic) {
        for (unsigned i = 0; i < 32; i++) {
            dma_write(qts, 4 * i, DMA_IOVA + ED);
        }
    }
    qtest_writel(qts, OHCI + 0x20,
                 DMA_IOVA + (periodic ? 0x500 : ED));
    qtest_writel(qts, OHCI + 0x28, DMA_IOVA + 0x600);
    qtest_writel(qts, OHCI + 0x08, 6); /* ControlListFilled, BulkListFilled */
    qtest_writel(qts, OHCI + 0x04, periodic ? 0xb4 : 0xb0);
    frame = qtest_readl(qts, OHCI + 0x3c);
    hcca_frame = dma_read(qts, 0x80);
    qtest_clock_step_next(qts);
    g_assert_cmphex(qtest_readl(qts, OHCI + 0x0c) & (1U << 4),
                    ==, 1U << 4); /* UnrecoverableError */
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(dma_read(qts, failing + 4 * i), ==, saved[i]);
    }
    g_assert_cmphex(dma_read(qts, ED + 8), ==, DMA_IOVA + failing);
    g_assert_cmphex(dma_read(qts, TD_STATUS) >> 28, ==, 15);
    for (unsigned ed = 0x400; ed <= 0x600; ed += 0x100) {
        g_assert_cmphex(dma_read(qts, ed + 8), ==, DMA_IOVA + ed + 0x10);
        g_assert_cmphex(dma_read(qts, ed + 0x10) >> 28, ==, 15);
    }
    g_assert_cmphex(dma_read(qts, 0x84), ==, 0);
    g_assert_cmphex(qtest_readl(qts, OHCI + 0x3c), ==, frame);
    g_assert_cmphex(dma_read(qts, 0x80), ==, hcca_frame);
    qtest_clock_step(qts, 10000000);
    g_assert_cmphex(qtest_readl(qts, OHCI + 0x3c), ==, frame);
    g_assert_cmphex(dma_read(qts, ED + 8), ==, DMA_IOVA + failing);
}

static void test_port(const void *opaque)
{
    const FuelUSBTest *test = opaque;
    unsigned port = test->port;
    PayloadFault fault = test->fault;
    unsigned other = port == 1 ? 2 : 1;
    g_autoptr(GError) error = NULL;
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *trace_path = g_strdup_printf("fuel-usb-%u-%u.trace",
                                                 getpid(), g_random_int());
    g_autofree char *trace = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    uint8_t page[4096] = { 0 };
    QTestState *qts;
    int fd = g_file_open_tmp("sn1-fuel-usb-prom-XXXXXX", &prom, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(prom);
    qts = qtest_initf("-M fuel,fuel-board-id-word=0x4abc,"
                      "fuel-ioc3-subsystem-id=0xc30a,fuel-l1-type-code=0x61,"
                      "fuel-bedrock-revision=0x2 -cpu R14000 -smp 1 "
                      "-m 512M -bios %s -nodefaults -trace file=%s",
                      quoted, trace_path);
    /* RAM banks need programming before CPU accesses or PCI DMA. */
    qtest_writeq(qts, SPACE_IO + 0x01000000 + 0x780008, 0x33);
    qtest_writel(qts, CONFIG + 0x10, 0x10000000);
    qtest_writel(qts, CONFIG + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, XBRIDGE + 0x200 + 5 * 8 + 4,
                 DEV_MEMORY_SPACE | 0x100);
    /* ATE no-swap keeps OHCI's little-endian descriptors intact. */
    qtest_writeq(qts, XBRIDGE + 0x10000,
                 DMA_PHYS | (UINT64_C(0xa) << 8) | (1U << 5) | 1);
    qtest_memwrite(qts, DMA_PHYS, page, sizeof(page));
    qtest_writel(qts, OHCI + 0x18, DMA_IOVA);
    qtest_writel(qts, OHCI + PORT(port), 1U << 8); /* SetPortPower */
    g_assert_cmphex(qtest_readl(qts, OHCI + PORT(port)) & PORT_CCS, ==, 0);
    qtest_qmp_device_add(qts, "usb-kbd", "keyboard",
                         "{'port': %s}", port == 1 ? "1" : "2");
    g_assert_cmphex(qtest_readl(qts, OHCI + PORT(port)) &
                    (PORT_CCS | PORT_CSC), ==, PORT_CCS | PORT_CSC);
    g_assert_cmphex(qtest_readl(qts, OHCI + PORT(other)) & PORT_CCS, ==, 0);
    qtest_writel(qts, OHCI + PORT(port), 1U << 4); /* SetPortReset */
    g_assert_cmphex(qtest_readl(qts, OHCI + PORT(port)) &
                    (PORT_PES | PORT_PRSC), ==, PORT_PES | PORT_PRSC);
    if (fault) {
        test_payload_dma(qts, fault, test->periodic);
    } else {
        get_descriptor(qts, 8);
        get_descriptor(qts, 16);
        get_descriptor(qts, 18);
        get_descriptor(qts, 32);
    }
    qtest_writel(qts, OHCI + PORT(port), PORT_CSC | PORT_PRSC);
    qtest_qmp_device_del(qts, "keyboard");
    g_assert_cmphex(qtest_readl(qts, OHCI + PORT(port)) &
                    (PORT_CCS | PORT_PES | PORT_CSC), ==, PORT_CSC);
    g_assert_cmphex(qtest_readl(qts, OHCI + PORT(other)) & PORT_CCS, ==, 0);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(prom), ==, 0);
    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, NULL));
    if (fault) {
        unsigned packets = 0;
        const char *cursor = trace;

        while ((cursor = strstr(cursor, "usb_ohci_td_packet_status"))) {
            packets++;
            cursor++;
        }
        g_assert_cmpuint(packets, ==, fault == DMA_IN ? 1 : 0);
    }
    g_assert_cmpint(g_unlink(trace_path), ==, 0);
}

int main(int argc, char **argv)
{
    static const FuelUSBTest tests[] = {
        { "port1-control-transfer", 1, DMA_NONE },
        { "port2-control-transfer", 2, DMA_NONE },
        { "control-setup-dma-error", 1, DMA_SETUP },
        { "control-out-dma-error", 1, DMA_OUT },
        { "control-in-dma-error", 1, DMA_IN },
        { "periodic-setup-dma-error", 1, DMA_SETUP, true },
        { "periodic-out-dma-error", 1, DMA_OUT, true },
        { "periodic-in-dma-error", 1, DMA_IN, true },
    };

    g_test_init(&argc, &argv, NULL);
    for (unsigned i = 0; i < G_N_ELEMENTS(tests); i++) {
        g_autofree char *name = g_strdup_printf("/fuel/usb/%s", tests[i].name);

        qtest_add_data_func(name, &tests[i], test_port);
    }
    return g_test_run();
}
