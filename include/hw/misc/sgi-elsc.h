/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI Origin 200 module system controller (MSC)
 */

#ifndef HW_MISC_SGI_ELSC_H
#define HW_MISC_SGI_ELSC_H

#include "hw/i2c/i2c.h"
#include "qemu/timer.h"
#include "chardev/char-fe.h"
#include "system/runstate.h"
#include "qom/object.h"

#define TYPE_SGI_ELSC "sgi-elsc"
OBJECT_DECLARE_SIMPLE_TYPE(SGIElscState, SGI_ELSC)

/*
 * Length, text, checksum summing to zero mod 256. A length above 94 overruns
 * the firmware's own 96-byte buffers; never emit one.
 */
#define SGI_ELSC_MAX_TEXT   94
#define SGI_ELSC_MAX_FRAME  (SGI_ELSC_MAX_TEXT + 2)

/*
 * AUX input queued for "get". The depth is a model choice; no source states
 * the real queue's.
 */
#define SGI_ELSC_RX_QUEUE_SIZE 256

/* The front-panel eight-character alphanumeric display. */
#define SGI_ELSC_DISPLAY_CHARS 8

/* The supplies "vlm" margins: 3.45 V, 5 V and a third output. */
#define SGI_ELSC_SUPPLIES 3

/* The longest "pwr d" delay or "hbt" interval, in seconds: 600. */
#define SGI_ELSC_DELAY_MAX 0x258

/*
 * A command whose effect is deferred until the guest reads its "ok" (see
 * deferred_action/deferred_committed): "rst" resets, "pwr c" power-cycles
 * (a reset that also re-arms the cold-power latch), "pwr d" shuts down and
 * "pwr u" powers an unpowered module up.
 */
typedef enum SGIElscAction {
    SGI_ELSC_ACTION_NONE = 0,
    SGI_ELSC_ACTION_RESET,
    SGI_ELSC_ACTION_CYCLE,
    SGI_ELSC_ACTION_SHUTDOWN,
    SGI_ELSC_ACTION_POWER_UP,
} SGIElscAction;

/*
 * The messages a command makes the controller queue for the Hubs, which
 * "get" returns in place of console input.
 */
typedef enum SGIElscMessage {
    SGI_ELSC_MESSAGE_NONE = 0,
    SGI_ELSC_MESSAGE_PD_REMOTE,
    SGI_ELSC_MESSAGE_PD_CYCLE,
    SGI_ELSC_MESSAGE_RST,
    SGI_ELSC_MESSAGE_COUNT,
} SGIElscMessage;

/* Unprefixed spellings used by the model and its migration state. */
#define ELSC_ACTION_NONE     SGI_ELSC_ACTION_NONE
#define ELSC_ACTION_RESET    SGI_ELSC_ACTION_RESET
#define ELSC_ACTION_CYCLE    SGI_ELSC_ACTION_CYCLE
#define ELSC_ACTION_SHUTDOWN SGI_ELSC_ACTION_SHUTDOWN
#define ELSC_ACTION_POWER_UP SGI_ELSC_ACTION_POWER_UP

/*
 * An Origin 2000 module complex owns what is module-scoped rather than
 * node-scoped here: what a reset or a power-off actually reaches, when this
 * controller occupies the shared bus, and which processor the module's one
 * alternate console belongs to. It installs these before realize
 * (sgi_elsc_set_module_ops). Every member is optional, and with none
 * installed the controller answers for its own module, which is what the
 * single-module Origin 200 needs: a module that is powered whenever the
 * machine runs.
 */
