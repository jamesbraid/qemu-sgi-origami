/*
 * SGI QLogic ISP12160 dual-channel SCSI adapter: the part's register
 * contract through the real crosstalk-to-PCI path.
 *
 * The adapter is plugged into bridge slot 3, beside the board's own two
 * ISP1040s and its IOC3, so every access here crosses the same byte-lane
 * correction and crosstalk decode a guest driver would.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Crosstalk space, and the bridge's own widget window inside it. */
#define SPACE_XTALK     0x20000000000ULL
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

/*
 * The bridge's registers are 32 bits in the low half of an eight-byte slot,
 * so every register address carries this offset.
 */
#define REG_DATA                4

#define DEV_MEMORY_SPACE        (1u << 12)

/* PCI type 0 configuration space, one 4 KB block per device. */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000

#define PCI_VENDOR_ID           0x00
#define PCI_COMMAND             0x04
#define PCI_COMMAND_IO          0x0001
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_COMMAND_MASTER      0x0004
#define PCI_CLASS_REVISION      0x08
#define PCI_BAR0                0x10
#define PCI_BAR1                0x14
#define PCI_INTERRUPT_LINE      0x3c

/* Class 0x01 mass storage, subclass 0x00 SCSI, programming interface 0. */
#define PCI_CLASS_SCSI          0x010000

/*
 * The board fits its two ISP1040s at devices 0 and 1 and the IOC3 at 2, so
 * the plugged adapter takes slot 3 and the 1 MB window that goes with it.
 */
#define MACHINE  "-M origin200 -m 64 -device qlogic-isp12160,bus=pci,addr=3"
#define SLOT     3

#define WINDOW            (BRIDGE_BASE + 0x700000)
#define CFG               (BRIDGE_BASE + BRIDGE_CFG0 + \
                           SLOT * CFG0_DEVICE_STRIDE)
#define REG_BRIDGE_DEVICE (BRIDGE_BASE + 0x200 + 8 * SLOT + REG_DATA)

/* A window carries PCI address bits 31:20, so both BARs land 20-bit aligned. */
#define ADAPTER_MMIO      0x10700000u
#define ADAPTER_IO        0x00700000u

/* The guest's halfword address: offset ^ (4 - width), low two bits only. */
#define GUEST_HALFWORD(natural)  (((natural) & ~3u) | (((natural) & 3u) ^ 2u))

/* Offsets in the adapter's own window: PCI space, so no plus-four applies. */
#define ISP_REG_ID_LOW          0x00
#define ISP_REG_ID_HIGH         0x02
#define ISP_REG_BUS_CFG0        0x04
#define ISP_REG_BUS_CFG1        0x06
#define ISP_REG_BUS_CTRL        0x08
#define ISP_REG_DMA_CMD_CFG     0x20
#define ISP_REG_DMA_DATA_CFG    0x40
#define ISP_REG_MAILBOX_BASE    0x70
#define ISP_MAILBOX(n)          (ISP_REG_MAILBOX_BASE + (n) * 2)

/* Two offsets inside the banked window at 0x80, far enough apart to differ. */
#define ISP_REG_BANK_BASE       0x80
#define ISP_REG_BANK_MIDDLE     0xa0

#define ISP_BUS_CTRL_SOFT_RESET         0x0001
#define ISP_BUS_CTRL_INT_ENABLE         0x0002
#define ISP_BUS_CTRL_RISC_INT_ENABLE    0x0004

/* Bus configuration 1 bits 9:8 are this family's four-way bank selector. */
#define ISP_BANK_SEL_RISC       0x0000
#define ISP_BANK_SEL_SXP0       0x0100
#define ISP_BANK_SEL_SXP1       0x0200
#define ISP_BANK_SEL_DMA        0x0300

/*
 * The zero selection comes last, so a model that ignores the field ends up
 * with the RISC bank's value everywhere and fails the read-back loop.
 */
static const uint16_t bank_selections[] = {
    ISP_BANK_SEL_SXP0, ISP_BANK_SEL_SXP1, ISP_BANK_SEL_DMA, ISP_BANK_SEL_RISC
};

#define ISP_VENDOR_ID           0x1077
#define ISP12160_DEVICE_ID      0x1216
#define ISP12160_REVISION       0x06

/* The two identity registers as one doubleword. */
#define ISP_ID_PAIR       (((uint32_t)ISP12160_DEVICE_ID << 16) | ISP_VENDOR_ID)

/* 256 bytes of registers: what a BAR written with all ones must report. */
#define ISP_REG_WINDOW_MASK     0xffffff00u

/* The mailbox protocol's own registers and the two host commands it uses. */
#define ISP_REG_BUS_STATUS      0x0a
#define ISP_REG_SEMAPHORE       0x0c
#define ISP_REG_HOST_CMD        0xc0
#define ISP_HOST_CMD_SHIFT          12
#define ISP_HOST_CMD_SET_HOST_INT   0x5
#define ISP_HOST_CMD_CLEAR_RISC_INT 0x7

