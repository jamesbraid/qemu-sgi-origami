/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Wacom IV UD-series serial tablet, as a chardev backend.
 *
 * Model: a UD-1212-R, ROM 1.3. It talks the WACOM IV command set and sends
 * the ROM 1.2-and-later binary packet of the WTC-1002 software interface
 * manual (section 4.1). Unlike the Penpartner model in chardev/wctablet.c
 * it follows what the manual page infers is the UD power-up state from
 * Appendix C.2: suppressed mode with a zero increment, transmitting without
 * being told to start. The manual page
 * docs/system/mips/sgi/hardware/wacom-iv.rst lists the commands, the packet
 * fields and the approximations.
 *
 * Use it as "-chardev wacom-iv,id=tablet" behind a serial port. It needs no
 * option parser: a chardev class without chr_parse gets a plain common
 * backend from qemu_chr_parse_opts().
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "chardev/char.h"
#include "chardev/char-serial.h"
#include "ui/input.h"
#include "qom/object.h"
#include "trace.h"

#define WACOM_IV_TYPE_NAME      "chardev-wacom-iv"

/*
 * ~C answer for a UD-1212-R: the highest coordinate at 0.02 mm, which
 * converts to 1270 lpi. Packets carry coordinates at the logical resolution
 * NR sets, at most 2540 lpi on ROM 1.2 and later (WTC-1002 p. 11).
 */
#define WACOM_IV_MAX_X          15240
#define WACOM_IV_MAX_Y          15240
#define WACOM_IV_MAX_RES        2540

#define WACOM_IV_OUTBUF_LEN     512
#define WACOM_IV_QUERY_LEN      100

/* "~#" answer; the manual gives the form, the ROM level is dlwacom's. */
static const char wacom_iv_model_string[] = "~#UD-1212-R00 V1.3-5\r";

/*
 * ~R answer at power up and after RE. Appendix C.2 gives this as the UD
 * series' only ~R response without marking it as the power-up one, so the
 * identification is inferred (see the manual page).
 */
#define WACOM_IV_DEFAULT_BODY   0xE203C000u
#define WACOM_IV_DEFAULT_TT     2
#define WACOM_IV_DEFAULT_RES    1270

/* ~R answer after the $ reset to WACOM II-S, Appendix C.4. */
#define WACOM_IV_II_S_BODY      0xA21BC800u

/* Pressure is a signed byte; the manual's "no pressure" is about -120. */
#define WACOM_IV_PRESSURE_UP    (-120)
#define WACOM_IV_PRESSURE_DOWN  120

/* Time to send one 7-byte packet: 7 bytes of 10 bits (start, 8, stop). */
#define WACOM_IV_PACKET_BITS    70
/* IT counts in units of 5 ms. */
#define WACOM_IV_IT_UNIT_NS     (5 * SCALE_MS)

struct WacomIvChardev {
    Chardev parent;
    QemuInputHandlerState *hs;
    QEMUTimer *timer;

    uint8_t query[WACOM_IV_QUERY_LEN];
    int query_index;

    uint8_t outbuf[WACOM_IV_OUTBUF_LEN];
    int outlen;

    /* The host side of the line, from the frontend's SET_PARAMS. */
    int speed;
    int parity;
    int data_bits;
    int stop_bits;

    /* A command has arrived on a correct line; SP stops transmission. */
    bool talked;
    bool stopped;
    /* RQ or @ was sent: transmit only when asked, until ST. */
    bool remote;

    int axis[INPUT_AXIS__MAX];
    bool btns[INPUT_BUTTON__MAX];

    /* The current Setting, as ~R shows it. */
    uint32_t body;
    int tt;
    int xres, yres;
    /* What the tablet obeys: SU's increment, IT's interval in 5 ms units. */
    int increment;
    int interval;

