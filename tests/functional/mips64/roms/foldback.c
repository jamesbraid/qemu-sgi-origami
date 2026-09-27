/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Cross-node Hub addressing: does an access naming another node reach
 * it?
 *
 * A processor names a Hub other than its own by putting the target node
 * number in the address and using the node window. This checks that
 * such an
 * access arrives where it was addressed, in both directions -- a load
 * and a
 * store -- from every processor in the machine.
 *
 * It exists because that is measurably not what happens. Every access a
 * second-node processor addressed to the first node was delivered to
 * its own
 * node's Hub instead: 226 of 226 in one boot, and 440 attempts with no
 * crossing in another. An interrupt sent that way reaches the wrong
 * processor, which finds nothing to do, while the intended target is
 * never
 * told anything.
 *
 * Why a ROM and not a register test: a test driving the machine's
 * global
 * memory exercises a map that is CORRECT here. The fault lives in the
 * per-processor view, which only a processor can use, so a register
 * test of
 * this passes on a machine that still misroutes every cross-node
 * access. It
 * would be worse than no test, because it would report that the
 * contract
 * holds.
 *
 * THREE THINGS THIS DELIBERATELY DOES NOT DO, each of which would also
 * pass
 * on a broken machine:
 *
 *   - It never reads through the global map, for the reason above.
 *   - It never asserts that a misdirected access lands on its sender.
 * The
 *     misdirection preserves the register offset, so a processor aiming
 * at
 *     the other node's second slice lands on its OWN node's second
 * slice --
 *     a sibling rather than itself. "The sender interrupts itself"
 * holds on
 *     a machine that still misroutes across slices.
 *   - It never counts anything. Every access arrives somewhere and only
 * the
 *     destination is wrong, so the totals balance and any count is
 * satisfied
 *     by the fault.
 *
 * NO ROLES, AND WHY THAT MATTERS. Every processor runs this identical
 * path.
 * An earlier version needed one particular processor to act, which
 * cannot be
 * arranged: a bare-metal ROM here cannot learn which node it is on. The
 * register carrying the processor index faults on this processor, the
 * Hub's
 * own number is relative to its node, the node-identity register reads
 * zero
 * without firmware, and local addresses self-decode symmetrically. The
 * fault
 * closes the last route, since it folds every address space and not
 * only
 * this one -- so a second-node processor can observe nothing about the
 * first. The test cannot discover its own roles on the machine it
 * tests.
 *
 * It does not need to. Each processor's verdict follows from ITS OWN
 * program
 * order. Writing A to the first node and B to the second, then reading
 * both
 * back: a processor whose accesses fold has written A and then B to
 * what is
 * physically one register, so it reads B where A should be. Processors
 * of
 * different nodes that collide on a register write the same values to
 * it, so
 * no interleaving changes any verdict.
 *
 * NO SHARED MEMORY EITHER. An earlier version passed results through
 * RAM,
 * which does not work: cross-node memory folds the same way, so the
 * mailbox
 * would have been per-node copies. That version would have failed on a
 * CORRECTED machine, which is the worse direction for a test to be
 * wrong in.
 * Results travel through each Hub's own scratch instead.
 *
 * WHAT THE WIRING TASK MUST SETTLE FIRST, with evidence rather than a
 * prediction. Startup now lets four processors into a library written
 * for
 * one, and slicing the stack was the first consequence rather than the
 * only
 * one. Everything else the library keeps is shared: the exit path,
 * where the
 * first processor to arrive ends the run for all of them; the exception
 * record, one file-scope struct that this test's own probe resets and
 * reads;
 * and the console, which four processors write into and which the suite
 * parses
 * its verdict out of.
 *
 * The console case is not hypothetical -- it has been observed twice on
 * four
 * processors, and a third run came out clean, so it is intermittent:
 *
 *     FAIL FAIL foldbackfoldback assert= assert=0x10x1 ...
 *     ... verdict PASS foldback0xe1
 *
 * The runner requires the verdict as a whole anchored line, so the
 * second of
 * those scores FAIL on a machine where all four processors reported
 * crossing.
 * The failure mode is not messy output, it is a FLAKY FAILURE ON A
 * CORRECT
 * MACHINE -- which discredits the fix this test exists to protect,
 * rarely
 * enough to be argued about.
 *
 * These are safe today only because the runner gives one processor on
 * the
 * wrong machine and the counts stay zero on the right one. Both escapes
 * are
 * properties of the runner, and they end in the same change that lets
 * this
 * test run at all. Per-processor exception state, an elected reporter,
 * or a
 * stated rule that a multiprocessor ROM owns its own coordination --
 * any is
 * fine; the console one has to be settled before the test is
 * registered.
 *
 * NOT IN THE SUITE, AND DELIBERATELY SO. The suite's runner passes no
 * processor count, so every listed ROM runs on one processor -- where
 * this
 * one cannot say anything and refuses. Listing it today would put a
 * refusal
 * in the suite, and listing it without the refusal would have put a bus
 * error
 * there. It joins the suite when the runner can ask for the
 * configuration it
 * needs; until then it is run by hand, and this paragraph is why the
 * suite is
 * not covering it.
 *
 * Requires the four-processor two-node configuration. One node cannot
 * express
 * the fault, and a two-processor machine puts every processor on the
 * first
 * node.
 */

