/*
 * Test ISP1040 SCSI access through Crosstalk, Bridge byte correction and PCI.
 * Distinct mailbox bytes expose swaps on this path; x86 libqos covers the
 * device logic.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-test.h"

/* Crosstalk space, and the bridge's own widget window inside it. */
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_IO + ((uint64_t)BRIDGE_WIDGET << 24))

/*
 * The bridge's device(0) register: 32 bits in the low half of an eight-byte
 * slot.
 */
#define REG_DATA                4
#define BRIDGE_DEVICE0          0x200
#define REG_BRIDGE_DEVICE0      (BRIDGE_BASE + BRIDGE_DEVICE0 + REG_DATA)

#define DEV_SWAP_PROGRAMMED     (1u << 13)

/*
 * PCI type 0 configuration space; device 0 is the first of the board's two
 * SCSI adapters, fitted whenever the bridge exists.
 */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000
#define CFG0(device)            (BRIDGE_BASE + BRIDGE_CFG0 + \
                                 (device) * CFG0_DEVICE_STRIDE)
#define SCSI_DEVICE0            0

/* The per-slot form of the device register above. */
#define BRIDGE_DEVICE(n)        (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)

/* Device 0's window: the first of the two 2 MB slots. */
#define WINDOW_OFFSET_DEVICE0   0x200000
#define WINDOW_DEVICE0          (BRIDGE_BASE + WINDOW_OFFSET_DEVICE0)

/*
 * IOC3's slot and window, restated from tests/qtest/sn0-ioc3-test.c the way
 * that file restates its own constants.
 */
#define PCI_IOC3_DEVICE         2
#define WINDOW_IOC3             (BRIDGE_BASE + 0x600000)
#define IOC3_MMIO               0x10000000u

/* Serial port A's line-control and modem-control registers, a byte apart. */
#define SERIAL_A_BASE           0x020178
#define SERIAL_A_LCR            (SERIAL_A_BASE + 3)
#define SERIAL_A_MCR            (SERIAL_A_BASE + 4)

/* The adapter's BAR target; the window base field needs 20-bit alignment. */
#define ADAPTER_MMIO             0x10000000u

#define MACHINE_WITH_BRIDGE      "-M origin200"

/*
 * Offsets within the adapter's own window: PCI space, not the bridge's
 * eight-byte-slot layout, so no plus-four applies.
 */
#define ISP_REG_MAILBOX_BASE    0x70
#define ISP_MAILBOX(n)          (ISP_REG_MAILBOX_BASE + (n) * 2)

#define ISP_MAILBOX1_SIGNATURE  0x4953
#define ISP_MAILBOX2_SIGNATURE  0x5020
#define ISP_MAILBOX3_SIGNATURE  0x2020

/* The guest's halfword address: offset ^ (4 - width), low two bits only. */
#define GUEST_HALFWORD(natural)  (((natural) & ~3u) | (((natural) & 3u) ^ 2u))

/*
 * Adapter DMA also requires: the bus master bit, a translation entry, and the
 * mailbox protocol.
 */
#define ATE_BASE                0x010000
#define ATE(n)                  (BRIDGE_BASE + ATE_BASE + (n) * 8)
#define ATE_ENTRY(page, widget) ((uint64_t)(page) | \
                                 ((uint64_t)(widget) << 8) | 1)
#define PCI_TRANSLATED          0x40000000u
#define PAGE_SIZE_4K            0x1000u
#define TARGET_PAGE             0x00100000u
#define DIRECT64(phys)          (0xa500000000000000ULL | (uint64_t)(phys))

/*
 * The memory size is stated rather than left to the board's default, since
 * the address behind the last byte of it is what a refused transfer needs.
 */
#define MACHINE_WITH_64M        MACHINE_WITH_BRIDGE " -m 64"
#define RAM_TOP_64M             0x04000000u

#define ISP_REG_SEMAPHORE       0x0c
#define ISP_REG_HOST_CMD        0xc0
#define ISP_HOST_CMD_SHIFT      12
#define ISP_HOST_CMD_SET_HOST_INT   0x5
#define ISP_HOST_CMD_CLEAR_RISC_INT 0x7

#define ISP_MBOX_CMD_LOAD_RAM       0x0001
#define ISP_MBOX_CMD_DUMP_RAM       0x0003
#define ISP_MBOX_CMD_WRITE_RAM_WORD 0x0004
#define ISP_MBOX_CMD_VERIFY_CHECKSUM 0x0007
#define ISP_MBOX_CMD_READ_RAM_WORD  0x0005
#define ISP_MBOX_CMD_INIT_REQ_QUEUE_A64 0x0052
#define ISP_MBOX_CMD_INIT_RSP_QUEUE_A64 0x0053
#define ISP_MBOX_CMD_LOAD_RAM_A64   0x0009
#define ISP_MBOX_CMD_ABORT_COMMAND  0x0015
#define ISP_MBOX_CMD_ABORT_DEVICE   0x0016
#define ISP_MBOX_CMD_ABORT_TARGET   0x0017
#define ISP_MBOX_CMD_GET_TARGET_PARAMETERS  0x0028
#define ISP_MBOX_CMD_SET_RESET_DELAY_PARAMS 0x003a
#define ISP_MBOX_COMPLETE           0x4000
#define ISP_MBOX_INVALID            0x4001
#define ISP_MBOX_HOST_INTERFACE_ERROR 0x4002
#define ISP_MBOX_TEST_FAILED        0x4003

/* Ring entry layout, restated here the same way the mailbox offsets are. */
#define ISP_QUEUE_ENTRY_SIZE        64
#define ISP_ENTRY_OFF_TYPE          0
#define ISP_ENTRY_OFF_COUNT         1
#define ISP_ENTRY_TYPE_REQUEST      0x01
#define ISP_ENTRY_TYPE_STATUS       0x03
#define ISP_A64_ENTRY_TYPE_COMMAND  0x09
#define ISP_REQ_OFF_HANDLE          4
#define ISP_REQ_OFF_DEVICE          8
#define ISP_REQ_OFF_CDBLEN          10
#define ISP_REQ_OFF_SEGCOUNT        18
#define ISP_REQ_OFF_CDB             20
#define ISP_A64_REQ_OFF_SEGS        40
#define ISP_STATUS_OFF_HANDLE       4
#define ISP_STATUS_OFF_SCSI_STATUS  8
#define ISP_STATUS_OFF_COMPLETION   10
#define ISP_STATUS_OFF_STATE_FLAGS  12
#define ISP_STATUS_OFF_STATUS_FLAGS 14
#define ISP_IOCB_INCOMPLETE         0x0001
#define ISP_STATE_FLAG_GOT_BUS      0x0100
#define ISP_STATE_FLAG_GOT_TARGET   0x0200
#define ISP_STATUS_FLAG_TIMEOUT     0x0040
#define ISP_REG_REQUEST_QUEUE       (ISP_REG_MAILBOX_BASE + 0x08)
#define ISP_REG_RESPONSE_QUEUE      (ISP_REG_MAILBOX_BASE + 0x0a)