    /* Last packet sent, for suppression and pacing. */
    bool have_last;
    int last_x, last_y, last_switch;
    int64_t last_tx_ns;
    bool pending;
};
typedef struct WacomIvChardev WacomIvChardev;

DECLARE_INSTANCE_CHECKER(WacomIvChardev, WACOM_IV_CHARDEV, WACOM_IV_TYPE_NAME)

static void wacom_iv_accept_input(Chardev *chr);

/*
 * The tablet's own line comes from the Setting body (WTC-1002 Appendix B,
 * where bit 0 is the most significant): bits 2-3 the baud rate, 4-5 the
 * parity, 6 the data length and 7 the stop bits. ~* moves it, as the stock
 * IRIX module does to switch to 19200 baud; RE puts back the factory 9600
 * 8N1. The tablet hears and is heard only when the host side matches.
 */
static bool wacom_iv_line_ok(const WacomIvChardev *w)
{
    static const int speeds[] = { 2400, 4800, 9600, 19200 };
    static const char parities[] = { 'N', 'N', 'O', 'E' };
    uint32_t b = w->body;

    return w->speed == speeds[(b >> 28) & 3] &&
           w->parity == parities[(b >> 26) & 3] &&
           w->data_bits == ((b & (1u << 25)) ? 8 : 7) &&
           w->stop_bits == ((b & (1u << 24)) ? 2 : 1);
}

static void wacom_iv_queue_output(WacomIvChardev *w, const uint8_t *buf,
                                  int count)
{
    if (w->outlen + count > sizeof(w->outbuf)) {
        return;
    }
    memcpy(w->outbuf + w->outlen, buf, count);
    w->outlen += count;
    wacom_iv_accept_input(CHARDEV(w));
}

/* Power-up and RE state: suppressed, SU0, maximum rate, transmitting. */
static void wacom_iv_reset_settings(WacomIvChardev *w)
{
    w->body = WACOM_IV_DEFAULT_BODY;
    w->tt = WACOM_IV_DEFAULT_TT;
    w->xres = w->yres = WACOM_IV_DEFAULT_RES;
    w->increment = 0;
    w->interval = 0;
    w->stopped = false;
    w->remote = false;
    w->have_last = false;
    w->pending = false;
    timer_del(w->timer);
}

/*
 * The global resets: # to the WACOM IV defaults, $ to the WACOM II-S ones.
 * Both are 9600 baud 8N1 (Appendix C.2 and C.4). WACOM II-S itself is not
 * modelled: after $ the tablet answers queries but sends no coordinates
 * until a Setting selects WACOM IV again.
 */
static void wacom_iv_global_reset(WacomIvChardev *w, char which)
{
    char name[2] = { which, 0 };

    trace_wacom_iv_cmd(name);
    wacom_iv_reset_settings(w);
    if (which == '$') {
        w->body = WACOM_IV_II_S_BODY;
        w->tt = 0;
    }
}

/* Coordinates go out only under the WACOM IV command set (body bits 0-1). */
static bool wacom_iv_command_set_iv(const WacomIvChardev *w)
{
    return (w->body >> 30) == 3;
}

/*
 * 0..32767 console axis onto the tablet's 0..max coordinate, where max is
 * the ~C size at 1270 lpi rescaled to the axis' logical resolution.
 */
static int wacom_iv_scale(int value, int max_1270, int res)
{
    int64_t max = (int64_t)max_1270 * res / WACOM_IV_DEFAULT_RES;

    return (int64_t)value * max / INPUT_EVENT_ABS_MAX;
}

static int wacom_iv_switch_number(const WacomIvChardev *w)
{
    /*
     * Left is the tip switch, middle the first side switch, right the
     * second. WTC-1002 chapter 5 (p.39) gives switch numbers 4 and 5; 3,
     * tip plus first side switch, is the pre-ROM-1.2 value on p.34. The
     * mapping is the model's choice. A stylus has no number for both side
     * switches, so right wins over middle.
     */
    bool tip = w->btns[INPUT_BUTTON_LEFT];

    if (w->btns[INPUT_BUTTON_RIGHT]) {
        return tip ? 5 : 4;
    }
    if (w->btns[INPUT_BUTTON_MIDDLE]) {
        return tip ? 3 : 2;
    }
    return tip ? 1 : 0;
}

