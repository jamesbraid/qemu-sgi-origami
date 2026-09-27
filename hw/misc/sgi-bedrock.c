/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/visitor.h"
#include "qemu/cutils.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/misc/sgi-l1.h"
#include "exec/cpu-interrupt.h"
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
 * GPIO output is not wired to anything in hw/mips/sgi/sn1-node.c. The
 * node's cpu-enable-initial-mask grants every fitted slot at reset, a
 * model approximation of a reset value no source gives, and the PROM
 * clears a partner's bit when that partner fails its early liveness
 * check. Gating bedrock_hold_slot_running() on cpu-enable would tie the
 * join to that approximation and to the PROM's own verdict, where the
 * hold only needs to know which processors are running. "Present, running" here
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
 * The interfaces' other processors (B and D) read their own interface's
 * bits first, so they wait on their own publisher instead: the partner
 * park (SGI_BEDROCK_HOLD_PARTNER). Between the reset-edge join and the
 * publication the PROM runs a pair liveness handshake: each processor of
 * the pair writes a status value into its own real-time compare register,
 * which nothing arms at that point, and polls its partner's for the same
 * value, for a bounded number of reads. The publisher then takes the hub
 * lock and publishes; the partner instead waits out a fixed time on the
 * real-time counter, about 3 ms, before its first presence read. That timed
 * wait is the whole of the ordering: on hardware the publisher needs a few
 * dozen uncached accesses to publish, far less than 3 ms. Under
 * multi-threaded TCG the counter follows the host clock whether or not the
 * publisher's thread is running, so the wait can run out first. So once
 * both processors of the pair have posted since the join, the partner is
 * parked at its next real-time counter read until its publisher has
 * released the hub lock after publishing. Parking it any earlier cannot
 * work: the publisher would still be polling for the partner's post, and
 * its bounded poll disables the partner when it runs out ("peer_pled
 * failed: Other CPU on this node died"). Two earlier placements at the
 * partner's second LB scratch 1 write lost it that way in every boot
 * measured (docs/known-issues.md).
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
 * register. A processor this device has halted for a park still counts as
 * running: the halt is the model's, not the guest's.
 */
static bool bedrock_hold_slot_running(SGIBedrockState *s,
                                      unsigned int physical)
{
    CPUState *cs;

    if (!(s->processor_mask & (1U << physical))) {
        return false;
    }
    cs = bedrock_hold_cpu(s, physical);
    return cs && (!cs->halted || (s->park_mask & (1U << physical))) &&
           cpu_can_run(cs);
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
    case SGI_BEDROCK_HOLD_PARTNER:
        /* A partner, parked until its own interface's publisher is done. */
        return s->presence_abandoned ||
               s->presence_released[slot / SGI_BEDROCK_PI_PROCESSORS];
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

/*
 * An access to scratch 1 from outside this node's processors: the peer's
 * vector engine, which is how the global master tells a node its NASID
 * (the local-block datasheet, "Node number assignment"), or a qtest. The
 * hold only delays when a local post becomes visible; on hardware that post
 * reached the register before anything that arrives now. So every withheld
 * value lands first, in the order its processor wrote it, and the external
 * access is ordered after all of them: a write is not overwritten by a
 * local value released later, and a read sees the local posts. The held
 * processors stay parked until their own join completes; only the values
 * move.
 */
static void bedrock_scratch1_external(SGIBedrockState *s)
{
    for (;;) {
        int oldest = -1;

        for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
            if (s->hold_scratch1_withheld_valid[slot] &&
                (oldest < 0 || s->hold_scratch1_withheld_seq[slot] <
                               s->hold_scratch1_withheld_seq[oldest])) {
                oldest = slot;
            }
        }
        if (oldest < 0) {
            return;
        }
        trace_sgi_bedrock_hold_external_flush(oldest,
            s->hold_scratch1_withheld[oldest]);
        bedrock_hold_flush_scratch1(s, oldest);
    }
}

void sgi_bedrock_scratch1_read(SGIBedrockState *s)
{
    if (bedrock_hold_current_slot(s) < 0) {
        bedrock_scratch1_external(s);
    }
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
    case SGI_BEDROCK_HOLD_PARTNER:
        return "partner";
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
         * qtest, the peer's vector engine, or another access this node did
         * not issue: ordinary storage after every withheld local value.
         */
        bedrock_scratch1_external(s);
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
    s->hold_scratch1_joined[slot] = true;

    if (!bedrock_hold_applicable(s) || bedrock_hold_scratch1_ready(s)) {
        s->lb.scratch[1] = value;
        return;
    }

    s->hold_scratch1_withheld[slot] = value;
    s->hold_scratch1_withheld_valid[slot] = true;
    s->hold_scratch1_withheld_seq[slot] = ++s->hold_scratch1_seq;
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

/*
 * H4's two parks (the publisher's and the partner's) halt the parked
 * processor instead of blocking its host thread. A blocked thread is fine
 * under multi-threaded TCG, but under round-robin or icount the one TCG
 * thread runs every processor, so a blocking park would stall the very
 * processor it waits for; that is why the join used to apply to
 * multi-threaded TCG alone and an icount boot lost processor C for good.
 * A halted processor costs the others nothing under any scheduler.
 *
 * The halt is the model's, not the guest's: the processor stops after the
 * access that parked it and resumes at the next instruction, with nothing
 * guest-visible changed. The limitation: a guest WAIT on a processor an
 * interrupt woke early could be ended early by the release; the PROM runs
 * these barriers with interrupts disabled.
 *
 * The bound is on the virtual clock, so an icount run stays deterministic.
 * Under multi-threaded TCG the virtual clock follows host time, as the old
 * realtime bound did.
 *
 * Only an access this node's own processor issued can park, and not a
 * processor already in a hold or a park. Returns the executing slot, or -1.
 */
static int bedrock_park_slot(SGIBedrockState *s)
{
    int cslot = bedrock_hold_current_slot(s);

    if (cslot < 0 || !bedrock_hold_applicable(s)) {
        return -1;
    }
    return (s->hold_id[cslot] || s->park_kind[cslot]) ? -1 : cslot;
}

/* Arm the one park timer at the earliest bound still pending. */
static void bedrock_park_arm(SGIBedrockState *s)
{
    int64_t earliest = INT64_MAX;

    for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
        if (s->park_kind[slot]) {
            earliest = MIN(earliest, s->park_deadline[slot]);
        }
    }
    if (earliest == INT64_MAX) {
        timer_del(s->park_timer);
    } else {
        timer_mod(s->park_timer, earliest);
    }
}

/*
 * Runs on the parked processor's own thread, the way sgi-hub.c's
 * sgi_hub_cpu_unhalt does. A release queued before the halt took effect
 * clears the pending halt request instead, so the processor never stops.
 */
static void bedrock_park_unhalt(CPUState *cs, run_on_cpu_data data)
{
    SGIBedrockState *s = data.host_ptr;

    for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
        if (s->slot_cpu_index[slot] == cs->cpu_index) {
            if (!(s->park_mask & (1U << slot)) || s->park_kind[slot]) {
                /* Released twice, or parked again since: not ours to end. */
                return;
            }
            s->park_mask &= ~(1U << slot);
            break;
        }
    }
    cpu_reset_interrupt(cs, CPU_INTERRUPT_HALT);
    cs->halted = 0;
}