/*
 * A ring addressed with a 64-bit base: the direct map keeps the low 31 bits,
 * and the 64-bit base is what selects descriptor byte order.
 */
#define RING_PHYS                   0x00100000u
#define RING_IOVA                   (0xa500000000000000ULL | RING_PHYS)
#define RING_ENTRIES                4
#define RESP_PHYS                   0x00110000u
#define RESP_IOVA                   (0xa500000000000000ULL | RESP_PHYS)
#define BRIDGE_DIRECT_MAP_REG       (BRIDGE_BASE + 0x080 + REG_DATA)

/* Every access to the adapter goes through the window's byte-lane swizzle. */
static void isp_writew(QTestState *qts, unsigned natural, uint16_t val)
{
    qtest_writew(qts, WINDOW_DEVICE0 + GUEST_HALFWORD(natural), val);
}

static uint16_t isp_readw(QTestState *qts, unsigned natural)
{
    return qtest_readw(qts, WINDOW_DEVICE0 + GUEST_HALFWORD(natural));
}

static void isp_mbox_ack(QTestState *qts)
{
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
    isp_writew(qts, ISP_REG_SEMAPHORE, 0);
}

/* Puts one word into the adapter's memory and waits for the answer. */
static void isp_write_ram_word(QTestState *qts, uint16_t addr, uint16_t val)
{
    isp_writew(qts, ISP_MAILBOX(0), ISP_MBOX_CMD_WRITE_RAM_WORD);
    isp_writew(qts, ISP_MAILBOX(1), addr);
    isp_writew(qts, ISP_MAILBOX(2), val);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);
}

/* Mailbox 2 carries the answer, so it goes in holding something else. */
static uint16_t isp_read_ram_word(QTestState *qts, uint16_t addr)
{
    uint16_t val;

    isp_writew(qts, ISP_MAILBOX(0), ISP_MBOX_CMD_READ_RAM_WORD);
    isp_writew(qts, ISP_MAILBOX(1), addr);
    isp_writew(qts, ISP_MAILBOX(2), 0x5a5a);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    val = isp_readw(qts, ISP_MAILBOX(2));
    isp_mbox_ack(qts);
    return val;
}

/*
 * Mailboxes loaded the way a driver loads them; 6 and 7 written even at zero,
 * because mailboxes persist and stale halves would be reassembled.
 */
static void isp_ram_block(QTestState *qts, uint16_t cmd, uint16_t base,
                          uint32_t pci_addr, uint16_t count)
{
    isp_writew(qts, ISP_MAILBOX(0), cmd);
    isp_writew(qts, ISP_MAILBOX(1), base);
    isp_writew(qts, ISP_MAILBOX(2), pci_addr >> 16);
    isp_writew(qts, ISP_MAILBOX(3), pci_addr & 0xffff);
    isp_writew(qts, ISP_MAILBOX(4), count);
    isp_writew(qts, ISP_MAILBOX(6), 0);
    isp_writew(qts, ISP_MAILBOX(7), 0);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
}

/* The same four-mailbox split with the upper half carrying a real value. */
static void isp_ram_block64(QTestState *qts, uint16_t cmd, uint16_t base,
                            uint64_t pci_addr, uint16_t count)
{
    isp_writew(qts, ISP_MAILBOX(0), cmd);
    isp_writew(qts, ISP_MAILBOX(1), base);
    isp_writew(qts, ISP_MAILBOX(2), pci_addr >> 16);
    isp_writew(qts, ISP_MAILBOX(3), pci_addr);
    isp_writew(qts, ISP_MAILBOX(4), count);
    isp_writew(qts, ISP_MAILBOX(6), pci_addr >> 48);
    isp_writew(qts, ISP_MAILBOX(7), pci_addr >> 32);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
}

static void aim_device0_window_at_its_bar(QTestState *qts)
{
    /* Memory space: the aperture a real guest reaches the adapter through. */
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_BAR1, ADAPTER_MMIO);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, REG_BRIDGE_DEVICE0,
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
}

static void test_swap_bit_is_unset(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /*
     * Pinned before and after aiming: the tripwire for a guest ever setting
     * the swap bit both guests leave disabled.
     */
    g_assert_cmphex(qtest_readl(qts, REG_BRIDGE_DEVICE0) & DEV_SWAP_PROGRAMMED,
                    ==, 0);

    aim_device0_window_at_its_bar(qts);
    g_assert_cmphex(qtest_readl(qts, REG_BRIDGE_DEVICE0) & DEV_SWAP_PROGRAMMED,
                    ==, 0);

    qtest_quit(qts);
}

/*
 * qtest_readw/writew, not raw bytes: a host-framed byte decode crosses the
 * region's declared endianness differently and reads the halves reversed.
 */
static void test_reset_signature_through_the_fabric(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    uint64_t guest_mailbox0 = WINDOW_DEVICE0 + GUEST_HALFWORD(ISP_MAILBOX(0));
    uint64_t guest_mailbox1 = WINDOW_DEVICE0 + GUEST_HALFWORD(ISP_MAILBOX(1));
    uint64_t guest_mailbox2 = WINDOW_DEVICE0 + GUEST_HALFWORD(ISP_MAILBOX(2));
    uint64_t guest_mailbox3 = WINDOW_DEVICE0 + GUEST_HALFWORD(ISP_MAILBOX(3));

    aim_device0_window_at_its_bar(qts);

    /* The x86 suite's three values, through the real decode. */
    g_assert_cmphex(qtest_readw(qts, guest_mailbox1), ==, ISP_MAILBOX1_SIGNATURE);
    g_assert_cmphex(qtest_readw(qts, guest_mailbox2), ==, ISP_MAILBOX2_SIGNATURE);
    g_assert_cmphex(qtest_readw(qts, guest_mailbox3), ==, ISP_MAILBOX3_SIGNATURE);

    /*
     * Write-then-read round-trips even through a wrong transform; the check
     * is that mailbox 1 is not the word just written to mailbox 0.
     */
    qtest_writew(qts, guest_mailbox0, 0x1234);
    g_assert_cmphex(qtest_readw(qts, guest_mailbox0), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, guest_mailbox1), ==, ISP_MAILBOX1_SIGNATURE);

    qtest_quit(qts);
}

static void test_ioc3_slot_is_exempt_at_the_same_stride(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /*
     * The exemption half: the IOC3 stays at natural offsets, at exactly the
     * byte granularity a wrongly-applied ^3 would scatter.
     */
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_BAR0, IOC3_MMIO);
    qtest_writel(qts, CFG0(PCI_IOC3_DEVICE) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(PCI_IOC3_DEVICE),
                DEV_MEMORY_SPACE | (IOC3_MMIO >> 20));

    qtest_writeb(qts, WINDOW_IOC3 + SERIAL_A_LCR, 0x03);
    qtest_writeb(qts, WINDOW_IOC3 + SERIAL_A_MCR, 0x05);

    g_assert_cmphex(qtest_readb(qts, WINDOW_IOC3 + SERIAL_A_LCR), ==, 0x03);
    g_assert_cmphex(qtest_readb(qts, WINDOW_IOC3 + SERIAL_A_MCR), ==, 0x05);

    qtest_quit(qts);
}