static void wacom_iv_send_packet(WacomIvChardev *w, int x, int y, int sw)
{
    uint8_t p[7];
    int pressure = w->btns[INPUT_BUTTON_LEFT] ? WACOM_IV_PRESSURE_DOWN
                                              : WACOM_IV_PRESSURE_UP;
    uint8_t pbits = pressure & 0xff;

    /* Byte 1: sync, proximity, stylus, button flag, X15..X14. */
    p[0] = 0xe0 | (sw ? 0x08 : 0) | (x >> 14);
    p[1] = (x >> 7) & 0x7f;
    p[2] = x & 0x7f;
    /* Byte 4: B3..B0, P0, Y15..Y14. */
    p[3] = (sw << 3) | ((pbits & 1) << 2) | (y >> 14);
    p[4] = (y >> 7) & 0x7f;
    p[5] = y & 0x7f;
    /* Byte 7: Sp, P6..P1 (two's complement across Sp and P6..P0). */
    p[6] = ((pbits >> 7) << 6) | ((pbits >> 1) & 0x3f);

    trace_wacom_iv_packet(x, y, sw, pressure);
    wacom_iv_queue_output(w, p, sizeof(p));
}

static int64_t wacom_iv_interval_ns(const WacomIvChardev *w)
{
    /* The line carries one packet per 70 bit times, whatever IT asks. */
    int64_t line_ns = (int64_t)WACOM_IV_PACKET_BITS * NANOSECONDS_PER_SECOND /
                      w->speed;
    int64_t asked = (int64_t)w->interval * WACOM_IV_IT_UNIT_NS;

    return MAX(line_ns, asked);
}

/*
 * Send a packet if the suppression rules want one and the interval since
 * the last has passed. A wanted packet that cannot go yet is remembered
 * and retried by the timer or when the frontend drains the queue.
 */
static void wacom_iv_try_send(WacomIvChardev *w)
{
    int x = wacom_iv_scale(w->axis[INPUT_AXIS_X], WACOM_IV_MAX_X, w->xres);
    int y = wacom_iv_scale(w->axis[INPUT_AXIS_Y], WACOM_IV_MAX_Y, w->yres);
    int sw = wacom_iv_switch_number(w);
    int64_t now, due;

    if (!w->pending) {
        if (w->remote) {
            return;
        }
        if (w->have_last &&
            sw == w->last_switch &&
            abs(x - w->last_x) <= w->increment &&
            abs(y - w->last_y) <= w->increment) {
            return;
        }
        w->pending = true;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    due = w->last_tx_ns + wacom_iv_interval_ns(w);
    if (w->have_last && now < due) {
        timer_mod(w->timer, due);
        return;
    }
    if (w->outlen) {
        /* The guest has not drained the last packet yet. */
        return;
    }

    w->pending = false;
    w->have_last = true;
    w->last_x = x;
    w->last_y = y;
    w->last_switch = sw;
    w->last_tx_ns = now;
    wacom_iv_send_packet(w, x, y, sw);
}

static void wacom_iv_timer_cb(void *opaque)
{
    wacom_iv_try_send(opaque);
}

static void wacom_iv_input_event(DeviceState *dev, QemuConsole *src,
                                 QemuInputEvent *evt)
{
    WacomIvChardev *w = (WacomIvChardev *)dev;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        w->axis[evt->abs.axis] = evt->abs.value;
        break;
    case INPUT_EVENT_KIND_BTN:
        w->btns[evt->btn.button] = evt->btn.down;
        break;
    default:
        break;
    }
}

