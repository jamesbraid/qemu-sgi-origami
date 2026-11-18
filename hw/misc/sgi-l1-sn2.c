/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The Altix 350 node's L1 controller on the node's 16550 link; see
 * docs/system/mips/sgi/hardware/sn2-l1.rst.
 */
#include "qemu/osdep.h"
#include "hw/misc/sgi-l1-frame.h"
#include "hw/misc/sgi-l1-sn2.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/resettable.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "system/rtc.h"
#include "system/runstate.h"
#include "trace.h"

/* The class byte and up to 127 text bytes, then two check bytes. */
#define L1_SN2_FRAME_CAPACITY (128 + 2)
/* Encoded replies waiting for the node UART's receive FIFO. */
#define L1_SN2_TX_SIZE 1024
#define L1_SN2_COMMAND_SIZE 128

#define TYPE_SGI_L1_SN2_LINK "chardev-sgi-l1-sn2"
OBJECT_DECLARE_SIMPLE_TYPE(SGIL1SN2Link, SGI_L1_SN2_LINK)

struct SGIL1SN2State {
    DeviceState parent_obj;
    CharFrontend chr;
    Chardev *link;
    uint32_t cpus;
    uint32_t rack;
    uint32_t bay;
    uint32_t partition;
    SGIL1FrameState frame;
    /* The console index the last output used; typed input goes back to it. */
    uint8_t port_field;
    bool escaped;
    uint8_t command[L1_SN2_COMMAND_SIZE];
    uint32_t command_len;
    uint32_t tx_len;
    uint8_t tx[L1_SN2_TX_SIZE];
};

struct SGIL1SN2Link {
    Chardev parent_obj;
    SGIL1SN2State *controller;
};

/* A guest reset or power-off request: the main loop acts on it. */
static void l1_sn2_request_action(bool shutdown)
{
    if (shutdown) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    } else {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

/* The node's 16550 takes only what its receive FIFO has room for. */
static void l1_sn2_pump(SGIL1SN2State *s)
{
    while (s->tx_len) {
        int room = qemu_chr_be_can_write(s->link);
        unsigned n = MIN(room > 0 ? room : 0, s->tx_len);

        if (!n) {
            return;
        }
        qemu_chr_be_write(s->link, s->tx, n);
        s->tx_len -= n;
        memmove(s->tx, s->tx + n, s->tx_len);
    }
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_accept_input(&s->chr);
    }
}

static void l1_sn2_emit(SGIL1SN2State *s, const uint8_t *payload,
                        unsigned length)
{
    uint8_t encoded[2 * (128 + 2) + 2];
    unsigned n;

    /* The firmware's frame is 128 payload bytes including the header. */
    assert(length <= 128);
    /* The SN2 link escapes neither CR nor XON/XOFF. */
    n = sgi_l1_frame_encode(encoded, payload, length, false);
    if (n > sizeof(s->tx) - s->tx_len) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1-sn2: node link reply queue full, "
                      "reply dropped\n");
        return;
    }
    memcpy(s->tx + s->tx_len, encoded, n);
    s->tx_len += n;
    l1_sn2_pump(s);
}

static void l1_sn2_reply(SGIL1SN2State *s, uint8_t channel, int32_t code,
                         unsigned count, const uint8_t *fields,
                         unsigned length)
{
    uint8_t payload[128];

    assert(length <= sizeof(payload) - 6);
    payload[0] = 0x20 | channel;
    stl_be_p(payload + 1, code);
    payload[5] = count;
    if (length) {
        memcpy(payload + 6, fields, length);
    }
    l1_sn2_emit(s, payload, length + 6);
}

/*
 * Console input: one frame per character, header = the console index the
 * last output used, no class bits (inferred from the PROM's receiver).
 */
static int l1_sn2_console_can_read(void *opaque)
{
    SGIL1SN2State *s = opaque;

    return (sizeof(s->tx) - s->tx_len) / 16;
}