/*
 * Translated and physical addresses differ, so an untranslated transfer is
 * visible; both directions run because one can be right alone.
 */
static void test_ram_dma_translates_through_the_bridge(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    const uint16_t base = 0x1000;
    const uint16_t first = 0x1234, second = 0x5678;
    uint8_t seen[4];

    aim_device0_window_at_its_bar(qts);

    /* Nothing the adapter does reaches memory until it may master the bus. */
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE, 0));

    /* So a byte found in the page afterwards was put there by the adapter. */
    qtest_memset(qts, TARGET_PAGE, 0, PAGE_SIZE_4K);

    isp_write_ram_word(qts, base, first);

    isp_ram_block(qts, ISP_MBOX_CMD_DUMP_RAM, base, PCI_TRANSLATED, 1);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    /*
     * Raw bytes, because a word accessor would apply its own correction and
     * hide the effect under test: with the window's swap bit clear a two-byte
     * transfer at offset zero lands at offset zero ^ two with its halves
     * reversed.
     */
    qtest_memread(qts, TARGET_PAGE + 2, seen, 2);
    g_assert_cmphex(seen[0], ==, first >> 8);
    g_assert_cmphex(seen[1], ==, first & 0xff);

    /* And nothing landed where an uncorrected transfer would have. */
    qtest_memread(qts, TARGET_PAGE, seen, 2);
    g_assert_cmphex(seen[0], ==, 0);
    g_assert_cmphex(seen[1], ==, 0);

    /* The other direction, from the same translated page. */
    seen[0] = second >> 8;
    seen[1] = second & 0xff;
    qtest_memwrite(qts, TARGET_PAGE + 2, seen, 2);

    isp_ram_block(qts, ISP_MBOX_CMD_LOAD_RAM, base, PCI_TRANSLATED, 1);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    isp_writew(qts, ISP_MAILBOX(0), ISP_MBOX_CMD_READ_RAM_WORD);
    isp_writew(qts, ISP_MAILBOX(1), base);
    isp_writew(qts, ISP_MAILBOX(2), 0x5a5a);    /* not the answer */
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, second);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/*
 * This part moves controller RAM one 16-bit cycle per word. Two words,
 * because a single word cannot tell one reassembly order from another. The
 * order is written down here rather than left to whichever decomposition the
 * fabric is handed: at these alignments the per-word and block splits were
 * measured to deliver the same bytes, so what this pins is the order, not the
 * width.
 */
static void test_ram_load_reassembles_two_words_per_word(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    const uint16_t base = 0x1000;
    const uint16_t first = 0x1234, second = 0xabcd;
    uint8_t page[4];

    aim_device0_window_at_its_bar(qts);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE, 0));
    qtest_memset(qts, TARGET_PAGE, 0, PAGE_SIZE_4K);

    /* Neither word is the value the load is meant to leave behind. */
    isp_write_ram_word(qts, base, 0x5a5a);
    isp_write_ram_word(qts, base + 1, 0x5a5a);

    /*
     * Raw bytes, laid out as the per-word correction reads them back: offset
     * zero's halfword comes from offset two with its halves reversed, and
     * offset two's from offset zero, the same swap
     * test_ram_dma_translates_through_the_bridge pins one word at a time.
     */
    page[0] = second >> 8;
    page[1] = second & 0xff;
    page[2] = first >> 8;
    page[3] = first & 0xff;
    qtest_memwrite(qts, TARGET_PAGE, page, sizeof(page));

    isp_ram_block(qts, ISP_MBOX_CMD_LOAD_RAM, base, PCI_TRANSLATED, 2);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    g_assert_cmphex(isp_read_ram_word(qts, base), ==, first);
    g_assert_cmphex(isp_read_ram_word(qts, base + 1), ==, second);

    qtest_quit(qts);
}

/*
 * A dump the fabric refuses is a failed command, not a completed one: a guest
 * told the transfer completed would read back host memory the adapter never
 * wrote and take whatever was already there for an image.
 */
static void test_ram_dump_failed_dma_answers_an_error(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_64M);
    const uint16_t base = 0x1000;

    aim_device0_window_at_its_bar(qts);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    isp_write_ram_word(qts, base, 0x1234);

    isp_ram_block64(qts, ISP_MBOX_CMD_DUMP_RAM, base, DIRECT64(RAM_TOP_64M),
                    4);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_HOST_INTERFACE_ERROR);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/*
 * Two addresses written before either is read; answering with the most recent
 * write returns the second value and fails.
 */
static void test_read_ram_word_through_the_fabric(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    const uint16_t addr_a = 0x0800, addr_b = 0x0c00;
    const uint16_t val_a = 0x1234, val_b = 0xfedc;

    aim_device0_window_at_its_bar(qts);

    isp_write_ram_word(qts, addr_a, val_a);
    isp_write_ram_word(qts, addr_b, val_b);

    /*
     * Both output mailboxes are poisoned with values that are not the answer,
     * or a model that never wrote them could pass by accident.
     */
    isp_writew(qts, ISP_MAILBOX(0), ISP_MBOX_CMD_READ_RAM_WORD);
    isp_writew(qts, ISP_MAILBOX(1), addr_a);
    isp_writew(qts, ISP_MAILBOX(2), 0x5a5a);
    isp_writew(qts, ISP_MAILBOX(3), 0xc3c3);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, val_a);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(3)), ==, 0);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/* Little-endian field access into a locally built entry. */
static void put32le(uint8_t *b, uint32_t v)
{
    b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}

static void put16le(uint8_t *b, uint16_t v)
{
    b[0] = v; b[1] = v >> 8;
}

static uint32_t get32le(const uint8_t *b)
{
    return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t get16le(const uint8_t *b)
{
    return b[0] | (b[1] << 8);
}

/*
 * Reverse each aligned 32-bit word, the correction a word-value ring needs.
 */
static void normalise_words(uint8_t *buf, size_t len)
{
    size_t i;

    for (i = 0; i + 4 <= len; i += 4) {
        uint8_t t = buf[i];

        buf[i] = buf[i + 3];
        buf[i + 3] = t;
        t = buf[i + 1];
        buf[i + 1] = buf[i + 2];
        buf[i + 2] = t;
    }
}

static void isp_init_queue_a64(QTestState *qts, uint16_t cmd, uint16_t count,
                               uint64_t iova)
{
    isp_writew(qts, ISP_MAILBOX(0), cmd);
    isp_writew(qts, ISP_MAILBOX(1), count);
    isp_writew(qts, ISP_MAILBOX(2), (iova >> 16) & 0xffff);
    isp_writew(qts, ISP_MAILBOX(3), iova & 0xffff);
    isp_writew(qts, ISP_MAILBOX(6), (iova >> 48) & 0xffff);
    isp_writew(qts, ISP_MAILBOX(7), (iova >> 32) & 0xffff);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);
}

