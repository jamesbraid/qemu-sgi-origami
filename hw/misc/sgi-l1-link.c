/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * An L1 outside this process, reached over a chardev per UART window, and the
 * signal link that carries its lines. With no peer the controller is silent:
 * the node's bytes are dropped, never kept.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "exec/icount.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/blocker.h"
#include "hw/misc/sgi-l1.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(SGIL1LinkState, SGI_L1_LINK)

/* A socket holds about 280 one-byte writes; a frame burst must outlast that. */
#define LINK_TXQ 16384

typedef struct SGIL1LinkNode {
    SGIL1LinkState *link;
    unsigned transport;
    CharFrontend chr;
    SGIL1UARTState *window;
    /* Bytes the chardev has not taken yet, and the watch that flushes them. */
    uint8_t txq[LINK_TXQ];
    unsigned tx_head, tx_len;
    guint tx_watch;
    bool tx_full_logged;
} SGIL1LinkNode;

struct SGIL1LinkState {
    DeviceState parent_obj;
    SGIL1LinkNode node[SGI_L1_IROUTER_TRANSPORTS];
    /* Bit n: transport n has a peer connected. */
    uint8_t nodes_up;
    DeviceState *signal_link;
    Error *blocker;
};

static int link_can_receive(void *opaque)
{
    SGIL1LinkNode *n = opaque;

    return n->window ? sgi_l1_uart_room(n->window) : 0;
}

static void link_receive(void *opaque, const uint8_t *buf, int size)
{
    SGIL1LinkNode *n = opaque;

    trace_sgi_l1_link_rx(n->transport, size);
    sgi_l1_uart_queue(n->window, buf, size);
}

static void link_tx_clear(SGIL1LinkNode *n)
{
    n->tx_head = n->tx_len = 0;
    n->tx_full_logged = false;
    if (n->tx_watch) {
        g_source_remove(n->tx_watch);
        n->tx_watch = 0;
    }
}

static gboolean link_writable(void *chr, GIOCondition cond, void *opaque);

/* Give the chardev what it takes; wait until it is writable for the rest. */
static void link_flush(SGIL1LinkNode *n)
{
    while (n->tx_len) {
        int done = qemu_chr_fe_write(&n->chr, n->txq + n->tx_head,
                                     MIN(n->tx_len, LINK_TXQ - n->tx_head));

        if (done <= 0) {
            break;
        }
        n->tx_head = (n->tx_head + done) % LINK_TXQ;
        n->tx_len -= done;
    }
    if (!n->tx_len) {
        n->tx_full_logged = false;
    } else if (!n->tx_watch) {
        n->tx_watch = qemu_chr_fe_add_watch(&n->chr, G_IO_OUT | G_IO_HUP,
                                            link_writable, n);
        if (!n->tx_watch) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-l1-link: transport %u "
                          "backend cannot wait, %u bytes dropped\n",
                          n->transport, n->tx_len);
            n->tx_len = 0;
        }
    }
}

static gboolean link_writable(void *chr, GIOCondition cond, void *opaque)
{
    SGIL1LinkNode *n = opaque;

    n->tx_watch = 0;
    link_flush(n);
    return G_SOURCE_REMOVE;
}

static void link_event(void *opaque, QEMUChrEvent event)
{
    SGIL1LinkNode *n = opaque;

    if (event == CHR_EVENT_OPENED) {
        n->link->nodes_up |= 1 << n->transport;
    } else if (event == CHR_EVENT_CLOSED) {
        n->link->nodes_up &= ~(1 << n->transport);
        link_tx_clear(n);
    } else {
        return;
    }
    trace_sgi_l1_link_state(n->transport, event == CHR_EVENT_OPENED);
}

static bool link_attach_window(SGIL1Provider *p, unsigned transport,
                               SGIL1UARTState *window, Error **errp)
{
    SGIL1LinkNode *n = &SGI_L1_LINK(p)->node[transport];

    if (n->window) {
        error_setg(errp, "sgi-l1-link: transport %u already has a UART window",
                   transport);
        return false;
    }
    n->window = window;
    return true;
}

static void link_node_byte(SGIL1Provider *p, unsigned transport, uint8_t byte)
{
    SGIL1LinkState *s = SGI_L1_LINK(p);
    SGIL1LinkNode *n = &s->node[transport];
    bool sent = (s->nodes_up & (1 << transport)) && n->tx_len < LINK_TXQ;

    if (sent) {
        n->txq[(n->tx_head + n->tx_len++) % LINK_TXQ] = byte;
        if (!n->tx_watch) {
            link_flush(n);
        }
    } else if ((s->nodes_up & (1 << transport)) && !n->tx_full_logged) {
        n->tx_full_logged = true;
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-l1-link: transport %u transmit "
                      "queue full, bytes dropped until it drains\n",
                      transport);
    }
    trace_sgi_l1_link_tx(transport, byte, sent);
}

static void link_node_room(SGIL1Provider *p, unsigned transport)
{
    qemu_chr_fe_accept_input(&SGI_L1_LINK(p)->node[transport].chr);
}

