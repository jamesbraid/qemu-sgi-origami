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
#include "qobject/qdict.h"

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

/* One past the last word of the array, which is where a span may end. */
#define ISP_RAM_WORDS       0x10000

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
 * The other side of the same edge: a span whose last word is the array's
 * last word is a legitimate load, and the comparison that refuses the
 * span above must not refuse this one. Reading the final word back is what
 * separates an accepted request from a copy that stopped a word short.
 */
static void test_a64_ram_load_reaches_the_last_word(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    const uint16_t base = ISP_RAM_WORDS - ARRAY_SIZE(a64_image);
    uint8_t page[ARRAY_SIZE(a64_image) * 2];
    unsigned i;

    aim_window_at_memory_bar(qts);
    qtest_memset(qts, TARGET_PAGE, 0, PAGE_SIZE_4K);

    for (i = 0; i < ARRAY_SIZE(a64_image); i++) {
        put16le(page + i * 2, a64_image[i]);
    }
    normalise_words(page, sizeof(page));
    qtest_memwrite(qts, TARGET_PAGE, page, sizeof(page));

    g_assert_cmphex(isp_ram_load_a64(qts, base, DIRECT64(TARGET_PAGE),
                                     ARRAY_SIZE(a64_image)), ==,
                    ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    for (i = 0; i < ARRAY_SIZE(a64_image); i++) {
        g_assert_cmphex(isp_read_ram_word(qts, base + i), ==, a64_image[i]);
    }

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

/*
 * Rings and two-channel transfers. The adapter owns one request ring and
 * one response ring; the channel bit in each entry's device word is what
 * sends a command to one SCSI bus or the other.
 */

#define ISP_MBOX_CMD_INIT_RSP_QUEUE_A64     0x0053

/* Mailboxes 4 and 5 are the two ring cursors, and each has two roles. */
#define ISP_REG_REQUEST_QUEUE   ISP_MAILBOX(4)
#define ISP_REG_RESPONSE_QUEUE  ISP_MAILBOX(5)

#define ISP_QUEUE_ENTRY_SIZE        64
#define ISP_ENTRY_OFF_TYPE          0
#define ISP_ENTRY_OFF_COUNT         1
#define ISP_ENTRY_TYPE_STATUS       0x03
#define ISP_ENTRY_TYPE_MARKER       0x04
#define ISP_A64_ENTRY_TYPE_COMMAND  0x09
#define ISP_A64_ENTRY_TYPE_CONT     0x0a

#define ISP_REQ_OFF_HANDLE          4
#define ISP_REQ_OFF_DEVICE          8
#define ISP_REQ_OFF_CDBLEN          10
#define ISP_REQ_OFF_CONTROL         12
#define ISP_REQ_OFF_SEGCOUNT        18
#define ISP_REQ_OFF_CDB             20
#define ISP_REQ_CDB_LEN             12
#define ISP_A64_REQ_OFF_SEGS        40
#define ISP_A64_REQ_SEGS_INLINE     2
#define ISP_A64_CONT_OFF_SEGS       4
#define ISP_A64_CONT_SEGS           5
#define ISP_A64_SEG_SIZE            12

#define ISP_STATUS_OFF_HANDLE       4
#define ISP_STATUS_OFF_SCSI_STATUS  8
#define ISP_STATUS_OFF_COMPLETION   10
#define ISP_STATUS_OFF_STATE_FLAGS  12
#define ISP_STATUS_OFF_STATUS_FLAGS 14
#define ISP_STATUS_OFF_SENSE_LEN    18
#define ISP_STATUS_OFF_RESID        20
#define ISP_STATUS_OFF_SENSE_DATA   32
#define ISP_STATUS_SENSE_MAX        32

#define ISP_IOCB_COMPLETE           0x0000
#define ISP_IOCB_INCOMPLETE         0x0001
#define ISP_IOCB_DATA_UNDERRUN      0x0015

/* One bit per SCSI phase the command reached. */
#define ISP_STATE_FLAG_GOT_BUS      0x0100
#define ISP_STATE_FLAG_GOT_STATUS   0x1000
#define ISP_STATE_FLAG_GOT_SENSE    0x2000

/* CHECK CONDITION, with the bit that says the entry carries sense. */
#define ISP_SCSI_STATUS_CHECK_CONDITION 0x0202

/* Data from the target into host memory, and host memory to the target. */
#define ISP_REQ_CONTROL_DATA_IN     0x0020
#define ISP_REQ_CONTROL_DATA_OUT    0x0040

/* Bit 15 is the channel; the target and LUN below it are local to it. */
#define ISP_DEVICE_WORD(channel, target, lun) \
    ((uint16_t)(((channel) << 15) | ((target) << 8) | (lun)))

#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_INQUIRY            0x12
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2a

/* Both disks sit at the same target number, one on each channel. */
#define DISK_TARGET             1
#define EMPTY_TARGET            5
/* A second empty target, so the answer is not one target number's. */
#define ABSENT_TARGET           6

#define SECTOR_BYTES            512
#define DISK_BYTES              (1024 * 1024)
#define DISK0_SECTOR0_FILL      0x41
#define DISK1_SECTOR0_FILL      0x42

/* Ring and buffer homes, in pages of guest memory nothing else uses. */
#define RQST_PHYS               0x00200000u
#define RESP_PHYS               0x00300040u
#define BUF_PHYS                0x00400000u
#define RQST_SLOTS              4
#define RESP_SLOTS              8

static void put32le(uint8_t *b, uint32_t v)
{
    b[0] = v;
    b[1] = v >> 8;
    b[2] = v >> 16;
    b[3] = v >> 24;
}

static uint16_t get16le(const uint8_t *b)
{
    return b[0] | ((uint16_t)b[1] << 8);
}

static uint32_t get32le(const uint8_t *b)
{
    return b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

/*
 * Two raw files, one per channel, in a directory of their own. The
 * channel-0 disk's second sector carries a pattern with no repeated
 * neighbours, so a transfer split across descriptors cannot come back
 * looking right by accident.
 */
typedef struct DiskPair {
    char *dir;
    char *path[2];
} DiskPair;

static uint8_t disk0_sector1_byte(unsigned i)
{
    return (uint8_t)(i * 7 + 1);
}

static void disks_create(DiskPair *disks)
{
    g_autofree uint8_t *image = g_malloc0(DISK_BYTES);
    GError *err = NULL;
    unsigned i;

    disks->dir = g_dir_make_tmp("isp12160-XXXXXX", &err);
    g_assert_no_error(err);

    memset(image, DISK0_SECTOR0_FILL, SECTOR_BYTES);
    for (i = 0; i < SECTOR_BYTES; i++) {
        image[SECTOR_BYTES + i] = disk0_sector1_byte(i);
    }
    disks->path[0] = g_build_filename(disks->dir, "disk0.raw", NULL);
    g_assert_true(g_file_set_contents(disks->path[0], (const char *)image,
                                      DISK_BYTES, &err));
    g_assert_no_error(err);

    memset(image, 0, DISK_BYTES);
    memset(image, DISK1_SECTOR0_FILL, SECTOR_BYTES);
    disks->path[1] = g_build_filename(disks->dir, "disk1.raw", NULL);
    g_assert_true(g_file_set_contents(disks->path[1], (const char *)image,
                                      DISK_BYTES, &err));
    g_assert_no_error(err);
}

static void disks_remove(DiskPair *disks)
{
    unsigned i;

    for (i = 0; i < 2; i++) {
        g_assert_cmpint(unlink(disks->path[i]), ==, 0);
        g_free(disks->path[i]);
    }
    g_assert_cmpint(rmdir(disks->dir), ==, 0);
    g_free(disks->dir);
}

/*
 * The adapter's channel 0 is bus scsi.2 and channel 1 is scsi.3: the
 * board's own two ISP1040s took scsi.0 and scsi.1 before this one was
 * plugged. A wrong bus name is refused at start-up rather than silently
 * putting both disks on one channel.
 */
static char *isp12160_disk_args(const DiskPair *disks)
{
    return g_strdup_printf(
        MACHINE
        " -drive if=none,id=d0,format=raw,file=%s"
        " -device scsi-hd,bus=scsi.2,scsi-id=1,drive=d0"
        " -drive if=none,id=d1,format=raw,file=%s"
        " -device scsi-hd,bus=scsi.3,scsi-id=1,drive=d1",
        disks->path[0], disks->path[1]);
}

static QTestState *isp12160_start_with_disks(DiskPair *disks)
{
    QTestState *qts;
    g_autofree char *args = NULL;

    disks_create(disks);
    args = isp12160_disk_args(disks);

    qts = qtest_init(args);
    aim_window_at_memory_bar(qts);
    return qts;
}

/*
 * The ring geometry the helpers below work against. One test runs at a
 * time in this binary and every one of them calls init_queues() first, so
 * this belongs to the file rather than being threaded through each call.
 */
static struct {
    uint64_t rqst_iova;
    uint64_t resp_iova;
    uint16_t rqst_len;
    uint16_t resp_len;
    uint16_t rqst_next;    /* The slot the test will write next. */
    uint16_t resp_next;    /* The slot the test will read next. */
} ring;

/* A direct 64-bit transfer passes the low bits through unchanged. */
static uint64_t ring_phys(uint64_t iova)
{
    return iova & 0xffffffffu;
}

/* A ring lives in guest memory, so the bridge's word reversal applies. */
static void write_ring_entry(QTestState *qts, unsigned slot,
                             const uint8_t *entry)
{
    uint8_t buf[ISP_QUEUE_ENTRY_SIZE];

    memcpy(buf, entry, sizeof(buf));
    normalise_words(buf, sizeof(buf));
    qtest_memwrite(qts, ring_phys(ring.rqst_iova) + slot * ISP_QUEUE_ENTRY_SIZE,
                   buf, sizeof(buf));
}

/*
 * One queue initialisation. Mailbox 6 carries address bits 63:48 and
 * mailbox 7 bits 47:32, so a model that drops either upper word aims the
 * ring somewhere else. Mailboxes 4 and 5 carry the initial indices.
 */
static void init_one_queue(QTestState *qts, uint16_t cmd, uint16_t count,
                           uint64_t iova)
{
    const uint16_t in[] = {
        count,
        (uint16_t)(iova >> 16),
        (uint16_t)iova,
        0,
        0,
        (uint16_t)(iova >> 48),
        (uint16_t)(iova >> 32),
    };

    g_assert_cmphex(isp_mbox(qts, cmd, in, ARRAY_SIZE(in)), ==,
                    ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);
}

static void init_queues(QTestState *qts, uint64_t rqst_iova, uint16_t rqst_len,
                        uint64_t resp_iova, uint16_t resp_len,
                        bool request_first)
{
    if (request_first) {
        init_one_queue(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, rqst_len,
                       rqst_iova);
        init_one_queue(qts, ISP_MBOX_CMD_INIT_RSP_QUEUE_A64, resp_len,
                       resp_iova);
    } else {
        init_one_queue(qts, ISP_MBOX_CMD_INIT_RSP_QUEUE_A64, resp_len,
                       resp_iova);
        init_one_queue(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, rqst_len,
                       rqst_iova);
    }

    ring.rqst_iova = rqst_iova;
    ring.resp_iova = resp_iova;
    ring.rqst_len = rqst_len;
    ring.resp_len = resp_len;
    ring.rqst_next = 0;
    ring.resp_next = 0;
}

typedef struct RingSeg {
    uint64_t iova;
    uint32_t len;
} RingSeg;

/*
 * One A64 command written into the named slot, without publishing it.
 * Segments past the two the command entry holds spill into continuation
 * entries in the slots that follow, wrapping to slot zero the way a real
 * ring does.
 */
static void place_a64(QTestState *qts, uint16_t slot, uint32_t handle,
                      unsigned channel, unsigned target, unsigned lun,
                      const uint8_t *cdb, unsigned cdb_len, uint16_t control,
                      const RingSeg *segs, unsigned nsegs)
{
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    unsigned placed = 0;
    unsigned count = 1;
    unsigned index = slot;
    unsigned i;

    g_assert_cmpuint(cdb_len, <=, ISP_REQ_CDB_LEN);
    if (nsegs > ISP_A64_REQ_SEGS_INLINE) {
        count += (nsegs - ISP_A64_REQ_SEGS_INLINE + ISP_A64_CONT_SEGS - 1)
                 / ISP_A64_CONT_SEGS;
    }

    memset(entry, 0, sizeof(entry));
    entry[ISP_ENTRY_OFF_TYPE] = ISP_A64_ENTRY_TYPE_COMMAND;
    entry[ISP_ENTRY_OFF_COUNT] = count;
    put32le(entry + ISP_REQ_OFF_HANDLE, handle);
    put16le(entry + ISP_REQ_OFF_DEVICE, ISP_DEVICE_WORD(channel, target, lun));
    put16le(entry + ISP_REQ_OFF_CDBLEN, cdb_len);
    put16le(entry + ISP_REQ_OFF_CONTROL, control);
    put16le(entry + ISP_REQ_OFF_SEGCOUNT, nsegs);
    memcpy(entry + ISP_REQ_OFF_CDB, cdb, cdb_len);
    for (i = 0; i < ISP_A64_REQ_SEGS_INLINE && placed < nsegs; i++, placed++) {
        uint8_t *d = entry + ISP_A64_REQ_OFF_SEGS + i * ISP_A64_SEG_SIZE;

        put32le(d, (uint32_t)segs[placed].iova);
        put32le(d + 4, (uint32_t)(segs[placed].iova >> 32));
        put32le(d + 8, segs[placed].len);
    }
    write_ring_entry(qts, index, entry);

    while (placed < nsegs) {
        index = (index + 1) % ring.rqst_len;
        memset(entry, 0, sizeof(entry));
        entry[ISP_ENTRY_OFF_TYPE] = ISP_A64_ENTRY_TYPE_CONT;
        entry[ISP_ENTRY_OFF_COUNT] = 1;
        for (i = 0; i < ISP_A64_CONT_SEGS && placed < nsegs; i++, placed++) {
            uint8_t *d = entry + ISP_A64_CONT_OFF_SEGS + i * ISP_A64_SEG_SIZE;

            put32le(d, (uint32_t)segs[placed].iova);
            put32le(d + 4, (uint32_t)(segs[placed].iova >> 32));
            put32le(d + 8, segs[placed].len);
        }
        write_ring_entry(qts, index, entry);
    }

    ring.rqst_next = (slot + count) % ring.rqst_len;
}

/* The entries are all in place; this write is what runs them. */
static void publish_requests(QTestState *qts)
{
    isp_writew(qts, ISP_REG_REQUEST_QUEUE, ring.rqst_next);
}

/* One command placed and published, which is what most tests want. */
static void submit_a64(QTestState *qts, uint16_t slot, uint32_t handle,
                       unsigned channel, unsigned target, unsigned lun,
                       const uint8_t *cdb, unsigned cdb_len, uint16_t control,
                       const RingSeg *segs, unsigned nsegs)
{
    place_a64(qts, slot, handle, channel, target, lun, cdb, cdb_len, control,
              segs, nsegs);
    publish_requests(qts);
}

#define STATUS_STEP_NS      (1000 * 1000)
#define STATUS_WAIT_US      (10 * 1000 * 1000)

/*
 * The next completion in the ring, whichever handle it carries. Reading
 * mailbox 5 answers the adapter's response producer index, so the entry
 * the test has not read yet is ready once that index moves; writing
 * mailbox 5 back releases the slot. The wait is bounded in wall time
 * because a read held by a block filter is waiting on that filter's own
 * timer rather than on anything the model or the guest clock does.
 */
static uint32_t wait_next_status(QTestState *qts, uint8_t *status)
{
    gint64 deadline = g_get_monotonic_time() + STATUS_WAIT_US;

    while (isp_readw(qts, ISP_REG_RESPONSE_QUEUE) == ring.resp_next) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        qtest_clock_step(qts, STATUS_STEP_NS);
    }

    qtest_memread(qts, ring_phys(ring.resp_iova)
                       + ring.resp_next * ISP_QUEUE_ENTRY_SIZE,
                  status, ISP_QUEUE_ENTRY_SIZE);
    normalise_words(status, ISP_QUEUE_ENTRY_SIZE);
    g_assert_cmphex(status[ISP_ENTRY_OFF_TYPE], ==, ISP_ENTRY_TYPE_STATUS);

    ring.resp_next = (ring.resp_next + 1) % ring.resp_len;
    isp_writew(qts, ISP_REG_RESPONSE_QUEUE, ring.resp_next);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
    return get32le(status + ISP_STATUS_OFF_HANDLE);
}

/* The completion for one handle, which has to be the one that comes next. */
static void wait_status(QTestState *qts, uint32_t handle, uint8_t *status)
{
    g_assert_cmphex(wait_next_status(qts, status), ==, handle);
}

/* A command with no data, run to completion; the caller reads the status. */
static void run_simple(QTestState *qts, uint32_t handle, unsigned channel,
                       unsigned target, unsigned lun, uint8_t op,
                       uint8_t *status)
{
    uint8_t cdb[6] = { op };

    submit_a64(qts, ring.rqst_next, handle, channel, target, lun, cdb,
               sizeof(cdb), 0, NULL, 0);
    wait_status(qts, handle, status);
}

/*
 * A reset leaves the disk reporting a unit attention, which the next
 * command collects. Drivers absorb it the same way. Tests that care what
 * a reset did read the completion themselves instead of calling here.
 */
static void absorb_unit_attention(QTestState *qts, unsigned channel,
                                  unsigned target, unsigned lun,
                                  uint32_t handle)
{
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    run_simple(qts, handle, channel, target, lun, SCSI_TEST_UNIT_READY,
               status);
}

static void put_lba10(uint8_t *cdb, uint32_t lba, uint16_t blocks)
{
    cdb[2] = lba >> 24;
    cdb[3] = lba >> 16;
    cdb[4] = lba >> 8;
    cdb[5] = lba;
    cdb[7] = blocks >> 8;
    cdb[8] = blocks;
}

/* READ(10) of one sector into one descriptor, GOOD, into the caller's copy. */
static void read_sector(QTestState *qts, unsigned channel, unsigned target,
                        unsigned lun, uint32_t handle, uint32_t lba,
                        uint32_t buf_phys, uint8_t *out)
{
    uint8_t cdb[10] = { SCSI_READ_10 };
    const RingSeg seg = { DIRECT64(buf_phys), SECTOR_BYTES };
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    put_lba10(cdb, lba, 1);
    qtest_memset(qts, buf_phys, 0xcc, SECTOR_BYTES);
    submit_a64(qts, ring.rqst_next, handle, channel, target, lun, cdb,
               sizeof(cdb), ISP_REQ_CONTROL_DATA_IN, &seg, 1);
    wait_status(qts, handle, status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);

    qtest_memread(qts, buf_phys, out, SECTOR_BYTES);
    normalise_words(out, SECTOR_BYTES);
}

/*
 * WRITE(10) of one sector of a single repeated byte, which the lane
 * correction leaves alone in either direction.
 */
static void write_sector(QTestState *qts, unsigned channel, unsigned target,
                         unsigned lun, uint32_t handle, uint32_t lba,
                         uint32_t buf_phys, uint8_t fill)
{
    uint8_t cdb[10] = { SCSI_WRITE_10 };
    const RingSeg seg = { DIRECT64(buf_phys), SECTOR_BYTES };
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    put_lba10(cdb, lba, 1);
    qtest_memset(qts, buf_phys, fill, SECTOR_BYTES);
    submit_a64(qts, ring.rqst_next, handle, channel, target, lun, cdb,
               sizeof(cdb), ISP_REQ_CONTROL_DATA_OUT, &seg, 1);
    wait_status(qts, handle, status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);
}

static void expect_sector(const uint8_t *got, uint8_t fill)
{
    uint8_t want[SECTOR_BYTES];

    memset(want, fill, sizeof(want));
    g_assert_cmpmem(got, SECTOR_BYTES, want, sizeof(want));
}

/*
 * The two initialisation commands carry no ordering between them and no
 * shared field, so either order has to leave both rings where it was
 * told. The bases sit in different pages with different low words, and
 * the two passes swap which ring gets which, so a model holding one base
 * or one length for both aims a transfer at the other ring.
 */
static void test_queue_init_in_both_orders_with_distinct_bases(const void *data)
{
    unsigned pass;

    for (pass = 0; pass < 2; pass++) {
        bool request_first = pass == 1;
        uint64_t rqst = DIRECT64(request_first ? RQST_PHYS : RESP_PHYS);
        uint64_t resp = DIRECT64(request_first ? RESP_PHYS : RQST_PHYS);
        DiskPair disks;
        QTestState *qts = isp12160_start_with_disks(&disks);
        uint8_t got[SECTOR_BYTES];

        qtest_memset(qts, ring_phys(rqst), 0,
                     ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
        qtest_memset(qts, ring_phys(resp), 0,
                     ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);

        init_queues(qts, rqst, RQST_SLOTS, resp, RESP_SLOTS, request_first);

        /* Both cursors start where the initialisation put them. */
        g_assert_cmphex(isp_readw(qts, ISP_REG_REQUEST_QUEUE), ==, 0);
        g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 0);

        absorb_unit_attention(qts, 0, DISK_TARGET, 0, 0x000000a0);
        read_sector(qts, 0, DISK_TARGET, 0, 0x00000001, 0, BUF_PHYS, got);
        expect_sector(got, DISK0_SECTOR0_FILL);

        qtest_quit(qts);
        disks_remove(&disks);
    }
}

/*
 * Address bits 47:32 of the response ring reach the fabric. With them
 * naming an address four gigabytes past this machine's memory, the
 * completion has nowhere to land: a model that truncated the base to the
 * low words would write it over the sentinel slot instead and raise the
 * line for a completion the guest can never find at that address.
 */
static void test_response_queue_high_word_participates(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    const uint64_t resp_high = DIRECT64(RESP_PHYS) | (1ULL << 32);
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY };
    uint8_t slot0[ISP_QUEUE_ENTRY_SIZE], sentinel[ISP_QUEUE_ENTRY_SIZE];

    aim_window_at_memory_bar(qts);

    memset(sentinel, 0xee, sizeof(sentinel));
    qtest_memwrite(qts, RESP_PHYS, sentinel, sizeof(sentinel));
    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);

    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, resp_high, RESP_SLOTS,
                true);

    /* Nothing sits at this target, so the answer needs no disk. */
    submit_a64(qts, 0, 0x0000dead, 0, EMPTY_TARGET, 0, cdb, sizeof(cdb), 0,
               NULL, 0);

    /* The entry was consumed, so the quiet ring below is the refusal. */
    g_assert_cmphex(isp_readw(qts, ISP_REG_REQUEST_QUEUE), ==, 1);

    qtest_memread(qts, RESP_PHYS, slot0, sizeof(slot0));
    g_assert_cmpmem(slot0, sizeof(slot0), sentinel, sizeof(sentinel));
    g_assert_cmphex(isp_readw(qts, ISP_REG_BUS_STATUS) &
                    ISP_BUS_STATUS_INT_BITS, ==, 0);

    qtest_quit(qts);
}