/* The operator's L1 mode: Ctrl-T enters it; only "pwr d" is modelled. */
static void l1_sn2_operator_byte(SGIL1SN2State *s, uint8_t byte)
{
    if (byte == '\r' || byte == '\n') {
        s->command[s->command_len] = 0;
        s->escaped = false;
        s->command_len = 0;
        if (!strcmp((char *)s->command, "pwr d")) {
            l1_sn2_request_action(true);
            return;
        }
        qemu_log_mask(LOG_UNIMP, "sgi-l1-sn2: operator command \"%s\" is not "
                      "modelled\n", (char *)s->command);
    } else if (byte == 4) {
        s->escaped = false;
        s->command_len = 0;
    } else if (byte == 8 || byte == 0x7f) {
        if (s->command_len) {
            s->command_len--;
        }
    } else if (s->command_len < sizeof(s->command) - 1) {
        s->command[s->command_len++] = byte;
    }
}

static void l1_sn2_console_read(void *opaque, const uint8_t *buf, int size)
{
    SGIL1SN2State *s = opaque;

    warn_report_once("sgi-l1-sn2: console input frames are inferred");
    for (int i = 0; i < size; i++) {
        uint8_t payload[2] = { s->port_field & 0x1f, buf[i] };

        if (s->escaped) {
            l1_sn2_operator_byte(s, buf[i]);
            continue;
        }
        if (buf[i] == 0x14) {
            s->escaped = true;
            s->command_len = 0;
            continue;
        }
        l1_sn2_emit(s, payload, sizeof(payload));
    }
}

