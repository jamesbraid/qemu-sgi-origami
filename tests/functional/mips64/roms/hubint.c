/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Check interrupt output through the processor Cause register. A pending
 * bit with its mask clear must not raise a line. Setting the mask must
 * raise it without another pending-register write; clearing the pending
 * bit or mask must lower it. Register readback alone cannot catch these
 * output-update failures.
 */

#include "rom.h"

const char rom_name[] = "hubint";

/* Uncached Hub register window for node zero. */
#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define CCA_UNCACHED    2
#define ATTR_XTALK      1               /* the attribute that means crosstalk */
#define HUB_WINDOW      0x01000000ull   /* widget 1's small window, node zero */

#define HUB_REG(off)    XKPHYS(CCA_UNCACHED, ATTR_XTALK, HUB_WINDOW + (off))

#define PI_INT_MODIFY   0x000090
#define PI_INT_PEND0    0x000098
#define PI_INT_PEND1    0x0000a0
#define PI_INT_MASK0_A  0x0000a8
#define PI_INT_MASK1_A  0x0000b0

#define INT_MODIFY_SET  0x100u
#define INT_BANK_BITS   64

/* Cause bits for the two processor lines checked below. */
#define CAUSE_BANK0     (1u << 10)
#define CAUSE_BANK1     (1u << 11)

/* Select one interrupt number from each pending bank. */
#define NUMBER_BANK0    12
#define NUMBER_BANK1    70

static void hub_write(unsigned int off, uint64_t val)
{
    /*
     * Volatile preserves each required MMIO access.
     */
    *(volatile uint64_t *)(unsigned long)HUB_REG(off) = val;
}

static uint64_t hub_read(unsigned int off)
{
    /*
     * Volatile preserves each required MMIO access.
     */
    return *(volatile uint64_t *)(unsigned long)HUB_REG(off);
}

static void int_set(unsigned int number)
{
    hub_write(PI_INT_MODIFY, INT_MODIFY_SET | number);
}

static void int_clear(unsigned int number)
{
    hub_write(PI_INT_MODIFY, number);
}

void rom_main(void)
{
    uint64_t bank0 = 1ull << NUMBER_BANK0;
    uint64_t bank1 = 1ull << (NUMBER_BANK1 - INT_BANK_BITS);

    /* Keep interrupts disabled so the ROM can inspect Cause directly. */
    rom_set_status(rom_get_status() & ~ROM_ST_IE);

    /* Hub window accesses require 64-bit kernel addressing. */
    rom_set_status(rom_get_status() | ROM_ST_KX);

    /* Begin with both masks and pending banks clear. */
    hub_write(PI_INT_MASK0_A, 0);
    hub_write(PI_INT_MASK1_A, 0);
    int_clear(NUMBER_BANK0);
    int_clear(NUMBER_BANK1);

    rom_assert_eq(1, 0, hub_read(PI_INT_PEND0));
    rom_assert_eq(2, 0, hub_read(PI_INT_PEND1));
    rom_assert_eq(3, 0, rom_get_cause() & (CAUSE_BANK0 | CAUSE_BANK1));

    /* A pending bit alone must not raise the processor line. */
    int_set(NUMBER_BANK0);
    rom_assert_eq(4, bank0, hub_read(PI_INT_PEND0));
    rom_assert_eq(5, 0, rom_get_cause() & CAUSE_BANK0);

    /* Changing only the mask must update the processor line. */
    hub_write(PI_INT_MASK0_A, bank0);
    rom_assert_eq(6, bank0, hub_read(PI_INT_PEND0));
    rom_assert_eq(7, CAUSE_BANK0, rom_get_cause() & CAUSE_BANK0);

    /* Bank 0 activity must not raise bank 1. */
    rom_assert_eq(8, 0, rom_get_cause() & CAUSE_BANK1);

    /* Check bank 1 independently and verify bank 0 stayed pending. */
    hub_write(PI_INT_MASK1_A, bank1);
    int_set(NUMBER_BANK1);
    rom_assert_eq(9, bank1, hub_read(PI_INT_PEND1));
    rom_assert_eq(10, bank0, hub_read(PI_INT_PEND0));
    rom_assert_eq(11, CAUSE_BANK1, rom_get_cause() & CAUSE_BANK1);

    /* Clearing the pending bit must lower its line. */
    int_clear(NUMBER_BANK0);
    rom_assert_eq(12, 0, hub_read(PI_INT_PEND0));
    rom_assert_eq(13, 0, rom_get_cause() & CAUSE_BANK0);

    /* Clearing one bank must leave the other line asserted. */
    rom_assert_eq(14, CAUSE_BANK1, rom_get_cause() & CAUSE_BANK1);

    /* Clearing the mask lowers the line but preserves pending state. */
    hub_write(PI_INT_MASK1_A, 0);
    rom_assert_eq(15, bank1, hub_read(PI_INT_PEND1));
    rom_assert_eq(16, 0, rom_get_cause() & CAUSE_BANK1);

    int_clear(NUMBER_BANK1);
    rom_assert_eq(17, 0, hub_read(PI_INT_PEND1));
}
