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
#include <math.h>
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
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

#define CFG_BAR1            0x14
#define MAILBOX             0x4400000
#define VIP_ID_REG          0x4c00800
#define CTL                 0x7800000
#define STATUS              0x7800018
#define FLASH               0x7c00000

#define OP_CONTROL          0x03
#define OP_RING_BASE        0x09
#define OP_BUFFER_BASE      0x0c
#define OP_RING_CONSUMED    0x0b
#define OP_READ_WORD        0x10
#define OP_READ_HALF        0x12
#define OP_BLOCK_SEND       0x20
#define OP_DMA_BUFFER0      0x21
#define OP_LOAD_FIRST       0x23
#define OP_DMA_BUFFER1      0x24
#define OP_LOAD_AUDIO       0x28
#define OP_DMA_ABORT        0x26
#define OP_UNKNOWN          0x3f

#define STATUS_BYTES        0x4000
#define STATUS_CONSUMER     0x14
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

/* Dword 0x10 is a 128 MiB window; dword 0x14 is not a BAR and holds 0. */
static void test_config(void *obj, void *data, QGuestAllocator *alloc)
{
    QDivo *divo = obj;
    QPCIDevice *dev = &divo->dev;

    qpci_config_writel(dev, PCI_BASE_ADDRESS_0, 0xffffffff);
    g_assert_cmphex(qpci_config_readl(dev, PCI_BASE_ADDRESS_0), ==, 0xf8000000);
    qpci_config_writel(dev, CFG_BAR1, 0xffffffff);
    g_assert_cmphex(qpci_config_readl(dev, CFG_BAR1), ==, 0);
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

/* A word of host memory as the host reads it. */
static uint32_t mem_word(Divo *t, uint64_t addr)
{
    uint8_t bytes[4];

    qtest_memread(t->qts, HUB_ALIAS(addr), bytes, 4);
    return ldl_be_p(bytes);
}

/*
 * The board keeps its consumer index in the host's packet buffer, dword
 * 0x14, and writes it after every word. Established by the shipped host
 * code, which reads it from there; the packet is the model's choice.
 */
static void test_consumer(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;
    uint64_t status;

    divo_open(&t, obj, alloc);
    status = guest_alloc(alloc, STATUS_BYTES);
    qtest_memset(t.qts, HUB_ALIAS(status), 0xaa, STATUS_BYTES);
    ring_setup(&t);
    g_assert_cmphex(mem_word(&t, status + STATUS_CONSUMER), ==, 0xaaaaaaaa);
    mb_send(&t, 4, op_word(OP_BUFFER_BASE, dma_field(status)));
    g_assert_cmphex(mem_word(&t, status + STATUS_CONSUMER), ==, 4);
    /* The rest of the block is the host's. */
    g_assert_cmphex(mem_word(&t, status + 0x10), ==, 0xaaaaaaaa);
    g_assert_cmphex(entry_word(&t, 1, 0), ==, 0x01000000);
    g_assert_cmphex(entry_word(&t, 1, 1), ==, 4);
    mb_send(&t, 7, op_word(0x0a, 0x40));
    g_assert_cmphex(mem_word(&t, status + STATUS_CONSUMER), ==, 7);
    g_assert_cmphex(entry_word(&t, 2, 1), ==, 7);
    /* A half-sent word completes nothing. */
    wr(&t, MAILBOX + 9 * 0x10000, 0);
    g_assert_cmphex(mem_word(&t, status + STATUS_CONSUMER), ==, 7);
    /* The report of consumed packets moves it too, with no packet. */
    mb_send(&t, 8, op_word(OP_RING_CONSUMED, 1));
    g_assert_cmphex(mem_word(&t, status + STATUS_CONSUMER), ==, 8);
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
    uint64_t status;

    divo_open(&t, obj, alloc);
    status = guest_alloc(alloc, STATUS_BYTES);
    mb_send(&t, 0, op_word(OP_BUFFER_BASE, dma_field(status)));
    qpci_io_writeb(t.dev, t.bar, MAILBOX + 5 * 0x10000 + 3, 0xff);
    g_assert_cmphex(mem_word(&t, status + STATUS_CONSUMER), ==, 0);
}

/* Video out. The rhythm and the packet's time words are approximations. */
#define CTL_WIDTH           0
#define CTL_FIELD_LINES     1
#define CTL_ROW_DWORDS      2
#define CTL_FRAME_MODE      6
#define CTL_DMA             0x13
#define PAGE                0x4000
#define DESC_HEADER         0x50
#define PKT_DMA_DONE        0x8000
#define NTSC_FRAME_NS       33366666
#define PAL_FRAME_NS        40000000
#define NTSC_PAGES          43
#define PAL_PAGES           51

static char *sink_path;

static uint64_t ctl_word(unsigned index, uint32_t val)
{
    return op_word(OP_CONTROL, ((uint64_t)index << 52) | val);
}

static uint64_t sink_get(Divo *t, const char *prop)
{
    QDict *r = qtest_qmp(t->qts, "{'execute':'qom-get','arguments':"
                         "{'path':'/machine/peripheral/rec',"
                         "'property':%s}}", prop);
    uint64_t v = qdict_get_int(r, "return");

    qobject_unref(r);
    return v;
}

static void sink_wait(Divo *t, uint64_t written)
{
    for (int i = 0; i < 2000 && sink_get(t, "written") < written; i++) {
        g_usleep(5000);
    }
    g_assert_cmpuint(sink_get(t, "written"), ==, written);
}

/* Fill the board's view: ring, status block and a frame format. */
static void video_setup_bpp(Divo *t, unsigned width, unsigned lines,
                            unsigned bpp, unsigned mode)
{
    uint64_t status = guest_alloc(t->alloc, STATUS_BYTES);

    /* The device outlives the test before it: stop whatever it was doing. */
    mb_send(t, 0, op_word(OP_DMA_ABORT, 0));
    ring_setup(t);
    mb_send(t, 1, op_word(OP_BUFFER_BASE, dma_field(status)));
    mb_send(t, 2, ctl_word(CTL_WIDTH, width));
    mb_send(t, 3, ctl_word(CTL_FIELD_LINES, lines));
    mb_send(t, 4, ctl_word(CTL_ROW_DWORDS, width * bpp / 32));
    mb_send(t, 5, ctl_word(CTL_FRAME_MODE, mode));
}

static void video_setup(Divo *t, unsigned width, unsigned lines)
{
    video_setup_bpp(t, width, lines, 16, 1);
}

/*
 * A buffer of the given pages, its bytes a function of the position, and a
 * descriptor that lists the pages. Returns the descriptor's address.
 */
static uint64_t make_buffer(Divo *t, unsigned pages, uint8_t seed)
{
    uint64_t desc = guest_alloc(t->alloc, DESC_HEADER + 8 * pages);
    uint64_t data = guest_alloc(t->alloc, pages * PAGE);
    g_autofree uint8_t *bytes = g_malloc(pages * PAGE);
    g_autofree uint8_t *list = g_malloc(8 * pages);

    for (unsigned i = 0; i < pages * PAGE; i++) {
        bytes[i] = seed + i * 7 + (i >> 8);
    }
    qtest_memwrite(t->qts, HUB_ALIAS(data), bytes, pages * PAGE);
    for (unsigned i = 0; i < pages; i++) {
        /* Odd pages are 32-bit direct addresses, sign extended. */
        if (i & 1) {
            stq_be_p(list + 8 * i,
                     0xffffffff00000000ULL | (uint32_t)(data + i * PAGE));
        } else {
            stq_be_p(list + 8 * i, (1ULL << 63) | (data + i * PAGE));
        }
    }
    qtest_memwrite(t->qts, HUB_ALIAS(desc + DESC_HEADER), list, 8 * pages);
    return desc;
}

static uint64_t buffer_word(unsigned op, unsigned pages, uint64_t desc)
{
    return op_word(op, ((uint64_t)pages << 49) | dma_field(desc));
}

/* The ring entry of the first valid packet of a type, or -1. */
static int find_packet(Divo *t, uint32_t type)
{
    for (unsigned i = 0; i < RING_ENTRIES; i++) {
        if (entry_valid(t, i) && entry_word(t, i, 0) == type) {
            return i;
        }
    }
    return -1;
}

static unsigned count_packets(Divo *t, uint32_t type)
{
    unsigned n = 0;

    for (unsigned i = 0; i < RING_ENTRIES; i++) {
        n += entry_valid(t, i) && entry_word(t, i, 0) == type;
    }
    return n;
}

static void test_video_out_frame(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;
    uint64_t desc;
    g_autofree char *file = NULL;
    const char *head = "YUV4MPEG2 W720 H486 F30000:1001 It A0:0 C422\n";
    size_t hlen = strlen(head), luma = 720 * 486;
    gsize len;
    const uint8_t *last;
    int at;
    uint64_t before;

    divo_open(&t, obj, alloc);
    before = sink_get(&t, "written");
    video_setup(&t, 720, 243);
    desc = make_buffer(&t, NTSC_PAGES, 0x11);
    /* The start comes in a command block, as the shipped library sends it. */
    {
        uint64_t block = guest_alloc(alloc, 16);
        uint8_t words[8];

        stq_be_p(words, ctl_word(CTL_DMA, 1));
        qtest_memwrite(t.qts, HUB_ALIAS(block), words, 8);
        mb_send(&t, 4, op_word(OP_BLOCK_SEND, (1ULL << 51) | dma_field(block)));
    }
    mb_send(&t, 5, buffer_word(OP_DMA_BUFFER0, NTSC_PAGES, desc));

    /* Nothing completes before a frame time, and one does at it. */
    qtest_clock_step(t.qts, NTSC_FRAME_NS - 3000000);
    g_assert_cmpint(find_packet(&t, PKT_DMA_DONE), ==, -1);
    qtest_clock_step(t.qts, 3000000);
    at = find_packet(&t, PKT_DMA_DONE);
    g_assert_cmpint(at, >=, 0);
    g_assert_cmphex(entry_word(&t, at, 1), ==, 0);
    g_assert_cmpuint(entry_word(&t, at, 2), ==, 0);
    g_assert_cmpuint(entry_word(&t, at, 3), >, 0);
    g_assert_cmpuint(entry_word(&t, at, 4), ==, 0);
    g_assert_cmpuint(entry_word(&t, at, 5), ==, 0);
    sink_wait(&t, before + 1);

    /* The file is the frame: planar, luma and then the two chroma planes. */
    g_assert(g_file_get_contents(sink_path, &file, &len, NULL));
    g_assert_cmpuint(len, ==, hlen + (before + 1) * (6 + 2 * luma));
    g_assert(!memcmp(file, head, hlen));
    last = (const uint8_t *)file + len - 6 - 2 * luma;
    g_assert(!memcmp(last, "FRAME\n", 6));
    for (unsigned i = 0; i < 720 * 486 / 2; i++) {
        uint8_t y0 = 0x11 + (4 * i + 1) * 7 + ((4 * i + 1) >> 8);
        uint8_t cb = 0x11 + (4 * i) * 7 + ((4 * i) >> 8);
        uint8_t cr = 0x11 + (4 * i + 2) * 7 + ((4 * i + 2) >> 8);
        uint8_t y1 = 0x11 + (4 * i + 3) * 7 + ((4 * i + 3) >> 8);
        const uint8_t *f = last + 6;

        g_assert_cmphex(f[2 * i], ==, y0);
        g_assert_cmphex(f[2 * i + 1], ==, y1);
        g_assert_cmphex(f[luma + i], ==, cb);
        g_assert_cmphex(f[luma + luma / 2 + i], ==, cr);
    }
}

/* Two buffers complete a frame time apart; a stop lets them finish. */
static void test_video_out_rhythm(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;
    uint64_t d0, d1, before;
    int a, b;

    divo_open(&t, obj, alloc);
    before = sink_get(&t, "written");
    video_setup(&t, 720, 243);
    d0 = make_buffer(&t, NTSC_PAGES, 0x21);
    d1 = make_buffer(&t, NTSC_PAGES, 0x31);
    mb_send(&t, 4, ctl_word(CTL_DMA, 1));
    mb_send(&t, 5, buffer_word(OP_DMA_BUFFER0, NTSC_PAGES, d0));
    mb_send(&t, 6, buffer_word(OP_DMA_BUFFER1, NTSC_PAGES, d1));
    /* The stop comes at once, as for a one-frame transfer. */
    mb_send(&t, 7, ctl_word(CTL_DMA, 0));

    qtest_clock_step(t.qts, NTSC_FRAME_NS - 5000000);
    g_assert_cmpint(find_packet(&t, PKT_DMA_DONE), ==, -1);
    qtest_clock_step(t.qts, 5000000);
    g_assert_cmpuint(count_packets(&t, PKT_DMA_DONE), ==, 1);
    a = find_packet(&t, PKT_DMA_DONE);
    g_assert_cmphex(entry_word(&t, a, 1), ==, 0);
    g_assert_cmpuint(entry_word(&t, a, 5), ==, 0);
    qtest_clock_step(t.qts, NTSC_FRAME_NS);
    g_assert_cmpuint(count_packets(&t, PKT_DMA_DONE), ==, 2);
    b = a + 1;
    while (!entry_valid(&t, b) || entry_word(&t, b, 0) != PKT_DMA_DONE) {
        b++;
    }
    g_assert_cmphex(entry_word(&t, b, 1), ==, 1);
    g_assert_cmpuint(entry_word(&t, b, 5), ==, 1);
    /* Nothing follows once both are done. */
    qtest_clock_step(t.qts, 3 * NTSC_FRAME_NS);
    g_assert_cmpuint(count_packets(&t, PKT_DMA_DONE), ==, 2);
    sink_wait(&t, before + 2);
}

/* A 625-line format completes at 25 frames a second. */
static void test_video_out_pal(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;
    uint64_t desc;

    divo_open(&t, obj, alloc);
    video_setup(&t, 720, 288);
    desc = make_buffer(&t, PAL_PAGES, 0x61);
    mb_send(&t, 4, ctl_word(CTL_DMA, 1));
    mb_send(&t, 5, buffer_word(OP_DMA_BUFFER0, PAL_PAGES, desc));
    mb_send(&t, 6, ctl_word(CTL_DMA, 0));
    qtest_clock_step(t.qts, PAL_FRAME_NS - 5000000);
    g_assert_cmpint(find_packet(&t, PKT_DMA_DONE), ==, -1);
    qtest_clock_step(t.qts, 5000000);
    g_assert_cmpuint(count_packets(&t, PKT_DMA_DONE), ==, 1);
}

/* A buffer that is not a frame of the format completes and plays nothing. */
static void test_video_out_other_buffer(void *obj, void *data,
                                        QGuestAllocator *alloc)
{
    Divo t;
    uint64_t desc, before;

    divo_open(&t, obj, alloc);
    before = sink_get(&t, "frames");
    video_setup(&t, 720, 243);
    desc = make_buffer(&t, 2, 0x41);
    mb_send(&t, 4, ctl_word(CTL_DMA, 1));
    mb_send(&t, 5, buffer_word(OP_DMA_BUFFER0, 2, desc));
    mb_send(&t, 6, ctl_word(CTL_DMA, 0));
    qtest_clock_step(t.qts, 2 * NTSC_FRAME_NS);
    g_assert_cmpuint(count_packets(&t, PKT_DMA_DONE), ==, 1);
    g_assert_cmpuint(sink_get(&t, "frames"), ==, before);
}

/* An abort drops the queue and no completion follows. */
static void test_video_out_abort(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;
    uint64_t desc;

    divo_open(&t, obj, alloc);
    video_setup(&t, 720, 243);
    desc = make_buffer(&t, NTSC_PAGES, 0x51);
    mb_send(&t, 4, ctl_word(CTL_DMA, 1));
    mb_send(&t, 5, buffer_word(OP_DMA_BUFFER0, NTSC_PAGES, desc));
    mb_send(&t, 6, op_word(OP_DMA_ABORT, 0));
    qtest_clock_step(t.qts, 2 * NTSC_FRAME_NS);
    g_assert_cmpuint(count_packets(&t, PKT_DMA_DONE), ==, 0);
}

/* The last frame of the sink: the pointer to its planes, or NULL. */
static const uint8_t *sink_last(char **file, size_t luma_bytes)
{
    gsize len;

    g_assert(g_file_get_contents(sink_path, file, &len, NULL));
    return (const uint8_t *)*file + len - luma_bytes * 2;
}

static void play_one(Divo *t, uint64_t desc, unsigned pages)
{
    mb_send(t, 6, ctl_word(CTL_DMA, 1));
    mb_send(t, 7, buffer_word(OP_DMA_BUFFER0, pages, desc));
    mb_send(t, 8, ctl_word(CTL_DMA, 0));
    qtest_clock_step(t->qts, 2 * NTSC_FRAME_NS);
}

/*
 * Rows of 3 or 4 bytes a pixel are R, G, B (then alpha, ignored), converted
 * to limited-range BT.601 4:2:2 with the pair's chroma averaged. The
 * packing is chosen by the row length, the model's approximation.
 */
static void check_rgb(void *obj, QGuestAllocator *alloc, unsigned bytes,
                      unsigned pages)
{
    Divo t;
    uint64_t desc, before;
    g_autofree char *file = NULL;
    size_t luma = 720 * 486;
    const uint8_t *f;
    const uint8_t seed = 0x31;

    divo_open(&t, obj, alloc);
    before = sink_get(&t, "written");
    video_setup_bpp(&t, 720, 243, bytes * 8, 1);
    desc = make_buffer(&t, pages, seed);
    play_one(&t, desc, pages);
    sink_wait(&t, before + 1);
    f = sink_last(&file, luma);
    for (unsigned y = 0; y < 486; y++) {
        for (unsigned x = 0; x < 720; x += 2) {
            double cb = 0, cr = 0;

            for (unsigned k = 0; k < 2; k++) {
                unsigned o = y * 720 * bytes + (x + k) * bytes;
                double r = (uint8_t)(seed + (o + 0) * 7 + ((o + 0) >> 8));
                double g = (uint8_t)(seed + (o + 1) * 7 + ((o + 1) >> 8));
                double b = (uint8_t)(seed + (o + 2) * 7 + ((o + 2) >> 8));

                g_assert_cmpuint(f[y * 720 + x + k], ==,
                                 lround(16 + (65.481 * r + 128.553 * g +
                                              24.966 * b) / 255));
                cb += 128 + (-37.797 * r - 74.203 * g + 112.0 * b) / 255;
                cr += 128 + (112.0 * r - 93.786 * g - 18.214 * b) / 255;
            }
            g_assert_cmpuint(f[luma + (y * 720 + x) / 2], ==, lround(cb / 2));
            g_assert_cmpuint(f[luma + luma / 2 + (y * 720 + x) / 2], ==,
                             lround(cr / 2));
        }
    }
}

static void test_video_out_rgb24(void *obj, void *data, QGuestAllocator *alloc)
{
    check_rgb(obj, alloc, 3, 65);
}

static void test_video_out_rgba32(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    check_rgb(obj, alloc, 4, 86);
}

/*
 * Capture type 4 sends one field of lines, not two: the buffer is accepted
 * and, being not the sink's frame size, counted and dropped there.
 */
static void test_video_out_field(void *obj, void *data, QGuestAllocator *alloc)
{
    Divo t;
    uint64_t desc, frames, dropped;

    divo_open(&t, obj, alloc);
    frames = sink_get(&t, "frames");
    dropped = sink_get(&t, "dropped");
    video_setup_bpp(&t, 720, 243, 16, 4);
    desc = make_buffer(&t, 22, 0x71);
    play_one(&t, desc, 22);
    for (int i = 0; i < 2000 && sink_get(&t, "dropped") == dropped; i++) {
        g_usleep(5000);
    }
    g_assert_cmpuint(sink_get(&t, "frames"), ==, frames + 1);
    g_assert_cmpuint(sink_get(&t, "dropped"), ==, dropped + 1);
}

/* A row length no packing has completes the buffer and plays nothing. */
static void test_video_out_odd_row(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    Divo t;
    uint64_t desc, before;

    divo_open(&t, obj, alloc);
    before = sink_get(&t, "frames");
    video_setup_bpp(&t, 720, 243, 20, 1);
    desc = make_buffer(&t, 54, 0x41);
    play_one(&t, desc, 54);
    g_assert_cmpuint(count_packets(&t, PKT_DMA_DONE), ==, 1);
    g_assert_cmpuint(sink_get(&t, "frames"), ==, before);
}

/* A buffer queued before the start word plays once the stream starts. */
static void test_video_out_queue_first(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    Divo t;
    uint64_t desc;

    divo_open(&t, obj, alloc);
    video_setup(&t, 720, 243);
    desc = make_buffer(&t, NTSC_PAGES, 0x15);
    mb_send(&t, 4, buffer_word(OP_DMA_BUFFER0, NTSC_PAGES, desc));
    qtest_clock_step(t.qts, 2 * NTSC_FRAME_NS);
    g_assert_cmpint(find_packet(&t, PKT_DMA_DONE), ==, -1);
    mb_send(&t, 5, ctl_word(CTL_DMA, 1));
    qtest_clock_step(t.qts, NTSC_FRAME_NS);
    g_assert_cmpuint(count_packets(&t, PKT_DMA_DONE), ==, 1);
}

static void divo_register_nodes(void)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-divo-XXXXXX", NULL);
    g_autofree char *before = NULL;
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "addr=02.0,out-sink=rec",
    };

    g_assert(dir);
    sink_path = g_build_filename(dir, "out.y4m", NULL);
    before = g_strdup_printf("-device sgi-xio-test-target,id=hub-ram,size=16M "
                             "-device sgi-video-file-sink,id=rec,path=%s",
                             sink_path);
    opts.before_cmd_line = g_steal_pointer(&before);

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
    qos_add_test("video-out-frame", "sgi-divo-linc", test_video_out_frame,
                 NULL);
    qos_add_test("video-out-rhythm", "sgi-divo-linc", test_video_out_rhythm,
                 NULL);
    qos_add_test("video-out-rgb24", "sgi-divo-linc", test_video_out_rgb24,
                 NULL);
    qos_add_test("video-out-rgba32", "sgi-divo-linc", test_video_out_rgba32,
                 NULL);
    qos_add_test("video-out-field", "sgi-divo-linc", test_video_out_field,
                 NULL);
    qos_add_test("video-out-odd-row", "sgi-divo-linc", test_video_out_odd_row,
                 NULL);
    qos_add_test("video-out-queue-first", "sgi-divo-linc",
                 test_video_out_queue_first, NULL);
    qos_add_test("video-out-pal", "sgi-divo-linc", test_video_out_pal, NULL);
    qos_add_test("video-out-other-buffer", "sgi-divo-linc",
                 test_video_out_other_buffer, NULL);
    qos_add_test("video-out-abort", "sgi-divo-linc", test_video_out_abort,
                 NULL);
}

libqos_init(divo_register_nodes);
