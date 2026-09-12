/*
 * SGI Origin 200 module system controller (MSC)
 *
 * Copyright (c) 2026 Origami contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SGI_ELSC_H
#define HW_MISC_SGI_ELSC_H

#include "hw/i2c/i2c.h"
#include "hw/misc/sgi-diagnostic.h"
#include "qemu/timer.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_SGI_ELSC "sgi-elsc"
OBJECT_DECLARE_SIMPLE_TYPE(SGIElscState, SGI_ELSC)

/*
 * Length, text, checksum summing to zero mod 256. A length above 94
 * overruns the firmware's own 96-byte buffers; never emit one.
 */
#define SGI_ELSC_MAX_TEXT   94
#define SGI_ELSC_MAX_FRAME  (SGI_ELSC_MAX_TEXT + 2)

/*
 * AUX input queued for "get". The depth is a model choice; no source
 * states the real queue's.
 */
#define SGI_ELSC_RX_QUEUE_SIZE 256

/* The front-panel eight-character alphanumeric display. */
#define SGI_ELSC_DISPLAY_CHARS 8

/*
 * A destructive command whose effect is deferred until the guest reads its
 * "ok" (see deferred_action/deferred_committed): "rst" resets, "pwr c"
 * power-cycles (a reset that also re-arms the cold-power latch) and "pwr d"
 * shuts down.
 */
typedef enum SGIElscAction {
    SGI_ELSC_ACTION_NONE = 0,
    SGI_ELSC_ACTION_RESET,
    SGI_ELSC_ACTION_CYCLE,
    SGI_ELSC_ACTION_SHUTDOWN,
} SGIElscAction;

/* The unprefixed spellings the model and its migration state are written in. */
#define ELSC_ACTION_NONE     SGI_ELSC_ACTION_NONE
#define ELSC_ACTION_RESET    SGI_ELSC_ACTION_RESET
#define ELSC_ACTION_CYCLE    SGI_ELSC_ACTION_CYCLE
#define ELSC_ACTION_SHUTDOWN SGI_ELSC_ACTION_SHUTDOWN

/*
 * An Origin 2000 module complex owns what is module-scoped rather than
 * node-scoped here: what a reset or a power-off actually reaches, when this
 * controller occupies the shared bus, and which processor the module's one
 * alternate console belongs to. It installs these before realize
 * (sgi_elsc_set_module_ops). Every member is optional, and with none
 * installed the controller answers for its own module, which is what the
 * single-module Origin 200 needs.
 */
typedef struct SGIElscModuleOps {
    /* True when the module can actually carry the action out. */
    bool (*can_execute)(void *opaque, SGIElscAction action);
    /* Carry it out. Only called after can_execute() returned true. */
    void (*execute)(void *opaque, SGIElscAction action);
    /* This controller is being addressed as a target, or has finished. */
    void (*slave_active)(void *opaque, bool active);
    /* The processor that most recently sent console output. */
    void (*console_owner)(void *opaque, unsigned processor);
    /* Queued alternate-console input is or is not waiting. */
    void (*console_input)(void *opaque, bool pending);
    /* One bit slot on the module-owned midplane identity line. */
    bool (*onewire_slot)(void *opaque, uint32_t pulse, uint32_t sample);
} SGIElscModuleOps;

struct SGIElscState {
    I2CSlave parent_obj;
    DeviceDiagnosticSink *diagnostic;
    bool diagnostic_arbitration_broadcast;

    /* The module owner, or NULL where this controller is the whole module. */
    const SGIElscModuleOps *module_ops;
    void *module_opaque;

    /*
     * The request, one byte per I2C send; sized one over the legal maximum
     * so an over-long guest frame lands here, not in adjacent state.
     */
    uint8_t rxbuf[SGI_ELSC_MAX_FRAME + 1];
    uint32_t rxlen;

    /* The response being read back, built once the request is complete. */
    uint8_t txbuf[SGI_ELSC_MAX_FRAME];
    uint32_t txlen;
    uint32_t txpos;
    /*
     * The pending response's owner: the global processor code whose
     * request framed txbuf, -1 when none. While the response is
     * unconsumed, the owner's arbitration grant uses signal 2
     * (elsc_arb_step()).
     */
    int32_t resp_owner;

