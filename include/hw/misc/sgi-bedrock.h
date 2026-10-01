/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_BEDROCK_H
#define HW_MISC_SGI_BEDROCK_H

#include "hw/core/sysbus.h"
#include "hw/mips/sgi-net-port.h"
#include "hw/mips/sgi/net-cable.h"
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

/* processor-slot-order: fill the fitted slots in ascending slot order. */
#define SGI_BEDROCK_SLOT_ORDER_BY_SLOT UINT32_MAX

/* What a parked processor waits for; zero is no park. */
typedef enum SGIBedrockHoldKind {
    SGI_BEDROCK_HOLD_SCRATCH1 = 1,  /* reset barrier: others' first writes */
    SGI_BEDROCK_HOLD_PRESENCE = 2,  /* a publisher awaits the other's */
    SGI_BEDROCK_HOLD_PARTNER = 3,   /* a partner awaits its publisher */
    SGI_BEDROCK_HOLD_LIVENESS = 4,  /* a poster awaits its partner's post */
} SGIBedrockHoldKind;

struct SGIBedrockState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    /*
     * A zero-based view of the register window, so a vector request from a
     * peer addresses hub registers wherever mmio itself is mapped: a
     * container over two aliases of mmio (see bedrock_init()).
     */
    MemoryRegion hub_window;
    MemoryRegion hub_window_part[2];
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
    /*
     * Pulsed after every node-local reset, for the board to restart the
     * node's processors: resetting this device's own units leaves them
     * running where they were.
     */
    qemu_irq node_reset;
    uint32_t processor_count;
    uint32_t processor_mask;
    uint32_t cpu_enable_initial_mask;
    uint32_t cpu_base;
    /*
     * Which processor occupies each of the four slots, two bits per
     * realized processor: bits 1:0 name the slot of the processor at
     * cpu_base, bits 3:2 the slot of the next one, and so on. Only the
     * board knows this. A processor-mask says which slots are fitted but
     * not which processor the machine put in each, and the two answers
     * differ as soon as both slots of both interfaces are fitted, which
     * is what made a processor in interface 0's second slot report
     * itself as interface 0's first.
     *
     * SGI_BEDROCK_SLOT_ORDER_BY_SLOT keeps the processors in ascending
     * slot order, which is what a caller that has no board behind it
     * (the device's own tests) wants and what every partial population
     * of this product comes to anyway.
     */
    uint32_t processor_slot_order;
    /* Resolved from the order above: the vCPU in each slot, -1 if empty. */
    int slot_cpu_index[SGI_BEDROCK_PROCESSORS];
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
     * Each fitted slot's processor I/O path: a region the node places over
     * that processor's uncached view of the I/O space, which forwards to the
     * processor's own view (cpu_sysmem, "cpu-sysmem<slot>") at io_space_base
     * plus the offset. It is where a graphics store is counted against the
     * slot's graphics credits. A slot with no view gets no path.
     */
    MemoryRegion *cpu_sysmem[SGI_BEDROCK_PROCESSORS];
    uint64_t io_space_base;
    struct SGIBedrockProcessorPath {
        struct SGIBedrockState *bedrock;
        unsigned int slot;
    } pi_path[SGI_BEDROCK_PROCESSORS];
    AddressSpace pi_view_as[SGI_BEDROCK_PROCESSORS];
    MemoryRegion pi_io[SGI_BEDROCK_PROCESSORS];
    bool pi_io_ready[SGI_BEDROCK_PROCESSORS];
    /*
     * The NUMAlink 3 cable on this node's one network port, or NULL. A
     * machine wires it the way it wires a Hub's; the link may also be set
     * after realize, which is how a two-node register test joins two
     * standalone Bedrocks. Nothing is cached from it: link-up and a vector
     * request both consult the cable when they happen.
     */
    SGINetCableState *net_cable;

    /*
     * Cached CPUState resolution per physical processor slot (0..3: A, B,
     * C, D), lazily filled the way sgi-hub.c's sgi_hub_cpu() fills its own
     * cpu[] cache. Neither reset state nor migrated state: derived from the
     * machine's topology, which outlives the device.
     */
    CPUState *hold_cpu[SGI_BEDROCK_PROCESSORS];

    /*
     * The reset-barrier park at LB scratch 1, host-only and discarded
     * across reset and migration -- see sgi_bedrock_hold_reset() and the
     * overview in sgi-bedrock.c. It joins across every populated processor
     * of the node, so its condition is recomputed on every relevant write.
     *
     * hold_scratch1_seen: whether this processor has made its first write
     * to LB scratch 1 since reset. hold_scratch1_withheld(_valid): the
     * value a second-or-later write is holding back until every other
     * present, running processor has made its own first write. That value
     * is guest-visible once it lands, so unlike the rest of the park it
     * travels in a migration stream and lands on load (sgi-bedrock.c,
     * bedrock_post_load()).
     */
    bool hold_scratch1_seen[SGI_BEDROCK_PROCESSORS];
    bool hold_scratch1_withheld_valid[SGI_BEDROCK_PROCESSORS];
    uint64_t hold_scratch1_withheld[SGI_BEDROCK_PROCESSORS];
    /*
     * The order the withheld values were written in: an access from
     * outside the node lands them all, oldest first, before its own
     * (sgi_bedrock_scratch1_external in sgi-bedrock.c).
     */
    uint64_t hold_scratch1_withheld_seq[SGI_BEDROCK_PROCESSORS];
    uint64_t hold_scratch1_seq;

    /*
     * The presence-publication join, host-only like the barrier above
     * and reset with them. presence_seen: whether a processor of that
     * interface has written II scratch 0 since reset -- the stock PROM's
     * publication of the interface's fitted processors. presence_abandoned:
     * a park on the join timed out, so the join is over for this reset
     * whether or not a publication ever arrives. See the presence comment
     * in sgi-bedrock.c.
     *
     * The partner park's state, the same lifetime. hold_scratch1_released:
     * the reset-edge barrier is over, its release (the master's second
     * scratch-1 write) having landed. hold_scratch1_joined: the processor has
     * made a scratch-1 write after that, so it is past the join; the master's
     * release itself does not count. presence_posted: its first write to its
     * own real-time compare register after that, the post the PROM's pair
     * liveness handshake exchanges. presence_released: the interface's
     * publisher has released the hub lock after publishing.
     */
    bool presence_seen[SGI_BEDROCK_PROCESSORS / SGI_BEDROCK_PI_PROCESSORS];
    bool presence_abandoned;
    bool hold_scratch1_released;
    bool hold_scratch1_joined[SGI_BEDROCK_PROCESSORS];
    bool presence_posted[SGI_BEDROCK_PROCESSORS];
    bool presence_released[SGI_BEDROCK_PROCESSORS / SGI_BEDROCK_PI_PROCESSORS];

    /*
     * Every park halts the parked processor rather than block a host
     * thread (bedrock_park_start in sgi-bedrock.c), so they work the same
     * under every TCG scheduling mode. park_kind is the park each slot is
     * in, zero for none; park_deadline its bound on the virtual clock, and
     * park_timer fires at the earliest bound. park_mask is the parked slots
     * as a migration stream carries them: a halted processor travels with
     * its CPU state, so the destination has to know to wake it.
     */
    QEMUTimer *park_timer;
    uint8_t park_kind[SGI_BEDROCK_PROCESSORS];
    int64_t park_start[SGI_BEDROCK_PROCESSORS];
    int64_t park_deadline[SGI_BEDROCK_PROCESSORS];
    uint8_t park_mask;
};

