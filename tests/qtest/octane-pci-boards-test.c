/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * The Octane's PCI option boards: the one-slot PCI shoehorn in an XIO slot
 * and the three-slot PCI module on Crossbow port 13, each a Bridge on a
 * port of its own (docs/system/mips/sgi/hardware/pci-expansion.rst).
 *
 * Per board: the Bridge's widget identity, the board's one-wire record, the
 * flash window an empty board answers, a function in every slot (a QLogic
 * ISP1040 for the register paths, an ISP2200 for the identity of the card
 * the Fibre Channel arrays use), an interrupt reaching HEART through the
 * slot's Bridge pin, and a bus master reaching RAM. The refusals cover the
 * slots the topology lacks.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-launch.h"
#include "sgi-sonic-hdsp-image.h"

#define XBOW 0x10000000ULL
#define HEART 0x0ff00000ULL
#define RAM 0x20000000ULL
#define HEART_PENDING (HEART + 0x10030)
#define HEART_MASK_SET (HEART + 0x10000)
#define XBOW_LINK_STATUS(w) (XBOW + 0x100 + ((w) - 8) * 0x40 + 0x10 + 4)
#define XBOW_LINK_ALIVE (1u << 31)

#define WIDGET_BASE(w) (XBOW + ((uint64_t)(w) << 24))
#define REG_DATA 4
#define BRIDGE_REG(w, offset) (WIDGET_BASE(w) + (offset) + REG_DATA)
#define CFG(w, dev) (WIDGET_BASE(w) + 0x020000 + (dev) * 0x1000)
#define BRIDGE_DEVICE(w, dev) (WIDGET_BASE(w) + 0x200 + 8 * (dev) + REG_DATA)
#define FLASH_SELECT0 (0xc00000)
#define FLASH_SELECT1 (0xe00000)

/* A Bridge device's 2 MB windows for devices 0 and 1, 1 MB from device 2. */
#define DEVIO(w, dev) (WIDGET_BASE(w) + ((dev) < 2 ? \
                       0x200000 + (dev) * 0x200000 : \
                       0x600000 + ((dev) - 2) * 0x100000))

#define WIDGET_ID_BRIDGE_REVISION(r) (((r) << 28) | 0x0c00206d)
#define WIDGET_ID_REVISION_MASK 0x0fffffffu

#define ISP_MAILBOX(w, dev, n) (DEVIO(w, dev) + ((0x70 + 2 * (n)) ^ 2))
#define ISP_HOST_CMD(w, dev) (DEVIO(w, dev) + 0xc2)
#define ISP_SEMAPHORE(w, dev) (DEVIO(w, dev) + 0x0e)
/* Bus control: interrupt and RISC interrupt enables, halfword lane swapped. */
#define ISP_BUS_CTRL(w, dev) (DEVIO(w, dev) + (0x08 ^ 2))
#define ISP_BUS_CTRL_INTERRUPTS 0x0006
#define ISP_MBOX_COMPLETE 0x4000
#define ISP1040_ID 0x10201077u
#define ISP2200_ID 0x22001077u

/* The one-wire master in the Bridge and the bits software uses. */
#define BRIDGE_NIC 0x0b0
#define NIC_LEVEL (1u << 0)
#define NIC_DONE (1u << 1)
#define NIC_PULSE(n) ((uint32_t)(n) << 10)
#define NIC_SAMPLE(n) ((uint32_t)(n) << 2)
#define NIC_CMD_MATCH_ROM 0x55
#define NIC_CMD_SEARCH_ROM 0xf0
#define NIC_CMD_READ_MEMORY 0xf0
#define RECORD_PAGE 32
#define RECORD_SIZE 64
#define NIC_CRC_RESIDUE 0xb001

#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "reset-mode=0x1aa683"

/*
 * What the catalogue's board slots put on the command line for each
 * product (hw/mips/sgi/sgi-machines.json, board-slots of octane).
 */
#define SHOEHORN_PROPS "board-part=030-1275-004,board-revision=A," \
    "flash-empty-reads-high=true,first-slot=1,slots=1"

