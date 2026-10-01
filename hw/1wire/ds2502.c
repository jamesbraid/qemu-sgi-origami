/* SPDX-License-Identifier: BSD-3-Clause */
/* DS2501 and DS2502 add-only EPROM memory commands. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "migration/vmstate.h"
#include "hw/1wire/onewire.h"
#include "hw/1wire/crc.h"
#include "hw/1wire/ds2502.h"
#include "trace/trace-hw_1wire.h"

/*
 * DS2502-family add-only EPROM (the DS2502 datasheet, section C.1).
 * Every memory command starts with a 3-byte header: command, TA1 (address
 * low), and TA2 (address high). An echoed CRC-8 covers those bytes. The
 * commands differ in how they frame the data phase:
 *
 *   Read Memory (F0h)          a flat stream, no per-page CRC; if the read
 *                               runs through the last byte of memory
 *                               (007Fh) one CRC-8 over the data read
 *                               follows, and only then.
 *   Read Data+CRC (C3h)        a CRC-8 after every 32-byte page, with no
 *                               defined end. The datasheet never caps it
 *                               at end of memory, so the model does not
 *                               either.
 *   Read Status (AAh)          the 8-byte status field 0000h-0007h, closed
 *                               by one CRC-8 the same way Read Memory's is.
 *
 * F0h and C3h answering identically at a page boundary is exactly the bug
 * this device exists to avoid; the two are kept as visibly separate code
 * paths below rather than unified, so that difference cannot regress
 * silently.
 *
 * Write Memory (0Fh) and Write Status (55h) are refused: these parts are
 * modelled add-only, and the base 1-Wire layer logs LOG_UNIMP for any byte
 * a part's function_byte declines.
 */

#define DS2502_CMD_READ_MEMORY   0xf0
#define DS2502_CMD_READ_DATA_CRC 0xc3
#define DS2502_CMD_READ_STATUS   0xaa
#define DS2502_CMD_WRITE_MEMORY  0x0f
#define DS2502_CMD_WRITE_STATUS  0x55

#define DS2502_STATUS_FACTORY_ADDR 0x0007

/*
 * What the *previous* function_byte call placed on the wire, so this call
 * knows how to account for it before
 * deciding what comes next; see the one-window lag documented on
 * OneWireSlaveClass::function_byte.
 */
typedef enum {
    DS2502_UNIT_DATA,       /* a plain data or status byte */
    DS2502_UNIT_PAGE_CRC,   /* C3h's per-32-byte-page CRC-8 */
    DS2502_UNIT_FINAL_CRC,  /* F0h/AAh's single end-of-field CRC-8 */
} DS2502Unit;

#define TYPE_ONEWIRE_DS2502_FAMILY "onewire-ds2502-family"
OBJECT_DECLARE_TYPE(DS2502State, DS2502Class, ONEWIRE_DS2502_FAMILY)

struct DS2502Class {
    OneWireSlaveClass parent_class;
    uint32_t pages;        /* programmable pages: DS2501 two, DS2502 four */
};

struct DS2502State {
    OneWireSlave parent_obj;

    uint8_t data[DS2502_DATA_SIZE];

    /*
     * Header capture: the command byte, then TA1, then TA2, exactly as
     * received on the wire. The echoed CRC-8 is
     * computed over the address as MASKED rather than over these; they are
     * kept because the mask width is a
     * property of the command, so the raw bytes are what a later window has
     * to re-derive the address from.
     */
    uint8_t cmd;
    uint8_t hdr[3];
    uint8_t hdr_pos;        /* header bytes captured so far, 0..3 */

    bool streaming;         /* header consumed; serving content now */
    uint16_t addr;          /* address of the byte last placed on the wire */
    uint16_t page_end;      /* C3h only: last address of the current page */
    uint8_t crc_acc;        /* running CRC-8 of the segment now in progress */
    uint8_t sent_byte;      /* the data/status byte value last placed */
    /* a DS2502Unit value; fixed width so it migrates */
    uint8_t unit;
    bool finished;          /* nothing further is defined; answer 0xff */
};

static uint8_t ds2502_data_byte(DS2502State *d, uint16_t addr)
{
    return (addr < DS2502_DATA_SIZE) ? d->data[addr] : 0xff;
}

