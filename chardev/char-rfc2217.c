/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * RFC 2217 chardev: carries a serial line over a socket chardev with the
 * Telnet Com Port Control option, so a process on the other end sees the
 * baud rate, framing, break and DTR/RTS the guest sets.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "chardev/char-serial.h"
#include "chardev-internal.h"

#define IAC             255
#define SB              250
#define SE              240
#define WILL            251
#define WONT            252
#define DO              253
#define DONT            254
#define BINARY          0
#define COM_PORT        44

/* Client to server commands, RFC 2217 section 2. */
#define SET_BAUDRATE    1
#define SET_DATASIZE    2
#define SET_PARITY      3
#define SET_STOPSIZE    4
#define SET_CONTROL     5
#define CTL_BREAK_ON    5
#define CTL_BREAK_OFF   6
#define CTL_DTR_ON      8
#define CTL_DTR_OFF     9
#define CTL_RTS_ON      11
#define CTL_RTS_OFF     12

typedef enum { RX_DATA, RX_IAC, RX_OPT, RX_SB, RX_SB_IAC } RxState;

typedef struct Rfc2217Chardev {
    Chardev parent;
    CharFrontend sock;
    RxState rx;
    bool have_params;
    QEMUSerialSetParams params;
    uint8_t optcmd;
    int tiocm;
    bool brk;
    /* Bytes the socket could not take without blocking. */
    uint64_t dropped;
} Rfc2217Chardev;

DECLARE_INSTANCE_CHECKER(Rfc2217Chardev, RFC2217_CHARDEV, TYPE_CHARDEV_RFC2217)

/*
 * Never block the main loop on the socket: write while it takes bytes, then
 * drop the rest and count it. A hung server must not freeze the VM. A dropped
 * command can leave the stream mid-subnegotiation until the next reconnect.
 */
static void rfc_write(Rfc2217Chardev *r, const uint8_t *buf, int len)
{
    while (len > 0) {
        int n = qemu_chr_fe_write(&r->sock, buf, len);

        if (n <= 0) {
            break;
        }
        buf += n;
        len -= n;
    }
    if (len > 0) {
        if (!r->dropped) {
            warn_report("rfc2217: socket is not accepting data, dropping "
                        "guest output");
        }
        r->dropped += len;
    }
}

static void rfc_send(Rfc2217Chardev *r, const uint8_t *buf, int len)
{
    g_autofree uint8_t *out = g_malloc(2 * len);
    int n = 0;

    for (int i = 0; i < len; i++) {
        out[n++] = buf[i];
        if (buf[i] == IAC) {
            out[n++] = IAC;
        }
    }
    rfc_write(r, out, n);
}

/* IAC SB COM-PORT cmd value IAC SE, built whole; 0xff in value doubled. */
static void rfc_command(Rfc2217Chardev *r, uint8_t cmd, const uint8_t *val,
                        int len)
{
    uint8_t out[4 + 2 * 4 + 2] = { IAC, SB, COM_PORT, cmd };
    int n = 4;

    assert(len <= 4);
    for (int i = 0; i < len; i++) {
        out[n++] = val[i];
        if (val[i] == IAC) {
            out[n++] = IAC;
        }
    }
    out[n++] = IAC;
    out[n++] = SE;
    rfc_write(r, out, n);
}

static void rfc_control(Rfc2217Chardev *r, uint8_t code)
{
    rfc_command(r, SET_CONTROL, &code, 1);
}

/* In the order RFC 2217 recommends: baud rate, data size, parity, stops. */
static void rfc_send_params(Rfc2217Chardev *r)
{
    const QEMUSerialSetParams *p = &r->params;
    uint8_t baud[4] = { p->speed >> 24, p->speed >> 16, p->speed >> 8,
                        p->speed };
    uint8_t size = p->data_bits;
    uint8_t parity = p->parity == 'O' ? 2 : p->parity == 'E' ? 3 : 1;
    uint8_t stop = p->stop_bits;

    rfc_command(r, SET_BAUDRATE, baud, 4);
    rfc_command(r, SET_DATASIZE, &size, 1);
    rfc_command(r, SET_PARITY, &parity, 1);
    rfc_command(r, SET_STOPSIZE, &stop, 1);
}

