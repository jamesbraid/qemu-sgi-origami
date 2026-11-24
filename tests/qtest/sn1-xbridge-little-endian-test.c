/*
 * The XBridge head with control bit 9 clear; a little-endian processor is
 * memread and memwrite decoded by hand. Values are from tables 117 to 132.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/pci/pci_regs.h"
#include "libqtest.h"

#define BASE            0x10000000ULL
#define TARGET_A        0x20000000ULL
#define SRC             0x8000
#define DST             0xc000
#define MEM32           0x40000000ULL
#define PCI_IO_RANGE    0x100000000ULL

#define XB_CONTROL      0x020
#define XB_DIRECT_MAP   0x080
#define XB_INT_STATUS   0x100
#define XB_INT_RESET    0x110
#define XB_DEVICE(n)    (0x200 + 8 * (n))
#define CONTROL_BIG_ENDIAN (1u << 9)
#define CONTROL_MEM_SWAP (1u << 22)
#define CONTROL_IO_SWAP (1u << 23)
#define DEV_MEMORY_SPACE (1u << 12)
#define DEV_SWAP        (1u << 13)
#define DEV_SWAP_DIRECT (1u << 19)
#define DEV_64BIT       (1u << 22)
#define INT_INVALID_ADDR (1u << 24)
#define INT_RESET_INVALID_ADDR (1u << 3)

#define CFG0            (BASE + 0x20000)
#define ATE_BASE        (BASE + 0x10000)

/* The card is in slot 0 and its I/O BAR is reached through slot 1's window. */
#define CARD_WINDOW     (BASE + 0x200000)
#define IO_WINDOW       (BASE + 0x400000)
#define CARD_BAR0       0x13000000u
#define CARD_BAR1       0x13000100u
#define CARD_IO_BAR     0x100u
#define WIDE            (CARD_BAR1 - CARD_BAR0)

/* The card's DMA engine in BAR0, and its 64-bit constant and scratch. */
#define XFER_ADDRESS    0x00
#define XFER_LENGTH     0x04
#define XFER_SEED       0x08
#define XFER_GO         0x0c
#define XFER_SOURCE     0x14
#define XFER_ADDRESS_HI 0x18
#define XFER_SOURCE_HI  0x1c
#define XFER_GO_FILL    1
#define XFER_GO_COPY    2
#define WIDE_FIXED      0x00
#define WIDE_SCRATCH    0x08

typedef struct Rig {
    QTestState *qts;
    bool le;            /* the system order the test last selected */
} Rig;

/* A card on a slot wired to the 64-bit data path, or not. */
static Rig rig_start(bool wide)
{
    Rig r = { 0 };

    r.qts = qtest_initf("-M none -cpu R10000 -S "
        "-device sgi-xio-test-target,id=targeta "
        "-device sgi-xbridge-head,id=head,req64-slots=%d,"
        "target-0=/machine/peripheral/targeta/targeta.ram[0],"
        "target-a=/machine/peripheral/targeta/targeta.ram[0] "
        "-device %s,id=card,bus=head.0,addr=0",
        wide ? 1 : 0,
        wide ? "sgi-bridge-dma-test-64bit" : "sgi-bridge-dma-test");
    qtest_sysbus_mmio_map(r.qts, "/machine/peripheral/head", 0, BASE);
    qtest_sysbus_mmio_map(r.qts, "/machine/peripheral/targeta", 0, TARGET_A);
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
    return cpu_read(r, BASE + offset + (r->le ? 0 : 4), 4);
}

static void reg_write(const Rig *r, unsigned offset, uint32_t value)
{
    cpu_write(r, BASE + offset + (r->le ? 0 : 4), 4, value);
}

static void reg_update(const Rig *r, unsigned offset, uint32_t clear,
                       uint32_t set)
{
    reg_write(r, offset, (reg_read(r, offset) & ~clear) | set);
}

static void set_system_little_endian(Rig *r, bool le)
{
    reg_update(r, XB_CONTROL, CONTROL_BIG_ENDIAN, le ? 0 : CONTROL_BIG_ENDIAN);
    r->le = le;
}

static void set_device(const Rig *r, unsigned slot, uint32_t clear,
                       uint32_t set)
{
    reg_update(r, XB_DEVICE(slot), clear, set);
}

static void set_control(const Rig *r, uint32_t clear, uint32_t set)
{
    reg_update(r, XB_CONTROL, clear, set);
}