/*
 * A 64-bit ring base selects word-reversed descriptors both directions; an
 * absent target still completes with the handle, so no disk is needed.
 */
static void test_a64_ring_is_word_value(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    const uint32_t handle = 0x1234abcd;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    aim_device0_window_at_its_bar(qts);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    /* Direct map with no offset, so the low bits reach memory unchanged. */
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);

    qtest_memset(qts, RING_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);

    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_RSP_QUEUE_A64, RING_ENTRIES,
                       RESP_IOVA);
    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, RING_ENTRIES,
                       RING_IOVA);

    /*
 * Built canonically, then reversed per word: one transform covers the bytes,
 * the 32-bit handle and the 16-bit fields together. This is the property a
 * per-field correction would break.
 */
    memset(entry, 0, sizeof(entry));
    entry[ISP_ENTRY_OFF_TYPE] = ISP_A64_ENTRY_TYPE_COMMAND;
    entry[ISP_ENTRY_OFF_COUNT] = 1;
    put32le(entry + ISP_REQ_OFF_HANDLE, handle);
    put16le(entry + ISP_REQ_OFF_DEVICE, 0x0100);    /* target 1, lun 0 */
    put16le(entry + ISP_REQ_OFF_CDBLEN, 6);
    put16le(entry + ISP_REQ_OFF_SEGCOUNT, 0);
    normalise_words(entry, sizeof(entry));
    qtest_memwrite(qts, RING_PHYS, entry, sizeof(entry));

    /* Publish one entry and let the adapter run it. */
    isp_writew(qts, ISP_REG_REQUEST_QUEUE, 1);

    /*
     * The completion reads back through the same correction; raw, the type
     * byte is visibly not a status entry.
     */
    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 1);

    qtest_memread(qts, RESP_PHYS, status, sizeof(status));
    g_assert_cmphex(status[ISP_ENTRY_OFF_TYPE], !=, ISP_ENTRY_TYPE_STATUS);
    normalise_words(status, sizeof(status));
    g_assert_cmphex(status[ISP_ENTRY_OFF_TYPE], ==, ISP_ENTRY_TYPE_STATUS);
    g_assert_cmphex(get32le(status + ISP_STATUS_OFF_HANDLE), ==, handle);

    qtest_quit(qts);
}

/*
 * Pinned: completion, the segment cursor, and the guest buffer untouched. Not
 * pinned: bytes reaching the medium. A round trip cancels the correction,
 * and only a reader outside the ring's byte order settles it.
 */
static void test_a64_ring_data_out(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -drive id=d0,if=none,file=null-co://,"
                                 "size=1048576,format=raw"
                                 " -device scsi-hd,bus=scsi.0,scsi-id=1,"
                                 "lun=0,drive=d0");
    const uint32_t handle = 0x55aa1234;
    const uint32_t buf_phys = 0x00120000;
    const uint64_t buf_iova = 0xa500000000000000ULL | buf_phys;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t block[512], seen[512];
    unsigned i;

    aim_device0_window_at_its_bar(qts);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);

    qtest_memset(qts, RING_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);

    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_RSP_QUEUE_A64, RING_ENTRIES,
                       RESP_IOVA);
    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, RING_ENTRIES,
                       RING_IOVA);

    /*
     * Every byte differs from its neighbours, so a missing, doubled or
     * wrong-width correction shows rather than cancelling.
     */
    for (i = 0; i < sizeof(block); i++) {
        block[i] = (uint8_t)(i * 7 + 1);
    }
    memcpy(seen, block, sizeof(seen));
    normalise_words(seen, sizeof(seen));
    qtest_memwrite(qts, buf_phys, seen, sizeof(seen));

    memset(entry, 0, sizeof(entry));
    entry[ISP_ENTRY_OFF_TYPE] = ISP_A64_ENTRY_TYPE_COMMAND;
    entry[ISP_ENTRY_OFF_COUNT] = 1;
    put32le(entry + ISP_REQ_OFF_HANDLE, handle);
    put16le(entry + ISP_REQ_OFF_DEVICE, 0x0100);
    put16le(entry + ISP_REQ_OFF_CDBLEN, 10);
    put16le(entry + ISP_REQ_OFF_SEGCOUNT, 1);
    /* WRITE(10), one block at LBA 0. */
    entry[ISP_REQ_OFF_CDB + 0] = 0x2a;
    entry[ISP_REQ_OFF_CDB + 8] = 1;
    put32le(entry + ISP_A64_REQ_OFF_SEGS + 0, (uint32_t)buf_iova);
    put32le(entry + ISP_A64_REQ_OFF_SEGS + 4, (uint32_t)(buf_iova >> 32));
    put32le(entry + ISP_A64_REQ_OFF_SEGS + 8, sizeof(block));
    normalise_words(entry, sizeof(entry));
    qtest_memwrite(qts, RING_PHYS, entry, sizeof(entry));

    isp_writew(qts, ISP_REG_REQUEST_QUEUE, 1);
    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 1);

    /* The guest's own buffer must not have been rewritten under it. */
    qtest_memread(qts, buf_phys, seen, sizeof(seen));
    {
        uint8_t expect[512];

        memcpy(expect, block, sizeof(expect));
        normalise_words(expect, sizeof(expect));
        g_assert_cmpmem(seen, sizeof(seen), expect, sizeof(expect));
    }

    qtest_quit(qts);
}

/*
 * A drive self-test completes GOOD through the command ring: IRIX fx issues
 * SEND DIAGNOSTIC when it opens a disk and reports the disk broken on CHECK
 * CONDITION.
 */
static void build_a64_command(uint8_t *entry, uint32_t handle, uint16_t device,
                              uint8_t op, uint8_t op_byte1)
{
    memset(entry, 0, ISP_QUEUE_ENTRY_SIZE);
    entry[ISP_ENTRY_OFF_TYPE] = ISP_A64_ENTRY_TYPE_COMMAND;
    entry[ISP_ENTRY_OFF_COUNT] = 1;
    put32le(entry + ISP_REQ_OFF_HANDLE, handle);
    put16le(entry + ISP_REQ_OFF_DEVICE, device);
    put16le(entry + ISP_REQ_OFF_CDBLEN, 6);
    put16le(entry + ISP_REQ_OFF_SEGCOUNT, 0);
    entry[ISP_REQ_OFF_CDB + 0] = op;
    entry[ISP_REQ_OFF_CDB + 1] = op_byte1;
    normalise_words(entry, ISP_QUEUE_ENTRY_SIZE);
}

