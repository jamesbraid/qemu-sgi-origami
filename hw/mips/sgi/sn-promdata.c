/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Per-CPU cache-as-RAM window shared by sgi-sn0-node and sgi-sn1-node. */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "system/reset.h"
#include "trace.h"
#include "cpu.h"
#include "sn-promdata.h"

/*
 * IP27 PROM software boot status uses FPR 0, not a hardware register. Bit 1
 * marks cache-as-RAM in the IRIX 6.5.7 PROM.
 */
#define SN_PROM_BOOT_STATUS_CACHE_AS_RAM (1U << 1)

/*
 * Poll each processor every 10 ms for its stack switch and OS handoff. Delay
 * extends the mapped block or reader-local routing, respectively.
 */
#define SN_PROMDATA_POLL_NS (10 * SCALE_MS)

/*
 * Give a mapped CPU 6000 more polls after a sibling releases. Observed
 * residency is far shorter; QEMU_CLOCK_VIRTUAL stretches under host load. A
 * live sibling exits within a few polls. A disabled CPU never exits; the PROM
 * reports "CPU failed early init", so its window must not stay mapped.
 */
#define SN_PROMDATA_STRAGGLER_LIMIT 6000

/*
 * A virtual address of this processor's, translated through the MMU rather
 * than pattern-matched, because the phase does not stay on the unmapped
 * aliases: besides the compatibility segments and XKPHYS, a guarded probe can
 * run from a TLB-mapped alias of the flash, and under ERL the low region is
 * an unmapped alias of physical memory. A node-qualified physical address
 * reduces to its node-local offset. False when the address does not
 * translate.
 */
static bool sn_promdata_local_phys(SNPromDataState *s, uint64_t vaddr,
                                   uint64_t *local)
{
    TranslateForDebugResult r;
    uint64_t phys;

    if (!cpu_translate_for_debug(CPU(s->cpu), vaddr, &r)) {
        return false;
    }
    /*
     * An uncached alias names the same node-local text: drop the
     * uncached-attribute window above the address, then the node.
     */
    phys = r.physaddr & ((1ULL << MIPS_UNCACHED_WINDOW_SHIFT) - 1);
    if (phys - s->node_base < s->node_memory_size) {
        phys -= s->node_base;
    }
    *local = phys;
    return true;
}

/*
 * The phase's execution homes, from the full 64-bit PC. Comparing physically
 * also admits the cached alias of a RAM copy of the PROM; harmless, because
 * the phase bit already over-covers the residency. Post-exit PROM code does
 * touch the window: on the eight-processor Origin 2000 a node's first
 * processor makes 28,673 more doubleword accesses after its stack switch, a
 * memory-test pass, and the held latch serves them from the private block
 * until the OS handoff.
 */
static bool sn_promdata_pc_in_prom(SNPromDataState *s)
{
    uint64_t phys;

    if (!sn_promdata_local_phys(s, s->cpu->env.active_tc.PC, &phys)) {
        return false;
    }

    return (phys >= s->flash_offset &&
            phys < s->flash_offset + s->flash_size) ||
           (s->copy_size != 0 &&
            phys >= s->copy_offset && phys < s->copy_offset + s->copy_size);
}

/* The stack pointer, resolved the same way, still inside the window. */
static bool sn_promdata_sp_in_window(SNPromDataState *s)
{
    uint64_t phys;

    return sn_promdata_local_phys(s, s->cpu->env.active_tc.gpr[29], &phys) &&
           phys >= SN_PROMDATA_PADDR &&
           phys < SN_PROMDATA_PADDR + SN_PROMDATA_SIZE;
}

/*
 * Leave private mode when both the FPR 0 phase bit and PROM execution PC have
 * cleared. Check the bit first to avoid PC translation on private accesses.
 * The poll also calls this test for CPUs that stop using the filter handlers.
 */
static bool sn_promdata_should_leave(SNPromDataState *s)
{
    return !(s->cpu->env.active_fpu.fpr[0].d &
             SN_PROM_BOOT_STATUS_CACHE_AS_RAM) &&
           !sn_promdata_pc_in_prom(s);
}