typedef struct PciBoard {
    const char *name;
    const char *device;         /* -device argument before slot= */
    unsigned int widget;
    unsigned int bridge_revision;
    unsigned int slots;         /* PCI devices first_slot .. +slots-1 */
    unsigned int first_slot;
    const char *barcode;
    const char *part;
    const char *revision;
    const char *record_name;
} PciBoard;

static const PciBoard shoehorn = {
    "shoehorn", "sgi-pci-shoehorn," SHOEHORN_PROPS, 10, 4, 1, 1,
    "EMUO65", "030-1275-004", "A", "XTALK_PCI",
};

static const PciBoard shoehorn_9 = {
    "shoehorn-widget-9", "sgi-pci-shoehorn," SHOEHORN_PROPS, 9, 4, 1, 1,
    "EMUO64", "030-1275-004", "A", "XTALK_PCI",
};

static const PciBoard shoehorn_12 = {
    "shoehorn-widget-12", "sgi-pci-shoehorn," SHOEHORN_PROPS, 12, 4, 1, 1,
    "EMUO67", "030-1275-004", "A", "XTALK_PCI",
};

static const PciBoard cage = {
    "cage", "sgi-octane-pci-cage", 13, 3, 3, 1,
    "EMUO68", "030-0952-005", "E", "XTALKPCI",
};

static char *flash_path;

static QTestState *start(bool octane2, const char *extra)
{
    g_autofree char *quoted = g_shell_quote(flash_path);

    if (octane2) {
        return qtest_initf(MEMTX_TEST_DEVICE "-vga none -M octane2 -m 256M "
                           "-cpu " OCTANE2_CPU " -bios %s -S %s",
                           quoted, extra);
    }
    return qtest_initf(MEMTX_TEST_DEVICE "-vga none -M octane -m 128M "
                       "-cpu R10000 -bios %s -S %s", quoted, extra);
}

static QTestState *start_board(bool octane2, const PciBoard *b,
                               const char *cards)
{
    g_autofree char *args = g_strdup_printf(
        "-device %s,slot=widget-%u,pci-bus-name=pci%u %s",
        b->device, b->widget, b->widget, cards);

    return start(octane2, args);
}

static MemTxResult read32(QTestState *qts, uint64_t addr, uint32_t *value)
{
    return memtx_readl(qts, addr, value);
}

static void test_widget_identity(bool octane2, const PciBoard *b)
{
    QTestState *qts = start_board(octane2, b, "");
    uint32_t id;

    g_assert_cmpuint(read32(qts, BRIDGE_REG(b->widget, 0), &id), ==,
                     MEMTX_OK);
    g_assert_cmphex(id, ==, WIDGET_ID_BRIDGE_REVISION(b->bridge_revision));
    /* The onboard Bridge on port 15 is a different widget with its own id. */
    g_assert_cmpuint(read32(qts, BRIDGE_REG(15, 0), &id), ==, MEMTX_OK);
    g_assert_cmphex(id & WIDGET_ID_REVISION_MASK, ==,
                    WIDGET_ID_BRIDGE_REVISION(0));
    /* The port's link is alive in the Crossbow. */
    g_assert_cmpuint(read32(qts, XBOW_LINK_STATUS(b->widget), &id), ==,
                     MEMTX_OK);
    g_assert_cmphex(id & XBOW_LINK_ALIVE, ==, XBOW_LINK_ALIVE);
    qtest_system_reset(qts);
    g_assert_cmpuint(read32(qts, BRIDGE_REG(b->widget, 0), &id), ==,
                     MEMTX_OK);
    g_assert_cmphex(id, ==, WIDGET_ID_BRIDGE_REVISION(b->bridge_revision));
    g_assert_cmpuint(read32(qts, XBOW_LINK_STATUS(b->widget), &id), ==,
                     MEMTX_OK);
    g_assert_cmphex(id & XBOW_LINK_ALIVE, ==, XBOW_LINK_ALIVE);
    qtest_quit(qts);
}

static bool nic_slot(QTestState *qts, unsigned int widget, unsigned int pulse,
                     unsigned int sample)
{
    uint32_t nic;

    qtest_writel(qts, BRIDGE_REG(widget, BRIDGE_NIC),
                 NIC_PULSE(pulse) | NIC_SAMPLE(sample));
    nic = qtest_readl(qts, BRIDGE_REG(widget, BRIDGE_NIC));
    g_assert_cmphex(nic & NIC_DONE, ==, NIC_DONE);
    return nic & NIC_LEVEL;
}

