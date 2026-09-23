/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/misc/sgi-l1.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/cpus.h"
#include "system/tcg.h"
#include "trace.h"

#define BEDROCK_REMOTE_REGISTERS_SIZE 0x8000000
#define BEDROCK_L1_UART_OFFSET 0x80
/* The remote form of the hub's register window: offsets 22:0 under bit 23. */
#define BEDROCK_IO_REGISTERS_SIZE 0x800000
#define BEDROCK_PI_MODIFY_OFFSET 0x90
#define BEDROCK_PI1_OFFSET 0x200000

bool sgi_bedrock_hub_registers_permitted(SGIBedrockState *s)
{
    return s->initiator_region < 64 &&
           (s->md.current_cell & (UINT64_C(1) << s->initiator_region));
}

/*
 * The multi-threaded rendezvous hold at LB scratch 1 (H1). See the struct's
 * own comment in sgi-bedrock.h and docs/known-issues.md for what this
 * approximates and why: the stock PROM meets a node's processors at this
 * register, on the assumption that real processors leave reset on the same
 * edge and run continuously, which a host thread does not. A bounded hold
 * restores that assumption for a bounded time, the same shape as
 * sgi-hub.c's RT-mailbox hold (sgi_hub_rt_hold and neighbors, ~line 1440
 * onward): queued work on the held vCPU's own thread, waiting on its halt
 * condition with the BQL released, yielding to other queued work and
 * resuming behind it, and ending on a stop, a deliverable interrupt, a
 * partner that can no longer run, or SGI_BEDROCK_HOLD_MS.
 *
 * This hold differs from the Hub's in shape, not in mechanism: the Hub
 * always holds one processor against one fixed partner. H1 joins across
 * every populated processor of the node -- up to three partners in the
 * four-CPU case -- so a hold's condition is a predicate recomputed fresh on
 * every wake (bedrock_hold_ready) rather than a single tagged event a
 * setter answers once (contrast sgi_hub_rt_release, which flips a specific
 * hold kind). Waking a hold here (bedrock_hold_wake_kind) only kicks it
 * back into its own loop to re-check that predicate.
 *
 * One deliberate deviation from the Hub template: sgi_hub_slice_can_read()
 * requires the partner's pi_cpu_enable, because on SN0 that register
 * really controls whether the Hub's vCPU is scheduled at all
 * (sgi_hub_set_cpu_enable calls through to a halt/unhalt run_on_cpu). On
 * this Origin 300 model every populated processor's vCPU runs
 * unconditionally from sn1_machine_reset() regardless of the Bedrock PI's
 * own cpu-enable register (see hw/mips/sgi/sn1.c); Bedrock's cpu-enable
 * GPIO output is not wired to anything in hw/mips/sgi/sn1-node.c, and
 * cpu-enable-initial-mask leaves exactly one physical slot enabled at
 * reset (sn1-node.c: "Initial C grant is a model approximation"). The very
 * race H1 exists to close is between that one initially-enabled processor
 * and a populated sibling whose own cpu-enable bit is still clear at the
 * moment of the race. Gating bedrock_hold_slot_running() on cpu-enable
 * would therefore exclude the sibling from the join and silently disable
 * the hold in exactly the case it is meant to fix. "Present, running" here
 * is population (processor-mask) plus real vCPU schedulability (not
 * halted, cpu_can_run()); the PI cpu-enable register plays no part.
 *
 * Which vCPU is in which slot comes from the board's processor-slot-order,
 * not from counting fitted slots: see bedrock_resolve_slot_processors().
 */

#define SGI_BEDROCK_HOLD_MS 2000 /* mirrors sgi-hub.c's SGI_HUB_RT_HOLD_MS */

/*
 * The second rendezvous (H3). The stock PROM meets a node's processors
 * again well after reset, during "Copying PROM code to memory", over a
 * software lock word it keeps in a memory/directory register (MD
 * performance counter 0) and an in-RAM per-processor table that no device
 * sees. A processor claims a bit of the lock word -- an atomic
 * read-modify-write it brackets with the node's hub lock, LB scratch 3's
 * read-set-if-zero twin -- checks the table for a peer's publication, and
 * clears the bit and retries when it is not yet there; the peer publishes
 * only once its own separate bring-up reaches that point. While one
 * processor holds the bit, the other spins reading the lock word, waiting
 * for the bit to clear. On real hardware both processors run continuously
 * off one clock, so the holder's check and the poller's wait overlap and
 * resolve in bounded time. Under multi-threaded TCG each processor is its
 * own host thread, and a contended host can leave one thread unscheduled
 * while the other's tight spin keeps its host core busy -- which is exactly
 * what denies the first a turn, so the boot stalls at the copy phase with
 * the lock word's bit set and neither processor advancing
 * (docs/known-issues.md, "The second-rendezvous hub-lock hold";
 * docs/notebooks/origin300-irix-disk-boot-copy-stall-rootcause-20260917.md).
 *
 * H3 restores the shared-clock overlap for a bounded time, in the same
 * queued-work shape as H1 and the SN0 Hub's RT-mailbox hold, and never
 * fabricates a value the guest reads. A write's effect and a scratch-3
 * read-set-if-zero's result are fixed before any park. A perf-counter-0
 * read is the other way around: it is sampled after its park completes,
 * so the pause lets a peer's write land first and the read observes the
 * fresh value instead of a stale one. It acts at the two registers the
 * spin touches:
 *
 *   - The lock word is MD performance counter 0. A processor spinning on it
 *     -- reading it to test a bit, or claiming and releasing a bit in a
 *     retry loop, both of which are seen in the stall -- touches it over and
 *     over while its peer, starved elsewhere, does not touch it at all. So a
 *     processor that has accessed the word SGI_BEDROCK_PERF_SPIN times in a
 *     row with no access from its peer cedes a bounded quantum
 *     (SGI_BEDROCK_HOLD_PERF_YIELD), freeing its host core for that starved
 *     peer to run. The run counts this processor's own reads and writes
 *     alike, so a monopolising claim/release retry loop -- which writes the
 *     word every pass -- still trips it; only the peer's own access resets
 *     it, so genuine hand-off, where the two alternate, never does.
 *     sgi_bedrock_perf_counter_access() is the hook, from read and write.
 *
 *   - A processor whose hub-lock twin read finds LB scratch 3 already held
 *     by a peer is parked until the peer clears it
 *     (SGI_BEDROCK_HOLD_SCRATCH3_HELD), rather than left polling a lock a
 *     descheduled holder cannot release -- the same shape H1 uses at the
 *     reset-edge rendezvous. This covers the brief window the two
 *     processors contend the hub lock itself.
 */
/*
 * Both picked heuristically, not from a measurement: high enough that an
 * ordinary brief critical section never trips a yield, low enough that the
 * first yield still lands promptly in guest-instruction time. Contrast
 * SGI_BEDROCK_HOLD_MS above, which mirrors an existing hold's bound.
 */
#define SGI_BEDROCK_YIELD_MS 2       /* a fairness quantum, not a join bound */
#define SGI_BEDROCK_PERF_SPIN 8192   /* one-sided accesses before a yield */

