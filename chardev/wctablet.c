/*
 * QEMU Wacom Penpartner serial tablet emulation
 *
 * some protocol details:
 *   http://linuxwacom.sourceforge.net/wiki/index.php/Serial_Protocol_IV
 *
 * Copyright (c) 2016 Anatoli Huseu1
 * Copyright (c) 2016,17 Gerd Hoffmann
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "chardev/char-serial.h"
#include "qapi/error.h"
#include "ui/input.h"
#include "trace.h"
#include "qom/object.h"


#define WC_OUTPUT_BUF_MAX_LEN 512
#define WC_COMMAND_MAX_LEN 60

#define WC_L7(n) ((n) & 127)
#define WC_M7(n) (((n) >> 7) & 127)
#define WC_H2(n) ((n) >> 14)

#define WC_L4(n) ((n) & 15)
#define WC_H4(n) (((n) >> 4) & 15)

/* Model string and config string */
#define WC_MODEL_STRING_LENGTH 18
uint8_t WC_MODEL_STRING[WC_MODEL_STRING_LENGTH + 1] = "~#CT-0045R,V1.3-5,";

/* "~#" reply for model=UD-1212-R: a Wacom UD-series tablet */
#define WC_UD1212R_MODEL_STRING_LENGTH 21
uint8_t WC_UD1212R_MODEL_STRING[WC_UD1212R_MODEL_STRING_LENGTH + 1] =
    "~#UD-1212-R00 V1.3-5\r";

#define WC_CONFIG_STRING_LENGTH 8
uint8_t WC_CONFIG_STRING[WC_CONFIG_STRING_LENGTH + 1] = "96,N,8,0";

#define WC_FULL_CONFIG_STRING_LENGTH 61
uint8_t WC_FULL_CONFIG_STRING[WC_FULL_CONFIG_STRING_LENGTH + 1] = {
    0x5c, 0x39, 0x36, 0x2c, 0x4e, 0x2c, 0x38, 0x2c,
    0x31, 0x28, 0x01, 0x24, 0x57, 0x41, 0x43, 0x30,
    0x30, 0x34, 0x35, 0x5c, 0x5c, 0x50, 0x45, 0x4e, 0x5c,
    0x57, 0x41, 0x43, 0x30, 0x30, 0x30, 0x30, 0x5c,
    0x54, 0x61, 0x62, 0x6c, 0x65, 0x74, 0x0d, 0x0a,
    0x43, 0x54, 0x2d, 0x30, 0x30, 0x34, 0x35, 0x52,
    0x2c, 0x56, 0x31, 0x2e, 0x33, 0x2d, 0x35, 0x0d,
    0x0a, 0x45, 0x37, 0x29
};

/* This structure is used to save private info for Wacom Tablet. */
struct TabletChardev {
    Chardev parent;
    QemuInputHandlerState *hs;

    /* Query string from serial */
    uint8_t query[100];
    int query_index;

    /* Command to be sent to serial port */
    uint8_t outbuf[WC_OUTPUT_BUF_MAX_LEN];
    int outlen;

    int line_speed;
    bool send_events;
    int axis[INPUT_AXIS__MAX];
    bool btns[INPUT_BUTTON__MAX];

    /*
     * Identity to present on "~#", and the coordinate range to report on
     * "~C" and scale outgoing packets to. NULL/0 selects the default
     * Wacom Penpartner CT-0045R, which has neither a settable range nor
     * an addressable "~C" query.
     */
    char *model;
    int max_x, max_y;
};
typedef struct TabletChardev TabletChardev;

#define TYPE_CHARDEV_WCTABLET "chardev-wctablet"
DECLARE_INSTANCE_CHECKER(TabletChardev, WCTABLET_CHARDEV,
                         TYPE_CHARDEV_WCTABLET)


static void wctablet_chr_accept_input(Chardev *chr);

static void wctablet_shift_input(TabletChardev *tablet, int count)
{
    tablet->query_index -= count;
    memmove(tablet->query, tablet->query + count, tablet->query_index);
    tablet->query[tablet->query_index] = 0;
}

static void wctablet_queue_output(TabletChardev *tablet, uint8_t *buf, int count)
{
    if (tablet->outlen + count > sizeof(tablet->outbuf)) {
        return;
    }

    memcpy(tablet->outbuf + tablet->outlen, buf, count);
    tablet->outlen += count;
    wctablet_chr_accept_input(CHARDEV(tablet));
}