/* Five descriptors: two in the command entry, three in one continuation. */
#define WRAP_SEGS           5
#define WRAP_SENTINEL       0xcc
#define WRAP_GUARD          16
#define WRAP_REGION         (WRAP_GUARD * (WRAP_SEGS + 1) + 2 * SECTOR_BYTES)

static const uint32_t wrap_seg_len[WRAP_SEGS] = { 256, 256, 256, 128, 128 };

/*
 * A command at the last slot whose continuation is at slot zero: the
 * adapter has to walk the chain around the end of the ring, and the two
 * sectors it reads have to land in five descriptors of three different
 * lengths. Every descriptor is fenced, and the second sector's bytes all
 * differ from their neighbours, so a dropped continuation, a descriptor
 * taken in the wrong order and a transfer running past its length each
 * show up in the compare rather than cancelling out.
 */
static void test_a64_request_with_continuation_across_ring_wrap(
    const void *data)
{
    DiskPair disks;
    QTestState *qts = isp12160_start_with_disks(&disks);
    RingSeg segs[WRAP_SEGS];
    uint32_t off[WRAP_SEGS];
    uint8_t cdb[10] = { SCSI_READ_10 };
    uint8_t marker[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t sectors[2 * SECTOR_BYTES];
    uint8_t region[WRAP_REGION], want[WRAP_REGION];
    unsigned i, cursor, pos;

    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);

    /*
     * Slot 0 collects the disk's power-on unit attention and slots 1 and 2
     * are markers, which the adapter steps past. That leaves the consumer
     * at the last slot, which is where the command has to go for its
     * continuation to wrap.
     */
    absorb_unit_attention(qts, 0, DISK_TARGET, 0, 0x000000a0);
    memset(marker, 0, sizeof(marker));
    marker[ISP_ENTRY_OFF_TYPE] = ISP_ENTRY_TYPE_MARKER;
    marker[ISP_ENTRY_OFF_COUNT] = 1;
    write_ring_entry(qts, 1, marker);
    write_ring_entry(qts, 2, marker);
    isp_writew(qts, ISP_REG_REQUEST_QUEUE, 3);
    g_assert_cmphex(isp_readw(qts, ISP_REG_REQUEST_QUEUE), ==, 3);

    memset(want, WRAP_SENTINEL, sizeof(want));
    qtest_memset(qts, BUF_PHYS, WRAP_SENTINEL, sizeof(want));

    cursor = WRAP_GUARD;
    for (i = 0; i < WRAP_SEGS; i++) {
        off[i] = cursor;
        segs[i].iova = DIRECT64(BUF_PHYS + cursor);
        segs[i].len = wrap_seg_len[i];
        cursor += wrap_seg_len[i] + WRAP_GUARD;
    }
    g_assert_cmpuint(cursor, ==, sizeof(want));

    memset(sectors, DISK0_SECTOR0_FILL, SECTOR_BYTES);
    for (i = 0; i < SECTOR_BYTES; i++) {
        sectors[SECTOR_BYTES + i] = disk0_sector1_byte(i);
    }
    pos = 0;
    for (i = 0; i < WRAP_SEGS; i++) {
        memcpy(want + off[i], sectors + pos, wrap_seg_len[i]);
        pos += wrap_seg_len[i];
    }
    g_assert_cmpuint(pos, ==, sizeof(sectors));

    put_lba10(cdb, 0, 2);
    submit_a64(qts, 3, 0x0000c047, 0, DISK_TARGET, 0, cdb, sizeof(cdb),
               ISP_REQ_CONTROL_DATA_IN, segs, WRAP_SEGS);
    wait_status(qts, 0x0000c047, status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);

    qtest_memread(qts, BUF_PHYS, region, sizeof(region));
    normalise_words(region, sizeof(region));
    g_assert_cmpmem(region, sizeof(region), want, sizeof(want));

    /* Two slots taken from the last one: the chain wrapped past zero. */
    g_assert_cmphex(isp_readw(qts, ISP_REG_REQUEST_QUEUE), ==, 1);

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * The same target number and LUN on each channel, holding different
 * disks. A model with one SCSI bus, or one that ignores the channel bit,
 * answers both reads from the same medium.
 */
static void test_same_target_both_channels_read_different_disks(
    const void *data)
{
    DiskPair disks;
    QTestState *qts = isp12160_start_with_disks(&disks);
    uint8_t got[SECTOR_BYTES];

    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);

    absorb_unit_attention(qts, 0, DISK_TARGET, 0, 0x000000a0);
    absorb_unit_attention(qts, 1, DISK_TARGET, 0, 0x000000a1);

    /* wait_status() refuses a completion carrying the other handle. */
    read_sector(qts, 0, DISK_TARGET, 0, 0x00000011, 0, BUF_PHYS, got);
    expect_sector(got, DISK0_SECTOR0_FILL);

    read_sector(qts, 1, DISK_TARGET, 0, 0x00000022, 0, BUF_PHYS, got);
    expect_sector(got, DISK1_SECTOR0_FILL);

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * Two LUNs behind one target. The core's device lookup answers the first
 * device at a target when nothing sits at the LUN asked for, so a
 * duplicate-address check that does not compare the LUN refuses the second
 * disk and the machine will not start. Both are read, because starting is
 * only half of it: a request has to reach the LUN it names.
 */
static void test_two_luns_behind_one_target(const void *data)
{
    DiskPair disks;
    QTestState *qts;
    g_autofree char *args = NULL;
    uint8_t got[SECTOR_BYTES];

    disks_create(&disks);
    args = g_strdup_printf(
        MACHINE
        " -drive if=none,id=d0,format=raw,file=%s"
        " -device scsi-hd,bus=scsi.3,scsi-id=1,lun=0,drive=d0"
        " -drive if=none,id=d1,format=raw,file=%s"
        " -device scsi-hd,bus=scsi.3,scsi-id=1,lun=1,drive=d1",
        disks.path[0], disks.path[1]);
    qts = qtest_init(args);
    aim_window_at_memory_bar(qts);

    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);

    absorb_unit_attention(qts, 1, DISK_TARGET, 0, 0x000000c0);
    absorb_unit_attention(qts, 1, DISK_TARGET, 1, 0x000000c1);

    read_sector(qts, 1, DISK_TARGET, 0, 0x00000071, 0, BUF_PHYS, got);
    expect_sector(got, DISK0_SECTOR0_FILL);

    read_sector(qts, 1, DISK_TARGET, 1, 0x00000072, 0, BUF_PHYS, got);
    expect_sector(got, DISK1_SECTOR0_FILL);

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * A write on channel 1 has to reach channel 1's medium and stay there
 * across the guest's soft reset, and channel 0's disk has to be untouched
 * at the same block. The first command after the reset is where the disk
 * reports it: the reset reaches every realised bus, not only the first.
 */
static void test_write_persists_on_channel_1(const void *data)
{
    DiskPair disks;
    QTestState *qts = isp12160_start_with_disks(&disks);
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t got[SECTOR_BYTES];

    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);

    /*
     * Both power-on unit attentions collected, so the ones the tests read
     * after the reset can only have come from the reset.
     */
    absorb_unit_attention(qts, 0, DISK_TARGET, 0, 0x000000a0);
    absorb_unit_attention(qts, 1, DISK_TARGET, 0, 0x000000a1);
    write_sector(qts, 1, DISK_TARGET, 0, 0x00000031, 5, BUF_PHYS, 0x5a);
    read_sector(qts, 1, DISK_TARGET, 0, 0x00000032, 5, BUF_PHYS, got);
    expect_sector(got, 0x5a);

    isp_writew(qts, ISP_REG_BUS_CTRL, ISP_BUS_CTRL_SOFT_RESET);
    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);

    /* Channel 1's disk saw the reset, the same as channel 0's. */
    run_simple(qts, 0x00000041, 1, DISK_TARGET, 0, SCSI_TEST_UNIT_READY,
               status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==,
                    ISP_SCSI_STATUS_CHECK_CONDITION);
    run_simple(qts, 0x00000042, 0, DISK_TARGET, 0, SCSI_TEST_UNIT_READY,
               status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==,
                    ISP_SCSI_STATUS_CHECK_CONDITION);

    read_sector(qts, 1, DISK_TARGET, 0, 0x00000033, 5, BUF_PHYS, got);
    expect_sector(got, 0x5a);

    /* The other channel's block at the same number never took the write. */
    read_sector(qts, 0, DISK_TARGET, 0, 0x00000034, 5, BUF_PHYS, got);
    expect_sector(got, 0x00);

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * A real INQUIRY payload from each channel's own disk, not a zero-filled
 * transfer that a status entry alone would not distinguish from one.
 */