/*
 * The presence publication (H4). Right after H1's join the stock PROM's
 * processors set off for their first barrier, and the lowest fitted
 * processor of each interface first publishes its interface's fitted
 * processors as bits 4 to 7 of II scratch 0 (A writes A and B, C writes C
 * and D), under the hub lock. Every barrier and pair-master decision from
 * then on reads those bits, and a processor whose bit is clear is treated
 * as absent: its partner does not wait for it, and a processor on the other
 * interface takes the handshake role it would otherwise leave to it. On
 * hardware the publisher, running continuously from the same reset edge,
 * reaches its two stores long before any other processor reaches its first
 * such read. Under multi-threaded TCG the publisher's host thread can be
 * left unscheduled for that whole window, so its peers read the bits as
 * still clear, pass their first barrier without it, and stay one barrier
 * ahead of it for the rest of the boot: the two sides' barriers then pair
 * off by one, harmlessly until the copy barrier, where the peers switch to
 * the PROM's RAM copy while the publisher is still making it and fetch
 * zeros (docs/known-issues.md, "The presence-publication join";
 * docs/notebooks/origin300-copy-phase-stall-presence-race-20260924.md).
 *
 * H4 restores that ordering for a bounded time, in the same queued-work
 * shape as H1, and never fabricates a value the guest reads. It parks the
 * publishers only: a publisher whose own interface has published is parked
 * at the hub lock release that follows, until the other populated
 * interface has published too (SGI_BEDROCK_HOLD_PRESENCE). Its first
 * presence read is of the other interface, in the handshake between the two
 * publishers. Parking it at the release rather than at its stores keeps the
 * hub lock free for the other publisher, whose stores are what the park
 * waits for, so the two publishers cannot wait on each other.
 *
 * The interfaces' other processors (B and D), whose first presence reads
 * are of their own interface's bits, are not parked. Two placements were
 * measured and both lost the parked processor to the PROM's early
 * peer-liveness diagnostic ("peer_pled failed: Other CPU on this node
 * died"): at its second LB scratch 1 write until both interfaces had
 * published (270 to 610 ms), and until its own had (120 to 260 ms). The
 * master runs that check on its partner before it publishes, so a partner
 * parked until the publication cannot pass it, and the publishers' own
 * parks of 17 to 76 ms did (docs/known-issues.md).
 *
 * The join is over once every populated interface has published, so the
 * park cannot fire again until reset; and a park that times out abandons
 * the join for the rest of the reset rather than parking every later
 * release for the bound again, since a publisher that never comes is a
 * processor the PROM's own console will report as failed.
 */

typedef struct SGIBedrockHold {
    SGIBedrockState *bedrock;
    unsigned int slot;
    uint64_t id;
    int64_t start; /* realtime it first ran; a yield keeps the deadline */
} SGIBedrockHold;

static void bedrock_hold_run(CPUState *cs, run_on_cpu_data data);

/* The physical slot (0..3: A, B, C, D) the executing processor is, or -1. */
static int bedrock_hold_current_slot(SGIBedrockState *s)
{
    if (!current_cpu) {
        return -1;
    }
    for (unsigned int physical = 0; physical < SGI_BEDROCK_PROCESSORS;
         physical++) {
        if (s->slot_cpu_index[physical] == current_cpu->cpu_index) {
            return (int)physical;
        }
    }
    return -1;
}

/* A physical slot's CPUState, resolved once and cached; NULL if absent. */
static CPUState *bedrock_hold_cpu(SGIBedrockState *s, unsigned int physical)
{
    if (unlikely(!s->hold_cpu[physical])) {
        if (s->slot_cpu_index[physical] < 0) {
            return NULL;
        }
        s->hold_cpu[physical] = qemu_get_cpu(s->slot_cpu_index[physical]);
    }
    return s->hold_cpu[physical];
}

/*
 * Populated and actually able to run -- see the deviation from the Hub
 * template above for why this does not also check the PI cpu-enable
 * register.
 */
static bool bedrock_hold_slot_running(SGIBedrockState *s,
                                      unsigned int physical)
{
    CPUState *cs;

    if (!(s->processor_mask & (1U << physical))) {
        return false;
    }
    cs = bedrock_hold_cpu(s, physical);
    return cs && !cs->halted && cpu_can_run(cs);
}

/* Whether a hold is worth taking at all: this node has someone to wait on. */
static bool bedrock_hold_applicable(SGIBedrockState *s)
{
    unsigned int running = 0;

    if (!tcg_enabled()) {
        return false;
    }
    for (unsigned int p = 0; p < SGI_BEDROCK_PROCESSORS; p++) {
        if (bedrock_hold_slot_running(s, p)) {
            running++;
        }
    }
    return running >= 2;
}

/* Whether any present, running processor sits on this interface. */
static bool bedrock_interface_populated(SGIBedrockState *s,
                                        unsigned int interface)
{
    for (unsigned int p = 0; p < SGI_BEDROCK_PI_PROCESSORS; p++) {
        if (bedrock_hold_slot_running(s, interface *
                                      SGI_BEDROCK_PI_PROCESSORS + p)) {
            return true;
        }
    }
    return false;
}

/*
 * The interface's publisher: its lowest present, running processor, the
 * same rule the PROM's pair-master decision uses (slot A, or B with A
 * absent). -1 for an unpopulated interface.
 */
static int bedrock_publisher_slot(SGIBedrockState *s, unsigned int interface)
{
    for (unsigned int p = 0; p < SGI_BEDROCK_PI_PROCESSORS; p++) {
        unsigned int physical = interface * SGI_BEDROCK_PI_PROCESSORS + p;

        if (bedrock_hold_slot_running(s, physical)) {
            return (int)physical;
        }
    }
    return -1;
}

/* H4's join: every populated interface has published, or the join is over. */
static bool bedrock_presence_pending(SGIBedrockState *s)
{
    if (s->presence_abandoned) {
        return false;
    }
    for (unsigned int i = 0; i < SGI_BEDROCK_PROCESSORS /
         SGI_BEDROCK_PI_PROCESSORS; i++) {
        if (bedrock_interface_populated(s, i) && !s->presence_seen[i]) {
            return true;
        }
    }
    return false;
}

/* The same, for every populated interface but this slot's own. */
static bool bedrock_presence_other_pending(SGIBedrockState *s,
                                           unsigned int slot)
{
    unsigned int own = slot / SGI_BEDROCK_PI_PROCESSORS;

    if (s->presence_abandoned) {
        return false;
    }
    for (unsigned int i = 0; i < SGI_BEDROCK_PROCESSORS /
         SGI_BEDROCK_PI_PROCESSORS; i++) {
        if (i != own && bedrock_interface_populated(s, i) &&
            !s->presence_seen[i]) {
            return true;
        }
    }
    return false;
}

/* Whether this slot is its own interface's publisher. */
static bool bedrock_presence_publisher(SGIBedrockState *s, unsigned int slot)
{
    return bedrock_publisher_slot(s, slot / SGI_BEDROCK_PI_PROCESSORS) ==
           (int)slot;
}

/*
 * H1's join: every other present, running processor has made its first
 * scratch-1 write since reset.
 */
static bool bedrock_hold_scratch1_ready(SGIBedrockState *s)
{
    for (unsigned int p = 0; p < SGI_BEDROCK_PROCESSORS; p++) {
        if (bedrock_hold_slot_running(s, p) && !s->hold_scratch1_seen[p]) {
            return false;
        }
    }
    return true;
}