    /*
     * The arbitration grant rotation, free-running from realize
     * whether or not anything polls (elsc_arb_step()).
     */
    I2CBus *bus;
    /*
     * The slave controller whose PCF observes this master's grant ring.
     * Machine wiring owns both devices for the same lifetime.
     */
    SGIElscState *arb_peer;
    QEMUTimer *arb_timer;
    /*
     * Whether this controller publishes grants. Only the master module's
     * controller owns the rotation.
     */
    bool arbitration;
    /* Hub-supplied local population, used to find a grant's queue owner. */
    uint32_t num_cpus;
    /*
     * Hub-supplied: the global index of this node's slice A processor,
     * naming this controller's instance in the arbitration trace. The
     * code space is global processor indexes, so it also says which
     * codes are local.
     */
    uint32_t cpu_base;
    /* Number of positions in the ring; eight unless a machine narrows it. */
    uint32_t arb_positions;
    uint8_t dip_switches;
    uint32_t arb_code;    /* next encoded ring position */
    uint32_t arb_rev;     /* full rotations completed */
    bool arb_present;     /* legacy pre-v12 gap state, migrated only */
    bool arb_held;        /* legacy pre-v12 hold state, migrated only */
    uint8_t arb_signal;   /* most recently published grant */
    int64_t arb_hold_until;   /* legacy pre-v12 deadline, migrated only */

    /*
     * The AUX serial port, multiplexed with the command channel by one
     * input rule: bytes after 0x14 are a controller command, never queued.
     */
    CharFrontend chr;
    uint8_t rx_queue[SGI_ELSC_RX_QUEUE_SIZE];
    uint32_t rx_queue_len;
    bool in_escape;   /* discarding a controller command typed on the line */

    /*
     * Supervisor mode gates the destructive commands (page 124 of the
     * Origin 200 Owner's Guide); "pas" sets it, factory default "none".
     */
    bool supervisor;

    /*
     * Cold power-on latch, read once by "pwr q". Set when the controller
     * first powers up (realize), never re-armed by a warm reset, and
     * cleared when read -- so only the first query in a power cycle sees
     * it. The clear-on-read one-shot is from the 6.5.7 driver and is
     * unverified on 6.5.30.
     */
    bool cold_power_on;

    /*
     * A destructive command deferred until the guest reads its "ok": what to
     * do (an ELSC_ACTION_* value), whether the ack has been read, and whether
     * its bottom half has been scheduled.
     *
     * ARMED (action set, not committed): accepted, ack queued but unread -- a
     * superseding request cancels it. COMMITTED (action set, committed): the
     * ack was fully read at the fetch seam; supersession can no longer cancel
     * it. SCHEDULED (deferred_scheduled): a transaction boundary has queued the
     * bottom half that carries the effect out; it is set where the bottom half
     * is scheduled and cleared when it runs. The effect, and any side effect,
     * fires only from that bottom half, so a canceled command leaves no trace.
     * All three fields are migrated so an action in flight behaves correctly
     * across a migration: post_load re-arms exactly the bottom half the source
     * had scheduled and not yet run (deferred_scheduled), without reasoning
     * about the frame state at all.
     *
     * The actions are "rst" (reset), "pwr c" (power-cycle) and "pwr d"
     * (shutdown).
     */
    uint8_t deferred_action;
    bool deferred_committed;
    bool deferred_scheduled;

    /*
     * The "tas" test-and-set lock cell. tas_held is the occupancy; tas_lock
     * is the owner byte, meaningful only while held. Occupancy is kept
     * separate from the owner byte because any byte is a legal owner --
     * NUL (0x00) included -- so the owner value alone cannot signal "free".
     * A distinct service carried over the bus, not the arbitration.
     */
    bool tas_held;
    uint8_t tas_lock;

    /* The alphanumeric display, written by "dsp" and "dsc". */
    uint8_t display[SGI_ELSC_DISPLAY_CHARS];

    /* Warn once per dark-console boot, not once per dropped flush. */
    bool aux_no_backend_warned;
};

/* Install the module owner. Wiring, so it must precede realize. */
/*
 * Clear the protocol queues without resetting the controller. A module
 * owner's recovery, reset and power paths reinitialise this much and no
 * more.
 */
void sgi_elsc_reinitialize(SGIElscState *s);

void sgi_elsc_set_module_ops(SGIElscState *s, const SGIElscModuleOps *ops,
                             void *opaque);

/* Join a realized slave to its master's arbitration ring. */
void sgi_elsc_share_arbitration(SGIElscState *master, SGIElscState *slave,
                                unsigned int positions);

#endif /* HW_MISC_SGI_ELSC_H */
