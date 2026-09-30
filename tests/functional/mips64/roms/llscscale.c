/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Does load-linked and store-conditional throughput scale with
 * processors?
 *
 * Every processor runs the same two loops: a long run of load-linked
 * and
 * store-conditional increments of a word nobody else touches, then a
 * short
 * run on a word its node's other processor is hammering too. The first
 * loop
 * is embarrassingly parallel -- four processors doing it should take
 * about as long as one. The native QEMU ROM test
 * (`func-mips64-sgi_roms`, registered by
 * `tests/functional/mips64/test_sgi_roms.py`) has its duration recorded in
 * Meson's test results; the framework does not calculate a throughput
 * ratio.
 *
 * NOTHING PASSES OR FAILS ON THAT NUMBER, which was settled by
 * measuring it
 * rather than by being careful with it. On a 32-processor host shared
 * with
 * other work, one unchanged build produced whole-run ratios of 1.28,
 * 3.15,
 * 3.90 and 5.86, and the build this exists to tell it apart from
 * produced
 * 4.65 -- inside that spread. Watching the emulator through a
 * four-processor
 * run showed its four threads holding about an eighth of a processor
 * each,
 * so the division was reading the host's scheduler rather than the
 * machine.
 * What this ROM decides, it decides through the assertions below, and
 * no
 * amount of host load can move those.
 *
 * It exists because the model has been on both sides of that ratio.
 * Making
 * every R10000 load-linked and store-conditional leave translated
 * execution
 * for the emulator's stop-the-world exclusive section is correct and is
 * also
 * a global serialiser: no two processors can be inside one at the same
 * time,
 * so the private loop stops being parallel and the machine gets slower
 * the
 * more processors it has. An eight-processor operating-system guest
 * thrashed
 * at about 45,000 voluntary context switches per second per processor.
 * That
 * is a real measurement but a terrible regression test -- it needs an
 * operating system, an installed disk and a quarter of an hour, and a
 * guest
 * sitting in firmware turns out to be contending for something else
 * entirely.
 * This is the same question with nothing underneath it: two runs of one
 * image, a ratio, and an answer in seconds.
 *
 * WHAT IT ASSERTS is not the timing. Wall time belongs to the runner,
 * which
 * can compare two runs; a ROM sees one. What the ROM asserts is that
 * the
 * arithmetic survived, which is what makes a fast run meaningful: every
 * private word reached its full count, and the shared word equals the
 * number
 * of processors times the shared count. A model that got its speed by
 * losing
 * atomicity fails the shared word, and that failure is the
 * discriminating
 * one -- the private words would still be right, because nothing
 * contends
 * for them.
 *
 * WHY THE TWO LOOPS ARE TIMED SEPARATELY AND PRINTED. The private loop
 * is
 * the one whose time should fall as processors are added; the shared
 * loop
 * cannot scale on any implementation, because the processors really are
 * contending for one word, and mistaking that honest contention for a
 * serialisation defect is the easiest wrong conclusion available here.
 * So
 * each phase is measured on its own between barriers and the reporter
 * prints
 * both, and the shared loop is deliberately a small fraction of the
 * private
 * one so that it cannot move the whole-run ratio much either way.
 *
 * WHY THE MEASURED LOOPS TOUCH NOTHING BUT NODE MEMORY. Every uncached
 * Hub
 * register access in this machine is serialised against every other
 * one, so
 * a probe, a published verdict or a spin on a scratch register inside a
 * timed
 * loop would be measuring that serialisation rather than the
 * instructions
 * under test -- and it would look equally bad whichever way the
 * instructions
 * behaved. All of that is kept outside the two phases, the spins that
 * do read
 * a Hub register pause between polls, and what remains inside is
 * load-linked and store-conditional against node memory through the
 * cached
 * view.
 *
 * The MIPS64 instruction set reference (MD00087 revision 6.06)
 * for
 * the store-conditional rules this leans on -- on success the word is
 * written
 * and one is placed in the destination register, on failure memory is
 * not
 * modified and zero is placed there -- and its restriction of atomic
 * read-modify-write to cached locations, which is why every access to a
 * counter here goes through the cached view exactly as the llsc ROM
 * does.
 * Register offsets and their meanings come from the Hub
 * processor-interface
 * datasheet's processor status and control table and from its network-
 * and
 * I/O-interface scratch registers.
 *
 * R10000 ONLY, for the reason the startup code gives: the Hub is
 * reached
 * through an address carrying an uncached-space attribute in two bits
 * the
 * R4000 reads as physical address, so on that model the first read here
 * takes an address error before any counting starts.
 */

