/*
 * The Bridge in little-endian host mode. A little-endian processor is
 * memread and memwrite decoded by hand; expected values are literals.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/pci/pci_regs.h"
#include "libqtest.h"

#define WIDGET_BASE     0x40000000ULL
#define TRANSFER_RAM    0x02000000ULL

#define BRIDGE_CONTROL  0x020
#define BRIDGE_DEVICE(n) (0x200 + 8 * (n))
#define CONTROL_IO_SWAP (1u << 23)
#define CONTROL_MEM_SWAP (1u << 22)
#define CONTROL_BIG_ENDIAN (1u << 9)
#define DEV_MEMORY_SPACE (1u << 12)
#define DEV_SWAP        (1u << 13)
#define DEV_64BIT       (1u << 22)

#define CFG0_BASE       (WIDGET_BASE + 0x20000)
#define XLATE_BASE      (WIDGET_BASE + 0x40000000ULL)

/* The card sits in slot 3; slot 4's window carries its I/O space. */
#define CARD_SLOT       3
#define IO_SLOT         4
#define CARD_WINDOW     (WIDGET_BASE + 0x700000)
#define IO_WINDOW       (WIDGET_BASE + 0x800000)
#define CARD_BAR0       0x13000000u
#define CARD_BAR1       0x13000100u
#define CARD_IO_BAR     0x100u

/* The card's 64-bit register BAR: a constant, then a scratch doubleword. */
#define WIDE_FIXED      0x00
#define WIDE_SCRATCH    0x08

/* The card's DMA engine, in BAR0. */
#define XFER_ADDRESS    0x00
#define XFER_LENGTH     0x04
#define XFER_SEED       0x08
#define XFER_GO         0x0c
#define XFER_SOURCE     0x14
#define XFER_GO_FILL    1
#define XFER_GO_COPY    2

#define BRIDGE_DIRECT_MAP 0x080
#define DEV_SWAP_TRANSLATED (1u << 20)
#define DEV_SWAP_DIRECT (1u << 19)
#define ATE_BASE        (WIDGET_BASE + 0x10000)
#define PCI_TRANSLATED  0x40000000u
#define PCI_DIRECT      0x80000000u

typedef struct Rig {
    QTestState *qts;
    bool le;            /* the system order the test last selected */
} Rig;

/* A 64-bit card in a slot the board wires to the 64-bit data path. */
static Rig rig_start(bool wide)
{
    Rig r = { 0 };

    r.qts = qtest_initf("-M none -cpu R10000 "
                        "-device sgi-xio-test-target,id=transfer-ram,size=16M "
                        "-device sgi-bridge,id=bridge,board-part=030-1244-001,"
                        "fabric=/machine/unattached/system[0],req64-slots=%d "
                        "-device %s,bus=pci,addr=3,vendor-id=0x1234,"
                        "device-id=0x5678", wide ? 1 << CARD_SLOT : 0,
                        wide ? "sgi-bridge-dma-test-64bit" :
                               "sgi-bridge-dma-test");
    qtest_sysbus_mmio_map(r.qts, "/machine/peripheral/bridge", 0, WIDGET_BASE);
    qtest_sysbus_mmio_map(r.qts, "/machine/peripheral/transfer-ram", 0,
                          TRANSFER_RAM);
    return r;
}

static uint64_t le_read(QTestState *qts, uint64_t addr, unsigned size)
{
    uint8_t b[8];

    qtest_memread(qts, addr, b, size);
    return ldn_le_p(b, size);
}

static void le_write(QTestState *qts, uint64_t addr, unsigned size,
                     uint64_t value)
{
    uint8_t b[8];

    stn_le_p(b, size, value);
    qtest_memwrite(qts, addr, b, size);
}

/* One access from the processor of the system's current order. */
static uint64_t cpu_read(const Rig *r, uint64_t addr, unsigned size)
{
    if (r->le) {
        return le_read(r->qts, addr, size);
    }
    switch (size) {
    case 1:
        return qtest_readb(r->qts, addr);
    case 2:
        return qtest_readw(r->qts, addr);
    case 4:
        return qtest_readl(r->qts, addr);
    default:
        return qtest_readq(r->qts, addr);
    }
}