/* Micron MT9VDDT6472A 512 MB SPD: datasheet Table 21, bytes 0 to 71. */
#define L1_SN2_SPD_SIZE 256
static const uint8_t l1_sn2_spd[L1_SN2_SPD_SIZE] = {
    0x80, 0x08, 0x07, 0x0d, 0x0b, 0x01, 0x48, 0x00,
    0x04, 0x50, 0x70, 0x02, 0x82, 0x08, 0x08, 0x01,
    0x0e, 0x04, 0x1c, 0x01, 0x02, 0x20, 0xc0, 0x60,
    0x70, 0x75, 0x75, 0x3c, 0x28, 0x3c, 0x28, 0x80,
    0x60, 0x60, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x37, 0x46, 0x30, 0x28, 0x50, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0xd3,
    0x2c, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

/* Argument-free requests: a blank L1's answers (sn2-l1.rst, "Replies"). */
static const struct {
    uint16_t command;
    int32_t status;
    uint8_t count;
    uint32_t value[4];
} l1_sn2_plain[] = {
    { 0x0001, 0, 1, { 0 } },
    /* The module word and partition follow the rack, bay and partition. */
    { 0x0003, 0, 2, { 0, 'O' } },
    { 0x0008, -104, 0, { 0 } },
    /* Nodes, processors, then presence and enable of the one node. */
    { 0x0009, 0, 1, { 0 } },
    { 0x0010, 0, 4, { 1, 1, 1, 1 } },
    { 0x000e, 0, 1, { 0 } },
    { 0x0012, 0, 1, { 0 } },
    { 0x0013, 0, 2, { 0, 'O' } },
    { 0x0015, 0, 1, { 0 } },
};

/* The node's 512-byte IPMI FRU: a header and an empty board area. */
static const uint8_t l1_sn2_fru[512] = {
    0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xfe,
    0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xc0,
    0xc0, 0xc0, 0xc0, 0xc1, 0x00, 0x00, 0x00, 0x7c,
};

/*
 * The node's chassis area (selector 1): a common header and an empty chassis
 * information area, each summing to zero; contents unknown.
 */
static const uint8_t l1_sn2_chassis_fru[512] = {
    0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xfe,
    0x01, 0x01, 0x00, 0xc0, 0xc0, 0xc1, 0x00, 0xbd,
};

/* Answer a read of up to 64 bytes: the bytes left, then the raw window. */
static bool l1_sn2_window(SGIL1SN2State *s, uint8_t channel,
                          const uint8_t *data, unsigned size, uint32_t offset,
                          uint32_t length)
{
    uint8_t fields[70];
    unsigned n, left;

    if (offset >= size) {
        l1_sn2_reply(s, channel, -105, 0, NULL, 0);
        return true;
    }
    n = (uint64_t)offset + length > size ? size - offset : length;
    left = size - offset - n;
    if (n > 64) {
        return false;
    }
    fields[0] = 0;
    stl_be_p(fields + 1, left);
    if (n) {
        fields[5] = 0x80 | n;
        memcpy(fields + 6, data + offset, n);
    }
    l1_sn2_reply(s, channel, 0, n ? 2 : 1, fields, n ? 6 + n : 5);
    return true;
}

/* EEPROM read: id, selector, offset, length. False if contents unknown. */
static bool l1_sn2_eeprom(SGIL1SN2State *s, uint8_t channel,
                          const uint8_t *args, unsigned args_len,
                          unsigned count)
{
    uint32_t v[4];

    if (count < 4) {
        l1_sn2_reply(s, channel, -100, 0, NULL, 0);
        return true;
    }
    if (count != 4 || args_len != 20) {
        return false;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (args[i * 5]) {
            return false;
        }
        v[i] = ldl_be_p(args + i * 5 + 1);
    }
    if (v[0] == 1 && v[1] == 2) {
        warn_report_once("sgi-l1-sn2: the board information area is empty "
                         "(approximation)");
        return l1_sn2_window(s, channel, l1_sn2_fru, sizeof(l1_sn2_fru),
                             v[2], v[3]);
    }
    if (v[0] == 1 && v[1] == 1) {
        warn_report_once("sgi-l1-sn2: the chassis area is empty "
                         "(approximation)");
        return l1_sn2_window(s, channel, l1_sn2_chassis_fru,
                             sizeof(l1_sn2_chassis_fru), v[2], v[3]);
    }
    if (v[0] == 98 && v[1] == 2) {
        /* No riser fitted (the strap, *inferred*): the record is absent. */
        warn_report_once("sgi-l1-sn2: no riser record (approximation)");
        l1_sn2_reply(s, channel, -104, 0, NULL, 0);
        return true;
    }
    if (v[0] < 8 || v[0] > 23) {
        return false;
    }
    /* Bank 0 holds the only DIMMs; every other slot reads as empty. */
    if (v[0] != 8 && v[0] != 16 && v[0] != 9 && v[0] != 17) {
        l1_sn2_reply(s, channel, -104, 0, NULL, 0);
        return true;
    }
    if (v[1] != 4) {
        l1_sn2_reply(s, channel, v[1] > 4 ? -105 : -104, 0, NULL, 0);
        return true;
    }
    warn_report_once("sgi-l1-sn2: the DIMM records are a chosen part "
                     "(approximation)");
    return l1_sn2_window(s, channel, l1_sn2_spd, L1_SN2_SPD_SIZE, v[2], v[3]);
}

/* Answer the SN2 node's task-6 requests the PROM sends. */
static bool l1_sn2_request(SGIL1SN2State *s, uint8_t channel, uint32_t address,
                           uint16_t command, const uint8_t *args,
                           unsigned args_len, unsigned count)
{
    for (unsigned i = 0; i < ARRAY_SIZE(l1_sn2_plain); i++) {
        uint8_t fields[20];
        uint32_t value[4];

        if (l1_sn2_plain[i].command != command || count) {
            continue;
        }
        warn_report_once("sgi-l1-sn2: node identity replies are "
                         "approximations");
        memcpy(value, l1_sn2_plain[i].value, sizeof(value));
        if (command == 0x0003) {
            value[0] = (s->rack << 6) | s->bay;
        } else if (command == 0x0009) {
            value[0] = s->partition;
        } else if (command == 0x0010) {
            /* One processor per front-side bus, at slices 0 and 2. */
            value[1] = s->cpus;
            value[2] = value[3] = s->cpus > 1 ? 0x5 : 0x1;
        }
        for (unsigned j = 0; j < l1_sn2_plain[i].count; j++) {
            fields[j * 5] = 0;
            stl_be_p(fields + j * 5 + 1, value[j]);
        }
        l1_sn2_reply(s, channel, l1_sn2_plain[i].status, l1_sn2_plain[i].count,
                 fields, l1_sn2_plain[i].count * 5);
        return true;
    }
    switch (command) {
    case 0x0000:
        /* The PROM probes for an L2 with "version"; this 350 has none. */
        if (address == 0x1ffff003) {
            if (count != 1 || !args_len || args[0] != 1 ||
                !memchr(args + 1, 0, args_len - 1) ||
                strcmp((char *)args + 1, "version")) {
                return false;
            }
            l1_sn2_reply(s, channel, -1, 0, NULL, 0);
            return true;
        }
        /* Reboot: "* rst" (no L2) or "p <partition> rst", to task 3. */
        if (address != 0x0ffff003 || count != 1 || !args_len || args[0] != 1 ||
            !memchr(args + 1, 0, args_len - 1) ||
            (strcmp((char *)args + 1, "* rst") &&
             !(g_str_has_prefix((char *)args + 1, "p ") &&
               g_str_has_suffix((char *)args + 1, " rst")))) {
            return false;
        }
        l1_sn2_reply(s, channel, 0, 0, NULL, 0);
        l1_sn2_request_action(false);
        return true;
    case 0x200e: {
        /* Get the time: six integers, the standard RTC clock's UTC. */
        struct tm tm;
        uint8_t fields[30];
        int v[6];

        if (count) {
            l1_sn2_reply(s, channel, -100, 0, NULL, 0);
            return true;
        }
        qemu_get_timedate(&tm, 0);
        v[0] = tm.tm_year + 1900;
        v[1] = tm.tm_mon + 1;
        v[2] = tm.tm_mday;
        v[3] = tm.tm_hour;
        v[4] = tm.tm_min;
        v[5] = tm.tm_sec;
        for (unsigned i = 0; i < 6; i++) {
            fields[i * 5] = 0;
            stl_be_p(fields + i * 5 + 1, v[i]);
        }
        l1_sn2_reply(s, channel, 0, 6, fields, sizeof(fields));
        return true;
    }
    case 0x200d:
        /* Set the time: the 'O' L1's helper does nothing and returns 0. */
        l1_sn2_reply(s, channel, 0, 0, NULL, 0);
        return true;
    case 0x0006:
        return l1_sn2_eeprom(s, channel, args, args_len, count);
    case 0x100b:
        /* ESTIMATE: the real L1 sends no reply; this ends the PROM's wait. */
        if (count != 3) {
            l1_sn2_reply(s, channel, -100, 0, NULL, 0);
            return true;
        }
        qemu_log_mask(LOG_UNIMP, "sgi-l1-sn2: request 0x100b answered by "
                      "approximation, the L1 sends no reply\n");
        trace_sgi_l1_sn2_estimate(command);
        l1_sn2_reply(s, channel, 0, 0, NULL, 0);
        return true;
    case 0x1005:
        /* Display text: the L1 acks it and shows it. */
        if (count < 1) {
            l1_sn2_reply(s, channel, -100, 0, NULL, 0);
            return true;
        }
        if (args[0] == 1) {
            trace_sgi_l1_sn2_display((const char *)args + 1);
        }
        l1_sn2_reply(s, channel, 0, 0, NULL, 0);
        return true;
    case 0x1007:
        /* The event subchannel the kernel opened: acked, no events follow. */
        l1_sn2_reply(s, channel, count == 1 ? 0 : -100, 0, NULL, 0);
        return true;
    default:
        return false;
    }
}

/* The controller tasks the PROM talks to: 6, and 3 with request code 0. */
static bool l1_sn2_routed(uint32_t address, uint16_t command)
{
    return (address & 0x1f) == 6 ||
           ((address & 0x1f) == 3 && !command) ||
           (address == 0x1ffff003 && !command);
}

static void l1_sn2_request_frame(SGIL1SN2State *s, const uint8_t *p,
                                 unsigned length)
{
    uint8_t channel = p[0] & 0x1f;
    uint32_t address;
    uint16_t command;

    if ((p[0] >> 5) == 2 && qemu_chr_fe_backend_connected(&s->chr)) {
        /* Console text from the PROM: the bytes after the class byte. */
        s->port_field = (s->port_field & 0x60) | channel;
        qemu_chr_fe_write_all(&s->chr, p + 1, length - 1);
        return;
    }
    if (p[0] >> 5) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1-sn2: unsupported class=%u index=%u length=%u\n",
                      p[0] >> 5, channel, length);
        return;
    }
    if (length < 7) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-l1-sn2: short request\n");
        return;
    }
    if (length == 7) {
        /* The PROM sends no argument count for a request with none. */
        uint8_t padded[8];

        memcpy(padded, p, 7);
        padded[7] = 0;
        l1_sn2_request_frame(s, padded, 8);
        return;
    }
    address = ldl_be_p(p + 1);
    command = lduw_be_p(p + 5);
    if (trace_event_get_state_backends(TRACE_SGI_L1_REQUEST)) {
        g_autofree char *hex = g_malloc(length * 2 + 1);

        qemu_hexdump_to_buffer(hex, length * 2 + 1, p + 8, length - 8);
        trace_sgi_l1_request("sn2", s->rack, s->bay, 0, address, command,
                             p[7], hex);
    }
    if (command >= 0x3001 && command <= 0x3003) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1-sn2: unsupported chunk code=0x%04x\n",
                      command);
    } else if (!sgi_l1_arguments_valid(p + 8, length - 8, p[7])) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-l1-sn2: malformed arguments code=0x%04x\n", command);
    } else if (!l1_sn2_routed(address, command) ||
               !l1_sn2_request(s, channel, address, command, p + 8,
                               length - 8, p[7])) {
        g_autofree char *hex = g_malloc(length * 2 + 1);

        qemu_hexdump_to_buffer(hex, length * 2 + 1, p + 8, length - 8);
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1-sn2: unsupported request index=%u "
                      "address=0x%08x code=0x%04x args=%u bytes=%s\n",
                      channel, address, command, p[7], hex);
    }
}

