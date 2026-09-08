/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Protocol responder for the processor-facing Origin 300 L1 UART. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-l1.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/runstate.h"

static uint8_t l1_irq_cause(SGIL1State *s)
{
    if ((s->ier & 1) && s->queue_len) {
        return 4;
    }
    if ((s->ier & 2) && s->thre_pending) {
        return 2;
    }
    return 1;
}

static void l1_update_irq(SGIL1State *s)
{
    qemu_set_irq(s->uart, l1_irq_cause(s) != 1);
}

static uint16_t l1_fcs(uint16_t value, uint8_t byte)
{
    value ^= byte;
    for (unsigned i = 0; i < 8; i++) {
        value = (value >> 1) ^ ((value & 1) ? 0x8408 : 0);
    }
    return value;
}

static void l1_emit(SGIL1State *s, const uint8_t *payload, unsigned length)
{
    uint8_t encoded[2 * SGI_L1_FRAME_SIZE + 2];
    uint16_t crc = 0xffff;
    unsigned n = 0;

    assert(length <= 127);
    for (unsigned i = 0; i < length; i++) {
        crc = l1_fcs(crc, payload[i]);
    }
    crc = ~crc;
    encoded[n++] = 0x7e;
    for (unsigned i = 0; i < length + 2; i++) {
        uint8_t byte = i < length ? payload[i] :
                       i == length ? crc : crc >> 8;
        if (byte == 0x7e || byte == 0x7d) {
            encoded[n++] = 0x7d;
            byte ^= 0x20;
        }
        encoded[n++] = byte;
    }
    encoded[n++] = 0x7e;
    if (n > SGI_L1_QUEUE_SIZE - s->queue_len) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-l1: receive queue full\n");
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        s->queue[(s->queue_head + s->queue_len++) % SGI_L1_QUEUE_SIZE] =
            encoded[i];
    }
    l1_update_irq(s);
}

static void l1_reply(SGIL1State *s, uint8_t channel, int32_t code,
                     unsigned count, const uint8_t *fields, unsigned length)
{
    uint8_t payload[127];

    assert(length <= sizeof(payload) - 6);
    payload[0] = 0x20 | channel;
    stl_be_p(payload + 1, code);
    payload[5] = count;
    if (length) {
        memcpy(payload + 6, fields, length);
    }
    l1_emit(s, payload, length + 6);
}

/* Reject malformed typed arguments before any command changes state. */
static bool l1_arguments(const uint8_t *p, unsigned length, unsigned count)
{
    unsigned offset = 0;

    for (unsigned i = 0; i < count; i++) {
        uint8_t tag;
        if (offset == length) {
            return false;
        }
        tag = p[offset++];
        if (tag == 0) {
            if (length - offset < 4) {
                return false;
            }
            offset += 4;
        } else if (tag == 1) {
            const uint8_t *end = memchr(p + offset, 0, length - offset);
            if (!end) {
                return false;
            }
            offset = end - p + 1;
        } else if (tag & 0x80) {
            if (length - offset < (tag & 0x7f)) {
                return false;
            }
            offset += tag & 0x7f;
        } else {
            return false;
        }
    }
    return offset == length;
}

static void l1_eeprom(SGIL1State *s, uint8_t channel, const uint8_t *args)
{
    uint32_t component = ldl_be_p(args + 1);
    uint32_t area = ldl_be_p(args + 6);
    uint32_t offset = ldl_be_p(args + 11);
    uint32_t requested = ldl_be_p(args + 16);
    unsigned index, length;
    SGIL1EEPROM *record;
    uint8_t fields[121];

    if (args[0] || args[5] || args[10] || args[15]) {
        l1_reply(s, channel, -100, 0, NULL, 0);
        return;
    }
    if (component >= 16 || (area != 1 && area != 2 && area != 4)) {
        l1_reply(s, channel, -104, 0, NULL, 0);
        return;
    }
    index = area == 1 ? 0 : area == 2 ? 1 : 2;
    record = &s->records[index * 16 + component];
    if (!record->length) {
        l1_reply(s, channel, -104, 0, NULL, 0);
        return;
    }
    if (area == 4) {
        uint8_t sum = 0;
        for (unsigned i = 0; i < 64; i++) {
            sum += record->data[i];
        }
        if (sum) {
            l1_reply(s, channel, -107, 0, NULL, 0);
            return;
        }
    }
    if (offset > record->length) {
        l1_reply(s, channel, -105, 0, NULL, 0);
        return;
    }
    length = MIN(requested, record->length - offset);
    if (length > 115) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: EEPROM reply exceeds frame capacity\n");
        l1_reply(s, channel, -105, 0, NULL, 0);
        return;
    }
    fields[0] = 0;
    stl_be_p(fields + 1, record->length - offset - length);
    if (requested) {
        fields[5] = 0x80 | length;
        memcpy(fields + 6, record->data + offset, length);
        l1_reply(s, channel, 0, 2, fields, 6 + length);
    } else {
        l1_reply(s, channel, 0, 1, fields, 5);
    }
}