#include "rom.h"

const char rom_name[] = "foldback";

#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define CCA_UNCACHED    2
#define ATTR_XTALK      1               /* the attribute that means crosstalk */

#define HUB_LOCAL       0x01000000ull   /* "my own Hub", whoever is asking */
#define HUB_NODE        0x01800000ull   /* "the Hub of the node I name" */
#define NODE(n)         ((uint64_t)(n) << 32)

#define PI_PROCESSOR_NUMBER 0x000020    /* which of its node's two is reading */

/*
 * The board's own master/slave strap, readable locally on either node
 * without needing a node number of its own -- unlike
 * PI_PROCESSOR_NUMBER
 * above, this one exists before anything has been assigned.
 */
#define MD_SLOT_ID_USTAT     0x220048
#define MD_SLOT_MASTER       (1ull << 7)  /* set on the first node only */

/*
 * Vector routing: how the PROM assigns a Hub a node number it does not
 * yet
 * have (docs/datasheets/hub-network-interface.md, "Vector routing").
 * Route
 * 0x0 reaches the device immediately connected to the local Hub -- the
 * direct peer, never the local Hub itself.
 */
#define SN0_NI_STATUS             0x600000
#define SN0_NI_STATUS_MORE_NODES  (1ull << 18)  /* one node knows of another */
#define SN0_NI_STATUS_REGION_SIZE (1ull << 17)
#define SN0_NI_STATUS_NODE_SHIFT  8
#define SN0_NI_VECTOR_PARMS       0x600200
#define SN0_NI_VECTOR             0x600208
#define SN0_NI_VECTOR_DATA        0x600210
#define SN0_PIOTYPE_WRITE         1

/*
 * Two pairs of scratch registers, both plain storage with no side
 * effects.
 * The markers and the verdicts use DIFFERENT pairs: a processor's
 * marker
 * write to the other node would otherwise land on the register that
 * node's
 * own processor had just published its verdict into, and erase it.
 */
#define IO_SCRATCH(slice)   (0x400150 + 8 * (slice))
#define NI_SCRATCH(slice)   (0x600100 + 8 * (slice))

#define OWN(off)        XKPHYS(CCA_UNCACHED, ATTR_XTALK, HUB_LOCAL + (off))
#define AT(node, off) \
    XKPHYS(CCA_UNCACHED, ATTR_XTALK, NODE(node) + HUB_NODE + (off))

/*
 * Markers fit the scratch register's writable mask, and are distinct on
 * sight.
 */
#define MARK_NODE0      0x0AAAull
#define MARK_NODE1      0x0555ull

#define VERDICT_PASS    0x0E1ull        /* both reads arrived where addressed */
/* The first node's read returned the second's. */
#define VERDICT_FOLD    0x0F0ull
/* Neither shape: the test itself is wrong. */
#define VERDICT_OTHER   0x0EEull
#define VERDICT_NONE    0ull

/*
 * Distinct from any assertion number: the machine, not the contract.
 */
#define ROM_STATUS_WRONG_MACHINE 200

/*
 * One assertion, made once, by one processor. A per-processor assertion
 * was
 * tried and removed: four processors share one console, so several
 * failing at
 * once produce a verdict the suite cannot parse -- measured at five
 * runs in
 * eight unparseable, and the rest visibly interleaved. What that
 * assertion
 * was for is instead built into which processor is allowed to speak.
 */
#define ASSERT_ALL_CROSSED   1