/* A frame closed on a good check: serve it, then reopen for the next. */
static void l1_sn2_parse(SGIL1SN2State *s, uint8_t byte)
{
    SGIL1FrameState *frame = &s->frame;
    bool ends_data = byte == 0x7e && frame->in_frame &&
                     (frame->length || frame->escape || frame->overflow);

    if (sgi_l1_frame_parse(frame, L1_SN2_FRAME_CAPACITY, byte)) {
        l1_sn2_request_frame(s, frame->data, frame->length - 2);
        sgi_l1_frame_restart(frame);
    } else if (ends_data) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-l1-sn2: invalid frame\n");
    }
}

static int l1_sn2_link_write(Chardev *chr, const uint8_t *buf, int len)
{
    SGIL1SN2State *s = SGI_L1_SN2_LINK(chr)->controller;

    /* The platform's 16550 calls this synchronously under the BQL. */
    if (s && DEVICE(s)->realized && !resettable_is_in_reset(OBJECT(s))) {
        for (int i = 0; i < len; i++) {
            l1_sn2_parse(s, buf[i]);
        }
    }
    return len;
}

static void l1_sn2_link_accept_input(Chardev *chr)
{
    SGIL1SN2State *s = SGI_L1_SN2_LINK(chr)->controller;

    if (s) {
        l1_sn2_pump(s);
    }
}