#define INQUIRY_BYTES   36

static void test_inquiry_real_payload_on_both_channels(const void *data)
{
    DiskPair disks;
    QTestState *qts = isp12160_start_with_disks(&disks);
    unsigned channel;

    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);

    for (channel = 0; channel < 2; channel++) {
        uint32_t buf = BUF_PHYS + channel * PAGE_SIZE_4K;
        uint8_t cdb[6] = { SCSI_INQUIRY };
        const RingSeg seg = { DIRECT64(buf), INQUIRY_BYTES };
        uint8_t status[ISP_QUEUE_ENTRY_SIZE];
        uint8_t payload[INQUIRY_BYTES];
        uint32_t handle = 0x00000050 + channel;

        cdb[4] = INQUIRY_BYTES;
        qtest_memset(qts, buf, WRAP_SENTINEL, INQUIRY_BYTES);
        submit_a64(qts, ring.rqst_next, handle, channel, DISK_TARGET, 0, cdb,
                   sizeof(cdb), ISP_REQ_CONTROL_DATA_IN, &seg, 1);
        wait_status(qts, handle, status);
        g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                        ISP_IOCB_COMPLETE);

        qtest_memread(qts, buf, payload, sizeof(payload));
        normalise_words(payload, sizeof(payload));
        g_assert_cmpmem(payload + 8, 8, "QEMU    ", 8);
        g_assert_cmpmem(payload + 16, 13, "QEMU HARDDISK", 13);
    }

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * Each cursor mailbox does one thing on a read and a different thing on a
 * write. Mailbox 4 read is the adapter's request consumer and written is
 * the host's producer; mailbox 5 read is the adapter's response producer
 * and written is the host's consumer. A model backing either one with a
 * single word hands the last write back and fails here.
 */