#define NIC_RESET(q, w) nic_slot((q), (w), 520, 65)
#define NIC_READ(q, w) nic_slot((q), (w), 6, 13)
#define NIC_WRITE_ONE(q, w) nic_slot((q), (w), 6, 13)
#define NIC_WRITE_ZERO(q, w) nic_slot((q), (w), 80, 30)

static void nic_write_byte(QTestState *qts, unsigned int widget,
                           uint8_t byte)
{
    for (unsigned int i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            NIC_WRITE_ONE(qts, widget);
        } else {
            NIC_WRITE_ZERO(qts, widget);
        }
    }
}

static uint8_t nic_read_byte(QTestState *qts, unsigned int widget)
{
    uint8_t byte = 0;

    for (unsigned int i = 0; i < 8; i++) {
        if (NIC_READ(qts, widget)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

/* CRC-16, reflected polynomial 0xa001, as a Bridge probe runs it. */
static uint16_t crc16(const uint8_t *data, unsigned int len)
{
    uint16_t crc = 0;

    for (unsigned int i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}

/* A text field of the record, space-padded to its width. */
static void put_field(uint8_t *at, const char *text, unsigned int width)
{
    memset(at, ' ', width);
    memcpy(at, text, MIN(strlen(text), width));
}

/*
 * The record laid out from the fields, independently of the model: page 0
 * holds a header byte, the barcode (ten characters), ten spaces and the
 * part's first nine characters, page 1 the dash number, revision, six
 * unprogrammed bytes and the name (fourteen characters), each page closed
 * by the inverted CRC-16 of the rest.
 */
static void expected_record(const PciBoard *b, uint8_t *record)
{
    uint16_t crc;

    memset(record, 0xff, RECORD_SIZE);
    record[0] = 0x01;
    put_field(record + 1, b->barcode, 10);
    put_field(record + 11, "", 10);
    memcpy(record + 21, b->part, 9);
    crc = ~crc16(record, 30);
    record[30] = crc;
    record[31] = crc >> 8;
    put_field(record + 32, "", 3);
    memcpy(record + 35, b->part + 9, 3);
    put_field(record + 38, b->revision, 4);
    put_field(record + 48, b->record_name, 14);
    crc = ~crc16(record + RECORD_PAGE, 30);
    record[62] = crc;
    record[63] = crc >> 8;
}

static void test_onewire_record(bool octane2, const PciBoard *b)
{
    QTestState *qts = start_board(octane2, b, "");
    uint8_t want[RECORD_SIZE], got[RECORD_SIZE];
    uint64_t rom = 0;

    expected_record(b, want);
    g_assert_false(NIC_RESET(qts, b->widget));
    nic_write_byte(qts, b->widget, NIC_CMD_SEARCH_ROM);
    /* One part on the bus: every bit answers without a discrepancy. */
    for (unsigned int bit = 0; bit < 64; bit++) {
        bool one = NIC_READ(qts, b->widget);
        bool zero = NIC_READ(qts, b->widget);

        g_assert_true(one != zero);
        if (one) {
            rom |= UINT64_C(1) << bit;
            NIC_WRITE_ONE(qts, b->widget);
        } else {
            NIC_WRITE_ZERO(qts, b->widget);
        }
    }
    g_assert_false(NIC_RESET(qts, b->widget));
    nic_write_byte(qts, b->widget, NIC_CMD_MATCH_ROM);
    for (unsigned int i = 0; i < 8; i++) {
        nic_write_byte(qts, b->widget, rom >> (i * 8));
    }
    nic_write_byte(qts, b->widget, NIC_CMD_READ_MEMORY);
    nic_write_byte(qts, b->widget, 0);
    nic_write_byte(qts, b->widget, 0);
    for (unsigned int i = 0; i < RECORD_SIZE; i++) {
        got[i] = nic_read_byte(qts, b->widget);
    }
    g_assert_cmpmem(got, RECORD_SIZE, want, RECORD_SIZE);
    g_assert_cmphex(crc16(got, RECORD_PAGE), ==, NIC_CRC_RESIDUE);
    g_assert_cmphex(crc16(got + RECORD_PAGE, RECORD_PAGE), ==,
                    NIC_CRC_RESIDUE);
    qtest_quit(qts);
}

/*
 * IRIX reads the start of the flash window and its second select on every
 * Bridge it finds. A board with nothing fitted is not known to fail them,
 * so both read all ones; a write has no part to land in and fails.
 */
static void test_flash_window(bool octane2, const PciBoard *b)
{
    QTestState *qts = start_board(octane2, b, "");
    uint64_t base = WIDGET_BASE(b->widget);
    uint32_t value;
    uint8_t byte;
    uint16_t half;

    for (unsigned int select = 0; select < 2; select++) {
        uint64_t at = base + (select ? FLASH_SELECT1 : FLASH_SELECT0);

        g_assert_cmpuint(read32(qts, at, &value), ==, MEMTX_OK);
        g_assert_cmphex(value, ==, 0xffffffffu);
        g_assert_cmpuint(read32(qts, at + 0x1ffffc, &value), ==, MEMTX_OK);
        g_assert_cmphex(value, ==, 0xffffffffu);
        g_assert_cmpuint(memtx_readw(qts, at + 2, &half), ==, MEMTX_OK);
        g_assert_cmphex(half, ==, 0xffff);
        g_assert_cmpuint(memtx_readb(qts, at + 1, &byte), ==, MEMTX_OK);
        g_assert_cmphex(byte, ==, 0xff);
        g_assert_cmpuint(memtx_writew(qts, at, 0x1234), !=, MEMTX_OK);
    }
    qtest_quit(qts);
}

static void bar_and_enable(QTestState *qts, const PciBoard *b,
                           unsigned int dev, uint32_t pci_base)
{
    qtest_writel(qts, CFG(b->widget, dev) + 0x14, pci_base);
    qtest_writel(qts, CFG(b->widget, dev) + 0x04, 6);
    qtest_writel(qts, BRIDGE_DEVICE(b->widget, dev), 0x1000 | (pci_base >> 20));
    qtest_writew(qts, ISP_BUS_CTRL(b->widget, dev), ISP_BUS_CTRL_INTERRUPTS);
}

static char *isp_cards(const PciBoard *b, const char *driver)
{
    GString *args = g_string_new("");

    for (unsigned int i = 0; i < b->slots; i++) {
        g_string_append_printf(args, "-device %s,bus=pci%u,addr=%u ", driver,
                               b->widget, b->first_slot + i);
    }
    return g_string_free(args, false);
}

/* A function answers in every slot, and nothing at the other devices. */
static void test_slot_functions(bool octane2, const PciBoard *b)
{
    g_autofree char *cards = isp_cards(b, "qlogic-isp1040");
    QTestState *qts = start_board(octane2, b, cards);
    uint32_t value;

    for (unsigned int dev = 0; dev < 8; dev++) {
        bool slot = dev >= b->first_slot && dev < b->first_slot + b->slots;

        if (slot) {
            g_assert_cmpuint(read32(qts, CFG(b->widget, dev), &value), ==,
                             MEMTX_OK);
            g_assert_cmphex(value, ==, ISP1040_ID);
        } else {
            /* A configuration cycle nobody claims is a master time-out. */
            g_assert_cmpuint(read32(qts, CFG(b->widget, dev), &value), !=,
                             MEMTX_OK);
        }
    }
    qtest_quit(qts);
}

static void test_isp2200_identity(bool octane2, const PciBoard *b)
{
    g_autofree char *cards = isp_cards(b, "qlogic-isp2200");
    QTestState *qts = start_board(octane2, b, cards);
    uint32_t value;

    for (unsigned int i = 0; i < b->slots; i++) {
        g_assert_cmpuint(read32(qts, CFG(b->widget, b->first_slot + i),
                                &value), ==, MEMTX_OK);
        g_assert_cmphex(value, ==, ISP2200_ID);
    }
    qtest_quit(qts);
}

/*
 * A mailbox command on the card at @dev, which raises the card's INTA until
 * the host clears it; the caller checks the interrupt path before the clear.
 */
static void isp_command(QTestState *qts, const PciBoard *b, unsigned int dev,
                        uint16_t command, uint32_t pci_address)
{
    unsigned int w = b->widget;

    qtest_writew(qts, ISP_MAILBOX(w, dev, 4), 4);
    qtest_writew(qts, ISP_MAILBOX(w, dev, 6), 0);
    qtest_writew(qts, ISP_MAILBOX(w, dev, 7), 0);
    qtest_writew(qts, ISP_MAILBOX(w, dev, 0), command);
    qtest_writew(qts, ISP_MAILBOX(w, dev, 1), 0x2000);
    qtest_writew(qts, ISP_MAILBOX(w, dev, 2), pci_address >> 16);
    qtest_writew(qts, ISP_MAILBOX(w, dev, 3), pci_address);
    qtest_writew(qts, ISP_HOST_CMD(w, dev), 0x5000);
}

static void isp_clear(QTestState *qts, const PciBoard *b, unsigned int dev)
{
    qtest_writew(qts, ISP_HOST_CMD(b->widget, dev), 0x7000);
    qtest_writew(qts, ISP_SEMAPHORE(b->widget, dev), 0);
}

/*
 * The card's INTA reaches the Bridge pin numbered as its device, the pin's
 * interrupt packet reaches HEART as the vector the Bridge was told, and
 * clearing the card clears it all. The pin comes from the Bridge's
 * interrupt status register.
 */
static void test_interrupt_to_heart(bool octane2, const PciBoard *b)
{
    g_autofree char *cards = isp_cards(b, "qlogic-isp1040");
    QTestState *qts = start_board(octane2, b, cards);
    unsigned int w = b->widget;

    for (unsigned int i = 0; i < b->slots; i++) {
        unsigned int dev = b->first_slot + i;
        uint32_t pin = 1u << dev;
        unsigned int vector = 17 + dev;
        uint64_t bit = UINT64_C(1) << vector;

        bar_and_enable(qts, b, dev, 0x10000000u + dev * 0x01000000u);
        qtest_writel(qts, BRIDGE_REG(w, 0x030), 8u << 16);
        qtest_writel(qts, BRIDGE_REG(w, 0x038), 0x80);
        qtest_writel(qts, BRIDGE_REG(w, 0x130 + 8 * dev), vector);
        qtest_writel(qts, BRIDGE_REG(w, 0x118), pin);
        qtest_writel(qts, BRIDGE_REG(w, 0x108), pin);
        qtest_writeq(qts, HEART_MASK_SET, bit);
        g_assert_cmphex(qtest_readq(qts, HEART_PENDING) & bit, ==, 0);

        isp_command(qts, b, dev, 1, 0xa0005000);
        g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(w, 0x100)) & pin, ==, pin);
        g_assert_cmphex(qtest_readq(qts, HEART_PENDING) & bit, ==, bit);

        isp_clear(qts, b, dev);
        g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(w, 0x100)) & pin, ==, 0);
        g_assert_cmphex(qtest_readq(qts, HEART_PENDING) & bit, ==, 0);
        qtest_writel(qts, BRIDGE_REG(w, 0x108), 0);
    }
    qtest_quit(qts);
}