static void bedrock_park_start(SGIBedrockState *s, unsigned int slot,
                               SGIBedrockHoldKind kind)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->park_kind[slot] = kind;
    s->park_mask |= 1U << slot;
    s->park_start[slot] = now;
    s->park_deadline[slot] = now + SGI_BEDROCK_HOLD_MS * SCALE_MS;
    trace_sgi_bedrock_hold_begin(current_cpu->cpu_index,
                                 bedrock_hold_kind_name(kind), slot, now, 0);
    cpu_interrupt(current_cpu, CPU_INTERRUPT_HALT);
    bedrock_park_arm(s);
}

static void bedrock_park_release(SGIBedrockState *s, unsigned int slot,
                                 const char *outcome)
{
    CPUState *cs = bedrock_hold_cpu(s, slot);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->park_kind[slot] = 0;
    /* A withheld scratch-1 write lands before this processor runs again. */
    bedrock_hold_flush_scratch1(s, slot);
    if (!cs) {
        s->park_mask &= ~(1U << slot);
        return;
    }
    trace_sgi_bedrock_hold_end(cs->cpu_index, outcome,
                               (now - s->park_start[slot]) / SCALE_US, 0);
    async_run_on_cpu(cs, bedrock_park_unhalt, RUN_ON_CPU_HOST_PTR(s));
}