static void test_cursor_roles_are_distinct(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY };
    unsigned i;

    aim_window_at_memory_bar(qts);
    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);

    /* Three commands to an empty target: answered without a disk. */
    for (i = 0; i < 3; i++) {
        submit_a64(qts, i, 0x00000060 + i, 0, EMPTY_TARGET, 0, cdb,
                   sizeof(cdb), 0, NULL, 0);
    }

    g_assert_cmphex(isp_readw(qts, ISP_REG_REQUEST_QUEUE), ==, 3);
    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 3);

    /* A partial release: the read still answers the producer. */
    isp_writew(qts, ISP_REG_RESPONSE_QUEUE, 1);
    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 3);
    isp_writew(qts, ISP_REG_RESPONSE_QUEUE, 3);
    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 3);

    /*
     * Seven is three in a four-entry ring: a producer index the adapter
     * has already reached, published again. Nothing runs, the consumer
     * stays where it is, and the read answers the consumer rather than
     * the seven that was written.
     */
    isp_writew(qts, ISP_REG_REQUEST_QUEUE, 7);
    g_assert_cmphex(isp_readw(qts, ISP_REG_REQUEST_QUEUE), ==, 3);
    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==, 3);

    qtest_quit(qts);
}

/*
 * A one-sector read into descriptors describing twice that much. The
 * command asks for 512 bytes, so the rest of the described memory has to
 * come back as the test left it and the entry has to say how many of the
 * described bytes went unused. Padding the transfer instead would leave
 * a driver believing a full buffer it never received.
 */
#define UNDERRUN_DESCRIBED  (2 * SECTOR_BYTES)