/*
 * The 32-bit command entry (type 0x01): the header fields sit at the same
 * offsets as the A64 form, so only the type byte differs.
 */
static void build_command(uint8_t *entry, uint32_t handle, uint16_t device,
                          uint8_t op, uint8_t op_byte1)
{
    memset(entry, 0, ISP_QUEUE_ENTRY_SIZE);
    entry[ISP_ENTRY_OFF_TYPE] = ISP_ENTRY_TYPE_REQUEST;
    entry[ISP_ENTRY_OFF_COUNT] = 1;
    put32le(entry + ISP_REQ_OFF_HANDLE, handle);
    put16le(entry + ISP_REQ_OFF_DEVICE, device);
    put16le(entry + ISP_REQ_OFF_CDBLEN, 6);
    put16le(entry + ISP_REQ_OFF_SEGCOUNT, 0);
    entry[ISP_REQ_OFF_CDB + 0] = op;
    entry[ISP_REQ_OFF_CDB + 1] = op_byte1;
    normalise_words(entry, ISP_QUEUE_ENTRY_SIZE);
}

static void test_a64_ring_send_diagnostic(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -drive id=d0,if=none,file=null-co://,"
                                 "size=1048576,format=raw"
                                 " -device scsi-hd,bus=scsi.0,scsi-id=1,"
                                 "lun=0,drive=d0");
    const uint32_t diag_handle = 0x5d1a0001;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    aim_device0_window_at_its_bar(qts);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);

    qtest_memset(qts, RING_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);

    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_RSP_QUEUE_A64, RING_ENTRIES,
                       RESP_IOVA);
    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, RING_ENTRIES,
                       RING_IOVA);

    /*
     * A TEST UNIT READY first, to absorb the drive's power-on UNIT ATTENTION;
     * then the SEND DIAGNOSTIC whose status is checked.
     */
    build_a64_command(entry, 0x5d1a0000, 0x0100, 0x00, 0x00);
    qtest_memwrite(qts, RING_PHYS, entry, sizeof(entry));
    build_a64_command(entry, diag_handle, 0x0100, 0x1d, 0x04);
    qtest_memwrite(qts, RING_PHYS + ISP_QUEUE_ENTRY_SIZE, entry, sizeof(entry));

    isp_writew(qts, ISP_REG_REQUEST_QUEUE, 2);
    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 2);

    qtest_memread(qts, RESP_PHYS + ISP_QUEUE_ENTRY_SIZE, status,
                  sizeof(status));
    normalise_words(status, sizeof(status));
    g_assert_cmphex(status[ISP_ENTRY_OFF_TYPE], ==, ISP_ENTRY_TYPE_STATUS);
    g_assert_cmphex(get32le(status + ISP_STATUS_OFF_HANDLE), ==, diag_handle);
    /* GOOD, not CHECK CONDITION. */
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);

    qtest_quit(qts);
}


/*
 * Empty target: GOT_BUS, no GOT_TARGET, and the selection-timeout bit only
 * IRIX reads. Without it IRIX faults every empty target it probes.
 */
static void test_a64_ring_missing_target(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -drive id=d0,if=none,file=null-co://,"
                                 "size=1048576,format=raw"
                                 " -device scsi-hd,bus=scsi.0,scsi-id=1,"
                                 "lun=0,drive=d0");
    const uint32_t probe_handle = 0x0e0e7a01;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint16_t state_flags;

    aim_device0_window_at_its_bar(qts);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);

    qtest_memset(qts, RING_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);

    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_RSP_QUEUE_A64, RING_ENTRIES,
                       RESP_IOVA);
    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, RING_ENTRIES,
                       RING_IOVA);

    /*
     * device 0x0500 = target 5, lun 0, an empty slot; op is TEST UNIT READY.
     */
    build_a64_command(entry, probe_handle, 0x0500, 0x00, 0x00);
    qtest_memwrite(qts, RING_PHYS, entry, sizeof(entry));

    isp_writew(qts, ISP_REG_REQUEST_QUEUE, 1);
    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 1);

    qtest_memread(qts, RESP_PHYS, status, sizeof(status));
    normalise_words(status, sizeof(status));

    g_assert_cmphex(status[ISP_ENTRY_OFF_TYPE], ==, ISP_ENTRY_TYPE_STATUS);
    g_assert_cmphex(get32le(status + ISP_STATUS_OFF_HANDLE), ==, probe_handle);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_INCOMPLETE);

    /* Reached the bus, selected nothing. */
    state_flags = get16le(status + ISP_STATUS_OFF_STATE_FLAGS);
    g_assert_cmphex(state_flags & ISP_STATE_FLAG_GOT_BUS, ==,
                    ISP_STATE_FLAG_GOT_BUS);
    g_assert_cmphex(state_flags & ISP_STATE_FLAG_GOT_TARGET, ==, 0);

    /* The bit IRIX needs to accept the empty slot instead of faulting. */
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATUS_FLAGS), ==,
                    ISP_STATUS_FLAG_TIMEOUT);

    qtest_quit(qts);
}


/*
 * The same completion through the 32-bit entry: the entry type, not the ring,
 * selects the handler.
 */
static void test_ring_missing_target(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -drive id=d0,if=none,file=null-co://,"
                                 "size=1048576,format=raw"
                                 " -device scsi-hd,bus=scsi.0,scsi-id=1,"
                                 "lun=0,drive=d0");
    const uint32_t probe_handle = 0x0e0e7a20;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint16_t state_flags;

    aim_device0_window_at_its_bar(qts);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);

    qtest_memset(qts, RING_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);

    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_RSP_QUEUE_A64, RING_ENTRIES,
                       RESP_IOVA);
    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, RING_ENTRIES,
                       RING_IOVA);

    /*
     * device 0x0500 = target 5, lun 0, an empty slot; op is TEST UNIT READY.
     */
    build_command(entry, probe_handle, 0x0500, 0x00, 0x00);
    qtest_memwrite(qts, RING_PHYS, entry, sizeof(entry));

    isp_writew(qts, ISP_REG_REQUEST_QUEUE, 1);
    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 1);

    qtest_memread(qts, RESP_PHYS, status, sizeof(status));
    normalise_words(status, sizeof(status));

    g_assert_cmphex(status[ISP_ENTRY_OFF_TYPE], ==, ISP_ENTRY_TYPE_STATUS);
    g_assert_cmphex(get32le(status + ISP_STATUS_OFF_HANDLE), ==, probe_handle);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_INCOMPLETE);

    /* Reached the bus, selected nothing. */
    state_flags = get16le(status + ISP_STATUS_OFF_STATE_FLAGS);
    g_assert_cmphex(state_flags & ISP_STATE_FLAG_GOT_BUS, ==,
                    ISP_STATE_FLAG_GOT_BUS);
    g_assert_cmphex(state_flags & ISP_STATE_FLAG_GOT_TARGET, ==, 0);

    /* The bit IRIX needs to accept the empty slot instead of faulting. */
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATUS_FLAGS), ==,
                    ISP_STATUS_FLAG_TIMEOUT);

    qtest_quit(qts);
}