/*
 * An add-in IOC3 raises its SuperIO interrupt on the slot's INTB, which the
 * Bridge takes on pin device ^ 4: pin 6 for the module's slot 2. Its INTA
 * pin, the device number, stays low. The pin is inferred from the public
 * drivers; this checks the model's wiring. The shoehorn wires only a card it
 * fits itself, so a card added to it with -device has INTA alone.
 */
#define IOC3_INT_STATUS(w, dev) (DEVIO(w, dev) + 0x1c)
#define IOC3_INT_ENABLE_SET(w, dev) (DEVIO(w, dev) + 0x20)
#define IOC3_SERIAL_A_IER(w, dev) (DEVIO(w, dev) + 0x020178 + 1)
#define IOC3_SIO_SRC_A_PASSTHROUGH (1u << 6)
#define IER_THRI 0x02

static void test_superio_interrupt_pin(bool octane2, const PciBoard *b)
{
    unsigned int dev = b->first_slot + 1;
    unsigned int w = b->widget;
    g_autofree char *cards = g_strdup_printf(
        "-device sgi-ioc3,bus=pci%u,addr=%u,subsystem-id=0xffff,"
        "population=2", w, dev);
    QTestState *qts = start_board(octane2, b, cards);
    uint32_t inta = 1u << dev;
    uint32_t intb = 1u << (dev ^ 4);

    qtest_writel(qts, CFG(w, dev) + 0x10, 0x10000000u);
    qtest_writel(qts, CFG(w, dev) + 0x04, 2);
    qtest_writel(qts, BRIDGE_DEVICE(w, dev), 0x1000 | (0x10000000u >> 20));
    qtest_writel(qts, BRIDGE_REG(w, 0x108), inta | intb);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(w, 0x100)) & (inta | intb),
                    ==, 0);
    qtest_writel(qts, IOC3_INT_ENABLE_SET(w, dev),
                 IOC3_SIO_SRC_A_PASSTHROUGH);
    qtest_writeb(qts, IOC3_SERIAL_A_IER(w, dev), IER_THRI);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(w, 0x100)) & (inta | intb),
                    ==, intb);
    qtest_quit(qts);
}

