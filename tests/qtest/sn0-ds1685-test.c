/*
 * DS1685/DS1687 RTC register tests through IOC3's indirect byte bus:
 * address/data ports replace the multiplexed bus. Wrong index latching
 * breaks all reads; wrong whole-file banking breaks the 40h-7Fh checks.
 * No current machine carries this RTC. Origin 200 IP29 uses a Timekeeper
 * on the first byte-bus select; tests add an Octane-identified controller
 * and drive its RTC instead.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

/* Crosstalk space and the bridge that carries both controllers. */
#define CONSOLE_WIDGET  8
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)CONSOLE_WIDGET << 24))

#define BRIDGE_CFG0     0x020000
#define CFG0_DEVICE_STRIDE 0x1000
#define CFG0(device)    (BRIDGE_BASE + BRIDGE_CFG0 + \
                         (device) * CFG0_DEVICE_STRIDE)
#define REG_DATA        4         /* registers sit at a slot's upper half */
#define BRIDGE_DEVICE(n) (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)

/*
 * The machine's own controller sits at device 2. This one takes device 3,
 * whose bridge window is the megabyte above it. The first two slots get two
 * megabytes each and the rest one, so the bases are not evenly spaced.
 */
#define OCTANE_PCI_DEVICE 3
#define OCTANE_WINDOW    0x700000
#define OCTANE_PCI_MMIO  0x10100000u
#define OCTANE_BASE      (BRIDGE_BASE + OCTANE_WINDOW)

/* The two byte-bus selects this part answers on, inside the chip's window. */
#define BYTEBUS_DEV1    (OCTANE_BASE + 0xa0000)   /* address port */
#define BYTEBUS_DEV2    (OCTANE_BASE + 0xc0000)   /* data port */

/* The Timekeeper's select, which this board must leave empty. */
#define BYTEBUS_DEV0    (OCTANE_BASE + 0x80000)

#define SUBSYS_IP29     "0xc303"   /* the Origin 200's board */
#define SUBSYS_IP30     "0xc304"   /* the Octane's, the one with this clock */

#define OCTANE_IOC3(subsys) \
    "-M origin200 -device sgi-ioc3,id=octane,addr=" \
    G_STRINGIFY(OCTANE_PCI_DEVICE) ",subsystem-id=" subsys

/* Register numbers, written to the address port rather than to an address. */
#define R_SECONDS       0x00
#define R_SECONDS_ALARM 0x01
#define R_MINUTES       0x02
#define R_MINUTES_ALARM 0x03
#define R_HOURS         0x04
#define R_HOURS_ALARM   0x05
#define R_WEEKDAY       0x06
#define R_DAY           0x07
#define R_MONTH         0x08
#define R_YEAR          0x09
#define R_CTRL_A        0x0a
#define R_CTRL_B        0x0b
#define R_CTRL_C        0x0c
#define R_CTRL_D        0x0d
#define R_RAM_LOW       0x0e    /* first of the fifty common bytes */
#define R_MODEL         0x40    /* bank 1; bank 0 has user RAM here */
#define R_CENTURY       0x48
#define R_DATE_ALARM    0x49
#define R_EXT_CTRL_4A   0x4a
#define R_EXT_CTRL_4B   0x4b
#define R_EXTRAM_ADDR   0x50
#define R_EXTRAM_DATA   0x53

#define A_UIP           0x80
#define A_DV2           0x40
#define A_DV1           0x20
#define A_DV0           0x10    /* the bank bit */

#define B_SET           0x80
#define B_PIE           0x40
#define B_AIE           0x20
#define B_UIE           0x10
#define B_DM            0x04

#define C_IRQF          0x80
#define C_PF            0x40
#define C_AF            0x20
#define C_UF            0x10

#define D_VRT           0x80

#define X4A_VRT2        0x80
#define X4A_INCR        0x40
#define X4A_RESERVED    0x30
#define X4A_PAB         0x08
#define X4A_RF          0x04
#define X4A_WF          0x02
#define X4A_KF          0x01

#define X4B_E32K        0x40
#define X4B_RIE         0x04
#define X4B_WIE         0x02
#define X4B_KSE         0x01

#define MODEL_DS1685    0x71

/* A guest's bus scan for the second controller: BAR, command, window aim. */
static void octane_attach(QTestState *qts)
{
    qtest_writel(qts, CFG0(OCTANE_PCI_DEVICE) + PCI_BAR0, OCTANE_PCI_MMIO);
    qtest_writel(qts, CFG0(OCTANE_PCI_DEVICE) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(OCTANE_PCI_DEVICE),
                 DEV_MEMORY_SPACE | (OCTANE_PCI_MMIO >> 20));
}

