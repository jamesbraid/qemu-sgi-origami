/*
 * DS1685/DS1687 RTC register tests through IOC3's indirect byte bus:
 * address/data ports replace the multiplexed bus. Wrong index latching
 * breaks all reads; wrong whole-file banking breaks the 40h-7Fh checks.
 * No current machine carries this RTC. Origin 200 IP29 uses a Timekeeper
 * on the first byte-bus select; tests add an Octane-identified controller
 * and drive its RTC instead.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include "sgi-launch.h"
#include "sgi-test.h"

/* I/O space, and the bridge that carries both controllers. */
#define CONSOLE_WIDGET  8
#define BRIDGE_BASE     (SPACE_IO + ((uint64_t)CONSOLE_WIDGET << 24))

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
#define B_24_12         0x02

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

static void test_initial_calendar_and_mode_writes(const void *data)
{
    QTestState *qts = qtest_init(OCTANE_IOC3(SUBSYS_IP30)
                                 " -rtc base=2024-12-31T23:45:17,clock=vm");

    octane_attach(qts);
    g_assert_cmphex(reg_read(qts, R_CTRL_B), ==, B_DM | B_24_12);
    g_assert_cmphex(reg_read(qts, R_SECONDS), ==, 17);
    g_assert_cmphex(reg_read(qts, R_MINUTES), ==, 45);
    g_assert_cmphex(reg_read(qts, R_HOURS), ==, 23);
    g_assert_cmphex(reg_read(qts, R_DAY), ==, 31);
    g_assert_cmphex(reg_read(qts, R_MONTH), ==, 12);
    g_assert_cmphex(reg_read(qts, R_YEAR), ==, 24);
    select_bank(qts, 1);
    g_assert_cmphex(reg_read(qts, R_CENTURY), ==, 20);

    /* Format bits do not convert stored calendar bytes. */
    reg_write(qts, R_CTRL_B, B_SET | B_DM);
    g_assert_cmphex(reg_read(qts, R_HOURS), ==, 23);
    reg_write(qts, R_CTRL_B, B_SET | B_DM | B_24_12);
    g_assert_cmphex(reg_read(qts, R_HOURS), ==, 23);
    reg_write(qts, R_CTRL_B, B_SET | B_24_12);
    g_assert_cmphex(reg_read(qts, R_HOURS), ==, 23);
    g_assert_cmphex(reg_read(qts, R_DAY), ==, 31);
    reg_write(qts, R_HOURS, 0x11);
    reg_write(qts, R_DAY, 0x25);
    reg_write(qts, R_CTRL_B, B_SET | B_DM | B_24_12);
    g_assert_cmphex(reg_read(qts, R_HOURS), ==, 0x11);
    g_assert_cmphex(reg_read(qts, R_DAY), ==, 0x25);
    qtest_quit(qts);
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


static void test_volatile_counter_hold(const void *data)
{
    QTestState *qts = qtest_init(OCTANE_IOC3(SUBSYS_IP30)
                                " -rtc base=2024-01-01T00:00:00,clock=vm");
    uint8_t a;

    octane_attach(qts);
    qtest_clock_step(qts, 2 * 1000000000LL);
    g_assert_cmpuint(reg_read(qts, R_SECONDS), ==, 2);
    a = reg_read(qts, R_CTRL_A);
    reg_write(qts, R_CTRL_A, a & ~A_DV1);
    qtest_clock_step(qts, 10 * 1000000000LL);
    g_assert_cmpuint(reg_read(qts, R_SECONDS), ==, 2);
    reg_write(qts, R_CTRL_A, a);
    qtest_clock_step(qts, 1000000000LL);
    g_assert_cmpuint(reg_read(qts, R_SECONDS), ==, 3);
    qtest_quit(qts);
}


static QTestState *start_with_store(const char *store, const char *clock,
                                   const char *base)
{
    g_autofree char *clock_args = clock ?
        g_strdup_printf(" -drive if=none,id=rtc-store-clock,file=%s,format=raw",
                        clock) : g_strdup("");
    QTestState *qts = qtest_initf(OCTANE_IOC3(SUBSYS_IP30)
        ",ds1685-drive=rtc-store -rtc base=%s,clock=vm"
        " -drive if=none,id=rtc-store,file=%s,format=raw"
        "%s", base, store, clock_args);

    octane_attach(qts);
    return qts;
}

static void create_store(const char *store, const char *clock)
{
    char image[512] = { 0 };
    char reference[24] = { 0 };

    g_assert_true(g_file_set_contents(store, image, sizeof(image), NULL));
    if (clock) {
        g_assert_true(g_file_set_contents(clock, reference,
                                          sizeof(reference), NULL));
    }
}

static void set_calendar_2025(QTestState *qts)
{
    reg_write(qts, R_CTRL_B, B_SET | B_DM | B_24_12);
    reg_write(qts, R_SECONDS, 10);
    reg_write(qts, R_MINUTES, 0);
    reg_write(qts, R_HOURS, 0);
    reg_write(qts, R_WEEKDAY, 4);
    reg_write(qts, R_DAY, 1);
    reg_write(qts, R_MONTH, 1);
    reg_write(qts, R_YEAR, 25);
    select_bank(qts, 1);
    reg_write(qts, R_CENTURY, 20);
    reg_write(qts, R_CTRL_B, B_DM | B_24_12);
}

static void test_storage_process_restart(const void *data)
{
    g_autofree char *dir = g_dir_make_tmp("ds1685-store-XXXXXX", NULL);
    g_autofree char *store = g_build_filename(dir, "rtc.raw", NULL);
    g_autofree char *clock = g_build_filename(dir, "rtc.clock", NULL);
    g_autofree char *image = NULL;
    g_autofree char *reference = NULL;
    gsize len;
    QTestState *qts;

    create_store(store, clock);
    qts = start_with_store(store, clock, "2024-01-01T00:00:00");
    set_calendar_2025(qts);
    reg_write(qts, R_RAM_LOW, 0xa5);
    select_bank(qts, 0);
    reg_write(qts, 0x40, 0x5a);
    select_bank(qts, 1);
    reg_write(qts, R_EXTRAM_ADDR, 0x7f);
    reg_write(qts, R_EXTRAM_DATA, 0x3c);
    qtest_clock_step(qts, 2 * 1000000000LL);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(store, &image, &len, NULL));
    g_assert_cmpuint(len, ==, 512);
    g_assert_cmphex((uint8_t)image[0x0e], ==, 0xa5);
    g_assert_cmphex((uint8_t)image[0x40], ==, 0x5a);
    g_assert_cmphex((uint8_t)image[0x13f], ==, 0x3c);
    g_assert_true(g_file_get_contents(clock, &reference, &len, NULL));
    g_assert_cmpuint(len, ==, 24);
    g_assert_cmpmem(reference, 8, "DS168RF1", 8);
    g_assert_cmpint(ldq_le_p(reference + 8), ==, 1704067202);
    g_assert_cmpint(ldq_le_p(reference + 16), ==, 1735689612);

    qts = start_with_store(store, clock, "2024-01-01T00:00:12");
    g_assert_cmpuint(reg_read(qts, R_SECONDS), ==, 22);
    g_assert_cmpuint(reg_read(qts, R_YEAR), ==, 25);
    g_assert_cmphex(reg_read(qts, R_RAM_LOW), ==, 0xa5);
    g_assert_cmphex(reg_read(qts, R_CTRL_A) & A_DV0, ==, A_DV0);
    g_assert_cmphex(reg_read(qts, R_EXTRAM_DATA), ==, 0x3c);
    qtest_system_reset(qts);
    octane_attach(qts);
    g_assert_cmpuint(reg_read(qts, R_SECONDS), ==, 22);
    g_assert_cmphex(reg_read(qts, R_RAM_LOW), ==, 0xa5);
    qtest_quit(qts);
    g_unlink(store);
    g_unlink(clock);
    g_rmdir(dir);
}