static bool bedrock_hold_ready(SGIBedrockState *s, unsigned int slot,
                               SGIBedrockHoldKind kind)
{
    switch (kind) {
    case SGI_BEDROCK_HOLD_SCRATCH3_HELD:
        /* Parked until the peer clears the hub lock it held. */
        return s->lb.scratch[3] == 0;
    case SGI_BEDROCK_HOLD_PERF_YIELD:
        /* A fairness quantum: nothing to wait for but the bound below. */
        return false;
    case SGI_BEDROCK_HOLD_PRESENCE:
        /* A publisher, parked until the other interface publishes too. */
        return !bedrock_presence_other_pending(s, slot);
    case SGI_BEDROCK_HOLD_SCRATCH1:
    default:
        return bedrock_hold_scratch1_ready(s);
    }
}

/*
 * Land a processor's own withheld scratch-1 write, if one is waiting. Every
 * caller is an access by that same processor that must not observe a post
 * it has already taken back.
 */
static void bedrock_hold_flush_scratch1(SGIBedrockState *s,
                                        unsigned int slot)
{
    if (!s->hold_scratch1_withheld_valid[slot]) {
        return;
    }
    s->lb.scratch[1] = s->hold_scratch1_withheld[slot];
    s->hold_scratch1_withheld_valid[slot] = false;
}

static const char *bedrock_hold_kind_name(SGIBedrockHoldKind kind)
{
    switch (kind) {
    case SGI_BEDROCK_HOLD_SCRATCH3_HELD:
        return "scratch3-held";
    case SGI_BEDROCK_HOLD_PERF_YIELD:
        return "perf-yield";
    case SGI_BEDROCK_HOLD_PRESENCE:
        return "presence";
    case SGI_BEDROCK_HOLD_SCRATCH1:
    default:
        return "scratch1";
    }
}

/*
 * Kick every processor currently held on this register, so each re-checks
 * its own join condition; mirrors sgi_hub_rt_release's wake, adapted for an
 * N-way join where the setter cannot itself say which holds now clear.
 */
static void bedrock_hold_wake_kind(SGIBedrockState *s,
                                   SGIBedrockHoldKind kind)
{
    for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
        CPUState *cs;

        if (!s->hold_id[slot] || s->hold_kind[slot] != kind) {
            continue;
        }
        cs = bedrock_hold_cpu(s, slot);
        if (cs) {
            trace_sgi_bedrock_hold_wake(current_cpu ? current_cpu->cpu_index
                                                     : -1, cs->cpu_index,
                                        s->hold_id[slot]);
            qemu_cpu_kick(cs);
        }
    }
}

static void bedrock_hold_start(SGIBedrockState *s, unsigned int slot,
                               SGIBedrockHoldKind kind)
{
    SGIBedrockHold *hold = g_new0(SGIBedrockHold, 1);

    hold->bedrock = SGI_BEDROCK(object_ref(OBJECT(s)));
    hold->slot = slot;
    hold->id = ++s->hold_seq;
    s->hold_id[slot] = hold->id;
    s->hold_kind[slot] = kind;
    trace_sgi_bedrock_hold_begin(current_cpu->cpu_index,
                                 bedrock_hold_kind_name(kind), slot,
                                 qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
                                 hold->id);
    async_run_on_cpu(current_cpu, bedrock_hold_run, RUN_ON_CPU_HOST_PTR(hold));
}

static void bedrock_hold_run(CPUState *cs, run_on_cpu_data data)
{
    SGIBedrockHold *hold = data.host_ptr;
    SGIBedrockState *s = hold->bedrock;
    unsigned int slot = hold->slot;
    SGIBedrockHoldKind kind = s->hold_kind[slot];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t deadline;
    const char *outcome;
    bool yield = false;
    bool timed_out = false;
    int wait_ms;

    if (!hold->start) {
        hold->start = now;
    }
    deadline = hold->start +
               (kind == SGI_BEDROCK_HOLD_PERF_YIELD ?
                SGI_BEDROCK_YIELD_MS : SGI_BEDROCK_HOLD_MS) * SCALE_MS;

    for (;;) {
        now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (s->hold_id[slot] != hold->id) {
            outcome = "answered";
        } else if (bedrock_hold_ready(s, slot, kind)) {
            outcome = "satisfied";
        } else if (cs->stop || cs->unplug) {
            outcome = "preempted by a stop";
        } else if (cpu_has_work(cs)) {
            outcome = "preempted by an interrupt";
        } else if (!cpu_work_list_empty(cs)) {
            /*
             * Work queued on this processor runs before its next
             * instruction, and the 100 Hz promdata poll queues some on
             * every processor. Step aside for it and take the hold up
             * again behind it; the guest cannot come between.
             */
            outcome = NULL;
            yield = true;
        } else if (now >= deadline) {
            outcome = "timed out";
            timed_out = true;
        } else {
            outcome = NULL;
        }

        if (yield) {
            trace_sgi_bedrock_hold_yield(cs->cpu_index, hold->id,
                                         (now - hold->start) / SCALE_US);
            async_run_on_cpu(cs, bedrock_hold_run, data);
            return;
        }
        if (outcome) {
            break;
        }

        wait_ms = (deadline - now) / SCALE_MS + 1;
        qemu_cond_timedwait_bql(cs->halt_cond, wait_ms);
    }

    if (s->hold_id[slot] == hold->id) {
        s->hold_id[slot] = 0;
    }
    /*
     * A join that timed out is over for this reset: the publisher it
     * waited for is not coming, and parking every later release for the
     * bound again would only stretch the boot the PROM is about to report
     * on. Wake the other publisher if it is parked on it too.
     */
    if (timed_out && kind == SGI_BEDROCK_HOLD_PRESENCE &&
        !s->presence_abandoned) {
        s->presence_abandoned = true;
        bedrock_hold_wake_kind(s, SGI_BEDROCK_HOLD_PRESENCE);
    }
    /*
     * Whatever ended it, a withheld scratch-1 write lands before this
     * processor's next instruction: it cannot observe a post it has
     * already taken back.
     */
    bedrock_hold_flush_scratch1(s, slot);
    trace_sgi_bedrock_hold_end(cs->cpu_index, outcome,
                               (now - hold->start) / SCALE_US, hold->id);
    object_unref(OBJECT(s));
    g_free(hold);
}

/*
 * Best-effort round-robin (thread=single) help: nothing can block the
 * single host thread, so kick every processor that still owes the join a
 * write, the way sgi_hub_kick_peer does on a write-side phase change. A
 * kicked processor may then run before this quantum's guest-visible poll
 * budget expires; full round-robin reliability is not claimed -- see
 * docs/known-issues.md.
 */
static void bedrock_hold_kick_pending(SGIBedrockState *s,
                                      SGIBedrockHoldKind kind,
                                      unsigned int self)
{
    for (unsigned int p = 0; p < SGI_BEDROCK_PROCESSORS; p++) {
        CPUState *peer;

        if (p == self || s->hold_scratch1_seen[p] ||
            !bedrock_hold_slot_running(s, p)) {
            continue;
        }
        peer = bedrock_hold_cpu(s, p);
        if (peer && peer != current_cpu) {
            trace_sgi_bedrock_hold_kick(current_cpu->cpu_index,
                                        peer->cpu_index,
                                        bedrock_hold_kind_name(kind));
            qemu_cpu_kick(peer);
        }
    }
}

