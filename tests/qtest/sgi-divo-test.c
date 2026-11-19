/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * QTest for the DIVO board's LINC PCI function behind the Bridge: identity,
 * config space, the 128 MiB window, its special words, the interrupt, the
 * mailbox, the flash and the firmware stand-in's packets. sgi-divo-board-test.c
 * covers the board that places two of them.
 *
 * The facts, and the stand-in as an approximation, are in
 * docs/system/mips/sgi/hardware/divo.rst. A test marked "approximation" pins
 * a model choice, not a measured hardware fact.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "hw/pci/pci_regs.h"

#define LINC_VENDOR_ID      0x10a9
#define LINC_DEVICE_ID      0x0002
#define LINC_SLOT           2
#define BRIDGE_INT_STATUS   0x40000104ULL
#define TRANSFER_RAM        0x02000000ULL
/* The board's DMA names widget 8, which this head answers with a copy. */
#define HUB_ALIAS(a)        ((8ULL << 48) | (a))

#define CFG_CONSUMER        0x14
#define MAILBOX             0x4400000
#define VIP_ID_REG          0x4c00800
#define CTL                 0x7800000
#define STATUS              0x7800018
#define FLASH               0x7c00000

#define OP_RING_BASE        0x09
#define OP_RING_CONSUMED    0x0b
#define OP_READ_WORD        0x10
#define OP_READ_HALF        0x12
#define OP_BLOCK_SEND       0x20
#define OP_LOAD_FIRST       0x23
#define OP_LOAD_AUDIO       0x28
#define OP_UNKNOWN          0x3f

#define RING_ENTRIES        64
#define ENTRY               0x80
#define VALID               0x7c

typedef struct QDivo {
    QOSGraphObject obj;
    QPCIDevice dev;
} QDivo;

typedef struct Divo {
    QDivo *divo;
    QPCIDevice *dev;
    QTestState *qts;
    QGuestAllocator *alloc;
    QPCIBar bar;
    uint64_t ring;
} Divo;

static void *divo_get_driver(void *obj, const char *interface)
{
    QDivo *divo = obj;

    if (!g_strcmp0(interface, "pci-device")) {
        return &divo->dev;
    }
    fprintf(stderr, "%s not present in sgi-divo-linc\n", interface);
    g_assert_not_reached();
}

static void *divo_create(void *pci_bus, QGuestAllocator *alloc, void *addr)
{
    QDivo *divo = g_new0(QDivo, 1);

    qpci_device_init(&divo->dev, pci_bus, addr);
    divo->obj.get_driver = divo_get_driver;
    return &divo->obj;
}

static void divo_open(Divo *t, void *obj, QGuestAllocator *alloc)
{
    memset(t, 0, sizeof(*t));
    t->divo = obj;
    t->dev = &t->divo->dev;
    t->qts = t->dev->bus->qts;
    t->alloc = alloc;
    qtest_sysbus_mmio_map(t->qts, "/machine/peripheral/hub-ram", 0,
                          HUB_ALIAS(TRANSFER_RAM));
    qpci_device_enable(t->dev);
    t->bar = qpci_iomap(t->dev, 0, NULL);
}

static uint32_t rd(Divo *t, uint32_t off)
{
    return qpci_io_readl(t->dev, t->bar, off);
}

static void wr(Divo *t, uint32_t off, uint32_t val)
{
    qpci_io_writel(t->dev, t->bar, off, val);
}

static bool pin(Divo *t)
{
    return qtest_readl(t->qts, BRIDGE_INT_STATUS) & (1u << LINC_SLOT);
}

/* A flash byte at the host's offset sits at that offset xor 3 on the bus. */
static void fl_wb(Divo *t, uint32_t at, uint8_t val)
{
    qpci_io_writeb(t->dev, t->bar, FLASH + (at ^ 3), val);
}

static uint8_t fl_rb(Divo *t, uint32_t at)
{
    return qpci_io_readb(t->dev, t->bar, FLASH + (at ^ 3));
}

static void fl_cmd(Divo *t, uint8_t cmd)
{
    fl_wb(t, 0x5555, 0xaa);
    fl_wb(t, 0x2aaa, 0x55);
    fl_wb(t, 0x5555, cmd);
}

/* The stand-in answers after a short delay, as a board's firmware would. */
static void settle(Divo *t)
{
    qtest_clock_step(t->qts, 1000000);
}