/*
 * Both output mailboxes are poisoned first: zero is exactly the answer a
 * stale mailbox could hold by accident.
 */
static void test_verify_checksum_answers_both_mailboxes(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    const uint16_t base = 0x2000;

    aim_device0_window_at_its_bar(qts);

    /*
     * Word three of the image is its length; with the rest zero the sum is
     * the length itself, a value no mailbox holds by accident.
     */
    isp_write_ram_word(qts, base + 3, 4);

    isp_writew(qts, ISP_MAILBOX(1), base);
    isp_writew(qts, ISP_MAILBOX(2), 0x5a5a);    /* not the answer */
    isp_writew(qts, ISP_MAILBOX(0), ISP_MBOX_CMD_VERIFY_CHECKSUM);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    /* A non-zero sum is a failing image, and both mailboxes carry the sum. */
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_TEST_FAILED);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(1)), ==, 4);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, 4);
    isp_mbox_ack(qts);

    /*
     * And a zero-sum image completes: both halves together rule out a model
     * that always fails or always reports the length.
     */
    isp_write_ram_word(qts, base, 0xfffc);

    isp_writew(qts, ISP_MAILBOX(1), base);
    isp_writew(qts, ISP_MAILBOX(2), 0x5a5a);
    isp_writew(qts, ISP_MAILBOX(0), ISP_MBOX_CMD_VERIFY_CHECKSUM);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(1)), ==, 0);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, 0);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}


/*
 * Through the DIRECT window: its swap bit is the other of the two, so
 * consulting the wrong bit passes the translated test and fails the boot.
 */
static void test_ram_dma_through_the_direct_window(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    const uint16_t base = 0x1000;
    const uint16_t word = 0x1234;
    uint8_t seen[4];

    aim_device0_window_at_its_bar(qts);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    /*
     * The add-512-megabytes field resets set; left alone it moves the
     * transfer half a gigabyte away, which reads as a byte-order failure and
     * is not one.
     */
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);

    qtest_memset(qts, TARGET_PAGE, 0, PAGE_SIZE_4K);
    isp_write_ram_word(qts, base, word);

    isp_ram_block(qts, ISP_MBOX_CMD_DUMP_RAM, base,
                  0x80000000u | TARGET_PAGE, 1);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    /*
     * With the window's bit clear, offset zero lands at zero ^ two, halves
     * reversed.
     */
    qtest_memread(qts, TARGET_PAGE + 2, seen, 2);
    g_assert_cmphex(seen[0], ==, word >> 8);
    g_assert_cmphex(seen[1], ==, word & 0xff);

    /* And the offset an uncorrected transfer would have used is untouched. */
    qtest_memread(qts, TARGET_PAGE, seen, 2);
    g_assert_cmphex(seen[0], ==, 0);
    g_assert_cmphex(seen[1], ==, 0);

    qtest_quit(qts);
}

static void test_pci_status_reset_value(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /*
     * IRIX's io_config_space diagnostic compares the whole Command/Status
     * dword against 0x02000000 (Status 0x0200, medium DEVSEL) and fails
     * the I/O slot on a zero Status half. Only the Status half is asserted
     * here: the board pre-enables Command for no-firmware bring-up, and the
     * miniroot's own reset clears the writable half while the read-only
     * Status survives.
     */
    g_assert_cmphex(qtest_readl(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND) >> 16,
                    ==, 0x0200u);

    /*
     * The capability bits are read-only and the error latches
     * write-one-to-clear from zero, so writing all ones leaves the Status
     * half exactly 0x0200.
     */
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND) >> 16,
                    ==, 0x0200u);

    qtest_quit(qts);
}

/*
 * GET_TARGET_PARAMETERS answers async/narrow and completes; the
 * invalid-command answer made Linux log a mailbox failure per target.
 */
static void test_mbox_get_target_parameters(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    aim_device0_window_at_its_bar(qts);

    isp_writew(qts, ISP_MAILBOX(1), 1 << 8);    /* target 1 */
    isp_writew(qts, ISP_MAILBOX(0), ISP_MBOX_CMD_GET_TARGET_PARAMETERS);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, 0);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(3)), ==, 0);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/*
 * SET_RESET_DELAY_PARAMS completes rather than answering invalid, which left
 * IRIX logging a mailbox error during adapter init.
 */
static void test_mbox_set_reset_delay_params(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    aim_device0_window_at_its_bar(qts);

    isp_writew(qts, ISP_MAILBOX(0), ISP_MBOX_CMD_SET_RESET_DELAY_PARAMS);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/*
 * Bit 15 of the target-parameter selector picks a bus on the family's
 * dual-channel part. This part has one bus and never read the bit, so a
 * request carrying it gets the same answer as one without: turning it into a
 * parameter error would be a new refusal of a request drivers have always
 * been able to make.
 */
static void test_mbox_get_target_parameters_ignores_bit_15(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    aim_device0_window_at_its_bar(qts);

    isp_writew(qts, ISP_MAILBOX(1), 0x8100);    /* target 1, bit 15 set */
    isp_writew(qts, ISP_MAILBOX(0), ISP_MBOX_CMD_GET_TARGET_PARAMETERS);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, 0);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(3)), ==, 0);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/*
 * The A64 RAM load is the dual-channel part's opcode. Nothing in this part's
 * contract carries it, so answering it would invent a command, and a guest
 * that probed for it would conclude this adapter has one.
 */
static void test_mbox_a64_ram_load_is_not_this_part(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    aim_device0_window_at_its_bar(qts);

    isp_writew(qts, ISP_MAILBOX(0), ISP_MBOX_CMD_LOAD_RAM_A64);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_INVALID);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/*
 * Abort command, abort device and target reset are the dual-channel part's
 * opcodes. This part's contract lists no abort or target-reset mailbox at
 * all, so answering one would tell a guest that probes for it that this
 * adapter can retire a command, and the guest would then wait for a
 * retirement that never arrives.
 */
static void test_mbox_recovery_commands_are_not_this_part(const void *data)
{
    static const uint16_t commands[] = {
        ISP_MBOX_CMD_ABORT_COMMAND,
        ISP_MBOX_CMD_ABORT_DEVICE,
        ISP_MBOX_CMD_ABORT_TARGET,
    };
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    unsigned i;

    aim_device0_window_at_its_bar(qts);

    for (i = 0; i < ARRAY_SIZE(commands); i++) {
        isp_writew(qts, ISP_MAILBOX(1), 1 << 8);    /* target 1, LUN 0 */
        isp_writew(qts, ISP_MAILBOX(2), 0);
        isp_writew(qts, ISP_MAILBOX(3), 0);
        isp_writew(qts, ISP_MAILBOX(0), commands[i]);
        isp_writew(qts, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

        g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_INVALID);
        isp_mbox_ack(qts);
    }

    qtest_quit(qts);
}