#define ISP_BUS_STATUS_INT_REQ      0x0002
#define ISP_BUS_STATUS_RISC_INT_REQ 0x0004
#define ISP_BUS_STATUS_INT_BITS     (ISP_BUS_STATUS_INT_REQ | \
                                     ISP_BUS_STATUS_RISC_INT_REQ)
#define ISP_SEMAPHORE_LOCK          0x0001

/* The signature mailboxes 1-4 carry out of reset, and the not-ready code. */
#define ISP_MAILBOX1_SIGNATURE  0x4953
#define ISP_MAILBOX2_SIGNATURE  0x5020
#define ISP_MAILBOX3_SIGNATURE  0x2020
#define ISP_MAILBOX4_SIGNATURE  0x0001
#define ISP_MBOX_NOT_READY      0x0004

#define ISP_MBOX_CMD_EXEC_FIRMWARE          0x0002
#define ISP_MBOX_CMD_WRITE_RAM_WORD         0x0004
#define ISP_MBOX_CMD_READ_RAM_WORD          0x0005
#define ISP_MBOX_CMD_MAILBOX_REG_TEST       0x0006
#define ISP_MBOX_CMD_VERIFY_CHECKSUM        0x0007
#define ISP_MBOX_CMD_ABOUT_FIRMWARE         0x0008
#define ISP_MBOX_CMD_LOAD_RAM_A64           0x0009
#define ISP_MBOX_CMD_GET_FIRMWARE_STATUS    0x001f
#define ISP_MBOX_CMD_GET_TARGET_PARAMETERS  0x0028
#define ISP_MBOX_CMD_SET_RESET_DELAY_PARAMS 0x003a
#define ISP_MBOX_CMD_INIT_REQ_QUEUE_A64     0x0052

#define ISP_MBOX_COMPLETE               0x4000
#define ISP_MBOX_HOST_INTERFACE_ERROR   0x4002
#define ISP_MBOX_TEST_FAILED            0x4003
#define ISP_MBOX_CMD_PARAM_ERROR        0x4006

/* What the part reports about itself: version 10.4.0, 512 requests. */
#define ISP12160_FIRMWARE_MAJOR     10
#define ISP12160_FIRMWARE_MINOR     4
#define ISP12160_FIRMWARE_PATCH     0
#define ISP_FIRMWARE_MAX_REQUESTS   512

/* A value no command answers with, so an untouched output shows up. */
#define NOT_THE_ANSWER          0x5a5a

/*
 * What the adapter's DMA needs: the page-mapped window's first translation
 * entry, and the 64-bit direct form, which names its target widget in the
 * top nibble and passes the low 48 bits through.
 */
#define ATE_BASE                0x010000
#define ATE(n)                  (BRIDGE_BASE + ATE_BASE + (n) * 8)
#define ATE_ENTRY(page, widget) ((uint64_t)(page) | \
                                 ((uint64_t)(widget) << 8) | 1)
#define PCI_TRANSLATED          0x40000000u
#define PAGE_SIZE_4K            0x1000u
#define TARGET_PAGE             0x00100000u
#define DIRECT64(phys)          (0xa500000000000000ULL | (uint64_t)(phys))

/* The machine is given 64 MB, so this is the first address behind nothing. */
#define RAM_TOP_64M             0x04000000u

/* Every access to the adapter goes through the window's byte-lane swizzle. */
static void isp_writew(QTestState *qts, unsigned natural, uint16_t val)
{
    qtest_writew(qts, WINDOW + GUEST_HALFWORD(natural), val);
}

static uint16_t isp_readw(QTestState *qts, unsigned natural)
{
    return qtest_readw(qts, WINDOW + GUEST_HALFWORD(natural));
}

/* The order Linux uses; the other order is exercised where it is the point. */
static void isp_mbox_ack(QTestState *qts)
{
    isp_writew(qts, ISP_REG_SEMAPHORE, 0);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
}

/*
 * A whole submission: inputs into mailboxes 1 upwards, the opcode last,
 * then the host interrupt the protocol triggers on. The answer is read
 * without acknowledging, which is the caller's decision to make.
 */
static uint16_t isp_mbox(QTestState *qts, uint16_t cmd, const uint16_t *in,
                         int nin)
{
    int i;

    for (i = 0; i < nin; i++) {
        isp_writew(qts, ISP_MAILBOX(1 + i), in[i]);
    }
    isp_writew(qts, ISP_MAILBOX(0), cmd);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
    return isp_readw(qts, ISP_MAILBOX(0));
}

static void isp_write_ram_word(QTestState *qts, uint16_t addr, uint16_t val)
{
    const uint16_t in[] = { addr, val };

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_WRITE_RAM_WORD, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);
}

/* Mailbox 2 carries the answer, so it goes in holding something else. */
static uint16_t isp_read_ram_word(QTestState *qts, uint16_t addr)
{
    const uint16_t in[] = { addr, NOT_THE_ANSWER };
    uint16_t val;

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_READ_RAM_WORD, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_COMPLETE);
    val = isp_readw(qts, ISP_MAILBOX(2));
    isp_mbox_ack(qts);
    return val;
}