static void sn_promdata_trace_phase(SNPromDataState *s, const char *event)
{
    CPUMIPSState *env = &s->cpu->env;
    uint64_t peer_writes = s->poll->peer_writes ?
        s->poll->peer_writes(s->poll->peer) : 0;

    trace_sn_promdata_phase(s->cpu->parent_obj.cpu_index, event,
                            env->active_tc.PC, env->active_tc.gpr[29],
                            env->CP0_EntryHi & 0xff, s->private_reads,
                            s->private_writes, peer_writes,
                            qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
}

/*
 * Poll each mapped CPU on itself; rearm while one still needs the window.
 * PROM 6.156 gives the first CPU 1.92-1.96 million private accesses versus
 * 95-131 thousand for the second. It usually releases last, but a slower
 * sibling can release later without being disabled. Index-based exemption
 * left that sibling's window mapped for the run after a one-poll delay.
 * Give either CPU SN_PROMDATA_STRAGGLER_LIMIT more polls after its sibling
 * releases. The bound admits live CPUs without running the 100 Hz timer
 * forever for a disabled CPU ("CPU failed early init" on eight-CPU boots).
 *
 * Keep watching phase-private CPUs without a fast mapping, with no straggler
 * bound. Fast accesses bypass the filter; only filter accesses run
 * sn_promdata_is_private(). After the stack switch, all but the CPU doing
 * node-ID renumbering stopped touching the filter in an eight-CPU boot.
 * Without polling, phase_private and prom-copy-local remain latched for the
 * run. sn_promdata_check_stack() applies the same handoff test here. A CPU
 * parked in PROM after its stack switch, or on a node without the fast path,
 * costs 100 Hz until it exits. A mapped CPU disabled before its stack switch
 * remains subject to the straggler bound.
 */
static void sn_promdata_check_stack(CPUState *cs, run_on_cpu_data data);
static void sn_promdata_leave_private(CPUState *cs, run_on_cpu_data data);

static void sn_promdata_poll_cb(void *opaque)
{
    SNPromDataPoll *poll = opaque;
    bool any_released = false;
    bool rearm = false;
    unsigned int i;

    for (i = 0; i < poll->count; i++) {
        if (poll->slots[i].present && !poll->slots[i].fast) {
            any_released = true;
            break;
        }
    }

    for (i = 0; i < poll->count; i++) {
        SNPromDataState *s = &poll->slots[i];
        bool watched = s->present &&
            (s->fast || (s->phase_private && !s->leaving_private));

        if (!watched) {
            continue;
        }
        if (s->fast) {
            if (!any_released) {
                s->straggler_polls = 0;
            } else if (++s->straggler_polls > SN_PROMDATA_STRAGGLER_LIMIT) {
                continue;
            }
        }
        /*
         * A processor that is not running yet has the previous sample still
         * queued; one is enough, however long it stays stopped.
         */
        if (cpu_work_list_empty(CPU(s->cpu))) {
            async_run_on_cpu(CPU(s->cpu), sn_promdata_check_stack,
                             RUN_ON_CPU_HOST_PTR(s));
        }
        rearm = true;
    }
    if (rearm) {
        timer_mod(poll->timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SN_PROMDATA_POLL_NS);
    }
}

void sn_promdata_poll_init(SNPromDataPoll *poll, SNPromDataState *slots,
                          unsigned int count,
                          uint64_t (*peer_writes)(const void *peer),
                          const void *peer)
{
    poll->slots = slots;
    poll->count = count;
    poll->peer_writes = peer_writes;
    poll->peer = peer;
    /*
     * Poll every node, including those without fast mapping. A CPU may stop
     * touching the filter after OS handoff; without the poll, phase-private
     * routing of the window and PROM RAM copy persists. On multi-node systems
     * that misroutes remote memory to the reader's node. Fast mode controls
     * only whether the private block is mapped as RAM.
     */
    poll->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sn_promdata_poll_cb, poll);
}

void sn_promdata_poll_free(SNPromDataPoll *poll)
{
    if (poll->timer) {
        timer_free(poll->timer);
        poll->timer = NULL;
    }
}

/*
 * Map private RAM above the filter after phase recognition. Cached loads and
 * stores resolve to host pointers without dispatch or the big lock. The
 * second address space still sends uncached accesses through the filter; OS
 * handoff also needs a filter access. Polling restores the filter at the
 * stack switch, before handoff. A cached fetch from the mapped window would
 * execute RAM, not end the phase; PROM never fetches there, and OS execution
 * follows the switch.
 */
static void sn_promdata_latch(SNPromDataState *s)
{
    s->phase_private = true;
    if (s->fast_allowed) {
        s->fast = true;
        memory_region_set_enabled(&s->data, true);
    }
    if (!timer_pending(s->poll->timer)) {
        timer_mod(s->poll->timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SN_PROMDATA_POLL_NS);
    }
    sn_promdata_trace_phase(s, "latch");
}