/* A card in each slot masters RAM through the Bridge's direct map. */
static void test_dma_to_ram(bool octane2, const PciBoard *b)
{
    g_autofree char *cards = isp_cards(b, "qlogic-isp1040");
    QTestState *qts = start_board(octane2, b, cards);
    const uint8_t pattern[8] = { 0x10, 0x21, 0x32, 0x43,
                                 0x54, 0x65, 0x76, 0x87 };
    uint8_t seen[sizeof(pattern)];
    unsigned int w = b->widget;

    qtest_writel(qts, BRIDGE_REG(w, 0x080), 8u << 20);
    for (unsigned int i = 0; i < b->slots; i++) {
        unsigned int dev = b->first_slot + i;
        uint64_t from = RAM + 0x5000 + 0x100 * i;
        uint64_t to = RAM + 0x6000 + 0x100 * i;

        bar_and_enable(qts, b, dev, 0x10000000u + dev * 0x01000000u);
        qtest_memwrite(qts, from, pattern, sizeof(pattern));
        memset(seen, 0, sizeof(seen));
        qtest_memwrite(qts, to, seen, sizeof(seen));

        isp_command(qts, b, dev, 1, 0xa0000000u + (from - RAM));
        g_assert_cmphex(qtest_readw(qts, ISP_MAILBOX(w, dev, 0)), ==,
                        ISP_MBOX_COMPLETE);
        isp_clear(qts, b, dev);
        isp_command(qts, b, dev, 3, 0xa0000000u + (to - RAM));
        g_assert_cmphex(qtest_readw(qts, ISP_MAILBOX(w, dev, 0)), ==,
                        ISP_MBOX_COMPLETE);
        isp_clear(qts, b, dev);
        qtest_memread(qts, to, seen, sizeof(seen));
        g_assert_cmpmem(seen, sizeof(seen), pattern, sizeof(pattern));
    }
    qtest_quit(qts);
}