static void wacom_iv_input_sync(DeviceState *dev)
{
    wacom_iv_try_send((WacomIvChardev *)dev);
}

static const QemuInputHandler wacom_iv_handler = {
    .name  = "QEMU Wacom IV Tablet",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = wacom_iv_input_event,
    .sync  = wacom_iv_input_sync,
};

/*
 * Own the console's absolute pointer only while a driver has set the line
 * and spoken to the tablet, and release it on SP. A tablet that claimed it
 * at creation would starve PS/2 of mouse events whenever no guest driver
 * had the port open. Activating on every call keeps this handler ahead of
 * any relative pointer regardless of creation order.
 */
static void wacom_iv_update_pointer(WacomIvChardev *w)
{
    bool want = wacom_iv_line_ok(w) && wacom_iv_command_set_iv(w) &&
                w->talked && !w->stopped;

    if (want) {
        if (!w->hs) {
            w->hs = qemu_input_handler_register((DeviceState *)w,
                                                &wacom_iv_handler);
        }
        qemu_input_handler_activate(w->hs);
    } else {
        if (w->hs) {
            qemu_input_handler_unregister(w->hs);
            w->hs = NULL;
        }
        w->pending = false;
        timer_del(w->timer);
    }
}

static void wacom_iv_shift_input(WacomIvChardev *w, int count)
{
    w->query_index -= count;
    memmove(w->query, w->query + count, w->query_index);
    w->query[w->query_index] = 0;
}

static void wacom_iv_reply_setting(WacomIvChardev *w, const char *prefix,
                                   uint32_t body, int inc, int tt, int xres,
                                   int yres)
{
    char buf[48];
    int len = snprintf(buf, sizeof(buf), "~R%s%08X,%03d,%02d,%04d,%04d\r",
                       prefix, body, inc, tt, xres, yres);

    wacom_iv_queue_output(w, (uint8_t *)buf, len);
}

/*
 * A logical resolution the model takes, from NR or a Setting rewrite. The
 * manual gives 2540 as the maximum for ROM 1.2 and later and no outcome
 * above it. Refusing 0 and below is the model's choice.
 */
static bool wacom_iv_res_ok(int res)
{
    return res >= 1 && res <= WACOM_IV_MAX_RES;
}

/* ~*, the Setting rewrite. The manual gives it no reply. */
static void wacom_iv_rewrite_setting(WacomIvChardev *w, const char *s)
{
    unsigned int body, inc, tt, xr, yr;
    int n = sscanf(s, "%8x,%3u,%2u,%4u,%4u", &body, &inc, &tt, &xr, &yr);

    if (n >= 1 && strspn(s, "0123456789abcdefABCDEF") == 8) {
        w->body = body;
    }
    if (n == 5) {
        w->increment = inc;
        w->tt = tt;
        w->interval = tt;
        if (wacom_iv_res_ok(xr)) {
            w->xres = xr;
        }
        if (wacom_iv_res_ok(yr)) {
            w->yres = yr;
        }
    }
}

/*
 * RQ1 or "@": enter remote request mode and send one coordinate pair at
 * once, whatever the suppression rules say. It also ends an SP, as the
 * manual's SP entry says it may. Only ST leaves the mode.
 */
static void wacom_iv_request(WacomIvChardev *w)
{
    w->stopped = false;
    w->remote = true;
    w->pending = true;
    wacom_iv_update_pointer(w);
    wacom_iv_try_send(w);
}

static bool wacom_iv_is(const char *cmd, const char *name)
{
    return strncmp(cmd, name, 2) == 0;
}

