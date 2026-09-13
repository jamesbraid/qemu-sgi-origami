/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A minimum arrival latency for received frames. See
 * include/hw/net/sgi-net-rx-latency.h.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/net/sgi-net-rx-latency.h"
#include "trace.h"

static SGINetRxLatencyEntry *sgi_net_rx_latency_slot(SGINetRxLatency *l,
                                                     uint32_t n)
{
    return &l->ring[(l->head + n) % SGI_NET_RX_LATENCY_DEPTH];
}

/*
 * The head's deadline drives the timer. Deadlines only ever increase along
 * the line -- the floor is constant and arrivals are ordered -- so one timer
 * serves every held completion.
 */
static void sgi_net_rx_latency_rearm(SGINetRxLatency *l)
{
    if (l->count == 0) {
        timer_del(l->timer);
        return;
    }
    timer_mod(l->timer, sgi_net_rx_latency_slot(l, 0)->deadline_ns);
}

/* Double the wait after a refusal, from the floor and up to the cap. */
static int64_t sgi_net_rx_latency_backoff(const SGINetRxLatency *l,
                                          int64_t previous)
{
    int64_t next = previous ? previous * 2 :
                   (l->floor_ns ? l->floor_ns :
                    SGI_NET_RX_LATENCY_RETRY_BASE_NS);

    return MIN(next, SGI_NET_RX_LATENCY_RETRY_MAX_NS);
}

static void sgi_net_rx_latency_expire(void *opaque)
{
    SGINetRxLatency *l = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t record[SGI_NET_RX_LATENCY_RECORD_MAX];
    bool published = false;

    /*
     * Copy the record out before publishing it. Publishing raises the model's
     * receive source, and a model is free to flush the backend from there,
     * which lands back in sgi_net_rx_latency_hold() and appends to this same
     * line -- so the head is only dropped once the model says it landed.
     */
    while (l->count != 0) {
        SGINetRxLatencyEntry *e = sgi_net_rx_latency_slot(l, 0);

        if (e->deadline_ns > now) {
            break;
        }
        memcpy(record, e->record, l->record_size);
        trace_sgi_net_rx_latency_attempt(l->count - 1);
        if (!l->publish(l->opaque, record)) {
            /*
             * Refused. Hold it and try again, backing off so a fault the
             * guest never repairs does not spin the timer or the log.
             */
            e = sgi_net_rx_latency_slot(l, 0);
            e->backoff_ns = sgi_net_rx_latency_backoff(l, e->backoff_ns);
            e->deadline_ns = now + e->backoff_ns;
            break;
        }
        l->head = (l->head + 1) % SGI_NET_RX_LATENCY_DEPTH;
        l->count--;
        published = true;
    }

    sgi_net_rx_latency_rearm(l);
    if (published && l->drain) {
        l->drain(l->opaque);
    }
}

bool sgi_net_rx_latency_init(SGINetRxLatency *l, uint32_t floor_us,
                             size_t record_size,
                             SGINetRxLatencyPublish publish,
                             SGINetRxLatencyDrain drain, void *opaque,
                             Error **errp)
{
    assert(record_size <= SGI_NET_RX_LATENCY_RECORD_MAX);
    /* Held as nanoseconds in a 32-bit field, so the property has a ceiling. */
    if (floor_us > SGI_NET_RX_LATENCY_MAX_US) {
        error_setg(errp, "rx-arrival-latency-us must be %u or less",
                   SGI_NET_RX_LATENCY_MAX_US);
        return false;
    }
    l->head = 0;
    l->count = 0;
    l->floor_ns = floor_us * 1000u;
    l->record_size = record_size;
    l->publish = publish;
    l->drain = drain;
    l->opaque = opaque;
    l->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sgi_net_rx_latency_expire, l);
    return true;
}

void sgi_net_rx_latency_cleanup(SGINetRxLatency *l)
{
    timer_free(l->timer);
    l->timer = NULL;
}

void sgi_net_rx_latency_reset(SGINetRxLatency *l)
{
    if (l->timer) {
        timer_del(l->timer);
    }
    l->head = 0;
    l->count = 0;
}

bool sgi_net_rx_latency_full(const SGINetRxLatency *l)
{
    return l->count >= SGI_NET_RX_LATENCY_DEPTH;
}

/* Put one completion on the tail of the line, due after delay_ns. */
static void sgi_net_rx_latency_enqueue(SGINetRxLatency *l, const void *record,
                                       int64_t delay_ns)
{
    SGINetRxLatencyEntry *e;

    /*
     * A full line is the caller's to prevent: every model asks
     * sgi_net_rx_latency_full() before it stages a frame, and refuses the
     * frame the way it already refuses one with nowhere to go.
     */
    assert(!sgi_net_rx_latency_full(l));

    e = sgi_net_rx_latency_slot(l, l->count);
    e->deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns;
    e->backoff_ns = 0;
    memcpy(e->record, record, l->record_size);
    l->count++;
    trace_sgi_net_rx_latency_hold(l->count, e->deadline_ns);
    sgi_net_rx_latency_rearm(l);
}

void sgi_net_rx_latency_hold(SGINetRxLatency *l, const void *record)
{
    if (l->floor_ns == 0) {
        sgi_net_rx_latency_no_wire(l, record);
        return;
    }
    sgi_net_rx_latency_enqueue(l, record, l->floor_ns);
}

void sgi_net_rx_latency_no_wire(SGINetRxLatency *l, const void *record)
{
    /*
     * Behind whatever is already waiting, or the index this publishes could
     * pass one still held and then move backwards when that one lands.
     */
    if (l->count != 0) {
        sgi_net_rx_latency_enqueue(l, record, l->floor_ns);
        return;
    }

    trace_sgi_net_rx_latency_attempt(0);
    if (l->publish(l->opaque, record)) {
        return;
    }

    /*
     * Refused. The frame is in the guest's buffer already, so it is held for
     * a retry rather than dropped -- which is what the models did with the
     * same refusal before this existed, by leaving it for the backend to
     * offer again.
     */
    sgi_net_rx_latency_enqueue(l, record,
                               sgi_net_rx_latency_backoff(l, 0));
}

static const VMStateDescription vmstate_sgi_net_rx_latency_entry = {
    .name = "sgi-net-rx-latency/entry",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(deadline_ns, SGINetRxLatencyEntry),
        VMSTATE_INT64(backoff_ns, SGINetRxLatencyEntry),
        VMSTATE_UINT8_ARRAY(record, SGINetRxLatencyEntry,
                            SGI_NET_RX_LATENCY_RECORD_MAX),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * The held completions travel with the device rather than being dropped or
 * published early at the save point: a completion published out of its own
 * timer would run against an interrupt controller that may already have been
 * written out, and dropping it would lose a frame the guest's buffer already
 * holds. The floor, record size and callbacks are set at realize on both
 * ends and are not part of the stream.
 */
const VMStateDescription vmstate_sgi_net_rx_latency = {
    .name = "sgi-net-rx-latency",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(head, SGINetRxLatency),
        VMSTATE_UINT32(count, SGINetRxLatency),
        VMSTATE_TIMER_PTR(timer, SGINetRxLatency),
        VMSTATE_STRUCT_ARRAY(ring, SGINetRxLatency,
                             SGI_NET_RX_LATENCY_DEPTH, 1,
                             vmstate_sgi_net_rx_latency_entry,
                             SGINetRxLatencyEntry),
        VMSTATE_END_OF_LIST()
    }
};