void sgi_bedrock_hold_scratch1_write(SGIBedrockState *s, uint64_t value)
{
    int cslot = bedrock_hold_current_slot(s);
    unsigned int slot;

    if (cslot < 0) {
        /*
         * qtest, DMA, or an access this node did not issue: ordinary
         * storage, no hold bookkeeping.
         */
        s->lb.scratch[1] = value;
        return;
    }
    slot = (unsigned int)cslot;
    bedrock_hold_flush_scratch1(s, slot); /* self-consistency */

    if (!s->hold_scratch1_seen[slot]) {
        s->lb.scratch[1] = value;
        s->hold_scratch1_seen[slot] = true;
        bedrock_hold_wake_kind(s, SGI_BEDROCK_HOLD_SCRATCH1);
        return;
    }

    if (!bedrock_hold_applicable(s) || bedrock_hold_scratch1_ready(s)) {
        s->lb.scratch[1] = value;
        return;
    }

    s->hold_scratch1_withheld[slot] = value;
    s->hold_scratch1_withheld_valid[slot] = true;
    if (qemu_tcg_mttcg_enabled()) {
        bedrock_hold_start(s, slot, SGI_BEDROCK_HOLD_SCRATCH1);
    } else {
        bedrock_hold_kick_pending(s, SGI_BEDROCK_HOLD_SCRATCH1, slot);
    }
}

/*
 * Only an access this node's own processor issued can take a hold: a qtest
 * backdoor read or a DMA engine carries no vCPU to park, and a standalone
 * child device has no owning node at all. The holds are a multi-threaded-TCG
 * fairness aid; round-robin hands slices around itself, so H3 does not apply
 * there (docs/known-issues.md). Returns the executing slot, or -1.
 */
static int bedrock_h3_slot(SGIBedrockState *s)
{
    int cslot = bedrock_hold_current_slot(s);

    if (cslot < 0 || !qemu_tcg_mttcg_enabled() || !bedrock_hold_applicable(s)) {
        return -1;
    }
    return s->hold_id[cslot] ? -1 : cslot;
}

void sgi_bedrock_perf_counter_access(SGIBedrockState *s)
{
    int cslot = bedrock_h3_slot(s);

    if (cslot < 0) {
        return;
    }

    /*
     * The other processor touching the word is a hand-off: reset the run to
     * this processor. Otherwise this processor is accessing the word again
     * with the other still absent. A short run is ordinary; a full spin's
     * worth with the other never touching it is one processor monopolising
     * the lock while the other is starved, so cede a bounded quantum for the
     * other to run. The run counts this processor's own reads and writes
     * alike, so a claim/release retry loop trips it; only the other's own
     * access resets it, so genuine alternation never does. No wall clock.
     */
    if (s->perf_last_slot != cslot) {
        s->perf_last_slot = cslot;
        s->perf_run = 0;
    }
    if (++s->perf_run < SGI_BEDROCK_PERF_SPIN) {
        return;
    }
    s->perf_run = 0;
    bedrock_hold_start(s, (unsigned int)cslot, SGI_BEDROCK_HOLD_PERF_YIELD);
}

void sgi_bedrock_scratch3_acquire(SGIBedrockState *s, uint64_t observed)
{
    int cslot;

    /* Only a peer already holding the hub lock is worth waiting on. */
    if (observed == 0) {
        return;
    }
    cslot = bedrock_h3_slot(s);
    if (cslot < 0) {
        return;
    }
    bedrock_hold_start(s, (unsigned int)cslot, SGI_BEDROCK_HOLD_SCRATCH3_HELD);
}

void sgi_bedrock_scratch3_released(SGIBedrockState *s)
{
    int cslot;

    /* Wake every peer parked on the hub lock so each re-checks its release. */
    bedrock_hold_wake_kind(s, SGI_BEDROCK_HOLD_SCRATCH3_HELD);

    /*
     * H4's publisher park: a publisher releasing the hub lock after its own
     * interface's publication waits here, with the lock free, for the other
     * interface's. Nothing to wait for once the join is over.
     */
    cslot = bedrock_h3_slot(s);
    if (cslot < 0 || !bedrock_presence_publisher(s, (unsigned int)cslot) ||
        !s->presence_seen[cslot / SGI_BEDROCK_PI_PROCESSORS] ||
        !bedrock_presence_other_pending(s, (unsigned int)cslot)) {
        return;
    }
    bedrock_hold_start(s, (unsigned int)cslot, SGI_BEDROCK_HOLD_PRESENCE);
}

void sgi_bedrock_presence_published(SGIBedrockState *s)
{
    int cslot = bedrock_hold_current_slot(s);
    unsigned int interface;

    if (cslot < 0) {
        /* qtest, DMA, or an access this node did not issue: no bookkeeping. */
        return;
    }
    interface = (unsigned int)cslot / SGI_BEDROCK_PI_PROCESSORS;
    s->presence_seen[interface] = true;
    trace_sgi_bedrock_presence_published(current_cpu->cpu_index, interface,
                                         bedrock_presence_pending(s));
    /* A publisher parked on H4's join re-checks it. */
    bedrock_hold_wake_kind(s, SGI_BEDROCK_HOLD_PRESENCE);
}

/*
 * Release every hold, and forget every first-write and withheld value: a
 * fresh start. Mirrors sgi_hub_rt_hold_reset(); called from this device's
 * own reset, from the local-reset GPIO (bedrock_local_reset, since that
 * resets the LB and II registers this state tracks without cold-resetting
 * this device itself), and from unrealize.
 */
void sgi_bedrock_hold_reset(SGIBedrockState *s)
{
    for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
        CPUState *cs = bedrock_hold_cpu(s, slot);

        if (s->hold_id[slot] && cs) {
            qemu_cpu_kick(cs);
        }
        s->hold_id[slot] = 0;
        s->hold_scratch1_seen[slot] = false;
        s->hold_scratch1_withheld_valid[slot] = false;
    }
    s->perf_last_slot = -1;
    s->perf_run = 0;
    for (unsigned int i = 0; i < SGI_BEDROCK_PROCESSORS /
         SGI_BEDROCK_PI_PROCESSORS; i++) {
        s->presence_seen[i] = false;
    }
    s->presence_abandoned = false;
}

/* True when the window refuses the access, with the reason logged. */
static bool bedrock_remote_refused(SGIBedrockState *s, hwaddr addr,
                                   unsigned size, const char *access)
{
    if (!s->l1_uart_as_initialized) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock: remote register window %s "
                      "at 0x%" HWADDR_PRIx " with no L1 controller linked\n",
                      access, addr);
        return true;
    }
    if (!sgi_bedrock_hub_registers_permitted(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock: remote register window "
                      "%s at 0x%" HWADDR_PRIx " denied for region %u\n",
                      access, addr, (unsigned)s->initiator_region);
        return true;
    }
    if (addr < BEDROCK_L1_UART_OFFSET ||
        addr + size > BEDROCK_L1_UART_OFFSET + SGI_L1_SIZE) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock: remote register window %s "
                      "at 0x%" HWADDR_PRIx " is not decoded\n", access, addr);
        return true;
    }
    return false;
}

/*
 * Marshal one access to or from the attached L1 controller's UART, addr
 * already relative to its own eight registers. Both register-window forms
 * reach the same UART at offset 0x80 (docs/datasheets/l1-controller.md);
 * each form's own ops function gates the call with its own refusal check
 * and owns its own log wording, then shares this one forwarding path.
 */