static void wacom_iv_command(WacomIvChardev *w, char *cmd)
{
    if (!strcmp(cmd, "RE")) {
        trace_wacom_iv_cmd(cmd);
        /* The reset window may swallow what follows; flush it instead. */
        wacom_iv_reset_settings(w);
        w->query_index = 0;
        w->query[0] = 0;
    } else if (!strcmp(cmd, "~C")) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "~C%d,%d\r", WACOM_IV_MAX_X,
                           WACOM_IV_MAX_Y);

        trace_wacom_iv_cmd(cmd);
        wacom_iv_queue_output(w, (uint8_t *)buf, len);
    } else if (wacom_iv_is(cmd, "~R")) {
        trace_wacom_iv_cmd(cmd);
        if (cmd[2] == '1' || cmd[2] == '2') {
            /* M1 and M2 are not modelled; they read as the power-up Setting. */
            char which[2] = { cmd[2], 0 };

            wacom_iv_reply_setting(w, which, WACOM_IV_DEFAULT_BODY, 0,
                                   WACOM_IV_DEFAULT_TT, WACOM_IV_DEFAULT_RES,
                                   WACOM_IV_DEFAULT_RES);
        } else {
            wacom_iv_reply_setting(w, "", w->body, w->increment, w->tt,
                                   w->xres, w->yres);
        }
    } else if (wacom_iv_is(cmd, "~*")) {
        trace_wacom_iv_cmd(cmd);
        wacom_iv_rewrite_setting(w, cmd + 2);
    } else if (wacom_iv_is(cmd, "SP")) {
        trace_wacom_iv_cmd(cmd);
        w->stopped = true;
    } else if (wacom_iv_is(cmd, "ST")) {
        trace_wacom_iv_cmd(cmd);
        w->stopped = false;
        w->remote = false;
    } else if (!strcmp(cmd, "RQ1")) {
        trace_wacom_iv_cmd(cmd);
        wacom_iv_request(w);
    } else if (wacom_iv_is(cmd, "SU")) {
        trace_wacom_iv_cmd(cmd);
        w->increment = atoi(cmd + 2);
    } else if (wacom_iv_is(cmd, "IT")) {
        trace_wacom_iv_cmd(cmd);
        w->interval = atoi(cmd + 2);
        w->tt = w->interval;
    } else if (wacom_iv_is(cmd, "NR")) {
        int res = atoi(cmd + 2);

        trace_wacom_iv_cmd(cmd);
        if (wacom_iv_res_ok(res)) {
            w->xres = w->yres = res;
        }
    } else {
        /* OC, AS, DE, FM, PH, SR, SW, PO ...: accepted, no reply. */
        trace_wacom_iv_cmd_ignored(cmd);
    }
}

static int wacom_iv_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    WacomIvChardev *w = WACOM_IV_CHARDEV(chr);
    char *pos;
    int i;

    if (!wacom_iv_line_ok(w)) {
        return len;
    }
    for (i = 0; i < len && w->query_index < sizeof(w->query) - 1; i++) {
        w->query[w->query_index++] = buf[i];
    }
    w->query[w->query_index] = 0;
    w->talked = true;

    /* One write can carry several commands; answer all that are whole. */
    for (;;) {
        while (w->query_index > 0 &&
               strchr("@$#\r\n", w->query[0])) {
            if (w->query[0] == '@') {
                trace_wacom_iv_cmd("@");
                wacom_iv_request(w);
            } else if (w->query[0] == '#' || w->query[0] == '$') {
                wacom_iv_global_reset(w, w->query[0]);
            }
            wacom_iv_shift_input(w, 1);
        }
        if (!wacom_iv_line_ok(w)) {
            /* A reset moved the line; later bytes came at the old speed. */
            w->query_index = 0;
            break;
        }
        if (!w->query_index) {
            break;
        }

        if (!strncmp((char *)w->query, "~#", 2)) {
            /* The driver does not wait for a terminator after this one. */
            trace_wacom_iv_cmd("~#");
            wacom_iv_shift_input(w, 2);
            wacom_iv_queue_output(w, (const uint8_t *)wacom_iv_model_string,
                                  strlen(wacom_iv_model_string));
            continue;
        }

        pos = strpbrk((char *)w->query, "\r\n");
        if (!pos) {
            if (w->query_index >= sizeof(w->query) - 1) {
                /* Not a command; do not wedge on it. */
                w->query_index = 0;
            }
            break;
        }
        *pos = 0;
        wacom_iv_command(w, (char *)w->query);
        /* RE empties the buffer; otherwise drop the line and its end. */
        if (w->query_index) {
            wacom_iv_shift_input(w, pos - (char *)w->query + 1);
        }
        if (!wacom_iv_line_ok(w)) {
            /* ~* moved the line; later bytes came at the old speed. */
            w->query_index = 0;
            break;
        }
    }

    wacom_iv_update_pointer(w);
    return len;
}

