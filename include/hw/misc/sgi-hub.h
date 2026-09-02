/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI SN0 Hub ASIC: one 8 MB register window per node, reached through
 * either half of the crossbar's widget 1 window. See docs/specs/sgi-hub.rst.
 */

#ifndef HW_MISC_SGI_HUB_H
#define HW_MISC_SGI_HUB_H

#include "qemu/units.h"
#include "hw/core/irq.h"
#include "hw/misc/sgi-diagnostic.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/char/serial-mm.h"
#include "hw/i2c/pcf8584.h"
#include "hw/misc/sgi-elsc.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SGI_HUB "sgi-hub"
OBJECT_DECLARE_SIMPLE_TYPE(SGIHubState, SGI_HUB)

/*
 * The whole -serial assignment in one place: an index collision between two
 * files is invisible at either call site. Every module carries the same set
 * of serial connectors, so the lines are node-major with a fixed stride:
 * node n owns the block [4n, 4n+4). Within a block, port A of the base I/O
 * controller comes first (also the firmware console), then port B, then the
 * system controller's AUX port -- where firmware redirects after a failed
 * diagnostic -- then the bring-up port. One physical connector per module
 * means one backend per module, so each module's ports take their own
 * indices rather than sharing the first module's: a machine that handed
 * index 2 to two modules' controllers could not launch with that backend
 * provisioned. The four constants below are offsets within a node's block;
 * SN0_NODE_SERIAL() turns a (node, offset) pair into the -serial index, and
 * for node 0 the offset already is that index. The bring-up port has no
 * shipped connector and exists on node 0 only. The machine resolves these
 * indices and hands each Hub its own backend; the Hub never calls
 * serial_hd() itself.
 */
#define SN0_SERIAL_LINES_PER_NODE  4

#define SN0_IOC3_SERIAL_A_CHARDEV  0
#define SN0_IOC3_SERIAL_B_CHARDEV  1
#define SN0_ELSC_CHARDEV           2
#define SN0_JUNK_UART_CHARDEV      3

#define SN0_NODE_SERIAL(node, offset) \
    ((node) * SN0_SERIAL_LINES_PER_NODE + (offset))

/* Timer callback argument: which processor's comparator fired. */
typedef struct SGIHubCounterLine {
    SGIHubState *hub;
    unsigned index;
} SGIHubCounterLine;

#define SGI_HUB_WINDOW_SIZE (8 * MiB)

/* Processor sockets a node has, which makes every A/B register a pair. */
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

/* The four uncached spaces a node-qualified physical address selects. */
#define SGI_HUB_CLAIM_SPACES  4

/*
 * The node-number strides a Hub can route: its own board's and one
 * directly wired peer's -- the two-node reduction of the routing-table
 * rule that the address-map datasheet permits for this machine.
 */
#define SGI_HUB_CLAIM_STRIDES 2

struct SGIHubState {
    SysBusDevice parent_obj;

    MemoryRegion regs;
    DeviceDiagnosticSink *diagnostic;

    /*
     * The PCF8584 mapped over this window's I2C addresses (Origami project
     * tree, docs/datasheets/pcf8584-elsc.md); embedded because the bus it
     * masters belongs to this node.
     */
    PCF8584State i2c;

    /* Diagnostic reference; the I2C bus retains object ownership. */
    SGIElscState *elsc;

    /* The seven big windows, as one region for the machine to place. */
    MemoryRegion bwin;
    MemoryRegion bwin_window[SGI_HUB_BWIN_ENTRIES];
    MemoryRegion bwin_empty;

    /* Crosstalk space and widget stride, both the machine's to define. */
    MemoryRegion *xtalk;
    uint32_t xtalk_widget_shift;

    /* The BTEs' node-local memory view; only the machine can supply it. */
    MemoryRegion *memory;
    AddressSpace bte_as;

    /*
     * The claim overlays: per attached processor view, one alias per
     * uncached space per claimable node-number stride, aimed by the
     * programmed node numbers over an error floor for a stride neither
     * Hub claims. The machine wires the per-view containers, the
     * canonical map the aliases resolve into, and the geometry; the Hub
     * owns the regions and switches them from its own status store.
     */
    MemoryRegion *claim_view[SGI_HUB_PROCESSORS];
    MemoryRegion *claim_target;
    uint32_t claim_node_shift;
    uint32_t claim_space_shift;
    uint32_t claim_own_node;
    uint32_t claim_peer_node;
    MemoryRegion claim_alias[SGI_HUB_PROCESSORS][SGI_HUB_CLAIM_SPACES]
                            [SGI_HUB_CLAIM_STRIDES];
    MemoryRegion claim_error[SGI_HUB_PROCESSORS][SGI_HUB_CLAIM_SPACES]
                            [SGI_HUB_CLAIM_STRIDES];

    /*
     * One level-triggered line per processor per pending bank, published
     * as the named GPIO outputs "bank0" and "bank1", indexed by processor.
     */
    qemu_irq bank_irq[SGI_HUB_PENDING_BANKS][SGI_HUB_PROCESSORS];