/*
 * The four address words the reviewed loader supplies, 6 and 7 written even
 * at zero because mailboxes persist and a stale upper half would be
 * reassembled into the address. Mailbox 5 is not an input here; it is
 * written so the array reads as mailboxes 1 through 7 in order.
 */
static uint16_t isp_ram_load_a64(QTestState *qts, uint16_t base, uint64_t addr,
                                 uint16_t count)
{
    const uint16_t in[] = {
        base,
        (uint16_t)(addr >> 16),
        (uint16_t)addr,
        count,
        0,
        (uint16_t)(addr >> 48),
        (uint16_t)(addr >> 32),
    };

    return isp_mbox(qts, ISP_MBOX_CMD_LOAD_RAM_A64, in, ARRAY_SIZE(in));
}

static void put16le(uint8_t *b, uint16_t v)
{
    b[0] = v;
    b[1] = v >> 8;
}

/*
 * Reverse each aligned 32-bit word. The bridge's byte lanes apply exactly
 * this on the way in, so memory holds the transform's inverse and the
 * adapter's DMA sees the stream the test means it to see.
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

/*
 * The command half alone is written back: the status half beside it is
 * read-only or write-one-to-clear, and a read-modify-write of the whole
 * doubleword would hand its latches back to it.
 */
static void enable_command_bits(QTestState *qts, uint32_t bits)
{
    uint32_t cmd = qtest_readl(qts, CFG + PCI_COMMAND) & 0xffffu;

    qtest_writel(qts, CFG + PCI_COMMAND, cmd | bits);
}

static void aim_window_at_memory_bar(QTestState *qts)
{
    qtest_writel(qts, CFG + PCI_BAR1, ADAPTER_MMIO);
    enable_command_bits(qts, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, REG_BRIDGE_DEVICE,
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
}

static void aim_window_at_io_bar(QTestState *qts)
{
    qtest_writel(qts, CFG + PCI_BAR0, ADAPTER_IO);
    enable_command_bits(qts, PCI_COMMAND_IO | PCI_COMMAND_MASTER);
    qtest_writel(qts, REG_BRIDGE_DEVICE, ADAPTER_IO >> 20);
}

/*
 * The part's own identity, and the two apertures it decodes. The BARs are
 * sized rather than assumed: a window of the wrong length puts the register
 * file somewhere a driver's fixed offsets miss, and the space bit is what
 * tells the two apertures apart.
 */
static void test_identity(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    uint32_t bar;

    g_assert_cmphex(qtest_readl(qts, CFG + PCI_VENDOR_ID), ==, ISP_ID_PAIR);
    g_assert_cmphex(qtest_readl(qts, CFG + PCI_CLASS_REVISION) >> 8, ==,
                    PCI_CLASS_SCSI);
    g_assert_cmphex(qtest_readl(qts, CFG + PCI_CLASS_REVISION) & 0xff, ==,
                    ISP12160_REVISION);
    /* The adapter presents one interrupt line, on INTA. */
    g_assert_cmphex((qtest_readl(qts, CFG + PCI_INTERRUPT_LINE) >> 8) & 0xff,
                    ==, 1);

    qtest_writel(qts, CFG + PCI_BAR0, 0xffffffffu);
    bar = qtest_readl(qts, CFG + PCI_BAR0);
    g_assert_cmphex(bar & ~0x3u, ==, ISP_REG_WINDOW_MASK);
    g_assert_cmphex(bar & 0x1u, ==, 1);

    qtest_writel(qts, CFG + PCI_BAR1, 0xffffffffu);
    bar = qtest_readl(qts, CFG + PCI_BAR1);
    g_assert_cmphex(bar & ~0xfu, ==, ISP_REG_WINDOW_MASK);
    g_assert_cmphex(bar & 0x1u, ==, 0);

    qtest_quit(qts);
}

/*
 * The register file is cycled sixteen bits at a time whatever the guest
 * asks for. The two widths either side of that are handled differently and
 * both are pinned, because the declared access size is the only thing
 * standing between a register's side effects and being run twice.
 */
static void test_window_accepts_only_16_bit(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    unsigned b;

    aim_window_at_memory_bar(qts);

    g_assert_cmphex(isp_readw(qts, ISP_REG_ID_LOW), ==, ISP_VENDOR_ID);
    g_assert_cmphex(isp_readw(qts, ISP_REG_ID_HIGH), ==, ISP12160_DEVICE_ID);

    /*
     * A doubleword is split into two halfword cycles rather than reaching
     * the register file as one, so it answers with both identity registers.
     * The discriminating half is what a single wide cycle would answer:
     * the low register alone, which is what an undeclared implementation
     * width produces.
     */
    g_assert_cmphex(qtest_readl(qts, WINDOW + ISP_REG_ID_LOW), ==,
                    ISP_ID_PAIR);

    /*
     * A byte is narrower than the register file decodes, so it is refused
     * outright rather than split: every byte of the identity pair reads
     * zero instead of the byte that lives at that offset.
     */
    for (b = 0; b < 4; b++) {
        g_assert_cmphex(qtest_readb(qts, WINDOW + b), ==, 0);
    }

    qtest_quit(qts);
}

/*
 * One register file behind both apertures. Mailboxes 6 and 7 hold no reset
 * signature, so a value read back through one BAR was put there through the
 * other; both directions run because one can alias correctly alone.
 */
static void test_both_bars_alias_one_register_file(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);

    aim_window_at_io_bar(qts);
    isp_writew(qts, ISP_MAILBOX(7), 0x5a5a);

    aim_window_at_memory_bar(qts);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(7)), ==, 0x5a5a);

    isp_writew(qts, ISP_MAILBOX(6), 0xa5a5);

    aim_window_at_io_bar(qts);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(6)), ==, 0xa5a5);

    qtest_quit(qts);
}