static void put(uint64_t addr, uint64_t val)
{
    /*
     * Volatile preserves accesses through the uncached physical-memory
     * mapping.
     */
    *(volatile uint64_t *)(unsigned long)addr = val;
}

static uint64_t get(uint64_t addr)
{
    /*
     * Volatile preserves accesses through the uncached physical-memory
     * mapping.
     */
    return *(volatile uint64_t *)(unsigned long)addr;
}

void rom_main(void)
{
    unsigned int slice, first_node;
    uint64_t r0, r1, verdict;
    uint64_t seen[2][2];
    unsigned int node, s;

    /*
     * 64-bit kernel addressing on: every Hub window here is reached
     * through
     * the 64-bit unmapped space and nothing below it. Without this the
     * first
     * access takes an address error instead of reaching the Hub.
     */
    rom_set_status(rom_get_status() | ROM_ST_KX);

    /*
     * Reaches the reader's OWN Hub through the local-space window,
     * which is
     * how a processor on either node gets its own number. The
     * address-map fix
     * must keep that window covering this offset: a bounded window
     * based at
     * the space origin rather than at the Hub's local window would not,
     * the
     * read would reach the first node's Hub, and that Hub answers a
     * reader
     * outside its range with ZERO -- so both processors of the second
     * node
     * would take the same slice and quietly share a stack. No fault, no
     * message, and only on more than one processor.
     */
    slice = (unsigned int)(get(OWN(PI_PROCESSOR_NUMBER)) & 1);

    /*
     * Reset leaves every Hub believing it is node zero, and the probe
     * just
     * below -- everything this ROM tests -- cannot name a second node
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
     *
     * The first node's OWN status never gains a node-number bit -- it
     * stays at the reset value of zero, which is already its correct,
     * permanent number, so its second processor cannot wait on that
     * field
     * the way the second node's two processors do. It needs its own
     * local
     * signal instead, published only after the peer's number has
     * landed:
     * the documented "more nodes" bit (bit 18, M mode versus N mode),
     * written locally in program order right after the vector-routed
     * write that assigned the peer's number. Without this, that second
     * processor reaches the probe below with nothing having waited for
     * the peer at all, and loses the race often enough to be measured
     * --
     * intermittent "no second node" failures on a machine that has one.
     */
    first_node = (get(OWN(MD_SLOT_ID_USTAT)) & MD_SLOT_MASTER) != 0;
    if (first_node) {
        if (slice == 0) {
            put(OWN(SN0_NI_VECTOR), 0);
            put(OWN(SN0_NI_VECTOR_DATA), SN0_NI_STATUS_REGION_SIZE |
                (1ull << SN0_NI_STATUS_NODE_SHIFT));
            put(OWN(SN0_NI_VECTOR_PARMS), SN0_NI_STATUS | SN0_PIOTYPE_WRITE);
            put(OWN(SN0_NI_STATUS),
                get(OWN(SN0_NI_STATUS)) | SN0_NI_STATUS_MORE_NODES);
        } else {
            while ((get(OWN(SN0_NI_STATUS)) & SN0_NI_STATUS_MORE_NODES) ==
                   0) {
            }
        }
    } else {
        while (((get(OWN(SN0_NI_STATUS)) >> SN0_NI_STATUS_NODE_SHIFT) &
                0xff) == 0) {
        }
    }

    /*
     * REFUSE RATHER THAN REPORT, if this machine cannot express the
     * fault.
     * The two-node four-processor configuration is required: one node
     * has no
     * second Hub to name, and a two-processor machine puts every
     * processor on
     * the first node.
     *
     * Run on one node the second Hub's window is not there at all, and
     * touching it raises a bus error -- which surfaces as an
     * unexplained
     * exception naming an address, and reads like a regression in
     * something
     * unrelated. Probe for that Hub first and say plainly what is
     * missing.
     *
     * This matters because the suite's runner cannot yet ask for the
     * configuration this test needs, so the wrong-machine case is the
     * LIKELY
     * one rather than a corner.
     */
    rom_exc_reset();
    ROM_TRY("ld $27, 0(%[a])", [a] "r"((unsigned long)AT(1, IO_SCRATCH(0))));
    if (rom_exc.count != 0) {
        rom_puts("foldback: no second node on this machine -- this test needs\n"
                 "          the two-node four-processor configuration, and\n"
                 "          cannot say anything about cross-node addressing\n"
                 "          without it.\n");
        rom_exit(ROM_STATUS_WRONG_MACHINE);
    }

    /*
     * Name each node explicitly, and read back through the same views.
     */
    put(AT(0, IO_SCRATCH(slice)), MARK_NODE0);
    put(AT(1, IO_SCRATCH(slice)), MARK_NODE1);
    r0 = get(AT(0, IO_SCRATCH(slice)));
    r1 = get(AT(1, IO_SCRATCH(slice)));

    if (r0 == MARK_NODE0 && r1 == MARK_NODE1) {
        verdict = VERDICT_PASS;
    } else if (r0 == MARK_NODE1 && r1 == MARK_NODE1) {
        /*
         * The first node's read returned the SECOND node's marker: this
         * processor's accesses were all delivered to its own Hub, so
         * its own
         * two writes went to one register and the later one is what it
         * sees.
         * The value names where the access went.
         */
        verdict = VERDICT_FOLD;
    } else {
        verdict = VERDICT_OTHER;
    }

    /*
     * Publish into this processor's OWN Hub, which it can always reach.
     */
    put(OWN(NI_SCRATCH(slice)), verdict);

    /*
     * EXACTLY ONE processor may speak, on either outcome. Four of them
     * share
     * one console and the suite reads its verdict out of it as a whole
     * line,
     * so two writers interleaving produce a verdict nobody can parse.
     * Both
     * outcomes have been seen doing it: a correct machine scored a
     * failure
     * because its success line came out as "foldback: node PASS
     * foldback",
     * and a broken one lost the value naming the fold when two failing
     * processors reported at once.
     *
     * Electing one needs an identity none of them can discover in
     * general.
     * The processor that can speak is exactly the one whose accesses
     * arrive
     * where it addresses them, and for that processor the question
     * answers
     * itself with two reads and no extra write:
     *
     *   the first node's marker, seen through its OWN Hub, is A only if
     * its
     *   own Hub IS the first node's.
     *
     * The three conditions are one requirement each. Its own verdict
     * must
     * have passed, which is what says its cross-node READS returned
     * what was
     * addressed -- the collection below is nothing but cross-node
     * reads, and
     * a processor whose reads do not arrive would report the machine it
     * can
     * see rather than the machine there is. Its own Hub must be the
     * first
     * node's, which is what makes it one processor rather than two. And
     * the
     * first of a node's pair, so the two cannot both qualify.
     *
     * A processor whose accesses folded wrote both markers into one
     * register
     * and sees B there, so it fails the third test as well as the
     * first, and
     * never describes a machine it is seeing through the fold. On a
     * machine
     * where even that processor
     * folds, no one is elected and the run says nothing rather than
     * saying
     * something wrong. The mechanism is a timeout: every processor
     * parks,
     * nothing calls exit, and the run is scored a failure when its
     * budget
     * expires -- so that case takes the full budget rather than failing
     * promptly, which is worth expecting.
     */
    if (verdict != VERDICT_PASS ||
        slice != 0 ||
        get(OWN(IO_SCRATCH(slice))) != MARK_NODE0) {
        for (;;) {
            /*
             * verdict published; this processor has nothing further to
             * add
             */
        }
    }

    /*
     * The elected processor's own accesses arrive, so its view of both
     * nodes is real and it can collect every verdict.
     */
    for (;;) {
        unsigned int ready = 1;

        for (node = 0; node < 2; node++) {
            for (s = 0; s < 2; s++) {
                seen[node][s] = get(AT(node, NI_SCRATCH(s)));
                if (seen[node][s] == VERDICT_NONE) {
                    ready = 0;
                }
            }
        }
        if (ready) {
            break;
        }
    }

    for (node = 0; node < 2; node++) {
        for (s = 0; s < 2; s++) {
            rom_puts("foldback: node ");
            rom_puthex(node);
            rom_puts(" processor ");
            rom_puthex(s);
            rom_puts(" verdict ");
            rom_puthex(seen[node][s]);
            rom_puts(seen[node][s] == VERDICT_PASS ?
                     " crossed\n" : " DID NOT CROSS\n");
        }
    }

    rom_assert_eq(ASSERT_ALL_CROSSED, VERDICT_PASS, seen[1][0]);
    rom_assert_eq(ASSERT_ALL_CROSSED, VERDICT_PASS, seen[1][1]);
    rom_assert_eq(ASSERT_ALL_CROSSED, VERDICT_PASS, seen[0][0]);
    rom_assert_eq(ASSERT_ALL_CROSSED, VERDICT_PASS, seen[0][1]);
}
