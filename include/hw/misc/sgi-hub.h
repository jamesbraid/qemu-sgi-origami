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
#include "hw/misc/sgi-diagnostic.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/pcf8584.h"
#include "hw/misc/sgi-elsc.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SGI_HUB "sgi-hub"
OBJECT_DECLARE_SIMPLE_TYPE(SGIHubState, SGI_HUB)

/*
 * The whole -serial assignment in one place: an index collision between two
 * files is invisible at either call site. The first tower's three shipped
 * connectors come first (port A is the firmware console; the AUX port is
 * where firmware redirects after a failed diagnostic), then the optional
 * bring-up port, so the shipped numbering never moves. The second tower
 * adds one connector of its own -- its system controller's AUX port --
 * which takes the next index rather than reusing the first tower's: one
 * physical connector per tower means one backend per tower, and a machine
 * that handed index 2 to both towers' controllers could not launch with
 * that backend provisioned. The machine resolves these indices and hands
 * each Hub its own backend; the Hub never calls serial_hd() itself.
 */
#define SN0_IOC3_SERIAL_A_CHARDEV  0
#define SN0_IOC3_SERIAL_B_CHARDEV  1
#define SN0_ELSC_CHARDEV           2
#define SN0_JUNK_UART_CHARDEV      3
#define SN0_ELSC_NODE1_CHARDEV     4

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
 * The seven big windows: 512 MB each, aimed by the Hub's own translation
 * table entries -- how firmware makes a widget reachable at all.
 */
/* Processor sockets a node has, which is what makes every A/B register a pair. */
#define SGI_HUB_PROCESSORS  2

#define SGI_HUB_META_ROUTES  32
#define SGI_HUB_LOCAL_ROUTES 16

/* The two pending banks, of 64 interrupt numbers each. */
#define SGI_HUB_PENDING_BANKS 2
#define SGI_HUB_BANK_BITS     64

/* The two identical block transfer engines in the Hub's I/O section. */
#define SGI_HUB_BTES 2

#define SGI_HUB_BWIN_ENTRIES 7
#define SGI_HUB_BWIN_SIZE    (512 * MiB)
#define SGI_HUB_BWIN_TOTAL   (SGI_HUB_BWIN_ENTRIES * SGI_HUB_BWIN_SIZE)

struct SGIHubState {
    SysBusDevice parent_obj;

    MemoryRegion regs;
    DeviceDiagnosticSink *diagnostic;

    /*
     * The real PCF8584 I2C-bus controller, mapped over this window's I2C
     * data/control-status addresses at a higher priority than sgi_hub_ops
     * answers directly -- see sgi_hub_realize(); the chip's behavioral
     * contract is in the Origami project tree's controller datasheet
     * (docs/datasheets/pcf8584-elsc.md there, not in this QEMU tree).
     * Embedded rather than reached through
     * the machine, because the bus it masters (the ELSC, the identity
     * EEPROM) belongs to this node and nothing outside the Hub needs to
     * name it.
     */
    PCF8584State i2c;

    /* Diagnostic reference; the I2C bus retains object ownership. */
    SGIElscState *elsc;

    /* The seven big windows, as one region for the machine to place. */
    MemoryRegion bwin;
    MemoryRegion bwin_window[SGI_HUB_BWIN_ENTRIES];
    MemoryRegion bwin_empty;

    /*
     * The crosstalk space and widget stride, both the machine's to define:
     * the stride exists only in the model, and a wrong one decodes silently
     * onto the wrong widget.
     */
    MemoryRegion *xtalk;
    uint32_t xtalk_widget_shift;

    /*
     * The BTEs' node-local memory view, handed in: the programmed node
     * number reads zero on every node, so only the machine can say which
     * memory a node-less address means.
     */
    MemoryRegion *memory;
    AddressSpace bte_as;

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

    /* Node number preprogrammed by synthesized firmware, or zero at reset. */
    uint32_t nasid;

    /* Whether this processor module is the Origin 200 slave chassis. */
    bool slave_module;