/*
 * Four banks share the window at 0x80, chosen by bus configuration 1 bits
 * 9:8 rather than the ISP1040's single bit 3. Every selection writes two
 * offsets and all eight words come back, so a model that aliases the SCSI
 * processor and DMA banks, or decodes the older field, is rejected.
 */
static void test_three_banks_hold_distinct_words(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    unsigned i;

    aim_window_at_memory_bar(qts);

    for (i = 0; i < ARRAY_SIZE(bank_selections); i++) {
        uint16_t sel = bank_selections[i];

        isp_writew(qts, ISP_REG_BUS_CFG1, sel);
        isp_writew(qts, ISP_REG_BANK_BASE, 0x1000 + sel);
        isp_writew(qts, ISP_REG_BANK_MIDDLE, 0x2000 + sel);
    }

    for (i = 0; i < ARRAY_SIZE(bank_selections); i++) {
        uint16_t sel = bank_selections[i];

        isp_writew(qts, ISP_REG_BUS_CFG1, sel);
        g_assert_cmphex(isp_readw(qts, ISP_REG_BANK_BASE), ==, 0x1000 + sel);
        g_assert_cmphex(isp_readw(qts, ISP_REG_BANK_MIDDLE), ==, 0x2000 + sel);
    }

    qtest_quit(qts);
}

/*
 * The ISP1040 drops everything outside its threshold, burst and bank-select
 * bits. This part's selector sits above that mask, and no source establishes
 * a narrower one, so the register stores a write whole and hands it back.
 */
static void test_cfg1_reads_back_all_bits(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);

    aim_window_at_memory_bar(qts);

    isp_writew(qts, ISP_REG_BUS_CFG1, 0xb2c5);
    g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_CFG1), ==, 0xb2c5);

    isp_writew(qts, ISP_REG_BUS_CFG1, 0x0000);
    g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_CFG1), ==, 0x0000);

    qtest_quit(qts);
}

/*
 * The guest's soft reset puts the window and its selector back to the
 * power-on state. A bank that survived would hand a re-initialising driver
 * the previous run's contents, and every selection is checked because
 * clearing one bank is what a model with a single backing array does.
 */
static void test_soft_reset_clears_banks_and_cfg1(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    unsigned i;

    aim_window_at_memory_bar(qts);

    for (i = 0; i < ARRAY_SIZE(bank_selections); i++) {
        isp_writew(qts, ISP_REG_BUS_CFG1, bank_selections[i]);
        isp_writew(qts, ISP_REG_BANK_BASE, 0x1000 + bank_selections[i]);
    }

    isp_writew(qts, ISP_REG_BUS_CTRL, ISP_BUS_CTRL_SOFT_RESET);
    g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_CFG1), ==, 0);

    for (i = 0; i < ARRAY_SIZE(bank_selections); i++) {
        isp_writew(qts, ISP_REG_BUS_CFG1, bank_selections[i]);
        g_assert_cmphex(isp_readw(qts, ISP_REG_BANK_BASE), ==, 0);
    }

    qtest_quit(qts);
}

/*
 * The ISP1040's two flat DMA blocks are not in this part's map; the family
 * moved that state into the DMA bank. Storing a write at the old offsets
 * would hand a driver back a configuration the adapter never applied.
 */
static void test_flat_dma_registers_are_unimplemented(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);

    aim_window_at_memory_bar(qts);

    isp_writew(qts, ISP_REG_DMA_CMD_CFG, 0xffff);
    isp_writew(qts, ISP_REG_DMA_DATA_CFG, 0xffff);

    g_assert_cmphex(isp_readw(qts, ISP_REG_DMA_CMD_CFG), ==, 0);
    g_assert_cmphex(isp_readw(qts, ISP_REG_DMA_DATA_CFG), ==, 0);

    qtest_quit(qts);
}

/*
 * Nothing reads a revision from bus configuration 0 on this part and the
 * contract assigns it none, so it reads zero rather than repeating the
 * ISP1040's revision field.
 */