static void test_storage_rejects_backend(const void *data)
{
    const char *kind = data;
    g_autofree char *dir = g_dir_make_tmp("ds1685-bad-XXXXXX", NULL);
    g_autofree char *store = g_build_filename(dir, "rtc.raw", NULL);
    g_autofree char *clock = g_build_filename(dir, "rtc.clock", NULL);
    g_autofree char *config = g_build_filename(dir, "blkdebug.conf", NULL);
    g_autofree char *drive = NULL;
    g_autofree char *rtc = NULL;
    g_autofree char *clock_drive = NULL;
    char image[1024] = { 0 };
    const char *message = "requires exactly 512";
    const char *args[] = { qtest_qemu_binary(NULL), "-M", "origin200",
        "-display", "none", "-qmp", "stdio", "-drive", NULL,
        "-device", NULL, "-accel", "qtest", "-drive", NULL, NULL };

    create_store(store, clock);
    if (!strcmp(kind, "empty") || !strcmp(kind, "oversize")) {
        g_assert_true(g_file_set_contents(store, image,
            !strcmp(kind, "empty") ? 0 : sizeof(image), NULL));
    }
    if (!strcmp(kind, "read-error") || !strcmp(kind, "clock-read-error")) {
        g_assert_true(g_file_set_contents(config,
            "[inject-error]\nevent = \"read_aio\"\n"
            "errno = \"5\"\nonce = \"on\"\n", -1, NULL));
        drive = g_strdup_printf("if=none,id=rtc-store,file=blkdebug:%s:%s,"
                                 "format=raw", config, store);
        if (!strcmp(kind, "clock-read-error")) {
            clock_drive = g_strdup_printf(
                "if=none,id=rtc-store-clock,file=blkdebug:%s:%s,format=raw",
                config, clock);
            g_clear_pointer(&drive, g_free);
            drive = g_strdup_printf(
                "if=none,id=rtc-store,file=%s,format=raw", store);
        }
        message = !strcmp(kind, "clock-read-error") ?
            "reading clock reference failed" : "reading NVRAM image failed";
    } else {
        drive = g_strdup_printf("if=none,id=rtc-store,file=%s,format=raw%s",
            store, !strcmp(kind, "readonly") ? ",readonly=on" : "");
        if (!strcmp(kind, "readonly")) {
            message = "read-only";
        }
    }
    rtc = g_strdup_printf("sgi-ioc3,addr=3,subsystem-id=%s,%s=rtc-store",
        !strcmp(kind, "wrong-board") ? SUBSYS_IP29 : SUBSYS_IP30,
        !strcmp(kind, "orphan") ? "ds1685-clock-drive" : "ds1685-drive");
    if (!strcmp(kind, "orphan")) {
        message = "clock-drive requires drive";
    } else if (!strcmp(kind, "wrong-board")) {
        message = "requires a board with a DS1685";
    }
    args[8] = drive;
    args[10] = rtc;
    if (!clock_drive) {
        clock_drive = g_strdup_printf(
            "if=none,id=rtc-store-clock,file=%s,format=raw", clock);
    }
    args[14] = clock_drive;
    assert_launch_rejected(args, message);
    g_unlink(store);
    g_unlink(clock);
    g_unlink(config);
    g_rmdir(dir);
}