static uint8_t ds2502_status_byte(uint16_t addr)
{
    /*
     * 0007h is factory-programmed 00h; every other status byte
     * (write-protect, the four redirection bytes, and the
     * two reserved bytes) is unprogrammed EPROM, which reads as 0xff.
     */
    return (addr == DS2502_STATUS_FACTORY_ADDR) ? 0x00 : 0xff;
}

/* Read Memory (F0h): flat stream, one CRC-8 only at the true end of memory
 * (007Fh). */
static uint8_t ds2502_advance_read_memory(DS2502State *d)
{
    if (d->unit == DS2502_UNIT_FINAL_CRC) {
        d->finished = true;
        return 0xff;
    }

    d->crc_acc = onewire_crc8(d->crc_acc, &d->sent_byte, 1);

    if (d->addr == DS2502_DATA_SIZE - 1) {
        d->unit = DS2502_UNIT_FINAL_CRC;
        return d->crc_acc;
    }

    d->addr++;
    d->sent_byte = ds2502_data_byte(d, d->addr);
    d->unit = DS2502_UNIT_DATA;
    return d->sent_byte;
}

/*
 * Read Data / Generate 8-bit CRC (C3h): a CRC-8 after every 32-byte page,
 * indefinitely. The datasheet does not cap this command at end of memory.
 */
static uint8_t ds2502_advance_read_data_crc(DS2502State *d)
{
    if (d->unit == DS2502_UNIT_PAGE_CRC) {
        d->crc_acc = 0;
        d->addr++;                              /* now page-aligned */
        d->page_end = d->addr | (DS2502_PAGE_SIZE - 1);
        d->sent_byte = ds2502_data_byte(d, d->addr);
        d->unit = DS2502_UNIT_DATA;
        return d->sent_byte;
    }

    d->crc_acc = onewire_crc8(d->crc_acc, &d->sent_byte, 1);

    if (d->addr == d->page_end) {
        d->unit = DS2502_UNIT_PAGE_CRC;
        return d->crc_acc;
    }

    d->addr++;
    d->sent_byte = ds2502_data_byte(d, d->addr);
    d->unit = DS2502_UNIT_DATA;
    return d->sent_byte;
}

/*
 * Read Status (AAh): the 8-byte field 0000h-0007h, one CRC-8 at the end. A
 * start address already past 0007h leaves
 * the closing CRC unreachable within any bounded transaction: addr is a
 * 16-bit counter that only ever increments,
 * so it would have to wrap all the way through the full 0000h-FFFFh range
 * before landing on 0007h again.
 */
static uint8_t ds2502_advance_read_status(DS2502State *d)
{
    if (d->unit == DS2502_UNIT_FINAL_CRC) {
        d->finished = true;
        return 0xff;
    }

    d->crc_acc = onewire_crc8(d->crc_acc, &d->sent_byte, 1);

    if (d->addr == DS2502_STATUS_FACTORY_ADDR) {
        d->unit = DS2502_UNIT_FINAL_CRC;
        return d->crc_acc;
    }

    d->addr++;
    d->sent_byte = ds2502_status_byte(d->addr);
    d->unit = DS2502_UNIT_DATA;
    return d->sent_byte;
}

static uint8_t ds2502_advance_stream(DS2502State *d)
{
    uint8_t value;

    if (d->finished) {
        trace_ds2502_read(d->cmd, d->addr, d->unit, 0xff);
        return 0xff;
    }

    switch (d->cmd) {
    case DS2502_CMD_READ_MEMORY:
        value = ds2502_advance_read_memory(d);
        break;
    case DS2502_CMD_READ_DATA_CRC:
        value = ds2502_advance_read_data_crc(d);
        break;
    case DS2502_CMD_READ_STATUS:
        value = ds2502_advance_read_status(d);
        break;
    default:
        g_assert_not_reached();
    }
    trace_ds2502_read(d->cmd, d->addr, d->unit, value);
    return value;
}

/*
 * The address the stream reads from, and the one the echoed CRC-8 covers. A
 * memory start address above 007Fh has
 * its 9 MSBs forced to zero (datasheet C.1); no masking is documented for the
 * status field, so AAh keeps the
 * address as sent.
 */
static uint16_t ds2502_start_address(const DS2502State *d)
{
    uint16_t raw = ((uint16_t)d->hdr[2] << 8) | d->hdr[1];

    return d->cmd == DS2502_CMD_READ_STATUS
           ? raw : raw & (DS2502_DATA_SIZE - 1);
}

