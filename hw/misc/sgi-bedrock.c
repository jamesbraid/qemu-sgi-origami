/* SPDX-License-Identifier: BSD-3-Clause */
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
 * Processors joined where the PROM assumes lockstep; the model has no
 * shared clock.
 *
 * The stock IP35 PROM (6.210) brings a node's processors up on the
 * assumption that they left reset on the same clock edge and have run
 * continuously since. Four of its early handshakes rely on that: each
 * orders one processor's stores before another's reads by nothing more
 * than both running at hardware speed, with a margin of microseconds to a
 * few milliseconds. The model has no shared clock. Each processor is a TCG
 * vCPU, and under multi-threaded TCG a host thread can fall seconds behind
 * its peers, so each of those orderings can fail. At each of the four
 * points the model therefore parks the processor that arrived early until
 * the ones it must follow have caught up. That is one approximation, not
 * hardware behaviour, and it is bounded: a park changes when the parked
 * processor runs next, never a value the guest reads, and every park ends
 * at SGI_BEDROCK_HOLD_MS of virtual time whether or not its condition
 * arrives. The four, in boot order:
 *
 * - The reset barrier (SGI_BEDROCK_HOLD_SCRATCH1). Every processor clears
 *   LB scratch 1 on its reset path (0x1fc007ec in 6.210), and the master
 *   then posts its release there (0x1fc01278), which the others poll for
 *   with no timeout. A clear that lands after the release erases it. So a
 *   processor's second or later write is withheld, and the writer parked,
 *   until every other running processor has made its first write; the
 *   withheld value lands when the park ends, before the writer runs again.
 *
 * - The pair liveness handshake (SGI_BEDROCK_HOLD_LIVENESS), described
 *   with the presence publication below: the first of a pair to post its
 *   status is parked until its partner posts.
 *
 * - The presence publication, publisher side (SGI_BEDROCK_HOLD_PRESENCE)
 *   and partner side (SGI_BEDROCK_HOLD_PARTNER), described below.
 *
 * A park halts the processor rather than blocking its host thread, so it
 * works the same under multi-threaded TCG, round robin and -icount: see
 * bedrock_park_start(). The later rendezvous in the copy phase has no park:
 * with these four, the stock PROM reaches its copy with the processors in
 * step, and a boot campaign with no park there saw no copy-phase stall.
 *
 * "Running" means populated (processor-mask) and schedulable (not halted by
 * the guest, cpu_can_run()); a processor halted by a park still counts. The
 * PI cpu-enable register plays no part. On this Origin 300 model every
 * populated processor's vCPU runs from sn1_machine_reset() whatever that
 * register says (Bedrock's cpu-enable GPIO output is not wired in
 * hw/mips/sgi/sn1-node.c); the node's cpu-enable-initial-mask grants every
 * fitted slot at reset, itself an approximation, and the PROM clears a
 * partner's bit when that partner fails its liveness check. Gating the
 * joins on it would tie them to that approximation and to the PROM's own
 * verdict. Which vCPU is in which slot comes from the board's
 * processor-slot-order: see bedrock_resolve_slot_processors().
 */

#define SGI_BEDROCK_HOLD_MS 2000 /* mirrors sgi-hub.c's SGI_HUB_RT_HOLD_MS */

/*
 * The presence publication. Right after the reset barrier the stock PROM's
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
 * zeros.
 *
 * The publisher park restores that ordering. It parks the publishers
 * only: a publisher whose own interface has published is parked at the hub
 * lock release that follows, until the other populated interface has
 * published too (SGI_BEDROCK_HOLD_PRESENCE). Its first
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
 * measured.
 *
 * The liveness handshake itself gets a park too (SGI_BEDROCK_HOLD_LIVENESS):
 * the first of a pair to post is parked until its partner posts, since a
 * partner descheduled by the host can otherwise post after the poll has run
 * out. The post is recognised as a processor's first write to its own
 * compare register after the join, whatever the value; the PROM writes
 * nothing else there in between.
 *
 * The join is over once every populated interface has published, so the
 * parks cannot fire again until reset; and a publisher's or partner's park
 * that times out abandons the join for the rest of the reset rather than
 * parking every later release for the bound again, since a publisher that
 * never comes is a processor the PROM's own console will report as failed.
 * A liveness park that times out ends only itself: the partner that never
 * posted is one the PROM disables, and the join goes on without it.
 *
 * The join state is also cleared by a migration load and by a local reset
 * (see sgi_bedrock_hold_reset()), so a guest past the PROM that wrote LB
 * scratch 1 twice and then its own compare register would be parked up to
 * the bound. No such guest is known; the PROM is the only writer of LB
 * scratch 1 seen so far.
 */

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
 * Populated and actually able to run -- see "Running" in the overview
 * above for why this does not also check the PI cpu-enable register. A
 * processor this device has halted for a park still counts as running:
 * the halt is the model's, not the guest's.
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

