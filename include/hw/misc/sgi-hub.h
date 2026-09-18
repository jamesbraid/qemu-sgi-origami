/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI SN0 Hub ASIC: one 8 MB register window per node, reached through
 * either half of the crossbar's widget 1 window. See docs/specs/sgi-hub.rst.
 */

#ifndef HW_MISC_SGI_HUB_H
#define HW_MISC_SGI_HUB_H

#include "qemu/units.h"
#include "hw/1wire/ds2502.h"
#include "hw/1wire/onewire.h"
#include "hw/core/clock.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/char/serial-mm.h"
#include "hw/i2c/pcf8584.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/mips/sn0-fabric.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SGI_HUB "sgi-hub"
OBJECT_DECLARE_SIMPLE_TYPE(SGIHubState, SGI_HUB)

typedef struct SGIHubModuleIdentity {
    uint8_t rom[8];
    uint8_t page[2][DS2502_PAGE_SIZE];
} SGIHubModuleIdentity;

/*
 * The whole -serial assignment in one place: an index collision between two
 * files is invisible at either call site. Every module carries the same set
 * of serial connectors, so the lines are node-major with a fixed stride:
 * node n owns the block [6n, 6n+6). Within a block, port A of the base I/O
 * controller comes first (also the firmware console), then port B, then the
 * system controller's AUX port -- where firmware redirects after a failed
 * diagnostic -- then the bring-up port, then the two base I/O transmit
 * records. One physical connector per module means one backend per module,
 * so each module's ports take their own indices rather than sharing the
 * first module's: a machine that handed index 2 to two modules' controllers
 * could not launch with that backend provisioned. The six constants below
 * are offsets within a node's block; SN0_NODE_SERIAL() turns a (node,
 * offset) pair into the -serial index, and for node 0 the offset already is
 * that index. The bring-up port has no shipped connector and exists on node
 * 0 only. The two record lines have no connector either: they are write-only
 * sinks taking what each serial port transmitted, so a console consumer that
 * stops reading cannot starve the record. The machine resolves these indices
 * and hands each Hub its own backend; the Hub never calls serial_hd()
 * itself.
 */
#define SN0_SERIAL_LINES_PER_NODE  6

#define SN0_IOC3_SERIAL_A_CHARDEV  0
#define SN0_IOC3_SERIAL_B_CHARDEV  1
#define SN0_ELSC_CHARDEV           2
#define SN0_JUNK_UART_CHARDEV      3
#define SN0_IOC3_RECORD_A_CHARDEV  4
#define SN0_IOC3_RECORD_B_CHARDEV  5

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

#define SGI_HUB_POPULATION_I2C_CONTROLLER   (1U << 0)
#define SGI_HUB_POPULATION_ELSC               (1U << 1)
#define SGI_HUB_POPULATION_ALL               ((1U << 2) - 1)

typedef enum SGIHubOneWireKind {
    SGI_HUB_ONEWIRE_NONE = 0,
    SGI_HUB_ONEWIRE_BLANK_DS2505,
} SGIHubOneWireKind;

/* The four uncached spaces a node-qualified physical address selects. */
#define SGI_HUB_CLAIM_SPACES  4

/*
 * The views the Hub's claim aliases fill: one per attached processor, and
 * one for the Hub's own engines, which address memory as a processor does
 * minus the per-processor pieces.
 */
#define SGI_HUB_CLAIM_VIEWS   (SGI_HUB_PROCESSORS + 1)
#define SGI_HUB_DEVICE_VIEW   SGI_HUB_PROCESSORS

/*
 * The nameless low window the cached-alias size bounds: the memory
 * space's, one per processor view -- the register's domain is the
 * cached space alone (CALIAS_BASE is the cached base in the public
 * alias table). The machine pre-creates each window at the largest
 * size the register encodes; the Hub only resizes and gates it.
 */
#define SGI_HUB_CALIAS_MAX_SIZE (64 * MiB)

struct SGIHubState {
    SysBusDevice parent_obj;

    MemoryRegion regs;

    /*
     * The PCF8584 mapped over this window's I2C addresses (Origami project
     * tree, docs/datasheets/pcf8584-elsc.md); embedded because the bus it
     * masters belongs to this node.
     */
    PCF8584State i2c;