static QTestState *octane_start(void)
{
    QTestState *qts = qtest_init(OCTANE_IOC3(SUBSYS_IP30));

    octane_attach(qts);
    return qts;
}

/* The two halves of every access, kept separate so tests can interleave. */
static void select_reg(QTestState *qts, uint8_t reg)
{
    qtest_writeb(qts, BYTEBUS_DEV1, reg);
}

static uint8_t data_read(QTestState *qts)
{
    return qtest_readb(qts, BYTEBUS_DEV2);
}

static void data_write(QTestState *qts, uint8_t val)
{
    qtest_writeb(qts, BYTEBUS_DEV2, val);
}

static uint8_t reg_read(QTestState *qts, uint8_t reg)
{
    select_reg(qts, reg);
    return data_read(qts);
}

static void reg_write(QTestState *qts, uint8_t reg, uint8_t val)
{
    select_reg(qts, reg);
    data_write(qts, val);
}

/*
 * Read-modify-write, the way both kernels move the bank bit. Writing Register
 * A blind would clobber the oscillator bits, and writing back a value that
 * had UIP set would set a read-only bit.
 */
static void select_bank(QTestState *qts, unsigned int bank)
{
    uint8_t a = reg_read(qts, R_CTRL_A);

    reg_write(qts, R_CTRL_A, bank ? (a | A_DV0) : (a & ~A_DV0));
}

/*
 * The address port latches, and nothing about touching the data port
 * disturbs it. Every other test in this file rides on this one.
 */
static void test_the_index_latch_persists(const void *data)
{
    QTestState *qts = octane_start();

    select_bank(qts, 0);

    /* Two reads of one register, with one index write between them: none. */
    select_reg(qts, R_RAM_LOW);
    data_write(qts, 0x5a);
    g_assert_cmphex(data_read(qts), ==, 0x5a);
    g_assert_cmphex(data_read(qts), ==, 0x5a);

    /* Two writes without re-selecting land in the same cell, not two. */
    data_write(qts, 0xa5);
    g_assert_cmphex(data_read(qts), ==, 0xa5);
    g_assert_cmphex(reg_read(qts, R_RAM_LOW + 1), ==, 0x00);

    qtest_quit(qts);
}

/* Seven address bits: bit 7 of a written index is not part of the number. */
static void test_the_index_is_seven_bits(const void *data)
{
    QTestState *qts = octane_start();

    select_bank(qts, 0);

    reg_write(qts, R_RAM_LOW + 2, 0x3c);
    g_assert_cmphex(reg_read(qts, 0x80 | (R_RAM_LOW + 2)), ==, 0x3c);

    /* Also test the file top, where an oversized mask shows. */
    reg_write(qts, 0x7f, 0xc3);
    g_assert_cmphex(reg_read(qts, 0xff), ==, 0xc3);

    qtest_quit(qts);
}

/*
 * Only 40h-7Fh changes meaning with the bank bit, so a byte
 * written into bank-0 user RAM at 4Ah must still be there after a trip
 * through bank 1, where 4Ah is the extended control register.
 */
static void test_banking_swaps_only_the_upper_half(const void *data)
{
    QTestState *qts = octane_start();

    select_bank(qts, 0);
    reg_write(qts, R_EXT_CTRL_4A, 0x5a);
    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A), ==, 0x5a);

    select_bank(qts, 1);
    /*
     * The extended control register, not the byte just written. VRT2 set and
     * the increment bit clear is what an idle part reads; 0x5a is neither.
     */
    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A) & X4A_VRT2, ==, X4A_VRT2);
    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A), !=, 0x5a);
    g_assert_cmphex(reg_read(qts, R_MODEL), ==, MODEL_DS1685);

    select_bank(qts, 0);
    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A), ==, 0x5a);
    g_assert_cmphex(reg_read(qts, R_MODEL), !=, MODEL_DS1685);

    qtest_quit(qts);
}

