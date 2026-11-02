/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Origin 3000 power-bay controller: the bay end of the L1's power-bay
 * serial link.
 *
 * The L1 talks to the bay over one UART at 19200 baud in framed
 * request/reply transactions. This model decodes the stuffed frames the L1
 * sends on a character device, checks their checksum and answers the
 * commands the bay's application answers. The bay's boards are not modelled:
 * the readings come from one input snapshot property and the FRU contents
 * behind the streamed read are zeros, each logged as unimplemented.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/misc/sgi-pbay.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "system/reset.h"

/* Stuffing: a start byte, a terminator and the escape that hides them. */
#define PBAY_START      0xa0
#define PBAY_END        0xa5
#define PBAY_ESCAPE     0xaa
#define PBAY_ESC_START  0xb0
#define PBAY_ESC_END    0xb5
#define PBAY_ESC_ESCAPE 0xba

/*
 * Frame body layout: destination, source, a zero, control, command, then
 * a request's data or a reply's completion code at offset 7.
 */
#define PBAY_OFF_DST    0
#define PBAY_OFF_SRC    2
#define PBAY_OFF_CTL    5
#define PBAY_OFF_CMD    6
#define PBAY_OFF_DATA   7
#define PBAY_HEADER     7
#define PBAY_CHECKSUM   2
#define PBAY_REPLY_BIT  0x80

/*
 * The largest reply body: a streamed read can carry 256 samples behind the
 * header and the tunnelled prefix.
 */
#define PBAY_MAX_REPLY  320

/* The commands the bay's application answers. */
#define CMD_ADDRESS     0x02
#define CMD_STATUS_A    0x0a
#define CMD_STATE       0x0b
#define CMD_SILENT      0x10
#define CMD_BLOCK       0x11
#define CMD_ASSIGN      0x12
#define CMD_TYPE        0x13
#define CMD_TUNNEL      0x20
#define CMD_VERSION     0xc0
#define CMD_SILENT_A2   0xa2
/* Flash loader commands: not in the application's table. */
#define CMD_FLASH_DATA  0xc1
#define CMD_FLASH_END   0xc2

/* Completion code for a command or pair the application does not have. */
#define CC_INVALID      0xc1

#define PBAY_DEVICE_TYPE    0x22

/* The loader's frame status: 0 normally, 0xff when it flags a frame. */
#define PBAY_STATUS_BAD     0xff

/* Image version 0x0012 (00.18) and its build time word, little-endian. */
static const uint8_t pbay_version_reply[] = {
    0x12, 0x00, 0x60, 0xa0, 0xd4, 0x39,
};

/* Tunnelled network functions, shifted left two. */
#define NF_00   0x00
#define NF_10   0x10
#define NF_18   0x18
#define NF_28   0x28

/* One of the bay's records: its key's next id, table size and table bytes. */
typedef struct PbayRecord {
    uint16_t next;
    uint8_t max;                /* what count 0xff returns */
    uint8_t body[0x30];
} PbayRecord;

