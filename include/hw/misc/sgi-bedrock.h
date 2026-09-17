/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_H
#define HW_MISC_SGI_BEDROCK_H

#include "hw/core/sysbus.h"
#include "hw/misc/sgi-bedrock-lb.h"
#include "hw/misc/sgi-bedrock-io.h"
#include "hw/misc/sgi-bedrock-md.h"
#include "hw/misc/sgi-bedrock-ni.h"
#include "hw/misc/sgi-bedrock-pi.h"
#include "hw/misc/sgi-bedrock-xb.h"

#define TYPE_SGI_BEDROCK "sgi-bedrock"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockState, SGI_BEDROCK)

#define SGI_BEDROCK_SIZE 0x7e0048

/* The four physical processor slots a node's two PIs together address. */
#define SGI_BEDROCK_PROCESSORS 4

/* What a held processor waits for. Mirrors sgi-hub.c's SGIHubRTHoldKind. */
typedef enum SGIBedrockHoldKind {
    SGI_BEDROCK_HOLD_SCRATCH1 = 1,       /* H1: LB scratch 1's withheld write */
    SGI_BEDROCK_HOLD_SCRATCH3_HELD = 2,  /* H3: the hub lock a peer holds */
    SGI_BEDROCK_HOLD_PERF_YIELD = 3,     /* H3: a monopoliser cedes a quantum */
} SGIBedrockHoldKind;

struct SGIBedrockState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    /*
     * A zero-based view of the register window for the vector engine, so a
     * routed read addresses hub registers wherever mmio itself is mapped.
     */
    MemoryRegion hub_window;
    MemoryRegion hub_special_registers;
    MemoryRegion remote_registers;
    /*
     * The L1 UART's own eight registers, zero-based and sized to exactly
     * them (SGI_L1_SIZE), so a caller can place this one slot at offset
     * 0x80 in either register-window form without claiming anything else
     * in the window. bedrock_remote_ops still owns the remote form's own
     * decode at that offset directly; this region is what lets the local
     * form reach the same UART (see sn1-node.c's sn1_node_create_bedrock).
     */
    MemoryRegion l1_uart_registers;
    MemoryRegion io_registers;
    SGIBedrockPIState pi[2];
    SGIBedrockIOState io;
    SGIBedrockMDState md;
    SGIBedrockLBState lb;
    SGIBedrockNIState ni;
    SGIBedrockXBState xb;
    qemu_irq hardware_irq[2 * SGI_BEDROCK_PI_PROCESSORS *
                           SGI_BEDROCK_PI_HW_IRQS];
    qemu_irq nmi[2 * SGI_BEDROCK_PI_PROCESSORS];
    qemu_irq cpu_enable[2 * SGI_BEDROCK_PI_PROCESSORS];
    uint32_t processor_count;
    uint32_t processor_mask;
    uint32_t cpu_enable_initial_mask;
    uint32_t cpu_base;
    uint8_t revision;
    uint8_t local_region;
    uint8_t installed_memory_bank_mask;
    uint16_t installed_memory_bank_mib[8];
    uint16_t inbound_widget_mask;
    uint8_t inbound_device;
    uint64_t device_mask_reset;
    MemoryRegion *dma_mr;
    MemoryRegion *node_mr;
    MemoryRegion *l1_uart_mr;
    AddressSpace dma_as;
    bool dma_as_initialized;
    AddressSpace l1_uart_as;
    bool l1_uart_as_initialized;
    uint8_t initiator_region;
    MemoryRegion *widget_mr[16];

    /*
     * Cached CPUState resolution per physical processor slot (0..3: A, B,
     * C, D), lazily filled the way sgi-hub.c's sgi_hub_cpu() fills its own
     * cpu[] cache. Neither reset state nor migrated state: derived from the
     * machine's topology, which outlives the device.
     */
    CPUState *hold_cpu[SGI_BEDROCK_PROCESSORS];

    /*
     * The multi-threaded rendezvous hold at LB scratch 1 (H1), host-only
     * and discarded across reset and migration -- see sgi_bedrock_hold_reset()
     * and docs/known-issues.md. It mirrors sgi-hub.c's RT-mailbox hold
     * (sgi_hub_rt_hold and neighbors): queued work on the held vCPU,
     * waiting on its own halt condition, one hold per processor at a time.
     * Unlike the Hub's fixed pairwise partner, this hold joins across
     * every populated processor of the node, so a hold's wait condition is
     * recomputed fresh on every wake rather than answered by a single
     * tagged release.
     *
     * hold_scratch1_seen: whether this processor has made its first write
     * to LB scratch 1 since reset. hold_scratch1_withheld(_valid): the
     * value a second-or-later write is holding back until every other
     * present, running processor has made its own first write.
     *
     * hold_id/hold_kind: the active hold on each slot, zero for none, and
     * which register it is waiting on. hold_seq hands out ascending ids
     * the way sgi-hub.c's pi_rt_hold_seq does.
     *
     * perf_last_slot is the processor that last accessed the software lock
     * word (MD performance counter 0), or -1, and perf_run is its unbroken
     * run of accesses since the other processor last touched the word -- a
     * long run being one processor monopolising the lock while the other is
     * starved elsewhere, the way the copy-phase stall was traced. See the H3
     * hold's own comment in sgi-bedrock.c and docs/known-issues.md.
     */
    bool hold_scratch1_seen[SGI_BEDROCK_PROCESSORS];
    bool hold_scratch1_withheld_valid[SGI_BEDROCK_PROCESSORS];
    uint64_t hold_scratch1_withheld[SGI_BEDROCK_PROCESSORS];
    int perf_last_slot;
    uint64_t perf_run;
    uint64_t hold_id[SGI_BEDROCK_PROCESSORS];
    uint8_t hold_kind[SGI_BEDROCK_PROCESSORS];
    uint64_t hold_seq;
};

bool sgi_bedrock_hub_registers_permitted(SGIBedrockState *s);

/*
 * The rendezvous hold's entry point, called from lb_write() for LB scratch 1
 * (docs/known-issues.md and docs/datasheets/bedrock-local-block.md). It
 * applies or withholds the write itself; the caller does not also store it.
 */
void sgi_bedrock_hold_reset(SGIBedrockState *s);
void sgi_bedrock_hold_scratch1_write(SGIBedrockState *s, uint64_t value);

/*
 * The second rendezvous's holds (H3), docs/known-issues.md and
 * docs/datasheets/bedrock-local-block.md. Each is called after the register
 * value has already been produced and only decides whether to park the
 * reader; no value the guest reads changes.
 *
 * sgi_bedrock_perf_counter_access(): called from md_read() and md_write() for
 * the software lock word (MD performance counter 0). A processor that has
 * accessed the word many times over while the other has not touched it is
 * monopolising the lock with the other starved, so it cedes a bounded
 * quantum, freeing its host core for the starved processor to run.
 *
 * sgi_bedrock_scratch3_acquire(): called from lb_read() with the value LB
 * scratch 3's hub-lock twin returned; nonzero means a peer holds it, so the
 * reader is parked rather than left polling. sgi_bedrock_scratch3_released()
 * is called from lb_write() when a processor clears the lock, to wake it.
 */
void sgi_bedrock_perf_counter_access(SGIBedrockState *s);
void sgi_bedrock_scratch3_acquire(SGIBedrockState *s, uint64_t observed);
void sgi_bedrock_scratch3_released(SGIBedrockState *s);

#endif