/*
 * The Sonic Solutions HDSP audio card (sonic-hdsp.rst, "Placement") is a
 * two-function device with a PCI-to-PCI bridge unit on function 0, so it
 * must sit in a slot of either board and answer there once its own firmware
 * has cleared the configuration-retry bit. A synthetic image stands in for
 * the card's firmware. The card's BAR0 is 128 MiB, which only the Bridge's
 * 0x40000000 range through the widget's large window can reach; this test
 * does not go there (see the page's "Placement").
 */
#define SONIC_FN(w, dev, fn) (CFG(w, dev) + 0x100 * (fn))

static void test_sonic_hdsp(bool octane2, const PciBoard *b)
{
    Image img = { FW_RAISE_OMR, 0x03c1 };
    g_autofree char *firmware = flash_file(&img);
    g_autofree char *quoted = g_shell_quote(flash_path);
    g_autofree char *args = g_strdup_printf(
        "-device %s,slot=widget-%u,pci-bus-name=pci%u "
        "-device sgi-sonic-hdsp,bus=pci%u,addr=%u,flash-image=%s",
        b->device, b->widget, b->widget, b->widget, b->first_slot, firmware);
    QTestState *qts;
    unsigned int w = b->widget, dev = b->first_slot;
    uint32_t value, pin = 1u << (dev ^ 4);

    if (octane2) {
        qts = qtest_initf(MEMTX_TEST_DEVICE "-vga none -M octane2 -m 256M "
                          "-cpu " OCTANE2_CPU " -bios %s %s", quoted, args);
    } else {
        qts = qtest_initf(MEMTX_TEST_DEVICE "-vga none -M octane -m 128M "
                          "-cpu R10000 -bios %s %s", quoted, args);
    }

    /* The card retries every configuration cycle until its firmware is up. */
    g_assert_cmpuint(read32(qts, SONIC_FN(w, dev, 0), &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0xffffffff);
    qtest_clock_step(qts, 5 * 1000 * 1000);

    g_assert_cmpuint(read32(qts, SONIC_FN(w, dev, 0), &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x060e1263);
    /* Header type 0x81: the slot scan probes functions 1 to 7. */
    g_assert_cmpuint(read32(qts, SONIC_FN(w, dev, 0) + 0x0c, &value), ==,
                     MEMTX_OK);
    g_assert_cmphex((value >> 16) & 0xff, ==, 0x81);
    g_assert_cmpuint(read32(qts, SONIC_FN(w, dev, 1), &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x05dc1263);
    /* Function 2 does not exist. */
    g_assert_cmpuint(read32(qts, SONIC_FN(w, dev, 2), &value), !=, MEMTX_OK);

    /*
     * INTB: the image has written outbound message 0, which the model raises
     * on the pin the ATU's interrupt pin register names, INTB. The Bridge
     * takes it on pin device ^ 4, not on the device's own number.
     */
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(w, 0x100)) & pin, ==, pin);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(w, 0x100)) & (1u << dev), ==,
                    0);
    qtest_quit(qts);
    unlink(firmware);
}

