/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Whose big-window table decodes an address that names no node?
 *
 * A processor reaches a crosstalk widget through one of the Hub's seven
 * big
 * windows, and which widget a window reaches is programmed in that
 * Hub's own
 * table. So an access carries two questions, not one: which node's
 * window it
 * lands in, and therefore which node's table translates it.
 *
 * An address naming no node names node zero -- the node number in it is
 * literally zero, and there is no separate encoding for "mine". So such
 * an
 * access must reach the FIRST node's window and be decoded by the FIRST
 * node's table, from whichever processor issues it.
 *
 * This checks that, and it exists because the machine did the opposite:
 * a
 * per-processor alias covered every crosstalk address, so an access
 * naming no
 * node was captured and handed to the reader's own node. The interrupt
 * register on the same space was measured doing exactly that, and this
 * is the
 * same question asked of the surface next door.
 *
 * HOW IT TELLS THE TWO APART. Both nodes carry a Bridge on widget
 * eight, and
 * a Bridge answers its identification register with a value that cannot
 * be
 * zero. So the tables are programmed in opposite senses:
 *
 *     window 0:  the first node's table valid, the second node's
 * disabled
 *     window 1:  the second node's table valid, the first node's
 * disabled
 *
 * and a processor reads its own node's identification through BOTH
 * windows,
 * naming no node. The pair says which table decoded it:
 *
 *     (identification, 0)   decoded by the first node's table   --
 * correct
 *     (0, identification)   decoded by the reader's own table   -- the
 * fault
 *
 * The pair is what makes it sound. A single window would rest on
 * zero-versus-not-zero, which any stuck value fakes; a stuck zero fails
 * the
 * first window and a stuck identification fails the second, so no one
 * value
 * produces either pair.
 *
 * And the first node's processors are a control that costs nothing:
 * their own
 * table IS the first node's, so they must read (identification, 0) on a
 * broken machine as well as a corrected one. If they do not, the probe
 * is
 * measuring something other than what it thinks, and it refuses a
 * verdict
 * rather than reporting one.
 *
 * Requires the two-node four-processor configuration, for the reason
 * every
 * cross-node
 * check does: one node has no second table to be decoded by.
 */

#include "rom.h"

const char rom_name[] = "bwinnode";

#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define CCA_UNCACHED    2
#define ATTR_XTALK      1

#define HUB_LOCAL       0x01000000ull   /* this reader's own Hub */
#define BWIN_BASE       0x20000000ull   /* the seven big windows */
#define BWIN_SIZE       0x20000000ull   /* 512 MiB each */

/*
 * This node's own Hub, whichever node that is.
 */
#define OWN(off)        XKPHYS(CCA_UNCACHED, ATTR_XTALK, HUB_LOCAL + (off))
/*
 * A big window, naming no node -- the address under test.
 */
#define NAMELESS(n) \
    XKPHYS(CCA_UNCACHED, ATTR_XTALK, BWIN_BASE + (n) * BWIN_SIZE)

#define MD_SLOT_ID_USTAT     0x220048
#define MD_SLOT_MASTER       (1ull << 7)  /* set on the first node only */

/*
 * Vector routing: how the PROM assigns a Hub a node number it does not
 * yet
 * have (docs/datasheets/hub-network-interface.md, "Vector routing").
 * Route
 * 0x0 reaches the device immediately connected to the local Hub -- the
 * direct peer, never the local Hub itself -- so this is only ever
 * issued by
 * the first node, and only once assigned does the second node's Hub
 * answer
 * to a node-qualified address at all.
 */
#define SN0_NI_STATUS             0x600000
#define SN0_NI_STATUS_REGION_SIZE (1ull << 17)
#define SN0_NI_STATUS_NODE_SHIFT  8
#define SN0_NI_VECTOR_PARMS       0x600200
#define SN0_NI_VECTOR             0x600208
#define SN0_NI_VECTOR_DATA        0x600210
#define SN0_PIOTYPE_WRITE         1

#define IO_BWIN_TABLE        0x400160
#define IO_BWIN_STRIDE       8
/* Widget 8, first slice, memory. */
#define BWIN_AT_BRIDGE       (BRIDGE_WIDGET << 8)
/* Asks for I/O space; left reaching nothing. */
#define BWIN_DISABLED        (1ull << 12)

/*
 * The Bridge and how to read it, taken from the ROM that already reads
 * this
 * device rather than derived again: it answers on widget 8, and each of
 * its
 * registers is 32 bits in the low half of an eight-byte slot, so the
 * data is
 * four bytes into the slot on this big-endian machine. Its
 * identification
 * cannot read zero -- a bit of it is wired to one -- which is what
 * makes
 * "something answered" distinguishable from "nothing did".
 */
#define BRIDGE_WIDGET        8ull
#define REG_DATA             4
#define BRIDGE_PART          0xc002u
#define BRIDGE_PART_SHIFT    12
#define BRIDGE_PART_MASK     0xffffu

