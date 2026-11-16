/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Ycable chardev: a serial splitter. Guest output goes to every leg and
 * each leg's output reaches the guest, one read at a time. Line settings
 * (ioctls) go to every leg.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "chardev/char-serial.h"
#include "chardev-internal.h"

#define MAX_LEGS 4
/* Bytes held for the guest before the legs are told to wait. */
#define MAX_QUEUED 4096

typedef struct YcableChunk {
    GByteArray *data;
    size_t off;
} YcableChunk;

typedef struct YcableChardev {
    Chardev parent;
    CharFrontend legs[MAX_LEGS];
    int nlegs;
    GQueue chunks;
    size_t queued;
    bool flushing;
    /* Guest bytes a leg could not take without blocking. */
    uint64_t dropped[MAX_LEGS];
} YcableChardev;

DECLARE_INSTANCE_CHECKER(YcableChardev, YCABLE_CHARDEV, TYPE_CHARDEV_YCABLE)

/*
 * Never block the main loop on a leg: write while it takes bytes, then
 * drop the rest and count it. A hung responder must not freeze the VM.
 */
static int ycable_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    YcableChardev *y = YCABLE_CHARDEV(chr);

    for (int i = 0; i < y->nlegs; i++) {
        const uint8_t *p = buf;
        int left = len;

        while (left > 0) {
            int n = qemu_chr_fe_write(&y->legs[i], p, left);

            if (n <= 0) {
                break;
            }
            p += n;
            left -= n;
        }
        if (left > 0) {
            if (!y->dropped[i]) {
                warn_report("ycable: leg %d is not reading, dropping "
                            "guest output for it", i);
            }
            y->dropped[i] += left;
        }
    }
    return len;
}

static int ycable_chr_ioctl(Chardev *chr, int cmd, void *arg)
{
    YcableChardev *y = YCABLE_CHARDEV(chr);
    int ret = -ENOTSUP;

    for (int i = 0; i < y->nlegs; i++) {
        if (qemu_chr_fe_ioctl(&y->legs[i], cmd, arg) == 0) {
            ret = 0;
            /* A read has one answer: the first leg that gives one. */
            if (cmd == CHR_IOCTL_SERIAL_GET_TIOCM) {
                break;
            }
        }
    }
    return ret;
}

/*
 * A 16550 takes one byte at a time, so a read cannot be handed over whole.
 * Hold each leg read as a chunk and feed the guest one chunk at a time.
 */
static void ycable_flush(YcableChardev *y)
{
    Chardev *chr = CHARDEV(y);
    YcableChunk *c;
    bool drained = false;

    if (y->flushing) {
        return;
    }
    y->flushing = true;
    while ((c = g_queue_peek_head(&y->chunks))) {
        int n = qemu_chr_be_can_write(chr);
        size_t left = c->data->len - c->off;
        size_t at = c->off;

        if (n <= 0) {
            break;
        }
        n = MIN(left, n);
        c->off += n;
        y->queued -= n;
        drained = true;
        qemu_chr_be_write(chr, c->data->data + at, n);
        if (c->off == c->data->len) {
            g_queue_pop_head(&y->chunks);
            g_byte_array_free(c->data, true);
            g_free(c);
        }
    }
    y->flushing = false;
    if (drained) {
        for (int i = 0; i < y->nlegs; i++) {
            qemu_chr_fe_accept_input(&y->legs[i]);
        }
    }
}

static void ycable_chr_accept_input(Chardev *chr)
{
    ycable_flush(YCABLE_CHARDEV(chr));
}

static int ycable_can_read(void *opaque)
{
    YcableChardev *y = opaque;

    return y->queued < MAX_QUEUED ? MAX_QUEUED - y->queued : 0;
}

static void ycable_read(void *opaque, const uint8_t *buf, int size)
{
    YcableChardev *y = opaque;
    YcableChunk *c = g_new0(YcableChunk, 1);

    c->data = g_byte_array_sized_new(size);
    g_byte_array_append(c->data, buf, size);
    g_queue_push_tail(&y->chunks, c);
    y->queued += size;
    ycable_flush(y);
}

/* A leg's chardev can be gone at shutdown. */
static bool ycable_any_open(YcableChardev *y)
{
    for (int i = 0; i < y->nlegs; i++) {
        if (y->legs[i].chr && y->legs[i].chr->be_open) {
            return true;
        }
    }
    return false;
}

