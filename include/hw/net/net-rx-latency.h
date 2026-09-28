/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A minimum arrival latency for received frames, shared by the SGI machines'
 * ethernet adapters.
 *
 * The models write a frame into the buffer the guest posted as soon as the
 * backend hands it over, but a real adapter cannot tell the guest about it
 * until the frame has actually crossed the wire and the peer has answered.
 * This holds the visible half of a receive -- the status word, the index the
 * guest reads, and the interrupt -- until an arrival deadline on
 * QEMU_CLOCK_VIRTUAL. It is a delay line, not a serialiser: each held
 * completion carries its own deadline and several may be outstanding, so the
 * floor costs latency and not throughput.
 *
 * The floor is an approximation, not a measured property of any of these
 * parts. It is not adapter-specific: every adapter model that uses it shares
 * this same derivation, and the same open question of what hardware
 * measurement would replace it with a real number.
 */

#ifndef HW_NET_SGI_NET_RX_LATENCY_H
#define HW_NET_SGI_NET_RX_LATENCY_H

#include "qapi/error.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"

/*
 * The default floor, in microseconds, and the measurement it comes from.
 *
 * Swept on the IOC3, whose loss rate without a floor is high enough for a
 * 50-ping boot to see it, with IRIX 6.5.30 running a ping loop: no floor lost
 * 18 of 100 pings; 20 us lost 3 of 150, 50 us 2 of 150 and 100 us 2 of 150;
 * 200 us lost none of 150, and none of another 300 on earlier builds with
 * the same floor. Any floor removes most of the loss -- 20 us against none is
 * not close -- but only 200 us has removed all of it, and against the pooled
 * 20-to-100 us points that difference holds at p = 0.015 (Fisher's exact test,
 * two-sided) once the earlier 300 are counted, p = 0.20 without them. So 200 us
 * stays; nothing shorter has been shown to do the job. The Xframe's loss is
 * too rare for a 50-ping boot to separate the floors, and it lost none at
 * 20 us and above.
 *
 * That agrees with the reasoning the default started from. The smallest real
 * exchange at the slowest rate these adapters present is about 20 us -- a
 * 64-byte request and its reply are 672 bit times each at 100 Mbit/s, 6.72 us
 * a frame, and a peer answering from its interrupt handler spends at least as
 * much again -- and that alone removes most of the loss. The rest needs the
 * emulator's stretch of guest-side work against the virtual clock accounted
 * for too: a traced IOC3 boot puts the guest's receive service at 68 to 480 us
 * of virtual time where the modelled processor would need something on the
 * order of 10 us (an estimate), a stretch of seven to forty-eight, and
 * twenty microseconds times the low end of that is about 140 us.
 *
 * The floor is not free: a saturated UDP transfer into the guest arrives short
 * at it where it arrives complete with no floor. That is the inherent
 * trade-off of holding completions at all: a taller floor trades less loss
 * for more added latency.
 *
 * A floor of zero turns the line off and publishes every completion where it
 * was staged, which is the model's behaviour before this existed.
 */
#define SGI_NET_RX_LATENCY_DEFAULT_US 200

/*
 * Completions that may be in flight at once: one floor's worth of frames at
 * the fastest link any of these adapters presents, which is about 167
 * 1500-byte frames in 200 microseconds at 10 Gbit/s, rounded up. The cap is
 * there to bound the state, not the rate -- a line shorter than a floor's
 * worth of arrivals turns into a rate limit, and a 16-deep one cost about
 * three parts in a thousand of a 16 MB receive-side UDP transfer.
 *
 * It is sized for the default floor. Raising rx-arrival-latency-us well above
 * that without raising this makes the line a rate limit again, at this many
 * frames per floor.
 */
#define SGI_NET_RX_LATENCY_DEPTH 256

/*
 * A publish the guest's own bus refuses is tried again rather than dropped:
 * the frame is already in the guest's buffer and only the word that says so
 * did not land. Each refusal doubles the wait up to this, so a fault the
 * guest never repairs costs a few log lines a second rather than thousands,
 * and the attempt after a repair still lands.
 */
#define SGI_NET_RX_LATENCY_RETRY_MAX_NS (250 * 1000 * 1000)

/* Where that backoff starts when there is no floor to start it from. */
#define SGI_NET_RX_LATENCY_RETRY_BASE_NS (1000 * 1000)

