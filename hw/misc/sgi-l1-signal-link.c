/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The machine end of the L1 signal link: board lines over a chardev as
 * fixed 9-byte messages. The board end is not here.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/blocker.h"
#include "trace.h"

#define TYPE_SGI_L1_SIGNAL_LINK "sgi-l1-signal-link"
OBJECT_DECLARE_SIMPLE_TYPE(SGIL1SignalLinkState, SGI_L1_SIGNAL_LINK)

#define MSG_LEN 9
#define KIND_HELLO 1
#define KIND_CHANGE 2
#define KIND_STATE 3
#define PROTOCOL_VERSION 1
#define ROLE_MACHINE 1
#define ROLE_BOARD 2

/* Line ids: bit (id - 1) of the packed levels. The board drives 1 to 7. */
#define LINE_POWER_GOOD 8
#define LINE_NODE_RESET_STATE 9
#define LINE_COUNT 9
#define BOARD_LINES 7
#define MACHINE_MASK 0x180

struct SGIL1SignalLinkState {
    DeviceState parent_obj;
    CharFrontend chr;
    uint32_t epoch;
    /* Level of every line as this end last saw it, kept across reconnects. */
    uint16_t lines;
    /* This end's own change counter, for the life of the process. */
    uint32_t seq;
    /* The last peer change taken, and the peer's epoch from its last hello. */
    uint32_t peer_seq;
    uint32_t peer_epoch;
    bool peer_known;
    bool new_epoch;
    uint8_t up;
    uint8_t rx[MSG_LEN];
    unsigned rx_len;
    /* Board lines 1 to 7, in id order: the "nmi" list is four entries. */
    qemu_irq out[BOARD_LINES];
    Error *blocker;
};

/* CRC-8, polynomial 0x07, initial value 0, over bytes 0 to 7. */
static uint8_t msg_check(const uint8_t *m)
{
    uint8_t crc = 0;

    for (int i = 0; i < MSG_LEN - 1; i++) {
        crc ^= m[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
        }
    }
    return crc;
}

static bool msg_valid(const uint8_t *m)
{
    if (m[MSG_LEN - 1] != msg_check(m) || m[0] < KIND_HELLO ||
        m[0] > KIND_STATE) {
        return false;
    }
    return m[0] != KIND_CHANGE ||
           (m[5] >= 1 && m[5] <= BOARD_LINES && !m[6] && m[7] <= 1);
}

static void send_msg(SGIL1SignalLinkState *s, uint8_t kind, uint32_t seq,
                     uint8_t line, uint16_t level)
{
    uint8_t m[MSG_LEN];

    m[0] = kind;
    stl_be_p(m + 1, seq);
    m[5] = line;
    stw_be_p(m + 6, level);
    m[8] = msg_check(m);
    trace_sgi_l1_signal_tx(kind, seq, line, level);
    /* A short write would leave half a message: close and resync instead. */
    if (qemu_chr_fe_write(&s->chr, m, MSG_LEN) != MSG_LEN) {
        trace_sgi_l1_signal_reject("write", seq);
        qemu_chr_fe_disconnect(&s->chr);
    }
}

static void apply_line(SGIL1SignalLinkState *s, unsigned id, unsigned level)
{
    s->lines = deposit32(s->lines, id - 1, 1, level);
    qemu_set_irq(s->out[id - 1], level);
}

static void handle_msg(SGIL1SignalLinkState *s, const uint8_t *m)
{
    uint32_t seq = ldl_be_p(m + 1);
    uint16_t level = lduw_be_p(m + 6);

    trace_sgi_l1_signal_rx(m[0], seq, m[5], level);
    switch (m[0]) {
    case KIND_HELLO:
        if (level != ROLE_BOARD || m[5] != PROTOCOL_VERSION) {
            trace_sgi_l1_signal_reject("hello", seq);
            qemu_chr_fe_disconnect(&s->chr);
            return;
        }
        s->new_epoch = !s->peer_known || seq != s->peer_epoch;
        s->peer_epoch = seq;
        s->peer_known = true;
        break;
    case KIND_CHANGE:
        /* A signed difference, so the counter can wrap. */
        if ((int32_t)(seq - s->peer_seq) <= 0) {
            trace_sgi_l1_signal_reject("stale", seq);
        } else if (seq != s->peer_seq + 1) {
            trace_sgi_l1_signal_reject("gap", seq);
            qemu_chr_fe_disconnect(&s->chr);
        } else {
            s->peer_seq = seq;
            apply_line(s, m[5], m[7]);
        }
        break;
    case KIND_STATE: {
        unsigned changed = 0;
        int32_t d = seq - s->peer_seq;
        uint32_t missed = d > 0 ? d : 0;

        for (unsigned id = 1; id <= BOARD_LINES; id++) {
            if (extract32(level, id - 1, 1) != extract32(s->lines, id - 1, 1)) {
                apply_line(s, id, extract32(level, id - 1, 1));
                changed++;
            }
        }
        /* Changes the levels do not show, a pulse among them, are gone. */
        trace_sgi_l1_signal_resync(s->peer_epoch, changed,
                                   !s->new_epoch && missed > changed ?
                                   missed - changed : 0, s->new_epoch);
        s->peer_seq = seq;
        break;
    }
    }
}