static void l1_console_write(SGIL1State *s, const char *text)
{
    qemu_chr_fe_write_all(&s->chr, (const uint8_t *)text, strlen(text));
}

static void l1_prompt(SGIL1State *s)
{
    g_autofree char *text = g_strdup_printf("%03uc%02u-L1> ", s->rack, s->bay);
    l1_console_write(s, text);
}

static void l1_enter_console(SGIL1State *s, bool returning)
{
    g_autofree char *text = g_strdup_printf(
        "%s console mode %03uc%02u console, <CTRL-T> to escape to L1\r\n",
        returning ? "re-entering" : "entering", s->rack, s->bay);
    s->console_mode = true;
    s->return_to_console = false;
    l1_console_write(s, text);
}

static void l1_console_command(SGIL1State *s)
{
    const char *command = (char *)s->command;

    s->command[s->command_len] = 0;
    if (!strcmp(command, "l1")) {
        s->return_to_console = false;
    } else if (!strcmp(command, "version")) {
        if (s->revision_major < 0) {
            l1_console_write(s, "L1 protocol responder; "
                             "firmware revision unavailable\r\n");
        } else {
            g_autofree char *text = g_strdup_printf(
                "L1 %d.%d.%d (protocol responder)\r\n",
                s->revision_major, s->revision_minor, s->revision_patch);
            l1_console_write(s, text);
        }
    } else {
        l1_console_write(s, "command unavailable in protocol responder\r\n");
        qemu_log_mask(LOG_UNIMP, "sgi-l1: unsupported operator command\n");
    }
    s->command_len = 0;
    if (s->return_to_console) {
        l1_enter_console(s, true);
    } else {
        l1_prompt(s);
    }
}

static int l1_console_can_receive(void *opaque)
{
    SGIL1State *s = opaque;

    /* One input byte can expand to an eight-byte escaped console frame. */
    return SGI_L1_QUEUE_SIZE - s->queue_len >= 8;
}

static void l1_console_receive(void *opaque, const uint8_t *buf, int size)
{
    SGIL1State *s = opaque;

    for (int i = 0; i < size; i++) {
        uint8_t byte = buf[i];
        if (s->console_mode) {
            if (byte == 0x14) {
                s->console_mode = false;
                s->return_to_console = true;
                l1_console_write(s, "escaping to L1 system controller\r\n");
                l1_prompt(s);
            } else {
                uint8_t payload[] = { 4, byte };
                l1_emit(s, payload, sizeof(payload));
            }
        } else if (byte == 4) {
            s->command_len = 0;
            l1_enter_console(s, false);
        } else if (byte == '\r' || byte == '\n') {
            l1_console_command(s);
        } else if (byte == 8 || byte == 0x7f) {
            if (s->command_len) {
                s->command_len--;
            }
        } else if (s->command_len < sizeof(s->command) - 1) {
            s->command[s->command_len++] = byte;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-l1: operator command too long\n");
        }
    }
}

static void l1_console_event(void *opaque, QEMUChrEvent event)
{
    SGIL1State *s = opaque;

    if (event == CHR_EVENT_OPENED && !s->console_mode) {
        l1_prompt(s);
    }
}