/*
 * Two LUNs behind one target, an address this adapter has always taken. The
 * INQUIRY each one answers is what says the second disk is really there: a
 * LUN with nothing behind it is answered by the target itself, with the
 * peripheral qualifier that means no device.
 */
#define INQUIRY_BYTES               36
#define INQUIRY_NO_DEVICE           0x7f
#define LUN_BUF_PHYS                0x00120000u
#define LUN_BUF_IOVA                (0xa500000000000000ULL | LUN_BUF_PHYS)

static void build_a64_inquiry(uint8_t *entry, uint32_t handle,
                              uint16_t device, uint64_t buf_iova)
{
    memset(entry, 0, ISP_QUEUE_ENTRY_SIZE);
    entry[ISP_ENTRY_OFF_TYPE] = ISP_A64_ENTRY_TYPE_COMMAND;
    entry[ISP_ENTRY_OFF_COUNT] = 1;
    put32le(entry + ISP_REQ_OFF_HANDLE, handle);
    put16le(entry + ISP_REQ_OFF_DEVICE, device);
    put16le(entry + ISP_REQ_OFF_CDBLEN, 6);
    put16le(entry + ISP_REQ_OFF_SEGCOUNT, 1);
    entry[ISP_REQ_OFF_CDB + 0] = 0x12;
    entry[ISP_REQ_OFF_CDB + 4] = INQUIRY_BYTES;
    put32le(entry + ISP_A64_REQ_OFF_SEGS + 0, (uint32_t)buf_iova);
    put32le(entry + ISP_A64_REQ_OFF_SEGS + 4, (uint32_t)(buf_iova >> 32));
    put32le(entry + ISP_A64_REQ_OFF_SEGS + 8, INQUIRY_BYTES);
    normalise_words(entry, ISP_QUEUE_ENTRY_SIZE);
}

static void test_two_luns_behind_one_target(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -drive id=d0,if=none,file=null-co://,"
                                 "size=1048576,format=raw"
                                 " -device scsi-hd,bus=scsi.0,scsi-id=1,"
                                 "lun=0,drive=d0"
                                 " -drive id=d1,if=none,file=null-co://,"
                                 "size=1048576,format=raw"
                                 " -device scsi-hd,bus=scsi.0,scsi-id=1,"
                                 "lun=1,drive=d1");
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t payload[INQUIRY_BYTES];
    unsigned lun;

    aim_device0_window_at_its_bar(qts);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);

    qtest_memset(qts, RING_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);

    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_RSP_QUEUE_A64, RING_ENTRIES,
                       RESP_IOVA);
    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, RING_ENTRIES,
                       RING_IOVA);

    for (lun = 0; lun < 2; lun++) {
        uint32_t handle = 0x10a00000 + lun;

        qtest_memset(qts, LUN_BUF_PHYS, 0xcc, INQUIRY_BYTES);
        build_a64_inquiry(entry, handle, 0x0100 | lun, LUN_BUF_IOVA);
        qtest_memwrite(qts, RING_PHYS + lun * ISP_QUEUE_ENTRY_SIZE, entry,
                       sizeof(entry));
        isp_writew(qts, ISP_REG_REQUEST_QUEUE, lun + 1);
        g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, lun + 1);

        qtest_memread(qts, RESP_PHYS + lun * ISP_QUEUE_ENTRY_SIZE, status,
                      sizeof(status));
        normalise_words(status, sizeof(status));
        g_assert_cmphex(get32le(status + ISP_STATUS_OFF_HANDLE), ==, handle);
        g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);

        qtest_memread(qts, LUN_BUF_PHYS, payload, sizeof(payload));
        normalise_words(payload, sizeof(payload));
        g_assert_cmphex(payload[0], !=, INQUIRY_NO_DEVICE);
        g_assert_cmpmem(payload + 8, 8, "QEMU    ", 8);
    }

    qtest_quit(qts);
}

/*
 * The fabric-refused-transfer pair: with 64 MB of memory the first unbacked
 * address is right after it, and a transfer there is refused the way one
 * through an invalid translation entry would be.
 */
#define ISP_REG_BUS_STATUS          0x0a
#define ISP_BUS_STATUS_INT_REQ      0x0002
#define ISP_BUS_STATUS_RISC_INT_REQ 0x0004
#define ISP_BUS_STATUS_INT_BITS     (ISP_BUS_STATUS_INT_REQ | \
                                     ISP_BUS_STATUS_RISC_INT_REQ)
#define RAM_TOP_64M                 0x04000000u
#define STRADDLE_PHYS               (RAM_TOP_64M - ISP_REQ_OFF_CDB)
#define STRADDLE_IOVA               (0xa500000000000000ULL | STRADDLE_PHYS)
#define UNBACKED_RESP_IOVA          (0xa500000000000000ULL | RAM_TOP_64M)

/*
 * A request entry the fabric only half answers: headers in the last bytes of
 * memory, the CDB starting exactly at the boundary, so the header reads
 * succeed and the CDB read is refused. The pinned outcome is a drop: consume
 * index on the entry, nothing posted or raised. The discriminating shape
 * matters: an entirely unreadable entry takes the fail-visibly default and
 * posts a completion, which would pass for the wrong reason.
 */
static void test_a_refused_cdb_read_drops_the_request(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE " -m 64");
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint16_t quiescent;

    aim_device0_window_at_its_bar(qts);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);

    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);

    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_RSP_QUEUE_A64, RING_ENTRIES,
                       RESP_IOVA);
    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, RING_ENTRIES,
                       STRADDLE_IOVA);
    quiescent = isp_readw(qts, ISP_REG_BUS_STATUS) & ISP_BUS_STATUS_INT_BITS;

    /*
     * Only the header bytes are written. The rest has no memory behind it.
     * The device word names an empty target, which would complete
     * incomplete-with-timeout if the entry were acted on at all.
     */
    build_command(entry, 0x51add1e5, 0x0500, 0x00, 0x00);
    qtest_memwrite(qts, STRADDLE_PHYS, entry, ISP_REQ_OFF_CDB);

    isp_writew(qts, ISP_REG_REQUEST_QUEUE, 1);

    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 0);
    g_assert_cmphex(isp_readw(qts, ISP_REG_REQUEST_QUEUE), ==, 0);
    g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_STATUS) &
                    ISP_BUS_STATUS_INT_BITS, ==, quiescent);

    qtest_quit(qts);
}

/*
 * The write half: a completion bound for a refused response ring is lost, but
 * the produce index must not step over a slot never written and the interrupt
 * must not be raised. Together they point the guest at a stale entry as
 * this command's answer.
 */