static void test_underrun_reports_residual_and_leaves_sentinels(
    const void *data)
{
    DiskPair disks;
    QTestState *qts = isp12160_start_with_disks(&disks);
    const RingSeg segs[2] = {
        { DIRECT64(BUF_PHYS), SECTOR_BYTES },
        { DIRECT64(BUF_PHYS + SECTOR_BYTES), SECTOR_BYTES },
    };
    uint8_t cdb[10] = { SCSI_READ_10 };
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t region[UNDERRUN_DESCRIBED], want[UNDERRUN_DESCRIBED];

    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);
    absorb_unit_attention(qts, 0, DISK_TARGET, 0, 0x000000a0);

    /*
     * The disk's second sector holds a pattern rather than more of the
     * first sector's fill, so a transfer that ran on past the command's
     * length is a mismatch here and not an accidental match.
     */
    memset(want, WRAP_SENTINEL, sizeof(want));
    memset(want, DISK0_SECTOR0_FILL, SECTOR_BYTES);
    qtest_memset(qts, BUF_PHYS, WRAP_SENTINEL, sizeof(want));

    put_lba10(cdb, 0, 1);
    submit_a64(qts, ring.rqst_next, 0x00000081, 0, DISK_TARGET, 0, cdb,
               sizeof(cdb), ISP_REQ_CONTROL_DATA_IN, segs, ARRAY_SIZE(segs));
    wait_status(qts, 0x00000081, status);

    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_DATA_UNDERRUN);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);
    g_assert_cmpuint(get32le(status + ISP_STATUS_OFF_RESID), ==, SECTOR_BYTES);

    qtest_memread(qts, BUF_PHYS, region, sizeof(region));
    normalise_words(region, sizeof(region));
    g_assert_cmpmem(region, sizeof(region), want, sizeof(want));

    qtest_quit(qts);
    disks_remove(&disks);
}

/* Far past the end of a one-megabyte disk. */
#define LBA_BEYOND_DISK             0x7fffffffu

/* Fixed-format sense: eighteen bytes, the key in byte 2's low nibble. */
#define SENSE_FIXED_BYTES           18
#define SENSE_KEY_MASK              0x0f
#define SENSE_KEY_ILLEGAL_REQUEST   0x05
#define SENSE_KEY_UNIT_ATTENTION    0x06

/* What the response ring holds before the adapter writes any of it. */
#define RESP_SENTINEL               0xee

/*
 * A read the target refuses. The transport did its work, so the entry
 * has to carry the target's own status and its sense bytes rather than
 * an empty failure: a driver that cannot see ILLEGAL REQUEST here has no
 * way to tell a bad command from a broken disk. The sense area is 32
 * bytes at 0x20 and the entry ends at 0x40, so a shorter count must not
 * leave the adapter writing past the slot it was given.
 */
static void test_check_condition_carries_real_sense(const void *data)
{
    DiskPair disks;
    QTestState *qts = isp12160_start_with_disks(&disks);
    const RingSeg seg = { DIRECT64(BUF_PHYS), SECTOR_BYTES };
    uint8_t cdb[10] = { SCSI_READ_10 };
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t past[ISP_QUEUE_ENTRY_SIZE], untouched[ISP_QUEUE_ENTRY_SIZE];
    uint16_t sense_len, state;

    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, RESP_SENTINEL,
                 ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    memset(untouched, RESP_SENTINEL, sizeof(untouched));
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);

    /* Slot 0 takes the power-on unit attention, so this read lands in 1. */
    absorb_unit_attention(qts, 0, DISK_TARGET, 0, 0x000000a0);

    qtest_memset(qts, BUF_PHYS, WRAP_SENTINEL, SECTOR_BYTES);
    put_lba10(cdb, LBA_BEYOND_DISK, 1);
    submit_a64(qts, ring.rqst_next, 0x00000091, 0, DISK_TARGET, 0, cdb,
               sizeof(cdb), ISP_REQ_CONTROL_DATA_IN, &seg, 1);
    wait_status(qts, 0x00000091, status);

    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==,
                    ISP_SCSI_STATUS_CHECK_CONDITION);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);

    state = get16le(status + ISP_STATUS_OFF_STATE_FLAGS);
    g_assert_cmphex(state & ISP_STATE_FLAG_GOT_STATUS, ==,
                    ISP_STATE_FLAG_GOT_STATUS);
    g_assert_cmphex(state & ISP_STATE_FLAG_GOT_SENSE, ==,
                    ISP_STATE_FLAG_GOT_SENSE);

    sense_len = get16le(status + ISP_STATUS_OFF_SENSE_LEN);
    g_assert_cmpuint(sense_len, >=, SENSE_FIXED_BYTES);
    g_assert_cmpuint(sense_len, <=, ISP_STATUS_SENSE_MAX);
    g_assert_cmphex(status[ISP_STATUS_OFF_SENSE_DATA + 2] & SENSE_KEY_MASK,
                    ==, SENSE_KEY_ILLEGAL_REQUEST);

    /* The slot after this one still holds what the test put there. */
    qtest_memread(qts, ring_phys(ring.resp_iova) + 2 * ISP_QUEUE_ENTRY_SIZE,
                  past, sizeof(past));
    g_assert_cmpmem(past, sizeof(past), untouched, sizeof(untouched));

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * Nothing sits at this target on either channel. Both answers say the
 * command never got past the bus, so a driver retries elsewhere instead
 * of waiting on a device that is not there, and the handle comes back so
 * it knows which command it is retiring.
 */
static void test_missing_target_on_each_channel(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY };
    const uint32_t handles[2] = { 0x00000031, 0x00000032 };
    unsigned channel;

    aim_window_at_memory_bar(qts);
    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);

    for (channel = 0; channel < 2; channel++) {
        uint8_t status[ISP_QUEUE_ENTRY_SIZE];

        submit_a64(qts, ring.rqst_next, handles[channel], channel,
                   ABSENT_TARGET, 0, cdb, sizeof(cdb), 0, NULL, 0);
        /* wait_status() refuses an entry carrying any other handle. */
        wait_status(qts, handles[channel], status);

        g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                        ISP_IOCB_INCOMPLETE);
        g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATE_FLAGS), ==,
                        ISP_STATE_FLAG_GOT_BUS);
    }

    qtest_quit(qts);
}

/*
 * A refused command on one channel beside a good one on the other, both
 * published together. The failure has to stay on its own channel: the
 * other command's payload arrives whole, and the refused command's own
 * medium is left as it was.
 */
static void test_a_failure_on_one_channel_leaves_the_other_alone(
    const void *data)
{
    DiskPair disks;
    QTestState *qts = isp12160_start_with_disks(&disks);
    const RingSeg bad_seg = { DIRECT64(BUF_PHYS), SECTOR_BYTES };
    const RingSeg good_seg = { DIRECT64(BUF_PHYS + SECTOR_BYTES),
                               SECTOR_BYTES };
    uint8_t bad_cdb[10] = { SCSI_READ_10 };
    uint8_t good_cdb[10] = { SCSI_READ_10 };
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t payload[SECTOR_BYTES], sentinels[SECTOR_BYTES];

    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);
    absorb_unit_attention(qts, 0, DISK_TARGET, 0, 0x000000a0);
    absorb_unit_attention(qts, 1, DISK_TARGET, 0, 0x000000a1);

    memset(sentinels, WRAP_SENTINEL, sizeof(sentinels));
    qtest_memset(qts, BUF_PHYS, WRAP_SENTINEL, 2 * SECTOR_BYTES);

    put_lba10(bad_cdb, LBA_BEYOND_DISK, 1);
    put_lba10(good_cdb, 0, 1);
    place_a64(qts, ring.rqst_next, 0x000000b1, 1, DISK_TARGET, 0, bad_cdb,
              sizeof(bad_cdb), ISP_REQ_CONTROL_DATA_IN, &bad_seg, 1);
    place_a64(qts, ring.rqst_next, 0x000000b0, 0, DISK_TARGET, 0, good_cdb,
              sizeof(good_cdb), ISP_REQ_CONTROL_DATA_IN, &good_seg, 1);
    publish_requests(qts);

    /* The refused command needs no medium, so it retires first. */
    wait_status(qts, 0x000000b1, status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==,
                    ISP_SCSI_STATUS_CHECK_CONDITION);
    qtest_memread(qts, BUF_PHYS, payload, sizeof(payload));
    g_assert_cmpmem(payload, sizeof(payload), sentinels, sizeof(sentinels));

    wait_status(qts, 0x000000b0, status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);
    qtest_memread(qts, BUF_PHYS + SECTOR_BYTES, payload, sizeof(payload));
    normalise_words(payload, sizeof(payload));
    expect_sector(payload, DISK0_SECTOR0_FILL);

    /* Channel 1's own medium is where it was before the refusal. */
    read_sector(qts, 1, DISK_TARGET, 0, 0x000000b2, 0, BUF_PHYS, payload);
    expect_sector(payload, DISK1_SECTOR0_FILL);

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * Recovery. A command has to still be outstanding when the abort or the
 * reset arrives, so these tests put both disks behind one throttle
 * filter. The group's bucket lets a first sector read through and holds
 * the ones after it, and a held read cannot finish inside the write that
 * published it. Cancelling such a read waits for it, which is why every
 * wait below is bounded in wall time rather than in polls.
 *
 * The rate is what the held read then costs: the bucket drains in
 * (bytes accounted - rate/10) / rate seconds of real time, and that same
 * figure is the margin between filling the bucket and submitting the
 * read under test. A kilobyte a second puts both at four tenths of a
 * second -- long enough that the handful of commands in between cannot
 * close it, short enough that seven tests fit in the suite's time.
 */
#define THROTTLE_BPS_READ   1024