/* The line is open while any leg is. */
static void ycable_event(void *opaque, QEMUChrEvent event)
{
    YcableChardev *y = opaque;
    Chardev *chr = CHARDEV(y);
    bool open;

    if (event == CHR_EVENT_BREAK) {
        qemu_chr_be_event(chr, event);
        return;
    }
    if (event != CHR_EVENT_OPENED && event != CHR_EVENT_CLOSED) {
        return;
    }
    open = ycable_any_open(y);
    if (open != chr->be_open) {
        qemu_chr_be_event(chr, open ? CHR_EVENT_OPENED : CHR_EVENT_CLOSED);
    }
}

static void ycable_chr_update_read_handler(Chardev *chr)
{
    YcableChardev *y = YCABLE_CHARDEV(chr);

    for (int i = 0; i < y->nlegs; i++) {
        qemu_chr_fe_set_handlers_full(&y->legs[i], ycable_can_read,
                                      ycable_read, ycable_event, NULL, y,
                                      chr->gcontext, true, false);
    }
    chr->be_open = ycable_any_open(y);
}

static bool ycable_chr_open(Chardev *chr, ChardevBackend *backend,
                            Error **errp)
{
    YcableChardev *y = YCABLE_CHARDEV(chr);
    strList *list = backend->u.ycable.data->legs;

    if (!list) {
        error_setg(errp, "ycable: 'legs.0' is not defined");
        return false;
    }
    for (; list; list = list->next) {
        Chardev *s = qemu_chr_find(list->value);

        if (!s) {
            error_setg(errp, "ycable: chardev can't be found by id '%s'",
                       list->value);
            return false;
        }
        if (CHARDEV_IS_HUB(s) || CHARDEV_IS_MUX(s)) {
            error_setg(errp, "ycable: leg '%s' must not be a hub or have "
                       "'mux=on'", list->value);
            return false;
        }
        if (y->nlegs == MAX_LEGS) {
            error_setg(errp, "ycable: at most %d legs", MAX_LEGS);
            return false;
        }
        if (!qemu_chr_fe_init(&y->legs[y->nlegs], s, errp)) {
            return false;
        }
        y->nlegs++;
    }
    ycable_chr_update_read_handler(chr);
    return true;
}

static void ycable_chr_parse(QemuOpts *opts, ChardevBackend *backend,
                             Error **errp)
{
    ChardevYcable *yc;
    strList **tail;

    backend->type = CHARDEV_BACKEND_KIND_YCABLE;
    yc = backend->u.ycable.data = g_new0(ChardevYcable, 1);
    qemu_chr_parse_common(opts, qapi_ChardevYcable_base(yc));

    tail = &yc->legs;
    for (int i = 0; i < MAX_LEGS; i++) {
        g_autofree char *key = g_strdup_printf("legs.%d", i);
        const char *leg = qemu_opt_get(opts, key);

        if (!leg) {
            for (int j = i + 1; j < MAX_LEGS; j++) {
                g_autofree char *later = g_strdup_printf("legs.%d", j);

                if (qemu_opt_get(opts, later)) {
                    error_setg(errp, "ycable: '%s' is set but '%s' is not",
                               later, key);
                    return;
                }
            }
            break;
        }
        QAPI_LIST_APPEND(tail, g_strdup(leg));
    }
}

static void ycable_chr_finalize(Object *obj)
{
    YcableChardev *y = YCABLE_CHARDEV(obj);
    YcableChunk *c;

    for (int i = 0; i < y->nlegs; i++) {
        qemu_chr_fe_deinit(&y->legs[i], false);
    }
    while ((c = g_queue_pop_head(&y->chunks))) {
        g_byte_array_free(c->data, true);
        g_free(c);
    }
}

static void ycable_chr_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_parse = ycable_chr_parse;
    cc->chr_open = ycable_chr_open;
    cc->chr_write = ycable_chr_write;
    cc->chr_ioctl = ycable_chr_ioctl;
    cc->chr_accept_input = ycable_chr_accept_input;
    cc->chr_update_read_handler = ycable_chr_update_read_handler;
}

static const TypeInfo ycable_chr_type_info = {
    .name = TYPE_CHARDEV_YCABLE,
    .parent = TYPE_CHARDEV,
    .class_init = ycable_chr_class_init,
    .instance_size = sizeof(YcableChardev),
    .instance_finalize = ycable_chr_finalize,
};

static void register_types(void)
{
    type_register_static(&ycable_chr_type_info);
}

type_init(register_types);