static bool signals_connected(SGIL1LinkState *s)
{
    g_autofree char *id = object_property_get_str(OBJECT(s->signal_link),
                                                  "signals", &error_abort);

    return *id;
}

static void link_realize(DeviceState *dev, Error **errp)
{
    SGIL1LinkState *s = SGI_L1_LINK(dev);
    bool linked = false;

    for (unsigned i = 0; i < SGI_L1_IROUTER_TRANSPORTS; i++) {
        linked |= qemu_chr_fe_backend_connected(&s->node[i].chr);
    }
    if (linked != signals_connected(s)) {
        error_setg(errp, "sgi-l1-link: a node chardev and a signals chardev "
                   "must be given together");
        return;
    }
    if (linked && icount_enabled()) {
        /* A device cannot see -icount's sleep or align, so allow none. */
        error_setg(errp, "sgi-l1-link: -icount is not allowed with an "
                   "external controller: the machine's virtual time must not "
                   "run ahead of a separate controller process, whose board "
                   "uses its own fixed -icount");
        return;
    }
    /* Realized even when inert: a machine wires its lines either way. */
    if (!qdev_realize(s->signal_link, NULL, errp) || !linked) {
        return;
    }
    /* The peer's state is outside the machine, so it is not saved. */
    error_setg(&s->blocker, "sgi-l1-link: snapshots and migration are not "
               "supported with firmware L1 controllers: the controller "
               "processes and the bytes in flight between them are outside "
               "this machine");
    if (migrate_add_blocker(&s->blocker, errp) < 0) {
        qdev_unrealize(s->signal_link);
        return;
    }
    for (unsigned i = 0; i < SGI_L1_IROUTER_TRANSPORTS; i++) {
        qemu_chr_fe_set_handlers(&s->node[i].chr, link_can_receive,
                                 link_receive, link_event, NULL, &s->node[i],
                                 NULL, true);
    }
}

static void link_unrealize(DeviceState *dev)
{
    SGIL1LinkState *s = SGI_L1_LINK(dev);

    for (unsigned i = 0; i < SGI_L1_IROUTER_TRANSPORTS; i++) {
        qemu_chr_fe_set_handlers(&s->node[i].chr, NULL, NULL, NULL, NULL,
                                 NULL, NULL, true);
        link_tx_clear(&s->node[i]);
    }
    s->nodes_up = 0;
    if (s->blocker) {
        migrate_del_blocker(&s->blocker);
    }
    qdev_unrealize(s->signal_link);
}

static const char *const signal_gpios[] = {
    "node-power", "reset-hold", "soft-reset", "nmi", "power-good",
    "node-reset-state",
};

static void link_init(Object *obj)
{
    SGIL1LinkState *s = SGI_L1_LINK(obj);

    for (unsigned i = 0; i < SGI_L1_IROUTER_TRANSPORTS; i++) {
        s->node[i].link = s;
        s->node[i].transport = i;
    }
    object_property_add_uint8_ptr(obj, "nodes-up", &s->nodes_up,
                                  OBJ_PROP_FLAG_READ);

    /* The board's lines come out under the names sgi-l1 gives its own. */
    s->signal_link = qdev_new("sgi-l1-signal-link");
    s->signal_link->id = g_strdup("signal-link");
    object_property_add_child(obj, "signal-link", OBJECT(s->signal_link));
    object_unref(OBJECT(s->signal_link));
    object_property_add_alias(obj, "signals", OBJECT(s->signal_link),
                              "signals");
    object_property_add_alias(obj, "epoch", OBJECT(s->signal_link), "epoch");
    for (unsigned i = 0; i < ARRAY_SIZE(signal_gpios); i++) {
        qdev_pass_gpios(s->signal_link, DEVICE(obj), signal_gpios[i]);
    }
}

static const Property link_properties[] = {
    DEFINE_PROP_CHR("node0", SGIL1LinkState, node[0].chr),
    DEFINE_PROP_CHR("node1", SGIL1LinkState, node[1].chr),
    DEFINE_PROP_CHR("node2", SGIL1LinkState, node[2].chr),
    DEFINE_PROP_CHR("node3", SGIL1LinkState, node[3].chr),
};

static void link_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SGIL1ProviderClass *pc = SGI_L1_PROVIDER_CLASS(klass);

    dc->desc = "SGI L1 node link to an external controller";
    dc->realize = link_realize;
    dc->unrealize = link_unrealize;
    dc->user_creatable = true;
    dc->hotpluggable = false;
    device_class_set_props(dc, link_properties);
    pc->attach_window = link_attach_window;
    pc->node_byte = link_node_byte;
    pc->node_room = link_node_room;
}

static const TypeInfo link_types[] = {
    {
        .name = TYPE_SGI_L1_LINK,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(SGIL1LinkState),
        .instance_init = link_init,
        .class_init = link_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_SGI_L1_PROVIDER },
            { }
        },
    },
};

DEFINE_TYPES(link_types)