static void cpu_write(const Rig *r, uint64_t addr, unsigned size,
                      uint64_t value)
{
    if (r->le) {
        le_write(r->qts, addr, size, value);
        return;
    }
    switch (size) {
    case 1:
        qtest_writeb(r->qts, addr, value);
        break;
    case 2:
        qtest_writew(r->qts, addr, value);
        break;
    case 4:
        qtest_writel(r->qts, addr, value);
        break;
    default:
        qtest_writeq(r->qts, addr, value);
        break;
    }
}

/*
 * A big-endian system finds a register at its slot's offset plus four, a
 * little-endian one at the slot's offset.
 */
static uint32_t reg_read(const Rig *r, unsigned offset)
{
    return cpu_read(r, WIDGET_BASE + offset + (r->le ? 0 : 4), 4);
}

static void reg_write(const Rig *r, unsigned offset, uint32_t value)
{
    cpu_write(r, WIDGET_BASE + offset + (r->le ? 0 : 4), 4, value);
}

static void reg_update(const Rig *r, unsigned offset, uint32_t clear,
                       uint32_t set)
{
    reg_write(r, offset, (reg_read(r, offset) & ~clear) | set);
}

static void set_system_little_endian(Rig *r, bool le)
{
    reg_update(r, BRIDGE_CONTROL, CONTROL_BIG_ENDIAN,
               le ? 0 : CONTROL_BIG_ENDIAN);
    r->le = le;
}

static void set_device(const Rig *r, unsigned slot, uint32_t clear,
                       uint32_t set)
{
    reg_update(r, BRIDGE_DEVICE(slot), clear, set);
}

static void set_control(const Rig *r, uint32_t clear, uint32_t set)
{
    reg_update(r, BRIDGE_CONTROL, clear, set);
}

/* The card's BARs at fixed PCI addresses, set up as a big-endian system. */
static Rig rig_start_with_card(bool wide)
{
    Rig r = rig_start(wide);
    uint64_t cfg = CFG0_BASE + CARD_SLOT * 0x1000;

    qtest_writel(r.qts, cfg + PCI_BASE_ADDRESS_0, CARD_BAR0);
    qtest_writel(r.qts, cfg + PCI_BASE_ADDRESS_1, CARD_BAR1);
    qtest_writel(r.qts, cfg + PCI_BASE_ADDRESS_2, CARD_IO_BAR | 1);
    qtest_writel(r.qts, cfg + PCI_COMMAND, 7);
    set_device(&r, CARD_SLOT, 0x1fffffff, DEV_MEMORY_SPACE | 0x130);
    set_device(&r, IO_SLOT, 0x1fffffff, 0);
    return r;
}

/* The two device-window bits that pick the table row. */
static void select_row(const Rig *r, bool swap, bool wide)
{
    set_device(r, CARD_SLOT, DEV_SWAP | DEV_64BIT,
               (swap ? DEV_SWAP : 0) | (wide ? DEV_64BIT : 0));
}

/*
 * The card's constant is ef cd ab 89 67 45 23 01 at PCI offsets 0 to 7;
 * each row is one aligned read and the value the processor gets.
 */
typedef struct PioRead {
    unsigned size;
    unsigned offset;
    uint64_t value;
} PioRead;