/* Bank changes leave addresses below 40h unchanged. */
static void test_the_low_half_is_common_to_both_banks(const void *data)
{
    QTestState *qts = octane_start();
    uint8_t a, b;

    select_bank(qts, 0);
    reg_write(qts, 0x20, 0x77);
    a = reg_read(qts, R_CTRL_A);
    b = reg_read(qts, R_CTRL_B);

    select_bank(qts, 1);
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0x77);
    /* A differs in the bank bit alone; B, C and D not at all. */
    g_assert_cmphex(reg_read(qts, R_CTRL_A) & ~A_DV0, ==, a & ~A_DV0);
    g_assert_cmphex(reg_read(qts, R_CTRL_B), ==, b);
    g_assert_cmphex(reg_read(qts, R_CTRL_D) & D_VRT, ==, D_VRT);

    /* Writing through bank 1 lands in the same cell bank 0 sees. */
    reg_write(qts, 0x20, 0x88);
    select_bank(qts, 0);
    g_assert_cmphex(reg_read(qts, 0x20), ==, 0x88);

    qtest_quit(qts);
}

/*
 * OpenBSD's power-button path reaches 4Ah with no bank management of its own,
 * because the clock routines left the part in bank 1. A model that quietly
 * restores bank 0 turns that read into a read of user RAM.
 */
static void test_the_bank_bit_survives_unrelated_accesses(const void *data)
{
    QTestState *qts = octane_start();

    select_bank(qts, 1);

    (void)reg_read(qts, R_SECONDS);
    reg_write(qts, R_RAM_LOW, 0x11);
    (void)reg_read(qts, R_CTRL_D);

    g_assert_cmphex(reg_read(qts, R_CTRL_A) & A_DV0, ==, A_DV0);
    g_assert_cmphex(reg_read(qts, R_MODEL), ==, MODEL_DS1685);

    qtest_quit(qts);
}

/*
 * Linux spins on this bit with no timeout, in probe and on every time read.
 * A model that answers unimplemented registers with all-ones wedges the guest
 * here, before any console message names the clock, which is why an assertion
 * this small is worth writing.
 */
static void test_the_increment_bit_reads_clear(const void *data)
{
    QTestState *qts = octane_start();
    uint8_t val;

    select_bank(qts, 1);

    val = reg_read(qts, R_EXT_CTRL_4A);
    /*
     * Prove the register is answering before reading anything into a clear
     * bit: an empty byte-bus select reads 0x00, which has the increment bit
     * clear too. Without this the assertion below passes on a hole.
     */
    g_assert_cmphex(val & X4A_VRT2, ==, X4A_VRT2);
    g_assert_cmphex(val & X4A_INCR, ==, 0);
    g_assert_cmphex(val, !=, 0xff);

    /* Linux's own order: SET first, then the bank, then the poll. */
    select_bank(qts, 0);
    reg_write(qts, R_CTRL_B, reg_read(qts, R_CTRL_B) | B_SET);
    select_bank(qts, 1);
    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A) & X4A_INCR, ==, 0);

    qtest_quit(qts);
}

/* Register C's three flags go away when they are read. */
static void test_register_c_clears_its_flags_on_read(const void *data)
{
    QTestState *qts = octane_start();

    select_bank(qts, 0);

    /*
     * The periodic tap is the only one of C's three sources a test can arm
     * without waiting a second, so drive the flag through the alarm instead:
     * an update-ended flag needs a second to pass. Use the extended path's
     * mirror image and assert the rule that matters: reading clears.
     * Register C is read-only, so the flag has to come from the part.
     */
    reg_write(qts, R_CTRL_A, (reg_read(qts, R_CTRL_A) & ~0x0f) | 0x0f);
    reg_write(qts, R_CTRL_B, reg_read(qts, R_CTRL_B) | B_PIE);
    /* Rate code 1111 is the slowest tap, 500 ms, so one step covers it. */
    qtest_clock_step(qts, 600 * 1000 * 1000);

    g_assert_cmphex(reg_read(qts, R_CTRL_C) & C_PF, ==, C_PF);
    /* Gone on the second read, without anything having written it. */
    g_assert_cmphex(reg_read(qts, R_CTRL_C) & C_PF, ==, 0);

    /* The low four bits of C never read as anything but zero. */
    g_assert_cmphex(reg_read(qts, R_CTRL_C) & 0x0f, ==, 0);

    qtest_quit(qts);
}

/*
 * The opposite rule, on the other flag register: 4Ah's three flags survive
 * any number of reads and go only when a zero is written over them. A model
 * that clears these on read loses interrupts; one that fails to clear C on
 * read storms. Both assertions are needed or one model passes both files.
 */