bool sgi_bedrock_hub_registers_permitted(SGIBedrockState *s);

/* The node number this Bedrock decodes by: the local block's latched one. */
uint8_t sgi_bedrock_node_number(SGIBedrockState *s);

/*
 * The processor I/O path of physical slot 'slot' (0 to 3), for the node to
 * place over that processor's uncached view of the I/O space, or NULL when
 * the slot was given no processor view.
 */
MemoryRegion *sgi_bedrock_processor_io(SGIBedrockState *s, unsigned int slot);

/* Whether this node's port has a peer at the other end of an up cable. */
bool sgi_bedrock_link_up(SGIBedrockState *s);

/*
 * A warm reset sent over this node's link (network-interface port reset):
 * the Bedrock at the other end of an up cable resets if its reset enable
 * permits it. Nothing happens with no link.
 */
void sgi_bedrock_port_reset_send(SGIBedrockState *s);

/*
 * Send a vector request out of this node's network port. The device at the
 * other end of the cable consumes packet->vector; SGI_NET_PACKET_NONE when no
 * device answers.
 */
SGINetPacketResult sgi_bedrock_vector_send(SGIBedrockState *s,
                                           SGINetPacket *packet);

/*
 * The reset-barrier park's entry point, called from lb_write() for LB
 * scratch 1. It applies or withholds the write itself; the caller does not
 * also store it.
 */
void sgi_bedrock_hold_reset(SGIBedrockState *s);
void sgi_bedrock_hold_scratch1_write(SGIBedrockState *s, uint64_t value);

/*
 * Called from lb_read() before it returns LB scratch 1: a read from outside
 * the node sees every value the node's own processors have written, withheld
 * or not.
 */
void sgi_bedrock_scratch1_read(SGIBedrockState *s);

/*
 * The memory-directory unit latched a correctable read error: raise the
 * memory-correctable bit, pending bank 1 bit 61, on both processor
 * interfaces.
 */
void sgi_bedrock_memory_correctable_error(SGIBedrockState *s);
/*
 * The memory-directory unit latched an uncorrectable error: raise the
 * memory-uncorrectable bit, error-interrupt pending bit 24, on both
 * processor interfaces, whose error lines follow it through each
 * processor's error mask.
 */
void sgi_bedrock_memory_uncorrectable_error(SGIBedrockState *s);

/*
 * Called from lb_write() when a processor clears LB scratch 3, the hub lock
 * the PROM publishes presence under: records a publisher's release and parks
 * it for the presence join. No value the guest reads changes.
 */
void sgi_bedrock_scratch3_released(SGIBedrockState *s);

/*
 * The presence-publication join: called from
 * ii_write() after a processor's write to II scratch 0 has landed. It only
 * records which interface has published and wakes processors parked on the
 * join; no value the guest reads changes.
 */
void sgi_bedrock_presence_published(SGIBedrockState *s);

/*
 * The presence join's liveness and partner parks:
 * called from pi_write() after a real-time compare write of processor @slot
 * of @interface has landed, and from pi_read() after a real-time counter
 * read has been sampled. They record the pair's liveness posts, park the
 * first of a pair to post until its partner posts, and park an interface's
 * second processor at its timed wait until its publisher has published; no
 * value the guest reads changes.
 */
void sgi_bedrock_rt_compare_written(SGIBedrockState *s,
                                    unsigned int interface,
                                    unsigned int slot);
void sgi_bedrock_rt_counter_read(SGIBedrockState *s);

#endif