typedef struct SGIElscModuleOps {
    /* Whether the module is powered. */
    bool (*powered)(void *opaque);
    /* True when the module can actually carry the action out. */
    bool (*can_execute)(void *opaque, SGIElscAction action);
    /* Carry it out. Only called after can_execute() returned true. */
    void (*execute)(void *opaque, SGIElscAction action);
    /*
     * Pulse the module's NMI output for 100 ms; false when the module has
     * no NMI path or a pulse is already running.
     */
    bool (*nmi)(void *opaque);
    /* This controller is being addressed as a target, or has finished. */
    void (*slave_active)(void *opaque, bool active);
    /* The processor that most recently sent console output. */
    void (*console_owner)(void *opaque, unsigned processor);
    /* Queued alternate-console input is or is not waiting. */
    void (*console_input)(void *opaque, bool pending);
    /* One bit slot on the module-owned midplane identity line. */
    bool (*onewire_slot)(void *opaque, uint32_t pulse, uint32_t sample);
    /*
     * A controller message for the Hubs: announce it to every processor
     * code for its lifetime.
     */
    void (*post_message)(void *opaque);
    /*
     * Whether the message is still announced to this processor; if so, it
     * stops being announced to it.
     */
    bool (*take_message)(void *opaque, unsigned processor);
} SGIElscModuleOps;

struct SGIElscState {
    I2CSlave parent_obj;

    /* The module owner, or NULL where this controller is the whole module. */
    const SGIElscModuleOps *module_ops;
    void *module_opaque;

    /*
     * The request, one byte per I2C send; sized one over the legal maximum so
     * an over-long guest frame lands here, not in adjacent state.
     */
    uint8_t rxbuf[SGI_ELSC_MAX_FRAME + 1];
    uint32_t rxlen;

    /* The response being read back, built once the request is complete. */
    uint8_t txbuf[SGI_ELSC_MAX_FRAME];
    uint32_t txlen;
    uint32_t txpos;
    /*
     * The pending response's owner: the global processor code whose request
     * framed txbuf, -1 when none. While the response is unconsumed, the
     * owner's arbitration grant uses signal 2 (elsc_arb_step()).
     */
    int32_t resp_owner;

    /*
     * The arbitration grant rotation, free-running from realize whether or
     * not anything polls (elsc_arb_step()).
     */
    I2CBus *bus;
    /*
     * The slave controller whose PCF observes this master's grant ring.
     * Machine wiring owns both devices for the same lifetime.
     */
    SGIElscState *arb_peer;
    QEMUTimer *arb_timer;
    /*
     * The one-second wait between an acknowledged "rst" and the reset it
     * asks for. Another "rst" inside the wait starts it again.
     */
    QEMUTimer *rst_timer;
    /*
     * The one-second wait between "nmi" and its pulse, and, with no module
     * owner to drive the pulse, the 100 ms the controller holds its own
     * NMI output.
     */
    QEMUTimer *nmi_timer;
    QEMUTimer *nmi_pulse_timer;
    bool nmi_pulse_active;
    qemu_irq nmi_out;
    /*
     * The next machine reset is one this controller asked for, a module
     * reset or power cycle, rather than the controller's own start: the
     * controller keeps running through it and keeps what it holds. Not
     * migrated: the reset it marks is not part of the stream either.
     */
    bool self_reset;
    Notifier shutdown_notifier;
    /*
     * The power-down countdown "pwr d <sec>" arms, on the virtual clock in
     * milliseconds. Each timed request reloads it; expiry shuts down.
     */
    QEMUTimer *pwr_down_timer;
    /*
     * Whether this controller publishes grants. Only the master module's
     * controller owns the rotation.
     */
    bool arbitration;
    /*
     * build: which build of the controller firmware this is, "origin200" or
     * "origin2000". One program built two ways, differing in the commands
     * it answers and its version string. origin2000 is the parsed result.
     */
    char *build;
    bool origin2000;
    /* Hub-supplied local population, used to find a grant's queue owner. */
    uint32_t num_cpus;
    /*
     * Hub-supplied: the global index of this node's slice A processor, naming
     * this controller's instance in the arbitration trace. The code space is
     * global processor indexes, so it also says which codes are local.
     */
    uint32_t cpu_base;
    /* Number of positions in the ring: four processor codes and the slave. */
    uint32_t arb_positions;
    uint8_t dip_switches;
    uint32_t arb_code;    /* next encoded ring position */
    uint8_t arb_signal;   /* most recently published grant */