/* The card's BARs at fixed PCI addresses, set up as a big-endian system. */
static Rig rig_start_with_card(bool wide)
{
    Rig r = rig_start(wide);

    qtest_writel(r.qts, CFG0 + PCI_BASE_ADDRESS_0, CARD_BAR0);
    qtest_writel(r.qts, CFG0 + PCI_BASE_ADDRESS_1, CARD_BAR1);
    qtest_writel(r.qts, CFG0 + PCI_BASE_ADDRESS_2, CARD_IO_BAR | 1);
    qtest_writel(r.qts, CFG0 + PCI_COMMAND, 7);
    set_device(&r, 0, 0x1fffffff, DEV_MEMORY_SPACE | 0x130);
    set_device(&r, 1, 0x1fffffff, 0);
    return r;
}

static void select_row(const Rig *r, bool swap, bool wide)
{
    set_device(r, 0, DEV_SWAP | DEV_64BIT,
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

/* Little-endian system, swap clear (tables 125 to 132): address order. */
static const PioRead le_ordered[7] = {
    { 1, 0, 0xef }, { 1, 5, 0x45 }, { 2, 0, 0xcdef }, { 2, 6, 0x0123 },
    { 4, 0, 0x89abcdef }, { 4, 4, 0x01234567 }, { 8, 0, 0x0123456789abcdefULL }
};
/* Little-endian system, swap set, 32-bit device (tables 117 to 120). */
static const PioRead le_word_swapped[7] = {
    { 1, 0, 0x89 }, { 1, 5, 0x23 }, { 2, 0, 0xab89 }, { 2, 6, 0x6745 },
    { 4, 0, 0xefcdab89 }, { 4, 4, 0x67452301 }, { 8, 0, 0x67452301efcdab89ULL }
};
/* Little-endian system, swap set, 64-bit device (tables 121 to 124). */
static const PioRead le_double_swapped[7] = {
    { 1, 0, 0x01 }, { 1, 5, 0xab }, { 2, 0, 0x2301 }, { 2, 6, 0xefcd },
    { 4, 0, 0x67452301 }, { 4, 4, 0xefcdab89 }, { 8, 0, 0xefcdab8967452301ULL }
};
/* Big-endian system, swap set: address order. */
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
        g_test_message("size %u offset %u", rows[i].size, rows[i].offset);
        g_assert_cmphex(cpu_read(r, window + rows[i].offset, rows[i].size), ==,
                        rows[i].value);
    }
}