static MemTxResult bedrock_l1_uart_transfer_read(SGIBedrockState *s,
                                                 hwaddr addr, uint64_t *value,
                                                 unsigned size,
                                                 MemTxAttrs attrs)
{
    uint8_t buffer[8];
    MemTxResult result;

    result = address_space_read(&s->l1_uart_as, addr, attrs, buffer, size);
    if (result != MEMTX_OK) {
        return result;
    }
    switch (size) {
    case 1:
        *value = buffer[0];
        break;
    case 2:
        *value = lduw_be_p(buffer);
        break;
    case 4:
        *value = ldl_be_p(buffer);
        break;
    case 8:
        *value = ldq_be_p(buffer);
        break;
    default:
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult bedrock_l1_uart_transfer_write(SGIBedrockState *s,
                                                   hwaddr addr, uint64_t value,
                                                   unsigned size,
                                                   MemTxAttrs attrs)
{
    uint8_t buffer[8];

    switch (size) {
    case 1:
        buffer[0] = value;
        break;
    case 2:
        stw_be_p(buffer, value);
        break;
    case 4:
        stl_be_p(buffer, value);
        break;
    case 8:
        stq_be_p(buffer, value);
        break;
    default:
        return MEMTX_DECODE_ERROR;
    }
    return address_space_write(&s->l1_uart_as, addr, attrs, buffer, size);
}

static MemTxResult bedrock_remote_read(void *opaque, hwaddr addr,
                                       uint64_t *value, unsigned size,
                                       MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;

    if (bedrock_remote_refused(s, addr, size, "read")) {
        return MEMTX_DECODE_ERROR;
    }
    return bedrock_l1_uart_transfer_read(s, addr - BEDROCK_L1_UART_OFFSET,
                                         value, size, attrs);
}

static MemTxResult bedrock_remote_write(void *opaque, hwaddr addr,
                                        uint64_t value, unsigned size,
                                        MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;

    if (bedrock_remote_refused(s, addr, size, "write")) {
        return MEMTX_DECODE_ERROR;
    }
    return bedrock_l1_uart_transfer_write(s, addr - BEDROCK_L1_UART_OFFSET,
                                          value, size, attrs);
}

static const MemoryRegionOps bedrock_remote_ops = {
    .read_with_attrs = bedrock_remote_read,
    .write_with_attrs = bedrock_remote_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/*
 * True when the LOCAL form's own L1 UART slot refuses the access, with the
 * reason logged. This is bedrock_remote_refused's counterpart for the
 * local register-window form: same three refusals, form-neutral wording
 * (no window name), and the region is already sized to exactly the UART
 * (SGI_L1_SIZE) so its own range check needs no offset arithmetic.
 */
static bool bedrock_l1_uart_refused(SGIBedrockState *s, hwaddr addr,
                                    unsigned size, const char *access)
{
    if (!s->l1_uart_as_initialized) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock: L1 UART %s at 0x%" HWADDR_PRIx
                      " with no L1 controller linked\n", access, addr);
        return true;
    }
    if (!sgi_bedrock_hub_registers_permitted(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock: L1 UART %s at 0x%"
                      HWADDR_PRIx " denied for region %u\n", access, addr,
                      (unsigned)s->initiator_region);
        return true;
    }
    if (addr + size > SGI_L1_SIZE) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock: L1 UART %s at 0x%" HWADDR_PRIx
                      " is not decoded\n", access, addr);
        return true;
    }
    return false;
}

static MemTxResult bedrock_l1_uart_read(void *opaque, hwaddr addr,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;

    if (bedrock_l1_uart_refused(s, addr, size, "read")) {
        return MEMTX_DECODE_ERROR;
    }
    return bedrock_l1_uart_transfer_read(s, addr, value, size, attrs);
}

static MemTxResult bedrock_l1_uart_write(void *opaque, hwaddr addr,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;

    if (bedrock_l1_uart_refused(s, addr, size, "write")) {
        return MEMTX_DECODE_ERROR;
    }
    return bedrock_l1_uart_transfer_write(s, addr, value, size, attrs);
}

static const MemoryRegionOps bedrock_l1_uart_ops = {
    .read_with_attrs = bedrock_l1_uart_read,
    .write_with_attrs = bedrock_l1_uart_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static MemTxResult bedrock_hub_special_read(void *opaque, hwaddr addr,
                                            uint64_t *value, unsigned size,
                                            MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;
    MemoryRegionSection section;
    MemTxResult result;

    if (!sgi_bedrock_hub_registers_permitted(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock: hub register read at "
                      "0x%" HWADDR_PRIx " denied for region %u\n", addr,
                      (unsigned)s->initiator_region);
        return MEMTX_DECODE_ERROR;
    }
    section = memory_region_find(&s->mmio, addr, size);
    if (!section.mr || section.mr == &s->mmio ||
        int128_lt(section.size, int128_make64(size))) {
        if (section.mr) {
            memory_region_unref(section.mr);
        }
        return MEMTX_DECODE_ERROR;
    }
    result = memory_region_dispatch_read(section.mr,
                                         section.offset_within_region, value,
                                         size_memop(size) | MO_BE, attrs);
    memory_region_unref(section.mr);
    return result;
}

static MemTxResult bedrock_hub_special_write(void *opaque, hwaddr addr,
                                             uint64_t value, unsigned size,
                                             MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;
    MemoryRegionSection section;
    MemTxResult result;

    if (!sgi_bedrock_hub_registers_permitted(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock: hub register write at "
                      "0x%" HWADDR_PRIx " denied for region %u\n", addr,
                      (unsigned)s->initiator_region);
        return MEMTX_DECODE_ERROR;
    }
    section = memory_region_find(&s->mmio, addr, size);
    if (!section.mr || section.mr == &s->mmio ||
        int128_lt(section.size, int128_make64(size))) {
        if (section.mr) {
            memory_region_unref(section.mr);
        }
        return MEMTX_DECODE_ERROR;
    }
    result = memory_region_dispatch_write(section.mr,
                                          section.offset_within_region, value,
                                          size_memop(size) | MO_BE, attrs);
    memory_region_unref(section.mr);
    return result;
}

static const MemoryRegionOps bedrock_hub_special_ops = {
    .read_with_attrs = bedrock_hub_special_read,
    .write_with_attrs = bedrock_hub_special_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static MemTxResult bedrock_io_registers_read(void *opaque, hwaddr addr,
                                             uint64_t *value, unsigned size,
                                             MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP, "sgi-bedrock: crosstalk read of hub register "
                  "0x%" HWADDR_PRIx " is not supported\n", addr);
    *value = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult bedrock_io_registers_write(void *opaque, hwaddr addr,
                                              uint64_t value, unsigned size,
                                              MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;

    switch (addr) {
    case BEDROCK_PI_MODIFY_OFFSET:
        sgi_bedrock_pi_io_modify(&s->pi[0], value, s->local_region);
        return MEMTX_OK;
    case BEDROCK_PI1_OFFSET + BEDROCK_PI_MODIFY_OFFSET:
        sgi_bedrock_pi_io_modify(&s->pi[1], value, s->local_region);
        return MEMTX_OK;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock: crosstalk write of 0x%"
                      PRIx64 " to hub register 0x%" HWADDR_PRIx
                      " is not supported\n", value, addr);
        return MEMTX_DECODE_ERROR;
    }
}

static const MemoryRegionOps bedrock_io_registers_ops = {
    .read_with_attrs = bedrock_io_registers_read,
    .write_with_attrs = bedrock_io_registers_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void bedrock_forward_irq(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    qemu_set_irq(s->hardware_irq[n], level);
}

static void bedrock_forward_nmi(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    qemu_set_irq(s->nmi[n], level);
}

static void bedrock_forward_cpu_enable(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    qemu_set_irq(s->cpu_enable[n], level);
}

static void bedrock_lb_error(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    sgi_bedrock_pi_set_lb_error(&s->pi[0], level);
    sgi_bedrock_pi_set_lb_error(&s->pi[1], level);
}

static void bedrock_ni_error(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    sgi_bedrock_pi_set_ni_error(&s->pi[0], level);
    sgi_bedrock_pi_set_ni_error(&s->pi[1], level);
}

static void bedrock_xb_error(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    sgi_bedrock_pi_set_xb_error(&s->pi[0], level);
    sgi_bedrock_pi_set_xb_error(&s->pi[1], level);
}

/*
 * The L1 controller's UART is one link per node, not one per processor
 * interface (docs/datasheets/l1-controller.md), so it is broadcast to both
 * PI0 and PI1's own pending bit 4 the same way lb-error, ni-error and
 * xb-error already broadcast their single node-wide line to both.
 */
static void bedrock_uart_irq(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    sgi_bedrock_pi_set_uart_irq(&s->pi[0], level);
    sgi_bedrock_pi_set_uart_irq(&s->pi[1], level);
}

static void bedrock_local_reset(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    if (level) {
        trace_sgi_bedrock_local_reset(current_cpu ? current_cpu->cpu_index
                                      : -1);
        device_cold_reset(DEVICE(&s->pi[0]));
        device_cold_reset(DEVICE(&s->pi[1]));
        device_cold_reset(DEVICE(&s->io));
        sgi_bedrock_md_soft_reset(&s->md);
        device_cold_reset(DEVICE(&s->lb));
        device_cold_reset(DEVICE(&s->ni));
        device_cold_reset(DEVICE(&s->xb));
        /*
         * The local-reset GPIO resets the LB and II scratch registers this
         * device's hold state tracks (each above), without cold-resetting
         * this device itself, so the hold state needs its own clear here
         * too, alongside bedrock_reset_enter's for the ordinary reset path.
         */
        sgi_bedrock_hold_reset(s);
    }
}

/*
 * Work out which vCPU sits in each of the four slots, from the board's own
 * processor-slot-order. Only the board knows: a mask names the fitted
 * slots, and once all four are fitted it no longer says which processor
 * the machine put in each of them.
 */
static bool bedrock_resolve_slot_processors(SGIBedrockState *s, Error **errp)
{
    unsigned int assigned = 0;

    for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
        s->slot_cpu_index[slot] = -1;
    }
    if (s->processor_slot_order == SGI_BEDROCK_SLOT_ORDER_BY_SLOT) {
        unsigned int logical = s->cpu_base;

        for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
            if (s->processor_mask & (1U << slot)) {
                s->slot_cpu_index[slot] = (int)logical++;
            }
        }
        return true;
    }
    for (unsigned int processor = 0; processor < s->processor_count;
         processor++) {
        unsigned int slot = (s->processor_slot_order >> (2 * processor)) & 3;

        if (!(s->processor_mask & (1U << slot))) {
            error_setg(errp, "Bedrock processor %u is placed in empty slot %u",
                       processor, slot);
            return false;
        }
        if (s->slot_cpu_index[slot] >= 0) {
            error_setg(errp, "Bedrock slot %u holds more than one processor",
                       slot);
            return false;
        }
        s->slot_cpu_index[slot] = (int)(s->cpu_base + processor);
        assigned++;
    }
    if (assigned != s->processor_count) {
        error_setg(errp, "Bedrock processor-slot-order covers %u of %u "
                   "processors", assigned, s->processor_count);
        return false;
    }
    return true;
}