static QTestState *isp12160_start_with_throttled_disks(DiskPair *disks)
{
    QTestState *qts;
    g_autofree char *args = NULL;

    disks_create(disks);
    args = g_strdup_printf(
        MACHINE
        " -object throttle-group,id=tg,x-bps-read=%u"
        " -blockdev driver=file,node-name=f0,filename=%s"
        " -blockdev driver=throttle,node-name=t0,throttle-group=tg,file=f0"
        " -device scsi-hd,bus=scsi.2,scsi-id=1,drive=t0"
        " -blockdev driver=file,node-name=f1,filename=%s"
        " -blockdev driver=throttle,node-name=t1,throttle-group=tg,file=f1"
        " -device scsi-hd,bus=scsi.3,scsi-id=1,drive=t1",
        THROTTLE_BPS_READ, disks->path[0], disks->path[1]);

    qts = qtest_init(args);
    aim_window_at_memory_bar(qts);
    return qts;
}

/* Handles for the work that clears the way, kept out of the tests' range. */
#define RECOVERY_UA_CHANNEL0    0x000000a0
#define RECOVERY_UA_CHANNEL1    0x000000a1
#define RECOVERY_FILL_BUCKET    0x000000a2

/* The recovery mailboxes, three of which the family did not answer. */
#define ISP_MBOX_CMD_ABORT_COMMAND  0x0015
#define ISP_MBOX_CMD_ABORT_DEVICE   0x0016
#define ISP_MBOX_CMD_ABORT_TARGET   0x0017
#define ISP_MBOX_CMD_BUS_RESET      0x0018

#define ISP_MBOX_CMD_ERROR          0x4005

/* What a retired request carries where a finished one carries its result. */
#define ISP_IOCB_RESET              0x0004
#define ISP_IOCB_ABORTED            0x0005

/*
 * Queues, disks, and the read that fills the throttle group's bucket.
 * That read has to happen before the ones under test are submitted, or
 * they finish inside the write that published them and there is nothing
 * left to recover.
 */
static QTestState *recovery_start(DiskPair *disks)
{
    QTestState *qts = isp12160_start_with_throttled_disks(disks);
    uint8_t got[SECTOR_BYTES];

    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);
    absorb_unit_attention(qts, 0, DISK_TARGET, 0, RECOVERY_UA_CHANNEL0);
    absorb_unit_attention(qts, 1, DISK_TARGET, 0, RECOVERY_UA_CHANNEL1);

    read_sector(qts, 0, DISK_TARGET, 0, RECOVERY_FILL_BUCKET, 0, BUF_PHYS,
                got);
    expect_sector(got, DISK0_SECTOR0_FILL);
    return qts;
}

/* A one-sector read placed in the ring, left for the caller to publish. */
static void place_read(QTestState *qts, uint32_t handle, unsigned channel,
                       uint32_t buf_phys)
{
    uint8_t cdb[10] = { SCSI_READ_10 };
    const RingSeg seg = { DIRECT64(buf_phys), SECTOR_BYTES };

    put_lba10(cdb, 0, 1);
    place_a64(qts, ring.rqst_next, handle, channel, DISK_TARGET, 0, cdb,
              sizeof(cdb), ISP_REQ_CONTROL_DATA_IN, &seg, 1);
}

/* The ring stays put, checked after giving a late entry time to land. */
#define QUIET_WAIT_US   (500 * 1000)

static void expect_no_further_status(QTestState *qts)
{
    gint64 until = g_get_monotonic_time() + QUIET_WAIT_US;

    while (g_get_monotonic_time() < until) {
        qtest_clock_step(qts, STATUS_STEP_NS);
    }
    g_assert_cmphex(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==,
                    ring.resp_next);
}

/*
 * The whole entry a retirement writes: a completion code, and every other
 * field left at zero. A retired command reached no phase, carries no
 * target status, transferred nothing and has no sense to report, so a
 * model filling any of those in would be describing work it did not do.
 */
static void expect_retired(const uint8_t *status, uint16_t completion)
{
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    completion);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATE_FLAGS), ==, 0);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATUS_FLAGS), ==, 0);
    g_assert_cmpuint(get32le(status + ISP_STATUS_OFF_RESID), ==, 0);
    g_assert_cmpuint(get16le(status + ISP_STATUS_OFF_SENSE_LEN), ==, 0);
}

/*
 * A reset raises a unit attention on every device it reached, and the
 * next command to that device collects it. Reading it back is the only
 * thing that separates a model that reset the target from one that
 * retired the target's requests and reset nothing, and the probe drains
 * the attention so the commands after it see a settled device.
 */
static void expect_reset_reported(QTestState *qts, unsigned channel,
                                  uint32_t handle)
{
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    run_simple(qts, handle, channel, DISK_TARGET, 0, SCSI_TEST_UNIT_READY,
               status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==,
                    ISP_SCSI_STATUS_CHECK_CONDITION);
    g_assert_cmphex(status[ISP_STATUS_OFF_SENSE_DATA + 2] & SENSE_KEY_MASK,
                    ==, SENSE_KEY_UNIT_ATTENTION);
}

/* The same probe on a device the reset had no business reaching. */
static void expect_no_reset_reported(QTestState *qts, unsigned channel,
                                     uint32_t handle)
{
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    run_simple(qts, handle, channel, DISK_TARGET, 0, SCSI_TEST_UNIT_READY,
               status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);
}

/*
 * The two entries a scoped recovery command leaves behind: the retired
 * one and the survivor, which finishes normally. They arrive in
 * whichever order the backend releases them -- the cancellation waits
 * for the read it is retiring, and the other channel's read can finish
 * inside that wait -- and nothing in the contract orders them, so they
 * are sorted by handle rather than by arrival.
 */
static void expect_one_retired_one_survivor(QTestState *qts,
                                            uint32_t retired_handle,
                                            uint16_t completion,
                                            uint32_t survivor_handle)
{
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    unsigned seen;

    for (seen = 0; seen < 2; seen++) {
        uint32_t handle = wait_next_status(qts, status);

        if (handle == retired_handle) {
            expect_retired(status, completion);
        } else {
            g_assert_cmphex(handle, ==, survivor_handle);
            g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                            ISP_IOCB_COMPLETE);
            g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==,
                            0);
        }
    }
}

/* The survivor's payload arrived whole while its neighbour was retired. */
static void expect_survivor_payload(QTestState *qts, uint32_t buf_phys,
                                    uint8_t fill)
{
    uint8_t payload[SECTOR_BYTES];

    qtest_memread(qts, buf_phys, payload, sizeof(payload));
    normalise_words(payload, sizeof(payload));
    expect_sector(payload, fill);
}

/* One read outstanding per channel, at the same target and LUN. */
static void place_one_read_per_channel(QTestState *qts, uint32_t handle0,
                                       uint32_t handle1)
{
    qtest_memset(qts, BUF_PHYS, WRAP_SENTINEL, 2 * SECTOR_BYTES);
    place_read(qts, handle0, 0, BUF_PHYS);
    place_read(qts, handle1, 1, BUF_PHYS + SECTOR_BYTES);
    publish_requests(qts);
}

/*
 * Abort device names a channel, a target and a LUN and carries no
 * handle. The same target and LUN number is busy on the other channel,
 * so a model that drops the channel out of the selector retires both.
 */