static void check_pio_row(bool le, bool swap, bool wide, const PioRead *rows)
{
    Rig r = rig_start_with_card(false);

    set_system_little_endian(&r, le);
    select_row(&r, swap, wide);
    check_reads(&r, CARD_WINDOW + WIDE + WIDE_FIXED, rows, 7);
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

/* The card's scratch bytes, read back in the order-neutral placement. */
static void scratch_bytes(Rig *r, uint8_t out[8])
{
    bool was_le = r->le;
    bool was_swap = reg_read(r, XB_DEVICE(0)) & DEV_SWAP;
    bool was_wide = reg_read(r, XB_DEVICE(0)) & DEV_64BIT;

    set_system_little_endian(r, true);
    select_row(r, false, false);
    for (unsigned i = 0; i < 8; i++) {
        out[i] = cpu_read(r, CARD_WINDOW + WIDE + WIDE_SCRATCH + i, 1);
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
        cpu_write(&r, CARD_WINDOW + WIDE + c->offset, c->size, c->value);
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
    set_device(&r, 1, DEV_SWAP, DEV_SWAP);
    cpu_write(&r, IO_WINDOW + CARD_IO_BAR, 4, 0xa1b2c3d4);

    set_device(&r, 1, DEV_SWAP, 0);
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
 * The 32-bit memory alias follows control bit 22 and the I/O range bit 23,
 * as a window follows its swap bit; the device's own bit changes nothing.
 */
static void test_ranges(void)
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
    Rig r = rig_start_with_card(false);
    uint64_t alias = MEM32 + CARD_BAR1;

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        set_system_little_endian(&r, cases[i].le);
        set_control(&r, CONTROL_MEM_SWAP | CONTROL_IO_SWAP, cases[i].control);
        g_assert_cmphex(cpu_read(&r, BASE + alias, cases[i].size), ==,
                        cases[i].value);
        /* Bit 23, and the device's own swap bit, leave the alias alone. */
        set_control(&r, 0, CONTROL_IO_SWAP);
        set_device(&r, 0, DEV_SWAP, DEV_SWAP);
        g_assert_cmphex(cpu_read(&r, BASE + alias, cases[i].size), ==,
                        cases[i].value);
        set_device(&r, 0, DEV_SWAP, 0);
    }

    /* I/O space: bit 23 alone, and bit 22 reaches no window. */
    set_system_little_endian(&r, true);
    set_control(&r, CONTROL_MEM_SWAP | CONTROL_IO_SWAP, 0);
    cpu_write(&r, BASE + PCI_IO_RANGE + CARD_IO_BAR, 4, 0xa1b2c3d4);
    set_control(&r, 0, CONTROL_MEM_SWAP);
    g_assert_cmphex(cpu_read(&r, BASE + PCI_IO_RANGE + CARD_IO_BAR, 4), ==,
                    0xa1b2c3d4);
    g_assert_cmphex(cpu_read(&r, CARD_WINDOW + WIDE, 4), ==, 0x89abcdef);
    set_control(&r, CONTROL_MEM_SWAP, CONTROL_IO_SWAP);
    g_assert_cmphex(cpu_read(&r, BASE + PCI_IO_RANGE + CARD_IO_BAR, 4), ==,
                    0xd4c3b2a1);
    qtest_quit(r.qts);
}

/* Natural words at their own offsets, either swap bit; card 1b36:0005. */
static void test_config_reads(void)
{
    static const PioRead le_rows[] = {
        { 4, 0x00, 0x00051b36 }, { 2, 0x00, 0x1b36 }, { 2, 0x02, 0x0005 },
        { 1, 0x00, 0x36 }, { 1, 0x01, 0x1b }, { 1, 0x02, 0x05 },
        { 1, 0x03, 0x00 }, { 4, 0x10, CARD_BAR0 }, { 2, 0x04, 7 },
        { 8, 0x10, 0x1300010013000000ULL },
    };
    static const PioRead be_rows[] = {
        { 4, 0x00, 0x00051b36 }, { 2, 0x02, 0x1b36 }, { 2, 0x00, 0x0005 },
        { 1, 0x03, 0x36 }, { 1, 0x02, 0x1b }, { 1, 0x01, 0x05 },
        { 1, 0x00, 0x00 }, { 4, 0x10, CARD_BAR0 }, { 2, 0x06, 7 },
        { 8, 0x10, 0x1300000013000100ULL },
    };
    Rig r = rig_start_with_card(false);

    for (unsigned swap = 0; swap < 2; swap++) {
        set_device(&r, 0, DEV_SWAP, swap ? DEV_SWAP : 0);
        set_system_little_endian(&r, true);
        check_reads(&r, CFG0, le_rows, ARRAY_SIZE(le_rows));
        set_system_little_endian(&r, false);
        /* The doubleword row is the lower address's word in the upper half. */
        check_reads(&r, CFG0, be_rows, ARRAY_SIZE(be_rows));
    }
    qtest_quit(r.qts);
}

/* Each write is checked from the other order, so a pair of errors cancels. */
static void test_config_writes(void)
{
    Rig r = rig_start_with_card(false);

    set_system_little_endian(&r, true);
    cpu_write(&r, CFG0 + PCI_COMMAND, 2, 0x0006);
    cpu_write(&r, CFG0 + PCI_INTERRUPT_LINE, 1, 0x5a);
    cpu_write(&r, CFG0 + PCI_BASE_ADDRESS_0, 4, 0x14000000);
    cpu_write(&r, CFG0 + PCI_BASE_ADDRESS_1, 8, 0x1500010016000200ULL);

    set_system_little_endian(&r, false);
    g_assert_cmphex(cpu_read(&r, CFG0 + (PCI_COMMAND ^ 2), 2), ==, 0x0006);
    g_assert_cmphex(cpu_read(&r, CFG0 + (PCI_INTERRUPT_LINE ^ 3), 1), ==,
                    0x5a);
    g_assert_cmphex(cpu_read(&r, CFG0 + PCI_BASE_ADDRESS_0, 4), ==,
                    0x14000000);
    g_assert_cmphex(cpu_read(&r, CFG0 + PCI_BASE_ADDRESS_1, 4), ==,
                    0x16000200);
    g_assert_cmphex(cpu_read(&r, CFG0 + PCI_BASE_ADDRESS_2, 4), ==,
                    0x15000101);
    qtest_quit(r.qts);
}

/*
 * Registers: with bit 9 clear the slot's lower word answers too, naturally
 * for a little-endian processor; the upper word answers as before.
 */
static void test_register_layout(void)
{
    Rig r = rig_start(false);
    uint32_t id = reg_read(&r, 0);
    uint32_t control = reg_read(&r, XB_CONTROL);

    g_assert_cmphex(id, ==, 0x2d002049);
    g_assert_cmphex(control & CONTROL_BIG_ENDIAN, ==, CONTROL_BIG_ENDIAN);
    /* The lower word of a slot is undefined to a big-endian system. */
    g_assert_cmphex(qtest_readl(r.qts, BASE), ==, 0);
    g_assert_cmphex(reg_read(&r, XB_INT_STATUS) & INT_INVALID_ADDR, ==,
                    INT_INVALID_ADDR);
    reg_write(&r, XB_INT_RESET, INT_RESET_INVALID_ADDR);

    set_system_little_endian(&r, true);
    g_assert_cmphex(reg_read(&r, 0), ==, id);
    g_assert_cmphex(reg_read(&r, XB_CONTROL), ==,
                    control & ~CONTROL_BIG_ENDIAN);
    g_assert_cmphex(reg_read(&r, XB_INT_STATUS) & INT_INVALID_ADDR, ==, 0);

    /* The upper word keeps its register: a big-endian read is natural. */
    g_assert_cmphex(qtest_readl(r.qts, BASE + 4), ==, id);
    g_assert_cmphex(le_read(r.qts, BASE + 4, 4), ==, bswap32(id));

    /* A write at the lower word lands, as the other order sees it. */
    reg_write(&r, XB_DEVICE(5), 0x18001234);
    qtest_writel(r.qts, BASE + XB_DEVICE(6) + 4, 0x18001235);
    g_assert_cmphex(reg_read(&r, XB_DEVICE(6)), ==, 0x18001235);
    set_system_little_endian(&r, false);
    g_assert_cmphex(reg_read(&r, XB_DEVICE(5)), ==, 0x18001234);
    qtest_quit(r.qts);
}

/* A little-endian processor reaches the bit at the reset layout, swapped. */
static void test_processor_clears_the_bit(void)
{
    Rig r = rig_start(false);
    uint32_t control = reg_read(&r, XB_CONTROL);

    r.le = true;
    g_assert_cmphex(le_read(r.qts, BASE + XB_CONTROL + 4, 4), ==,
                    bswap32(control));
    le_write(r.qts, BASE + XB_CONTROL + 4, 4,
             bswap32(control & ~CONTROL_BIG_ENDIAN));
    g_assert_cmphex(reg_read(&r, XB_CONTROL), ==,
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
    g_assert_cmphex(reg_read(&r, XB_CONTROL) & CONTROL_BIG_ENDIAN, ==,
                    CONTROL_BIG_ENDIAN);
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

static void xfer(const Rig *r, unsigned reg, uint32_t value)
{
    cpu_write(r, CARD_WINDOW + reg, 4, value);
}

static void xfer_address(const Rig *r, unsigned reg, unsigned reg_hi,
                         uint64_t pci)
{
    xfer(r, reg, pci);
    xfer(r, reg_hi, pci >> 32);
}

/* The card fills sixteen bytes at a PCI address; memory is read raw. */
static void check_dma_write(const Rig *r, uint64_t pci, const uint8_t *want)
{
    uint8_t seen[16];

    qtest_memset(r->qts, TARGET_A + SRC, 0xee, 16);
    xfer_address(r, XFER_ADDRESS, XFER_ADDRESS_HI, pci);
    xfer(r, XFER_LENGTH, 16);
    xfer(r, XFER_SEED, 0x40);
    xfer(r, XFER_GO, XFER_GO_FILL);
    qtest_memread(r->qts, TARGET_A + SRC, seen, 16);
    g_assert_cmpmem(seen, 16, want, 16);
}

/* Read through the route, write in order: the copy shows the route. */
static void check_dma_read(const Rig *r, uint64_t pci, const uint8_t *want)
{
    uint64_t in_order = 0xa000000000000000ULL | DST |
                        (r->le ? 0 : 1ULL << 55);
    uint8_t seen[16];

    qtest_memwrite(r->qts, TARGET_A + SRC, pattern_ordered, 16);
    qtest_memset(r->qts, TARGET_A + DST, 0xee, 16);
    xfer_address(r, XFER_SOURCE, XFER_SOURCE_HI, pci);
    xfer_address(r, XFER_ADDRESS, XFER_ADDRESS_HI, in_order);
    xfer(r, XFER_LENGTH, 16);
    xfer(r, XFER_GO, XFER_GO_COPY);
    qtest_memread(r->qts, TARGET_A + DST, seen, 16);
    g_assert_cmpmem(seen, 16, want, 16);
}

#define ATE_INDEX 8

static void dma_modes(bool wide)
{
    Rig r = rig_start_with_card(wide);
    const uint8_t *swapped = wide ? pattern_double : pattern_word;

    reg_write(&r, XB_DIRECT_MAP, 0xa00000);
    for (unsigned le = 0; le < 2; le++) {
        set_system_little_endian(&r, le);
        for (unsigned route = 0; route < 3; route++) {
            for (unsigned set = 0; set < 2; set++) {
                /* Bytes stay in order when the bit equals control bit 9. */
                bool ordered = set == !le;
                uint64_t pci;

                set_device(&r, 0, DEV_SWAP_DIRECT, 0);
                qtest_writeq(r.qts, ATE_BASE + ATE_INDEX * 8,
                             0x8a01 | (route == 1 && set ? 1 << 5 : 0));
                if (route == 0) {
                    pci = 0x80000000u | SRC;
                    set_device(&r, 0, DEV_SWAP_DIRECT,
                               set ? DEV_SWAP_DIRECT : 0);
                } else if (route == 1) {
                    pci = 0x40000000u | SRC;
                } else {
                    pci = 0xa000000000000000ULL | SRC |
                          (set ? 1ULL << 55 : 0);
                }
                g_test_message("le=%u route=%u set=%u", le, route, set);
                check_dma_write(&r, pci, ordered ? pattern_ordered : swapped);
                check_dma_read(&r, pci, ordered ? pattern_ordered : swapped);
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

/*
 * The translation table keeps its big-endian arrangement in both modes: a
 * little-endian processor stores an entry byte-reversed.
 */
static void test_translation_table_stays_big_endian(void)
{
    uint64_t entry = 0x0000abcd00008a01ULL;
    uint64_t at = ATE_BASE + ATE_INDEX * 8;
    Rig r = rig_start_with_card(false);

    set_system_little_endian(&r, true);
    le_write(r.qts, at, 8, bswap64(entry));
    g_assert_cmphex(qtest_readl(r.qts, at + 4), ==, entry >> 32);

    reg_write(&r, XB_DIRECT_MAP, 0xa00000);
    le_write(r.qts, at, 8, bswap64(0x8a01));
    check_dma_write(&r, 0x40000000u | SRC, pattern_ordered);
    qtest_quit(r.qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn1/xbridge-le/pio-little-endian", test_pio_little_endian);
    qtest_add_func("/sn1/xbridge-le/pio-big-endian", test_pio_big_endian);
    qtest_add_func("/sn1/xbridge-le/pio-writes", test_pio_writes);
    qtest_add_func("/sn1/xbridge-le/io-window", test_io_window);
    qtest_add_func("/sn1/xbridge-le/ranges", test_ranges);
    qtest_add_func("/sn1/xbridge-le/config-reads", test_config_reads);
    qtest_add_func("/sn1/xbridge-le/config-writes", test_config_writes);
    qtest_add_func("/sn1/xbridge-le/register-layout", test_register_layout);
    qtest_add_func("/sn1/xbridge-le/processor-clears-the-bit",
                   test_processor_clears_the_bit);
    qtest_add_func("/sn1/xbridge-le/reset-restores-big-endian",
                   test_reset_restores_big_endian);
    qtest_add_func("/sn1/xbridge-le/dma-32bit-slot", test_dma_32bit_slot);
    qtest_add_func("/sn1/xbridge-le/dma-64bit-slot", test_dma_64bit_slot);
    qtest_add_func("/sn1/xbridge-le/translation-table",
                   test_translation_table_stays_big_endian);
    return g_test_run();
}