/* Keys 1 and 4 to 9 are derived; the rest are tabulated. */
static const PbayRecord pbay_records[13] = {
    [0] = { 0x0002, 0x20, {
        0x01, 0x00, 0x01, 0x02, 0x1b, 0x22, 0x00, 0x07, 0x15, 0x01, 0x00,
        0x03, 0x09, 0x71, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0xc0, 0x00,
        0x00, 0x08, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
    [2] = { 0x0003, 0x20, {
        0x02, 0x00, 0x01, 0x02, 0x1b, 0x22, 0x00, 0x01, 0x0a, 0x01, 0x00,
        0x03, 0x08, 0x70, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x00, 0xc0, 0x00,
        0x00, 0x06, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
    [3] = { 0x0004, 0x10, {
        0x03, 0x00, 0x01, 0x12, 0x0b, 0x22, 0x01, 0xcb, 0x00, 0x00, 0x00,
        0x00, 0x15, 0x01, 0x00, 0x00 } },
    [10] = { 0x000b, 0x10, {
        0x0a, 0x00, 0x01, 0x11, 0x0b, 0x22, 0xac, 0x80, 0x00, 0x00, 0x10,
        0x00, 0x15, 0x01, 0x00, 0x00 } },
    [11] = { 0x000c, 0x10, {
        0x0b, 0x00, 0x01, 0x11, 0x0b, 0x22, 0xae, 0x80, 0x00, 0x00, 0x10,
        0x00, 0xb0, 0x01, 0x00, 0x00 } },
    /*
     * The header's length byte, 0x2c, makes this record 49 bytes where the
     * table holds 48; a read returns the table's.
     */
    [12] = { 0xffff, 0x30, {
        0x0c, 0x00, 0x01, 0x01, 0x2c, 0x22, 0x00, 0x0f, 0x14, 0x09, 0x00,
        0x03, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
        0x00, 0x01, 0x01, 0x02, 0x00, 0x0b, 0x08, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xff, 0x00 } },
};

typedef struct PbayReply {
    uint8_t buf[PBAY_MAX_REPLY];
    unsigned len;
} PbayReply;

static void reply_byte(PbayReply *r, uint8_t b)
{
    g_assert(r->len < sizeof(r->buf));
    r->buf[r->len++] = b;
}

static void reply_zeros(PbayReply *r, unsigned n)
{
    while (n--) {
        reply_byte(r, 0);
    }
}

static uint16_t pbay_checksum(const uint8_t *body, unsigned len)
{
    unsigned sum = 0;

    for (unsigned i = 0; i < len; i++) {
        sum += body[i];
    }
    return 0xffff - (sum + len + 1);
}

static void pbay_put_stuffed(uint8_t *out, unsigned *n, uint8_t b)
{
    switch (b) {
    case PBAY_START:
        out[(*n)++] = PBAY_ESCAPE;
        out[(*n)++] = PBAY_ESC_START;
        break;
    case PBAY_END:
        out[(*n)++] = PBAY_ESCAPE;
        out[(*n)++] = PBAY_ESC_END;
        break;
    case PBAY_ESCAPE:
        out[(*n)++] = PBAY_ESCAPE;
        out[(*n)++] = PBAY_ESC_ESCAPE;
        break;
    default:
        out[(*n)++] = b;
    }
}

/*
 * Send @body with its checksum, stuffed, behind a start byte and ahead of
 * the terminator.
 */
static void pbay_send(SGIPbayState *s, const uint8_t *body, unsigned len)
{
    uint8_t wire[2 * (PBAY_MAX_REPLY + PBAY_CHECKSUM) + 2];
    uint16_t sum = pbay_checksum(body, len);
    unsigned n = 0;

    wire[n++] = PBAY_START;
    for (unsigned i = 0; i < len; i++) {
        pbay_put_stuffed(wire, &n, body[i]);
    }
    pbay_put_stuffed(wire, &n, sum & 0xff);
    pbay_put_stuffed(wire, &n, sum >> 8);
    wire[n++] = PBAY_END;
    qemu_chr_fe_write_all(&s->chr, wire, n);
}

/*
 * The reply header: to the request's source, from the request's
 * destination, the control byte with its reply bit and the command echoed.
 */
static void reply_begin(PbayReply *r, const uint8_t *req)
{
    reply_byte(r, req[PBAY_OFF_SRC]);
    reply_byte(r, req[PBAY_OFF_SRC + 1]);
    reply_byte(r, req[PBAY_OFF_DST]);
    reply_byte(r, req[PBAY_OFF_DST + 1]);
    reply_byte(r, 0);
    reply_byte(r, req[PBAY_OFF_CTL] | PBAY_REPLY_BIT);
    reply_byte(r, req[PBAY_OFF_CMD]);
}

/* A header-only reply: the completion code and nothing else. */
static void reply_error(PbayReply *r, const uint8_t *req, uint8_t cc)
{
    r->len = 0;
    reply_begin(r, req);
    reply_byte(r, cc);
}

/* The one-hot state code's bit number, 1 to 8, or 0 for no state. */
static uint8_t pbay_state_bit(uint8_t code)
{
    return code ? ctz32(code) + 1 : 0;
}

/*
 * Sensors 1 to 14 read five inputs through the P1 snapshot: sensors 1 to 6
 * four inputs masked by the sensor's own bit and one by 0x04, sensors 7 to
 * 14 two inputs masked by their bit.
 */
static uint8_t pbay_sensor_mask(uint8_t sensor, uint8_t snap)
{
    uint8_t bits = 0;

    if (sensor <= 6) {
        for (unsigned i = 0; i < 4; i++) {
            bits |= !!(snap & (1 << (sensor - 1))) << i;
        }
        bits |= !!(snap & 0x04) << 4;
    } else {
        for (unsigned i = 0; i < 2; i++) {
            bits |= !!(snap & (1 << (sensor - 7))) << i;
        }
    }
    return bits;
}

/* Keys 0 and 1 are one record; keys 4 to 9 are the FRU locators a0 to aa. */
static void pbay_record_read(PbayReply *r, uint8_t key, uint8_t count)
{
    PbayRecord rec;
    bool fru = key >= 4 && key <= 9;
    unsigned n;

    if (fru) {
        rec = (PbayRecord) {
            .next = key + 1, .max = 0x10,
            .body = { key, 0x00, 0x01, 0x11, 0x0b, 0x22,
                      0xa0 + 2 * (key - 4), 0x80, 0x00, 0x00, 0x10, 0x00,
                      0x0a, key - 3, 0x00, 0x00 },
        };
    } else {
        rec = pbay_records[key == 1 ? 0 : key];
    }
    reply_byte(r, rec.next & 0xff);
    reply_byte(r, rec.next >> 8);

    if (count == 0xff) {
        n = rec.max;
    } else if (count >= 1 && count <= rec.max) {
        n = count;
    } else {
        /* A locator still returns its key byte. */
        n = fru ? 1 : 0;
    }
    for (unsigned i = 0; i < n; i++) {
        reply_byte(r, rec.body[i]);
    }
}

/*
 * A tunnelled request carries, from offset 7: the inner target byte, the
 * network function shifted left two with the unit in its low bits, the
 * command number and the data. The reply carries the link completion code,
 * a copy of the target byte, the network function plus four, the command
 * and an inner completion code, then the pair's data.
 */
static void pbay_tunnel(SGIPbayState *s, const uint8_t *req, unsigned len,
                        PbayReply *r)
{
    const uint8_t *t = req + PBAY_OFF_DATA;
    unsigned tlen = len - PBAY_OFF_DATA;
    /* The data bytes from offset 10; a short request reads as zero. */
    uint8_t d[8] = { 0 };
    uint8_t netfn, cmd;

    if (tlen < 3) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pbay: tunnelled request of %u bytes, expected at "
                      "least 3\n", tlen);
        return;
    }
    memcpy(d, t + 3, MIN(tlen - 3, sizeof(d)));
    netfn = t[1] & ~3;
    cmd = t[2];

    if (s->frame_status == PBAY_STATUS_BAD) {
        if (netfn != NF_00 && netfn != NF_10 && netfn != NF_18 &&
            netfn != NF_28) {
            qemu_log_mask(LOG_UNIMP, "sgi-pbay: tunnelled network "
                          "function 0x%02x is not answered\n", netfn);
            return;
        }
        /* The prefix with both completion codes 0xff and no data. */
        reply_begin(r, req);
        reply_byte(r, PBAY_STATUS_BAD);
        reply_byte(r, t[0]);
        reply_byte(r, t[1] + 4);
        reply_byte(r, cmd);
        reply_byte(r, PBAY_STATUS_BAD);
        return;
    }

    reply_begin(r, req);
    reply_byte(r, 0);
    reply_byte(r, t[0]);
    reply_byte(r, t[1] + 4);
    reply_byte(r, cmd);
    reply_byte(r, 0);

    switch (netfn << 8 | cmd) {
    case NF_00 << 8 | 0x00:
        reply_byte(r, 0x00);
        reply_byte(r, 0x22);
        reply_byte(r, 0x22);
        reply_byte(r, 0x00);
        reply_byte(r, 0x00);
        return;
    case NF_10 << 8 | 0x2d:
        if (d[0] >= 1 && d[0] <= 14) {
            reply_byte(r, 0x00);
            reply_byte(r, 0x00);
            reply_byte(r, pbay_sensor_mask(d[0], s->input_snapshot));
            reply_byte(r, 0x00);
            return;
        }
        if (d[0] == 15) {
            qemu_log_mask(LOG_UNIMP, "sgi-pbay: sensor 15 replies at once, "
                          "the bay replies on a later sample\n");
            reply_byte(r, s->input_snapshot);
            reply_byte(r, 0x00);
            return;
        }
        break;
    case NF_18 << 8 | 0x03:
        qemu_log_mask(LOG_UNIMP, "sgi-pbay: tunnelled 0x18,0x03 runs an "
                      "output sequence, which is not modelled\n");
        return;
    case NF_18 << 8 | 0x04:
        reply_byte(r, 0x56);
        reply_byte(r, 0x00);
        return;
    case NF_28 << 8 | 0x10:
        /* The handler copies its own reply over the request it reads. */
        reply_byte(r, 0x10);
        reply_byte(r, 0x00);
        reply_byte(r, 0x10);
        reply_byte(r, 0x00);
        return;
    case NF_28 << 8 | 0x11:
        if (d[0] >= 0xa0 && d[0] <= 0xae && !(d[0] & 1)) {
            unsigned count = d[3] ? d[3] : 256;

            qemu_log_mask(LOG_UNIMP, "sgi-pbay: device 0x%02x read returns "
                          "zeros, its contents are not modelled\n", d[0]);
            reply_byte(r, d[3]);
            reply_zeros(r, count);
            return;
        }
        break;
    case NF_28 << 8 | 0x20:
        reply_byte(r, 0x01);
        reply_byte(r, 0x0c);
        reply_zeros(r, 12);
        return;
    case NF_28 << 8 | 0x23:
        if (d[2] <= 12) {
            pbay_record_read(r, d[2], d[5]);
            return;
        }
        break;
    default:
        break;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-pbay: unimplemented tunnelled "
                  "request 0x%02x,0x%02x\n", netfn, cmd);
    reply_error(r, req, CC_INVALID);
}

/* The link commands the application's table holds, 0x20 aside. */
static bool pbay_in_table(uint8_t cmd)
{
    switch (cmd) {
    case CMD_ADDRESS:
    case CMD_STATUS_A:
    case CMD_STATE:
    case CMD_SILENT:
    case CMD_BLOCK:
    case CMD_ASSIGN:
    case CMD_TYPE:
    case CMD_VERSION:
    case CMD_SILENT_A2:
        return true;
    default:
        return false;
    }
}

static void pbay_handle(SGIPbayState *s, const uint8_t *req, unsigned len)
{
    uint16_t dst = lduw_le_p(req + PBAY_OFF_DST);
    uint8_t cmd = req[PBAY_OFF_CMD];
    PbayReply r = { .len = 0 };

    /* Only the address and version requests are answered for any bay. */
    if (dst != SGI_PBAY_ADDRESS && cmd != CMD_ADDRESS && cmd != CMD_VERSION) {
        return;
    }

    if (cmd == CMD_TUNNEL) {
        pbay_tunnel(s, req, len, &r);
        if (r.len) {
            pbay_send(s, r.buf, r.len);
        }
        return;
    }

    if (s->frame_status == PBAY_STATUS_BAD && pbay_in_table(cmd)) {
        /* 0xa2 stays silent; 0x10 and the rest answer 0xff. */
        if (cmd != CMD_SILENT_A2) {
            reply_error(&r, req, PBAY_STATUS_BAD);
            pbay_send(s, r.buf, r.len);
        }
        return;
    }

    reply_begin(&r, req);
    reply_byte(&r, 0);          /* completion code */
    switch (cmd) {
    case CMD_ADDRESS:
        /* The one reply whose source is fixed, whatever was addressed. */
        r.buf[PBAY_OFF_SRC] = SGI_PBAY_ADDRESS & 0xff;
        r.buf[PBAY_OFF_SRC + 1] = SGI_PBAY_ADDRESS >> 8;
        reply_byte(&r, SGI_PBAY_ADDRESS & 0xff);
        reply_byte(&r, SGI_PBAY_ADDRESS >> 8);
        break;
    case CMD_STATUS_A:
        reply_byte(&r, 0x01);
        reply_byte(&r, 0x08);
        reply_byte(&r, 0x02);
        break;
    case CMD_STATE:
        reply_byte(&r, pbay_state_bit(s->state_code));
        break;
    case CMD_SILENT:
    case CMD_SILENT_A2:
        return;
    case CMD_BLOCK:
        reply_zeros(&r, 16);
        break;
    case CMD_ASSIGN:
        break;
    case CMD_TYPE:
        reply_byte(&r, PBAY_DEVICE_TYPE);
        break;
    case CMD_VERSION:
        for (unsigned i = 0; i < sizeof(pbay_version_reply); i++) {
            reply_byte(&r, pbay_version_reply[i]);
        }
        reply_byte(&r, s->flash_chunk);
        break;
    case CMD_FLASH_DATA:
    case CMD_FLASH_END:
        qemu_log_mask(LOG_UNIMP, "sgi-pbay: unimplemented command 0x%02x, "
                      "answered by the flash loader, which is not "
                      "modelled\n", cmd);
        reply_error(&r, req, CC_INVALID);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-pbay: unimplemented command 0x%02x\n",
                      cmd);
        reply_error(&r, req, CC_INVALID);
        break;
    }
    pbay_send(s, r.buf, r.len);
}

/* A complete unstuffed frame: check its length and checksum, then act. */
static void pbay_frame(SGIPbayState *s)
{
    unsigned body = s->rx_len - PBAY_CHECKSUM;
    uint16_t sum;

    if (s->rx_len < PBAY_HEADER + PBAY_CHECKSUM) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pbay: frame of %u bytes dropped, shorter than a "
                      "header and checksum\n", s->rx_len);
        return;
    }
    sum = lduw_le_p(s->rx + body);
    if (sum != pbay_checksum(s->rx, body)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pbay: frame dropped, checksum 0x%04x, expected "
                      "0x%04x\n", sum, pbay_checksum(s->rx, body));
        return;
    }
    pbay_handle(s, s->rx, body);
}