/* The presence join: every populated interface has published, or it is over. */
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
 * The reset barrier: every other present, running processor has made its
 * first scratch-1 write since reset.
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
    case SGI_BEDROCK_HOLD_PRESENCE:
        /* A publisher, parked until the other interface publishes too. */
        return !bedrock_presence_other_pending(s, slot);
    case SGI_BEDROCK_HOLD_PARTNER:
        /* A partner, parked until its own interface's publisher is done. */
        return s->presence_abandoned ||
               s->presence_released[slot / SGI_BEDROCK_PI_PROCESSORS];
    case SGI_BEDROCK_HOLD_LIVENESS:
        /* A poster, parked until its partner (the other slot) posts too. */
        return s->presence_posted[slot ^ 1] ||
               !bedrock_hold_slot_running(s, slot ^ 1);
    case SGI_BEDROCK_HOLD_SCRATCH1:
    default:
        return bedrock_hold_scratch1_ready(s);
    }
}

/*
 * Land a processor's own withheld scratch-1 write, if one is waiting: at
 * that processor's next scratch-1 write, or when its park ends, before it
 * runs again, so it never observes a post it has already taken back.
 */
static void bedrock_hold_flush_scratch1(SGIBedrockState *s,
                                        unsigned int slot)
{
    if (!s->hold_scratch1_withheld_valid[slot]) {
        return;
    }
    s->lb.scratch[1] = s->hold_scratch1_withheld[slot];
    s->hold_scratch1_withheld_valid[slot] = false;
    s->hold_scratch1_released = true;
}

/*
 * An access to scratch 1 from outside this node's processors: the peer's
 * vector engine, which is how the global master tells a node its NASID
 * during node number assignment, or a qtest. The
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
    case SGI_BEDROCK_HOLD_PRESENCE:
        return "presence";
    case SGI_BEDROCK_HOLD_PARTNER:
        return "partner";
    case SGI_BEDROCK_HOLD_LIVENESS:
        return "liveness";
    case SGI_BEDROCK_HOLD_SCRATCH1:
    default:
        return "scratch1";
    }
}

/*
 * Every park halts the parked processor instead of blocking its host
 * thread. A blocked thread is fine under multi-threaded TCG, but under
 * round-robin or icount the one TCG thread runs every processor, so a
 * blocking park would stall the very processor it waits for; that is why
 * the joins used to apply to multi-threaded TCG alone and an icount boot
 * lost processor C for good. A
 * halted processor costs the others nothing under any scheduler. The halt
 * is the model's, not the guest's: the processor stops at the end of the
 * translation block holding the access that parked it and resumes where
 * that block left off, with nothing guest-visible changed. The limitation:
 * a guest WAIT on a processor an interrupt woke early could be ended early
 * by the release; the PROM runs these barriers with interrupts disabled.
 * The bound is on the virtual clock, so an icount run stays deterministic.
 * Under multi-threaded TCG the virtual clock follows host time, as the old
 * realtime bound did. Only an access this node's own processor issued can
 * park, and not a processor already parked. Returns the executing slot, or
 * -1.
 */
static int bedrock_park_slot(SGIBedrockState *s)
{
    int cslot = bedrock_hold_current_slot(s);

    if (cslot < 0 || !bedrock_hold_applicable(s)) {
        return -1;
    }
    return s->park_kind[cslot] ? -1 : cslot;
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
                                 bedrock_hold_kind_name(kind), slot, now);
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
                               (now - s->park_start[slot]) / SCALE_US);
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
 * A publisher's or partner's park that reaches its bound ends the join for
 * this reset: the publisher it waited for is not coming, and parking every
 * later release for the bound again would only stretch the boot the PROM
 * is about to report on. The remaining parks re-check against the abandoned
 * join and release. A liveness or reset-barrier park reaching its bound
 * ends only itself: the processor it waited for is one the PROM reports.
 */