static void test_bus_cfg0_reads_zero(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);

    aim_window_at_memory_bar(qts);

    g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_CFG0), ==, 0);

    qtest_quit(qts);
}

/*
 * Bit 0 requests the soft reset and the adapter clears it: that is the bit
 * both drivers poll to learn the reset finished. The reset lands on the
 * whole register, so enables written in the same word do not survive it
 * either, which is how the ISP1040 already behaves. Both drivers enable
 * interrupts in a later write, and that write is ordinary storage.
 */
static void test_interface_control_reset_self_clears(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);

    aim_window_at_memory_bar(qts);

    isp_writew(qts, ISP_REG_BUS_CTRL,
               ISP_BUS_CTRL_SOFT_RESET | ISP_BUS_CTRL_INT_ENABLE |
               ISP_BUS_CTRL_RISC_INT_ENABLE);
    g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_CTRL), ==, 0);

    isp_writew(qts, ISP_REG_BUS_CTRL,
               ISP_BUS_CTRL_INT_ENABLE | ISP_BUS_CTRL_RISC_INT_ENABLE);
    g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_CTRL), ==,
                    ISP_BUS_CTRL_INT_ENABLE | ISP_BUS_CTRL_RISC_INT_ENABLE);

    qtest_quit(qts);
}

/*
 * The product signature both drivers compare against before they will
 * attach, and mailbox 0 out of the busy state that gates every command.
 */
static void test_reset_signature(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);

    aim_window_at_memory_bar(qts);

    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(1)), ==, ISP_MAILBOX1_SIGNATURE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, ISP_MAILBOX2_SIGNATURE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(3)), ==, ISP_MAILBOX3_SIGNATURE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(4)), ==, ISP_MAILBOX4_SIGNATURE);

    /* The initialisation both drivers run waits for this to go away. */
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), !=, ISP_MBOX_NOT_READY);

    qtest_quit(qts);
}

/*
 * A different pattern in each tested mailbox: one backing word, aliased
 * storage or a constant answer all show up, and the register test is what
 * the drivers use to decide the adapter is present at all.
 */
static void test_mailbox_test_returns_distinct_patterns(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    static const uint16_t pattern[] = {
        0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777
    };
    unsigned i;

    aim_window_at_memory_bar(qts);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_MAILBOX_REG_TEST, pattern,
                             ARRAY_SIZE(pattern)), ==, ISP_MBOX_COMPLETE);

    for (i = 0; i < ARRAY_SIZE(pattern); i++) {
        g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(1 + i)), ==, pattern[i]);
    }
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/*
 * Both words written before either is read, at addresses far enough apart
 * that byte addressing lands elsewhere; the second is near the end of the
 * array, where a smaller allocation folds it onto the first.
 */
static void test_ram_word_round_trip_two_addresses(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    const uint16_t low[] = { 0x0010, NOT_THE_ANSWER, NOT_THE_ANSWER };
    const uint16_t high[] = { 0xfff0, NOT_THE_ANSWER, NOT_THE_ANSWER };

    aim_window_at_memory_bar(qts);

    isp_write_ram_word(qts, 0x0010, 0xbeef);
    isp_write_ram_word(qts, 0xfff0, 0xcafe);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_READ_RAM_WORD, low,
                             ARRAY_SIZE(low)), ==, ISP_MBOX_COMPLETE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, 0xbeef);
    /* Mailbox 3 is an output of this command too, and it answers zero. */
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(3)), ==, 0);
    isp_mbox_ack(qts);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_READ_RAM_WORD, high,
                             ARRAY_SIZE(high)), ==, ISP_MBOX_COMPLETE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, 0xcafe);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(3)), ==, 0);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/* Eight words with sixteen distinct bytes, so no swap of any pair hides. */
#define A64_LOAD_BASE       0x1000
#define A64_SENTINEL        0x7e7e

static const uint16_t a64_image[] = {
    0x1234, 0xabcd, 0x5678, 0x9aef, 0x0d71, 0xb3c5, 0x2f68, 0x4a97
};

/*
 * The loader the reviewed driver uses, through the page-mapped window: the
 * transfer is a little-endian word stream at the adapter's DMA interface,
 * so bytes 34 12 cd ab are words 0x1234 and 0xabcd. Sentinels sit either
 * side of the span, and the translation entry is what makes an untranslated
 * transfer land somewhere with nothing in it.
 */
