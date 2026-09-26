/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The per-CPU cache-as-RAM private window, shared by sgi-sn0-node and
 * sgi-sn1-node. See sn-promdata.h and
 * docs/datasheets/firmware-structures.md, "The PROM cache-as-RAM phase and
 * its boot-status bit", for the contract this implements.
 */

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
 * Not a hardware register: the IP27 PROM's own software convention of
 * boot-status flags in FPR 0. Bit 1 marks the cache-as-RAM phase. Read
 * from the IRIX 6.5.7 PROM and stated in our own words; see the datasheet.
 */
#define SN_PROM_BOOT_STATUS_CACHE_AS_RAM (1U << 1)

/*
 * How often a processor whose window is mapped as RAM samples its stack
 * pointer for the exit's stack switch. A late sample only leaves the
 * block mapped that much longer, in a span nothing fetches from the
 * window; an early one only restores the filter sooner.
 */
#define SN_PROMDATA_POLL_NS (10 * SCALE_MS)

/*
 * How many extra poll periods a still-mapped slot gets once a sibling in
 * its node has already released, before this poll gives up on it too.
 * Every measured boot's own residency is a small fraction of this many
 * periods (see the struct comment on straggler_polls), and QEMU_CLOCK_VIRTUAL
 * itself stretches under host load rather than this count needing to, so a
 * generous bound stays generous under load rather than firing early. This
 * is what distinguishes a slower-but-live sibling, which reaches its own
 * exit within a handful of periods, from a genuinely disabled processor
 * that never does -- the boot's own PROM console reports that case as "CPU
 * failed early init" and it is not this window's job to keep it mapped
 * forever.
 */
#define SN_PROMDATA_STRAGGLER_LIMIT 6000

/*
 * A virtual address of this processor's, translated through the MMU
 * rather than pattern-matched, because the phase does not stay on the
 * unmapped aliases: besides the compatibility segments and XKPHYS, a
 * guarded probe can run from a TLB-mapped alias of the flash, and under
 * ERL the low region is an unmapped alias of physical memory. A
 * node-qualified physical address reduces to its node-local offset.
 * False when the address does not translate.
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
 * The phase's execution homes, from the full 64-bit PC. Comparing
 * physically also admits the cached alias of a RAM copy of the PROM;
 * harmless, because the phase bit already over-covers the residency.
 * Post-exit PROM code does touch the window: on the eight-processor
 * Origin 2000 a node's first processor makes 28,673 more doubleword
 * accesses after its stack switch, a memory-test pass, and the held
 * latch serves them from the private block until the OS handoff.
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
 * Whether this CPU has withdrawn both of the phase's own recognition
 * inputs at once -- the boot-status bit and the PC being in one of the
 * phase's execution homes -- the same handoff test sn_promdata_is_private()
 * applies on a filter access. Test the bit first: the PC translation
 * only has to run on the phase's way out, not on every private access.
 * Shared with sn_promdata_check_stack() so a CPU whose later traffic
 * never dispatches through the filter's read/write handlers again still
 * gets this test applied, instead of only a CPU whose traffic does.
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
 * The node's poll: each mapped processor samples itself; rearm while any
 * still needs it. A node's first processor does the bulk of the resident
 * work -- 1.92 to 1.96 million private accesses against 95 to 131
 * thousand for its second, measured on PROM 6.156 -- and is usually the
 * last of the two to release, but not always: a slower sibling that
 * releases after the other is still a live processor finishing the same
 * phase, not the "CPU failed early init" case the eight-processor boot's
 * own PROM console reports for a genuinely disabled one. Coupling a
 * slot's exemption to a specific sibling's index conflates the two --
 * whichever slot happens to release first was treated as proof the other
 * never would, which stranded a still-mapped, still-booting sibling's
 * window open for the rest of the run once it lost the race by even one
 * poll period. Give every slot that outlasts a released sibling a bounded
 * number of further chances (SN_PROMDATA_STRAGGLER_LIMIT) instead: enough
 * for a live straggler to reach its own exit, not so many that a truly
 * disabled processor keeps the node's 100 Hz timer running indefinitely.
 *
 * A slot that has released its fast mapping but not yet left the phase
 * stays watched too, unbounded by the straggler count above: the fast
 * path's own accesses never reach sn_promdata_is_private() in the first
 * place (data answers them directly, above the filter), so a processor
 * that never again touches the small filter window after its own stack
 * switch -- every processor but the one doing the boot's node-ID
 * renumbering, on a measured eight-processor boot -- would otherwise
 * never run that reactive detector again and would keep phase_private,
 * and this window's prom-copy-local alias, latched for the rest of its
 * run. sn_promdata_check_stack() applies the same handoff test here.
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
         * A processor that is not running yet has the previous sample
         * still queued; one is enough, however long it stays stopped.
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
     * Every node polls, fast or not: a processor that never touches the
     * filter window after the OS handoff never runs the per-access
     * handoff test, and would keep the phase -- and with it the
     * reader-local routing of the window and of the PROM's RAM-copy range
     * -- for the rest of its run. On a machine of several nodes that
     * routing sends another node's memory at those addresses to the
     * reader's own. The fast path only decides whether the private block
     * is mapped as RAM.
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
 * The phase recognised. From here the private RAM block itself answers
 * this processor's cached window accesses: mapped above the filter, a
 * translated load or store resolves to a host pointer and takes neither
 * the dispatch path nor the big lock. The filter stays beneath for what
 * RAM cannot decide -- the uncached view, which the processor's second
 * address space routes to the filter alone, and the release at OS
 * handoff, which needs an access to evaluate. The poll puts the filter
 * back at the exit's stack switch, before that release can matter. While
 * the block is mapped a cached instruction fetch from the window executes
 * the block rather than ending the phase; the PROM never fetches there,
 * and the OS runs long after the switch.
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
 * The residency the block served is over: the filter answers again
 * from the same block, evaluating the release per access as before.
 * One-way until reset.
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
 * On the processor's own thread: its stack pointer outside the window
 * is the exit's stack switch, the PROM having moved to its real
 * per-processor stack (the cache-as-RAM exit section of
 * docs/datasheets/firmware-structures.md; confirmed on PROM 6.156 by
 * every processor of the eight-processor boots this was measured on).
 */