static void test_the_extended_flags_clear_only_by_writing_zero(const void *data)
{
    QTestState *qts = octane_start();
    const uint8_t flags[] = { X4A_RF, X4A_WF, X4A_KF };
    unsigned int i;

    select_bank(qts, 1);

    for (i = 0; i < ARRAY_SIZE(flags); i++) {
        uint8_t base = reg_read(qts, R_EXT_CTRL_4A) & ~(X4A_RF | X4A_WF |
                                                        X4A_KF);

        /* Software can set these, as the datasheet describes. */
        reg_write(qts, R_EXT_CTRL_4A, base | flags[i]);

        g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A) & flags[i], ==, flags[i]);
        g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A) & flags[i], ==, flags[i]);
        g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A) & flags[i], ==, flags[i]);

        reg_write(qts, R_EXT_CTRL_4A, base);
        g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A) & flags[i], ==, 0);
    }

    qtest_quit(qts);
}

/*
 * The line is level and IRQF is derived rather than latched: a flag with its
 * enable clear drives nothing, enabling it asserts with no new event, and
 * clearing the flag deasserts. The pin itself is a board wire this controller
 * does not carry. On an Octane the clock takes its own bridge interrupt,
 * so IRQF is the guest-visible face of the same signal.
 */
static void test_the_interrupt_is_level_and_derived(const void *data)
{
    QTestState *qts = octane_start();
    uint8_t base;

    select_bank(qts, 1);
    base = reg_read(qts, R_EXT_CTRL_4A) & ~(X4A_RF | X4A_WF | X4A_KF);

    /* A flag with no enable: recorded, but driving nothing. */
    reg_write(qts, R_EXT_CTRL_4B, reg_read(qts, R_EXT_CTRL_4B) & ~X4B_WIE);
    reg_write(qts, R_EXT_CTRL_4A, base | X4A_WF);
    select_bank(qts, 0);
    g_assert_cmphex(reg_read(qts, R_CTRL_C) & C_IRQF, ==, 0);

    /* Enabling an already-set flag asserts immediately, with no new event. */
    select_bank(qts, 1);
    reg_write(qts, R_EXT_CTRL_4B, reg_read(qts, R_EXT_CTRL_4B) | X4B_WIE);
    select_bank(qts, 0);
    g_assert_cmphex(reg_read(qts, R_CTRL_C) & C_IRQF, ==, C_IRQF);

    /*
     * And it stays asserted across a read of C, because the flag driving it
     * is not one of C's. A latched IRQF would drop here.
     */
    g_assert_cmphex(reg_read(qts, R_CTRL_C) & C_IRQF, ==, C_IRQF);

    select_bank(qts, 1);
    reg_write(qts, R_EXT_CTRL_4A, base);
    select_bank(qts, 0);
    g_assert_cmphex(reg_read(qts, R_CTRL_C) & C_IRQF, ==, 0);

    qtest_quit(qts);
}

/* Nothing on our paths reads the model byte, so only a test catches it. */
static void test_the_model_byte_identifies_the_part(const void *data)
{
    QTestState *qts = octane_start();

    select_bank(qts, 1);
    g_assert_cmphex(reg_read(qts, R_MODEL), ==, MODEL_DS1685);

    reg_write(qts, R_MODEL, 0x00);
    g_assert_cmphex(reg_read(qts, R_MODEL), ==, MODEL_DS1685);

    /* Both cells report a healthy battery, as the model does. */
    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A) & X4A_VRT2, ==, X4A_VRT2);
    select_bank(qts, 0);
    g_assert_cmphex(reg_read(qts, R_CTRL_D) & D_VRT, ==, D_VRT);

    qtest_quit(qts);
}

/* The four read-only exceptions the vendor lists together. */
static void test_read_only_bits_refuse_writes(const void *data)
{
    QTestState *qts = octane_start();
    uint8_t a;

    select_bank(qts, 0);

    /* Register A bit 7 is the update-in-progress bit and is not writable. */
    a = reg_read(qts, R_CTRL_A);
    reg_write(qts, R_CTRL_A, a | A_UIP);
    g_assert_cmphex(reg_read(qts, R_CTRL_A) & A_UIP, ==, 0);

    /* Registers C and D are read-only entire. */
    reg_write(qts, R_CTRL_C, 0xff);
    g_assert_cmphex(reg_read(qts, R_CTRL_C) & 0x0f, ==, 0);
    reg_write(qts, R_CTRL_D, 0xff);
    g_assert_cmphex(reg_read(qts, R_CTRL_D), ==, D_VRT);

    /* Bit 7 of the seconds byte, the one exception among the time bytes. */
    reg_write(qts, R_CTRL_B, reg_read(qts, R_CTRL_B) | B_SET);
    reg_write(qts, R_SECONDS, 0xff);
    g_assert_cmphex(reg_read(qts, R_SECONDS) & 0x80, ==, 0);

    qtest_quit(qts);
}