static void bedrock_park_expire(void *opaque)
{
    SGIBedrockState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool expired = false;

    for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
        if (s->park_kind[slot] && now >= s->park_deadline[slot]) {
            expired |= s->park_kind[slot] == SGI_BEDROCK_HOLD_PRESENCE ||
                       s->park_kind[slot] == SGI_BEDROCK_HOLD_PARTNER;
            bedrock_park_release(s, slot, "timed out");
        }
    }
    if (expired) {
        s->presence_abandoned = true;
    }
    bedrock_park_recheck(s);
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
        /* A processor parked on the barrier re-checks it. */
        bedrock_park_recheck(s);
        return;
    }
    /*
     * A second write before the barrier is over is the master's release of
     * the others (0x7f80000 at 0x1fc01278 in the stock PROM), not a join:
     * the master's next write to its own compare register is a progress
     * code, not its liveness post. Every processor's join is its write in
     * main (0x1fc69a00), after the release.
     */
    if (s->hold_scratch1_released) {
        s->hold_scratch1_joined[slot] = true;
    }

    if (bedrock_park_slot(s) != cslot || bedrock_hold_scratch1_ready(s)) {
        s->lb.scratch[1] = value;
        s->hold_scratch1_released = true;
        return;
    }

    /*
     * The processor runs on to the end of the translation block holding
     * this store before the halt takes it; in PROM 6.210 the release is
     * followed by a return, so nothing in between reads scratch 1.
     */
    s->hold_scratch1_withheld[slot] = value;
    s->hold_scratch1_withheld_valid[slot] = true;
    s->hold_scratch1_withheld_seq[slot] = ++s->hold_scratch1_seq;
    bedrock_park_start(s, slot, SGI_BEDROCK_HOLD_SCRATCH1);
}

void sgi_bedrock_scratch3_released(SGIBedrockState *s)
{
    int cslot;

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
     * The publisher park: a publisher releasing the hub lock after its own
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
    /* A publisher parked on the presence join re-checks it. */
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
        !s->hold_scratch1_joined[cslot] || s->presence_posted[cslot]) {
        return;
    }
    s->presence_posted[cslot] = true;

    /* A partner parked on its own liveness post re-checks. */
    bedrock_park_recheck(s);

    /*
     * The pair liveness park. Each processor of a pair posts here and then
     * polls its partner's register for the post, for 200,000 reads; a
     * poller that gives up clears its partner's CPU enable, and the PROM
     * reports the partner as "CPU failed early init". On hardware both
     * processors reach the post off one clock, microseconds apart. In the
     * model each runs the PROM's fixed 3 s real-time counter delay before it
     * on its own host thread, which can spend most of that time waiting for
     * a host processor, and the partner can post after the poll has run
     * out. So the
     * first of a pair to post is parked, its post already stored, until its
     * partner posts; both polls then succeed at once. A partner that never
     * posts releases it at the bound, and the PROM disables that partner as
     * it would a dead one. Nothing guest-visible changes.
     */
    if (bedrock_park_slot(s) != cslot ||
        bedrock_hold_ready(s, (unsigned int)cslot,
                           SGI_BEDROCK_HOLD_LIVENESS)) {
        return;
    }
    bedrock_park_start(s, (unsigned int)cslot, SGI_BEDROCK_HOLD_LIVENESS);
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
 * Release every park, and forget every first-write and withheld value: a
 * fresh start. Called from this device's
 * own reset, from the local-reset GPIO (bedrock_local_reset, since that
 * resets the LB and II registers this state tracks without cold-resetting
 * this device itself), and from unrealize.
 */
