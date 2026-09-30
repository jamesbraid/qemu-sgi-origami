/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check the bridge identity, register window, and translation-table access
 * through crosstalk. This does not test DMA translation.
 */

#include "rom.h"

const char rom_name[] = "bridgebus";

#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define CCA_UNCACHED    2
#define ATTR_XTALK      1

/*
 * Widget 8's small window at node zero, as exposed by the configuration
 * graph.
 */
#define WIDGET          8
#define WIDGET_WINDOW   ((uint64_t)WIDGET * 0x1000000ull)
#define BRIDGE(off)     XKPHYS(CCA_UNCACHED, ATTR_XTALK, WIDGET_WINDOW + (off))

/*
 * Every register is 32 bits in the low half of an eight-byte slot, so
 * the
 * data
 * is at the slot's offset plus four on this big-endian machine.
 */
#define REG_DATA        4

#define BRIDGE_IDENTIFICATION   0x000
#define BRIDGE_CONTROL          0x020
#define BRIDGE_ATE_BASE         0x010000
#define BRIDGE_ATE_ENTRY_SIZE   8

/* Distinctive upper half for the table's 32-bit readback. */
#define ATE_TEST_VALUE  0x0000000123456102ull

/* Expected bridge identity, also reported by widget 0. */
#define BRIDGE_PART             0xc002
#define BRIDGE_MANUFACTURER     0x036

/* Widget 0 is reached through the last big window. */
#define WIDGET0         XKPHYS(CCA_UNCACHED, ATTR_XTALK, 0xe0000000ull)

static uint32_t bridge_read(uint64_t off)
{
    /*
     * Volatile preserves each required MMIO access.
     */
    return *(volatile uint32_t *)(unsigned long)BRIDGE(off + REG_DATA);
}

static void bridge_write(uint64_t off, uint32_t val)
{
    /*
     * Volatile preserves each required MMIO access.
     */
    *(volatile uint32_t *)(unsigned long)BRIDGE(off + REG_DATA) = val;
}

void rom_main(void)
{
    uint32_t id, control;

    /*
     * Interrupts stay off — nothing here wants control transferred —
     * and
     * 64-bit
     * kernel addressing goes on, because crosstalk space is only
     * reachable
     * through the 64-bit unmapped window.
     */
    rom_set_status((rom_get_status() & ~ROM_ST_IE) | ROM_ST_KX);

    /* Exercise bridge placement through the bus, not a local device hook. */
    id = bridge_read(BRIDGE_IDENTIFICATION);
    rom_assert_eq(1, BRIDGE_PART, (id >> 12) & 0xffff);
    rom_assert_eq(2, BRIDGE_MANUFACTURER, (id >> 1) & 0x7ff);
    rom_assert_eq(3, 1, id & 1);

    /*
     * Compare widget 0's identity with the bridge reached through widget 8;
     * this catches a wrong route even when each window responds alone.
     */
    rom_assert_eq(4, id, *(volatile uint32_t *)(unsigned long)(WIDGET0 + 4));

    /* Catch a stub or window mapping that responds only at offset zero. */
    control = bridge_read(BRIDGE_CONTROL);
    bridge_write(BRIDGE_CONTROL, control);
    rom_assert_eq(5, control, bridge_read(BRIDGE_CONTROL));

    /*
     * The table lies beyond the register block and requires a full aligned
     * 64-bit write; only the upper half reads back at offset plus four.
     * This catches narrow window mappings and accidental register-style
     * writes to table entries.
     */
    *(volatile uint64_t *)(unsigned long)BRIDGE(BRIDGE_ATE_BASE) =
        ATE_TEST_VALUE;
    rom_assert_eq(6, (uint32_t)(ATE_TEST_VALUE >> 32),
                  /*
                   * Volatile preserves each required MMIO access.
                   */
                  *(volatile uint32_t *)(unsigned long)
                      BRIDGE(BRIDGE_ATE_BASE + REG_DATA));

    /* Restore the table entry so later tests see reset state. */
    *(volatile uint64_t *)(unsigned long)BRIDGE(BRIDGE_ATE_BASE) = 0;
}
