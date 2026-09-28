/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The per-CPU cache-as-RAM private window every Scalable Node PROM uses
 * before DRAM is initialised: physical 0x10_0000, 32 KiB, split by processor
 * within the node. Both sgi-sn0-node and sgi-sn1-node install it through the
 * functions here so the rule has one implementation instead of a second copy
 * per generation.
 *
 * Where a node's realize enables the fast path (promdata-fast, on by default
 * for sgi-sn0-node and off for sgi-sn1-node), the private block also answers
 * each processor's cached accesses directly as RAM once its phase latches,
 * instead of every access dispatching through the filter's MMIO path under
 * the big lock; see the struct comments below and sn-promdata.c for the
 * mechanism.
 */

#ifndef HW_MIPS_SGI_SN_PROMDATA_H
#define HW_MIPS_SGI_SN_PROMDATA_H

#include "qemu/units.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "system/memory.h"
#include "migration/vmstate.h"

#define SN_PROMDATA_PADDR 0x100000
#define SN_PROMDATA_SIZE  (32 * KiB)

typedef struct SNPromDataState SNPromDataState;

/*
 * One node's shared poll. While any of the node's processors has its private
 * block mapped as RAM, or is still in the private phase, the timer samples
 * that processor every SN_PROMDATA_POLL_NS of guest time, on its thread,
 * until none remain watched (sn_promdata_poll_cb() in sn-promdata.c). Each
 * sample checks the exit's stack switch and, separately, the phase's handoff
 * test directly. The fast path's accesses never dispatch through the filter,
 * so a processor need not touch the filter window again for the poll to
 * notice it has left. slots/count name the node's promdata[] array, so the
 * callback can walk every slice without the node's type. peer/peer_writes let
 * sn_promdata_trace_phase() report a generation-specific write counter
 * alongside the filter's counts: SN0's ECC and directory back doors via
 * sn0.c's callback, or NULL when a generation tracks no such counter
 * (sgi-sn1-node does not supply one).
 *
 * A processor that stays in the phase without its block mapped and never
 * hands off stays watched, including a secondary left at the PROM monitor or
 * a processor on a node without the fast path that the PROM disables. The
 * timer samples it at 100 Hz of guest time for as long as it stays there. The
 * straggler limit (straggler_polls below) bounds only a slot that stays
 * mapped.
 */
typedef struct SNPromDataPoll {
    QEMUTimer *timer;
    SNPromDataState *slots;
    unsigned int count;
    uint64_t (*peer_writes)(const void *peer);
    const void *peer;
} SNPromDataPoll;

/*
 * One CPU's window. node_base and node_memory_size let the execution-range
 * test reduce a translated physical address to its node-local offset;
 * flash_offset/flash_size are the node-local reset-vector execution home the
 * PROM's boot-status bit is paired with. copy_offset/copy_size add a second
 * execution home. SN0's PROM jumps to a RAM copy of its text mid-phase by
 * ASID re-point rather than a PC change, so the range test must recognise
 * that copy too; a generation with no confirmed copy address passes copy_size
 * zero and gets the single-home test, with no copy_local region at all.
 *
 * copy_local is the routing side of that same RAM copy: while a CPU is in the
 * private phase, the copy_offset/copy_size range must keep resolving through
 * this reader's local memory rather than whatever a NASID claim map would
 * otherwise route it to (a claim map can stop naming this reader's NASID once
 * renumbering moves on, which is not a promise the PROM's self-copy depends
 * on). Installed only when copy_size is nonzero.
 *
 * data is the private block itself, always allocated but mapped above the
 * filter. fast_allowed is set only where the owning node's realize
 * passed fast to sn_promdata_init(). poll names that node's shared
 * SNPromDataPoll, armed at every latch, fast or not, and used to report the
 * peer write counter; it is set whenever this slice is present.
 * filter_uncached is a second window, installed into the caller's
 * cpu_uncached
 * container only when fast, that keeps an uncached reference routed to the
 * filter alone even while data answers the cached one; see
 * sn_promdata_init()'s cpu_uncached parameter.
 */
