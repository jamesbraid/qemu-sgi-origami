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
#include "hw/i2c/pcf8584.h"
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

    /*
     * The real PCF8584 I2C-bus controller, mapped over this window's I2C
     * data/control-status addresses at a higher priority than sgi_hub_ops
     * answers directly -- see sgi_hub_realize() and
     * docs/datasheets/pcf8584-elsc.md. Embedded rather than reached through
     * the machine, because the bus it masters (the ELSC, the identity
     * EEPROM) belongs to this node and nothing outside the Hub needs to
     * name it.
     */
    PCF8584State i2c;

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
     * Each processor has a mask for each shared pending bank, so each receives
     * its own pair of level-triggered interrupt lines.
     */
    qemu_irq bank_irq[SGI_HUB_PROCESSORS][SGI_HUB_PENDING_BANKS];

    /*
     * The real-time counter comparator's line into each processor, hardware
     * interrupt 2. Level-triggered on the same principle as the bank lines: it
     * follows the pending bit gated by the enable bit, and drops when the
     * handler acknowledges by clearing pending.
     */
    qemu_irq rt_irq[SGI_HUB_PROCESSORS];

    /* Which node this Hub is, which its network-interface status reports. */
    uint32_t nasid;

    /* Whether the direct inter-node CrayLink has a peer at the far end. */
    bool network_link_up;

    /* How many processor sockets this node has populated. */
    uint32_t cpus;

    /* Physical ELSC DIP switches, expressed as asserted switch bits. */
    uint8_t elsc_dip_switches;

    /* Global QEMU CPU index of this node's slice A. */
    uint32_t cpu_base;

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

    /* The counter's clock control -- bit 16 resets set, see the definition. */
    uint64_t pi_rt_local_ctrl;

    QEMUTimer *rt_timer[SGI_HUB_PROCESSORS];
    SGIHubCounterLine rt_line[SGI_HUB_PROCESSORS];
    uint64_t ni_status;

    /* The network interface's two 64-bit general-purpose scratch registers. */
    uint64_t ni_scratch[2];

    uint64_t io_link_status;
    uint64_t io_widget_control;
    uint64_t io_xtalk_timeout;
    uint64_t io_xtalk_prescale;
    uint32_t io_bwin[SGI_HUB_BWIN_ENTRIES];

    /* The I/O interface's two general-purpose scratch registers. */
    uint64_t io_scratch[2];

    /* Memory/directory performance counter 0. */
    uint64_t md_perf_cnt0;

    /* Memory/directory configuration register. Reset value is not zero. */
    uint64_t md_config;

    /*
     * The node's one-wire bus master. Thirty-four bits wide, so it does not
     * fit a word: the upper timing phase reaches bit 33.
     */
    /*
     * The two error-spool address registers, one per processor. Kept here
     * because the firmware uses processor B's as scratch that outlives a
     * reset: it parks a marker there, triggers the reset, and reads it back to
     * learn which pass it is on. On a single-processor machine that register
     * belongs to a processor which does not exist, which is presumably why it
     * was chosen.
     */
    uint64_t pi_err_spool[SGI_HUB_PROCESSORS];

    uint64_t md_onewire;

    /*
     * The bit slot a write to the one-wire master starts. A slot takes time on
     * a real bus, and completion must not be reported from inside the write:
     * a model that finishes instantly satisfies any test that only checks
     * completion eventually, which is precisely the check the datasheet warns
     * is too weak.
     */
    QEMUTimer *md_onewire_timer;

    /*
     * The junk-bus UART: a real NS16550-compatible serial interface in this
     * unit's own register space that a shipped Origin 200 does not have —
     * SGI fits one only for manufacturing and bring-up. origami can fit one
     * on purpose, off by default, because its presence probe is the one
     * console candidate that needs no other device working: real firmware
     * treats it as the highest-priority candidate and stops probing at the
     * first one that answers, so fitting it unasked would hide the two real
     * candidates behind it. Unfitted, the range it would occupy answers the
     * same unimplemented default as the rest of this register group.
     */
    bool junk_uart_present;
    Chardev *junk_uart_chr;
};

#endif /* HW_MISC_SGI_HUB_H */