static int signal_can_receive(void *opaque)
{
    return MSG_LEN;
}

static void signal_receive(void *opaque, const uint8_t *buf, int size)
{
    SGIL1SignalLinkState *s = opaque;

    /* A message that closes the link leaves the rest of the chunk unread. */
    for (int i = 0; i < size && s->up; i++) {
        s->rx[s->rx_len++] = buf[i];
        while (s->rx_len == MSG_LEN) {
            if (msg_valid(s->rx)) {
                s->rx_len = 0;
                handle_msg(s, s->rx);
                break;
            }
            trace_sgi_l1_signal_reject("junk", 0);
            memmove(s->rx, s->rx + 1, MSG_LEN - 1);
            s->rx_len--;
        }
    }
}

static void signal_event(void *opaque, QEMUChrEvent event)
{
    SGIL1SignalLinkState *s = opaque;

    if (event == CHR_EVENT_OPENED) {
        s->up = 1;
        s->rx_len = 0;
        trace_sgi_l1_signal_state(true);
        send_msg(s, KIND_HELLO, s->epoch, PROTOCOL_VERSION, ROLE_MACHINE);
        send_msg(s, KIND_STATE, s->seq, LINE_COUNT, s->lines & MACHINE_MASK);
    } else if (event == CHR_EVENT_CLOSED) {
        s->up = 0;
        trace_sgi_l1_signal_state(false);
    }
}

/* Only a change is a message; a level set to what it already is is not. */
static void set_machine_line(SGIL1SignalLinkState *s, unsigned id, int level)
{
    if (extract32(s->lines, id - 1, 1) == !!level) {
        return;
    }
    s->lines = deposit32(s->lines, id - 1, 1, !!level);
    s->seq++;
    if (s->up) {
        send_msg(s, KIND_CHANGE, s->seq, id, !!level);
    }
}

static void power_good_in(void *opaque, int n, int level)
{
    set_machine_line(opaque, LINE_POWER_GOOD, level);
}

static void node_reset_state_in(void *opaque, int n, int level)
{
    set_machine_line(opaque, LINE_NODE_RESET_STATE, level);
}

static void signal_realize(DeviceState *dev, Error **errp)
{
    SGIL1SignalLinkState *s = SGI_L1_SIGNAL_LINK(dev);

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        return;
    }
    /* The peer's state is outside the machine, so it is not saved. */
    error_setg(&s->blocker, "sgi-l1-signal-link: snapshots and migration are "
               "not supported: the peer's state is outside this process");
    if (migrate_add_blocker(&s->blocker, errp) < 0) {
        return;
    }
    qemu_chr_fe_set_handlers(&s->chr, signal_can_receive, signal_receive,
                             signal_event, NULL, s, NULL, true);
}

static void signal_unrealize(DeviceState *dev)
{
    SGIL1SignalLinkState *s = SGI_L1_SIGNAL_LINK(dev);

    qemu_chr_fe_set_handlers(&s->chr, NULL, NULL, NULL, NULL, NULL, NULL,
                             true);
    s->up = 0;
    if (s->blocker) {
        migrate_del_blocker(&s->blocker);
    }
}

static void signal_init(Object *obj)
{
    SGIL1SignalLinkState *s = SGI_L1_SIGNAL_LINK(obj);
    DeviceState *dev = DEVICE(obj);

    qdev_init_gpio_out_named(dev, &s->out[0], "node-power", 1);
    qdev_init_gpio_out_named(dev, &s->out[1], "reset-hold", 1);
    qdev_init_gpio_out_named(dev, &s->out[2], "soft-reset", 1);
    qdev_init_gpio_out_named(dev, &s->out[3], "nmi", 4);
    qdev_init_gpio_in_named(dev, power_good_in, "power-good", 1);
    qdev_init_gpio_in_named(dev, node_reset_state_in, "node-reset-state", 1);
    object_property_add_uint8_ptr(obj, "up", &s->up, OBJ_PROP_FLAG_READ);
    object_property_add_uint16_ptr(obj, "lines", &s->lines,
                                   OBJ_PROP_FLAG_READ);
}

static const Property signal_properties[] = {
    DEFINE_PROP_CHR("signals", SGIL1SignalLinkState, chr),
    DEFINE_PROP_UINT32("epoch", SGIL1SignalLinkState, epoch, 1),
};

static void signal_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "SGI L1 signal link to an external controller";
    dc->realize = signal_realize;
    dc->unrealize = signal_unrealize;
    dc->user_creatable = true;
    dc->hotpluggable = false;
    device_class_set_props(dc, signal_properties);
}

static const TypeInfo signal_types[] = {
    {
        .name = TYPE_SGI_L1_SIGNAL_LINK,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(SGIL1SignalLinkState),
        .instance_init = signal_init,
        .class_init = signal_class_init,
    },
};

DEFINE_TYPES(signal_types)