    /* Properties selecting the board population and I2C placement. */
    uint32_t population;
    uint64_t i2c_controller_offset;
    uint8_t i2c_elsc_addr;
    uint8_t i2c_nvram_first_addr;
    uint32_t i2c_nvram_pages;
    uint8_t onewire_kind;

    /* Diagnostic reference; the I2C bus retains object ownership. */
    SGIElscState *elsc;

    /* The seven big windows, as one region for the machine to place. */
    MemoryRegion bwin;
    MemoryRegion bwin_window[SGI_HUB_BWIN_ENTRIES];
    MemoryRegion bwin_io_window[SGI_HUB_BWIN_ENTRIES];
    MemoryRegion bwin_empty;

    /* Crosstalk space and widget stride, both the machine's to define. */
    MemoryRegion *xtalk;
    MemoryRegion *xtalk_io;
    uint32_t xtalk_widget_shift;

    /* The engines' view of the fabric: the device claim view, as a space. */
    AddressSpace bte_as;

    /*
     * The fabric this Hub's network interface is an endpoint on, and which
     * endpoint. The fabric hands the Hub a reachability map -- per node
     * number, the endpoint programmed with it that the fabric connects --
     * and the Hub aims its aliases by that map and by nothing else.
     */
    SN0FabricState *fabric;
    uint32_t fabric_endpoint;

    /*
     * The claim overlays: per view, one alias per uncached space per
     * fabric endpoint, placed at the node-number stride that endpoint
     * currently holds and aimed at the endpoint's own stride in the
     * canonical map, over an error floor across every stride. The machine
     * wires the per-view containers, the canonical map the aliases resolve
     * into, and the geometry; the Hub owns the regions and moves them
     * whenever the fabric's map changes.
     */
    MemoryRegion *claim_view[SGI_HUB_CLAIM_VIEWS];
    MemoryRegion *claim_target;
    uint32_t claim_node_shift;
    uint32_t claim_space_shift;
    MemoryRegion *claim_alias[SGI_HUB_CLAIM_VIEWS][SGI_HUB_CLAIM_SPACES];
    MemoryRegion claim_error[SGI_HUB_CLAIM_VIEWS][SGI_HUB_CLAIM_SPACES];
    bool claim_ready;

    /*
     * The nameless low memory window this Hub's cached-alias size
     * bounds, per processor view, pre-created and placed by the
     * machine; an unwired slot is a view the register does not reach.
     */
    MemoryRegion *calias_window[SGI_HUB_PROCESSORS];

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

    /* Physical processor-module slot, or zero for the Origin 200. */
    uint32_t physical_slot;

    /* Whether this processor module is the Origin 200 slave chassis. */
    bool slave_module;

    /* How many processor sockets this node has populated. */
    uint32_t cpus;

    /* Physical ELSC DIP switches, expressed as asserted switch bits. */
    uint8_t elsc_dip_switches;

    /* Global QEMU CPU index of this node's slice A. */
    uint32_t cpu_base;

    /*
     * This node's processors. qemu_get_cpu() walks the machine's whole
     * processor list, and the polled reads ask for them on every poll,
     * so they are resolved once and kept. Derived from the machine's
     * topology, which outlives the device, so neither reset state nor
     * migration state.
     */
    CPUState *cpu[SGI_HUB_PROCESSORS];

    /* Revision reported in the widget identity. */
    uint8_t widget_revision;

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

    /*
     * The clock the counter counts, and the virtual-clock time at which it
     * read zero. The rate is the machine's, not this device's: the Hub clock
     * is a separate domain because no source relates the two.
     */
    Clock *rt_clk;
    int64_t pi_rt_origin_ns;

    /*
     * The counter's reciprocal: the clock period it was computed for, the
     * multiplier that turns the counter's division into one multiply, and
     * the largest elapsed time the multiplier is exact over. Derived from
     * the clock and rebuilt whenever the period changes, so they are
     * neither reset state nor migrated state.
     */
    uint64_t rt_period;
    uint64_t rt_reciprocal;
    uint64_t rt_reciprocal_limit_ns;