static bool l1_route(SGIL1State *s, uint32_t address)
{
    unsigned type = address >> 28;
    unsigned rack = (address >> 18) & 0x3ff;
    unsigned bay = (address >> 12) & 0x3f;
    unsigned task = address & 0x1f;

    if (address & 0xfe0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-l1: reserved destination bits set: 0x%08x\n",
                      address);
        return false;
    }
    rack = rack == 0x3ff ? s->rack : rack;
    bay = bay == 0x3f ? s->bay : bay;
    if ((type != 0 && type != 4) || rack != s->rack || bay != s->bay ||
        (type == 0 && task != 3 && task != 6) ||
        (type == 4 && task != 6)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: destination not modeled: 0x%08x\n", address);
        return false;
    }
    return true;
}

static bool l1_task_command(uint32_t address, uint16_t command)
{
    if ((address & 0x1f) == 3) {
        return command == 0;
    }
    if ((address >> 28) == 4) {
        return command == 3;
    }
    switch (command) {
    case 1:
    case 9:
    case 0xa:
    case 2:
    case 3:
    case 5:
    case 6:
    case 0xe:
    case 0x1002:
    case 0x1004:
    case 0x1005:
        return true;
    default:
        return false;
    }
}

static void l1_request(SGIL1State *s)
{
    const uint8_t *p = s->frame;
    unsigned length = s->frame_len - 2;
    uint8_t channel = p[0] & 0x1f;
    uint32_t address;
    uint16_t command;
    unsigned count, args_len;
    const uint8_t *args;
    int32_t error = 0;
    uint8_t fields[15];
    unsigned field_count = 0, field_length = 0;

    if ((p[0] & 0xe0) == 0x40 && channel == 4) {
        if (s->console_mode) {
            qemu_chr_fe_write_all(&s->chr, p + 1, length - 1);
        } else {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: console output while operator is in L1 mode\n");
        }
        return;
    }
    if ((p[0] & 0xe0) != 0 || channel < 16 || length < 7) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: unsupported packet type or channel\n");
        return;
    }
    address = ldl_be_p(p + 1);
    command = lduw_be_p(p + 5);
    if (!l1_route(s, address)) {
        return;
    }
    if (!l1_task_command(address, command)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: request 0x%04x unavailable at 0x%08x\n",
                      command, address);
        l1_reply(s, channel, -101, 0, NULL, 0);
        return;
    }
    count = length == 7 ? 0 : p[7];
    args = p + MIN(length, 8);
    args_len = length > 8 ? length - 8 : 0;
    if (!l1_arguments(args, args_len, count)) {
        l1_reply(s, channel, -100, 0, NULL, 0);
        return;
    }
    switch (command) {
    case 1:
    case 9:
    case 0xa:
        if (count) {
            error = -100;
            break;
        }
        /* Fixed values for the single-chassis protocol model. */
        field_count = command == 0xa ? 2 : 1;
        field_length = 5 * field_count;
        memset(fields, 0, field_length);
        break;
    case 0xe:
        /* The firmware reads no argument here, so extra ones are answered. */
        fields[0] = 0;
        stl_be_p(fields + 1, (s->port_field >> 5) & 3);
        field_count = 1;
        field_length = 5;
        break;
    case 0x1002:
        if (count != 1) {
            error = -100;
            break;
        }
        if (args[0] != 0) {
            error = -105;
            break;
        }
        s->port_field = ((s->port_field & 0x60) | ldl_be_p(args + 1)) & 0x7f;
        break;
    case 0x1004:
    case 0x1005:
        if (count != 1 || args[0] != 1) {
            error = -100;
            break;
        }
        pstrcpy((char *)s->display[command - 0x1004], 128,
                (const char *)args + 1);
        break;
    case 2:
    case 3:
        if (count != 0) {
            error = -100;
            break;
        }
        fields[0] = 0;
        stl_be_p(fields + 1, (s->rack << 6) | s->bay);
        field_count = 1;
        field_length = 5;
        if (command == 3) {
            fields[5] = 0;
            stl_be_p(fields + 6, (address >> 28) == 4 ? 0x34 : 'C');
            field_count = 2;
            field_length = 10;
        }
        break;
    case 5:
        if (count) {
            error = -100;
            break;
        }
        if (s->revision_major < 0) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: firmware revision metadata not configured\n");
            error = -104;
            break;
        }
        fields[0] = fields[5] = fields[10] = 0;
        stl_be_p(fields + 1, s->revision_major);
        stl_be_p(fields + 6, s->revision_minor);
        stl_be_p(fields + 11, s->revision_patch);
        field_count = 3;
        field_length = 15;
        break;
    case 6:
        if (count != 4 || args_len != 20) {
            error = -100;
            break;
        }
        l1_eeprom(s, channel, args);
        return;
    case 0:
        if ((address & 0x1f) != 3) {
            error = -101;
        } else if (count != 1 || args[0] != 1) {
            error = -100;
        } else if (!strcmp((const char *)args + 1, "* pwr d")) {
            l1_reply(s, channel, 0, 0, NULL, 0);
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            return;
        } else {
            error = -101;
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-l1: request 0x%04x unavailable\n",
                      command);
        error = -101;
        break;
    }
    l1_reply(s, channel, error, field_count, fields, field_length);
}