    /*
     * Per-processor comparator line, hardware interrupt 2, level-triggered;
     * the named GPIO output "rt", indexed by processor.
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

    /* Widget manufacturer: the two sources disagree, so the machine chooses. */
    uint32_t manufacturer;

    uint64_t pi_region_present;
    uint32_t pi_calias_size;
    uint64_t pi_int_pend[2];
    bool pi_int_pend1_primary_credit;
    uint64_t pi_int_mask[4];
    uint8_t pi_cpu_enable[2];

    /*
     * Each processor's last-read value of each pending bank, for the
     * unchanged re-read handoff. Not guest-visible and not migrated.
     */
    uint64_t pi_int_pend_seen[SGI_HUB_PROCESSORS][SGI_HUB_PENDING_BANKS];
    bool pi_int_pend_seen_valid[SGI_HUB_PROCESSORS][SGI_HUB_PENDING_BANKS];

    /* Virtual-clock time at which the real-time counter read zero. */
    int64_t pi_rt_origin_ns;

    /* A match pends whether or not enabled; enable only gates the line. */
    uint64_t pi_rt_compare[SGI_HUB_PROCESSORS];
    uint8_t pi_rt_pending[SGI_HUB_PROCESSORS];
    uint8_t pi_rt_enable[SGI_HUB_PROCESSORS];

    /* The counter's clock control -- bit 16 resets set, see the definition. */
    uint64_t pi_rt_local_ctrl;

    QEMUTimer *rt_timer[SGI_HUB_PROCESSORS];
    SGIHubCounterLine rt_line[SGI_HUB_PROCESSORS];
    uint8_t ni_revision;
    uint64_t ni_status;

    /* The network interface's two 64-bit general-purpose scratch registers. */
    uint64_t ni_scratch[2];

    /* Direct-link vector PIO request and completion registers. */
    uint64_t ni_vector;
    uint64_t ni_vector_data;
    uint64_t ni_vector_status;
    uint64_t ni_return_vector;
    uint64_t ni_vector_read_data;

    /*
     * The LLP port parameters: sixteen bits of link tuning this machine
     * stores and never acts on. The port error register beside it keeps
     * no state at all -- it reads zero by stated modeling choice.
     */
    uint64_t ni_port_parms;

    /*
     * PIO protection: region-permission storage nothing consults. The
     * write-only override beside it keeps no state at all.
     */
    uint64_t ni_io_protect;

    /*
     * Reset permission (one bit, resets permissive, never consulted)
     * and link diagnostics (nineteen bits stored, never acted on).
     */
    uint64_t ni_protection;
    uint64_t ni_diag_parms;
    uint8_t ni_meta_route[SGI_HUB_META_ROUTES];
    uint8_t ni_local_route[SGI_HUB_LOCAL_ROUTES];

    uint64_t io_link_status;
    uint64_t io_widget_control;
    uint64_t io_xtalk_timeout;
    uint64_t io_xtalk_prescale;
    uint32_t io_bwin[SGI_HUB_BWIN_ENTRIES];

    /* The engines' six registers each, the whole of an engine's state. */
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

    /* Error spool addresses; firmware parks a reset marker in processor B's. */
    uint64_t pi_err_spool[SGI_HUB_PROCESSORS];
    uint32_t pi_err_mask[SGI_HUB_PROCESSORS];
    uint8_t pi_err_spool_size;
    uint64_t pi_err_status0[SGI_HUB_PROCESSORS];

    uint64_t md_onewire;

    /* A slot must not complete inside the write that starts it. */
    QEMUTimer *md_onewire_timer;

    /*
     * The responder on that wire: the PIMM manufacturing-record part,
     * DS2505-compatible (the hub one-wire datasheet's settled contract).
     * The serial is a synthetic per-unit property the machine wires; the
     * identity is derived from it at realize. The memory reads serve an
     * unprogrammed part; what the record bytes should say is an open
     * question.
     */
    uint64_t onewire_serial;
    uint64_t onewire_identity;
    uint8_t onewire_phase;
    uint8_t onewire_bit;
    uint8_t onewire_command;
    uint64_t onewire_match;
    bool onewire_level;    /* what the wire sampled in the slot underway */
    uint8_t onewire_args[2];   /* the two address bytes of a read command */
    uint8_t onewire_argn;
    uint8_t onewire_out[40];   /* the staged answer chunk, CRC16 included */
    uint16_t onewire_outbit;
    uint8_t onewire_outlen;
    uint16_t onewire_cursor;   /* next data or status address to serve */
    uint8_t onewire_end[2];    /* a plain read's trailing CRC16 */
    bool onewire_extended_data; /* extended read: serving page data now */

    /* Bring-up UART, off by default: firmware probes it as first console. */
    bool junk_uart_present;
    Chardev *junk_uart_chr;

    /* The port itself, the QOM child "uart"; realized only when fitted. */
    SerialMM junk_uart;

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