/* The host sends a word as two word cycles, the upper half first. */
static void mb_send(Divo *t, unsigned slot, uint64_t word)
{
    wr(t, MAILBOX + slot * 0x10000, word >> 32);
    wr(t, MAILBOX + slot * 0x10000 + 4, word);
    settle(t);
}

static uint64_t op_word(unsigned op, uint64_t payload)
{
    return ((uint64_t)op << 58) | payload;
}

/* A DMA address field: address >> 7 in bits 40:0. */
static uint64_t dma_field(uint64_t addr)
{
    return addr >> 7;
}

static void ring_setup(Divo *t)
{
    t->ring = guest_alloc(t->alloc, RING_ENTRIES * ENTRY);
    qtest_memset(t->qts, HUB_ALIAS(t->ring), 0, RING_ENTRIES * ENTRY);
    mb_send(t, 0, op_word(OP_RING_BASE, dma_field(t->ring)));
}

/* A word-value DMA window delivers the device's dwords as host numbers. */
static uint32_t entry_word(Divo *t, unsigned entry, unsigned dword)
{
    uint8_t bytes[4];

    qtest_memread(t->qts, HUB_ALIAS(t->ring) + entry * ENTRY + dword * 4,
                  bytes, 4);
    return ldl_be_p(bytes);
}

static bool entry_valid(Divo *t, unsigned entry)
{
    return entry_word(t, entry, VALID / 4) != 0;
}

static void ack(Divo *t)
{
    wr(t, STATUS, 1u << 20);
}

static void test_identity(void *obj, void *data, QGuestAllocator *alloc)
{
    QDivo *divo = obj;
    QPCIDevice *dev = &divo->dev;

    g_assert_cmphex(qpci_config_readw(dev, PCI_VENDOR_ID), ==, LINC_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==, LINC_DEVICE_ID);
    g_assert_cmphex(qpci_config_readb(dev, PCI_INTERRUPT_PIN), ==, 1);
}

/* Dword 0x10 is a 128 MiB window; dword 0x14 is not a BAR. */
static void test_config(void *obj, void *data, QGuestAllocator *alloc)
{
    QDivo *divo = obj;
    QPCIDevice *dev = &divo->dev;

    qpci_config_writel(dev, PCI_BASE_ADDRESS_0, 0xffffffff);
    g_assert_cmphex(qpci_config_readl(dev, PCI_BASE_ADDRESS_0), ==, 0xf8000000);
    g_assert_cmphex(qpci_config_readl(dev, CFG_CONSUMER), ==, 0x1f);
    qpci_config_writel(dev, CFG_CONSUMER, 0xffffffff);
    g_assert_cmphex(qpci_config_readl(dev, CFG_CONSUMER), ==, 0x1f);
    g_assert_cmphex(qpci_config_readb(dev, CFG_CONSUMER), ==, 0x1f);
    g_assert_cmphex(qpci_config_readb(dev, CFG_CONSUMER + 1), ==, 0);
}

/* Everything outside the special words is plain storage, to the last word. */
static void test_storage(void *obj, void *data, QGuestAllocator *alloc)
{
    static const uint32_t offsets[] = {
        0x0, 0x100, 0x4c00804, 0x4c00f10, 0x7800008, 0x7800020, 0x7802008,
        0x781000c, 0x7e08000, 0x7ffffff - 3,
    };
    Divo t;

    divo_open(&t, obj, alloc);
    for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
        wr(&t, offsets[i], 0x70345600 + i);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
        g_assert_cmphex(rd(&t, offsets[i]), ==, 0x70345600 + i);
    }
}

static void test_special_words(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;

    divo_open(&t, obj, alloc);
    g_assert_cmphex(rd(&t, VIP_ID_REG), ==, 0x000810a9);
    wr(&t, VIP_ID_REG, 0);
    g_assert_cmphex(rd(&t, VIP_ID_REG), ==, 0x000810a9);
    /* Bit 6 of the control word reads 0, whatever was written. */
    wr(&t, CTL, 0xffffffff);
    g_assert_cmphex(rd(&t, CTL), ==, 0xffffffbf);
    wr(&t, CTL, 0x40);
    g_assert_cmphex(rd(&t, CTL), ==, 0);
}