/* Little-endian system, swap clear: bytes stay in address order. */
static const PioRead le_ordered[7] = {
    { 1, 0, 0xef }, { 1, 5, 0x45 }, { 2, 0, 0xcdef }, { 2, 6, 0x0123 },
    { 4, 0, 0x89abcdef }, { 4, 4, 0x01234567 }, { 8, 0, 0x0123456789abcdefULL }
};
/* Little-endian system, swap set, 32-bit device: each word reversed. */
static const PioRead le_word_swapped[7] = {
    { 1, 0, 0x89 }, { 1, 5, 0x23 }, { 2, 0, 0xab89 }, { 2, 6, 0x6745 },
    { 4, 0, 0xefcdab89 }, { 4, 4, 0x67452301 }, { 8, 0, 0x67452301efcdab89ULL }
};
/* Little-endian system, swap set, 64-bit device: the doubleword reversed. */
static const PioRead le_double_swapped[7] = {
    { 1, 0, 0x01 }, { 1, 5, 0xab }, { 2, 0, 0x2301 }, { 2, 6, 0xefcd },
    { 4, 0, 0x67452301 }, { 4, 4, 0xefcdab89 }, { 8, 0, 0xefcdab8967452301ULL }
};
/* Big-endian system, swap set: bytes stay in address order. */
static const PioRead be_ordered[7] = {
    { 1, 0, 0xef }, { 1, 5, 0x45 }, { 2, 0, 0xefcd }, { 2, 6, 0x2301 },
    { 4, 0, 0xefcdab89 }, { 4, 4, 0x67452301 }, { 8, 0, 0xefcdab8967452301ULL }
};
/* Big-endian system, swap clear, 32-bit device. */
static const PioRead be_word_swapped[7] = {
    { 1, 0, 0x89 }, { 1, 5, 0x23 }, { 2, 0, 0x89ab }, { 2, 6, 0x4567 },
    { 4, 0, 0x89abcdef }, { 4, 4, 0x01234567 }, { 8, 0, 0x89abcdef01234567ULL }
};
/* Big-endian system, swap clear, 64-bit device. */
static const PioRead be_double_swapped[7] = {
    { 1, 0, 0x01 }, { 1, 5, 0xab }, { 2, 0, 0x0123 }, { 2, 6, 0xcdef },
    { 4, 0, 0x01234567 }, { 4, 4, 0x89abcdef }, { 8, 0, 0x0123456789abcdefULL }
};

static void check_reads(const Rig *r, uint64_t window, const PioRead *rows,
                        unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        g_assert_cmphex(cpu_read(r, window + rows[i].offset, rows[i].size), ==,
                        rows[i].value);
    }
}

static void check_pio_row(bool le, bool swap, bool wide, const PioRead *rows)
{
    Rig r = rig_start_with_card(false);

    set_system_little_endian(&r, le);
    select_row(&r, swap, wide);
    check_reads(&r, CARD_WINDOW + CARD_BAR1 - CARD_BAR0 + WIDE_FIXED, rows, 7);
    qtest_quit(r.qts);
}

static void test_pio_little_endian(void)
{
    check_pio_row(true, false, false, le_ordered);
    check_pio_row(true, true, false, le_word_swapped);
    check_pio_row(true, false, true, le_ordered);
    check_pio_row(true, true, true, le_double_swapped);
}

/* Control bit 9 set is the existing behaviour, unchanged. */
static void test_pio_big_endian(void)
{
    check_pio_row(false, true, false, be_ordered);
    check_pio_row(false, false, false, be_word_swapped);
    check_pio_row(false, true, true, be_ordered);
    check_pio_row(false, false, true, be_double_swapped);
}

/*
 * The card's scratch bytes, read back one at a time in the one placement
 * that is order-neutral for a byte: little-endian system, swap clear.
 */
static void scratch_bytes(Rig *r, uint8_t out[8])
{
    bool was_le = r->le;
    bool was_swap, was_wide;
    uint32_t dev;

    dev = reg_read(r, BRIDGE_DEVICE(CARD_SLOT));
    was_swap = dev & DEV_SWAP;
    was_wide = dev & DEV_64BIT;
    set_system_little_endian(r, true);
    select_row(r, false, false);
    for (unsigned i = 0; i < 8; i++) {
        out[i] = cpu_read(r, CARD_WINDOW + CARD_BAR1 - CARD_BAR0 +
                             WIDE_SCRATCH + i, 1);
    }
    select_row(r, was_swap, was_wide);
    set_system_little_endian(r, was_le);
}

typedef struct PioWrite {
    bool le, swap, wide;
    unsigned size;
    unsigned offset;
    uint64_t value;
    uint8_t scratch[8];     /* the card's bytes afterwards, in PCI order */
} PioWrite;