#include "rom.h"

const char rom_name[] = "llscscale";

/*
 * Chosen so a single-processor run takes a couple of seconds of wall
 * time --
 * long enough that process startup, image load and the host's own noise
 * are
 * a small part of what is being divided, short enough to sit well
 * inside the
 * runner's per-ROM budget on a busy machine. The shared count is a
 * twentieth
 * of the private one on purpose: contention for a single word cannot
 * scale
 * even on a correct model, so leaving it the larger half would put a
 * floor
 * under the whole-run ratio that has nothing to do with the defect
 * being
 * watched for. It is sized to catch a lost increment, not to be fast.
 */
#define PRIVATE_ITERS   2000000ull
#define SHARED_ITERS     100000ull

#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define CCA_UNCACHED    2
#define ATTR_XTALK      1               /* the attribute that means crosstalk */

#define HUB_LOCAL       0x01000000ull   /* "my own Hub", whoever is asking */
#define HUB_NODE        0x01800000ull   /* "the Hub of the node I name" */
#define NODE(n)         ((uint64_t)(n) << 32)

#define OWN(off)        XKPHYS(CCA_UNCACHED, ATTR_XTALK, HUB_LOCAL + (off))
#define AT(node, off) \
    XKPHYS(CCA_UNCACHED, ATTR_XTALK, NODE(node) + HUB_NODE + (off))

/*
 * Processor status and control, from the Hub processor-interface
 * datasheet.
 */
#define PI_PROCESSOR_NUMBER 0x000020    /* which of its node's two is reading */
/* Bit 0: the first socket is populated. */
#define PI_CPU_PRESENT_A    0x000040
/* Bit 0: the second socket is populated. */
#define PI_CPU_PRESENT_B    0x000048

/*
 * Two pairs of scratch registers, and which pair carries what is not
 * arbitrary. The markers below are written ACROSS nodes; everything
 * else is
 * written only to the writer's own Hub. Keeping them apart is what
 * stops a
 * marker aimed at another node from landing on the register that node's
 * own
 * processor is publishing through, which is the mistake the foldback
 * ROM
 * records having made.
 */
#define IO_SCRATCH          0x400150    /* cross-node markers */
#define NI_SCRATCH(slice)   (0x600100 + 8 * (slice))

/*
 * The handshake and the verdicts share the network-interface pair, one
 * register per processor, because there is no third pair and because
 * they
 * are ordered: a processor's register carries its arrival, then for the
 * first processor its release of the second, and only after the node's
 * last
 * barrier its verdict. The values are distinct so a reader can tell
 * which
 * stage it is looking at, and the markers sit in the other pair where a
 * remote write cannot reach them.
 */
#define ARRIVED         0x0A1ull        /* startup clearing is past */
#define READY           0x0A5ull        /* node counters are initialised */

#define VERDICT_PASS    0x0E1ull        /* every count came out right */
#define VERDICT_PRIVATE 0x0F2ull        /* a private word missed its total */
#define VERDICT_SHARED  0x0F3ull        /* shared increments differ */

/*
 * Markers fit the I/O scratch register's writable mask and name their
 * node.
 */
#define MARK(node)      (0x500ull + (node))

/*
 * How many nodes this ROM will look for. The runner's two-node
 * configuration
 * supplies
 * two; the probe below stops at the first node that is not there, so
 * this is
 * a bound on a loop rather than a claim about the machine.
 */
#define MAX_NODES       4
#define MAX_SLICES      2

/*
 * Distinct from any assertion number: the machine, not the contract.
 */
#define ROM_STATUS_NO_FIRST_PROCESSOR 200

/*
 * One assertion, made once, by one processor, for the reason the
 * foldback ROM
 * measured: the processors share one console and the runner reads its
 * verdict
 * out of it as a whole anchored line, so two of them failing at once
 * produce
 * a line nobody can parse. Every other processor publishes and parks.
 */
#define ASSERT_EVERY_COUNT_HELD  1

/*
 * The uncached and cached views of the compatibility segments differ in
 * one
 * address bit.
 */
#define UNCACHED_BIT 0x20000000ull
#define CACHED(p) ((uint64_t)(unsigned long)(p) & ~UNCACHED_BIT)

/*
 * One counter alone in each 128-byte secondary-cache block. Packed
 * together
 * they would share a block, and then the private loop would contend
 * after
 * all -- which is exactly the thing this ROM is trying to measure the
 * absence
 * of. The shared word and the barriers get blocks of their own for the
 * same
 * reason: a barrier every processor is writing must not sit next to the
 * word
 * whose uncontended increments are being timed.
 */