static void pbay_discard(SGIPbayState *s)
{
    s->rx_len = 0;
    s->rx_escape = false;
    s->rx_discard = false;
}

static void pbay_rx_byte(SGIPbayState *s, uint8_t b)
{
    if (b == PBAY_START) {
        /* A start byte only opens a frame; one inside a frame resyncs. */
        if (s->rx_len || s->rx_escape) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-pbay: start byte inside a frame, partial "
                          "frame dropped\n");
        }
        pbay_discard(s);
        return;
    }
    if (b == PBAY_END) {
        if (s->rx_discard) {
            /* The frame was already reported. */
        } else if (s->rx_escape) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-pbay: frame ended inside an escape\n");
        } else {
            pbay_frame(s);
        }
        pbay_discard(s);
        return;
    }
    if (s->rx_discard) {
        return;
    }
    if (s->rx_escape) {
        s->rx_escape = false;
        switch (b) {
        case PBAY_ESC_START:
            b = PBAY_START;
            break;
        case PBAY_ESC_END:
            b = PBAY_END;
            break;
        case PBAY_ESC_ESCAPE:
            b = PBAY_ESCAPE;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-pbay: bad escape 0xaa 0x%02x, frame "
                          "dropped\n", b);
            s->rx_discard = true;
            return;
        }
    } else if (b == PBAY_ESCAPE) {
        s->rx_escape = true;
        return;
    }
    if (s->rx_len == sizeof(s->rx)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-pbay: frame longer than %zu bytes dropped\n",
                      sizeof(s->rx));
        s->rx_discard = true;
        return;
    }
    s->rx[s->rx_len++] = b;
}