void sgi_bedrock_hold_reset(SGIBedrockState *s)
{
    for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
        CPUState *cs = bedrock_hold_cpu(s, slot);

        s->hold_scratch1_seen[slot] = false;
        s->hold_scratch1_withheld_valid[slot] = false;
        s->hold_scratch1_joined[slot] = false;
        s->presence_posted[slot] = false;
        /*
         * A parked processor is halted by this device alone; wake it, or a
         * local reset would leave it stopped for good where no board
         * restarts the processors on node-reset. The unhalt work checks
         * park_mask when it runs.
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
    for (unsigned int i = 0; i < SGI_BEDROCK_PROCESSORS /
         SGI_BEDROCK_PI_PROCESSORS; i++) {
        s->presence_seen[i] = false;
        s->presence_released[i] = false;
    }
    s->presence_abandoned = false;
    s->hold_scratch1_released = false;
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
 * reach the same UART at offset 0x80;
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
    qemu_log_mask(LOG_UNIMP, "sgi-bedrock: XIO read of hub register "
                  "0x%" HWADDR_PRIx " is not supported\n", addr);
    *value = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult bedrock_io_registers_write(void *opaque, hwaddr addr,
                                              uint64_t value, unsigned size,
                                              MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;

    if (!sgi_bedrock_available(s)) {
        return MEMTX_ACCESS_ERROR;
    }
    switch (addr) {
    case BEDROCK_PI_MODIFY_OFFSET:
        sgi_bedrock_pi_io_modify(&s->pi[0], value, s->local_region);
        return MEMTX_OK;
    case BEDROCK_PI1_OFFSET + BEDROCK_PI_MODIFY_OFFSET:
        sgi_bedrock_pi_io_modify(&s->pi[1], value, s->local_region);
        return MEMTX_OK;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock: XIO write of 0x%"
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
 * interface, so it is broadcast to both PI0 and PI1's own pending bit 4
 * the same way lb-error, ni-error and
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
        qemu_irq_pulse(s->node_reset);
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

/*
 * ---- A processor's I/O-space path ----
 *
 * Every uncached I/O-space request a processor makes enters its own
 * processor interface. The node places one region per fitted slot over that
 * processor's uncached view of the I/O space; each forwards to the
 * processor's own view, so it sees the whole physical address and knows the
 * slot. That is where a graphics store is counted: an uncached I/O store
 * into the slot's enabled graphics page costs the slot one credit. The
 * store goes on either way. Only a processor's uncached requests take this
 * view, so neither a debugger's access nor a device's DMA is counted.
 */
#define BEDROCK_IO_SPACE_SIZE   (1ULL << 40)

static bool bedrock_neighbor(SGIBedrockState *s, Object **peer,
                             unsigned int *peer_index);

static MemTxResult bedrock_pi_io_read(void *opaque, hwaddr addr,
                                      uint64_t *data, unsigned size,
                                      MemTxAttrs attrs)
{
    struct SGIBedrockProcessorPath *path = opaque;
    SGIBedrockState *s = path->bedrock;
    uint8_t bytes[8] = { 0 };
    MemTxResult result;
    bool previous;

    previous = sgi_pi_gfx_mark_store(false);
    result = address_space_read(&s->pi_view_as[path->slot],
                                s->io_space_base + addr, attrs,
                                bytes + 8 - size, size);
    sgi_pi_gfx_unmark_store(previous);
    *data = ldq_be_p(bytes);
    return result;
}

static MemTxResult bedrock_pi_io_write(void *opaque, hwaddr addr,
                                       uint64_t data, unsigned size,
                                       MemTxAttrs attrs)
{
    struct SGIBedrockProcessorPath *path = opaque;
    SGIBedrockState *s = path->bedrock;
    uint8_t bytes[8];
    MemTxResult result;
    bool counted, previous;

    counted = sgi_pi_gfx_count_store(
        &s->pi[path->slot / SGI_BEDROCK_PI_PROCESSORS].gfx,
        path->slot % SGI_BEDROCK_PI_PROCESSORS, addr, "sgi-bedrock");
    stq_be_p(bytes, data);
    /* Marked, so a head returns credit only for a counted store. */
    previous = sgi_pi_gfx_mark_store(counted);
    result = address_space_write(&s->pi_view_as[path->slot],
                                 s->io_space_base + addr, attrs,
                                 bytes + 8 - size, size);
    sgi_pi_gfx_unmark_store(previous);
    return result;
}

static const MemoryRegionOps bedrock_pi_io_ops = {
    .read_with_attrs = bedrock_pi_io_read,
    .write_with_attrs = bedrock_pi_io_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void bedrock_init_processor_paths(SGIBedrockState *s)
{
    for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
        g_autofree char *view_name = NULL;
        g_autofree char *io_name = NULL;

        if (!s->cpu_sysmem[slot]) {
            continue;
        }
        view_name = g_strdup_printf("sgi-bedrock.cpu%u.view", slot);
        io_name = g_strdup_printf("sgi-bedrock.cpu%u.io", slot);
        s->pi_path[slot].bedrock = s;
        s->pi_path[slot].slot = slot;
        address_space_init(&s->pi_view_as[slot], s->cpu_sysmem[slot],
                           view_name);
        memory_region_init_io(&s->pi_io[slot], OBJECT(s), &bedrock_pi_io_ops,
                              &s->pi_path[slot], io_name,
                              BEDROCK_IO_SPACE_SIZE);
        /* The forwarded request may come back into this device. */
        s->pi_io[slot].disable_reentrancy_guard = true;
        s->pi_io_ready[slot] = true;
    }
}

MemoryRegion *sgi_bedrock_processor_io(SGIBedrockState *s, unsigned int slot)
{
    g_assert(slot < SGI_BEDROCK_PROCESSORS);
    return s->pi_io_ready[slot] ? &s->pi_io[slot] : NULL;
}

/*
 * A graphics head behind this node returned credits: the node-widget map
 * names the node, the interface and the slot that get them. Another node is
 * the one at the far end of this node's cable, when it carries that number.
 */
static void bedrock_return_gfx_credits(Object *obj, unsigned int widget,
                                       uint32_t credits)
{
    SGIBedrockState *s = SGI_BEDROCK(obj);
    uint64_t map[2] = {
        sgi_bedrock_io_gfx_map(&s->io, 0), sgi_bedrock_io_gfx_map(&s->io, 1),
    };
    SGIPIGfxRoute route;
    SGIBedrockState *target = NULL;
    Object *peer;
    unsigned int peer_index;

    if (!sgi_pi_gfx_route_bedrock(map, widget, &route)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock: %u graphics credits "
                      "from widget 0x%x match no node-widget map entry\n",
                      credits, widget);
        return;
    }
    if (route.node == sgi_bedrock_node_number(s)) {
        target = s;
    } else if (bedrock_neighbor(s, &peer, &peer_index)) {
        target = (SGIBedrockState *)object_dynamic_cast(peer,
                                                        TYPE_SGI_BEDROCK);
        if (target && sgi_bedrock_node_number(target) != route.node) {
            target = NULL;
        }
    }
    if (!target) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock: %u graphics credits "
                      "from widget 0x%x name node %u, which no reachable "
                      "Bedrock carries\n", credits, widget, route.node);
        return;
    }
    sgi_pi_gfx_add_credits(&target->pi[route.pi].gfx, route.slot, credits,
                           "sgi-bedrock");
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
    bedrock_init_processor_paths(s);
}