/* The floor is kept in nanoseconds in a 32-bit field, so it has a ceiling. */
#define SGI_NET_RX_LATENCY_MAX_US (UINT32_MAX / 1000u)

/* Room for one model's completion record; see sgi_net_rx_latency_init(). */
#define SGI_NET_RX_LATENCY_RECORD_MAX 64

typedef struct SGINetRxLatencyEntry {
    int64_t deadline_ns;
    /* How long the next attempt waits, once a publish has been refused. */
    int64_t backoff_ns;
    uint8_t record[SGI_NET_RX_LATENCY_RECORD_MAX];
} SGINetRxLatencyEntry;

typedef struct SGINetRxLatency SGINetRxLatency;

/*
 * Publish one staged receive: write its completion, advance the index the
 * guest reads and raise the model's receive source. Called with the BQL held,
 * from the main loop's timer -- or, where the owner publishes without waiting
 * (a floor of zero, or a frame the MAC turned round itself), directly from
 * the path that staged it.
 *
 * Returns false when a write the guest has to see did not land -- a DMA the
 * bus refused, say. The completion stays at the head of the line and is tried
 * again one floor later, which is the same held-and-retried contract these
 * models already apply to a frame the backend has to re-offer.
 */
typedef bool (*SGINetRxLatencyPublish)(void *opaque, const void *record);

/* The line has freed a slot: let the backend offer whatever it is holding. */
typedef void (*SGINetRxLatencyDrain)(void *opaque);

struct SGINetRxLatency {
    /* Migrated: a held completion and its deadline survive a save. */
    SGINetRxLatencyEntry ring[SGI_NET_RX_LATENCY_DEPTH];
    uint32_t head;
    uint32_t count;
    QEMUTimer *timer;

    /*
     * Not migrated. The floor is a device property and the rest is wired at
     * realize, so both ends of a migration set them identically -- which
     * migration between identical machine builds already guarantees.
     */
    uint32_t floor_ns;
    uint32_t record_size;
    SGINetRxLatencyPublish publish;
    SGINetRxLatencyDrain drain;
    void *opaque;
};

/*
 * Wire the line up at realize. record_size is the size of the caller's own
 * completion record, which must be plain data: it is copied by value and
 * migrated as bytes. Returns false with errp set for a floor the line cannot
 * hold, in which case nothing is allocated and the caller must return.
 */
bool sgi_net_rx_latency_init(SGINetRxLatency *l, uint32_t floor_us,
                             size_t record_size,
                             SGINetRxLatencyPublish publish,
                             SGINetRxLatencyDrain drain, void *opaque,
                             Error **errp);

/* Release the timer at unrealize. */
void sgi_net_rx_latency_cleanup(SGINetRxLatency *l);

/*
 * Discard every held completion. A reset interrupts the engine mid-flight, so
 * frames it had taken but not yet reported are gone -- their buffers keep
 * whatever was written into them and no index or interrupt ever names them.
 * The backend is not flushed here: the register writes that re-enable receive
 * already do that, and flushing into a half-programmed engine does not.
 */
void sgi_net_rx_latency_reset(SGINetRxLatency *l);

/* True when the line has no room; the caller applies its own backpressure. */
bool sgi_net_rx_latency_full(const SGINetRxLatency *l);

/* Hold one staged completion until its deadline, or publish it now at zero. */
void sgi_net_rx_latency_hold(SGINetRxLatency *l, const void *record);

/*
 * The guest has made room -- posted a buffer, moved a consumer index, or
 * otherwise done the thing a refused publish was waiting on. Retry the head
 * at once instead of waiting out its backoff. A completion that has not yet
 * been refused is left alone: its arrival deadline has not passed, and
 * nothing the guest does brings a frame's arrival forward.
 */
void sgi_net_rx_latency_ready(SGINetRxLatency *l);

/*
 * Publish a completion that carries no arrival latency: a frame the MAC
 * turned round inside itself, which never reached a wire, a peer or a
 * transceiver. It still may not overtake a completion already waiting, since
 * the indices these publish only ever move forwards, so an idle line takes it
 * at once and a busy one takes it in order behind what is held.
 */
void sgi_net_rx_latency_no_wire(SGINetRxLatency *l, const void *record);

extern const VMStateDescription vmstate_sgi_net_rx_latency;

#endif