static void test_storage_write_error(const void *data)
{
    const char *kind = data;
    g_autofree char *dir = NULL;
    g_autofree char *store = NULL;
    g_autofree char *clock = NULL;
    g_autofree char *config = NULL;
    g_autofree char *rules = NULL;

    if (g_test_subprocess()) {
        g_autofree char *fault = g_strdup_printf("blkdebug:%s:%s",
            g_getenv("DS1685_ERROR_CONFIG"),
            g_getenv("DS1685_ERROR_IMAGE"));
        QTestState *qts = start_with_store(
            strstr(kind, "main") ? fault : g_getenv("DS1685_MAIN"),
            strstr(kind, "main") ? g_getenv("DS1685_CLOCK") : fault,
            "2024-01-01T00:00:00");
        g_autofree char *image = NULL;
        g_autofree char *ref = NULL;
        gsize len;

        reg_write(qts, R_RAM_LOW, 0xa5);
        g_assert_cmphex(reg_read(qts, R_RAM_LOW), ==, 0xa5);
        g_assert_true(g_file_get_contents(g_getenv("DS1685_MAIN"),
                                          &image, &len, NULL));
        g_assert_cmphex((uint8_t)image[R_RAM_LOW], ==, 0xa5);
        g_assert_true(g_file_get_contents(g_getenv("DS1685_CLOCK"),
                                          &ref, &len, NULL));
        g_assert_cmpmem(ref, 8, "DS168RF1", 8);
        qtest_quit(qts);
        return;
    }
    dir = g_dir_make_tmp("ds1685-error-XXXXXX", NULL);
    store = g_build_filename(dir, "rtc.raw", NULL);
    clock = g_build_filename(dir, "rtc.clock", NULL);
    config = g_build_filename(dir, "blkdebug.conf", NULL);
    create_store(store, clock);
    rules = g_strdup_printf("[inject-error]\nevent = \"%s\"\n"
        "errno = \"5\"\nonce = \"on\"\n",
        strstr(kind, "write") ? "write_aio" : "flush_to_disk");
    g_assert_true(g_file_set_contents(config, rules, -1, NULL));
    g_setenv("DS1685_ERROR_CONFIG", config, true);
    g_setenv("DS1685_ERROR_IMAGE", strstr(kind, "main") ? store : clock, true);
    g_setenv("DS1685_MAIN", store, true);
    g_setenv("DS1685_CLOCK", clock, true);
    g_test_trap_subprocess(NULL, 15 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_passed();
    g_test_trap_assert_stderr("*ds1685:*failed*Input/output error*"
                             "persistent storage recovered*");
    g_unsetenv("DS1685_ERROR_CONFIG");
    g_unsetenv("DS1685_ERROR_IMAGE");
    g_unsetenv("DS1685_MAIN");
    g_unsetenv("DS1685_CLOCK");
    g_unlink(store);
    g_unlink(clock);
    g_unlink(config);
    g_rmdir(dir);
}

/* Locate the mandatory footer of the full DS1685 VMState section. */
static gsize ds1685_stream_footer(const char *contents, gsize len)
{
    for (gsize i = 0; i + 14 < len; i++) {
        unsigned name_len = (uint8_t)contents[i + 5];
        g_autofree char *name = NULL;

        if ((uint8_t)contents[i] != 0x04 || i + 14 + name_len >= len) {
            continue;
        }
        name = g_strndup(contents + i + 6, name_len);
        if (!g_str_has_suffix(name, "ds1685")) {
            continue;
        }
        for (gsize j = i + 14 + name_len; j + 5 <= len; j++) {
            if ((uint8_t)contents[j] == 0x7e &&
                !memcmp(contents + j + 1, contents + i + 1, 4)) {
                return j;
            }
        }
    }
    g_assert_not_reached();
}

static void test_storage_failed_incoming(const void *data)
{
    g_autofree char *dir = g_dir_make_tmp("ds1685-failed-XXXXXX", NULL);
    g_autofree char *store = g_build_filename(dir, "rtc.raw", NULL);
    g_autofree char *clock = g_build_filename(dir, "rtc.clock", NULL);
    g_autofree char *source = g_build_filename(dir, "source.raw", NULL);
    g_autofree char *source_clock = g_build_filename(dir, "source.clock", NULL);
    g_autofree char *outgoing = NULL;
    g_autofree char *contents = NULL;
    g_autofree char *stream = g_build_filename(dir, "bad.stream", NULL);
    g_autofree char *quoted = g_shell_quote(stream);
    g_autofree char *uri = g_strdup_printf("exec:cat %s", quoted);
    g_autofree char *stored = NULL;
    char image[512] = { 0 }, reference[24];
    gsize len;
    QTestState *qts;

    image[R_RAM_LOW] = 0x5a;
    memset(reference, 0xa5, sizeof(reference));
    g_assert_true(g_file_set_contents(store, image, sizeof(image), NULL));
    g_assert_true(g_file_set_contents(clock, reference,
                                      sizeof(reference), NULL));
    if (data) {
        QTestState *from;

        create_store(source, source_clock);
        from = start_with_store(source, source_clock, "2024-01-01T00:00:00");
        reg_write(from, R_RAM_LOW, 0xa5);
        outgoing = g_strdup_printf("exec:cat > %s", quoted);
        sgi_migrate_stopped(from, outgoing);
        qtest_quit(from);
        g_assert_true(g_file_get_contents(stream, &contents, &len, NULL));
        gsize footer = ds1685_stream_footer(contents, len);

        /* Reject a footer after DS1685 post_load has accepted its fields. */
        contents[footer] = 0;
        g_assert_true(g_file_set_contents(stream, contents, len, NULL));
    } else {
        g_assert_true(g_file_set_contents(stream, "bad stream", -1, NULL));
    }
    qts = qtest_initf(OCTANE_IOC3(SUBSYS_IP30)
        ",ds1685-drive=rtc-store -rtc base=2024-01-01T00:00:00,clock=vm"
        " -drive if=none,id=rtc-store,file=%s,format=raw"
        " -drive if=none,id=rtc-store-clock,file=%s,format=raw"
        " -incoming defer", store, clock);
    qtest_qmp_assert_success(qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s,"
        "'exit-on-error':false}}", uri);
    sgi_wait_migration_status(qts, "failed", 30);
    if (data) {
        octane_attach(qts);
        g_assert_cmphex(reg_read(qts, R_RAM_LOW), ==, 0xa5);
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(store, &stored, &len, NULL));
    g_assert_cmpmem(stored, len, image, sizeof(image));
    g_clear_pointer(&stored, g_free);
    g_assert_true(g_file_get_contents(clock, &stored, &len, NULL));
    g_assert_cmpmem(stored, len, reference, sizeof(reference));
    g_unlink(store);
    g_unlink(clock);
    g_unlink(stream);
    g_unlink(source);
    g_unlink(source_clock);
    g_rmdir(dir);
}

static void test_storage_snapshot(const void *data)
{
    g_autofree char *dir = g_dir_make_tmp("ds1685-snapshot-XXXXXX", NULL);
    g_autofree char *store = g_build_filename(dir, "rtc.qcow2", NULL);
    g_autofree char *clock = g_build_filename(dir, "rtc-clock.qcow2", NULL);
    g_autofree char *reply = NULL;
    QTestState *qts;

    if (!have_qemu_img()) {
        g_test_skip("QTEST_QEMU_IMG not set; cannot create snapshot storage");
        return;
    }
    for (int i = 0; i < 2; i++) {
        const char *args[] = { g_getenv("QTEST_QEMU_IMG"), "create", "-q",
            "-f", "qcow2", i ? clock : store, "512", NULL };
        int status;

        g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0,
            NULL, NULL, NULL, NULL, &status, NULL));
        g_assert_true(WIFEXITED(status));
        g_assert_cmpint(WEXITSTATUS(status), ==, 0);
    }
    qts = qtest_initf(OCTANE_IOC3(SUBSYS_IP30)
        ",ds1685-drive=rtc-store -rtc base=2024-01-01T00:00:00,clock=vm"
        " -drive if=none,id=rtc-store,file=%s,format=qcow2"
        " -drive if=none,id=rtc-store-clock,file=%s,format=qcow2",
        store, clock);
    octane_attach(qts);
    set_calendar_2025(qts);
    reg_write(qts, R_RAM_LOW, 0xa5);
    reg_write(qts, R_CTRL_A, A_DV0);
    reg_write(qts, R_CTRL_B, B_SET | B_DM | B_24_12);
    reg_write(qts, R_SECONDS, 55);
    reply = qtest_hmp(qts, "savevm rtc-state");
    g_assert_nonnull(reply);
    g_assert_null(strstr(reply, "Error"));
    g_clear_pointer(&reply, g_free);
    reg_write(qts, R_RAM_LOW, 0x5a);
    reg_write(qts, R_SECONDS, 33);
    reply = qtest_hmp(qts, "loadvm rtc-state");
    g_assert_nonnull(reply);
    g_assert_null(strstr(reply, "Error"));
    g_assert_cmphex(reg_read(qts, R_RAM_LOW), ==, 0xa5);
    g_assert_cmpuint(reg_read(qts, R_SECONDS), ==, 55);
    g_assert_cmphex(reg_read(qts, R_CTRL_A) & A_DV1, ==, 0);
    qtest_system_reset(qts);
    octane_attach(qts);
    reg_write(qts, R_CTRL_B, B_DM | B_24_12);
    qtest_clock_step(qts, 1000000000LL);
    g_assert_cmpuint(reg_read(qts, R_SECONDS), ==, 56);
    qtest_quit(qts);
    g_unlink(store);
    g_unlink(clock);
    g_rmdir(dir);
}