/* A packet raises INTA; write-1-to-clear drops it; a new one raises it. */
static void test_interrupt(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;

    divo_open(&t, obj, alloc);
    ring_setup(&t);
    ack(&t);
    g_assert(!pin(&t));
    mb_send(&t, 1, op_word(OP_READ_WORD, 0x100));
    g_assert(pin(&t));
    g_assert_cmphex(rd(&t, STATUS) & (3u << 20), ==, 1u << 20);
    /* A write of a zero bit changes nothing. */
    wr(&t, STATUS, 0);
    g_assert(pin(&t));
    ack(&t);
    g_assert(!pin(&t));
    g_assert_cmphex(rd(&t, STATUS) & (3u << 20), ==, 0);
    mb_send(&t, 2, op_word(OP_READ_WORD, 0x104));
    g_assert(pin(&t));
    /* Bits 21 and 20 clear together; the panic bit never sets. */
    wr(&t, STATUS, 3u << 20);
    g_assert(!pin(&t));
    g_assert_cmphex(rd(&t, STATUS) & (1u << 21), ==, 0);
    /* The other bits are storage. */
    wr(&t, STATUS, 0x01000000);
    g_assert_cmphex(rd(&t, STATUS), ==, 0x01000000);
}

/*
 * The index follows the last slot written and goes back to the host as a
 * packet; a release from reset resets it. Approximation, as the packet's
 * form is the model's choice.
 */
static void test_consumer(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;

    divo_open(&t, obj, alloc);
    g_assert_cmphex(qpci_config_readl(t.dev, CFG_CONSUMER), ==, 0x1f);
    ring_setup(&t);
    g_assert_cmphex(qpci_config_readl(t.dev, CFG_CONSUMER), ==, 0);
    g_assert(entry_valid(&t, 0));
    g_assert_cmphex(entry_word(&t, 0, 0), ==, 0x01000000);
    g_assert_cmphex(entry_word(&t, 0, 1), ==, 0);
    mb_send(&t, 7, op_word(0x0a, 0x40));
    g_assert_cmphex(qpci_config_readl(t.dev, CFG_CONSUMER), ==, 7);
    g_assert_cmphex(entry_word(&t, 1, 0), ==, 0x01000000);
    g_assert_cmphex(entry_word(&t, 1, 1), ==, 7);
    /* A half-sent word completes nothing. */
    wr(&t, MAILBOX + 9 * 0x10000, 0);
    g_assert_cmphex(qpci_config_readl(t.dev, CFG_CONSUMER), ==, 7);
    wr(&t, CTL, 0x0c);
    g_assert_cmphex(qpci_config_readl(t.dev, CFG_CONSUMER), ==, 7);
    wr(&t, CTL, 0);
    g_assert_cmphex(qpci_config_readl(t.dev, CFG_CONSUMER), ==, 0x1f);
}

/* Approximation: the word reads and halfword reads get their reply types. */
static void test_standin_replies(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;

    divo_open(&t, obj, alloc);
    ring_setup(&t);
    /* Entry 0 is the consumer packet for the announcement itself. */
    mb_send(&t, 1, op_word(OP_READ_WORD, 0x7e08000));
    g_assert_cmphex(entry_word(&t, 1, 0), ==, 0x01000000);
    g_assert(entry_valid(&t, 2));
    g_assert_cmphex(entry_word(&t, 2, 0), ==, 0x10000);
    g_assert_cmphex(entry_word(&t, 2, 1), ==, 0);
    g_assert_cmphex(entry_word(&t, 2, 2), ==, 0);
    g_assert(!entry_valid(&t, 3));
    mb_send(&t, 2, op_word(OP_READ_HALF, 0x5000004));
    g_assert(entry_valid(&t, 4));
    g_assert_cmphex(entry_word(&t, 4, 0), ==, 0x20000);
}

/* Approximation: block sends and loads read their DMA and complete. */
static void test_standin_blocks(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;
    uint64_t block;

    divo_open(&t, obj, alloc);
    ring_setup(&t);
    block = guest_alloc(alloc, 1024);
    qtest_memset(t.qts, HUB_ALIAS(block), 0xa5, 1024);

    /* Three command words at the block, scratch slot 2. Each command */
    /* has a consumer packet before its completion. */
    mb_send(&t, 1, op_word(OP_BLOCK_SEND, (3ULL << 51) | (2ULL << 49) |
                                          dma_field(block)));
    g_assert_cmphex(entry_word(&t, 2, 0), ==, 0x08000000);
    g_assert_cmphex(entry_word(&t, 2, 2), ==, 2);

    mb_send(&t, 2, op_word(OP_LOAD_FIRST, (1ULL << 49) | dma_field(block)));
    g_assert_cmphex(entry_word(&t, 4, 0), ==, 0x20000000);
    g_assert_cmphex(entry_word(&t, 4, 1), ==, 1);

    mb_send(&t, 3, op_word(OP_LOAD_AUDIO, (3ULL << 49) | dma_field(block)));
    g_assert_cmphex(entry_word(&t, 6, 0), ==, 0x40000000);
    g_assert_cmphex(entry_word(&t, 6, 1), ==, 3);
}

