/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The I/O bridge reached the way a guest reaches it, through the
 * crosstalk
 * bus.
 *
 * The bridge has a thorough register test of its own, and this is not a
 * second
 * copy of it. That test drives the machine's memory directly, with no
 * processor
 * executing: it proves the device answers when something writes to its
 * address.
 * What it cannot prove is that the address is reachable — a machine
 * that
 * placed
 * the bridge on no bus, or on a bus nothing routes to, passes every
 * assertion
 * in
 * it. Isolated green and system working are different claims and only
 * one of
 * them was being made.
 *
 * So this asks the other question, and asks it the way the guest does:
 * a
 * processor forms a crosstalk address, the memory system routes it
 * through
 * the
 * node's small window for widget 8 into crosstalk space, and the bridge
 * answers
 * at the far end. Every layer between the instruction and the register
 * is the
 * one a guest uses, rather than being stepped over.
 *
 * Three things are checked, in the order they have to work.
 *
 * The identification register, because it is what anything walking the
 * fabric
 * reads first and because its value is the machine's identity rather
 * than the
 * chip's alone: this bridge is what widget 0 also reports itself as, so
 * the
 * two
 * have to agree or the machine is describing two different chips.
 *
 * A register beyond the identity, because a model that answered only at
 * offset
 * zero — a stub, or a window mapped one register wide — would satisfy
 * the
 * first
 * check completely.
 *
 * And an address translation entry, written and read back, because that
 * table
 * is what a device's transfers pass through. It is the largest thing in
 * the
 * chip that a processor can reach without a device attached, and
 * reaching it
 * proves the window is the whole window rather than its first page.
 *
 * What this deliberately does not do is exercise a transfer.
 * Translation only
 * happens for an address a device issues, so testing it needs a device
 * that
 * issues one, and the bridge's own test has that with a DMA master it
 * attaches
 * on the command line. This checks the table is reachable and writable
 * through
 * the bus; that its entries steer a real transfer is the other test's.
 *
 * Plain `-M origin200` includes the bridge, so the R10000 suite runs
 * this
 * ROM.
 * It fails rather than skips if the bridge is absent.
 *
 * R10000 only, and for the same reason as the Hub's interrupt ROM
 * rather than
 * a
 * new one: the route to crosstalk space is the 64-bit unmapped window
 * with a
 * non-zero uncached attribute, which is an R10000-family extension. The
 * bus
 * is
 * the machine's; reaching it is the processor's.
 */

#include "rom.h"

const char rom_name[] = "bridgebus";

#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define CCA_UNCACHED    2
#define ATTR_XTALK      1

/*
 * Widget 8's small window: the widget number times 16 MB, at node zero.
 * This
 * is
 * the address the configuration graph hands a guest and the address
 * every
 * boot
 * log has ended at, so it is the one worth testing rather than any
 * other
 * route
 * to the same device.
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

/*
 * A value whose upper half is distinctive, since the upper half is the
 * only
 * part that reads back. The low bits are a plausible entry — coherent,
 * with a
 * target widget — so that nothing here depends on the chip accepting
 * nonsense.
 */
#define ATE_TEST_VALUE  0x0000000123456102ull

/*
 * What this machine's bridge reports, which widget 0 must report too.
 */
#define BRIDGE_PART             0xc002
#define BRIDGE_MANUFACTURER     0x036

/*
 * Where widget 0 answers, which is through the last big window.
 */
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

    /*
     * The identification register, reached through the bus rather than
     * beside
     * it. A machine that built the bridge and placed it nowhere fails
     * here,
     * which is the failure the bridge's own test cannot see.
     */
    id = bridge_read(BRIDGE_IDENTIFICATION);
    rom_assert_eq(1, BRIDGE_PART, (id >> 12) & 0xffff);
    rom_assert_eq(2, BRIDGE_MANUFACTURER, (id >> 1) & 0x7ff);
    rom_assert_eq(3, 1, id & 1);

    /*
     * The same chip is what widget 0 reports itself as on this machine,
     * so
     * the
     * two addresses have to return the same identity. If they disagree
     * the
     * machine is describing two different chips in one place, and a
     * guest
     * branching on widget 0 would be deciding about a part it never
     * reaches.
     *
     * This is the assertion that needs both halves of the fabric at
     * once:
     * widget
     * 0 is reached through the last big window and the Hub's
     * translation
     * table,
     * and the bridge through a small window, so it fails if either
     * route is
     * wrong while every single-route check passes.
     */
    rom_assert_eq(4, id, *(volatile uint32_t *)(unsigned long)(WIDGET0 + 4));

    /*
     * A register past the identity. A stub answering only at offset
     * zero, or
     * a
     * window mapped one register wide, satisfies everything above.
     */
    control = bridge_read(BRIDGE_CONTROL);
    bridge_write(BRIDGE_CONTROL, control);
    rom_assert_eq(5, control, bridge_read(BRIDGE_CONTROL));

    /*
     * And the translation table, sixty-four kilobytes into the widget's
     * window.
     * Reaching it proves the window is the whole window rather than its
     * first
     * page — a mapping that covered only the register block would have
     * answered
     * every check above and failed here.
     *
     * Its access convention is its own and stricter than the register
     * block's:
     * an entry is written whole, as one 64-bit quantity at an
     * eight-byte
     * boundary, and only its upper half reads back, as a 32-bit read at
     * the
     * offset plus four. A 32-bit write would leave half an entry
     * behind, so
     * the
     * chip refuses it rather than serving it.
     *
     * This ROM had that wrong on its first run — it wrote 32 bits at
     * the
     * plus-
     * four offset, borrowing the register block's convention for a
     * table that
     * does not share it — and the model refused the access and said so
     * in a
     * log
     * line naming the rule. Recorded because the mistake is the natural
     * one:
     * the
     * plus-four convention is right for every register on this chip and
     * wrong
     * for the one structure that is not a register.
     */
    *(volatile uint64_t *)(unsigned long)BRIDGE(BRIDGE_ATE_BASE) =
        ATE_TEST_VALUE;
    rom_assert_eq(6, (uint32_t)(ATE_TEST_VALUE >> 32),
                  /*
                   * Volatile preserves each required MMIO access.
                   */
                  *(volatile uint32_t *)(unsigned long)
                      BRIDGE(BRIDGE_ATE_BASE + REG_DATA));

    /*
     * Put it back to the state reset left, so nothing downstream
     * inherits an
     * entry this ROM programmed. Nothing issues a transfer through the
     * table
     * yet, but a later test that assumed a clean table would be reading
     * this
     * ROM's leftovers.
     */
    *(volatile uint64_t *)(unsigned long)BRIDGE(BRIDGE_ATE_BASE) = 0;
}