static void test_storage_octane_named_drive(const void *data)
{
    const char *machine = data;
    g_autofree char *dir = g_dir_make_tmp("ds1685-octane-XXXXXX", NULL);
    g_autofree char *store = g_build_filename(dir, "rtc.raw", NULL);
    g_autofree char *clock = g_build_filename(dir, "rtc.clock", NULL);
    g_autofree char *firmware = g_build_filename(dir, "synthetic.rom", NULL);
    const char prom[4] = { 0 };
    const char *cpu = !strcmp(machine, "octane") ? "R10000" :
        "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654,"
        "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152,"
        "r12000-scache-block-words=32";
    const uint64_t bridge = 0x1f000000;
    const uint64_t index = bridge + 0x600000 + 0xa0000;
    const uint64_t data_port = bridge + 0x600000 + 0xc0000;
    g_autofree char *image = NULL;
    gsize len;

    create_store(store, clock);
    g_assert_true(g_file_set_contents(firmware, prom, sizeof(prom), NULL));
    for (int pass = 0; pass < 2; pass++) {
        QTestState *qts = qtest_initf("-M %s -cpu %s -m %dM -bios %s"
            " -rtc base=2024-01-01T00:00:00,clock=vm"
            " -drive if=none,id=sgi-nvram0,file=%s,format=raw"
            " -drive if=none,id=sgi-nvram0-clock,file=%s,format=raw",
            machine, cpu, !strcmp(machine, "octane") ? 128 : 256,
            firmware, store, clock);

        qtest_writel(qts, bridge + 0x022010, 0x10000000);
        qtest_writel(qts, bridge + 0x022004, 0x00000002);
        qtest_writel(qts, bridge + 0x214, 0x1100);
        qtest_writeb(qts, index, R_RAM_LOW);
        if (!pass) {
            qtest_writeb(qts, data_port, 0xa5);
        }
        g_assert_cmphex(qtest_readb(qts, data_port), ==, 0xa5);
        qtest_quit(qts);
    }
    g_assert_true(g_file_get_contents(store, &image, &len, NULL));
    g_assert_cmphex((uint8_t)image[R_RAM_LOW], ==, 0xa5);
    g_clear_pointer(&image, g_free);
    g_assert_true(g_file_get_contents(clock, &image, &len, NULL));
    g_assert_cmpmem(image, 8, "DS168RF1", 8);
    g_unlink(firmware);
    g_unlink(store);
    g_unlink(clock);
    g_rmdir(dir);
}