static void test_pio_writes(void)
{
    static const PioWrite cases[] = {
        { true, false, false, 4, 8, 0x11223344,
          { 0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0 } },
        { true, true, false, 4, 8, 0x11223344,
          { 0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0 } },
        { true, true, false, 2, 14, 0xaabb,
          { 0, 0, 0, 0, 0xaa, 0xbb, 0, 0 } },
        { true, true, false, 1, 9, 0x5a,
          { 0, 0, 0x5a, 0, 0, 0, 0, 0 } },
        { true, true, false, 8, 8, 0x0102030405060708ULL,
          { 0x05, 0x06, 0x07, 0x08, 0x01, 0x02, 0x03, 0x04 } },
        { true, false, true, 8, 8, 0x0102030405060708ULL,
          { 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 } },
        { true, true, true, 8, 8, 0x0102030405060708ULL,
          { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 } },
        { true, true, true, 4, 12, 0x11223344,
          { 0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0 } },
        { false, false, false, 4, 8, 0x11223344,
          { 0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0 } },
        { false, true, false, 4, 8, 0x11223344,
          { 0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0 } },
        { false, false, true, 8, 8, 0x0102030405060708ULL,
          { 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 } },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        const PioWrite *c = &cases[i];
        Rig r = rig_start_with_card(false);
        uint8_t seen[8];

        set_system_little_endian(&r, c->le);
        select_row(&r, c->swap, c->wide);
        cpu_write(&r, CARD_WINDOW + CARD_BAR1 - CARD_BAR0 + c->offset, c->size,
                  c->value);
        scratch_bytes(&r, seen);
        g_test_message("case %u", i);
        g_assert_cmpmem(seen, 8, c->scratch, 8);
        qtest_quit(r.qts);
    }
}

/* The I/O window places bytes as the memory windows do. */
static void test_io_window(void)
{
    Rig r = rig_start_with_card(false);
    uint8_t seen[4];

    set_system_little_endian(&r, true);
    set_device(&r, IO_SLOT, DEV_SWAP, DEV_SWAP);
    cpu_write(&r, IO_WINDOW + CARD_IO_BAR, 4, 0xa1b2c3d4);

    set_device(&r, IO_SLOT, DEV_SWAP, 0);
    for (unsigned i = 0; i < 4; i++) {
        seen[i] = cpu_read(&r, IO_WINDOW + CARD_IO_BAR + i, 1);
    }
    g_assert_cmpmem(seen, 4, ((uint8_t[]){ 0xa1, 0xb2, 0xc3, 0xd4 }), 4);
    g_assert_cmphex(cpu_read(&r, IO_WINDOW + CARD_IO_BAR, 4), ==, 0xd4c3b2a1);

    set_system_little_endian(&r, false);
    g_assert_cmphex(cpu_read(&r, IO_WINDOW + CARD_IO_BAR, 4), ==, 0xd4c3b2a1);
    qtest_quit(r.qts);
}

/*
 * Slot 4's window reaches the card's I/O BAR and slot 5's its memory BAR:
 * the window's swap bit picks the row, not the card's slot's.
 */
static void test_foreign_windows(void)
{
    const uint64_t other_mem = WIDGET_BASE + 0x900000;
    Rig r = rig_start_with_card(false);

    set_system_little_endian(&r, true);
    set_device(&r, IO_SLOT, DEV_SWAP | DEV_64BIT, 0);
    cpu_write(&r, IO_WINDOW + CARD_IO_BAR, 8, 0x0123456789abcdefULL);
    set_device(&r, 5, 0x1fffffff, DEV_MEMORY_SPACE | 0x130);

    for (unsigned swap = 0; swap < 2; swap++) {
        set_device(&r, CARD_SLOT, DEV_SWAP, swap ? 0 : DEV_SWAP);
        set_device(&r, IO_SLOT, DEV_SWAP, swap ? DEV_SWAP : 0);
        set_device(&r, 5, DEV_SWAP, swap ? DEV_SWAP : 0);
        check_reads(&r, IO_WINDOW + CARD_IO_BAR,
                    swap ? le_word_swapped : le_ordered, 7);
        check_reads(&r, other_mem + CARD_BAR1 - CARD_BAR0,
                    swap ? le_word_swapped : le_ordered, 7);
    }
    qtest_quit(r.qts);
}

/*
 * An ISP1040 has little-endian registers and a signature in its mailboxes,
 * 0x4953 at 0x72 to a little-endian processor.
 */