static void l1_parse(SGIL1State *s, uint8_t byte)
{
    if (byte == 0x7e) {
        if (s->in_frame && !s->overflow && !s->escape && s->frame_len >= 3) {
            uint16_t crc = 0xffff;
            for (unsigned i = 0; i < s->frame_len; i++) {
                crc = l1_fcs(crc, s->frame[i]);
            }
            if (crc == 0xf0b8) {
                l1_request(s);
            }
        }
        s->in_frame = true;
        s->frame_len = 0;
        s->escape = false;
        s->overflow = false;
        return;
    }
    if (!s->in_frame || s->overflow) {
        return;
    }
    if (!s->escape && byte == 0x7d) {
        s->escape = true;
        return;
    }
    if (s->escape) {
        byte ^= 0x20;
        s->escape = false;
    }
    if (s->frame_len == SGI_L1_FRAME_SIZE) {
        s->overflow = true;
        return;
    }
    s->frame[s->frame_len++] = byte;
}

static uint64_t l1_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIL1State *s = opaque;
    uint8_t value;

    switch (offset) {
    case 0:
        if (s->lcr & 0x80) {
            return s->divisor[0];
        }
        if (!s->queue_len) {
            return 0;
        }
        value = s->queue[s->queue_head];
        s->queue_head = (s->queue_head + 1) % SGI_L1_QUEUE_SIZE;
        s->queue_len--;
        l1_update_irq(s);
        qemu_chr_fe_accept_input(&s->chr);
        return value;
    case 8:
        return s->lcr & 0x80 ? s->divisor[1] : s->ier;
    case 0x10:
        value = l1_irq_cause(s);
        if (value == 2) {
            s->thre_pending = false;
            l1_update_irq(s);
        }
        return value;
    case 0x18:
        return s->lcr;
    case 0x20:
        return s->mcr;
    case 0x28:
        return 0x60 | (s->queue_len != 0);
    case 0x30:
        return 0x10;
    case 0x38:
        return s->scratch;
    default:
        g_assert_not_reached();
    }
}

static void l1_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SGIL1State *s = opaque;

    switch (offset) {
    case 0:
        if (s->lcr & 0x80) {
            s->divisor[0] = value;
        } else {
            s->thre_pending = false;
            l1_update_irq(s);
            l1_parse(s, value);
            s->thre_pending = (s->ier & 2) != 0;
        }
        break;
    case 8:
        if (s->lcr & 0x80) {
            s->divisor[1] = value;
        } else {
            if ((value & 2) && !(s->ier & 2)) {
                s->thre_pending = true;
            }
            s->ier = value;
            if (value & 0xfc) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-l1: unsupported UART interrupt enables\n");
            }
        }
        break;
    case 0x10:
        if (value & 2) {
            s->queue_head = s->queue_len = 0;
        }
        break;
    case 0x18:
        s->lcr = value;
        break;
    case 0x20:
        s->mcr = value;
        break;
    case 0x38:
        s->scratch = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-l1: write to read-only UART register\n");
        break;
    }
    l1_update_irq(s);
}