static void test_a_refused_status_write_raises_no_interrupt(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE " -m 64");
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint16_t quiescent;

    aim_device0_window_at_its_bar(qts);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);

    qtest_memset(qts, RING_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);

    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_RSP_QUEUE_A64, RING_ENTRIES,
                       UNBACKED_RESP_IOVA);
    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, RING_ENTRIES,
                       RING_IOVA);
    quiescent = isp_readw(qts, ISP_REG_BUS_STATUS) & ISP_BUS_STATUS_INT_BITS;

    /* A missing target completes without needing a disk behind it. */
    build_command(entry, 0x0e0e7a21, 0x0500, 0x00, 0x00);
    qtest_memwrite(qts, RING_PHYS, entry, sizeof(entry));

    isp_writew(qts, ISP_REG_REQUEST_QUEUE, 1);

    /*
     * The request itself was readable and is consumed; only its completion is
     * dropped, not faked.
     */
    g_assert_cmphex(isp_readw(qts, ISP_REG_REQUEST_QUEUE), ==, 1);
    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 0);
    g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_STATUS) &
                    ISP_BUS_STATUS_INT_BITS, ==, quiescent);

    qtest_quit(qts);
}

/*
 * The status entry pairs two 16-bit values into each 32-bit word:
 * scsi_status with completion, state_flags with status_flags. The firmware
 * reads the A64 response ring whole-word (the word-value reversal
 * normalise_words undoes here), and the four missing-target values are
 * pairwise distinct, so a change that transposed either shared pair
 * (reordering the entry's DMA or reading a field at the wrong lane) fails
 * here, where the missing-target tests, which assert the completion's meaning
 * rather than its byte placement, might not. This pins the placement.
 */
static void test_status_pairs_survive_word_reversal(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE
                                 " -drive id=d0,if=none,file=null-co://,"
                                 "size=1048576,format=raw"
                                 " -device scsi-hd,bus=scsi.0,scsi-id=1,"
                                 "lun=0,drive=d0");
    const uint32_t probe_handle = 0x50a1b2c3;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    aim_device0_window_at_its_bar(qts);
    qtest_writel(qts, CFG0(SCSI_DEVICE0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);

    qtest_memset(qts, RING_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RING_ENTRIES);

    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_RSP_QUEUE_A64, RING_ENTRIES,
                       RESP_IOVA);
    isp_init_queue_a64(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, RING_ENTRIES,
                       RING_IOVA);

    /*
     * Target 5 is an empty slot on the live bus; its completion carries the
     * missing-target flags, whose four values are pairwise distinct.
     */
    build_a64_command(entry, probe_handle, 0x0500, 0x00, 0x00);
    qtest_memwrite(qts, RING_PHYS, entry, sizeof(entry));

    isp_writew(qts, ISP_REG_REQUEST_QUEUE, 1);
    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 1);

    qtest_memread(qts, RESP_PHYS, status, sizeof(status));
    normalise_words(status, sizeof(status));

    g_assert_cmphex(status[ISP_ENTRY_OFF_TYPE], ==, ISP_ENTRY_TYPE_STATUS);
    /*
     * Each 16-bit field recovered in its own lane; a transposed half fails
     * both members of its pair.
     */
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0x0000);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_INCOMPLETE);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATE_FLAGS), ==,
                    ISP_STATE_FLAG_GOT_BUS);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATUS_FLAGS), ==,
                    ISP_STATUS_FLAG_TIMEOUT);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/qlogic-isp/pci-status-reset-value", NULL,
                        test_pci_status_reset_value);
    qtest_add_data_func("sn0/qlogic-isp/swap-bit-is-unset", NULL,
                        test_swap_bit_is_unset);
    qtest_add_data_func("sn0/qlogic-isp/reset-signature-through-the-fabric",
                        NULL, test_reset_signature_through_the_fabric);
    qtest_add_data_func("sn0/qlogic-isp/ioc3-slot-is-exempt-at-the-same-stride",
                        NULL, test_ioc3_slot_is_exempt_at_the_same_stride);
    qtest_add_data_func("sn0/qlogic-isp/ram-dma-translates-through-the-bridge",
                        NULL, test_ram_dma_translates_through_the_bridge);
    qtest_add_data_func("sn0/qlogic-isp/ram-load-reassembles-two-words",
                        NULL, test_ram_load_reassembles_two_words_per_word);
    qtest_add_data_func("sn0/qlogic-isp/ram-dump-failed-dma-answers-an-error",
                        NULL, test_ram_dump_failed_dma_answers_an_error);
    qtest_add_data_func("sn0/qlogic-isp/read-ram-word-through-the-fabric",
                        NULL, test_read_ram_word_through_the_fabric);
    qtest_add_data_func("sn0/qlogic-isp/a64-ring-is-word-value", NULL,
                        test_a64_ring_is_word_value);
    qtest_add_data_func("sn0/qlogic-isp/a64-ring-data-out", NULL,
                        test_a64_ring_data_out);
    qtest_add_data_func("sn0/qlogic-isp/a64-ring-send-diagnostic", NULL,
                        test_a64_ring_send_diagnostic);
    qtest_add_data_func("sn0/qlogic-isp/a64-ring-missing-target", NULL,
                        test_a64_ring_missing_target);
    qtest_add_data_func("sn0/qlogic-isp/ring-missing-target", NULL,
                        test_ring_missing_target);
    qtest_add_data_func("sn0/qlogic-isp/status-pairs-survive-word-reversal",
                        NULL, test_status_pairs_survive_word_reversal);
    qtest_add_data_func("sn0/qlogic-isp/ram-dma-through-the-direct-window",
                        NULL, test_ram_dma_through_the_direct_window);
    qtest_add_data_func("sn0/qlogic-isp/verify-checksum-answers-both-mailboxes",
                        NULL, test_verify_checksum_answers_both_mailboxes);
    qtest_add_data_func("sn0/qlogic-isp/mbox-get-target-parameters", NULL,
                        test_mbox_get_target_parameters);
    qtest_add_data_func("sn0/qlogic-isp/mbox-set-reset-delay-params", NULL,
                        test_mbox_set_reset_delay_params);
    qtest_add_data_func(
        "sn0/qlogic-isp/mbox-get-target-parameters-ignores-bit-15", NULL,
        test_mbox_get_target_parameters_ignores_bit_15);
    qtest_add_data_func(
        "sn0/qlogic-isp/mbox-recovery-commands-are-not-this-part", NULL,
        test_mbox_recovery_commands_are_not_this_part);
    qtest_add_data_func("sn0/qlogic-isp/mbox-a64-ram-load-is-not-this-part",
                        NULL, test_mbox_a64_ram_load_is_not_this_part);
    qtest_add_data_func("sn0/qlogic-isp/two-luns-behind-one-target", NULL,
                        test_two_luns_behind_one_target);
    qtest_add_data_func("sn0/qlogic-isp/refused-cdb-read-drops-the-request",
                        NULL, test_a_refused_cdb_read_drops_the_request);
    qtest_add_data_func("sn0/qlogic-isp/refused-status-write-no-interrupt",
                        NULL, test_a_refused_status_write_raises_no_interrupt);

    return g_test_run();
}