static void test_storage_all_ram(const void *data)
{
    g_autofree char *dir = g_dir_make_tmp("ds1685-ram-XXXXXX", NULL);
    g_autofree char *store = g_build_filename(dir, "rtc.raw", NULL);
    g_autofree char *clock = g_build_filename(dir, "rtc.clock", NULL);
    QTestState *qts;

    create_store(store, clock);
    qts = start_with_store(store, clock, "2024-01-01T00:00:00");
    for (int reg = R_RAM_LOW; reg < 0x40; reg++) {
        reg_write(qts, reg, reg ^ 0xa5);
    }
    select_bank(qts, 0);
    for (int reg = 0x40; reg < 0x80; reg++) {
        reg_write(qts, reg, reg ^ 0x5a);
    }
    select_bank(qts, 1);
    for (int addr = 0; addr < 128; addr++) {
        reg_write(qts, R_EXTRAM_ADDR, addr);
        reg_write(qts, R_EXTRAM_DATA, addr ^ 0xff);
    }
    qtest_quit(qts);
    qts = start_with_store(store, clock, "2024-01-01T00:00:12");
    for (int reg = R_RAM_LOW; reg < 0x40; reg++) {
        g_assert_cmphex(reg_read(qts, reg), ==, reg ^ 0xa5);
    }
    select_bank(qts, 0);
    for (int reg = 0x40; reg < 0x80; reg++) {
        g_assert_cmphex(reg_read(qts, reg), ==, reg ^ 0x5a);
    }
    select_bank(qts, 1);
    for (int addr = 0; addr < 128; addr++) {
        reg_write(qts, R_EXTRAM_ADDR, addr);
        g_assert_cmphex(reg_read(qts, R_EXTRAM_DATA), ==, addr ^ 0xff);
    }
    qtest_quit(qts);
    g_unlink(store);
    g_unlink(clock);
    g_rmdir(dir);
}