/*
 * The first node releases the second through this, and the second polls
 * it on
 * its OWN Hub -- a local read, which is the point: the second node must
 * not
 * have to reach across to learn that it may start, because reaching
 * across is
 * the very thing under test and it does not work on a machine that
 * folds.
 * This register masks all but its low bits, so the value is a small
 * one.
 */
#define IO_SCRATCH_0         0x400150
#define GO                   0x0A5ull

/*
 * Published by a second-node processor; read by the first node's
 * reporter.
 */
#define NI_SCRATCH(slice)    (0x600100 + 8 * (slice))
#define VERDICT_FIRST_NODE   0x0F1ull   /* decoded by the first node's table */
#define VERDICT_OWN_NODE     0x00Full   /* decoded by the reader's own table */

/*
 * The first node's Hub, named explicitly.
 */
#define AT(node, off)  XKPHYS(CCA_UNCACHED, ATTR_XTALK, \
                              ((uint64_t)(node) << 32) + 0x01800000ull + (off))

#define ASSERT_DECODED_BY_FIRST_NODE  1
#define STATUS_WRONG_MACHINE        200
#define STATUS_TABLE_WRITE_LOST     201
#define STATUS_CONTROL_BROKEN       202

static void put(uint64_t addr, uint64_t val)
{
    /*
     * Volatile preserves accesses through the uncached Hub register
     * mapping.
     */
    *(volatile uint64_t *)(unsigned long)addr = val;
}

static uint64_t get(uint64_t addr)
{
    /*
     * Volatile preserves accesses through the uncached Hub register
     * mapping.
     */
    return *(volatile uint64_t *)(unsigned long)addr;
}

static uint32_t get32(uint64_t addr)
{
    /*
     * Volatile preserves accesses through the uncached Hub register
     * mapping.
     */
    return *(volatile uint32_t *)(unsigned long)addr;
}

static uint64_t entry_addr(unsigned int n)
{
    return OWN(IO_BWIN_TABLE + n * IO_BWIN_STRIDE);
}

