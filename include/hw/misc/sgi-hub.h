/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI SN0 Hub ASIC.
 *
 * One 8 MB register window per node, reached through either half of the
 * crossbar's widget 1 small window: the lower half resolves to whichever node
 * the reading processor is attached to, so firmware can reach its own Hub
 * without knowing its node number, and the upper half names a node explicitly.
 */

#ifndef HW_MISC_SGI_HUB_H
#define HW_MISC_SGI_HUB_H

#include "qemu/units.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SGI_HUB "sgi-hub"
OBJECT_DECLARE_SIMPLE_TYPE(SGIHubState, SGI_HUB)

/*
 * What a comparator's timer is handed when it expires. A timer callback gets
 * one pointer, and both processors' comparators run the same callback, so the
 * pointer has to name which of the two fired rather than just the Hub.
 */
typedef struct SGIHubCounterLine {
    SGIHubState *hub;
    unsigned index;
} SGIHubCounterLine;

#define SGI_HUB_WINDOW_SIZE (8 * MiB)

/*
 * The seven big windows. Each is 512 MB of the node's crosstalk window aimed at
 * a slice of some widget's crosstalk space by one entry of the Hub's I/O
 * interface translation table, and programming those entries is how firmware
 * makes a widget reachable at all.
 *
 * They belong to the Hub rather than to the machine because the table does: a
 * second node's Hub has its own seven windows onto the same crosstalk space,
 * pointed wherever that node's firmware pointed them.
 */
/* Processor sockets a node has, which is what makes every A/B register a pair. */
#define SGI_HUB_PROCESSORS  2

/* The two pending banks, of 64 interrupt numbers each. */
#define SGI_HUB_PENDING_BANKS 2
#define SGI_HUB_BANK_BITS     64

#define SGI_HUB_BWIN_ENTRIES 7
#define SGI_HUB_BWIN_SIZE    (512 * MiB)
#define SGI_HUB_BWIN_TOTAL   (SGI_HUB_BWIN_ENTRIES * SGI_HUB_BWIN_SIZE)

struct SGIHubState {
    SysBusDevice parent_obj;

    MemoryRegion regs;

    /* The seven big windows, as one region for the machine to place. */
    MemoryRegion bwin;
    MemoryRegion bwin_window[SGI_HUB_BWIN_ENTRIES];
    MemoryRegion bwin_empty;

    /*
     * The crosstalk space the windows translate into, and how far apart the
     * widgets sit in it. Both are the machine's to define — a real crossbar
     * routes on a widget number rather than decoding an address bit, so the
     * stride exists only in the model — and the Hub is told rather than
     * assuming, because a wrong stride would decode silently onto the wrong
     * widget.
     */
    MemoryRegion *xtalk;
    uint32_t xtalk_widget_shift;

    /*
     * The two lines this Hub drives into the processor: the first pending bank
     * on hardware interrupt 0 and the second on hardware interrupt 1. Both are
     * level-triggered — the line follows the state of the pending bits and the
     * masks and drops when software clears the last bit it can see — so they
     * are
     * recomputed on every change rather than pulsed.
     */
    qemu_irq bank_irq[SGI_HUB_PENDING_BANKS];

    /*
     * The real-time counter comparator's line into each processor, hardware
     * interrupt 2. Level-triggered on the same principle as the bank lines: it
     * follows the pending bit gated by the enable bit, and drops when the
     * handler acknowledges by clearing pending.
     */
    qemu_irq rt_irq[SGI_HUB_PROCESSORS];

    /* Which node this Hub is, which its network-interface status reports. */
    uint32_t nasid;

    /* How many processor sockets this node has populated. */
    uint32_t cpus;

    /*
     * The manufacturer this Hub claims in its widget identification. A
     * candidate value rather than a constant: the two sources that name it
     * disagree, and nothing on any path we boot reads it, so this is a choice
     * the machine description makes rather than a fact anyone established.
     */
    uint32_t manufacturer;

    uint64_t pi_region_present;
    uint32_t pi_calias_size;
    uint64_t pi_int_pend[2];
    uint64_t pi_int_mask[4];
    uint8_t pi_cpu_enable[2];

    /* Virtual-clock time at which the real-time counter read zero. */
    int64_t pi_rt_origin_ns;

    /*
     * The comparator, per processor. A match sets the pending bit whether or
     * not the enable bit is set; the enable bit only gates pending onto the
     * processor's line, which is what lets a guest poll the bit with the
     * interrupt off.
     */
    uint64_t pi_rt_compare[SGI_HUB_PROCESSORS];
    uint8_t pi_rt_pending[SGI_HUB_PROCESSORS];
    uint8_t pi_rt_enable[SGI_HUB_PROCESSORS];

    QEMUTimer *rt_timer[SGI_HUB_PROCESSORS];
    SGIHubCounterLine rt_line[SGI_HUB_PROCESSORS];
    uint64_t ni_status;
    uint64_t io_link_status;
    uint64_t io_widget_control;
    uint64_t io_xtalk_timeout;
    uint64_t io_xtalk_prescale;
    uint32_t io_bwin[SGI_HUB_BWIN_ENTRIES];

    /* The I/O interface's two general-purpose scratch registers. */
    uint64_t io_scratch[2];

    /* Memory/directory performance counter 0. */
    uint64_t md_perf_cnt0;
};

#endif /* HW_MISC_SGI_HUB_H */