static void test_a64_ram_load_through_translated_dma(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    uint8_t page[ARRAY_SIZE(a64_image) * 2];
    unsigned i;

    aim_window_at_memory_bar(qts);
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE, 0));
    qtest_memset(qts, TARGET_PAGE, 0, PAGE_SIZE_4K);

    isp_write_ram_word(qts, A64_LOAD_BASE - 1, A64_SENTINEL);
    isp_write_ram_word(qts, A64_LOAD_BASE + ARRAY_SIZE(a64_image),
                       A64_SENTINEL);

    for (i = 0; i < ARRAY_SIZE(a64_image); i++) {
        put16le(page + i * 2, a64_image[i]);
    }
    normalise_words(page, sizeof(page));
    qtest_memwrite(qts, TARGET_PAGE, page, sizeof(page));

    g_assert_cmphex(isp_ram_load_a64(qts, A64_LOAD_BASE, PCI_TRANSLATED,
                                     ARRAY_SIZE(a64_image)), ==,
                    ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    for (i = 0; i < ARRAY_SIZE(a64_image); i++) {
        g_assert_cmphex(isp_read_ram_word(qts, A64_LOAD_BASE + i), ==,
                        a64_image[i]);
    }

    g_assert_cmphex(isp_read_ram_word(qts, A64_LOAD_BASE - 1), ==,
                    A64_SENTINEL);
    g_assert_cmphex(isp_read_ram_word(qts,
                                      A64_LOAD_BASE + ARRAY_SIZE(a64_image)),
                    ==, A64_SENTINEL);

    qtest_quit(qts);
}

/*
 * A span running past the last word is refused outright. The source is
 * readable memory, so a refused transfer cannot be the reason, and the
 * sentinels are the first and last words a wrapping copy would reach.
 */
static void test_a64_ram_load_rejects_end_crossing(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);

    aim_window_at_memory_bar(qts);

    isp_write_ram_word(qts, 0x0000, A64_SENTINEL);
    isp_write_ram_word(qts, 0xfff0, A64_SENTINEL);

    g_assert_cmphex(isp_ram_load_a64(qts, 0xfff0, DIRECT64(TARGET_PAGE),
                                     0x0020), ==, ISP_MBOX_CMD_PARAM_ERROR);
    isp_mbox_ack(qts);

    g_assert_cmphex(isp_read_ram_word(qts, 0x0000), ==, A64_SENTINEL);
    g_assert_cmphex(isp_read_ram_word(qts, 0xfff0), ==, A64_SENTINEL);

    qtest_quit(qts);
}

/*
 * A transfer the fabric refuses leaves the array as it was: no prefix, no
 * zero fill. Reporting the words loaded when none were would hand a guest
 * a corrupt image that its own checksum then blames on the download.
 */
static void test_a64_ram_load_failed_dma_commits_nothing(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    const uint16_t base = 0x2000;
    const unsigned count = 4;
    unsigned i;

    aim_window_at_memory_bar(qts);

    for (i = 0; i < count; i++) {
        isp_write_ram_word(qts, base + i, A64_SENTINEL);
    }

    g_assert_cmphex(isp_ram_load_a64(qts, base, DIRECT64(RAM_TOP_64M), count),
                    ==, ISP_MBOX_HOST_INTERFACE_ERROR);
    isp_mbox_ack(qts);

    for (i = 0; i < count; i++) {
        g_assert_cmphex(isp_read_ram_word(qts, base + i), ==, A64_SENTINEL);
    }

    qtest_quit(qts);
}

/*
 * The image's fourth word is its length, and a whole image sums to zero.
 * Both halves run: a model that always fails and a model that always
 * completes each pass one of them alone.
 */
#define CHECKSUM_BASE   0x3000

static void test_checksum_passes_then_one_corrupt_word_fails(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    static const uint16_t image[] = {
        0x0100, 0x0200, 0x0300, 0x0008, 0x1111, 0x2222, 0x3333
    };
    const uint16_t in[] = { CHECKSUM_BASE, NOT_THE_ANSWER };
    uint16_t sum = 0;
    unsigned i;

    aim_window_at_memory_bar(qts);

    for (i = 0; i < ARRAY_SIZE(image); i++) {
        sum += image[i];
        isp_write_ram_word(qts, CHECKSUM_BASE + i, image[i]);
    }
    /* The eighth word is whatever brings the sum to zero. */
    isp_write_ram_word(qts, CHECKSUM_BASE + ARRAY_SIZE(image),
                       (uint16_t)(0 - sum));

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_VERIFY_CHECKSUM, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_COMPLETE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(1)), ==, 0);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, 0);
    isp_mbox_ack(qts);

    /* One word, one bit: the answer is the difference, in both mailboxes. */
    isp_write_ram_word(qts, CHECKSUM_BASE + 5, 0x2223);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_VERIFY_CHECKSUM, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_TEST_FAILED);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(1)), ==, 0x0001);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, 0x0001);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/*
 * Execute is acknowledged without anything being interpreted, and the
 * modelled interface stays live afterwards: a model that treated execute as
 * handing the adapter over to a downloaded program would answer these two
 * commands differently, or not at all.
 */