struct counter_block {
    uint64_t n;
    uint64_t pad[15];
};

#define BARRIER_PRIVATE_START   0
#define BARRIER_SHARED_START    1
#define BARRIER_COUNTS_FINAL    2
#define BARRIERS                3

/*
 * Volatile keeps shared-memory accesses emitted; sync instructions
 * order
 * them.
 */
static volatile struct counter_block privates[MAX_SLICES]
    __attribute__((aligned(128)));
/*
 * Volatile keeps shared-memory accesses emitted; sync instructions
 * order
 * them.
 */
static volatile struct counter_block shared_word __attribute__((aligned(128)));
/*
 * Volatile keeps shared-memory accesses emitted; sync instructions
 * order
 * them.
 */
static volatile struct counter_block barriers[BARRIERS]
    __attribute__((aligned(128)));
/*
 * Volatile keeps shared-memory accesses emitted; sync instructions
 * order
 * them.
 */
static volatile uint64_t published_nodes;
/*
 * Volatile keeps shared-memory accesses emitted; sync instructions
 * order
 * them.
 */
static volatile uint64_t published_cpus;

static void put(uint64_t addr, uint64_t val)
{
    /*
     * Volatile keeps shared-memory accesses emitted; sync instructions
     * order
     * them.
     */
    *(volatile uint64_t *)(unsigned long)addr = val;
}

static uint64_t get(uint64_t addr)
{
    /*
     * Volatile keeps shared-memory accesses emitted; sync instructions
     * order
     * them.
     */
    return *(volatile uint64_t *)(unsigned long)addr;
}

/*
 * A doubleword read through an address the caller supplies, so that
 * every
 * access to a counter goes through the cached view.
 */