    /* A match pends whether or not enabled; enable only gates the line. */
    uint64_t pi_rt_compare[SGI_HUB_PROCESSORS];
    uint8_t pi_rt_pending[SGI_HUB_PROCESSORS];
    uint8_t pi_rt_enable[SGI_HUB_PROCESSORS];

    /*
     * The multi-threaded comparator hold, host-only and discarded across
     * reset and migration. Per processor: whether the other has read its
     * comparator since the value changed; how often its comparator has been
     * written; the other's write count at its last read and at its last
     * post hold; and its active hold, zero for none, with the hold's kind.
     *
     * A withdrawal hold also carries the comparator value its processor
     * wrote, which lands when the hold ends. That is a write the guest has
     * already made, so migration lands it before saving rather than
     * discarding it with the rest.
     */
    bool pi_rt_seen[SGI_HUB_PROCESSORS];
    uint64_t pi_rt_writes[SGI_HUB_PROCESSORS];
    uint64_t pi_rt_read_at[SGI_HUB_PROCESSORS];
    uint64_t pi_rt_held_at[SGI_HUB_PROCESSORS];
    uint64_t pi_rt_hold[SGI_HUB_PROCESSORS];
    uint8_t pi_rt_hold_kind[SGI_HUB_PROCESSORS];
    uint64_t pi_rt_hold_seq;
    bool pi_rt_withheld_valid[SGI_HUB_PROCESSORS];
    uint64_t pi_rt_withheld[SGI_HUB_PROCESSORS];

    /*
     * Whether the first processor has given its alive signal since reset.
     * Unlike the hold itself it cannot be re-derived from the registers --
     * firmware clears the alive bit again later -- so it is migrated.
     */
    bool pi_rt_alive_seen;

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

    /* The LLP port parameters and traffic-driven error counters. */
    uint64_t ni_port_parms;
    uint64_t ni_port_error;
    uint64_t ni_ageing[8];

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
    uint32_t ni_meta_route_written;
    uint16_t ni_local_route_written;

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

    /*
     * When the slot underway finishes. A slot must not complete inside the
     * write that starts it, so completion is derived from this deadline when
     * the register is read rather than delivered into it. INT64_MAX while no
     * slot has been started.
     */
    int64_t md_onewire_deadline_ns;

    /*
     * The wire itself, and the responder on it: the PIMM manufacturing-record
     * part, DS2505-compatible (the hub one-wire datasheet's settled
     * contract). The serial is a synthetic per-unit property the machine
     * wires, from which the part derives its ROM identity at realize. The
     * part is left unprogrammed, because what the record bytes should say is
     * an open question.
     */
    OneWireBus *onewire_bus;
    uint64_t onewire_serial;
    SGIHubModuleIdentity module_identity;
    bool has_module_identity;
    bool onewire_level;    /* what the wire sampled in the slot underway */

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
bool sgi_hub_sends_data_error(const SGIHubState *s);
void sgi_hub_latch_port_error(SGIHubState *s, uint64_t errors);
void sgi_hub_record_port_check_error(SGIHubState *s);
bool sgi_hub_first_turn(const SGIHubState *s, uint32_t destination,
                         uint8_t *turn);
void sgi_hub_latch_prom_pi_error(SGIHubState *s);
void sgi_hub_set_cpu_enable(SGIHubState *s, unsigned int cpu, bool enabled,
                            bool synchronous);
bool sgi_hub_attach_module_identity(SGIHubState *s,
                                    const SGIHubModuleIdentity *identity,
                                    Error **errp);

/*
 * Wire this Hub's own outbound big-window I/O traffic (the "xtalk-io"
 * property) to target: a crosstalk-attach link's own doing, since a
 * directly attached Hub never populates it. Must run before this Hub
 * realizes, and at most once; target typically outlives the call by
 * far (the crossbow link's own proxy region, filled in once its
 * Crossbow has realized).
 */
bool sgi_hub_set_xtalk_io(SGIHubState *s, MemoryRegion *target, Error **errp);

/* The fabric's side of the interface: this endpoint's map or link state moved. */
void sgi_hub_fabric_changed(SGIHubState *s);
bool sgi_hub_vector_access(SGIHubState *target, SN0FabricVectorAccess *access);

#endif /* HW_MISC_SGI_HUB_H */