static void test_execute_then_stateful_commands(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    const uint16_t start[] = { CHECKSUM_BASE };
    static const uint16_t queue[] = {
        8,                                              /* entries */
        (uint16_t)(DIRECT64(TARGET_PAGE) >> 16),
        (uint16_t)DIRECT64(TARGET_PAGE),
        0, 0,                                           /* initial indices */
        (uint16_t)(DIRECT64(TARGET_PAGE) >> 48),
        (uint16_t)(DIRECT64(TARGET_PAGE) >> 32),
    };

    aim_window_at_memory_bar(qts);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_EXEC_FIRMWARE, start,
                             ARRAY_SIZE(start)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    isp_write_ram_word(qts, 0x4321, 0xfeed);
    g_assert_cmphex(isp_read_ram_word(qts, 0x4321), ==, 0xfeed);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, queue,
                             ARRAY_SIZE(queue)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/*
 * The version a guest prints and the command budget it sizes its queues
 * from. Mailbox 6 is seeded, because the attribute word is zero and an
 * untouched output would read zero as well.
 */
static void test_about_firmware_and_status(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);

    aim_window_at_memory_bar(qts);

    isp_writew(qts, ISP_MAILBOX(6), NOT_THE_ANSWER);
    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_ABOUT_FIRMWARE, NULL, 0), ==,
                    ISP_MBOX_COMPLETE);
    g_assert_cmpuint(isp_readw(qts, ISP_MAILBOX(1)), ==,
                     ISP12160_FIRMWARE_MAJOR);
    g_assert_cmpuint(isp_readw(qts, ISP_MAILBOX(2)), ==,
                     ISP12160_FIRMWARE_MINOR);
    g_assert_cmpuint(isp_readw(qts, ISP_MAILBOX(3)), ==,
                     ISP12160_FIRMWARE_PATCH);
    g_assert_cmpuint(isp_readw(qts, ISP_MAILBOX(6)), ==, 0);
    isp_mbox_ack(qts);

    /* The version words are still in place, so both answers are the reply. */
    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_GET_FIRMWARE_STATUS, NULL, 0),
                    ==, ISP_MBOX_COMPLETE);
    g_assert_cmpuint(isp_readw(qts, ISP_MAILBOX(1)), ==, 0);
    g_assert_cmpuint(isp_readw(qts, ISP_MAILBOX(2)), ==,
                     ISP_FIRMWARE_MAX_REQUESTS);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/*
 * The two-channel settings a guest programs during attach. The reset delay
 * carries a value per channel, and the target-parameter reply is asked for
 * on both channels: answering invalid command for either one is what left
 * a driver logging a mailbox failure per target.
 */
static void test_reset_delay_accepted_for_both_channels(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    static const uint16_t delays[] = { 3, 5 };
    static const uint16_t selector[] = { 0x8100, 0x0100 };
    unsigned i;

    aim_window_at_memory_bar(qts);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_SET_RESET_DELAY_PARAMS, delays,
                             ARRAY_SIZE(delays)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    /* Channel 1 target 1, then channel 0 target 1: outputs seeded first. */
    for (i = 0; i < ARRAY_SIZE(selector); i++) {
        const uint16_t in[] = { selector[i], NOT_THE_ANSWER, NOT_THE_ANSWER };

        isp_writew(qts, ISP_MAILBOX(6), NOT_THE_ANSWER);
        g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_GET_TARGET_PARAMETERS, in,
                                 ARRAY_SIZE(in)), ==, ISP_MBOX_COMPLETE);
        g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==, 0);
        g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(3)), ==, 0);
        g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(6)), ==, 0);
        isp_mbox_ack(qts);
    }

    qtest_quit(qts);
}

/*
 * Delivery is disabled around the whole command, the way the reviewed
 * driver runs it, and the event still has to be there afterwards: masking
 * is not acknowledgement, reading is not acknowledgement, and the two
 * drivers acknowledge in opposite orders. One pass per order.
 */
static void test_masked_completion_survives_and_both_ack_orders_clear(
    const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    static const uint16_t pattern[] = { 0x1357, 0x2468 };
    unsigned pass, i;

    aim_window_at_memory_bar(qts);

    for (pass = 0; pass < ARRAY_SIZE(pattern); pass++) {
        isp_writew(qts, ISP_REG_BUS_CTRL, 0);

        g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_MAILBOX_REG_TEST,
                                 &pattern[pass], 1), ==, ISP_MBOX_COMPLETE);

        /* Twice: a read that acknowledged would come up empty the second. */
        for (i = 0; i < 2; i++) {
            g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_STATUS) &
                            ISP_BUS_STATUS_INT_BITS, ==,
                            ISP_BUS_STATUS_INT_BITS);
            g_assert_cmphex(isp_readw(qts, ISP_REG_SEMAPHORE) &
                            ISP_SEMAPHORE_LOCK, ==, ISP_SEMAPHORE_LOCK);
            g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(1)), ==,
                            pattern[pass]);
        }

        /* Restoring delivery does not consume the event either. */
        isp_writew(qts, ISP_REG_BUS_CTRL,
                   ISP_BUS_CTRL_INT_ENABLE | ISP_BUS_CTRL_RISC_INT_ENABLE);
        g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_STATUS) &
                        ISP_BUS_STATUS_INT_BITS, ==, ISP_BUS_STATUS_INT_BITS);
        g_assert_cmphex(isp_readw(qts, ISP_REG_SEMAPHORE) &
                        ISP_SEMAPHORE_LOCK, ==, ISP_SEMAPHORE_LOCK);

        if (pass == 0) {
            isp_writew(qts, ISP_REG_SEMAPHORE, 0);
            isp_writew(qts, ISP_REG_HOST_CMD,
                       ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
        } else {
            isp_writew(qts, ISP_REG_HOST_CMD,
                       ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
            isp_writew(qts, ISP_REG_SEMAPHORE, 0);
        }

        g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_STATUS) &
                        ISP_BUS_STATUS_INT_BITS, ==, 0);
        g_assert_cmphex(isp_readw(qts, ISP_REG_SEMAPHORE), ==, 0);
    }

    qtest_quit(qts);
}