/*
 * Everywhere else in the time and date registers a fixed-zero bit takes a
 * written 1 that a later update may overwrite. This model stores it rather
 * than masking it, so the assertion is that the byte reads back whole. SET is
 * raised so no update can intervene and make the answer depend on timing.
 */
static void test_fixed_zero_bits_hold_a_written_one(const void *data)
{
    QTestState *qts = octane_start();

    select_bank(qts, 0);
    reg_write(qts, R_CTRL_B, reg_read(qts, R_CTRL_B) | B_SET);

    reg_write(qts, R_WEEKDAY, 0xff);
    g_assert_cmphex(reg_read(qts, R_WEEKDAY), ==, 0xff);

    reg_write(qts, R_MONTH, 0xff);
    g_assert_cmphex(reg_read(qts, R_MONTH), ==, 0xff);

    qtest_quit(qts);
}

/* Reserved bank-1 locations read zero and swallow writes. */
static void test_reserved_locations_read_zero(const void *data)
{
    QTestState *qts = octane_start();
    const uint8_t reserved[] = { 0x4c, 0x4d, 0x51, 0x52, 0x54, 0x60, 0x7f };
    unsigned int i;

    select_bank(qts, 1);

    for (i = 0; i < ARRAY_SIZE(reserved); i++) {
        g_assert_cmphex(reg_read(qts, reserved[i]), ==, 0x00);
        reg_write(qts, reserved[i], 0xff);
        g_assert_cmphex(reg_read(qts, reserved[i]), ==, 0x00);
    }

    /* The two reserved bits of 4Ah are the opposite case: held, and inert. */
    reg_write(qts, R_EXT_CTRL_4A,
              reg_read(qts, R_EXT_CTRL_4A) | X4A_RESERVED);
    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A) & X4A_RESERVED, ==,
                    X4A_RESERVED);
    g_assert_cmphex(reg_read(qts, R_MODEL), ==, MODEL_DS1685);

    qtest_quit(qts);
}

/*
 * The extended RAM address does not advance on this member of the family, so
 * a driver has to rewrite it before every byte, and Linux does.
 */
static void test_the_extended_ram_address_does_not_advance(const void *data)
{
    QTestState *qts = octane_start();

    select_bank(qts, 1);

    reg_write(qts, R_EXTRAM_ADDR, 0x00);
    reg_write(qts, R_EXTRAM_DATA, 0x11);
    reg_write(qts, R_EXTRAM_DATA, 0x22);    /* same cell, not the next one */

    reg_write(qts, R_EXTRAM_ADDR, 0x7f);
    reg_write(qts, R_EXTRAM_DATA, 0x33);

    reg_write(qts, R_EXTRAM_ADDR, 0x00);
    g_assert_cmphex(reg_read(qts, R_EXTRAM_DATA), ==, 0x22);
    g_assert_cmphex(reg_read(qts, R_EXTRAM_DATA), ==, 0x22);

    reg_write(qts, R_EXTRAM_ADDR, 0x01);
    g_assert_cmphex(reg_read(qts, R_EXTRAM_DATA), ==, 0x00);

    reg_write(qts, R_EXTRAM_ADDR, 0x7f);
    g_assert_cmphex(reg_read(qts, R_EXTRAM_DATA), ==, 0x33);

    qtest_quit(qts);
}

/* Three regions, 50 bytes, 64 bytes and 128 bytes, none aliasing another. */
static void test_the_three_ram_regions_are_distinct(const void *data)
{
    QTestState *qts = octane_start();

    select_bank(qts, 0);
    reg_write(qts, R_RAM_LOW, 0xaa);        /* the common fifty */
    reg_write(qts, 0x40, 0xbb);             /* bank 0's sixty-four */

    select_bank(qts, 1);
    reg_write(qts, R_EXTRAM_ADDR, 0x00);
    reg_write(qts, R_EXTRAM_DATA, 0xcc);    /* the extended one-two-eight */

    g_assert_cmphex(reg_read(qts, R_RAM_LOW), ==, 0xaa);
    g_assert_cmphex(reg_read(qts, R_EXTRAM_DATA), ==, 0xcc);

    select_bank(qts, 0);
    g_assert_cmphex(reg_read(qts, R_RAM_LOW), ==, 0xaa);
    g_assert_cmphex(reg_read(qts, 0x40), ==, 0xbb);

    qtest_quit(qts);
}