static void sn_promdata_check_stack(CPUState *cs, run_on_cpu_data data)
{
    SNPromDataState *s = data.host_ptr;

    BQL_LOCK_GUARD();
    if (s->fast && !sn_promdata_sp_in_window(s)) {
        sn_promdata_unmap(s, "stack-switch");
    }

    /*
     * The stack switch above only ever drops the fast mapping; it says
     * nothing about whether the phase itself is over. Apply the same
     * handoff test a filter access would, here, on the poll's own
     * schedule: a CPU that has released its fast mapping may simply
     * never touch the filter window again for the rest of its run, and
     * sn_promdata_is_private() only ever runs when something does.
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
 * The window holds this CPU's cache-resident PROM stack for the whole
 * cache-as-RAM phase, entered when boot-status bit 1 is set with the PC
 * in one of the phase's execution homes. Once recognised, the phase holds
 * for this CPU until reset or OS handoff: on real hardware the window is
 * resident cache lines, and neither clearing the bookkeeping bit nor
 * moving the PC is a cache operation, so no single later state change may
 * reroute an access. Handoff is both recognition inputs withdrawn at once
 * -- the bit clear and the PC outside every home -- a state first reached
 * with the OS running. Uncached accesses always forward -- the phase's
 * exit copies the persistent structures out to real RAM through exactly
 * that path. sn_promdata_check_stack() applies the same handoff test
 * from the node's poll, not only from a filter access.
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
 * Only the operating system can execute from inside the window -- the
 * PROM never does -- so execution here ends the private phase for good.
 * The PC is translated through the MMU because kernels run mapped.
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
 * Runs at a quiescent point. Disabling the overlay exposes the DRAM
 * alias beneath, which backs cached translations at full speed.
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
 * migrate as RAM. Migration is between identical builds, so an older
 * stream is refused, not translated. The name stays "sn0-flash/
 * promdata-slice": SN0's wire format predates this file, and sgi-sn1-node
 * has no prior format of its own to preserve, so sharing the identifier
 * is harmless.
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
     * Firmware traps copy through guest pointers; the shared owner would
     * trip the reentrancy guard.
     */
    s->filter.disable_reentrancy_guard = true;
    qemu_register_reset(sn_promdata_reset, s);

    /* Intercept the PROM's cached carrier window before node RAM. */
    memory_region_add_subregion_overlap(
        cpu_sysmem, window_carrier_base + SN_PROMDATA_PADDR, &s->filter, 2);

    /*
     * Keep the RAM-copy range reader-local until this CPU leaves the
     * private phase (see the struct comment in sn-promdata.h). Disabled
     * at init: sn_promdata_is_private() enables it only once the phase is
     * actually latched, so a plain forwarding access before that point
     * still reaches whatever the claim map above node RAM would give it.
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
     * phase's stack is resident there, plus the processor's second
     * address space so an uncached reference still reaches the filter
     * alone. data itself is always allocated above (even when fast is
     * false, migrating a fast=false slice into it is harmless) but only
     * added as a subregion, and only ever enabled, in this case.
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
         * The uncached view: everything the cached one resolves, with
         * the filter alone at the window, so an uncached reference
         * forwards to node RAM whatever sits above the filter in the
         * cached view. The processor selects it by the access's cache
         * attribute.
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