/*
 * After residency, the filter serves the same block and checks release on
 * each access. Unmapping is one-way until reset.
 */
static void sn_promdata_unmap(SNPromDataState *s, const char *event)
{
    if (s->fast) {
        s->fast = false;
        memory_region_set_enabled(&s->data, false);
        sn_promdata_trace_phase(s, event);
    }
}

/*
 * On the CPU thread, an SP outside the window marks the PROM's move to its
 * real per-CPU stack. All eight CPUs showed this on PROM 6.156 boots.
 */
static void sn_promdata_check_stack(CPUState *cs, run_on_cpu_data data)
{
    SNPromDataState *s = data.host_ptr;

    BQL_LOCK_GUARD();
    if (s->fast && !sn_promdata_sp_in_window(s)) {
        sn_promdata_unmap(s, "stack-switch");
    }

    /*
     * The stack switch drops only the fast mapping. Poll the handoff test
     * too:
     * a CPU may never access the filter again after unmapping, and only a
     * filter access calls sn_promdata_is_private().
     */
    if (s->phase_private && !s->leaving_private &&
        sn_promdata_should_leave(s)) {
        s->phase_private = false;
        s->leaving_private = true;
        async_safe_run_on_cpu(CPU(s->cpu), sn_promdata_leave_private,
                              RUN_ON_CPU_HOST_PTR(s));
    }
}

/*
 * Boot-status bit 1 plus a PROM execution PC enters cache-as-RAM. The window
 * holds this CPU's resident PROM stack until reset or OS handoff. Clearing
 * the bit or moving the PC alone cannot reroute resident cache lines; handoff
 * requires both the bit clear and the PC outside every PROM execution home,
 * first reached in the OS. Uncached accesses always forward, letting PROM
 * copy persistent structures to RAM on exit. sn_promdata_check_stack() also
 * checks handoff from the node poll when no filter access occurs.
 */
static bool sn_promdata_is_private(SNPromDataState *s, MemTxAttrs attrs)
{
    if (!current_cpu || current_cpu != CPU(s->cpu)) {
        return false;
    }

    if (attrs.mips_uncached) {
        return false;
    }

    if (s->leaving_private) {
        return false;
    }

    if (s->phase_private) {
        if (sn_promdata_should_leave(s)) {
            s->phase_private = false;
            s->leaving_private = true;
            async_safe_run_on_cpu(CPU(s->cpu), sn_promdata_leave_private,
                                  RUN_ON_CPU_HOST_PTR(s));
        }
        return s->phase_private;
    }

    if ((s->cpu->env.active_fpu.fpr[0].d &
         SN_PROM_BOOT_STATUS_CACHE_AS_RAM) &&
        sn_promdata_pc_in_prom(s)) {
        sn_promdata_latch(s);
        if (s->copy_size != 0) {
            memory_region_set_enabled(&s->copy_local, true);
        }
    }

    return s->phase_private;
}

/*
 * Only the operating system can execute from inside the window; the PROM
 * never does. Execution here ends the private phase for good. The PC is
 * translated through the MMU because kernels run mapped.
 */
static bool sn_promdata_executing_here(SNPromDataState *s)
{
    TranslateForDebugResult r;

    if (!cpu_translate_for_debug(CPU(s->cpu), s->cpu->env.active_tc.PC, &r)) {
        return false;
    }

    return r.physaddr >= SN_PROMDATA_PADDR &&
           r.physaddr < SN_PROMDATA_PADDR + SN_PROMDATA_SIZE;
}

/*
 * Runs at a quiescent point. Disabling the overlay exposes the DRAM alias
 * beneath, which backs cached translations at full speed.
 */
static void sn_promdata_leave_private(CPUState *cs, run_on_cpu_data data)
{
    SNPromDataState *s = data.host_ptr;

    BQL_LOCK_GUARD();
    /* A reset since scheduling re-armed the overlay; leave it in place. */
    if (s->leaving_private) {
        memory_region_set_enabled(&s->filter, false);
        if (s->copy_size != 0) {
            memory_region_set_enabled(&s->copy_local, false);
        }
        sn_promdata_unmap(s, "leave-mapped");
        sn_promdata_trace_phase(s, "leave");
    }
}