static void test_isp1040(void)
{
    const uint64_t cfg = CFG0_BASE + 2 * 0x1000;
    const uint64_t window = WIDGET_BASE + 0x600000;
    Rig r;

    r.qts = qtest_init("-M none -cpu R10000 "
                       "-device sgi-xio-test-target,id=transfer-ram,size=16M "
                       "-device sgi-bridge,id=bridge,board-part=030-1244-001,"
                       "fabric=/machine/unattached/system[0] "
                       "-device qlogic-isp1040,bus=pci,addr=2");
    r.le = false;
    qtest_sysbus_mmio_map(r.qts, "/machine/peripheral/bridge", 0, WIDGET_BASE);
    qtest_writel(r.qts, cfg + PCI_BASE_ADDRESS_1, 0x11000000);
    qtest_writel(r.qts, cfg + PCI_COMMAND, 2);
    set_device(&r, 2, 0x1fffffff, DEV_MEMORY_SPACE | 0x110);

    g_assert_cmphex(cpu_read(&r, window + 0x70, 2), ==, 0x4953);
    g_assert_cmphex(cpu_read(&r, window + 0x72, 2), ==, 0x0000);

    set_system_little_endian(&r, true);
    g_assert_cmphex(cpu_read(&r, window + 0x72, 2), ==, 0x4953);
    g_assert_cmphex(cpu_read(&r, window + 0x74, 2), ==, 0x5020);
    g_assert_cmphex(cpu_read(&r, window + 0x76, 2), ==, 0x2020);
    g_assert_cmphex(cpu_read(&r, window + 0x78, 2), ==, 0x0001);
    g_assert_cmphex(cpu_read(&r, window + 0x72, 4), ==, 0x50204953);
    qtest_quit(r.qts);
}

/*
 * The 0x40000000 range follows control bit 22 as a window follows its swap
 * bit; bit 23 and the device's own bit change nothing.
 */
static void test_translated_range(void)
{
    static const struct {
        bool le;
        uint32_t control;
        unsigned size;
        uint64_t value;
    } cases[] = {
        { true, 0, 4, 0x89abcdef },
        { true, CONTROL_MEM_SWAP, 4, 0xefcdab89 },
        { true, 0, 8, 0x0123456789abcdefULL },
        { true, CONTROL_MEM_SWAP, 8, 0x67452301efcdab89ULL },
        { false, 0, 4, 0x89abcdef },
        { false, CONTROL_MEM_SWAP, 4, 0xefcdab89 },
    };
    uint64_t range = XLATE_BASE + CARD_BAR1;
    uint64_t window = CARD_WINDOW + CARD_BAR1 - CARD_BAR0;
    Rig r = rig_start_with_card(false);

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        set_system_little_endian(&r, cases[i].le);
        set_control(&r, CONTROL_MEM_SWAP | CONTROL_IO_SWAP, cases[i].control);
        g_assert_cmphex(cpu_read(&r, range, cases[i].size), ==,
                        cases[i].value);
        /* Bit 23, and the device's own swap bit, leave the range alone. */
        set_control(&r, 0, CONTROL_IO_SWAP);
        set_device(&r, CARD_SLOT, DEV_SWAP, DEV_SWAP);
        g_assert_cmphex(cpu_read(&r, range, cases[i].size), ==,
                        cases[i].value);
        set_device(&r, CARD_SLOT, DEV_SWAP, 0);
    }

    /* And the control bits do not reach a device window. */
    set_system_little_endian(&r, true);
    set_control(&r, CONTROL_IO_SWAP, CONTROL_MEM_SWAP);
    g_assert_cmphex(cpu_read(&r, window, 4), ==, 0x89abcdef);
    qtest_quit(r.qts);
}

/*
 * A configuration word is its natural value at its own offset, either swap
 * bit; a big-endian system aims narrow accesses at the other half.
 */