/* Release every park whose condition now holds. */
static void bedrock_park_recheck(SGIBedrockState *s)
{
    for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
        if (s->park_kind[slot] &&
            bedrock_hold_ready(s, slot, s->park_kind[slot])) {
            bedrock_park_release(s, slot, "satisfied");
        }
    }
    bedrock_park_arm(s);
}

/*
 * A park that reaches its bound ends the join for this reset: the
 * publisher it waited for is not coming, and parking every later release
 * for the bound again would only stretch the boot the PROM is about to
 * report on. Every other park is released with it.
 */
static void bedrock_park_expire(void *opaque)
{
    SGIBedrockState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool expired = false;

    for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
        if (s->park_kind[slot] && now >= s->park_deadline[slot]) {
            bedrock_park_release(s, slot, "timed out");
            expired = true;
        }
    }
    if (expired) {
        s->presence_abandoned = true;
    }
    bedrock_park_recheck(s);
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
     * A publisher releasing the lock after its interface's publication has
     * finished publishing: its partner, if parked, may now read the bits.
     */
    cslot = bedrock_hold_current_slot(s);
    if (cslot >= 0 && bedrock_presence_publisher(s, (unsigned int)cslot)) {
        unsigned int interface = (unsigned int)cslot /
                                 SGI_BEDROCK_PI_PROCESSORS;

        if (s->presence_seen[interface] &&
            !s->presence_released[interface]) {
            s->presence_released[interface] = true;
            bedrock_park_recheck(s);
        }
    }

    /*
     * H4's publisher park: a publisher releasing the hub lock after its own
     * interface's publication waits here, with the lock free, for the other
     * interface's. Nothing to wait for once the join is over.
     */
    cslot = bedrock_park_slot(s);
    if (cslot < 0 || !bedrock_presence_publisher(s, (unsigned int)cslot) ||
        !s->presence_seen[cslot / SGI_BEDROCK_PI_PROCESSORS] ||
        !bedrock_presence_other_pending(s, (unsigned int)cslot)) {
        return;
    }
    bedrock_park_start(s, (unsigned int)cslot, SGI_BEDROCK_HOLD_PRESENCE);
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
    bedrock_park_recheck(s);
}

void sgi_bedrock_rt_compare_written(SGIBedrockState *s,
                                    unsigned int interface,
                                    unsigned int slot)
{
    int cslot = bedrock_hold_current_slot(s);

    /*
     * Only a processor's own register counts as its post, and only its
     * first write after the reset-edge join: the PROM's earlier writes
     * there are progress codes, not the handshake.
     */
    if (cslot < 0 ||
        (unsigned int)cslot != interface * SGI_BEDROCK_PI_PROCESSORS + slot ||
        !s->hold_scratch1_joined[cslot]) {
        return;
    }
    s->presence_posted[cslot] = true;
}