/*
 * A reset is VCC arriving at a part whose cell never went away. Everything
 * the battery holds survives it: the register file, all three RAM regions,
 * the bank bit and the time.
 */
static void test_a_reset_keeps_the_battery_backed_state(const void *data)
{
    QTestState *qts = octane_start();
    uint8_t seconds, minutes;

    select_bank(qts, 0);
    reg_write(qts, R_RAM_LOW, 0xa1);
    reg_write(qts, 0x40, 0xb2);

    /* A time of our own, so what survives is not merely the host clock. */
    reg_write(qts, R_CTRL_B, (reg_read(qts, R_CTRL_B) | B_SET) & ~B_DM);
    reg_write(qts, R_SECONDS, 0x45);
    reg_write(qts, R_MINUTES, 0x21);
    reg_write(qts, R_HOURS, 0x09);
    reg_write(qts, R_DAY, 0x07);
    reg_write(qts, R_MONTH, 0x03);
    reg_write(qts, R_YEAR, 0x40);

    select_bank(qts, 1);
    reg_write(qts, R_CENTURY, 0x20);
    reg_write(qts, R_EXTRAM_ADDR, 0x10);
    reg_write(qts, R_EXTRAM_DATA, 0xc3);
    seconds = reg_read(qts, R_SECONDS);
    minutes = reg_read(qts, R_MINUTES);

    qtest_system_reset(qts);
    /* The reset takes the bridge aim and the BAR; re-aim as a guest would. */
    octane_attach(qts);

    /* Bank 1 still, because the bank bit is battery-backed state too. */
    g_assert_cmphex(reg_read(qts, R_CTRL_A) & A_DV0, ==, A_DV0);
    g_assert_cmphex(reg_read(qts, R_CENTURY), ==, 0x20);
    reg_write(qts, R_EXTRAM_ADDR, 0x10);
    g_assert_cmphex(reg_read(qts, R_EXTRAM_DATA), ==, 0xc3);
    g_assert_cmphex(reg_read(qts, R_SECONDS), ==, seconds);
    g_assert_cmphex(reg_read(qts, R_MINUTES), ==, minutes);

    select_bank(qts, 0);
    g_assert_cmphex(reg_read(qts, R_RAM_LOW), ==, 0xa1);
    g_assert_cmphex(reg_read(qts, 0x40), ==, 0xb2);

    qtest_quit(qts);
}

/*
 * And the other side of the same rule: a reset applies the four values the
 * vendor documents for a power-on, and only those.
 */
static void test_a_reset_restores_the_documented_power_up_bits(const void *data)
{
    QTestState *qts = octane_start();
    uint8_t a;

    select_bank(qts, 1);

    /*
     * Move the two documented values a guest can hold away from their
     * power-on state. The power-active bit is the third and cannot be
     * pre-set here: a guest that writes it powers the machine off, which is
     * the contract the shutdown test asserts, so this one only requires it
     * to read clear afterwards.
     */
    reg_write(qts, R_EXT_CTRL_4B, reg_read(qts, R_EXT_CTRL_4B) & ~X4B_E32K);
    a = reg_read(qts, R_CTRL_A);
    reg_write(qts, R_CTRL_A, (a & ~A_DV1) | A_DV2);

    g_assert_cmphex(reg_read(qts, R_CTRL_A) & A_DV1, ==, 0);
    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4B) & X4B_E32K, ==, 0);

    /* Something that must survive, so this is not merely a fresh device. */
    reg_write(qts, R_EXTRAM_ADDR, 0x40);
    reg_write(qts, R_EXTRAM_DATA, 0x9c);

    qtest_system_reset(qts);
    octane_attach(qts);

    g_assert_cmphex(reg_read(qts, R_CTRL_A) & A_DV1, ==, A_DV1);
    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4B) & X4B_E32K, ==, X4B_E32K);
    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A) & X4A_PAB, ==, 0);
    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A) & X4A_INCR, ==, 0);

    /* The reset applied those and nothing else: the RAM is still there. */
    reg_write(qts, R_EXTRAM_ADDR, 0x40);
    g_assert_cmphex(reg_read(qts, R_EXTRAM_DATA), ==, 0x9c);

    /*
     * SET clears the update-in-progress bit, so this reads 0 whatever the
     * moment. An assertion on a timing window would flake.
     */
    select_bank(qts, 0);
    reg_write(qts, R_CTRL_B, reg_read(qts, R_CTRL_B) | B_SET);
    g_assert_cmphex(reg_read(qts, R_CTRL_A) & A_UIP, ==, 0);

    qtest_quit(qts);
}