static void wacom_iv_accept_input(Chardev *chr)
{
    WacomIvChardev *w = WACOM_IV_CHARDEV(chr);
    uint8_t handed_over[WACOM_IV_OUTBUF_LEN];
    int len = MIN(qemu_chr_be_can_write(chr), w->outlen);

    if (len) {
        /*
         * Take the bytes off the queue first: qemu_chr_be_write() can
         * re-enter here when the frontend drains its FIFO from its own
         * read handler, and a nested call must not see them again.
         */
        memcpy(handed_over, w->outbuf, len);
        w->outlen -= len;
        if (w->outlen) {
            memmove(w->outbuf, w->outbuf + len, w->outlen);
        }
        qemu_chr_be_write(chr, handed_over, len);
    }
    if (w->pending && !w->outlen && w->hs) {
        wacom_iv_try_send(w);
    }
}

static int wacom_iv_chr_ioctl(Chardev *chr, int cmd, void *arg)
{
    WacomIvChardev *w = WACOM_IV_CHARDEV(chr);
    QEMUSerialSetParams *ssp;

    switch (cmd) {
    case CHR_IOCTL_SERIAL_SET_PARAMS:
        ssp = arg;
        trace_wacom_iv_line(ssp->speed, ssp->parity, ssp->data_bits,
                            ssp->stop_bits);
        /* The tablet does not reset on a line change; it just goes quiet. */
        w->speed = ssp->speed;
        w->parity = ssp->parity;
        w->data_bits = ssp->data_bits;
        w->stop_bits = ssp->stop_bits;
        if (!wacom_iv_line_ok(w)) {
            w->outlen = 0;
            w->query_index = 0;
        }
        wacom_iv_update_pointer(w);
        break;
    default:
        return -ENOTSUP;
    }
    return 0;
}

static bool wacom_iv_chr_open(Chardev *chr, ChardevBackend *backend,
                              Error **errp)
{
    WacomIvChardev *w = WACOM_IV_CHARDEV(chr);

    w->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wacom_iv_timer_cb, w);
    wacom_iv_reset_settings(w);
    qemu_chr_be_event(chr, CHR_EVENT_OPENED);
    return true;
}

static void wacom_iv_chr_finalize(Object *obj)
{
    WacomIvChardev *w = WACOM_IV_CHARDEV(obj);

    if (w->hs) {
        qemu_input_handler_unregister(w->hs);
    }
    if (w->timer) {
        timer_free(w->timer);
    }
}

static void wacom_iv_chr_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_open = wacom_iv_chr_open;
    cc->chr_write = wacom_iv_chr_write;
    cc->chr_ioctl = wacom_iv_chr_ioctl;
    cc->chr_accept_input = wacom_iv_accept_input;
}

static const TypeInfo wacom_iv_type_info = {
    .name = WACOM_IV_TYPE_NAME,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(WacomIvChardev),
    .instance_finalize = wacom_iv_chr_finalize,
    .class_init = wacom_iv_chr_class_init,
};

static void wacom_iv_register_types(void)
{
    type_register_static(&wacom_iv_type_info);
}

type_init(wacom_iv_register_types);