static void wctablet_reset(TabletChardev *tablet)
{
    /* clear buffers */
    tablet->query_index = 0;
    tablet->outlen = 0;
    /* reset state */
    tablet->send_events = false;
}

/*
 * Fill in the WACOM IV pressure-mode switch and pressure fields the
 * WTC-1002 "Software Interface Reference Manual" specifies for a
 * UD-series tablet at ROM version 1.2 and higher (section 4.1's data
 * format table, and section 4.2's table of button states). In WACOM
 * IV, switch and pressure data always ride together in the same
 * coordinate packet -- the PH command has no effect there -- so this
 * does not need to gate on any pressure-mode flag.
 *
 * Byte 1 bit 3 is the Button Flag: 1 whenever any switch is active.
 * Byte 4 bits 6-3 are B3-B0, the switch number: 0 = no switch, 1 =
 * tip switch, 2 = first side switch, 3 = tip + first side switch, 4 =
 * second side switch, 5 = tip + second side switch (the two-side-
 * switch stylus's range, per chapter 5). QEMU's LEFT/MIDDLE/RIGHT map
 * to tip/first side/second side; a stylus has no switch number for
 * both side switches down together, so RIGHT wins if MIDDLE and RIGHT
 * are both down.
 *
 * Byte 7 bits 5-0 are P6-P1, a 6-bit pressure magnitude (sign bit 6,
 * Sp, stays 0: QEMU's tablet has no analog pressure axis, only a
 * button). IRIX's wacom module derives its own "tip button" from the
 * pressure crossing a threshold (input(7)'s btnpressure control), so
 * LEFT down is reported as a high pressure -- 60, the top of the
 * range the manual calls safe for readers still expecting the older
 * 120-level format -- and zero otherwise.
 */
static void wctablet_set_ud_switch(TabletChardev *tablet, uint8_t codes[8])
{
    int switch_number = 0;

    if (tablet->btns[INPUT_BUTTON_RIGHT]) {
        switch_number = tablet->btns[INPUT_BUTTON_LEFT] ? 5 : 4;
    } else if (tablet->btns[INPUT_BUTTON_MIDDLE]) {
        switch_number = tablet->btns[INPUT_BUTTON_LEFT] ? 3 : 2;
    } else if (tablet->btns[INPUT_BUTTON_LEFT]) {
        switch_number = 1;
    }

    if (switch_number) {
        codes[0] |= 0x08;
    }
    codes[3] |= switch_number << 3;
    codes[6] = tablet->btns[INPUT_BUTTON_LEFT] ? 60 : 0;
}

static void wctablet_queue_event(TabletChardev *tablet)
{
    uint8_t codes[8] = { 0xe0, 0, 0, 0, 0, 0, 0 };
    int newX, newY;

    if (tablet->line_speed != 9600) {
        return;
    }

    if (tablet->max_x && tablet->max_y) {
        newX = (int64_t)tablet->axis[INPUT_AXIS_X] * tablet->max_x /
               INPUT_EVENT_ABS_MAX;
        newY = (int64_t)tablet->axis[INPUT_AXIS_Y] * tablet->max_y /
               INPUT_EVENT_ABS_MAX;
    } else {
        newX = tablet->axis[INPUT_AXIS_X] * 0.1537;
        newY = tablet->axis[INPUT_AXIS_Y] * 0.1152;
    }

    codes[0] = codes[0] | WC_H2(newX);
    codes[1] = codes[1] | WC_M7(newX);
    codes[2] = codes[2] | WC_L7(newX);

    codes[3] = codes[3] | WC_H2(newY);
    codes[4] = codes[4] | WC_M7(newY);
    codes[5] = codes[5] | WC_L7(newY);

    if (tablet->model) {
        wctablet_set_ud_switch(tablet, codes);
    } else if (tablet->btns[INPUT_BUTTON_LEFT]) {
        codes[0] = 0xa0;
    }

    wctablet_queue_output(tablet, codes, 7);
}

static void wctablet_input_event(DeviceState *dev, QemuConsole *src,
                                 QemuInputEvent *evt)
{
    TabletChardev *tablet = (TabletChardev *)dev;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        tablet->axis[evt->abs.axis] = evt->abs.value;
        break;

    case INPUT_EVENT_KIND_BTN:
        tablet->btns[evt->btn.button] = evt->btn.down;
        break;

    default:
        /* keep gcc happy */
        break;
    }
}