static void test_config_reads(void)
{
    static const PioRead le_rows[] = {
        { 4, 0x00, 0x56781234 }, { 2, 0x00, 0x1234 }, { 2, 0x02, 0x5678 },
        { 1, 0x00, 0x34 }, { 1, 0x01, 0x12 }, { 1, 0x02, 0x78 },
        { 1, 0x03, 0x56 }, { 4, 0x10, CARD_BAR0 }, { 2, 0x04, 7 },
    };
    static const PioRead be_rows[] = {
        { 4, 0x00, 0x56781234 }, { 2, 0x02, 0x1234 }, { 2, 0x00, 0x5678 },
        { 1, 0x03, 0x34 }, { 1, 0x02, 0x12 }, { 1, 0x01, 0x78 },
        { 1, 0x00, 0x56 }, { 4, 0x10, CARD_BAR0 }, { 2, 0x06, 7 },
    };
    uint64_t cfg = CFG0_BASE + CARD_SLOT * 0x1000;
    Rig r = rig_start_with_card(false);

    for (unsigned swap = 0; swap < 2; swap++) {
        set_device(&r, CARD_SLOT, DEV_SWAP, swap ? DEV_SWAP : 0);
        set_system_little_endian(&r, true);
        check_reads(&r, cfg, le_rows, ARRAY_SIZE(le_rows));
        set_system_little_endian(&r, false);
        check_reads(&r, cfg, be_rows, ARRAY_SIZE(be_rows));
    }
    qtest_quit(r.qts);
}

/* Each write is checked from the other order, so a pair of errors cancels. */
static void test_config_writes(void)
{
    uint64_t cfg = CFG0_BASE + CARD_SLOT * 0x1000;
    Rig r = rig_start_with_card(false);

    set_system_little_endian(&r, true);
    cpu_write(&r, cfg + PCI_COMMAND, 2, 0x0006);
    cpu_write(&r, cfg + PCI_INTERRUPT_LINE, 1, 0x5a);
    cpu_write(&r, cfg + PCI_BASE_ADDRESS_0, 4, 0x14000000);

    set_system_little_endian(&r, false);
    g_assert_cmphex(cpu_read(&r, cfg + (PCI_COMMAND ^ 2), 2), ==, 0x0006);
    g_assert_cmphex(cpu_read(&r, cfg + (PCI_INTERRUPT_LINE ^ 3), 1), ==, 0x5a);
    g_assert_cmphex(cpu_read(&r, cfg + PCI_BASE_ADDRESS_0, 4), ==, 0x14000000);
    qtest_quit(r.qts);
}

/*
 * Sixteen bytes 40 to 4f in PCI order, and where they land: in address
 * order, word-reversed and doubleword-reversed.
 */
static const uint8_t pattern_ordered[16] = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
};
static const uint8_t pattern_word[16] = {
    0x43, 0x42, 0x41, 0x40, 0x47, 0x46, 0x45, 0x44,
    0x4b, 0x4a, 0x49, 0x48, 0x4f, 0x4e, 0x4d, 0x4c,
};
static const uint8_t pattern_double[16] = {
    0x47, 0x46, 0x45, 0x44, 0x43, 0x42, 0x41, 0x40,
    0x4f, 0x4e, 0x4d, 0x4c, 0x4b, 0x4a, 0x49, 0x48,
};

#define SRC_PAGE        (TRANSFER_RAM + 0x1000)
#define DST_PAGE        (TRANSFER_RAM + 0x3000)
#define ATE_INDEX       5

static void xfer(const Rig *r, unsigned reg, uint32_t value)
{
    cpu_write(r, CARD_WINDOW + reg, 4, value);
}

/* The card fills sixteen bytes at a PCI address; memory is read raw. */
static void check_dma_write(const Rig *r, uint64_t pci, const uint8_t *want)
{
    uint8_t seen[16];

    qtest_memset(r->qts, SRC_PAGE, 0xee, 16);
    xfer(r, XFER_ADDRESS, pci);
    xfer(r, XFER_LENGTH, 16);
    xfer(r, XFER_SEED, 0x40);
    xfer(r, XFER_GO, XFER_GO_FILL);
    qtest_memread(r->qts, SRC_PAGE, seen, 16);
    g_assert_cmpmem(seen, 16, want, 16);
}

/* The card reads memory through a route and stores it in local space. */
static void check_dma_read(const Rig *r, uint64_t pci, const uint8_t *want)
{
    uint8_t seen[16];

    qtest_memwrite(r->qts, SRC_PAGE, pattern_ordered, 16);
    qtest_memset(r->qts, DST_PAGE, 0xee, 16);
    xfer(r, XFER_SOURCE, pci);
    xfer(r, XFER_ADDRESS, DST_PAGE);
    xfer(r, XFER_LENGTH, 16);
    xfer(r, XFER_GO, XFER_GO_COPY);
    qtest_memread(r->qts, DST_PAGE, seen, 16);
    g_assert_cmpmem(seen, 16, want, 16);
}