/*
 * Setting the power-active bit is how a guest turns the machine off. Linux
 * writes it and spins forever, so a model that merely stores it leaves the
 * guest wedged in its own power-off path.
 */
static void test_the_power_active_bit_requests_a_shutdown(const void *data)
{
    QTestState *qts = octane_start();
    uint8_t base;

    select_bank(qts, 1);

    /*
     * Hardware clears the bit again whenever an enabled wake-up or kickstart
     * is asking the supply back on. Clear both flags first, as
     * what Linux's power-off path does before it writes the bit.
     */
    base = reg_read(qts, R_EXT_CTRL_4A) & ~(X4A_RF | X4A_WF | X4A_KF);
    reg_write(qts, R_EXT_CTRL_4A, base);
    reg_write(qts, R_EXT_CTRL_4A, base | X4A_PAB);

    qtest_qmp_eventwait(qts, "SHUTDOWN");

    qtest_quit(qts);
}

/*
 * The same bit does not power the machine down while an enabled wake-up
 * alarm is asking it back on. The part clears the bit instead. This is the
 * one place the documented side effect is guest-visible, and a model that
 * skips it would power off where the hardware would not.
 */
static void test_an_enabled_wake_up_flag_clears_the_power_bit(const void *data)
{
    QTestState *qts = octane_start();
    uint8_t base;

    select_bank(qts, 1);
    base = reg_read(qts, R_EXT_CTRL_4A) & ~(X4A_RF | X4A_WF | X4A_KF);

    reg_write(qts, R_EXT_CTRL_4B, reg_read(qts, R_EXT_CTRL_4B) | X4B_WIE);
    reg_write(qts, R_EXT_CTRL_4A, base | X4A_WF | X4A_PAB);

    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A) & X4A_PAB, ==, 0);
    g_assert_cmphex(reg_read(qts, R_EXT_CTRL_4A) & X4A_WF, ==, X4A_WF);

    qtest_quit(qts);
}

/*
 * The Origin 200's own board fits a Timekeeper on the first select and no
 * clock on these two. If this part ever appeared there, the machine we build
 * would have grown a device, which is the thing the object-graph test exists
 * to prevent.
 */
static void test_the_origin_200_board_has_no_ds1685(const void *data)
{
    QTestState *qts = qtest_init(OCTANE_IOC3(SUBSYS_IP29));

    octane_attach(qts);

    /* The Timekeeper answers where it always did. */
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x0100, 0x5a);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x0100), ==, 0x5a);

    /* And the two selects this part would answer on are holes. */
    qtest_writeb(qts, BYTEBUS_DEV1, R_MODEL);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV2), !=, MODEL_DS1685);

    qtest_quit(qts);
}

/* And the mirror: the Octane's board has this clock and no Timekeeper. */
static void test_the_octane_board_has_no_timekeeper(const void *data)
{
    QTestState *qts = octane_start();

    select_bank(qts, 1);
    g_assert_cmphex(reg_read(qts, R_MODEL), ==, MODEL_DS1685);

    /* Nothing answers where the Origin 200's clock lives. */
    qtest_writeb(qts, BYTEBUS_DEV0 + 0x0100, 0x5a);
    g_assert_cmphex(qtest_readb(qts, BYTEBUS_DEV0 + 0x0100), !=, 0x5a);

    qtest_quit(qts);
}

/*
 * State that survives a power cycle has to survive a migration. The board
 * without this clock is migrated in the same test, because the migration
 * format is gated per population and a stream that walks a device the
 * receiving board never realized is a crash rather than a mismatch.
 */
static void migrate_one_board(const char *subsys, bool has_clock)
{
    g_autofree char *uri = NULL;
    g_autofree char *dir = NULL;
    g_autofree char *args = NULL;
    QTestState *from, *to;

    dir = g_dir_make_tmp("sn0-ds1685-XXXXXX", NULL);
    g_assert_nonnull(dir);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);

    args = g_strdup_printf("%s -device sgi-ioc3,id=octane,addr=%d,"
                           "subsystem-id=%s",
                           "-M origin200",
                           OCTANE_PCI_DEVICE, subsys);

    from = qtest_init(args);
    octane_attach(from);
    if (has_clock) {
        select_bank(from, 1);
        reg_write(from, R_EXTRAM_ADDR, 0x20);
        reg_write(from, R_EXTRAM_DATA, 0x5a);
        select_bank(from, 0);
        reg_write(from, R_RAM_LOW, 0xa5);
    }

    to = qtest_initf("%s -incoming %s", args, uri);

    sgi_migrate_stopped(from, uri);
    qtest_quit(from);

    octane_attach(to);
    if (has_clock) {
        /* The bank bit crossed too, so no bank is selected before reading. */
        g_assert_cmphex(reg_read(to, R_RAM_LOW), ==, 0xa5);
        select_bank(to, 1);
        reg_write(to, R_EXTRAM_ADDR, 0x20);
        g_assert_cmphex(reg_read(to, R_EXTRAM_DATA), ==, 0x5a);
        g_assert_cmphex(reg_read(to, R_MODEL), ==, MODEL_DS1685);
    }
    qtest_quit(to);

    g_rmdir(dir);
}