/* Slots the topology lacks, and a card where the board has no slot. */
static void test_refusals(void)
{
    g_autofree char *quoted = g_shell_quote(flash_path);
    g_autofree char *base = g_strdup_printf(
        "-vga none -M octane -m 128M -cpu R10000 -bios %s -qmp stdio "
        "-display none", quoted);
    struct {
        const char *devices;
        const char *message;
    } cases[] = {
        { "-device sgi-octane-pci-cage,slot=widget-12",
          "the PCI module goes in widget-13, not widget-12" },
        { "-device sgi-pci-shoehorn,slot=widget-13",
          "slot widget-13 is the PCI module's port and holds only an "
          "sgi-octane-pci-cage" },
        { "-device sgi-pci-shoehorn,slot=widget-14",
          "has no XIO slot widget-14" },
        { "-device sgi-pci-shoehorn,slot=widget-8",
          "has no XIO slot widget-8" },
        { "-device sgi-pci-shoehorn,slot=widget-10 "
          "-device sgi-pci-shoehorn,slot=widget-10,id=other",
          "slot widget-10 already holds a board" },
        { "-device sgi-octane-pci-cage,slot=widget-13,first-slot=4294967295,"
          "slots=2", "runs past Bridge device 7" },
        { "-device sgi-pci-shoehorn,slot=widget-10,first-slot=4294967295,"
          "slots=2", "runs past Bridge device 7" },
        { "-device sgi-octane-pci-cage,slot=widget-13 "
          "-device sgi-octane-pci-cage,slot=widget-13,id=other",
          "slot widget-13 already holds a board" },
        { "-device sgi-pci-shoehorn,slot=widget-10,slots=1,first-slot=1,"
          "pci-bus-name=p -device qlogic-isp1040,bus=p,addr=2",
          "slot 2 function 0 not available" },
        { "-device sgi-octane-pci-cage,slot=widget-13,pci-bus-name=p "
          "-device qlogic-isp1040,bus=p,addr=4",
          "slot 4 function 0 not available" },
        { "-device sgi-octane-pci-cage,slot=widget-13,pci-bus-name=p "
          "-device qlogic-isp1040,bus=p,addr=0",
          "slot 0 function 0 not available" },
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree char *line = g_strdup_printf("%s %s %s",
            qtest_qemu_binary(NULL), base, cases[i].devices);
        g_auto(GStrv) args = NULL;

        g_assert_true(g_shell_parse_argv(line, NULL, &args, NULL));
        assert_launch_rejected((const char *const *)args, cases[i].message);
    }
}