    /*
     * The AUX serial port, multiplexed with the command channel by one input
     * rule: bytes after 0x14 are a controller command, never queued.
     */
    CharFrontend chr;
    uint8_t rx_queue[SGI_ELSC_RX_QUEUE_SIZE];
    uint32_t rx_queue_len;
    bool in_escape;   /* discarding a controller command typed on the line */

    /*
     * Cold power-on latch, read once by "pwr q". Set when the controller
     * first powers up (realize), never re-armed by a warm reset, and cleared
     * when read, so only the first query in a power cycle sees it. The
     * clear-on-read one-shot is from the 6.5.7 driver and is unverified on
     * 6.5.30.
     */
    bool cold_power_on;

    /*
     * A destructive command deferred until the guest reads its "ok": what to
     * do (an ELSC_ACTION_* value), whether the ack has been read, and whether
     * its bottom half has been scheduled.
     *
     * ARMED (action set, not committed): accepted, ack queued but unread. A
     * superseding request cancels it. COMMITTED (action set, committed): the
     * ack was fully read at the fetch of the last frame byte; supersession
     * can no longer cancel it. SCHEDULED (deferred_scheduled): a transaction
     * boundary has queued the bottom half that carries the effect out; it is
     * set where the bottom half is scheduled and cleared when it runs. The
     * effect, and any side effect, fires only from that bottom half, so a
     * canceled command leaves no trace. All three fields are migrated so an
     * action in flight behaves correctly across a migration: post_load
     * re-arms exactly the bottom half the source had scheduled and not yet
     * run (deferred_scheduled), without reasoning about the frame state at
     * all.
     *
     * The actions are "rst" (reset), "pwr c" (power-cycle) and "pwr d"
     * (shutdown).
     */
    uint8_t deferred_action;
    bool deferred_committed;
    bool deferred_scheduled;

    /*
     * The "tas" test-and-set lock cell: one byte, ASCII '0' when free. A
     * distinct service carried over the bus, not the arbitration.
     */
    uint8_t tas_lock;

    /* The alphanumeric display, written by "dsp" and "dsc". */
    uint8_t display[SGI_ELSC_DISPLAY_CHARS];

    /*
     * Flags the controller keeps for its commands: the fan-high request
     * ("fan"), a declared multimodule controller ("fsc"), the factory's
     * short timeouts ("tst") and each supply's margin ("vlm", 'n', 'h' or
     * 'l' for the 3.45 V, 5 V and third outputs in that order).
     */
    bool fan_high;
    bool fsc_present;
    bool short_timeouts;
    uint8_t margins[SGI_ELSC_SUPPLIES];

    /*
     * What "sla" reports leave behind: on a master, whether its slave has
     * announced itself and the slave's last temperature letter; on a slave,
     * the report it would forward to its master.
     */
    bool slave_link;
    uint8_t slave_temp;
    uint8_t slave_report;

    /*
     * The controller message waiting for the Hubs (an SGIElscMessage) and,
     * with no module owner to announce it, the processor codes it is still
     * announced to and its one-second lifetime.
     */
    uint8_t message;
    uint8_t message_mask;
    QEMUTimer *message_timer;

    /* Warn once per dark-console boot, not once per dropped flush. */
    bool aux_no_backend_warned;
};

/* Install the module owner. Wiring, so it must precede realize. */
/*
 * Clear the protocol queues without resetting the controller. A module
 * owner's recovery, reset and power paths reinitialise this much and no more.
 */
void sgi_elsc_reinitialize(SGIElscState *s);

void sgi_elsc_set_module_ops(SGIElscState *s, const SGIElscModuleOps *ops,
                             void *opaque);

/* Join a realized slave to its master's arbitration ring. */
void sgi_elsc_share_arbitration(SGIElscState *master, SGIElscState *slave,
                                unsigned int positions);

#endif /* HW_MISC_SGI_ELSC_H */