static MemTxResult sn_promdata_read(void *opaque, hwaddr addr, uint64_t *val,
                                    unsigned size, MemTxAttrs attrs)
{
    SNPromDataState *s = opaque;
    uint8_t bytes[8];
    MemTxResult result;

    if (!s->leaving_private && current_cpu == CPU(s->cpu) &&
        sn_promdata_executing_here(s)) {
        s->leaving_private = true;
        async_safe_run_on_cpu(CPU(s->cpu), sn_promdata_leave_private,
                              RUN_ON_CPU_HOST_PTR(s));
    }

    if (sn_promdata_is_private(s, attrs)) {
        s->private_reads++;
        trace_sn_promdata_private(s->cpu->parent_obj.cpu_index, addr, size,
                                  false, s->cpu->env.active_tc.PC,
                                  s->cpu->env.active_tc.gpr[29]);
        *val = ldn_be_p(memory_region_get_ram_ptr(&s->data) + addr, size);
        return MEMTX_OK;
    }

    result = address_space_read(&address_space_memory,
                                s->node_base + SN_PROMDATA_PADDR + addr,
                                attrs, bytes, size);
    trace_sn_promdata_forward(s->cpu->parent_obj.cpu_index, s->node_base,
                               addr, size, false, attrs.mips_uncached,
                               s->phase_private, s->leaving_private,
                               s->cpu->env.active_tc.PC, result);
    *val = ldn_be_p(bytes, size);
    return result;
}

static MemTxResult sn_promdata_write(void *opaque, hwaddr addr, uint64_t val,
                                     unsigned size, MemTxAttrs attrs)
{
    SNPromDataState *s = opaque;
    uint8_t bytes[8];
    MemTxResult result;

    if (sn_promdata_is_private(s, attrs)) {
        s->private_writes++;
        trace_sn_promdata_private(s->cpu->parent_obj.cpu_index, addr, size,
                                  true, s->cpu->env.active_tc.PC,
                                  s->cpu->env.active_tc.gpr[29]);
        stn_be_p(memory_region_get_ram_ptr(&s->data) + addr, size, val);
        memory_region_set_dirty(&s->data, addr, size);
        return MEMTX_OK;
    }

    stn_be_p(bytes, size, val);
    result = address_space_write(&address_space_memory,
                                 s->node_base + SN_PROMDATA_PADDR + addr,
                                 attrs, bytes, size);
    trace_sn_promdata_forward(s->cpu->parent_obj.cpu_index, s->node_base,
                               addr, size, true, attrs.mips_uncached,
                               s->phase_private, s->leaving_private,
                               s->cpu->env.active_tc.PC, result);
    return result;
}

static const MemoryRegionOps sn_promdata_ops = {
    .read_with_attrs = sn_promdata_read,
    .write_with_attrs = sn_promdata_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
};

static void sn_promdata_reset(void *opaque)
{
    SNPromDataState *s = opaque;

    memset(memory_region_get_ram_ptr(&s->data), 0, SN_PROMDATA_SIZE);
    memory_region_set_dirty(&s->data, 0, SN_PROMDATA_SIZE);

    /* A reboot re-enters the cache-only phase; restore the overlay. */
    s->leaving_private = false;
    s->phase_private = false;
    s->fast = false;
    s->straggler_polls = 0;
    s->private_reads = 0;
    s->private_writes = 0;
    memory_region_set_enabled(&s->filter, true);
    if (s->copy_size != 0) {
        memory_region_set_enabled(&s->copy_local, false);
    }
    memory_region_set_enabled(&s->data, false);
    timer_del(s->poll->timer);
}

void sn_promdata_apply_load(SNPromDataState *s)
{
    if (s->present) {
        memory_region_set_enabled(&s->filter, !s->leaving_private);
        if (s->copy_size != 0) {
            memory_region_set_enabled(&s->copy_local,
                                      s->phase_private &&
                                      !s->leaving_private);
        }
        memory_region_set_enabled(&s->data, s->fast);
        /* Resume watching a slot still mapped or still in the phase. */
        if (s->fast || (s->phase_private && !s->leaving_private)) {
            sn_promdata_poll_cb(s->poll);
        }
    }
}

/*
 * Version 2 carries whether the RAM block is mapped; the block's bytes
 * migrate as RAM. Migration is between identical builds, so an older stream
 * is refused, not translated. The name stays "sn0-flash/ promdata-slice":
 * SN0's wire format predates this file, and sgi-sn1-node has no prior format
 * of its to preserve, so sharing the identifier is harmless.
 */
const VMStateDescription vmstate_sn_promdata_slice = {
    .name = "sn0-flash/promdata-slice",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(leaving_private, SNPromDataState),
        VMSTATE_BOOL(phase_private, SNPromDataState),
        VMSTATE_BOOL(fast, SNPromDataState),
        VMSTATE_END_OF_LIST()
    }
};