static void l1_sn2_link_class_init(ObjectClass *klass, const void *data)
{
    CHARDEV_CLASS(klass)->chr_write = l1_sn2_link_write;
    CHARDEV_CLASS(klass)->chr_accept_input = l1_sn2_link_accept_input;
}

static void l1_sn2_reset_enter(Object *obj, ResetType type)
{
    SGIL1SN2State *s = SGI_L1_SN2(obj);

    memset(&s->frame, 0, sizeof(s->frame));
    s->tx_len = 0;
    s->port_field = 0;
    s->escaped = false;
    s->command_len = 0;
}

static int l1_sn2_post_load(void *opaque, int version_id)
{
    SGIL1SN2State *s = opaque;

    if (s->frame.length > L1_SN2_FRAME_CAPACITY || s->tx_len > sizeof(s->tx) ||
        s->command_len >= sizeof(s->command) || s->port_field > 0x7f) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_l1_sn2 = {
    .name = "sgi-l1-sn2",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = l1_sn2_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_UNSAFE(frame.data, SGIL1SN2State, 0,
                              L1_SN2_FRAME_CAPACITY),
        VMSTATE_UINT32(frame.length, SGIL1SN2State),
        VMSTATE_BOOL(frame.in_frame, SGIL1SN2State),
        VMSTATE_BOOL(frame.escape, SGIL1SN2State),
        VMSTATE_BOOL(frame.overflow, SGIL1SN2State),
        VMSTATE_UINT8(port_field, SGIL1SN2State),
        VMSTATE_BOOL(escaped, SGIL1SN2State),
        VMSTATE_UINT8_ARRAY(command, SGIL1SN2State, L1_SN2_COMMAND_SIZE),
        VMSTATE_UINT32(command_len, SGIL1SN2State),
        VMSTATE_UINT32(tx_len, SGIL1SN2State),
        VMSTATE_UINT8_ARRAY(tx, SGIL1SN2State, L1_SN2_TX_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void l1_sn2_realize(DeviceState *dev, Error **errp)
{
    SGIL1SN2State *s = SGI_L1_SN2(dev);
    Chardev *link;

    if (s->cpus < 1 || s->cpus > 2) {
        error_setg(errp, "sgi-l1-sn2 cpus must be 1 or 2");
        return;
    }
    /* The id is generated; the machine gets the chardev from the accessor. */
    link = qemu_chardev_new(NULL, TYPE_SGI_L1_SN2_LINK, NULL, NULL, errp);
    if (!link) {
        return;
    }
    SGI_L1_SN2_LINK(link)->controller = s;
    s->link = link;
    qemu_chr_be_event(link, CHR_EVENT_OPENED);
    qemu_chr_fe_set_handlers(&s->chr, l1_sn2_console_can_read,
                             l1_sn2_console_read, NULL, NULL, s, NULL, true);
}

Chardev *sgi_l1_sn2_link(SGIL1SN2State *s)
{
    return s->link;
}

static void l1_sn2_unrealize(DeviceState *dev)
{
    SGIL1SN2State *s = SGI_L1_SN2(dev);

    qemu_chr_be_event(s->link, CHR_EVENT_CLOSED);
    SGI_L1_SN2_LINK(s->link)->controller = NULL;
    object_unparent(OBJECT(s->link));
    s->link = NULL;
}

static const Property l1_sn2_properties[] = {
    DEFINE_PROP_CHR("chardev", SGIL1SN2State, chr),
    DEFINE_PROP_UINT32("cpus", SGIL1SN2State, cpus, 1),
    DEFINE_PROP_UINT32("rack", SGIL1SN2State, rack, 1),
    DEFINE_PROP_UINT32("bay", SGIL1SN2State, bay, 1),
    DEFINE_PROP_UINT32("partition", SGIL1SN2State, partition, 0),
};

static void l1_sn2_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI SN2 node L1 controller";
    dc->realize = l1_sn2_realize;
    dc->unrealize = l1_sn2_unrealize;
    dc->vmsd = &vmstate_l1_sn2;
    dc->user_creatable = true;
    dc->hotpluggable = false;
    device_class_set_props(dc, l1_sn2_properties);
    rc->phases.enter = l1_sn2_reset_enter;
}

static const TypeInfo l1_sn2_types[] = {
    {
        .name = TYPE_SGI_L1_SN2_LINK,
        .parent = TYPE_CHARDEV,
        .instance_size = sizeof(SGIL1SN2Link),
        .class_init = l1_sn2_link_class_init,
    },
    {
        .name = TYPE_SGI_L1_SN2,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(SGIL1SN2State),
        .class_init = l1_sn2_class_init,
    },
};

DEFINE_TYPES(l1_sn2_types)