static void bedrock_realize(DeviceState *dev, Error **errp)
{
    SGIBedrockState *s = SGI_BEDROCK(dev);
    unsigned mask;

    if (s->processor_count > 4) {
        error_setg(errp, "Bedrock processor count exceeds four slots");
        return;
    }
    mask = s->processor_mask ? s->processor_mask :
        (1U << s->processor_count) - 1;
    if ((mask & ~0xfU) || ctpop32(mask) != s->processor_count) {
        error_setg(errp, "Bedrock processor mask must select exactly %u "
                   "of four slots", s->processor_count);
        return;
    }
    if (s->cpu_enable_initial_mask & ~mask) {
        error_setg(errp, "Bedrock initial CPU enable mask selects an "
                   "empty slot");
        return;
    }
    s->processor_mask = mask;
    if (!bedrock_resolve_slot_processors(s, errp)) {
        return;
    }
    if (!s->dma_mr) {
        error_setg(errp, TYPE_SGI_BEDROCK " 'dma-memory' link not set");
        return;
    }
    if (s->l1_uart_mr && s->initiator_region >= 64) {
        error_setg(errp, "initiator-region must be set below 64 when "
                   "l1-uart is linked");
        return;
    }
    if (s->revision >= 16) {
        error_setg(errp, "revision must fit four bits");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr, "sgi-bedrock-dma");
    s->dma_as_initialized = true;
    if (s->l1_uart_mr) {
        address_space_init(&s->l1_uart_as, s->l1_uart_mr,
                           "sgi-bedrock-l1-uart");
        s->l1_uart_as_initialized = true;
    }
    qdev_prop_set_uint8(DEVICE(&s->io), "revision", s->revision);
    qdev_prop_set_uint8(DEVICE(&s->io), "local-region", s->local_region);
    qdev_prop_set_uint16(DEVICE(&s->io), "inbound-widget-mask",
                         s->inbound_widget_mask);
    qdev_prop_set_uint8(DEVICE(&s->io), "inbound-device", s->inbound_device);
    qdev_prop_set_uint64(DEVICE(&s->io), "device-mask-reset",
                         s->device_mask_reset);
    sgi_bedrock_io_set_dma_as(&s->io, &s->dma_as);
    for (unsigned i = 0; i < ARRAY_SIZE(s->widget_mr); i++) {
        if (s->widget_mr[i]) {
            g_autofree char *name = g_strdup_printf("widget-%u", i);

            if (!object_property_set_link(OBJECT(&s->io), name,
                                          OBJECT(s->widget_mr[i]), errp)) {
                return;
            }
        }
    }
    if (!object_property_set_link(OBJECT(&s->io), "hub-registers",
                                  OBJECT(&s->io_registers), errp)) {
        return;
    }
    if (s->node_mr) {
        if (!object_property_set_link(OBJECT(&s->md), "node-memory",
                                      OBJECT(s->node_mr), errp)) {
            return;
        }
    }
    qdev_prop_set_uint8(DEVICE(&s->md), "installed-bank-mask",
                        s->installed_memory_bank_mask);
    for (unsigned bank = 0; bank < 8; bank++) {
        g_autofree char *property =
            g_strdup_printf("installed-bank%u-mib", bank);

        qdev_prop_set_uint16(DEVICE(&s->md), property,
                            s->installed_memory_bank_mib[bank]);
    }
    for (unsigned i = 0; i < 2; i++) {
        qdev_prop_set_uint32(DEVICE(&s->pi[i]), "interface", i);
        qdev_prop_set_int32(DEVICE(&s->pi[i]), "slot-a-cpu-index",
                           s->slot_cpu_index[i * SGI_BEDROCK_PI_PROCESSORS]);
        qdev_prop_set_int32(DEVICE(&s->pi[i]), "slot-b-cpu-index",
                           s->slot_cpu_index[i * SGI_BEDROCK_PI_PROCESSORS +
                                             1]);
        qdev_prop_set_uint32(DEVICE(&s->pi[i]), "processor-count",
                            s->processor_count);
        qdev_prop_set_uint32(DEVICE(&s->pi[i]), "processor-mask",
                            s->processor_mask);
        qdev_prop_set_uint8(DEVICE(&s->pi[i]), "cpu-enable-initial",
                           (s->cpu_enable_initial_mask >> (2 * i)) & 3);
        if (!qdev_realize(DEVICE(&s->pi[i]), sysbus_get_default(), errp)) {
            return;
        }
        memory_region_add_subregion(&s->mmio, i * 0x200000,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->pi[i]), 0));
        for (unsigned line = 0;
             line < SGI_BEDROCK_PI_PROCESSORS * SGI_BEDROCK_PI_HW_IRQS;
             line++) {
            qdev_connect_gpio_out_named(DEVICE(&s->pi[i]), "hardware-irq",
                                        line,
                qdev_get_gpio_in_named(dev, "pi-hardware-irq",
                    i * SGI_BEDROCK_PI_PROCESSORS *
                    SGI_BEDROCK_PI_HW_IRQS + line));
        }
        for (unsigned cpu = 0; cpu < SGI_BEDROCK_PI_PROCESSORS; cpu++) {
            qdev_connect_gpio_out_named(DEVICE(&s->pi[i]), "nmi", cpu,
                qdev_get_gpio_in_named(dev, "pi-nmi",
                    i * SGI_BEDROCK_PI_PROCESSORS + cpu));
            qdev_connect_gpio_out_named(DEVICE(&s->pi[i]), "cpu-enable", cpu,
                qdev_get_gpio_in_named(dev, "pi-cpu-enable",
                    i * SGI_BEDROCK_PI_PROCESSORS + cpu));
        }
    }
    s->lb.bedrock = s;
    s->md.bedrock = s;
    s->io.bedrock = s;
    qdev_prop_set_uint32(DEVICE(&s->lb), "revision", s->revision);
    /*
     * The local block's vector engine reads a hub register for a routed read;
     * the node's own register window is where a local route resolves.
     */
    if (!object_property_set_link(OBJECT(&s->lb), "hub-registers",
                                  OBJECT(&s->hub_window), errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->io), sysbus_get_default(), errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->lb), sysbus_get_default(), errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->ni), sysbus_get_default(), errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->md), sysbus_get_default(), errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->xb), sysbus_get_default(), errp)) {
        return;
    }
    memory_region_add_subregion(&s->mmio, 0x400000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->io), 0));
    memory_region_add_subregion(&s->mmio, 0x600000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->lb), 0));
    memory_region_add_subregion(&s->mmio, 0x680000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->ni), 0));
    memory_region_add_subregion(&s->mmio, 0x700000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->xb), 0));
    memory_region_add_subregion(&s->mmio, 0x780000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->md), 0));
    qdev_connect_gpio_out_named(DEVICE(&s->lb), "error", 0,
        qdev_get_gpio_in_named(dev, "lb-error", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->xb), "error", 0,
        qdev_get_gpio_in_named(dev, "xb-error", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->lb), "soft-reset", 0,
        qdev_get_gpio_in_named(dev, "local-reset", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->ni), "error", 0,
        qdev_get_gpio_in_named(dev, "ni-error", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->ni), "local-reset", 0,
        qdev_get_gpio_in_named(dev, "local-reset", 0));
}