void sn_promdata_init(SNPromDataState *s, Object *owner, MIPSCPU *cpu,
                      SNPromDataPoll *poll, bool fast,
                      hwaddr node_base, hwaddr node_memory_size,
                      hwaddr flash_offset, hwaddr flash_size,
                      hwaddr copy_offset, hwaddr copy_size,
                      MemoryRegion *cpu_sysmem, hwaddr window_carrier_base,
                      MemoryRegion *cpu_uncached,
                      MemoryRegion *cpu_uncached_alias,
                      const char *name_prefix)
{
    g_autofree char *data_name = g_strdup_printf("%s.promdata", name_prefix);
    g_autofree char *filter_name =
        g_strdup_printf("%s.promdata-filter", name_prefix);

    s->cpu = cpu;
    s->poll = poll;
    s->node_base = node_base;
    s->node_memory_size = node_memory_size;
    s->flash_offset = flash_offset;
    s->flash_size = flash_size;
    s->copy_offset = copy_offset;
    s->copy_size = copy_size;
    s->present = true;

    memory_region_init_ram(&s->data, owner, data_name, SN_PROMDATA_SIZE,
                           &error_fatal);
    memory_region_init_io(&s->filter, owner, &sn_promdata_ops, s,
                          filter_name, SN_PROMDATA_SIZE);
    /*
     * Firmware traps copy through guest pointers; the shared owner would trip
     * the reentrancy guard.
     */
    s->filter.disable_reentrancy_guard = true;
    qemu_register_reset(sn_promdata_reset, s);

    /* Intercept the PROM's cached carrier window before node RAM. */
    memory_region_add_subregion_overlap(
        cpu_sysmem, window_carrier_base + SN_PROMDATA_PADDR, &s->filter, 2);

    /*
     * Keep the RAM-copy range reader-local until this CPU leaves the private
     * phase (see the struct comment in sn-promdata.h). Disabled at init:
     * sn_promdata_is_private() enables it only once the phase is actually
     * latched, so a plain forwarding access before that point still reaches
     * whatever the claim map above node RAM would give it.
     */
    if (copy_size != 0) {
        g_autofree char *copy_name =
            g_strdup_printf("%s.prom-copy-local", name_prefix);

        memory_region_init_alias(&s->copy_local, owner, copy_name,
                                 get_system_memory(),
                                 node_base + copy_offset, copy_size);
        memory_region_set_enabled(&s->copy_local, false);
        memory_region_add_subregion_overlap(
            cpu_sysmem, window_carrier_base + copy_offset,
            &s->copy_local, 2);
    }

    /*
     * Above the filter, the private block as RAM, mapped only while the
     * phase's stack is resident there, plus the processor's second address
     * space so an uncached reference still reaches the filter alone. data
     * itself is always allocated above (even when fast is false, migrating a
     * fast=false slice into it is harmless) but only added as a subregion,
     * and only ever enabled, in this case.
     */
    if (fast) {
        g_autofree char *uncached_name =
            g_strdup_printf("%s.uncached", name_prefix);
        g_autofree char *uncached_alias_name =
            g_strdup_printf("%s.uncached.memory", name_prefix);
        g_autofree char *filter_uncached_name =
            g_strdup_printf("%s.promdata-filter.uncached", name_prefix);

        s->fast_allowed = true;
        memory_region_set_enabled(&s->data, false);
        memory_region_add_subregion_overlap(
            cpu_sysmem, window_carrier_base + SN_PROMDATA_PADDR,
            &s->data, 3);

        /*
         * The uncached view: everything the cached one resolves, with the
         * filter alone at the window, so an uncached reference forwards to
         * node RAM whatever sits above the filter in the cached view. The
         * processor selects it by the access's cache attribute.
         */
        memory_region_init(cpu_uncached, owner, uncached_name, UINT64_MAX);
        memory_region_init_alias(cpu_uncached_alias, owner,
                                 uncached_alias_name, cpu_sysmem, 0,
                                 UINT64_MAX);
        memory_region_add_subregion_overlap(cpu_uncached, 0,
                                            cpu_uncached_alias, 0);
        memory_region_init_alias(&s->filter_uncached, owner,
                                 filter_uncached_name, &s->filter, 0,
                                 SN_PROMDATA_SIZE);
        memory_region_add_subregion_overlap(
            cpu_uncached, window_carrier_base + SN_PROMDATA_PADDR,
            &s->filter_uncached, 1);
        object_property_set_link(OBJECT(cpu), "uncached-memory",
                                 OBJECT(cpu_uncached), &error_abort);
    }
}