struct SNPromDataState {
    MIPSCPU *cpu;
    SNPromDataPoll *poll;
    hwaddr node_base;
    hwaddr node_memory_size;
    hwaddr flash_offset;
    hwaddr flash_size;
    hwaddr copy_offset;
    hwaddr copy_size;
    MemoryRegion data;
    MemoryRegion filter;
    MemoryRegion filter_uncached;
    MemoryRegion copy_local;
    bool present;           /* the filter was installed (real PROM boot) */
    bool fast_allowed;      /* data may serve cached accesses as RAM */
    bool leaving_private;   /* the overlay has been (or is being) disabled */
    bool phase_private;     /* cache-as-RAM phase latched for this CPU */
    bool fast;              /* data is mapped above the filter */
    /*
     * Consecutive poll periods this slot has stayed mapped after a sibling in
     * the same node released first. Reset to 0 while no sibling has released
     * yet; see sn_promdata_poll_cb() in sn-promdata.c.
     */
    uint32_t straggler_polls;
    /* Through the filter, for tracing only. */
    uint64_t private_reads;
    uint64_t private_writes;
};

/*
 * Initialises one node's shared poll. slots/count name the node's promdata[]
 * array (so the callback can reach every slice without the caller's node
 * type); peer_writes/peer are an optional extra write counter
 * sn_promdata_trace_phase() adds to its trace line, or NULL where a
 * generation
 * has none. Always allocates the timer: every node polls for the OS handoff,
 * whether or not it enables the fast path.
 */
void sn_promdata_poll_init(SNPromDataPoll *poll, SNPromDataState *slots,
                           unsigned int count,
                           uint64_t (*peer_writes)(const void *peer),
                           const void *peer);
void sn_promdata_poll_free(SNPromDataPoll *poll);

/*
 * Installs the window as a subregion of cpu_sysmem, overlaid above the
 * container's existing content at window_carrier_base + SN_PROMDATA_PADDR,
 * and registers its reset handler. poll is the node's shared poll from
 * sn_promdata_poll_init(), required whenever the node installs the filter at
 * all. When copy_size is nonzero this also installs the copy_local routing
 * overlay described above, at window_carrier_base + copy_offset.
 *
 * When fast is true, this also maps the private block itself above the filter
 * and gives the processor a second, uncached address space: cpu_uncached and
 * cpu_uncached_alias are the caller's per-CPU storage for that space
 * (mirroring cpu_sysmem's construction) and are only touched in this case;
 * name_prefix (for example "sn0.node0.cpu0") names every region
 * sn_promdata_init() creates, suffixed the way each generation's calling code
 * used to spell them ("<prefix>.promdata", "<prefix>.promdata-filter",
 * "<prefix>.prom-copy-local", "<prefix>.uncached", "<prefix>.uncached.memory"
 * and "<prefix>.promdata-filter.uncached").
 *
 * owner is the node device the window's regions and reset handler are
 * attached to. The caller still owns *s's storage and the array-level
 * VMStateDescription that migrates it. vmstate_sn_promdata_slice below is
 * only the one-slice subsection.
 */
void sn_promdata_init(SNPromDataState *s, Object *owner, MIPSCPU *cpu,
                      SNPromDataPoll *poll, bool fast,
                      hwaddr node_base, hwaddr node_memory_size,
                      hwaddr flash_offset, hwaddr flash_size,
                      hwaddr copy_offset, hwaddr copy_size,
                      MemoryRegion *cpu_sysmem, hwaddr window_carrier_base,
                      MemoryRegion *cpu_uncached,
                      MemoryRegion *cpu_uncached_alias,
                      const char *name_prefix);

/*
 * Reapplies how far a post_load slice's cache-only phase had got, and resumes
 * the node's poll for a slice still mapped or still in the phase. A stream
 * carrying fast set into a destination started without promdata-fast is
 * therefore polled and released at the stack switch; before every node
 * polled,
 * that destination had no timer and left the block mapped.
 */
void sn_promdata_apply_load(SNPromDataState *s);

extern const VMStateDescription vmstate_sn_promdata_slice;

#endif /* HW_MIPS_SGI_SN_PROMDATA_H */