static void wctablet_input_sync(DeviceState *dev)
{
    TabletChardev *tablet = (TabletChardev *)dev;

    if (tablet->send_events) {
        wctablet_queue_event(tablet);
    }
}

static const QemuInputHandler wctablet_handler = {
    .name  = "QEMU Wacom Pen Tablet",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = wctablet_input_event,
    .sync  = wctablet_input_sync,
};

static void wctablet_chr_accept_input(Chardev *chr)
{
    TabletChardev *tablet = WCTABLET_CHARDEV(chr);
    uint8_t handed_over[WC_OUTPUT_BUF_MAX_LEN];
    int len, canWrite;

    canWrite = qemu_chr_be_can_write(chr);
    len = canWrite;
    if (len > tablet->outlen) {
        len = tablet->outlen;
    }

    if (len) {
        /*
         * Take the bytes off the queue before handing them over.
         * qemu_chr_be_write() re-enters this function whenever the
         * frontend drains its receive FIFO from inside its own read
         * handler, and a nested call that could still see these bytes
         * would send them a second time and leave outlen negative once
         * both frames had subtracted the same length.
         */
        memcpy(handed_over, tablet->outbuf, len);
        tablet->outlen -= len;
        if (tablet->outlen) {
            memmove(tablet->outbuf, tablet->outbuf + len, tablet->outlen);
        }
        qemu_chr_be_write(chr, handed_over, len);
    }
}

static int wctablet_chr_write(struct Chardev *chr,
                              const uint8_t *buf, int len)
{
    TabletChardev *tablet = WCTABLET_CHARDEV(chr);
    unsigned int i, clen;
    char *pos;

    if (tablet->line_speed != 9600) {
        return len;
    }
    for (i = 0; i < len && tablet->query_index < sizeof(tablet->query) - 1; i++) {
        tablet->query[tablet->query_index++] = buf[i];
    }
    tablet->query[tablet->query_index] = 0;

    /*
     * A single write() can deliver more than one complete command -- IRIX's
     * wacom STREAMS module bursts several during its init handshake -- so
     * keep answering until the buffer holds nothing more we recognize.
     */
    for (;;) {
        while (tablet->query_index > 0 && (tablet->query[0] == '@'  ||
                                           tablet->query[0] == '$'  ||
                                           tablet->query[0] == '#'  ||
                                           tablet->query[0] == '\r' ||
                                           tablet->query[0] == '\n')) {
            wctablet_shift_input(tablet, 1);
        }
        if (!tablet->query_index) {
            break;
        }

        if (strncmp((char *)tablet->query, "~#", 2) == 0) {
            /* init / detect sequence */
            trace_wct_init();
            wctablet_shift_input(tablet, 2);
            if (tablet->model) {
                wctablet_queue_output(tablet, WC_UD1212R_MODEL_STRING,
                                      WC_UD1212R_MODEL_STRING_LENGTH);
            } else {
                wctablet_queue_output(tablet, WC_MODEL_STRING,
                                      WC_MODEL_STRING_LENGTH);
            }
            continue;
        }

        /* detect line */
        pos = strchr((char *)tablet->query, '\r');
        if (!pos) {
            pos = strchr((char *)tablet->query, '\n');
        }
        if (!pos) {
            break;
        }
        clen = pos - (char *)tablet->query;

        /* process commands */
        if (strncmp((char *)tablet->query, "RE", 2) == 0 &&
            clen == 2) {
            trace_wct_cmd_re();
            wctablet_shift_input(tablet, 3);
            wctablet_queue_output(tablet, WC_CONFIG_STRING,
                                  WC_CONFIG_STRING_LENGTH);

        } else if (strncmp((char *)tablet->query, "~C", 2) == 0 &&
                   clen == 2 && tablet->max_x && tablet->max_y) {
            char maxbuf[32];
            int maxlen;

            trace_wct_cmd_c();
            wctablet_shift_input(tablet, 3);
            maxlen = snprintf(maxbuf, sizeof(maxbuf), "~C%d,%d\r",
                              tablet->max_x, tablet->max_y);
            wctablet_queue_output(tablet, (uint8_t *)maxbuf, maxlen);

        } else if (strncmp((char *)tablet->query, "ST", 2) == 0 &&
                   clen == 2) {
            trace_wct_cmd_st();
            wctablet_shift_input(tablet, 3);
            tablet->send_events = true;
            wctablet_queue_event(tablet);

        } else if (strncmp((char *)tablet->query, "SP", 2) == 0 &&
                   clen == 2) {
            trace_wct_cmd_sp();
            wctablet_shift_input(tablet, 3);
            tablet->send_events = false;

        } else if (strncmp((char *)tablet->query, "TS", 2) == 0 &&
                   clen == 3) {
            unsigned int input = tablet->query[2];
            uint8_t codes[7] = {
                0xa3,
                ((input & 0x80) == 0) ? 0x7e : 0x7f,
                (((WC_H4(input) & 0x7) ^ 0x5) << 4) | (WC_L4(input) ^ 0x7),
                0x03,
                0x7f,
                0x7f,
                0x00,
            };
            trace_wct_cmd_ts(input);
            wctablet_shift_input(tablet, 4);
            wctablet_queue_output(tablet, codes, 7);

        } else {
            tablet->query[clen] = 0; /* terminate line for printing */
            trace_wct_cmd_other((char *)tablet->query);
            wctablet_shift_input(tablet, clen + 1);

        }
    }

    return len;
}