/* A word the stand-in does not know is consumed and gets no other packet. */
static void test_standin_unknown(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;

    divo_open(&t, obj, alloc);
    ring_setup(&t);
    mb_send(&t, 1, op_word(OP_UNKNOWN, 0));
    g_assert_cmphex(entry_word(&t, 1, 0), ==, 0x01000000);
    g_assert(!entry_valid(&t, 2));
}

/* The next entry follows the host's report; an unconsumed entry is kept. */
static void test_ring_position(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;

    divo_open(&t, obj, alloc);
    ring_setup(&t);
    mb_send(&t, 1, op_word(OP_RING_CONSUMED, 9));
    /* The report is not itself acknowledged. */
    g_assert(!entry_valid(&t, 1));
    qtest_writel(t.qts, HUB_ALIAS(t.ring) + 11 * ENTRY + VALID, 1);
    mb_send(&t, 2, op_word(OP_READ_WORD, 0));
    g_assert(entry_valid(&t, 10));
    g_assert_cmphex(entry_word(&t, 10, 0), ==, 0x01000000);
    /* A valid entry is not overwritten; the reply is dropped. */
    g_assert_cmphex(entry_word(&t, 11, 0), ==, 0);
    g_assert(!entry_valid(&t, 12));

    /* The ring wraps after 64 entries; entry 0 still holds an old packet. */
    mb_send(&t, 5, op_word(OP_RING_CONSUMED, 62));
    mb_send(&t, 6, op_word(OP_READ_WORD, 0));
    g_assert_cmphex(entry_word(&t, 63, 0), ==, 0x01000000);
    g_assert_cmphex(entry_word(&t, 63, 1), ==, 6);
    g_assert_cmphex(entry_word(&t, 0, 1), ==, 0);
}

/* No packet is sent before the host announces its ring. */
static void test_no_ring(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;

    divo_open(&t, obj, alloc);
    mb_send(&t, 1, op_word(OP_READ_WORD, 0));
    g_assert(!pin(&t));
}

static void test_flash_identity(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;

    divo_open(&t, obj, alloc);
    /* Erased. */
    g_assert_cmphex(fl_rb(&t, 0), ==, 0xff);
    g_assert_cmphex(fl_rb(&t, 0x7ffff), ==, 0xff);
    fl_cmd(&t, 0x90);
    g_assert_cmphex(fl_rb(&t, 0), ==, 0x1f);
    g_assert_cmphex(fl_rb(&t, 1), ==, 0xa4);
    /* The halfword at host offset 0 sits at bus offset 2. */
    g_assert_cmphex(qpci_io_readw(t.dev, t.bar, FLASH + 2), ==, 0x1fa4);
    fl_wb(&t, 0, 0xf0);
    g_assert_cmphex(fl_rb(&t, 0), ==, 0xff);
}