static void test_storage_migration(const void *data)
{
    g_autofree char *dir = g_dir_make_tmp("ds1685-migrate-XXXXXX", NULL);
    g_autofree char *store = g_build_filename(dir, "source.raw", NULL);
    g_autofree char *clock = g_build_filename(dir, "source.clock", NULL);
    g_autofree char *dst = g_build_filename(dir, "dest.raw", NULL);
    g_autofree char *dst_clock = g_build_filename(dir, "dest.clock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    g_autofree char *image = NULL;
    g_autofree char *stream = g_build_filename(dir, "migration.stream", NULL);
    g_autofree char *prefix = g_build_filename(dir, "prefix.stream", NULL);
    g_autofree char *suffix = g_build_filename(dir, "suffix.stream", NULL);
    g_autofree char *fifo = g_build_filename(dir, "resume.fifo", NULL);
    bool staged = data && !strcmp(data, "staged");
    bool counting = data && !staged;
    int resume_fd = -1;
    gsize len;
    QTestState *from, *to;

    create_store(store, clock);
    {
        char sentinel[512] = { 0 };
        char reference[24];

        sentinel[R_RAM_LOW] = 0x5a;
        memset(reference, 0xa5, sizeof(reference));
        g_assert_true(g_file_set_contents(dst, sentinel,
                                          sizeof(sentinel), NULL));
        g_assert_true(g_file_set_contents(dst_clock, reference,
                                          sizeof(reference), NULL));
    }
    from = start_with_store(store, clock, "2024-01-01T00:00:00");
    set_calendar_2025(from);
    reg_write(from, R_RAM_LOW, 0xa5);
    reg_write(from, R_CTRL_B, B_SET | B_DM | B_24_12);
    reg_write(from, R_SECONDS, 55);
    reg_write(from, R_MONTH, 0);
    if (!counting) {
        reg_write(from, R_CTRL_A, A_DV0);
    }
    qtest_clock_step(from, 2 * 1000000000LL);
    reg_write(from, R_EXT_CTRL_4A, X4A_KF);
    reg_write(from, R_EXT_CTRL_4B, X4B_KSE);
    if (staged) {
        g_autofree char *outgoing = g_strdup_printf("exec:cat > %s", stream);
        g_autofree char *contents = NULL;
        gsize split;

        sgi_migrate_stopped(from, outgoing);
        g_assert_true(g_file_get_contents(stream, &contents, &len, NULL));
        /* Stop after the RTC footer while the rest of the stream is pending. */
        split = ds1685_stream_footer(contents, len) + 5;
        g_assert_cmpuint(split, <, len);
        g_assert_true(g_file_set_contents(prefix, contents, split, NULL));
        g_assert_true(g_file_set_contents(suffix, contents + split,
                                          len - split, NULL));
        g_assert_cmpint(mkfifo(fifo, 0600), ==, 0);
        resume_fd = open(fifo, O_RDWR);
        g_assert_cmpint(resume_fd, >=, 0);
        g_free(uri);
        uri = g_strdup_printf("exec:cat %s; read resume < %s; cat %s",
                              prefix, fifo, suffix);
    }
    to = qtest_initf(OCTANE_IOC3(SUBSYS_IP30)
        ",ds1685-drive=rtc-store -rtc base=2034-01-01T00:00:00,clock=vm"
        " -drive if=none,id=rtc-store,file=%s,format=raw"
        " -drive if=none,id=rtc-store-clock,file=%s,format=raw"
        " -incoming defer", dst, dst_clock);
    qtest_irq_intercept_out_named(to, "/machine/peripheral/octane/ds1685",
                                  "sysbus-irq");
    g_assert_false(qtest_get_irq(to, 0));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    if (staged) {
        int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

        while (!qtest_get_irq(to, 0)) {
            qtest_qmp_assert_success(to, "{'execute':'query-status'}");
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        }
        /* Exercise the save refusal while whole-stream success is unknown. */
        octane_attach(to);
        g_assert_cmphex(reg_read(to, R_RAM_LOW), ==, 0xa5);
        g_assert_true(g_file_get_contents(dst, &image, &len, NULL));
        g_assert_cmphex((uint8_t)image[R_RAM_LOW], ==, 0x5a);
        g_clear_pointer(&image, g_free);
        g_assert_cmpint(write(resume_fd, "go\n", 3), ==, 3);
        close(resume_fd);
    } else {
        sgi_migrate_stopped(from, uri);
    }
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_true(qtest_get_irq(to, 0));
    /* Completion must persist without an RTC access or process shutdown. */
    g_assert_true(g_file_get_contents(dst, &image, &len, NULL));
    g_assert_cmpuint(len, ==, 512);
    g_assert_cmphex((uint8_t)image[R_RAM_LOW], ==, 0xa5);
    g_assert_cmpuint((uint8_t)image[R_SECONDS], ==, 55);
    g_assert_cmpuint((uint8_t)image[R_MONTH], ==, 0);
    g_clear_pointer(&image, g_free);
    g_assert_true(g_file_get_contents(dst_clock, &image, &len, NULL));
    g_assert_cmpuint(len, >=, 24);
    g_assert_cmpmem(image, 8, "DS168RF1", 8);
    g_assert_cmpint(ldq_le_p(image + 16), ==,
                    counting ? 1735689612 : 1735689610);
    g_clear_pointer(&image, g_free);
    octane_attach(to);
    g_assert_cmphex(reg_read(to, R_RAM_LOW), ==, 0xa5);
    g_assert_cmpuint(reg_read(to, R_SECONDS), ==, 55);
    g_assert_cmpuint(reg_read(to, R_MONTH), ==, 0);
    g_assert_cmphex(reg_read(to, R_CTRL_A) & A_DV1, ==,
                    counting ? A_DV1 : 0);
    g_assert_cmphex(reg_read(to, R_EXT_CTRL_4A) & X4A_KF, ==, X4A_KF);
    g_assert_cmphex(reg_read(to, R_EXT_CTRL_4B) & X4B_KSE, ==, X4B_KSE);
    reg_write(to, R_RAM_LOW + 1, 0x5a);
    g_assert_true(g_file_get_contents(dst, &image, &len, NULL));
    g_assert_cmphex((uint8_t)image[R_RAM_LOW], ==, 0xa5);
    g_assert_cmphex((uint8_t)image[R_RAM_LOW + 1], ==, 0x5a);
    g_clear_pointer(&image, g_free);
    g_assert_true(g_file_get_contents(dst_clock, &image, &len, NULL));
    g_assert_cmpint(ldq_le_p(image + 16), ==,
                    counting ? 1735689612 : 1735689610);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(store);
    g_unlink(clock);
    g_unlink(dst);
    g_unlink(dst_clock);
    g_unlink(stream);
    g_unlink(prefix);
    g_unlink(suffix);
    g_unlink(fifo);
    g_rmdir(dir);
}

static void test_storage_explicit_reference(const void *data)
{
    g_autofree char *dir = g_dir_make_tmp("ds1685-explicit-XXXXXX", NULL);
    g_autofree char *store = g_build_filename(dir, "rtc.raw", NULL);
    g_autofree char *clock = g_build_filename(dir, "rtc.clock", NULL);
    g_autofree char *explicit = g_build_filename(dir, "explicit.clock", NULL);
    g_autofree char *contents = NULL;
    uint8_t reference[24];
    gsize len;
    QTestState *qts;

    create_store(store, clock);
    qts = start_with_store(store, clock, "2024-01-01T00:00:00");
    set_calendar_2025(qts);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(clock, &contents, &len, NULL));
    g_assert_cmpuint(len, ==, sizeof(reference));
    memcpy(reference, contents, sizeof(reference));
    stq_le_p(reference + 16, 1735689710);
    g_assert_true(g_file_set_contents(explicit, (char *)reference,
                                      sizeof(reference), NULL));
    qts = qtest_initf(OCTANE_IOC3(SUBSYS_IP30)
        ",ds1685-drive=rtc-store,ds1685-clock-drive=explicit-clock"
        " -rtc base=2024-01-01T00:00:00,clock=vm"
        " -drive if=none,id=rtc-store,file=%s,format=raw"
        " -drive if=none,id=rtc-store-clock,file=%s,format=raw"
        " -drive if=none,id=explicit-clock,file=%s,format=raw",
        store, clock, explicit);
    octane_attach(qts);
    g_assert_cmpuint(reg_read(qts, R_MINUTES), ==, 1);
    g_assert_cmpuint(reg_read(qts, R_SECONDS), ==, 50);
    reg_write(qts, R_RAM_LOW, 0xa5);
    qtest_quit(qts);
    g_clear_pointer(&contents, g_free);
    g_assert_true(g_file_get_contents(clock, &contents, &len, NULL));
    g_assert_cmpint(ldq_le_p(contents + 16), ==, 1735689610);
    g_unlink(store);
    g_unlink(clock);
    g_unlink(explicit);
    g_rmdir(dir);
}