static void dma_modes(bool wide)
{
    Rig r = rig_start_with_card(wide);
    const uint8_t *swapped = wide ? pattern_double : pattern_word;
    uint64_t routes[] = {
        PCI_DIRECT | SRC_PAGE,
        PCI_TRANSLATED + ATE_INDEX * 0x1000,
    };

    reg_write(&r, BRIDGE_DIRECT_MAP, 0);
    qtest_writeq(r.qts, ATE_BASE + ATE_INDEX * 8, SRC_PAGE | 1);

    for (unsigned le = 0; le < 2; le++) {
        set_system_little_endian(&r, le);
        for (unsigned route = 0; route < 2; route++) {
            for (unsigned set = 0; set < 2; set++) {
                /* Bytes stay in order when the bit equals control bit 9. */
                bool ordered = set == !le;

                set_device(&r, CARD_SLOT, DEV_SWAP_TRANSLATED | DEV_SWAP_DIRECT,
                           set ? (route ? DEV_SWAP_TRANSLATED :
                                          DEV_SWAP_DIRECT) : 0);
                g_test_message("le=%u route=%u set=%u", le, route, set);
                check_dma_write(&r, routes[route],
                                ordered ? pattern_ordered : swapped);
                check_dma_read(&r, routes[route],
                               ordered ? pattern_ordered : swapped);
            }
        }
    }
    qtest_quit(r.qts);
}

static void test_dma_32bit_slot(void)
{
    dma_modes(false);
}

static void test_dma_64bit_slot(void)
{
    dma_modes(true);
}

/* Local space has no swapper in either order. */
static void test_dma_local_space(void)
{
    Rig r = rig_start_with_card(false);

    set_system_little_endian(&r, true);
    set_device(&r, CARD_SLOT, 0, DEV_SWAP_TRANSLATED | DEV_SWAP_DIRECT);
    check_dma_write(&r, SRC_PAGE, pattern_ordered);
    qtest_quit(r.qts);
}

#define BRIDGE_IDENTIFICATION 0x000
#define BRIDGE_INT_STATUS 0x100
#define INT_INVALID_ADDR (1u << 24)
#define BRIDGE_INT_RESET 0x110
#define INT_RESET_REQ_DSP_GRP (1u << 3)

/*
 * Registers: with bit 9 clear the slot's lower word answers too, naturally
 * for a little-endian processor; the upper word answers as before.
 */
static void test_register_layout(void)
{
    Rig r = rig_start(false);
    uint32_t id = reg_read(&r, BRIDGE_IDENTIFICATION);
    uint32_t control = reg_read(&r, BRIDGE_CONTROL);

    g_assert_cmphex(control & CONTROL_BIG_ENDIAN, ==, CONTROL_BIG_ENDIAN);
    g_assert_cmphex(id & 1, ==, 1);
    /* The lower word of a slot reads as undefined: all ones, and latched. */
    g_assert_cmphex(qtest_readl(r.qts, WIDGET_BASE + BRIDGE_IDENTIFICATION),
                    ==, 0xffffffff);
    g_assert_cmphex(reg_read(&r, BRIDGE_INT_STATUS) & INT_INVALID_ADDR, ==,
                    INT_INVALID_ADDR);
    reg_write(&r, BRIDGE_INT_RESET, INT_RESET_REQ_DSP_GRP);

    set_system_little_endian(&r, true);
    g_assert_cmphex(reg_read(&r, BRIDGE_IDENTIFICATION), ==, id);
    g_assert_cmphex(reg_read(&r, BRIDGE_CONTROL), ==,
                    control & ~CONTROL_BIG_ENDIAN);
    g_assert_cmphex(reg_read(&r, BRIDGE_INT_STATUS) & INT_INVALID_ADDR, ==, 0);

    /* The upper word keeps its register: a big-endian read is natural. */
    g_assert_cmphex(qtest_readl(r.qts, WIDGET_BASE + BRIDGE_IDENTIFICATION + 4),
                    ==, id);
    g_assert_cmphex(le_read(r.qts, WIDGET_BASE + BRIDGE_IDENTIFICATION + 4, 4),
                    ==, bswap32(id));

    /* A write at the lower word lands, as the other order sees it. */
    reg_write(&r, BRIDGE_DEVICE(5), 0x18001234);
    qtest_writel(r.qts, WIDGET_BASE + BRIDGE_DEVICE(6) + 4, 0x18001235);
    g_assert_cmphex(reg_read(&r, BRIDGE_DEVICE(6)), ==, 0x18001235);
    set_system_little_endian(&r, false);
    g_assert_cmphex(reg_read(&r, BRIDGE_DEVICE(5)), ==, 0x18001234);
    qtest_quit(r.qts);
}