static void test_flash_program_erase(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    Divo t;

    divo_open(&t, obj, alloc);
    /* One sector, 256 bytes in ascending order, then it reads back. */
    fl_cmd(&t, 0xa0);
    for (unsigned i = 0; i < 256; i++) {
        fl_wb(&t, 0x1100 + i, i ^ 0x5a);
    }
    for (unsigned i = 0; i < 256; i++) {
        g_assert_cmphex(fl_rb(&t, 0x1100 + i), ==, i ^ 0x5a);
    }
    /* The load ended with the sector: the next byte is not data. */
    fl_wb(&t, 0x1200, 0x11);
    g_assert_cmphex(fl_rb(&t, 0x1200), ==, 0xff);

    /* Sector erase of the written sector only. */
    fl_cmd(&t, 0xa0);
    for (unsigned i = 0; i < 256; i++) {
        fl_wb(&t, 0x1200 + i, 0x77);
    }
    fl_cmd(&t, 0x80);
    fl_wb(&t, 0x5555, 0xaa);
    fl_wb(&t, 0x2aaa, 0x55);
    fl_wb(&t, 0x1180, 0x30);
    g_assert_cmphex(fl_rb(&t, 0x1100), ==, 0xff);
    g_assert_cmphex(fl_rb(&t, 0x11ff), ==, 0xff);
    g_assert_cmphex(fl_rb(&t, 0x1200), ==, 0x77);

    /* One prefix erases further sectors until 0xf0 ends it. */
    fl_cmd(&t, 0xa0);
    for (unsigned i = 0; i < 256; i++) {
        fl_wb(&t, 0x2000 + i, 0x33);
    }
    fl_cmd(&t, 0xa0);
    for (unsigned i = 0; i < 256; i++) {
        fl_wb(&t, 0x2100 + i, 0x44);
    }
    fl_cmd(&t, 0x80);
    fl_wb(&t, 0x5555, 0xaa);
    fl_wb(&t, 0x2aaa, 0x55);
    fl_wb(&t, 0x2000, 0x30);
    fl_wb(&t, 0x2100, 0x30);
    g_assert_cmphex(fl_rb(&t, 0x2000), ==, 0xff);
    g_assert_cmphex(fl_rb(&t, 0x2100), ==, 0xff);
    g_assert_cmphex(fl_rb(&t, 0x1200), ==, 0x77);
    fl_wb(&t, 0, 0xf0);
    /* Instant completion is the model's choice; no source checks it. */
}

/* A write that is not part of a command changes nothing. */
static void test_flash_stray(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;

    divo_open(&t, obj, alloc);
    fl_wb(&t, 0x7034, 0x42);
    g_assert_cmphex(fl_rb(&t, 0x7034), ==, 0xff);
    fl_wb(&t, 0x5555, 0xaa);
    fl_wb(&t, 0x2aaa, 0x99);
    fl_wb(&t, 0x5555, 0x90);
    g_assert_cmphex(fl_rb(&t, 0), ==, 0xff);
}

/* An access the window cannot take as a word cycle is refused. */
static void test_narrow_mailbox(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;

    divo_open(&t, obj, alloc);
    mb_send(&t, 0, op_word(0x0a, 0x40));
    qpci_io_writeb(t.dev, t.bar, MAILBOX + 5 * 0x10000 + 3, 0xff);
    g_assert_cmphex(qpci_config_readl(t.dev, CFG_CONSUMER), ==, 0);
}

static void divo_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "addr=02.0",
        .before_cmd_line = "-device sgi-xio-test-target,id=hub-ram,size=16M",
    };

    add_qpci_address(&opts,
                     &(QPCIAddress) { .devfn = QPCI_DEVFN(LINC_SLOT, 0) });

    qos_node_create_driver("sgi-divo-linc", divo_create);
    qos_node_consumes("sgi-divo-linc", "sgi-pci-bus", &opts);
    qos_node_produces("sgi-divo-linc", "pci-device");

    qos_add_test("identity", "sgi-divo-linc", test_identity, NULL);
    qos_add_test("config", "sgi-divo-linc", test_config, NULL);
    qos_add_test("storage", "sgi-divo-linc", test_storage, NULL);
    qos_add_test("special-words", "sgi-divo-linc", test_special_words, NULL);
    qos_add_test("interrupt", "sgi-divo-linc", test_interrupt, NULL);
    qos_add_test("consumer", "sgi-divo-linc", test_consumer, NULL);
    qos_add_test("standin-replies", "sgi-divo-linc", test_standin_replies,
                 NULL);
    qos_add_test("standin-blocks", "sgi-divo-linc", test_standin_blocks,
                 NULL);
    qos_add_test("standin-unknown", "sgi-divo-linc", test_standin_unknown,
                 NULL);
    qos_add_test("ring-position", "sgi-divo-linc", test_ring_position, NULL);
    qos_add_test("no-ring", "sgi-divo-linc", test_no_ring, NULL);
    qos_add_test("flash-identity", "sgi-divo-linc", test_flash_identity,
                 NULL);
    qos_add_test("flash-program-erase", "sgi-divo-linc",
                 test_flash_program_erase, NULL);
    qos_add_test("flash-stray", "sgi-divo-linc", test_flash_stray, NULL);
    qos_add_test("narrow-mailbox", "sgi-divo-linc", test_narrow_mailbox,
                 NULL);
}

libqos_init(divo_register_nodes);