static void test_storage_set_edit(const void *data)
{
    g_autofree char *dir = g_dir_make_tmp("ds1685-set-XXXXXX", NULL);
    g_autofree char *store = g_build_filename(dir, "rtc.raw", NULL);
    g_autofree char *clock = g_build_filename(dir, "rtc.clock", NULL);
    g_autofree char *reference = NULL;
    gsize len;
    QTestState *qts;

    create_store(store, clock);
    qts = start_with_store(store, clock, "2024-01-01T00:00:00");
    set_calendar_2025(qts);
    reg_write(qts, R_CTRL_B, B_SET | B_DM | B_24_12);
    reg_write(qts, R_SECONDS, 55);
    reg_write(qts, R_MONTH, 0);
    qtest_clock_step(qts, 7 * 1000000000LL);
    qtest_quit(qts);
    qts = start_with_store(store, clock, "2024-01-01T00:00:12");
    g_assert_cmpuint(reg_read(qts, R_SECONDS), ==, 55);
    g_assert_cmpuint(reg_read(qts, R_MONTH), ==, 0);
    g_assert_cmphex(reg_read(qts, R_CTRL_B) & B_SET, ==, B_SET);
    reg_write(qts, R_RAM_LOW, 0xa5);
    g_assert_true(g_file_get_contents(clock, &reference, &len, NULL));
    g_assert_cmpint(ldq_le_p(reference + 16), ==, 1735689622);
    reg_write(qts, R_MONTH, 1);
    reg_write(qts, R_CTRL_B, B_DM | B_24_12);
    qtest_clock_step(qts, 1000000000LL);
    g_assert_cmpuint(reg_read(qts, R_SECONDS), ==, 56);
    qtest_quit(qts);
    g_unlink(store);
    g_unlink(clock);
    g_rmdir(dir);
}

static void test_storage_preserves_raw_calendar(const void *data)
{
    g_autofree char *dir = g_dir_make_tmp("ds1685-raw-XXXXXX", NULL);
    g_autofree char *store = g_build_filename(dir, "rtc.raw", NULL);
    g_autofree char *clock = g_build_filename(dir, "rtc.clock", NULL);
    g_autofree char *image = NULL;
    gsize len;
    QTestState *qts;

    create_store(store, clock);
    qts = start_with_store(store, clock, "2024-01-01T00:00:00");
    reg_write(qts, R_WEEKDAY, 0xff);
    reg_write(qts, R_RAM_LOW, 0xa5);
    select_bank(qts, 1);
    g_assert_cmphex(reg_read(qts, R_WEEKDAY), ==, 0xff);
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_true(g_file_get_contents(store, &image, &len, NULL));
    g_assert_cmphex((uint8_t)image[R_WEEKDAY], ==, 0xff);
    g_assert_cmphex(reg_read(qts, R_WEEKDAY), ==, 0xff);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    qtest_clock_step(qts, 1000000000LL);
    g_assert_cmpuint(reg_read(qts, R_WEEKDAY), ==, 2);
    qtest_quit(qts);
    g_unlink(store);
    g_unlink(clock);
    g_rmdir(dir);
}

static void test_storage_restore_case(const void *data)
{
    const char *kind = data;
    g_autofree char *dir = g_dir_make_tmp("ds1685-restore-XXXXXX", NULL);
    g_autofree char *store = g_build_filename(dir, "rtc.raw", NULL);
    g_autofree char *clock = g_build_filename(dir, "rtc.clock", NULL);
    uint8_t image[512] = { 0 };
    uint8_t reference[24] = { 0 };
    g_autofree char *saved = NULL;
    gsize len;
    bool fresh = !strcmp(kind, "fresh-padding");
    bool invalid = !strcmp(kind, "invalid-calendar");
    bool stopped = !strcmp(kind, "stopped");
    bool countdown = !strcmp(kind, "countdown-reset");
    bool missing = !strcmp(kind, "missing-reference");
    bool negative = !strcmp(kind, "negative-elapsed");
    bool wrong_magic = !strcmp(kind, "wrong-magic");
    QTestState *qts;

    if (!fresh) {
        image[0] = 10;
        image[R_WEEKDAY] = 4;
        image[R_DAY] = 1;
        image[R_MONTH] = invalid ? 0 : 1;
        image[R_YEAR] = 25;
        image[R_CTRL_A] = stopped ? 0x10 : countdown ? 0x70 : 0x30;
        image[R_CTRL_B] = B_DM | B_24_12;
        image[R_RAM_LOW] = 0xa5;
        image[0x88] = 20;
    }
    memset(image + 320, 0xa5, 192);
    memcpy(reference, wrong_magic ? "M48TREF1" : "DS168RF1", 8);
    stq_le_p(reference + 8, negative ? 1704067220 : 1704067200);
    stq_le_p(reference + 16, 1735689610);
    g_assert_true(g_file_set_contents(store, (char *)image,
                                      sizeof(image), NULL));
    if (!missing) {
        g_assert_true(g_file_set_contents(clock, (char *)reference,
                                          sizeof(reference), NULL));
    }
    qts = start_with_store(store, missing ? NULL : clock,
                          "2024-01-01T00:00:00");
    g_assert_cmpuint(reg_read(qts, R_YEAR), ==, fresh || invalid ? 24 : 25);
    g_assert_cmpuint(reg_read(qts, R_SECONDS), ==, fresh || invalid ? 0 : 10);
    g_assert_cmphex(reg_read(qts, R_RAM_LOW), ==, fresh ? 0 : 0xa5);
    if (countdown) {
        reg_write(qts, R_CTRL_A, 0x30);
    }
    if (stopped || countdown) {
        qtest_clock_step(qts, 1000000000LL);
        g_assert_cmpuint(reg_read(qts, R_SECONDS), ==, 11);
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(store, &saved, &len, NULL));
    for (int i = 320; i < 512; i++) {
        g_assert_cmphex((uint8_t)saved[i], ==, 0);
    }
    g_unlink(store);
    if (!missing) {
        g_unlink(clock);
    }
    g_rmdir(dir);
}