/*
 * Release any hold a migration snapshot caught mid-wait: a vCPU parked in
 * bedrock_hold_cpu()'s wait condition has no way to be woken by a stream
 * that carries no callback state for it, so this mirrors sgi_hub_post_load()
 * kicking the same way through sgi_hub_rt_hold_reset().
 */
static int bedrock_post_load(void *opaque, int version_id)
{
    sgi_bedrock_hold_reset(opaque);
    return 0;
}

/*
 * SGIBedrockState carries no migrated fields of its own. Its registers live
 * entirely in the pi/io/md/lb/ni/xb sub-devices, each realized with its own
 * VMStateDescription (device_set_realized() registers every realized
 * sub-device by its own dc->vmsd, independent of this one). Everything else
 * directly in this struct is either wiring and links reproduced identically
 * from the same machine construction on both ends (MemoryRegion/AddressSpace
 * plumbing, qemu_irq fan-out, the dma/node/l1-uart/widget MemoryRegion
 * links), a qdev property normalized once in bedrock_realize() and never
 * mutated afterward (processor_count, processor_mask,
 * cpu_enable_initial_mask, cpu_base, revision, local_region,
 * installed_memory_bank_mask, installed_memory_bank_mib, inbound_widget_mask,
 * inbound_device, device_mask_reset, initiator_region), or the host-only
 * rendezvous-hold scheduling state documented on hold_cpu and
 * hold_scratch1_seen in sgi-bedrock.h: bounded, re-accumulated from the
 * guest's own polling, and never itself a value the guest reads. That state
 * -- hold_cpu, hold_scratch1_seen, hold_scratch1_withheld_valid,
 * hold_scratch1_withheld, perf_last_slot, perf_run, hold_id, hold_kind,
 * hold_seq, presence_seen, presence_abandoned -- stays out of this
 * description on purpose, the way
 * sgi-hub.c's vmstate_sgi_hub omits its own pi_rt_hold family; post_load
 * puts it through the same reset path a cold reset does.
 *
 * This description still exists, rather than leaving dc->vmsd unset, so this
 * device is accounted for in the migration stream and a future field that
 * does need migrating has a place to land already reviewed for the fields
 * around it.
 */