/*
 * End any park a migration snapshot caught: the stream carries no park
 * condition to wait on, so every parked processor is released, as a local
 * reset releases it. A withheld scratch-1 write lands first, in write
 * order, as it would have when its park ended: the master's release is a
 * value the other processors poll for with no timeout, so dropping it
 * would leave them polling for good. The LB's own state, scratch 1 among
 * it, has loaded by now; the LB is realized inside bedrock_realize(), so
 * its section comes first in the stream.
 */
static int bedrock_post_load(void *opaque, int version_id)
{
    SGIBedrockState *s = opaque;

    bedrock_scratch1_external(s);
    sgi_bedrock_hold_reset(s);
    return 0;
}

/*
 * SGIBedrockState carries no migrated fields of its own outside the two
 * park subsections described last here. Its registers live
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
 * park scheduling state documented on hold_scratch1_seen and the fields
 * after it in sgi-bedrock.h: bounded, and never itself a value the guest
 * reads. That state -- hold_cpu, hold_scratch1_seen, hold_scratch1_seq,
 * presence_seen, presence_abandoned, hold_scratch1_released,
 * hold_scratch1_joined, presence_posted, presence_released, park_kind,
 * park_start and park_deadline -- stays out of this description on
 * purpose, the way sgi-hub.c's vmstate_sgi_hub omits its own pi_rt_hold
 * family; post_load puts it through the same reset path a local reset
 * does.
 *
 * This description still exists, rather than leaving dc->vmsd unset, so this
 * device is accounted for in the migration stream and a future field that
 * does need migrating has a place to land already reviewed for the fields
 * around it.
 *
 * Two parts of the park are exceptions, each in a subsection sent only
 * while it holds something. park_mask travels while a processor is parked.
 * A parked processor is halted, and its halted flag migrates with its CPU
 * state; without the mask, the destination would not know the halt is this
 * device's and would never wake it. post_load's hold reset then releases
 * it. The withheld scratch-1 writes travel while one is waiting, with the
 * order they were written in: each is a value the guest reads once it
 * lands, and post_load lands them.
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

static bool bedrock_withheld_needed(void *opaque)
{
    SGIBedrockState *s = opaque;

    for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
        if (s->hold_scratch1_withheld_valid[slot]) {
            return true;
        }
    }
    return false;
}

static const VMStateDescription vmstate_sgi_bedrock_withheld = {
    .name = TYPE_SGI_BEDROCK "/withheld-scratch1",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = bedrock_withheld_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL_ARRAY(hold_scratch1_withheld_valid, SGIBedrockState,
                           SGI_BEDROCK_PROCESSORS),
        VMSTATE_UINT64_ARRAY(hold_scratch1_withheld, SGIBedrockState,
                             SGI_BEDROCK_PROCESSORS),
        VMSTATE_UINT64_ARRAY(hold_scratch1_withheld_seq, SGIBedrockState,
                             SGI_BEDROCK_PROCESSORS),
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
        &vmstate_sgi_bedrock_withheld,
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
    DEFINE_PROP_UINT64("io-space-base", SGIBedrockState, io_space_base, 0),
#define BEDROCK_CPU_SYSMEM_PROP(_n) \
    DEFINE_PROP_LINK("cpu-sysmem" #_n, SGIBedrockState, cpu_sysmem[_n], \
                     TYPE_MEMORY_REGION, MemoryRegion *)
    BEDROCK_CPU_SYSMEM_PROP(0), BEDROCK_CPU_SYSMEM_PROP(1),
    BEDROCK_CPU_SYSMEM_PROP(2), BEDROCK_CPU_SYSMEM_PROP(3),
#undef BEDROCK_CPU_SYSMEM_PROP
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
 * so it cannot make a real park withhold a value, and a guest-code test
 * would have to stop the processors at the moment the park holds one.
 * These two properties place and end that state directly and nothing
 * else. They are not hardware and no machine sets them.
 *
 * x-test-withhold-scratch1 "SLOT:VALUE" records VALUE as slot SLOT's
 * withheld scratch-1 write, as a parked processor's second post would be.
 * x-test-release-scratch1 SLOT lands it as the end of that park does.
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
    qdev_init_gpio_out_named(dev, &s->node_reset, "node-reset", 1);
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
     * the link reset the Bedrock. The
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
bool sgi_bedrock_available(SGIBedrockState *s)
{
    return !s || !s->available || s->available(s->available_opaque);
}

static SGINetPacketResult bedrock_net_receive(Object *port,
                                              unsigned int index,
                                              SGINetPacket *packet,
                                              Error **errp)
{
    SGIBedrockState *s = SGI_BEDROCK(port);
    bool write;

    if (!sgi_bedrock_available(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock: main power off\n");
        return SGI_NET_PACKET_ERROR;
    }
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
    SGIBedrockState *s = SGI_BEDROCK(dev);

    sgi_bedrock_hold_reset(s);
    for (unsigned int slot = 0; slot < SGI_BEDROCK_PROCESSORS; slot++) {
        if (!s->pi_io_ready[slot]) {
            continue;
        }
        if (s->pi_io[slot].container) {
            memory_region_del_subregion(s->pi_io[slot].container,
                                        &s->pi_io[slot]);
        }
        address_space_destroy(&s->pi_view_as[slot]);
        s->pi_io_ready[slot] = false;
    }
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
    SGIPIGfxClass *gpc = SGI_PI_GFX_CLASS(klass);

    dc->desc = "SGI Bedrock node controller";
    dc->realize = bedrock_realize;
    dc->unrealize = bedrock_unrealize;
    dc->vmsd = &vmstate_sgi_bedrock;
    dc->user_creatable = true;
    device_class_set_props(dc, bedrock_properties);
    rc->phases.enter = bedrock_reset_enter;
    npc->describe = bedrock_net_describe;
    npc->receive = bedrock_net_receive;
    gpc->return_credits = bedrock_return_gfx_credits;
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
        { TYPE_SGI_PI_GFX },
        { }
    },
};

static void bedrock_register_types(void)
{
    type_register_static(&bedrock_info);
}

type_init(bedrock_register_types)