static int wctablet_chr_ioctl(Chardev *chr, int cmd, void *arg)
{
    TabletChardev *tablet = WCTABLET_CHARDEV(chr);
    QEMUSerialSetParams *ssp;

    switch (cmd) {
    case CHR_IOCTL_SERIAL_SET_PARAMS:
        ssp = arg;
        if (tablet->line_speed != ssp->speed) {
            trace_wct_speed(ssp->speed);
            wctablet_reset(tablet);
            tablet->line_speed = ssp->speed;
        }
        break;
    default:
        return -ENOTSUP;
    }
    return 0;
}

static void wctablet_chr_finalize(Object *obj)
{
    TabletChardev *tablet = WCTABLET_CHARDEV(obj);

    if (tablet->hs) {
        qemu_input_handler_unregister(tablet->hs);
    }
    g_free(tablet->model);
}

static bool wctablet_chr_open(Chardev *chr,
                              ChardevBackend *backend,
                              Error **errp)
{
    TabletChardev *tablet = WCTABLET_CHARDEV(chr);
    ChardevWctablet *wctablet = backend->u.wctablet.data;

    if (wctablet->model) {
        if (strcmp(wctablet->model, "UD-1212-R") != 0) {
            error_setg(errp, "chardev: wctablet: unsupported model '%s'",
                      wctablet->model);
            return false;
        }
        tablet->model = g_strdup(wctablet->model);
        tablet->max_x = 15240;
        tablet->max_y = 15240;
    }

    /* init state machine */
    tablet->outlen = 0;
    if (!tablet->model) {
        /*
         * Unsolicited on open, matching the real Penpartner CT-0045R.
         * IRIX's wacom module expects an identified UD-series tablet to
         * stay quiet until asked, so the UD-1212-R model skips this.
         */
        memcpy(tablet->outbuf, WC_FULL_CONFIG_STRING,
              WC_FULL_CONFIG_STRING_LENGTH);
        tablet->outlen = WC_FULL_CONFIG_STRING_LENGTH;
    }
    tablet->query_index = 0;

    tablet->hs = qemu_input_handler_register((DeviceState *)tablet,
                                             &wctablet_handler);

    qemu_chr_be_event(chr, CHR_EVENT_OPENED);
    return true;
}

static void wctablet_chr_parse(QemuOpts *opts, ChardevBackend *backend,
                               Error **errp)
{
    const char *model = qemu_opt_get(opts, "model");
    ChardevWctablet *wctablet;

    backend->type = CHARDEV_BACKEND_KIND_WCTABLET;
    wctablet = backend->u.wctablet.data = g_new0(ChardevWctablet, 1);
    qemu_chr_parse_common(opts, qapi_ChardevWctablet_base(wctablet));
    wctablet->model = g_strdup(model);
}

static void wctablet_chr_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_parse = wctablet_chr_parse;
    cc->chr_open = wctablet_chr_open;
    cc->chr_write = wctablet_chr_write;
    cc->chr_ioctl = wctablet_chr_ioctl;
    cc->chr_accept_input = wctablet_chr_accept_input;
}

static const TypeInfo wctablet_type_info = {
    .name = TYPE_CHARDEV_WCTABLET,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(TabletChardev),
    .instance_finalize = wctablet_chr_finalize,
    .class_init = wctablet_chr_class_init,
};

static void register_types(void)
{
     type_register_static(&wctablet_type_info);
}

type_init(register_types);