/* A little-endian processor reaches the bit at the reset layout, swapped. */
static void test_processor_clears_the_bit(void)
{
    Rig r = rig_start(false);
    uint32_t control = reg_read(&r, BRIDGE_CONTROL);

    r.le = true;
    g_assert_cmphex(le_read(r.qts, WIDGET_BASE + BRIDGE_CONTROL + 4, 4), ==,
                    bswap32(control));
    le_write(r.qts, WIDGET_BASE + BRIDGE_CONTROL + 4, 4,
             bswap32(control & ~CONTROL_BIG_ENDIAN));
    g_assert_cmphex(reg_read(&r, BRIDGE_CONTROL), ==,
                    control & ~CONTROL_BIG_ENDIAN);
    qtest_quit(r.qts);
}

/* A reset puts the system back to big-endian, as the bit's reset value says. */
static void test_reset_restores_big_endian(void)
{
    Rig r = rig_start(false);

    set_system_little_endian(&r, true);
    qtest_qmp_assert_success(r.qts, "{'execute': 'system_reset'}");
    r.le = false;
    g_assert_cmphex(reg_read(&r, BRIDGE_CONTROL) & CONTROL_BIG_ENDIAN, ==,
                    CONTROL_BIG_ENDIAN);
    qtest_quit(r.qts);
}

/* The translation table keeps its big-endian arrangement in both modes. */
static void test_translation_table_stays_big_endian(void)
{
    uint64_t entry = 0xabcd000000000000ULL | SRC_PAGE | 1;
    uint64_t at = ATE_BASE + ATE_INDEX * 8;
    Rig r = rig_start_with_card(false);

    set_system_little_endian(&r, true);
    le_write(r.qts, at, 8, entry);
    g_assert_cmphex(qtest_readl(r.qts, at + 4), ==, bswap32((uint32_t)entry));

    le_write(r.qts, at, 8, bswap64(entry));
    g_assert_cmphex(qtest_readl(r.qts, at + 4), ==, entry >> 32);
    check_dma_write(&r, PCI_TRANSLATED + ATE_INDEX * 0x1000, pattern_ordered);
    qtest_quit(r.qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sgi-bridge-le/pio/little-endian", test_pio_little_endian);
    qtest_add_func("/sgi-bridge-le/pio/big-endian", test_pio_big_endian);
    qtest_add_func("/sgi-bridge-le/pio/writes", test_pio_writes);
    qtest_add_func("/sgi-bridge-le/pio/io-window", test_io_window);
    qtest_add_func("/sgi-bridge-le/pio/foreign-windows", test_foreign_windows);
    qtest_add_func("/sgi-bridge-le/pio/isp1040", test_isp1040);
    qtest_add_func("/sgi-bridge-le/pio/translated-range",
                   test_translated_range);

    qtest_add_func("/sgi-bridge-le/config/reads", test_config_reads);
    qtest_add_func("/sgi-bridge-le/config/writes", test_config_writes);

    qtest_add_func("/sgi-bridge-le/dma/32bit-slot", test_dma_32bit_slot);
    qtest_add_func("/sgi-bridge-le/dma/64bit-slot", test_dma_64bit_slot);
    qtest_add_func("/sgi-bridge-le/dma/local-space", test_dma_local_space);
    qtest_add_func("/sgi-bridge-le/dma/translation-table",
                   test_translation_table_stays_big_endian);

    qtest_add_func("/sgi-bridge-le/registers/layout", test_register_layout);
    qtest_add_func("/sgi-bridge-le/registers/processor-clears-the-bit",
                   test_processor_clears_the_bit);
    qtest_add_func("/sgi-bridge-le/registers/reset",
                   test_reset_restores_big_endian);

    return g_test_run();
}