static void test_storage_bcd_twelve_hour(const void *data)
{
    g_autofree char *dir = g_dir_make_tmp("ds1685-bcd-XXXXXX", NULL);
    g_autofree char *store = g_build_filename(dir, "rtc.raw", NULL);
    g_autofree char *clock = g_build_filename(dir, "rtc.clock", NULL);
    QTestState *qts;

    create_store(store, clock);
    qts = start_with_store(store, clock, "2024-01-01T00:00:00");
    reg_write(qts, R_CTRL_B, B_SET);
    reg_write(qts, R_SECONDS, 0x17);
    reg_write(qts, R_MINUTES, 0x45);
    reg_write(qts, R_HOURS, 0x81);
    reg_write(qts, R_WEEKDAY, 0x04);
    reg_write(qts, R_DAY, 0x01);
    reg_write(qts, R_MONTH, 0x01);
    reg_write(qts, R_YEAR, 0x25);
    select_bank(qts, 1);
    reg_write(qts, R_CENTURY, 0x20);
    reg_write(qts, R_CTRL_B, 0);
    qtest_quit(qts);
    qts = start_with_store(store, clock, "2024-01-01T00:00:02");
    g_assert_cmphex(reg_read(qts, R_SECONDS), ==, 0x19);
    g_assert_cmphex(reg_read(qts, R_MINUTES), ==, 0x45);
    g_assert_cmphex(reg_read(qts, R_HOURS), ==, 0x81);
    g_assert_cmphex(reg_read(qts, R_YEAR), ==, 0x25);
    g_assert_cmphex(reg_read(qts, R_CENTURY), ==, 0x20);
    qtest_quit(qts);
    g_unlink(store);
    g_unlink(clock);
    g_rmdir(dir);
}


int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sn0/ds1685/storage-explicit-reference", NULL,
                        test_storage_explicit_reference);
    qtest_add_data_func("/sn0/ds1685/storage-set-edit", NULL,
                        test_storage_set_edit);
    qtest_add_data_func("/sn0/ds1685/storage-raw-calendar", NULL,
                        test_storage_preserves_raw_calendar);
    qtest_add_data_func("/sn0/ds1685/storage-bcd-twelve-hour", NULL,
                        test_storage_bcd_twelve_hour);
    {
        const char *cases[] = { "fresh-padding", "invalid-calendar",
                               "missing-reference", "wrong-magic",
                               "negative-elapsed", "stopped",
                               "countdown-reset" };

        for (int i = 0; i < ARRAY_SIZE(cases); i++) {
            g_autofree char *name = g_strdup_printf(
                "/sn0/ds1685/storage-restore/%s", cases[i]);

            qtest_add_data_func(name, cases[i], test_storage_restore_case);
        }
    }
    {
        const char *bad[] = { "empty", "oversize", "readonly", "orphan",
                              "wrong-board", "read-error",
                              "clock-read-error" };
        const char *errors[] = { "main-write", "main-flush",
                                 "clock-write", "clock-flush" };

        for (int i = 0; i < ARRAY_SIZE(bad); i++) {
            g_autofree char *name = g_strdup_printf(
                "/sn0/ds1685/storage-reject/%s", bad[i]);
            qtest_add_data_func(name, bad[i], test_storage_rejects_backend);
        }
        for (int i = 0; i < ARRAY_SIZE(errors); i++) {
            g_autofree char *name = g_strdup_printf(
                "/sn0/ds1685/storage-error/%s", errors[i]);
            qtest_add_data_func(name, errors[i], test_storage_write_error);
        }
    }
    qtest_add_data_func("/sn0/ds1685/storage-failed-incoming", NULL,
                        test_storage_failed_incoming);
    qtest_add_data_func("/sn0/ds1685/storage-invalid-footer", "footer",
                        test_storage_failed_incoming);
    qtest_add_data_func("/sn0/ds1685/storage-snapshot", NULL,
                        test_storage_snapshot);
    qtest_add_data_func("/sn0/ds1685/storage-octane", "octane",
                        test_storage_octane_named_drive);
    qtest_add_data_func("/sn0/ds1685/storage-octane2", "octane2",
                        test_storage_octane_named_drive);
    qtest_add_data_func("/sn0/ds1685/storage-all-ram", NULL,
                        test_storage_all_ram);
    qtest_add_data_func("/sn0/ds1685/storage-running-migration", "running",
                        test_storage_migration);
    qtest_add_data_func("/sn0/ds1685/storage-staged-migration", "staged",
                        test_storage_migration);
    qtest_add_data_func("/sn0/ds1685/storage-migration", NULL,
                        test_storage_migration);
    qtest_add_data_func("/sn0/ds1685/storage-process-restart", NULL,
                        test_storage_process_restart);
    qtest_add_data_func("/sn0/ds1685/volatile-counter-hold", NULL,
                        test_volatile_counter_hold);
    qtest_add_data_func("/sn0/ds1685/initial-calendar-and-mode-writes", NULL,
                        test_initial_calendar_and_mode_writes);
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