static int pbay_can_receive(void *opaque)
{
    return sizeof(((SGIPbayState *)opaque)->rx);
}

static void pbay_receive(void *opaque, const uint8_t *buf, int size)
{
    SGIPbayState *s = opaque;

    for (int i = 0; i < size; i++) {
        pbay_rx_byte(s, buf[i]);
    }
}

static void pbay_reset_hold(Object *obj, ResetType type)
{
    SGIPbayState *s = SGI_PBAY(obj);

    pbay_discard(s);
}

static void pbay_realize(DeviceState *dev, Error **errp)
{
    SGIPbayState *s = SGI_PBAY(dev);

    if (s->flash_chunk == 0 || s->flash_chunk > SGI_PBAY_FLASH_CHUNK_MAX) {
        error_setg(errp, "flash-chunk must be 1 to 0x%x",
                   SGI_PBAY_FLASH_CHUNK_MAX);
        return;
    }
    if (s->state_code & (s->state_code - 1)) {
        error_setg(errp, "state-code must be zero or a single bit");
        return;
    }
    qemu_chr_fe_set_handlers(&s->chr, pbay_can_receive, pbay_receive,
                             NULL, NULL, s, NULL, true);
    /* The device has no bus, so nothing else reaches its reset. */
    qemu_register_resettable(OBJECT(dev));
}