static const MemoryRegionOps l1_ops = {
    .read = l1_read,
    .write = l1_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void l1_reset_enter(Object *obj, ResetType type)
{
    SGIL1State *s = SGI_L1(obj);

    s->lcr = s->ier = s->mcr = s->scratch = 0;
    s->thre_pending = false;
    memset(s->divisor, 0, sizeof(s->divisor));
    s->frame_len = s->queue_head = s->queue_len = 0;
    s->in_frame = s->escape = s->overflow = false;
    s->console_mode = s->return_to_console = false;
    s->command_len = 0;
    memset(s->display, 0, sizeof(s->display));
    s->port_field = s->node_position << 5;
}

static void l1_reset_hold(Object *obj, ResetType type)
{
    l1_update_irq(SGI_L1(obj));
}

static int l1_post_load(void *opaque, int version_id)
{
    SGIL1State *s = opaque;

    if (s->command_len >= sizeof(s->command) ||
        s->frame_len > SGI_L1_FRAME_SIZE ||
        s->queue_len > SGI_L1_QUEUE_SIZE ||
        s->queue_head >= SGI_L1_QUEUE_SIZE ||
        s->display[0][127] || s->display[1][127]) {
        return -EINVAL;
    }
    for (unsigned i = 32; i < 48; i++) {
        if (s->records[i].length && s->records[i].length < 64) {
            return -EINVAL;
        }
    }
    if (version_id < 2) {
        s->thre_pending = false;
    }
    if (version_id < 3) {
        s->port_field = s->node_position << 5;
    } else if (s->port_field > 0x7f) {
        return -EINVAL;
    }
    l1_update_irq(s);
    return 0;
}

static int l1_record_pre_load(void *opaque)
{
    SGIL1EEPROM *record = opaque;
    g_clear_pointer(&record->data, g_free);
    return 0;
}

static bool l1_record_size_valid(void *opaque, int version_id)
{
    return ((SGIL1EEPROM *)opaque)->length <= 65535;
}

static const VMStateDescription vmstate_l1_record = {
    .name = "sgi-l1/eeprom",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = l1_record_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(length, SGIL1EEPROM),
        VMSTATE_VALIDATE("record-size", l1_record_size_valid),
        VMSTATE_VBUFFER_ALLOC_UINT32(data, SGIL1EEPROM, 0, NULL, length),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_l1 = {
    .name = TYPE_SGI_L1,
    .version_id = 3,
    .minimum_version_id = 1,
    .post_load = l1_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(records, SGIL1State, 48, 0,
                             vmstate_l1_record, SGIL1EEPROM),
        VMSTATE_BOOL(console_mode, SGIL1State),
        VMSTATE_BOOL(return_to_console, SGIL1State),
        VMSTATE_UINT8_ARRAY(command, SGIL1State, 128),
        VMSTATE_UINT32(command_len, SGIL1State),
        VMSTATE_UINT8(lcr, SGIL1State),
        VMSTATE_UINT8(ier, SGIL1State),
        VMSTATE_BOOL_V(thre_pending, SGIL1State, 2),
        VMSTATE_UINT8(mcr, SGIL1State),
        VMSTATE_UINT8(scratch, SGIL1State),
        VMSTATE_UINT8_ARRAY(divisor, SGIL1State, 2),
        VMSTATE_UINT8_ARRAY(frame, SGIL1State, SGI_L1_FRAME_SIZE),
        VMSTATE_UINT32(frame_len, SGIL1State),
        VMSTATE_BOOL(in_frame, SGIL1State),
        VMSTATE_BOOL(escape, SGIL1State),
        VMSTATE_BOOL(overflow, SGIL1State),
        VMSTATE_UINT8_ARRAY(queue, SGIL1State, SGI_L1_QUEUE_SIZE),
        VMSTATE_UINT32(queue_head, SGIL1State),
        VMSTATE_UINT32(queue_len, SGIL1State),
        VMSTATE_UINT8_2DARRAY(display, SGIL1State, 2, 128),
        VMSTATE_UINT8_V(port_field, SGIL1State, 3),
        VMSTATE_END_OF_LIST()
    },
};

static void l1_realize(DeviceState *dev, Error **errp)
{
    SGIL1State *s = SGI_L1(dev);

    char **files[] = { s->chassis_files, s->board_files, s->spd_files };
    uint32_t counts[] = { s->chassis_count, s->board_count, s->spd_count };

    if (s->revision_major < -1 || s->revision_minor < -1 ||
        s->revision_patch < -1 ||
        ((s->revision_major < 0 || s->revision_minor < 0 ||
          s->revision_patch < 0) &&
         (s->revision_major >= 0 || s->revision_minor >= 0 ||
          s->revision_patch >= 0))) {
        error_setg(errp, "L1 revision metadata needs all three components");
        return;
    }
    if (s->rack >= 0x3ff || s->bay >= 0x3f) {
        error_setg(errp, "L1 rack or bay must be a concrete destination");
        return;
    }
    if (s->node_position > 3) {
        error_setg(errp, "L1 node-position must be 0 to 3");
        return;
    }
    for (unsigned area = 0; area < 3; area++) {
        if (counts[area] > 16) {
            error_setg(errp, "L1 EEPROM area has more than 16 components");
            return;
        }
        for (unsigned component = 0; component < counts[area]; component++) {
            SGIL1EEPROM *record = &s->records[area * 16 + component];
            gsize length;
            GError *local_error = NULL;
            char *data;

            if (!files[area][component][0]) {
                continue;
            }
            if (!g_file_get_contents(files[area][component], &data,
                                     &length, &local_error)) {
                error_setg(errp, "L1 EEPROM: %s", local_error->message);
                g_error_free(local_error);
                return;
            }
            if (!length || length > 65535 || (area == 2 && length < 64)) {
                error_setg(errp, "L1 EEPROM record length is invalid");
                g_free(data);
                return;
            }
            record->length = length;
            record->data = (uint8_t *)data;
        }
    }
    qemu_chr_fe_set_handlers(&s->chr, l1_console_can_receive,
                             l1_console_receive, l1_console_event, NULL,
                             s, NULL, true);
}

static char *l1_display_get(Object *obj, Error **errp)
{
    return g_strdup((char *)SGI_L1(obj)->display[1]);
}

static char *l1_display_first_get(Object *obj, Error **errp)
{
    return g_strdup((char *)SGI_L1(obj)->display[0]);
}

static const Property l1_properties[] = {
    DEFINE_PROP_CHR("chardev", SGIL1State, chr),
    DEFINE_PROP_INT32("revision-major", SGIL1State, revision_major, -1),
    DEFINE_PROP_INT32("revision-minor", SGIL1State, revision_minor, -1),
    DEFINE_PROP_INT32("revision-patch", SGIL1State, revision_patch, -1),
    DEFINE_PROP_ARRAY("chassis-eeprom", SGIL1State, chassis_count,
                      chassis_files, qdev_prop_string, char *),
    DEFINE_PROP_ARRAY("board-eeprom", SGIL1State, board_count,
                      board_files, qdev_prop_string, char *),
    DEFINE_PROP_ARRAY("spd-eeprom", SGIL1State, spd_count,
                      spd_files, qdev_prop_string, char *),
    DEFINE_PROP_UINT32("rack", SGIL1State, rack, 1),
    DEFINE_PROP_UINT32("bay", SGIL1State, bay, 1),
    DEFINE_PROP_UINT32("node-position", SGIL1State, node_position, 0),
};

static void l1_init(Object *obj)
{
    SGIL1State *s = SGI_L1(obj);

    memory_region_init_io(&s->mmio, obj, &l1_ops, s, TYPE_SGI_L1, SGI_L1_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_out_named(DEVICE(obj), &s->uart, "uart", 1);
}

static void l1_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI L1 processor UART protocol responder";
    dc->realize = l1_realize;
    dc->vmsd = &vmstate_l1;
    dc->user_creatable = true;
    device_class_set_props(dc, l1_properties);
    rc->phases.enter = l1_reset_enter;
    rc->phases.hold = l1_reset_hold;
    object_class_property_add_str(klass, "display-line-1", l1_display_first_get,
                                  NULL);
    object_class_property_add_str(klass, "display-line-2",
                                  l1_display_get, NULL);
}

static void l1_finalize(Object *obj)
{
    SGIL1State *s = SGI_L1(obj);
    for (unsigned i = 0; i < 48; i++) {
        g_free(s->records[i].data);
    }
}

static const TypeInfo l1_info = {
    .name = TYPE_SGI_L1,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIL1State),
    .instance_init = l1_init,
    .instance_finalize = l1_finalize,
    .class_init = l1_class_init,
};

static void l1_register_types(void)
{
    type_register_static(&l1_info);
}

type_init(l1_register_types)