static uint64_t peek_double(uint64_t addr)
{
    uint64_t v;

    __asm__ __volatile__("ld %0, 0(%1)" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static void poke_double(uint64_t addr, uint64_t v)
{
    __asm__ __volatile__("sd %1, 0(%0)" : : "r"(addr), "r"(v) : "memory");
}

/*
 * A pause between two reads of the same Hub register, in instructions
 * that
 * reach nothing. A spin that re-reads a Hub register as fast as it can
 * is a
 * load on every processor in the machine, not just on the one spinning,
 * so
 * the ones still working would be slowed by the ones already waiting.
 */
static void pause_between_polls(void)
{
    unsigned int i;

    for (i = 0; i < 4096; i++) {
        __asm__ __volatile__("");
    }
}

/*
 * The loop under measurement. The retry is not decoration: a
 * store-conditional
 * may fail for any reason the implementation likes, and a run that
 * dropped an
 * increment because it took the failure for a success would show up as
 * a
 * wrong total rather than as a slow one.
 */
static void llsc_add(uint64_t addr, uint64_t times)
{
    uint64_t i;

    for (i = 0; i < times; i++) {
        uint64_t seen, stored;

        do {
            __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                                 "lld %[seen], 0(%[p])\n\t"
                                 "daddiu %[stored], %[seen], 1\n\t"
                                 "scd %[stored], 0(%[p])\n\t"
                                 ".set pop"
                                 : [seen] "=&r"(seen), [stored] "=&r"(stored)
                                 : [p] "r"(addr)
                                 : "memory");
        } while (stored == 0);
    }
}

/*
 * Every processor of this node reaches the named barrier before any of
 * them
 * leaves it, built out of the instructions under test and out of node
 * memory
 * alone. Without one before each phase the processors would not start
 * together and a phase's measured time would include another's head
 * start.
 */
static void node_barrier(unsigned int which, unsigned int cpus)
{
    uint64_t addr = CACHED(&barriers[which].n);

    llsc_add(addr, 1);
    while (peek_double(addr) < cpus) {
    }
}

static int is_verdict(uint64_t published)
{
    uint64_t v = published & 0xff;

    return v == VERDICT_PASS || v == VERDICT_PRIVATE || v == VERDICT_SHARED;
}

/*
 * Whether a Hub answers at all for the named node, which is how this
 * ROM
 * learns how many there are: a node that is not there has no window,
 * and
 * naming it raises a bus error rather than reading anything.
 */
static int node_exists(unsigned int node)
{
    rom_exc_reset();
    ROM_TRY("ld $27, 0(%[a])", [a] "r"((unsigned long)AT(node, IO_SCRATCH)));
    return rom_exc.count == 0;
}

/*
 * How long a phase took, from the processor's own cycle counter. On
 * this
 * model that counter is derived from the emulator's virtual clock
 * rather
 * than from retired instructions, so a difference across a phase is
 * host
 * wall time in the counter's units and is not a cycle count.
 *
 * IT IS 32 BITS AND IT WRAPS, which on a loaded host is not a corner
 * case.
 * A four-processor phase has been measured past the wrap: the same arm
 * run
 * twice reported 2,845,845,973 ticks once and 192,669,425 the next
 * time,
 * for the same image on the same machine. Unsigned subtraction is exact
 * for a phase shorter than one wrap and says nothing useful beyond it.
 * So
 * these two numbers are a diagnostic for telling the private phase from
 * the shared one on a quiet host, and a verdict rests on the wall times
 * the runner measures instead.
 */
static uint32_t ticks_between(uint32_t before, uint32_t after)
{
    return after - before;
}

void rom_main(void)
{
    unsigned int slice, cpus, nodes, node, s;
    uint64_t seen[MAX_NODES][MAX_SLICES];
    unsigned int seen_cpus[MAX_NODES];
    uint64_t verdict, published;
    uint32_t at_private, after_private, at_shared, after_shared;

    /*
     * 64-bit kernel addressing on: every Hub window here is reached
     * through
     * the 64-bit unmapped space and nothing below it. Without this the
     * first
     * access takes an address error instead of reaching the Hub.
     */
    rom_set_status(rom_get_status() | ROM_ST_KX);

    slice = (unsigned int)(get(OWN(PI_PROCESSOR_NUMBER)) & 1);

    /*
     * ANNOUNCE BEFORE TOUCHING ANY COUNTER, and this ordering is the
     * whole
     * reason the handshake lives in Hub registers rather than in
     * memory.
     * Every processor runs the startup code, and the startup code
     * clears
     * writable memory -- so a processor still in it will wipe a counter
     * a
     * processor already past it has been incrementing, and the run
     * fails a
     * count on a machine that did nothing wrong. A flag in memory
     * cannot fix
     * that, because the clearing is what would erase the flag. A
     * scratch
     * register is the one piece of per-node storage the startup code
     * never
     * writes.
     */
    put(OWN(NI_SCRATCH(slice)), ARRIVED);

    if (slice == 0) {
        /*
         * Only the first processor of a node probes anything. The
         * exception
         * record the probe below reads is one structure per node, so
         * two
         * processors arming and inspecting it at once race each other
         * -- the
         * foldback ROM carries that finding. The second waits for a
         * published flag instead.
         */
        if (!(get(OWN(PI_CPU_PRESENT_A)) & 1)) {
            rom_puts("llscscale: the Hub reports no first processor while one\n"
                     "           is executing this, so nothing it says about\n"
                     "           the node's population can be believed.\n");
            rom_exit(ROM_STATUS_NO_FIRST_PROCESSOR);
        }
        cpus = 1 + (unsigned int)(get(OWN(PI_CPU_PRESENT_B)) & 1);

        /*
         * Wait out the other processor's memory clearing before undoing
         * it.
         */
        for (s = 1; s < cpus; s++) {
            while (get(OWN(NI_SCRATCH(s))) != ARRIVED) {
                pause_between_polls();
            }
        }

        for (nodes = 1; nodes < MAX_NODES && node_exists(nodes); nodes++) {
        }

        for (s = 0; s < MAX_SLICES; s++) {
            poke_double(CACHED(&privates[s].n), 0);
        }
        poke_double(CACHED(&shared_word.n), 0);
        for (s = 0; s < BARRIERS; s++) {
            poke_double(CACHED(&barriers[s].n), 0);
        }
        published_cpus = cpus;
        published_nodes = nodes;

        /*
         * The counters and the two published numbers must be in memory
         * before the release that says they are.
         */
        __asm__ __volatile__("sync" : : : "memory");
        put(OWN(NI_SCRATCH(0)), READY);
    } else {
        while (get(OWN(NI_SCRATCH(0))) != READY) {
            pause_between_polls();
        }
        __asm__ __volatile__("sync" : : : "memory");
        cpus = (unsigned int)published_cpus;
        nodes = (unsigned int)published_nodes;
    }

    /*
     * The measured work, and nothing but it between the timestamps.
     * Node
     * memory is node-local at this point -- no firmware has run, and
     * the
     * cached alias covers the reader's own node -- so the shared word
     * is one
     * word per node and the contention for it is between that node's
     * processors.
     */
    node_barrier(BARRIER_PRIVATE_START, cpus);
    at_private = rom_get_count();
    llsc_add(CACHED(&privates[slice].n), PRIVATE_ITERS);
    after_private = rom_get_count();

    node_barrier(BARRIER_SHARED_START, cpus);
    at_shared = rom_get_count();
    llsc_add(CACHED(&shared_word.n), SHARED_ITERS);
    after_shared = rom_get_count();

    /*
     * Nothing may read a total until every processor has finished
     * adding to
     * it, or an early reader reports its own earliness as a shortfall.
     */
    node_barrier(BARRIER_COUNTS_FINAL, cpus);

    if (slice != 0) {
        /*
         * Past the last barrier the first processor of this node has
         * what it
         * needs, and this one has nothing further to add. Its own
         * register
         * is safe to overwrite now: the release value it was waiting on
         * was
         * consumed before it started counting, and the barrier proves
         * that.
         */
        put(OWN(NI_SCRATCH(slice)), VERDICT_PASS);
        for (;;) {
        }
    }

    verdict = VERDICT_PASS;
    for (s = 0; s < cpus; s++) {
        if (peek_double(CACHED(&privates[s].n)) != PRIVATE_ITERS) {
            verdict = VERDICT_PRIVATE;
        }
    }
    /*
     * Checked last so it wins: a model that lost atomicity is what this
     * ROM
     * is worth running for, and the private words would look perfect on
     * one.
     */
    if (peek_double(CACHED(&shared_word.n)) != cpus * SHARED_ITERS) {
        verdict = VERDICT_SHARED;
    }

    /*
     * The population travels with the verdict, so the reporter knows
     * how many
     * processors of a node it is waiting for without reading that
     * node's
     * registers to find out.
     */
    put(OWN(NI_SCRATCH(0)), verdict | ((uint64_t)cpus << 8));

    /*
     * ELECT ONE REPORTER, by the method the foldback ROM arrived at,
     * generalised past two nodes. Each first processor writes a marker
     * naming each node into that node's I/O scratch, then reads its own
     * back:
     * the first node's marker is what its OWN Hub holds only if its own
     * Hub
     * is the first node's. Every first processor writes the same value
     * to the
     * same register, so no interleaving changes any reader's answer.
     *
     * It is worth more here than a node-identity bit would be, which
     * the Hub
     * does carry. The reporter's next act is to read every other node's
     * registers, and this election has already established that its
     * accesses
     * arrive where it addresses them -- on a machine where they did
     * not, it
     * would report the machine it can see rather than the machine there
     * is.
     * With one node the loop writes one marker and the only first
     * processor
     * reads it back.
     */
    for (node = 0; node < nodes; node++) {
        put(AT(node, IO_SCRATCH), MARK(node));
    }
    if (get(OWN(IO_SCRATCH)) != MARK(0)) {
        for (;;) {
            /*
             * verdict published; the first node's processor speaks
             */
        }
    }

    for (node = 0; node < nodes; node++) {
        while (!is_verdict(published = get(AT(node, NI_SCRATCH(0))))) {
            pause_between_polls();
        }
        seen[node][0] = published & 0xff;
        seen_cpus[node] = (unsigned int)(published >> 8) & 0xff;
        for (s = 1; s < seen_cpus[node] && s < MAX_SLICES; s++) {
            while (!is_verdict(published = get(AT(node, NI_SCRATCH(s))))) {
                pause_between_polls();
            }
            seen[node][s] = published & 0xff;
        }
    }

    for (node = 0; node < nodes; node++) {
        for (s = 0; s < seen_cpus[node] && s < MAX_SLICES; s++) {
            rom_puts("llscscale: node ");
            rom_puthex(node);
            rom_puts(" processor ");
            rom_puthex(s);
            rom_puts(" verdict ");
            rom_puthex(seen[node][s]);
            rom_puts(seen[node][s] == VERDICT_PASS ? " counts held\n"
                                                   : " COUNT WRONG\n");
        }
    }

    /*
     * The reporter's own two phases, which is what a ratio taken across
     * two
     * runs should be read against. Every processor ran the same loops,
     * so one
     * processor's split describes the run; printing all of them would
     * need
     * somewhere to put numbers this wide, and the scratch registers are
     * already carrying the verdicts.
     */
    rom_puts("llscscale: private phase ");
    rom_puthex(ticks_between(at_private, after_private));
    rom_puts(" ticks, shared phase ");
    rom_puthex(ticks_between(at_shared, after_shared));
    rom_puts(" ticks\n");

    for (node = 0; node < nodes; node++) {
        for (s = 0; s < seen_cpus[node] && s < MAX_SLICES; s++) {
            rom_assert_eq(ASSERT_EVERY_COUNT_HELD, VERDICT_PASS, seen[node][s]);
        }
    }
}