static void pbay_unrealize(DeviceState *dev)
{
    SGIPbayState *s = SGI_PBAY(dev);

    qemu_unregister_resettable(OBJECT(dev));
    qemu_chr_fe_set_handlers(&s->chr, NULL, NULL, NULL, NULL, NULL, NULL,
                             true);
}

static void pbay_init(Object *obj)
{
    SGIPbayState *s = SGI_PBAY(obj);

    /* Read-only, for tests: how far the current frame has come. */
    object_property_add_uint32_ptr(obj, "rx-len", &s->rx_len,
                                   OBJ_PROP_FLAG_READ);
    /* The pins the sensors read; settable while running. */
    object_property_add_uint8_ptr(obj, "input-snapshot", &s->input_snapshot,
                                  OBJ_PROP_FLAG_READWRITE);
    /*
     * The status the loader leaves with a frame: only 0xff changes the
     * replies. What makes the loader set it is not established, so a test
     * sets it.
     */
    object_property_add_uint8_ptr(obj, "frame-status", &s->frame_status,
                                  OBJ_PROP_FLAG_READWRITE);
}

static int pbay_post_load(void *opaque, int version_id)
{
    SGIPbayState *s = opaque;

    return s->rx_len <= sizeof(s->rx) ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_sgi_pbay = {
    .name = "sgi-pbay",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pbay_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(rx, SGIPbayState, SGI_PBAY_MAX_FRAME),
        VMSTATE_UINT32(rx_len, SGIPbayState),
        VMSTATE_BOOL(rx_escape, SGIPbayState),
        VMSTATE_BOOL(rx_discard, SGIPbayState),
        VMSTATE_UINT8(input_snapshot, SGIPbayState),
        VMSTATE_UINT8(frame_status, SGIPbayState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_pbay_properties[] = {
    DEFINE_PROP_CHR("chardev", SGIPbayState, chr),
    DEFINE_PROP_UINT8("flash-chunk", SGIPbayState, flash_chunk,
                      SGI_PBAY_FLASH_CHUNK_DEFAULT),
    DEFINE_PROP_UINT8("state-code", SGIPbayState, state_code, 0),
};

static void sgi_pbay_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI Origin 3000 power-bay controller";
    dc->realize = pbay_realize;
    dc->unrealize = pbay_unrealize;
    dc->vmsd = &vmstate_sgi_pbay;
    device_class_set_props(dc, sgi_pbay_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    rc->phases.hold = pbay_reset_hold;
}

static const TypeInfo sgi_pbay_types[] = {
    {
        .name          = TYPE_SGI_PBAY,
        .parent        = TYPE_DEVICE,
        .instance_size = sizeof(SGIPbayState),
        .instance_init = pbay_init,
        .class_init    = sgi_pbay_class_init,
    },
};

DEFINE_TYPES(sgi_pbay_types)