static void test_the_clock_survives_a_migration(const void *data)
{
    migrate_one_board(SUBSYS_IP30, true);
}

static void test_a_board_without_the_clock_still_migrates(const void *data)
{
    migrate_one_board(SUBSYS_IP29, false);
}


int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sn0/ds1685/index-latch-persists", NULL,
                        test_the_index_latch_persists);
    qtest_add_data_func("/sn0/ds1685/index-is-seven-bits", NULL,
                        test_the_index_is_seven_bits);
    qtest_add_data_func("/sn0/ds1685/banking-swaps-only-the-upper-half", NULL,
                        test_banking_swaps_only_the_upper_half);
    qtest_add_data_func("/sn0/ds1685/low-half-is-common", NULL,
                        test_the_low_half_is_common_to_both_banks);
    qtest_add_data_func("/sn0/ds1685/bank-bit-survives", NULL,
                        test_the_bank_bit_survives_unrelated_accesses);
    qtest_add_data_func("/sn0/ds1685/increment-bit-reads-clear", NULL,
                        test_the_increment_bit_reads_clear);
    qtest_add_data_func("/sn0/ds1685/register-c-clears-on-read", NULL,
                        test_register_c_clears_its_flags_on_read);
    qtest_add_data_func("/sn0/ds1685/extended-flags-clear-by-write", NULL,
                        test_the_extended_flags_clear_only_by_writing_zero);
    qtest_add_data_func("/sn0/ds1685/interrupt-is-level-and-derived", NULL,
                        test_the_interrupt_is_level_and_derived);
    qtest_add_data_func("/sn0/ds1685/model-byte", NULL,
                        test_the_model_byte_identifies_the_part);
    qtest_add_data_func("/sn0/ds1685/read-only-bits", NULL,
                        test_read_only_bits_refuse_writes);
    qtest_add_data_func("/sn0/ds1685/fixed-zero-bits-hold-a-one", NULL,
                        test_fixed_zero_bits_hold_a_written_one);
    qtest_add_data_func("/sn0/ds1685/reserved-locations-read-zero", NULL,
                        test_reserved_locations_read_zero);
    qtest_add_data_func("/sn0/ds1685/extended-ram-does-not-advance", NULL,
                        test_the_extended_ram_address_does_not_advance);
    qtest_add_data_func("/sn0/ds1685/three-ram-regions-are-distinct", NULL,
                        test_the_three_ram_regions_are_distinct);
    qtest_add_data_func("/sn0/ds1685/reset-keeps-battery-backed-state", NULL,
                        test_a_reset_keeps_the_battery_backed_state);
    qtest_add_data_func("/sn0/ds1685/reset-restores-power-up-bits", NULL,
                        test_a_reset_restores_the_documented_power_up_bits);
    qtest_add_data_func("/sn0/ds1685/power-active-bit-shuts-down", NULL,
                        test_the_power_active_bit_requests_a_shutdown);
    qtest_add_data_func("/sn0/ds1685/wake-up-flag-clears-the-power-bit", NULL,
                        test_an_enabled_wake_up_flag_clears_the_power_bit);
    qtest_add_data_func("/sn0/ds1685/origin-200-board-has-none", NULL,
                        test_the_origin_200_board_has_no_ds1685);
    qtest_add_data_func("/sn0/ds1685/octane-board-has-no-timekeeper", NULL,
                        test_the_octane_board_has_no_timekeeper);
    qtest_add_data_func("/sn0/ds1685/survives-a-migration", NULL,
                        test_the_clock_survives_a_migration);
    qtest_add_data_func("/sn0/ds1685/board-without-it-still-migrates", NULL,
                        test_a_board_without_the_clock_still_migrates);

    return g_test_run();
}