void sgi_bedrock_rt_counter_read(SGIBedrockState *s)
{
    int cslot = bedrock_park_slot(s);
    unsigned int interface;
    int publisher;

    if (cslot < 0) {
        return;
    }
    interface = (unsigned int)cslot / SGI_BEDROCK_PI_PROCESSORS;
    publisher = bedrock_publisher_slot(s, interface);
    /*
     * Only a partner, only once both of the pair have posted -- before
     * that its publisher is still polling for its post -- and only while
     * its publisher has yet to publish.
     */
    if (publisher < 0 || publisher == cslot || s->presence_abandoned ||
        s->presence_released[interface] || !s->presence_posted[cslot] ||
        !s->presence_posted[publisher]) {
        return;
    }
    bedrock_park_start(s, (unsigned int)cslot, SGI_BEDROCK_HOLD_PARTNER);
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
        s->hold_scratch1_joined[slot] = false;
        s->presence_posted[slot] = false;
        /*
         * A parked processor is halted by this device alone; wake it, or a
         * local reset (which does not restart the processors) would leave it
         * stopped for good. The unhalt work checks park_mask when it runs.
         */
        s->park_kind[slot] = 0;
        if (s->park_mask & (1U << slot)) {
            if (cs) {
                async_run_on_cpu(cs, bedrock_park_unhalt,
                                 RUN_ON_CPU_HOST_PTR(s));
            } else {
                s->park_mask &= ~(1U << slot);
            }
        }
    }
    if (s->park_timer) {
        timer_del(s->park_timer);
    }
    s->perf_last_slot = -1;
    s->perf_run = 0;
    for (unsigned int i = 0; i < SGI_BEDROCK_PROCESSORS /
         SGI_BEDROCK_PI_PROCESSORS; i++) {
        s->presence_seen[i] = false;
        s->presence_released[i] = false;
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

void sgi_bedrock_memory_correctable_error(SGIBedrockState *s)
{
    sgi_bedrock_pi_set_pending(&s->pi[0], 1, 61);
    sgi_bedrock_pi_set_pending(&s->pi[1], 1, 61);
}

void sgi_bedrock_memory_uncorrectable_error(SGIBedrockState *s)
{
    sgi_bedrock_pi_set_memory_uncorrectable(&s->pi[0]);
    sgi_bedrock_pi_set_memory_uncorrectable(&s->pi[1]);
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
    s->ni.bedrock = s;
    s->pi[0].bedrock = s;
    s->pi[1].bedrock = s;
    qdev_prop_set_uint32(DEVICE(&s->lb), "revision", s->revision);
    /*
     * A vector request from a peer reads or writes this node's registers
     * through the local block, at their window offset.
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
 * hold_seq, presence_seen, presence_abandoned, hold_scratch1_joined,
 * presence_posted, presence_released -- stays out of this description on
 * purpose, the way
 * sgi-hub.c's vmstate_sgi_hub omits its own pi_rt_hold family; post_load
 * puts it through the same reset path a cold reset does.
 *
 * This description still exists, rather than leaving dc->vmsd unset, so this
 * device is accounted for in the migration stream and a future field that
 * does need migrating has a place to land already reviewed for the fields
 * around it.
 *
 * park_mask is the one exception, and travels in a subsection sent only
 * while a processor is parked. A parked processor is halted, and its halted
 * flag migrates with its CPU state; without the mask, the destination would
 * not know the halt is this device's and would never wake it. post_load's
 * hold reset then releases it (park_kind, park_start and park_deadline stay
 * out, as the rest of the join's scheduling state does).
 */
static bool bedrock_park_needed(void *opaque)
{
    SGIBedrockState *s = opaque;

    return s->park_mask != 0;
}

static const VMStateDescription vmstate_sgi_bedrock_park = {
    .name = TYPE_SGI_BEDROCK "/park",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = bedrock_park_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(park_mask, SGIBedrockState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_bedrock = {
    .name = TYPE_SGI_BEDROCK,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = bedrock_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_bedrock_park,
        NULL
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

/* Where the zero-based hub window's two aliases meet: the I/O interface. */
#define BEDROCK_HUB_WINDOW_SPLIT 0x400000

/*
 * Test-only: a register-contract qtest cannot be a processor of the node,
 * so it cannot make a real hold withhold a value, and a guest-code test
 * that does races the hold's own wall-clock bound. These two properties
 * place and end that state directly and nothing else. They are not
 * hardware and no machine sets them.
 *
 * x-test-withhold-scratch1 "SLOT:VALUE" records VALUE as slot SLOT's
 * withheld scratch-1 write, as a held processor's second post would be.
 * x-test-release-scratch1 SLOT lands it as the end of that hold does.
 */
static void bedrock_test_withhold_scratch1(Object *obj, const char *value,
                                           Error **errp)
{
    SGIBedrockState *s = SGI_BEDROCK(obj);
    unsigned long slot;
    uint64_t word;
    const char *rest;

    if (qemu_strtoul(value, &rest, 0, &slot) || *rest != ':' ||
        slot >= SGI_BEDROCK_PROCESSORS ||
        qemu_strtou64(rest + 1, NULL, 0, &word)) {
        error_setg(errp, "x-test-withhold-scratch1 takes SLOT:VALUE");
        return;
    }
    s->hold_scratch1_withheld[slot] = word;
    s->hold_scratch1_withheld_valid[slot] = true;
    s->hold_scratch1_withheld_seq[slot] = ++s->hold_scratch1_seq;
}

static void bedrock_test_release_scratch1(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    SGIBedrockState *s = SGI_BEDROCK(obj);
    uint32_t slot;

    if (!visit_type_uint32(v, name, &slot, errp)) {
        return;
    }
    if (slot >= SGI_BEDROCK_PROCESSORS) {
        error_setg(errp, "x-test-release-scratch1 takes a slot below %u",
                   SGI_BEDROCK_PROCESSORS);
        return;
    }
    bedrock_hold_flush_scratch1(s, slot);
}

static void bedrock_init(Object *obj)
{
    SGIBedrockState *s = SGI_BEDROCK(obj);
    DeviceState *dev = DEVICE(obj);

    s->park_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bedrock_park_expire, s);
    object_initialize_child(obj, "pi0", &s->pi[0], TYPE_SGI_BEDROCK_PI);
    object_initialize_child(obj, "pi1", &s->pi[1], TYPE_SGI_BEDROCK_PI);
    object_initialize_child(obj, "io", &s->io, TYPE_SGI_BEDROCK_IO);
    object_initialize_child(obj, "md", &s->md, TYPE_SGI_BEDROCK_MD);
    object_initialize_child(obj, "lb", &s->lb, TYPE_SGI_BEDROCK_LB);
    object_initialize_child(obj, "ni", &s->ni, TYPE_SGI_BEDROCK_NI);
    object_initialize_child(obj, "xb", &s->xb, TYPE_SGI_BEDROCK_XB);
    memory_region_init(&s->mmio, obj, TYPE_SGI_BEDROCK, SGI_BEDROCK_SIZE);
    /*
     * A vector request from a peer names a hub register by its window
     * offset, so the local block needs the window at offset zero however
     * the machine places mmio. A lone alias of the whole window is not
     * enough: the memory core renders an address space from the aliased
     * region itself when an alias covers all of it, and that region carries
     * its mapped address, so every offset would miss. Two aliases, split at
     * the I/O interface, keep this container the root.
     */
    memory_region_init(&s->hub_window, obj, TYPE_SGI_BEDROCK "-hub-window",
                       SGI_BEDROCK_SIZE);
    memory_region_init_alias(&s->hub_window_part[0], obj,
                             TYPE_SGI_BEDROCK "-hub-window-low", &s->mmio, 0,
                             BEDROCK_HUB_WINDOW_SPLIT);
    memory_region_init_alias(&s->hub_window_part[1], obj,
                             TYPE_SGI_BEDROCK "-hub-window-high", &s->mmio,
                             BEDROCK_HUB_WINDOW_SPLIT,
                             SGI_BEDROCK_SIZE - BEDROCK_HUB_WINDOW_SPLIT);
    memory_region_add_subregion(&s->hub_window, 0, &s->hub_window_part[0]);
    memory_region_add_subregion(&s->hub_window, BEDROCK_HUB_WINDOW_SPLIT,
                                &s->hub_window_part[1]);
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
    /* Region 10: the ECC scrub carrier at four eighths of the node. */
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->md), 5));
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
    /*
     * Settable after realize as well as before: a machine wires it before
     * realize, while a -M none register test can only name a cable after
     * both Bedrocks and the cable exist. Weak, because the cable refers
     * back to this device.
     */
    object_property_add_str(obj, "x-test-withhold-scratch1", NULL,
                            bedrock_test_withhold_scratch1);
    object_property_add(obj, "x-test-release-scratch1", "uint32", NULL,
                        bedrock_test_release_scratch1, NULL, NULL);
    object_property_add_link(obj, "net-cable", TYPE_SGI_NET_CABLE,
                             (Object **)&s->net_cable,
                             object_property_allow_set_link, 0);
}

uint8_t sgi_bedrock_node_number(SGIBedrockState *s)
{
    return sgi_bedrock_lb_node_number(&s->lb);
}

static bool bedrock_neighbor(SGIBedrockState *s, Object **peer,
                             unsigned int *peer_index)
{
    return s->net_cable &&
           sgi_net_cable_neighbor(s->net_cable, OBJECT(s), 0, peer,
                                  peer_index);
}

bool sgi_bedrock_link_up(SGIBedrockState *s)
{
    Object *peer;
    unsigned int peer_index;

    return bedrock_neighbor(s, &peer, &peer_index);
}

void sgi_bedrock_port_reset_send(SGIBedrockState *s)
{
    SGIBedrockState *peer;
    Object *port;
    unsigned int index;
    uint64_t enable = 0;

    if (!bedrock_neighbor(s, &port, &index)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock: port reset with no "
                      "link reaches nothing\n");
        return;
    }
    peer = (SGIBedrockState *)object_dynamic_cast(port, TYPE_SGI_BEDROCK);
    if (!peer) {
        return;
    }
    /*
     * Reset enable bit 0, "warm reset enable", lets a reset arriving over
     * the link reset the Bedrock (the network-interface datasheet). The
     * arriving reset is the same one a local reset request makes, on the
     * model's reset equation.
     */
    sgi_bedrock_ni_regs_read(&peer->ni.regs, 0x10, &enable);
    trace_sgi_bedrock_port_reset_arrives(sgi_bedrock_node_number(peer),
                                         enable & 1);
    if (enable & 1) {
        bedrock_local_reset(peer, 0, 1);
    }
}