static void rfc_send_state(Rfc2217Chardev *r)
{
    /*
     * Binary both ways, so a server on a plain network terminal does not
     * rewrite CR; then the Com Port option.
     */
    const uint8_t hello[] = { IAC, WILL, COM_PORT, IAC, WILL, BINARY,
                              IAC, DO, BINARY };

    rfc_write(r, hello, sizeof(hello));
    if (r->have_params) {
        rfc_send_params(r);
    }
    rfc_control(r, r->tiocm & CHR_TIOCM_DTR ? CTL_DTR_ON : CTL_DTR_OFF);
    rfc_control(r, r->tiocm & CHR_TIOCM_RTS ? CTL_RTS_ON : CTL_RTS_OFF);
    rfc_control(r, r->brk ? CTL_BREAK_ON : CTL_BREAK_OFF);
}

static int rfc2217_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    rfc_send(RFC2217_CHARDEV(chr), buf, len);
    return len;
}

static int rfc2217_chr_ioctl(Chardev *chr, int cmd, void *arg)
{
    Rfc2217Chardev *r = RFC2217_CHARDEV(chr);
    int flags;

    switch (cmd) {
    case CHR_IOCTL_SERIAL_SET_PARAMS:
        r->params = *(QEMUSerialSetParams *)arg;
        r->have_params = true;
        if (chr->be_open) {
            rfc_send_params(r);
        }
        return 0;
    case CHR_IOCTL_SERIAL_SET_BREAK:
        r->brk = *(int *)arg;
        if (chr->be_open) {
            rfc_control(r, r->brk ? CTL_BREAK_ON : CTL_BREAK_OFF);
        }
        return 0;
    case CHR_IOCTL_SERIAL_SET_TIOCM:
        flags = *(int *)arg & (CHR_TIOCM_DTR | CHR_TIOCM_RTS);
        if (chr->be_open) {
            if ((flags ^ r->tiocm) & CHR_TIOCM_DTR) {
                rfc_control(r, flags & CHR_TIOCM_DTR ? CTL_DTR_ON
                                                     : CTL_DTR_OFF);
            }
            if ((flags ^ r->tiocm) & CHR_TIOCM_RTS) {
                rfc_control(r, flags & CHR_TIOCM_RTS ? CTL_RTS_ON
                                                     : CTL_RTS_OFF);
            }
        }
        r->tiocm = flags;
        return 0;
    case CHR_IOCTL_SERIAL_GET_TIOCM:
        /*
         * Inputs are not carried back: read as a device that asserts them.
         * Answering also keeps the UART passing DTR and RTS on.
         */
        *(int *)arg = CHR_TIOCM_CTS | CHR_TIOCM_DSR | CHR_TIOCM_CAR;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static void rfc2217_chr_accept_input(Chardev *chr)
{
    qemu_chr_fe_accept_input(&RFC2217_CHARDEV(chr)->sock);
}

static int rfc2217_can_read(void *opaque)
{
    return qemu_chr_be_can_write(CHARDEV(opaque));
}

/*
 * We offered Com Port and binary above, so a DO or WILL for those is the
 * answer and needs no reply. Refuse every other option, which also ends the
 * exchange: a WONT or DONT is never answered.
 */
static void rfc_negotiate(Rfc2217Chardev *r, uint8_t cmd, uint8_t opt)
{
    uint8_t reply[3] = { IAC, 0, opt };

    if (opt == BINARY || opt == COM_PORT) {
        if (cmd == WILL && opt == COM_PORT) {
            reply[1] = DO;
            rfc_write(r, reply, 3);
        }
        return;
    }
    if (cmd == DO || cmd == WILL) {
        reply[1] = cmd == DO ? WONT : DONT;
        rfc_write(r, reply, 3);
    }
}

/* Pass data through; drop Telnet commands and the server's option traffic. */
static void rfc2217_read(void *opaque, const uint8_t *buf, int size)
{
    Rfc2217Chardev *r = opaque;
    g_autofree uint8_t *out = g_malloc(size);
    int n = 0;

    for (int i = 0; i < size; i++) {
        uint8_t c = buf[i];

        switch (r->rx) {
        case RX_DATA:
            if (c == IAC) {
                r->rx = RX_IAC;
            } else {
                out[n++] = c;
            }
            break;
        case RX_IAC:
            if (c == IAC) {
                out[n++] = IAC;
                r->rx = RX_DATA;
            } else if (c == SB) {
                r->rx = RX_SB;
            } else if (c >= WILL && c <= DONT) {
                r->optcmd = c;
                r->rx = RX_OPT;
            } else {
                r->rx = RX_DATA;
            }
            break;
        case RX_OPT:
            rfc_negotiate(r, r->optcmd, c);
            r->rx = RX_DATA;
            break;
        case RX_SB:
            if (c == IAC) {
                r->rx = RX_SB_IAC;
            }
            break;
        case RX_SB_IAC:
            r->rx = c == SE ? RX_DATA : RX_SB;
            break;
        }
    }
    if (n) {
        qemu_chr_be_write(CHARDEV(r), out, n);
    }
}

static void rfc2217_event(void *opaque, QEMUChrEvent event)
{
    Rfc2217Chardev *r = opaque;

    if (event == CHR_EVENT_OPENED) {
        r->rx = RX_DATA;
        rfc_send_state(r);
    } else if (event != CHR_EVENT_CLOSED) {
        return;
    }
    qemu_chr_be_event(CHARDEV(r), event);
}

static bool rfc2217_chr_open(Chardev *chr, ChardevBackend *backend,
                             Error **errp)
{
    Rfc2217Chardev *r = RFC2217_CHARDEV(chr);
    const char *id = backend->u.rfc2217.data->chardev;
    Chardev *s = qemu_chr_find(id);

    if (!s) {
        error_setg(errp, "rfc2217: chardev can't be found by id '%s'", id);
        return false;
    }
    if (CHARDEV_IS_HUB(s) || CHARDEV_IS_MUX(s)) {
        error_setg(errp, "rfc2217: '%s' must not be a hub or have 'mux=on'",
                   id);
        return false;
    }
    if (!qemu_chr_fe_init(&r->sock, s, errp)) {
        return false;
    }
    qemu_chr_fe_set_handlers_full(&r->sock, rfc2217_can_read, rfc2217_read,
                                  rfc2217_event, NULL, r, chr->gcontext,
                                  true, false);
    chr->be_open = s->be_open;
    if (s->be_open) {
        rfc_send_state(r);
    }
    return true;
}

static void rfc2217_chr_parse(QemuOpts *opts, ChardevBackend *backend,
                              Error **errp)
{
    const char *chardev = qemu_opt_get(opts, "chardev");
    ChardevRfc2217 *rfc;

    if (!chardev) {
        error_setg(errp, "chardev: rfc2217: no chardev given");
        return;
    }
    backend->type = CHARDEV_BACKEND_KIND_RFC2217;
    rfc = backend->u.rfc2217.data = g_new0(ChardevRfc2217, 1);
    qemu_chr_parse_common(opts, qapi_ChardevRfc2217_base(rfc));
    rfc->chardev = g_strdup(chardev);
}

static void rfc2217_chr_finalize(Object *obj)
{
    qemu_chr_fe_deinit(&RFC2217_CHARDEV(obj)->sock, false);
}

static void rfc2217_chr_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_parse = rfc2217_chr_parse;
    cc->chr_open = rfc2217_chr_open;
    cc->chr_write = rfc2217_chr_write;
    cc->chr_ioctl = rfc2217_chr_ioctl;
    cc->chr_accept_input = rfc2217_chr_accept_input;
}

static const TypeInfo rfc2217_chr_type_info = {
    .name = TYPE_CHARDEV_RFC2217,
    .parent = TYPE_CHARDEV,
    .class_init = rfc2217_chr_class_init,
    .instance_size = sizeof(Rfc2217Chardev),
    .instance_finalize = rfc2217_chr_finalize,
};

static void register_types(void)
{
    type_register_static(&rfc2217_chr_type_info);
}

type_init(register_types);