static const VMStateDescription vmstate_sgi_bedrock = {
    .name = TYPE_SGI_BEDROCK,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = bedrock_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static const Property bedrock_properties[] = {
    DEFINE_PROP_UINT32("processor-count", SGIBedrockState,
                       processor_count, 1),
    DEFINE_PROP_UINT32("processor-mask", SGIBedrockState,
                       processor_mask, 0),
    DEFINE_PROP_UINT32("cpu-enable-initial-mask", SGIBedrockState,
                       cpu_enable_initial_mask, 0),
    /*
     * Index of this node's first processor, so each interface can name its
     * own pair and tell the reading processor A from B.
     */
    DEFINE_PROP_UINT32("cpu-base", SGIBedrockState, cpu_base, 0),
    DEFINE_PROP_UINT32("processor-slot-order", SGIBedrockState,
                       processor_slot_order, SGI_BEDROCK_SLOT_ORDER_BY_SLOT),
    DEFINE_PROP_UINT8("revision", SGIBedrockState, revision, 2),
    DEFINE_PROP_UINT8("local-region", SGIBedrockState, local_region, 0),
    DEFINE_PROP_UINT8("installed-bank-mask", SGIBedrockState,
                      installed_memory_bank_mask, UINT8_MAX),
    DEFINE_PROP_UINT16("installed-bank0-mib", SGIBedrockState,
                       installed_memory_bank_mib[0], 0),
    DEFINE_PROP_UINT16("installed-bank1-mib", SGIBedrockState,
                       installed_memory_bank_mib[1], 0),
    DEFINE_PROP_UINT16("installed-bank2-mib", SGIBedrockState,
                       installed_memory_bank_mib[2], 0),
    DEFINE_PROP_UINT16("installed-bank3-mib", SGIBedrockState,
                       installed_memory_bank_mib[3], 0),
    DEFINE_PROP_UINT16("installed-bank4-mib", SGIBedrockState,
                       installed_memory_bank_mib[4], 0),
    DEFINE_PROP_UINT16("installed-bank5-mib", SGIBedrockState,
                       installed_memory_bank_mib[5], 0),
    DEFINE_PROP_UINT16("installed-bank6-mib", SGIBedrockState,
                       installed_memory_bank_mib[6], 0),
    DEFINE_PROP_UINT16("installed-bank7-mib", SGIBedrockState,
                       installed_memory_bank_mib[7], 0),
    DEFINE_PROP_UINT16("inbound-widget-mask", SGIBedrockState,
                       inbound_widget_mask, (1U << 0xe) | (1U << 0xf)),
    DEFINE_PROP_UINT8("inbound-device", SGIBedrockState, inbound_device, 0),
    DEFINE_PROP_UINT64("device-mask-reset", SGIBedrockState,
                       device_mask_reset, UINT64_MAX),
    DEFINE_PROP_LINK("dma-memory", SGIBedrockState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("node-memory", SGIBedrockState, node_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("l1-uart", SGIBedrockState, l1_uart_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT8("initiator-region", SGIBedrockState,
                      initiator_region, UINT8_MAX),
#define BEDROCK_WIDGET_PROP(_n) \
    DEFINE_PROP_LINK("widget-" #_n, SGIBedrockState, widget_mr[_n], \
                     TYPE_MEMORY_REGION, MemoryRegion *)
    BEDROCK_WIDGET_PROP(0), BEDROCK_WIDGET_PROP(1),
    BEDROCK_WIDGET_PROP(2), BEDROCK_WIDGET_PROP(3),
    BEDROCK_WIDGET_PROP(4), BEDROCK_WIDGET_PROP(5),
    BEDROCK_WIDGET_PROP(6), BEDROCK_WIDGET_PROP(7),
    BEDROCK_WIDGET_PROP(8), BEDROCK_WIDGET_PROP(9),
    BEDROCK_WIDGET_PROP(10), BEDROCK_WIDGET_PROP(11),
    BEDROCK_WIDGET_PROP(12), BEDROCK_WIDGET_PROP(13),
    BEDROCK_WIDGET_PROP(14), BEDROCK_WIDGET_PROP(15),
#undef BEDROCK_WIDGET_PROP
};

static void bedrock_init(Object *obj)
{
    SGIBedrockState *s = SGI_BEDROCK(obj);
    DeviceState *dev = DEVICE(obj);

    object_initialize_child(obj, "pi0", &s->pi[0], TYPE_SGI_BEDROCK_PI);
    object_initialize_child(obj, "pi1", &s->pi[1], TYPE_SGI_BEDROCK_PI);
    object_initialize_child(obj, "io", &s->io, TYPE_SGI_BEDROCK_IO);
    object_initialize_child(obj, "md", &s->md, TYPE_SGI_BEDROCK_MD);
    object_initialize_child(obj, "lb", &s->lb, TYPE_SGI_BEDROCK_LB);
    object_initialize_child(obj, "ni", &s->ni, TYPE_SGI_BEDROCK_NI);
    object_initialize_child(obj, "xb", &s->xb, TYPE_SGI_BEDROCK_XB);
    memory_region_init(&s->mmio, obj, TYPE_SGI_BEDROCK, SGI_BEDROCK_SIZE);
    /*
     * The vector engine reads a hub register by its window offset. This alias
     * is never mapped, so it presents the window at offset zero however the
     * machine places mmio itself.
     */
    memory_region_init_alias(&s->hub_window, obj,
                             TYPE_SGI_BEDROCK "-hub-window", &s->mmio, 0,
                             SGI_BEDROCK_SIZE);
    memory_region_init_io(&s->hub_special_registers, obj,
                          &bedrock_hub_special_ops, s,
                          TYPE_SGI_BEDROCK "-hub-special-registers",
                          SGI_BEDROCK_SIZE);
    /* The checked view forwards into this device's own register tree. */
    s->hub_special_registers.disable_reentrancy_guard = true;
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->io), 1));
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->io), 2));
    memory_region_init_io(&s->remote_registers, obj, &bedrock_remote_ops, s,
                          TYPE_SGI_BEDROCK "-remote-registers",
                          BEDROCK_REMOTE_REGISTERS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->remote_registers);
    memory_region_init_io(&s->io_registers, obj, &bedrock_io_registers_ops,
                          s, TYPE_SGI_BEDROCK "-io-registers",
                          BEDROCK_IO_REGISTERS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->io_registers);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->md), 1));
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->md), 2));
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->md), 3));
    memory_region_init_io(&s->l1_uart_registers, obj, &bedrock_l1_uart_ops, s,
                          TYPE_SGI_BEDROCK "-l1-uart-registers",
                          SGI_L1_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->l1_uart_registers);
    /* Region 9: the memory-special fetch-and-op window over node memory. */
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->md), 4));
    qdev_init_gpio_out_named(dev, s->hardware_irq, "hardware-irq",
                             ARRAY_SIZE(s->hardware_irq));
    qdev_init_gpio_out_named(dev, s->nmi, "nmi", ARRAY_SIZE(s->nmi));
    qdev_init_gpio_out_named(dev, s->cpu_enable, "cpu-enable",
                             ARRAY_SIZE(s->cpu_enable));
    qdev_init_gpio_in_named(dev, bedrock_forward_irq, "pi-hardware-irq",
                            ARRAY_SIZE(s->hardware_irq));
    qdev_init_gpio_in_named(dev, bedrock_forward_nmi, "pi-nmi",
                            ARRAY_SIZE(s->nmi));
    qdev_init_gpio_in_named(dev, bedrock_forward_cpu_enable, "pi-cpu-enable",
                            ARRAY_SIZE(s->cpu_enable));
    qdev_init_gpio_in_named(dev, bedrock_lb_error, "lb-error", 1);
    qdev_init_gpio_in_named(dev, bedrock_ni_error, "ni-error", 1);
    qdev_init_gpio_in_named(dev, bedrock_xb_error, "xb-error", 1);
    qdev_init_gpio_in_named(dev, bedrock_uart_irq, "uart-irq", 1);
    qdev_init_gpio_in_named(dev, bedrock_local_reset, "local-reset", 1);
}

static void bedrock_reset_enter(Object *obj, ResetType type)
{
    sgi_bedrock_hold_reset(SGI_BEDROCK(obj));
}

static void bedrock_unrealize(DeviceState *dev)
{
    sgi_bedrock_hold_reset(SGI_BEDROCK(dev));
}

static void bedrock_finalize(Object *obj)
{
    SGIBedrockState *s = SGI_BEDROCK(obj);

    if (s->dma_as_initialized) {
        address_space_destroy(&s->dma_as);
    }
    if (s->l1_uart_as_initialized) {
        address_space_destroy(&s->l1_uart_as);
    }
}

static void bedrock_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI Bedrock node controller";
    dc->realize = bedrock_realize;
    dc->unrealize = bedrock_unrealize;
    dc->vmsd = &vmstate_sgi_bedrock;
    dc->user_creatable = true;
    device_class_set_props(dc, bedrock_properties);
    rc->phases.enter = bedrock_reset_enter;
}

static const TypeInfo bedrock_info = {
    .name = TYPE_SGI_BEDROCK,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockState),
    .instance_init = bedrock_init,
    .instance_finalize = bedrock_finalize,
    .class_init = bedrock_class_init,
};

static void bedrock_register_types(void)
{
    type_register_static(&bedrock_info);
}

type_init(bedrock_register_types)