/*
 * A second command submitted before the first is acknowledged is refused
 * whole: overwriting the standing result would lose the answer the guest
 * has not collected yet, and the opcode is left in mailbox 0 to show that
 * nothing ran.
 */
static void test_submission_while_locked_is_ignored(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    static const uint16_t first[] = { 0x1111 };

    aim_window_at_memory_bar(qts);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_MAILBOX_REG_TEST, first,
                             ARRAY_SIZE(first)), ==, ISP_MBOX_COMPLETE);

    isp_writew(qts, ISP_MAILBOX(1), 0x9999);
    isp_writew(qts, ISP_MAILBOX(0), ISP_MBOX_CMD_MAILBOX_REG_TEST);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_CMD_MAILBOX_REG_TEST);
    g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_STATUS) &
                    ISP_BUS_STATUS_INT_BITS, ==, ISP_BUS_STATUS_INT_BITS);
    g_assert_cmphex(isp_readw(qts, ISP_REG_SEMAPHORE) & ISP_SEMAPHORE_LOCK,
                    ==, ISP_SEMAPHORE_LOCK);

    /* Acknowledged, the same submission runs. */
    isp_mbox_ack(qts);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(1)), ==, 0x9999);
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/isp12160/identity", NULL, test_identity);
    qtest_add_data_func("sn0/isp12160/window-accepts-only-16-bit", NULL,
                        test_window_accepts_only_16_bit);
    qtest_add_data_func("sn0/isp12160/both-bars-alias-one-register-file",
                        NULL, test_both_bars_alias_one_register_file);
    qtest_add_data_func("sn0/isp12160/three-banks-hold-distinct-words", NULL,
                        test_three_banks_hold_distinct_words);
    qtest_add_data_func("sn0/isp12160/cfg1-reads-back-all-bits", NULL,
                        test_cfg1_reads_back_all_bits);
    qtest_add_data_func("sn0/isp12160/soft-reset-clears-banks-and-cfg1", NULL,
                        test_soft_reset_clears_banks_and_cfg1);
    qtest_add_data_func("sn0/isp12160/flat-dma-registers-are-unimplemented",
                        NULL, test_flat_dma_registers_are_unimplemented);
    qtest_add_data_func("sn0/isp12160/bus-cfg0-reads-zero", NULL,
                        test_bus_cfg0_reads_zero);
    qtest_add_data_func("sn0/isp12160/interface-control-reset-self-clears",
                        NULL, test_interface_control_reset_self_clears);
    qtest_add_data_func("sn0/isp12160/reset-signature", NULL,
                        test_reset_signature);
    qtest_add_data_func("sn0/isp12160/mailbox-test-returns-distinct-patterns",
                        NULL, test_mailbox_test_returns_distinct_patterns);
    qtest_add_data_func("sn0/isp12160/ram-word-round-trip-two-addresses",
                        NULL, test_ram_word_round_trip_two_addresses);
    qtest_add_data_func("sn0/isp12160/a64-ram-load-through-translated-dma",
                        NULL, test_a64_ram_load_through_translated_dma);
    qtest_add_data_func("sn0/isp12160/a64-ram-load-rejects-end-crossing",
                        NULL, test_a64_ram_load_rejects_end_crossing);
    qtest_add_data_func("sn0/isp12160/a64-ram-load-failed-dma-commits-nothing",
                        NULL, test_a64_ram_load_failed_dma_commits_nothing);
    qtest_add_data_func(
        "sn0/isp12160/checksum-passes-then-one-corrupt-word-fails", NULL,
        test_checksum_passes_then_one_corrupt_word_fails);
    qtest_add_data_func("sn0/isp12160/execute-then-stateful-commands", NULL,
                        test_execute_then_stateful_commands);
    qtest_add_data_func("sn0/isp12160/about-firmware-and-status", NULL,
                        test_about_firmware_and_status);
    qtest_add_data_func("sn0/isp12160/reset-delay-accepted-for-both-channels",
                        NULL, test_reset_delay_accepted_for_both_channels);
    qtest_add_data_func(
        "sn0/isp12160/masked-completion-survives-and-both-ack-orders-clear",
        NULL, test_masked_completion_survives_and_both_ack_orders_clear);
    qtest_add_data_func("sn0/isp12160/submission-while-locked-is-ignored",
                        NULL, test_submission_while_locked_is_ignored);

    return g_test_run();
}
