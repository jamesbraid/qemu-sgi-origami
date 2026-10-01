/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Each processor increments a private word, then contends on a shared
 * word. Assertions check the final counts. Timing is printed for
 * comparison between runs but does not determine pass or failure; host
 * scheduling and the 32-bit counter make it unsuitable as an assertion.
 * Hub polling and reporting stay outside the measured loops.
 */

#include "rom.h"

const char rom_name[] = "llscscale";

/* Keep shared contention short relative to the private workload. */
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

/* Processor number and population registers. */
#define PI_PROCESSOR_NUMBER 0x000020    /* which of its node's two is reading */
/* Bit 0: the first socket is populated. */
#define PI_CPU_PRESENT_A    0x000040
/* Bit 0: the second socket is populated. */
#define PI_CPU_PRESENT_B    0x000048

/* I/O scratch carries cross-node markers; network scratch carries status. */
#define IO_SCRATCH          0x400150    /* cross-node markers */
#define NI_SCRATCH(slice)   (0x600100 + 8 * (slice))

/* Per-processor scratch values distinguish startup, release, and verdict. */
#define ARRIVED         0x0A1ull        /* startup clearing is past */
#define READY           0x0A5ull        /* node counters are initialised */

#define VERDICT_PASS    0x0E1ull        /* every count came out right */
#define VERDICT_PRIVATE 0x0F2ull        /* a private word missed its total */
#define VERDICT_SHARED  0x0F3ull        /* shared increments differ */

/* Encode node identity in each cross-node marker. */
#define MARK(node)      (0x500ull + (node))

/* Bound node discovery; probing stops at the first absent node. */
#define MAX_NODES       4
#define MAX_SLICES      2

/* Use a status code distinct from assertion numbers. */
#define ROM_STATUS_NO_FIRST_PROCESSOR 200

/* One processor reports the shared console assertion. */
#define ASSERT_EVERY_COUNT_HELD  1

/* Cached and uncached compatibility-segment addresses differ by one bit. */
#define UNCACHED_BIT 0x20000000ull
#define CACHED(p) ((uint64_t)(unsigned long)(p) & ~UNCACHED_BIT)

/* Separate cache blocks keep private counters and barriers uncontended. */
struct counter_block {
    uint64_t n;
    uint64_t pad[15];
};

#define BARRIER_PRIVATE_START   0
#define BARRIER_SHARED_START    1
#define BARRIER_COUNTS_FINAL    2
#define BARRIERS                3

/* Volatile preserves shared accesses; sync orders them. */
static volatile struct counter_block privates[MAX_SLICES]
    __attribute__((aligned(128)));
/* Volatile preserves shared accesses; sync orders them. */
static volatile struct counter_block shared_word __attribute__((aligned(128)));
/* Volatile preserves shared accesses; sync orders them. */
static volatile struct counter_block barriers[BARRIERS]
    __attribute__((aligned(128)));
/* Volatile preserves shared accesses; sync orders them. */
static volatile uint64_t published_nodes;
/* Volatile preserves shared accesses; sync orders them. */
static volatile uint64_t published_cpus;

static void put(uint64_t addr, uint64_t val)
{
    /* Volatile preserves shared accesses; sync orders them. */
    *(volatile uint64_t *)(unsigned long)addr = val;
}

static uint64_t get(uint64_t addr)
{
    /* Volatile preserves shared accesses; sync orders them. */
    return *(volatile uint64_t *)(unsigned long)addr;
}

/* Read a doubleword through the caller-supplied cached address. */
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

/* Reduce Hub register traffic while waiting for another processor. */
static void pause_between_polls(void)
{
    unsigned int i;

    for (i = 0; i < 4096; i++) {
        __asm__ __volatile__("");
    }
}

/* Retry failed store-conditionals so every requested increment is counted. */
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

/* Start each measured phase only after all processors reach the barrier. */
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

/* Probe whether the named node Hub window responds. */
static int node_exists(unsigned int node)
{
    rom_exc_reset();
    ROM_TRY("ld $27, 0(%[a])", [a] "r"((unsigned long)AT(node, IO_SCRATCH)));
    return rom_exc.count == 0;
}

/*
 * Count follows model virtual time, not retired instructions. Its 32-bit
 * value can wrap; printed deltas are diagnostic only.
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

    /* Hub register windows use 64-bit kernel addressing. */
    rom_set_status(rom_get_status() | ROM_ST_KX);

    slice = (unsigned int)(get(OWN(PI_PROCESSOR_NUMBER)) & 1);

    /* Publish arrival before touching memory that another startup may clear. */
    put(OWN(NI_SCRATCH(slice)), ARRIVED);

    if (slice == 0) {
        /* Only one processor probes node population and publishes setup. */
        if (!(get(OWN(PI_CPU_PRESENT_A)) & 1)) {
            rom_puts("llscscale: the Hub reports no first processor while one\n"
                     "           is executing this, so nothing it says about\n"
                     "           the node's population can be believed.\n");
            rom_exit(ROM_STATUS_NO_FIRST_PROCESSOR);
        }
        cpus = 1 + (unsigned int)(get(OWN(PI_CPU_PRESENT_B)) & 1);

        /* Wait until other processors finish startup memory clearing. */
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

        /* Publish initialized memory before releasing the other processor. */
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

    /* Only the LL/SC loops fall between each phase’s timestamps. */
    node_barrier(BARRIER_PRIVATE_START, cpus);
    at_private = rom_get_count();
    llsc_add(CACHED(&privates[slice].n), PRIVATE_ITERS);
    after_private = rom_get_count();

    node_barrier(BARRIER_SHARED_START, cpus);
    at_shared = rom_get_count();
    llsc_add(CACHED(&shared_word.n), SHARED_ITERS);
    after_shared = rom_get_count();

    /* Read totals only after every processor finishes its increments. */
    node_barrier(BARRIER_COUNTS_FINAL, cpus);

    if (slice != 0) {
        /* The last barrier makes this processor’s scratch slot reusable. */
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
    /* Check shared total last so lost updates determine the verdict. */
    if (peek_double(CACHED(&shared_word.n)) != cpus * SHARED_ITERS) {
        verdict = VERDICT_SHARED;
    }

    /* Include the processor count so the reporter knows how many verdicts
     * to read. */
    put(OWN(NI_SCRATCH(0)), verdict | ((uint64_t)cpus << 8));

    /* Elect the first processor on node zero to report all results. */
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

    /* Print phase timings for diagnosis; assertions use counter totals. */
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