static void test_abort_device_is_channel_scoped(const void *data)
{
    DiskPair disks;
    QTestState *qts = recovery_start(&disks);
    const uint16_t in[] = { ISP_DEVICE_WORD(1, DISK_TARGET, 0), 0, 0 };

    place_one_read_per_channel(qts, 0x41, 0x42);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_ABORT_DEVICE, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    expect_one_retired_one_survivor(qts, 0x42, ISP_IOCB_ABORTED, 0x41);
    expect_survivor_payload(qts, BUF_PHYS, DISK0_SECTOR0_FILL);
    expect_no_further_status(qts);

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * Abort command names one handle. Its neighbour is on the same channel,
 * target and LUN, so only the handle tells them apart and a model
 * decoding the selector alone retires both.
 */
static void test_abort_command_by_handle(const void *data)
{
    DiskPair disks;
    QTestState *qts = recovery_start(&disks);
    const uint16_t in[] = { ISP_DEVICE_WORD(1, DISK_TARGET, 0), 0, 0x52 };

    qtest_memset(qts, BUF_PHYS, WRAP_SENTINEL, 2 * SECTOR_BYTES);
    place_read(qts, 0x51, 1, BUF_PHYS);
    place_read(qts, 0x52, 1, BUF_PHYS + SECTOR_BYTES);
    publish_requests(qts);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_ABORT_COMMAND, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    expect_one_retired_one_survivor(qts, 0x52, ISP_IOCB_ABORTED, 0x51);
    expect_survivor_payload(qts, BUF_PHYS, DISK1_SECTOR0_FILL);
    expect_no_further_status(qts);

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * A handle the adapter really is holding, named with the wrong channel.
 * The selector has to be matched whole: retiring on the handle alone
 * would cancel a command on a bus the host did not name, and answering
 * complete would tell it a command it is still waiting on is gone.
 */
static void test_abort_command_checks_the_whole_selector(const void *data)
{
    DiskPair disks;
    QTestState *qts = recovery_start(&disks);
    const uint16_t in[] = { ISP_DEVICE_WORD(1, DISK_TARGET, 0), 0, 0x71 };
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    /* The one outstanding command is this target and LUN on channel 0. */
    qtest_memset(qts, BUF_PHYS, WRAP_SENTINEL, SECTOR_BYTES);
    place_read(qts, 0x71, 0, BUF_PHYS);
    publish_requests(qts);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_ABORT_COMMAND, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_CMD_ERROR);
    isp_mbox_ack(qts);

    g_assert_cmphex(wait_next_status(qts, status), ==, 0x71);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);
    expect_survivor_payload(qts, BUF_PHYS, DISK0_SECTOR0_FILL);
    expect_no_further_status(qts);

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * Abort device with nothing outstanding at the address. The command
 * names no handle, so there is nothing for it to miss and nothing to
 * report a miss about: a driver clearing a device it has already drained
 * would read a command error as a failure.
 */
static void test_abort_device_with_nothing_outstanding(const void *data)
{
    DiskPair disks;
    QTestState *qts = isp12160_start_with_disks(&disks);
    const uint16_t in[] = { ISP_DEVICE_WORD(1, DISK_TARGET, 0), 0, 0 };

    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_ABORT_DEVICE, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    expect_no_further_status(qts);
    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * A handle nothing is using. Reporting the abort complete would tell the
 * host that a command it is still waiting on has been retired, and
 * posting an entry for a handle the host never issued is worse.
 */
static void test_abort_unknown_handle_is_a_command_error(const void *data)
{
    DiskPair disks;
    QTestState *qts = isp12160_start_with_disks(&disks);
    const uint16_t in[] = { ISP_DEVICE_WORD(1, DISK_TARGET, 0), 0, 0x77 };

    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_ABORT_COMMAND, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_CMD_ERROR);
    isp_mbox_ack(qts);

    expect_no_further_status(qts);
    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * Target reset reaches one target on one channel. The same target number
 * is busy on the other channel and has to survive, and the reset target
 * has to serve the next command rather than be left cancelled.
 */
static void test_target_reset_is_channel_scoped(const void *data)
{
    DiskPair disks;
    QTestState *qts = recovery_start(&disks);
    const uint16_t in[] = { ISP_DEVICE_WORD(1, DISK_TARGET, 0), 1 };
    uint8_t payload[SECTOR_BYTES];

    place_one_read_per_channel(qts, 0x41, 0x42);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_ABORT_TARGET, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    expect_one_retired_one_survivor(qts, 0x42, ISP_IOCB_RESET, 0x41);
    expect_survivor_payload(qts, BUF_PHYS, DISK0_SECTOR0_FILL);

    /*
     * The reset target reports the reset and the other channel's target
     * does not, so the devices really were reset and only the named one
     * was. Retiring the requests and resetting nothing would leave both
     * quiet here.
     */
    expect_reset_reported(qts, 1, 0x43);
    expect_no_reset_reported(qts, 0, 0x44);

    /* The reset target serves the next command. */
    read_sector(qts, 1, DISK_TARGET, 0, 0x45, 0, BUF_PHYS, payload);
    expect_sector(payload, DISK1_SECTOR0_FILL);

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * Bus reset takes its channel from mailbox 2. Mailbox 1 carries the
 * configured reset delay instead, so a model reading the channel out of
 * mailbox 1 resets a bus the host did not name: the delay here is three,
 * which is not even a channel this adapter has.
 */
static void test_bus_reset_is_channel_scoped(const void *data)
{
    DiskPair disks;
    QTestState *qts = recovery_start(&disks);
    const uint16_t in[] = { 3, 1 };
    uint8_t payload[SECTOR_BYTES];

    place_one_read_per_channel(qts, 0x41, 0x42);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_BUS_RESET, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    expect_one_retired_one_survivor(qts, 0x42, ISP_IOCB_RESET, 0x41);

    /*
     * The bus that was named reports the reset and the other one does
     * not, so a bus really was reset and it was the right one.
     */
    expect_reset_reported(qts, 1, 0x43);
    expect_no_reset_reported(qts, 0, 0x44);

    /* A reset bus, not a wiped one: both media are where they were. */
    read_sector(qts, 1, DISK_TARGET, 0, 0x45, 0, BUF_PHYS, payload);
    expect_sector(payload, DISK1_SECTOR0_FILL);
    read_sector(qts, 0, DISK_TARGET, 0, 0x46, 0, BUF_PHYS, payload);
    expect_sector(payload, DISK0_SECTOR0_FILL);

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * The host cannot see a command finish before its abort reaches the
 * adapter. The abort has to report that there was nothing to retire and
 * leave the completion that already went out alone: a second entry for
 * the same handle would retire a command the host has already accounted
 * for.
 */
static void test_natural_completion_racing_an_abort(const void *data)
{
    DiskPair disks;
    QTestState *qts = isp12160_start_with_disks(&disks);
    const uint16_t in[] = { ISP_DEVICE_WORD(1, DISK_TARGET, 0), 0, 0x62 };
    uint8_t payload[SECTOR_BYTES];

    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(qts, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);
    absorb_unit_attention(qts, 1, DISK_TARGET, 0, RECOVERY_UA_CHANNEL1);

    /* Nothing throttles this disk, so the read is over before the abort. */
    read_sector(qts, 1, DISK_TARGET, 0, 0x62, 0, BUF_PHYS, payload);
    expect_sector(payload, DISK1_SECTOR0_FILL);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_ABORT_COMMAND, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_CMD_ERROR);
    isp_mbox_ack(qts);

    expect_no_further_status(qts);
    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * A handle belongs to the host, and retiring a command hands it back.
 * The next command carrying it is a different command: the adapter must
 * not match it against the retired one or answer it twice.
 */
static void test_retired_handle_is_reusable(const void *data)
{
    DiskPair disks;
    QTestState *qts = recovery_start(&disks);
    const uint16_t in[] = { ISP_DEVICE_WORD(1, DISK_TARGET, 0), 0, 0x61 };
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t payload[SECTOR_BYTES];

    qtest_memset(qts, BUF_PHYS, WRAP_SENTINEL, SECTOR_BYTES);
    place_read(qts, 0x61, 1, BUF_PHYS);
    publish_requests(qts);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_ABORT_COMMAND, in,
                             ARRAY_SIZE(in)), ==, ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);

    g_assert_cmphex(wait_next_status(qts, status), ==, 0x61);
    expect_retired(status, ISP_IOCB_ABORTED);

    read_sector(qts, 1, DISK_TARGET, 0, 0x61, 0, BUF_PHYS, payload);
    expect_sector(payload, DISK1_SECTOR0_FILL);

    expect_no_further_status(qts);
    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * Migration. Everything the guest can still see after the stream is
 * replayed has to come back, and the two banks and the per-channel reset
 * delays this part adds are exactly the state an ISP1040 stream never
 * carried. Both instances open the same raw files: the destination holds
 * them inactive until the source hands over.
 */

/* The command the source runs, and the one the destination runs after it. */
#define MIGRATE_UA_CHANNEL1     0x00000070
#define MIGRATE_READ_CHANNEL1   0x00000071

#define MIGRATE_RAM_LOW_ADDR    0x0100
#define MIGRATE_RAM_LOW_WORD    0x1010
#define MIGRATE_RAM_HIGH_ADDR   0x8100
#define MIGRATE_RAM_HIGH_WORD   0x2020

static void migrate_stopped_vm(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);

    while (true) {
        QDict *ret = qtest_qmp_assert_success_ref(from,
            "{ 'execute': 'query-migrate' }");
        const char *status = qdict_get_str(ret, "status");
        bool done = g_str_equal(status, "completed");

        g_assert(!g_str_equal(status, "failed"));
        qobject_unref(ret);
        if (done) {
            return;
        }
        usleep(1000);
    }
}

/*
 * One value per bank, in the order the selection table lists them, so a
 * stream that carries the RISC and SCSI processor 0 banks alone leaves
 * two of these four reading zero on the far side.
 */
static const uint16_t migrate_bank_word[] = {
    0x1919, 0x2a2a, 0x3b3b, 0x0808
};

/*
 * The register test's patterns, and the mailboxes they go in. Mailboxes 4
 * and 5 are left out: with the rings live those two addresses are the
 * request producer and the response consumer, so writing a pattern there
 * publishes ring work rather than parking a word. What they read back is
 * checked as a cursor instead.
 */
static const uint8_t migrate_probe_mbox[] = { 1, 2, 3, 6, 7 };
static const uint16_t migrate_probe_pattern[] = {
    0x0a0a, 0x0b0b, 0x0c0c, 0x0f0f, 0x1a1a
};

/* Channel 0's delay and channel 1's, which no register hands back. */
static const uint16_t migrate_reset_delay[] = { 7, 9 };

static void test_migration_preserves_distinct_state(const void *data)
{
    DiskPair disks;
    g_autofree char *args = NULL;
    g_autofree char *dir = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    uint16_t rqst_cursor, resp_cursor;
    uint8_t payload[SECTOR_BYTES];
    unsigned i;

    disks_create(&disks);
    args = isp12160_disk_args(&disks);
    dir = g_dir_make_tmp("isp12160-migrate-XXXXXX", NULL);
    g_assert_nonnull(dir);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);

    from = qtest_init(args);
    aim_window_at_memory_bar(from);

    for (i = 0; i < ARRAY_SIZE(bank_selections); i++) {
        isp_writew(from, ISP_REG_BUS_CFG1, bank_selections[i]);
        isp_writew(from, ISP_REG_BANK_BASE, migrate_bank_word[i]);
    }

    /*
     * The source arm, so a miss on the far side is the stream's fault and
     * not a setup that never took.
     */
    for (i = 0; i < ARRAY_SIZE(bank_selections); i++) {
        isp_writew(from, ISP_REG_BUS_CFG1, bank_selections[i]);
        g_assert_cmphex(isp_readw(from, ISP_REG_BANK_BASE), ==,
                        migrate_bank_word[i]);
    }

    isp_write_ram_word(from, MIGRATE_RAM_LOW_ADDR, MIGRATE_RAM_LOW_WORD);
    isp_write_ram_word(from, MIGRATE_RAM_HIGH_ADDR, MIGRATE_RAM_HIGH_WORD);

    qtest_memset(from, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(from, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_queues(from, DIRECT64(RQST_PHYS), RQST_SLOTS, DIRECT64(RESP_PHYS),
                RESP_SLOTS, true);

    /* Collected here so the read on the far side is not the first command. */
    absorb_unit_attention(from, 1, DISK_TARGET, 0, MIGRATE_UA_CHANNEL1);

    g_assert_cmphex(isp_mbox(from, ISP_MBOX_CMD_SET_RESET_DELAY_PARAMS,
                             migrate_reset_delay,
                             ARRAY_SIZE(migrate_reset_delay)), ==,
                    ISP_MBOX_COMPLETE);
    isp_mbox_ack(from);

    /* Unacknowledged on purpose: the result and its latches migrate too. */
    for (i = 0; i < ARRAY_SIZE(migrate_probe_mbox); i++) {
        isp_writew(from, ISP_MAILBOX(migrate_probe_mbox[i]),
                   migrate_probe_pattern[i]);
    }
    isp_writew(from, ISP_MAILBOX(0), ISP_MBOX_CMD_MAILBOX_REG_TEST);
    isp_writew(from, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(isp_readw(from, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);

    rqst_cursor = isp_readw(from, ISP_REG_REQUEST_QUEUE);
    resp_cursor = isp_readw(from, ISP_REG_RESPONSE_QUEUE);
    g_assert_cmpuint(rqst_cursor, ==, ring.rqst_next);
    g_assert_cmpuint(resp_cursor, ==, ring.resp_next);

    to = qtest_initf("%s -incoming %s", args, uri);

    qtest_qmp_assert_success(from, "{ 'execute': 'stop' }");
    migrate_stopped_vm(from, uri);
    qtest_quit(from);

    qtest_qmp_assert_success(to, "{ 'execute': 'cont' }");

    for (i = 0; i < ARRAY_SIZE(migrate_probe_mbox); i++) {
        g_assert_cmphex(isp_readw(to, ISP_MAILBOX(migrate_probe_mbox[i])),
                        ==, migrate_probe_pattern[i]);
    }
    g_assert_cmphex(isp_readw(to, ISP_MAILBOX(0)), ==, ISP_MBOX_COMPLETE);

    g_assert_cmphex(isp_readw(to, ISP_REG_BUS_STATUS) &
                    ISP_BUS_STATUS_INT_BITS, ==, ISP_BUS_STATUS_INT_BITS);
    g_assert_cmphex(isp_readw(to, ISP_REG_SEMAPHORE) & ISP_SEMAPHORE_LOCK,
                    ==, ISP_SEMAPHORE_LOCK);
    isp_mbox_ack(to);

    for (i = 0; i < ARRAY_SIZE(bank_selections); i++) {
        isp_writew(to, ISP_REG_BUS_CFG1, bank_selections[i]);
        g_assert_cmphex(isp_readw(to, ISP_REG_BANK_BASE), ==,
                        migrate_bank_word[i]);
    }

    g_assert_cmphex(isp_read_ram_word(to, MIGRATE_RAM_LOW_ADDR), ==,
                    MIGRATE_RAM_LOW_WORD);
    g_assert_cmphex(isp_read_ram_word(to, MIGRATE_RAM_HIGH_ADDR), ==,
                    MIGRATE_RAM_HIGH_WORD);

    g_assert_cmpuint(isp_readw(to, ISP_REG_REQUEST_QUEUE), ==, rqst_cursor);
    g_assert_cmpuint(isp_readw(to, ISP_REG_RESPONSE_QUEUE), ==, resp_cursor);

    /* No queue initialisation here: the bases and lengths came over. */
    read_sector(to, 1, DISK_TARGET, 0, MIGRATE_READ_CHANNEL1, 0, BUF_PHYS,
                payload);
    expect_sector(payload, DISK1_SECTOR0_FILL);

    qtest_quit(to);
    disks_remove(&disks);
    /* The listening side unlinks its own socket, so the directory is empty. */
    g_assert_cmpint(rmdir(dir), ==, 0);
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
    qtest_add_data_func("sn0/isp12160/a64-ram-load-reaches-the-last-word",
                        NULL, test_a64_ram_load_reaches_the_last_word);
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
    qtest_add_data_func(
        "sn0/isp12160/queue-init-in-both-orders-with-distinct-bases", NULL,
        test_queue_init_in_both_orders_with_distinct_bases);
    qtest_add_data_func("sn0/isp12160/response-queue-high-word-participates",
                        NULL, test_response_queue_high_word_participates);
    qtest_add_data_func(
        "sn0/isp12160/a64-request-with-continuation-across-ring-wrap", NULL,
        test_a64_request_with_continuation_across_ring_wrap);
    qtest_add_data_func(
        "sn0/isp12160/same-target-both-channels-read-different-disks", NULL,
        test_same_target_both_channels_read_different_disks);
    qtest_add_data_func("sn0/isp12160/two-luns-behind-one-target", NULL,
                        test_two_luns_behind_one_target);
    qtest_add_data_func("sn0/isp12160/write-persists-on-channel-1", NULL,
                        test_write_persists_on_channel_1);
    qtest_add_data_func("sn0/isp12160/inquiry-real-payload-on-both-channels",
                        NULL, test_inquiry_real_payload_on_both_channels);
    qtest_add_data_func("sn0/isp12160/cursor-roles-are-distinct", NULL,
                        test_cursor_roles_are_distinct);
    qtest_add_data_func(
        "sn0/isp12160/underrun-reports-residual-and-leaves-sentinels", NULL,
        test_underrun_reports_residual_and_leaves_sentinels);
    qtest_add_data_func("sn0/isp12160/check-condition-carries-real-sense",
                        NULL, test_check_condition_carries_real_sense);
    qtest_add_data_func("sn0/isp12160/missing-target-on-each-channel", NULL,
                        test_missing_target_on_each_channel);
    qtest_add_data_func(
        "sn0/isp12160/a-failure-on-one-channel-leaves-the-other-alone", NULL,
        test_a_failure_on_one_channel_leaves_the_other_alone);
    qtest_add_data_func("sn0/isp12160/abort-device-is-channel-scoped", NULL,
                        test_abort_device_is_channel_scoped);
    qtest_add_data_func("sn0/isp12160/abort-command-by-handle", NULL,
                        test_abort_command_by_handle);
    qtest_add_data_func(
        "sn0/isp12160/abort-command-checks-the-whole-selector", NULL,
        test_abort_command_checks_the_whole_selector);
    qtest_add_data_func(
        "sn0/isp12160/abort-device-with-nothing-outstanding", NULL,
        test_abort_device_with_nothing_outstanding);
    qtest_add_data_func(
        "sn0/isp12160/abort-unknown-handle-is-a-command-error", NULL,
        test_abort_unknown_handle_is_a_command_error);
    qtest_add_data_func("sn0/isp12160/target-reset-is-channel-scoped", NULL,
                        test_target_reset_is_channel_scoped);
    qtest_add_data_func("sn0/isp12160/bus-reset-is-channel-scoped", NULL,
                        test_bus_reset_is_channel_scoped);
    qtest_add_data_func("sn0/isp12160/natural-completion-racing-an-abort",
                        NULL, test_natural_completion_racing_an_abort);
    qtest_add_data_func("sn0/isp12160/retired-handle-is-reusable", NULL,
                        test_retired_handle_is_reusable);
    qtest_add_data_func("sn0/isp12160/migration-preserves-distinct-state",
                        NULL, test_migration_preserves_distinct_state);

    return g_test_run();
}
