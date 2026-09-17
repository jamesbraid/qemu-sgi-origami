/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/misc/sgi-l1.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
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
 */

#define SGI_BEDROCK_HOLD_MS 2000 /* mirrors sgi-hub.c's SGI_HUB_RT_HOLD_MS */

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
    unsigned int logical = s->cpu_base;

    if (!current_cpu) {
        return -1;
    }
    for (unsigned int physical = 0; physical < SGI_BEDROCK_PROCESSORS;
         physical++) {
        if (!(s->processor_mask & (1U << physical))) {
            continue;
        }
        if (current_cpu->cpu_index == logical) {
            return (int)physical;
        }
        logical++;
    }
    return -1;
}

/* A physical slot's CPUState, resolved once and cached; NULL if absent. */
static CPUState *bedrock_hold_cpu(SGIBedrockState *s, unsigned int physical)
{
    unsigned int logical = s->cpu_base;

    if (unlikely(!s->hold_cpu[physical])) {
        if (!(s->processor_mask & (1U << physical))) {
            return NULL;
        }
        for (unsigned int p = 0; p < physical; p++) {
            if (s->processor_mask & (1U << p)) {
                logical++;
            }
        }
        s->hold_cpu[physical] = qemu_get_cpu(logical);
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
    return bedrock_hold_scratch1_ready(s);
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
    return kind == SGI_BEDROCK_HOLD_SCRATCH1 ? "scratch1" : "scratch0";
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
    deadline = hold->start + SGI_BEDROCK_HOLD_MS * SCALE_MS;

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

static MemTxResult bedrock_remote_read(void *opaque, hwaddr addr,
                                       uint64_t *value, unsigned size,
                                       MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;
    uint8_t buffer[8];
    MemTxResult result;

    if (bedrock_remote_refused(s, addr, size, "read")) {
        return MEMTX_DECODE_ERROR;
    }
    result = address_space_read(&s->l1_uart_as,
                                addr - BEDROCK_L1_UART_OFFSET,
                                attrs, buffer, size);
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

static MemTxResult bedrock_remote_write(void *opaque, hwaddr addr,
                                        uint64_t value, unsigned size,
                                        MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;
    uint8_t buffer[8];

    if (bedrock_remote_refused(s, addr, size, "write")) {
        return MEMTX_DECODE_ERROR;
    }
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
    return address_space_write(&s->l1_uart_as,
                               addr - BEDROCK_L1_UART_OFFSET,
                               attrs, buffer, size);
}

static const MemoryRegionOps bedrock_remote_ops = {
    .read_with_attrs = bedrock_remote_read,
    .write_with_attrs = bedrock_remote_write,
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

static void bedrock_local_reset(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    if (level) {
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
        qdev_prop_set_uint32(DEVICE(&s->pi[i]), "cpu-base", s->cpu_base);
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