/*
 * Over the address AS MASKED, which is what makes the datasheet's "deliberate
 * CRC mismatch" true: a master that
 * asked above 007Fh checks its own echo against the bytes it sent, the part
 * echoed the check over the address it
 * will actually read, and the disagreement is how the master finds out.
 * Echoing the check over the raw bytes
 * instead would agree with the master and hand it aliased data as if it were
 * the data it asked for.
 */
static uint8_t ds2502_header_crc(const DS2502State *d)
{
    uint16_t addr = ds2502_start_address(d);
    const uint8_t hdr[3] = { d->cmd, addr & 0xff, addr >> 8 };

    return onewire_crc8(0, hdr, sizeof(hdr));
}

static bool ds2502_function_byte(OneWireSlave *s, uint8_t in, uint8_t *out)
{
    DS2502State *d = ONEWIRE_DS2502_FAMILY(s);

    if (d->cmd == 0) {
        /* The first byte of a fresh memory function: the command. */
        switch (in) {
        case DS2502_CMD_READ_MEMORY:
        case DS2502_CMD_READ_DATA_CRC:
        case DS2502_CMD_READ_STATUS:
            trace_ds2502_command(in, true);
            d->cmd = in;
            d->hdr[0] = in;
            d->hdr_pos = 1;
            *out = 0xff;            /* TA1 is a master write */
            return true;
        case DS2502_CMD_WRITE_MEMORY:
        case DS2502_CMD_WRITE_STATUS:
        default:
            trace_ds2502_command(in, false);
            /*
             * Add-only: writes are refused, not misread as a Read Memory or
             * Read Status. An unrecognised byte is
             * refused the same way. The base layer logs LOG_UNIMP for both.
             */
            return false;
        }
    }

    if (d->hdr_pos < 3) {
        d->hdr[d->hdr_pos++] = in;      /* TA1, then TA2 */
        if (d->hdr_pos < 3) {
            *out = 0xff;                /* TA2 still to come, master write */
        } else {
            /*
             * Both address bytes are in hand: the header CRC-8 lands on the
             * wire in the very next window, right
             * after TA2.
             */
            *out = ds2502_header_crc(d);
            trace_ds2502_header(d->cmd, ds2502_start_address(d), *out);
        }
        return true;
    }

    if (!d->streaming) {
        /*
         * The header CRC window has just been read (in is filler). Start the
         * content stream: the byte right after
         * it is the first data or status byte.
         */
        d->streaming = true;
        d->crc_acc = 0;
        d->finished = false;
        d->unit = DS2502_UNIT_DATA;

        d->addr = ds2502_start_address(d);
        if (d->cmd == DS2502_CMD_READ_STATUS) {
            /*
             * Unmasked, per ds2502_start_address(): an out-of-range status
             * start leaves the closing CRC unreachable
             * within any bounded transaction, since addr would have to wrap
             * the full 16-bit range to reach 0007h
             * again.
             */
            d->sent_byte = ds2502_status_byte(d->addr);
        } else {
            if (d->cmd == DS2502_CMD_READ_DATA_CRC) {
                d->page_end = d->addr | (DS2502_PAGE_SIZE - 1);
            }
            d->sent_byte = ds2502_data_byte(d, d->addr);
        }
        *out = d->sent_byte;
        trace_ds2502_read(d->cmd, d->addr, d->unit, *out);
        return true;
    }

    *out = ds2502_advance_stream(d);
    return true;
}

static void ds2502_function_reset(OneWireSlave *s)
{
    DS2502State *d = ONEWIRE_DS2502_FAMILY(s);

    d->cmd = 0;
    memset(d->hdr, 0, sizeof(d->hdr));
    d->hdr_pos = 0;
    d->streaming = false;
    d->addr = 0;
    d->page_end = 0;
    d->crc_acc = 0;
    d->sent_byte = 0;
    d->unit = DS2502_UNIT_DATA;
    d->finished = false;
}