void rom_main(void)
{
    unsigned int first_node, slice;
    uint32_t through0, through1;
    uint64_t second[2];

    /*
     * Every window here is reached through the 64-bit unmapped space.
     */
    rom_set_status(rom_get_status() | ROM_ST_KX);

    /*
     * Refuse a machine that cannot express the question. Naming the
     * second
     * node's Hub on a single-node machine reaches a window that is not
     * there
     * and raises a bus error, which would otherwise surface as an
     * unexplained
     * exception in a test nobody would connect to node count.
     */
    first_node = (get(OWN(MD_SLOT_ID_USTAT)) & MD_SLOT_MASTER) != 0;
    slice = (unsigned int)(get(OWN(0x20)) & 0xff);

    /*
     * The second processor of the first node has nothing to do here and
     * must
     * not touch the shared exception record below, which is one struct
     * per
     * node -- two processors probing through it at once race each
     * other.
     */
    if (first_node && slice != 0) {
        for (;;) {
        }
    }

    /*
     * Reset leaves every Hub believing it is node zero, and the probe
     * just
     * below -- everything after it too -- cannot name a second node
     * until
     * one has been assigned a number distinct from the first. Real
     * hardware
     * gets this from the PROM's boot-time fabric discovery over the
     * link;
     * this ROM has no PROM before it, so the first node's first
     * processor
     * repeats the same vector-routed write the PROM would issue, once.
     * Every other processor only waits for it, through a local read of
     * its
     * own Hub's status -- the one thing a second-node processor can
     * check
     * before a node-qualified address has anything to answer.
     */
    if (first_node) {
        if (slice == 0) {
            put(OWN(SN0_NI_VECTOR), 0);
            put(OWN(SN0_NI_VECTOR_DATA), SN0_NI_STATUS_REGION_SIZE |
                (1ull << SN0_NI_STATUS_NODE_SHIFT));
            put(OWN(SN0_NI_VECTOR_PARMS), SN0_NI_STATUS | SN0_PIOTYPE_WRITE);
        }
    } else {
        while (((get(OWN(SN0_NI_STATUS)) >> SN0_NI_STATUS_NODE_SHIFT) &
                0xff) == 0) {
        }
    }

    /*
     * Only this processor probes for the second node, and only it can:
     * on a
     * machine that has one, every processor reads the master bit set
     * and
     * cannot tell. A second node's processor knows by reading the bit
     * clear.
     */
    rom_exc_reset();
    ROM_TRY("ld $27, 0(%[a])",
            [a] "r"((unsigned long)XKPHYS(CCA_UNCACHED, ATTR_XTALK,
                                          (1ull << 32) + HUB_LOCAL)));
    if (rom_exc.count != 0) {
        rom_puts("bwinnode: no second node on this machine -- this test needs\n"
                 "          the two-node four-processor configuration.\n");
        rom_exit(STATUS_WRONG_MACHINE);
    }

    /*
     * ONE PROCESSOR PROGRAMS BOTH TABLES, and it is the first node's,
     * because
     * its accesses arrive where they are addressed on a broken machine
     * as
     * well as a corrected one. Letting each node program its own would
     * leave
     * the two unordered: each would confirm its OWN write and neither
     * would
     * know the other had happened, so a second-node processor could
     * issue the
     * access under test while the first node's window still held its
     * reset
     * value -- pointing at widget zero, where this Bridge is not -- and
     * report
     * a fold that never occurred. That is a race in the probe, and it
     * reads
     * exactly like the defect the probe exists to find.
     *
     * Opposite senses, so the pair read back names the table that
     * decoded it.
     */
    if (first_node) {
        put(entry_addr(0), BWIN_AT_BRIDGE);
        put(entry_addr(1), BWIN_DISABLED);
        put(AT(1, IO_BWIN_TABLE), BWIN_DISABLED);
        put(AT(1, IO_BWIN_TABLE + IO_BWIN_STRIDE), BWIN_AT_BRIDGE);

        /*
         * Read both tables back. A window resets pointing at widget
         * zero --
         * which is VALID -- so a write that silently failed would leave
         * a
         * window reaching a widget rather than reaching nothing, and
         * the
         * probe would read a plausible pair that means nothing.
         */
        if (get(entry_addr(0)) != BWIN_AT_BRIDGE ||
            get(entry_addr(1)) != BWIN_DISABLED ||
            get(AT(1, IO_BWIN_TABLE)) != BWIN_DISABLED ||
            get(AT(1, IO_BWIN_TABLE + IO_BWIN_STRIDE)) != BWIN_AT_BRIDGE) {
            rom_puts("bwinnode: a big-window table write did not take\n");
            rom_exit(STATUS_TABLE_WRITE_LOST);
        }

        put(AT(1, IO_SCRATCH_0), GO);
    } else {
        while ((get(OWN(IO_SCRATCH_0)) & 0xfffull) != GO) {
        }
    }

    /*
     * The addresses under test: no node named, both windows.
     */
    through0 = get32(NAMELESS(0) + REG_DATA);
    through1 = get32(NAMELESS(1) + REG_DATA);

    /*
     * ONE SPEAKER, for the reason the neighbouring test learned the
     * hard way:
     * four processors share one console and the suite reads its verdict
     * from
     * it as a whole line, so two of them reporting at once produce a
     * verdict
     * nobody can parse. The processors that carry the finding are on
     * the
     * second node, and they publish it rather than announce it.
     */
    if (!first_node) {
        put(OWN(NI_SCRATCH(slice)),
            ((through0 >> BRIDGE_PART_SHIFT) & BRIDGE_PART_MASK) == BRIDGE_PART
            && through1 == 0 ? VERDICT_FIRST_NODE : VERDICT_OWN_NODE);
        for (;;) {
            /*
             * published; the first node's processor reports for
             * everyone
             */
        }
    }

    /*
     * The first node's first processor speaks. Its own table IS the
     * first
     * node's, so its pair must read correct on a broken machine as well
     * as a
     * corrected one -- if it does not, this probe is measuring
     * something
     * other than what it believes and declines to report at all.
     */
    if (((through0 >> BRIDGE_PART_SHIFT) & BRIDGE_PART_MASK) != BRIDGE_PART ||
        through1 != 0) {
        rom_puts("bwinnode: the first node's own reads are wrong, so nothing\n"
                 "          this probe says about the other node is trustworthy\n");
        rom_exit(STATUS_CONTROL_BROKEN);
    }

    /*
     * Collect the second node's verdicts. Reading the other node
     * explicitly
     * works from here on both builds -- a first-node processor's
     * accesses
     * arrive where addressed either way, which is what makes it the one
     * that
     * can see the whole machine.
     */
    for (;;) {
        second[0] = get(AT(1, NI_SCRATCH(0)));
        second[1] = get(AT(1, NI_SCRATCH(1)));
        if (second[0] != 0 && second[1] != 0) {
            break;
        }
    }

    rom_puts("bwinnode: second node processor 0 decoded by ");
    rom_puts(second[0] == VERDICT_FIRST_NODE ?
             "the first node\n" : "ITS OWN NODE\n");
    rom_puts("bwinnode: second node processor 1 decoded by ");
    rom_puts(second[1] == VERDICT_FIRST_NODE ?
             "the first node\n" : "ITS OWN NODE\n");

    rom_assert_eq(ASSERT_DECODED_BY_FIRST_NODE, VERDICT_FIRST_NODE, second[0]);
    rom_assert_eq(ASSERT_DECODED_BY_FIRST_NODE, VERDICT_FIRST_NODE, second[1]);
}