    /* Whether the direct inter-node CrayLink has a peer at the far end. */
    bool network_link_up;

    /* The Hub directly connected to this network port, when one is fitted. */
    SGIHubState *network_peer;

    /* How many processor sockets this node has populated. */
    uint32_t cpus;

    /* Physical ELSC DIP switches, expressed as asserted switch bits. */
    uint8_t elsc_dip_switches;

    /* Global QEMU CPU index of this node's slice A. */
    uint32_t cpu_base;

    /*
     * The claimed widget manufacturer: the two sources disagree and nothing
     * we boot reads it, so the machine chooses.
     */
    uint32_t manufacturer;

    uint64_t pi_region_present;
    uint32_t pi_calias_size;
    uint64_t pi_int_pend[2];
    bool pi_int_pend1_primary_credit;
    uint64_t pi_int_mask[4];
    uint8_t pi_cpu_enable[2];

    /*
     * Each local processor's last-read value of each pending bank, for the
     * poll-versus-sample distinction the RR scheduling assist needs.  Not
     * guest-visible state and deliberately not migrated: after a load the
     * first re-read of every bank goes unassisted, nothing more.
     */
    uint64_t pi_int_pend_seen[SGI_HUB_PROCESSORS][SGI_HUB_PENDING_BANKS];
    bool pi_int_pend_seen_valid[SGI_HUB_PROCESSORS][SGI_HUB_PENDING_BANKS];

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

    /* Direct-link vector PIO request and completion registers. */
    uint64_t ni_vector;
    uint64_t ni_vector_data;
    uint64_t ni_vector_status;
    uint64_t ni_return_vector;
    uint64_t ni_vector_read_data;
    uint8_t ni_meta_route[SGI_HUB_META_ROUTES];
    uint8_t ni_local_route[SGI_HUB_LOCAL_ROUTES];

    uint64_t io_link_status;
    uint64_t io_widget_control;
    uint64_t io_xtalk_timeout;
    uint64_t io_xtalk_prescale;
    uint32_t io_bwin[SGI_HUB_BWIN_ENTRIES];

    /*
     * The block transfer engines' six registers each: length/status,
     * source, destination, control/terminate, notification address,
     * interrupt address. The registers are the whole of an engine's state —
     * a launched transfer completes inside the launching store, so there is
     * never an in-flight transfer for migration to carry.
     */
    uint64_t bte_length[SGI_HUB_BTES];
    uint64_t bte_source[SGI_HUB_BTES];
    uint64_t bte_dest[SGI_HUB_BTES];
    uint64_t bte_control[SGI_HUB_BTES];
    uint64_t bte_notify[SGI_HUB_BTES];
    uint64_t bte_interrupt[SGI_HUB_BTES];

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
     * Error-spool addresses. Firmware parks a marker in processor B's
     * across a reset to learn which pass it is on.
     */
    uint64_t pi_err_spool[SGI_HUB_PROCESSORS];
    uint32_t pi_err_mask[SGI_HUB_PROCESSORS];
    uint8_t pi_err_spool_size;
    uint64_t pi_err_status0[SGI_HUB_PROCESSORS];

    uint64_t md_onewire;

    /*
     * A one-wire slot must not complete inside the write that starts it;
     * instant completion passes exactly the too-weak check the datasheet
     * warns about.
     */
    QEMUTimer *md_onewire_timer;

    /*
     * The bring-up UART, off by default: firmware probes it as the
     * highest-priority console candidate, so fitting it unasked hides the
     * real ones.
     */
    bool junk_uart_present;
    Chardev *junk_uart_chr;

    /*
     * The backend behind this node's system-controller AUX connector,
     * resolved and handed in by the machine. NULL is a connector with
     * no cable.
     */
    Chardev *elsc_chr;
};

uint32_t sgi_hub_programmed_nasid(const SGIHubState *s);
void sgi_hub_latch_prom_pi_error(SGIHubState *s);

#endif /* HW_MISC_SGI_HUB_H */
