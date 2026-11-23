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

typedef struct Rig {
    QTestState *qts;
    bool le;            /* the system order the test last selected */
} Rig;

static Rig rig_start(void)
{
    Rig r = { 0 };

    r.qts = qtest_init("-M none -cpu R10000 "
                       "-device sgi-xio-test-target,id=transfer-ram,size=16M "
                       "-device sgi-bridge,id=bridge,board-part=030-1244-001,"
                       "fabric=/machine/unattached/system[0] "
                       "-device sgi-bridge-dma-test,bus=pci,addr=3");
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

/* A register is the low word of its slot: the slot's offset plus four. */
static uint32_t reg_read(const Rig *r, unsigned offset)
{
    return qtest_readl(r->qts, WIDGET_BASE + offset + 4);
}

static void reg_write(const Rig *r, unsigned offset, uint32_t value)
{
    qtest_writel(r->qts, WIDGET_BASE + offset + 4, value);
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
static Rig rig_start_with_card(void)
{
    Rig r = rig_start();
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
    Rig r = rig_start_with_card();

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
        Rig r = rig_start_with_card();
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
    Rig r = rig_start_with_card();
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
    Rig r = rig_start_with_card();

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
    Rig r = rig_start_with_card();

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

    return g_test_run();
}