/* The shoehorn fits the XIO slots at the edges of the range as well. */
static void test_shoehorn_slots(void)
{
    const PciBoard *boards[] = { &shoehorn_9, &shoehorn, &shoehorn_12 };

    for (unsigned int i = 0; i < G_N_ELEMENTS(boards); i++) {
        QTestState *qts = start_board(false, boards[i], "");
        uint32_t id;

        g_assert_cmpuint(read32(qts, BRIDGE_REG(boards[i]->widget, 0), &id),
                         ==, MEMTX_OK);
        g_assert_cmphex(id, ==, WIDGET_ID_BRIDGE_REVISION(4));
        qtest_quit(qts);
    }
}

/* A shoehorn and the module together, each with a card in slot 1. */
static void test_both_boards_together(void)
{
    QTestState *qts = start(false,
        "-device sgi-pci-shoehorn," SHOEHORN_PROPS ",slot=widget-10,"
        "pci-bus-name=pci10 -device sgi-octane-pci-cage,slot=widget-13,"
        "pci-bus-name=pci13 -device qlogic-isp1040,bus=pci10,addr=1 "
        "-device qlogic-isp1040,bus=pci13,addr=1");
    uint32_t value;

    g_assert_cmpuint(read32(qts, CFG(10, 1), &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, ISP1040_ID);
    g_assert_cmpuint(read32(qts, CFG(13, 1), &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, ISP1040_ID);
    g_assert_cmpuint(read32(qts, CFG(13, 2), &value), !=, MEMTX_OK);
    qtest_quit(qts);
}

typedef void (*BoardTest)(bool octane2, const PciBoard *b);

typedef struct BoardCase {
    BoardTest test;
    bool octane2;
    const PciBoard *board;
} BoardCase;

static void run_case(gconstpointer data)
{
    const BoardCase *c = data;

    c->test(c->octane2, c->board);
}

static void add_board_test(const char *name, BoardTest test, bool octane2,
                           const PciBoard *board)
{
    BoardCase *c = g_new0(BoardCase, 1);
    g_autofree char *path = g_strdup_printf("/octane-pci/%s/%s/%s",
        octane2 ? "octane2" : "octane", board->name, name);

    c->test = test;
    c->octane2 = octane2;
    c->board = board;
    qtest_add_data_func(path, c, run_case);
}

int main(int argc, char **argv)
{
    const PciBoard *boards[] = { &shoehorn, &cage };
    int fd, result;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("octane-pci-flash-XXXXXX", &flash_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    for (unsigned int m = 0; m < 2; m++) {
        for (unsigned int i = 0; i < G_N_ELEMENTS(boards); i++) {
            add_board_test("widget-identity", test_widget_identity, m,
                           boards[i]);
            add_board_test("onewire-record", test_onewire_record, m,
                           boards[i]);
            add_board_test("flash-window", test_flash_window, m, boards[i]);
            add_board_test("slot-functions", test_slot_functions, m,
                           boards[i]);
            add_board_test("isp2200-identity", test_isp2200_identity, m,
                           boards[i]);
            add_board_test("interrupt-to-heart", test_interrupt_to_heart, m,
                           boards[i]);
            add_board_test("dma-to-ram", test_dma_to_ram, m, boards[i]);
            add_board_test("sonic-hdsp", test_sonic_hdsp, m, boards[i]);
        }
    }
    for (unsigned int m = 0; m < 2; m++) {
        add_board_test("superio-interrupt-pin", test_superio_interrupt_pin, m,
                       &cage);
    }
    qtest_add_func("/octane-pci/refusals", test_refusals);
    qtest_add_func("/octane-pci/shoehorn-slots", test_shoehorn_slots);
    qtest_add_func("/octane-pci/both-boards", test_both_boards_together);
    result = g_test_run();
    unlink(flash_path);
    g_free(flash_path);
    return result;
}