SGINetPacketResult sgi_bedrock_vector_send(SGIBedrockState *s,
                                           SGINetPacket *packet)
{
    Object *peer;
    unsigned int peer_index;

    if (!bedrock_neighbor(s, &peer, &peer_index)) {
        return SGI_NET_PACKET_NONE;
    }
    return sgi_net_port_receive(peer, peer_index, packet, &error_abort);
}

static const char *bedrock_net_describe(Object *port, unsigned int index)
{
    return "a Bedrock network port";
}

/*
 * A vector request that arrived over this node's cable. A Bedrock has one
 * port and does not forward, so only a route with nothing left in it ends
 * here, and the local block performs the access on this node's register
 * window. That path applies no CPU permission check, and a register with a
 * per-processor view sees whichever processor launched on the other node;
 * the firmware's targets (the revision word and the scratch registers)
 * depend on neither.
 *
 * A request that makes this node launch back into the sender while the
 * sender's own launch is in flight re-enters the sender's local block,
 * which the memory core's per-device re-entrancy guard refuses: that access
 * fails, so the inner launch latches an address error and QEMU warns once.
 * The firmware never builds such a bounce.
 */
static SGINetPacketResult bedrock_net_receive(Object *port,
                                              unsigned int index,
                                              SGINetPacket *packet,
                                              Error **errp)
{
    SGIBedrockState *s = SGI_BEDROCK(port);
    bool write;

    if (packet->vector) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock: vector route 0x%" PRIx64
                      " continues past a Bedrock, which does not forward\n",
                      packet->vector);
        return SGI_NET_PACKET_NONE;
    }
    switch (packet->op) {
    case SGI_NET_PACKET_READ:
        write = false;
        break;
    case SGI_NET_PACKET_WRITE:
        write = true;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock: vector request type %d is "
                      "not modelled\n", packet->op);
        return SGI_NET_PACKET_NONE;
    }
    if (sgi_bedrock_lb_hub_access(&s->lb, packet->addr,
                                  write ? &packet->data : &packet->value,
                                  write) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock: vector %s of hub "
                      "offset 0x%" HWADDR_PRIx " refused\n",
                      write ? "write" : "read", packet->addr);
        return SGI_NET_PACKET_ERROR;
    }
    packet->source = sgi_bedrock_node_number(s);
    return SGI_NET_PACKET_OK;
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

    timer_free(s->park_timer);
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
    SGINetPortClass *npc = SGI_NET_PORT_CLASS(klass);

    dc->desc = "SGI Bedrock node controller";
    dc->realize = bedrock_realize;
    dc->unrealize = bedrock_unrealize;
    dc->vmsd = &vmstate_sgi_bedrock;
    dc->user_creatable = true;
    device_class_set_props(dc, bedrock_properties);
    rc->phases.enter = bedrock_reset_enter;
    npc->describe = bedrock_net_describe;
    npc->receive = bedrock_net_receive;
}

static const TypeInfo bedrock_info = {
    .name = TYPE_SGI_BEDROCK,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockState),
    .instance_init = bedrock_init,
    .instance_finalize = bedrock_finalize,
    .class_init = bedrock_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_SGI_NET_PORT },
        { }
    },
};

static void bedrock_register_types(void)
{
    type_register_static(&bedrock_info);
}

type_init(bedrock_register_types)