void ds2502_set_image(DeviceState *dev, const uint8_t *buf, size_t len)
{
    DS2502State *d = ONEWIRE_DS2502_FAMILY(dev);
    DS2502Class *k = ONEWIRE_DS2502_FAMILY_GET_CLASS(d);
    size_t limit = k->pages * DS2502_PAGE_SIZE;
    size_t n = MIN(len, DS2502_DATA_SIZE);

    for (size_t i = 0; i < n; i++) {
        /*
         * A page beyond this part's programmable count always reads 0xff and
         * cannot be programmed, even if the
         * caller's image has real data there.
         */
        d->data[i] = (i < limit) ? buf[i] : 0xff;
    }
}

static void ds2502_instance_init(Object *obj)
{
    DS2502State *d = ONEWIRE_DS2502_FAMILY(obj);

    memset(d->data, 0xff, sizeof(d->data));    /* unprogrammed EPROM */
}

/*
 * `cmd` is loaded unvalidated: a migration stream is not trusted input.
 * ds2502_advance_stream()'s switch on `cmd`
 * only ever reaches its g_assert_not_reached() default when `cmd` holds
 * something other than 0 (no command chosen
 * yet) or one of the three read commands. This device never
 * produces such a state, but an incoming stream could claim it. Reject it
 * here instead of aborting QEMU, the same way the base
 * class's own post_load rejects an
 * out-of-range `bit` (hw/1wire/core.c).
 */
static int ds2502_post_load(void *opaque, int version_id)
{
    DS2502State *d = opaque;

    switch (d->cmd) {
    case 0:
    case DS2502_CMD_READ_MEMORY:
    case DS2502_CMD_READ_DATA_CRC:
    case DS2502_CMD_READ_STATUS:
        return 0;
    default:
        return -EINVAL;
    }
}

static const VMStateDescription vmstate_ds2502 = {
    .name = "ds2502",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ds2502_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_ONEWIRE_SLAVE(parent_obj, DS2502State),
        VMSTATE_UINT8_ARRAY(data, DS2502State, DS2502_DATA_SIZE),
        VMSTATE_UINT8(cmd, DS2502State),
        VMSTATE_UINT8_ARRAY(hdr, DS2502State, 3),
        VMSTATE_UINT8(hdr_pos, DS2502State),
        VMSTATE_BOOL(streaming, DS2502State),
        VMSTATE_UINT16(addr, DS2502State),
        VMSTATE_UINT16(page_end, DS2502State),
        VMSTATE_UINT8(crc_acc, DS2502State),
        VMSTATE_UINT8(sent_byte, DS2502State),
        VMSTATE_UINT8(unit, DS2502State),
        VMSTATE_BOOL(finished, DS2502State),
        VMSTATE_END_OF_LIST()
    }
};

static void ds2502_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    OneWireSlaveClass *k = ONEWIRE_SLAVE_CLASS(oc);

    k->function_reset = ds2502_function_reset;
    k->function_byte = ds2502_function_byte;
    dc->vmsd = &vmstate_ds2502;
}

typedef struct DS2502Variant {
    const char *type_name;
    uint32_t pages;
} DS2502Variant;

static const DS2502Variant ds2502_variants[] = {
    { TYPE_ONEWIRE_DS2501, 2 },    /* 512-bit sibling: pages 0 and 1 only */
    { TYPE_ONEWIRE_DS2502, 4 },    /* full 1024-bit part: all four pages */
};

static void ds2502_variant_class_init(ObjectClass *oc, const void *data)
{
    DS2502Class *k = ONEWIRE_DS2502_FAMILY_CLASS(oc);
    const DS2502Variant *variant = data;

    k->pages = variant->pages;
}

static const TypeInfo ds2502_family_info = {
    .name = TYPE_ONEWIRE_DS2502_FAMILY,
    .parent = TYPE_ONEWIRE_SLAVE,
    .instance_size = sizeof(DS2502State),
    .instance_init = ds2502_instance_init,
    .abstract = true,
    .class_size = sizeof(DS2502Class),
    .class_init = ds2502_class_init,
};

static void ds2502_register_types(void)
{
    TypeInfo variant_info = {
        .parent = TYPE_ONEWIRE_DS2502_FAMILY,
        .class_init = ds2502_variant_class_init,
    };

    type_register_static(&ds2502_family_info);
    for (size_t i = 0; i < ARRAY_SIZE(ds2502_variants); i++) {
        variant_info.name = ds2502_variants[i].type_name;
        variant_info.class_data = &ds2502_variants[i];
        type_register_static(&variant_info);
    }
}

type_init(ds2502_register_types)
