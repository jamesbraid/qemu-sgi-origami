/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "migration/vmstate.h"
#include "hw/1wire/onewire.h"
#include "hw/1wire/crc.h"
#include "hw/1wire/ds2505.h"

/*
 * DS2505-family add-only EPROM (docs/library/dallas-1wire-part-specs.md
 * section C.2). Same 3-byte memory-command header as the DS2502 family --
 * command, TA1 (address low), TA2 (address high) -- but this class echoes
 * no header CRC: unlike DS2502's F0h/AAh, which answer an echoed CRC-8
 * before any content, this class's F0h/AAh/A5h all start their content
 * (a data byte, a status byte, or A5h's redirection byte) in the very next
 * window after TA2. Confirmed independently against both hw/misc/sgi-hub.c
 * and hw/pci-host/sgi-bridge.c, the two existing hand-written DS2505-class
 * responders this device exists to replace (Tasks 7 and 8): neither answers
 * a header-CRC window either.
 *
 *   Read Memory (F0h)          a flat stream, no per-page CRC; if the read
 *                               runs through the last byte of the 2048-byte
 *                               field (07FFh) one CRC-16 over command,
 *                               both address bytes and every data byte
 *                               read follows, and only then. Unlike the
 *                               DS2502-class F0h CRC-8 (data only), this
 *                               class's CRC-16 covers the header too.
 *   Read Status (AAh)          the status/redirection field, organised
 *                               into 8-byte pages each closed by a CRC-16:
 *                               the first page's CRC covers the header
 *                               too, later pages' CRC covers just that
 *                               page's bytes (generator cleared each
 *                               time). This model has no Write Status, so
 *                               every status byte -- implemented region or
 *                               not -- reads unprogrammed 0xff; see
 *                               ds2505_status_byte().
 *   Extended Read Memory (A5h) presents the page-address redirection byte
 *                               for the addressed page before its data,
 *                               then a CRC-16 (header+redirection byte on
 *                               the first page, the redirection byte alone
 *                               on later pages), then that page's 32 data
 *                               bytes and a CRC-16 over just those bytes.
 *                               "The hardware of the DS2505 makes no
 *                               decisions based on the contents of the
 *                               Page Address Redirection Bytes" [datasheet]
 *                               -- it always streams the physically
 *                               addressed page; only the master decides
 *                               whether to act on redirection. This model
 *                               never programs a redirection byte away
 *                               from its unprogrammed 0xff ("not
 *                               redirected"), so a model that instead
 *                               tried to follow redirection would only be
 *                               distinguishable from a correct one by
 *                               which page's *data* comes back -- which is
 *                               exactly what the tests check.
 *
 * There is no C3h on this class -- that command, and its CRC-8, belong to
 * the DS2502 family alone (section C.1); this device does not decode it.
 *
 * Both the data field (F0h, and the pages A5h reads) and the status field
 * (AAh) share the same 07FFh top and the same start-address masking: "a
 * start address above the implemented top has its high address bits
 * forced to zero" [datasheet, section D.2] -- TA1/TA2 are an 11-bit
 * register regardless of which field they address. As with the DS2502
 * sibling, the header CRC always covers the address bytes as actually
 * sent, not the masked value used for the field access, so an
 * out-of-range start address produces a deliberate CRC mismatch rather
 * than a silent alias.
 *
 * Neither existing hand-written DS2505-class responder this device
 * replaces implements that masking rule as written: hw/misc/sgi-hub.c
 * masks a plain read's start address but refuses a status read outright
 * above a fixed offset (0x13f) instead of masking it, and
 * hw/pci-host/sgi-bridge.c does not mask either field, refusing instead.
 * Both already disagree with each other, so neither is authoritative here;
 * this model follows the datasheet extraction's stated rule instead. See
 * the task report for the discrepancy.
 *
 * Write Memory (0Fh), Speed Write Memory (F3h), Write Status (55h) and
 * Speed Write Status (F5h) are all refused: these parts are modelled
 * add-only, and the base 1-Wire layer logs LOG_UNIMP for any byte a part's
 * function_byte declines.
 */

#define DS2505_CMD_READ_MEMORY         0xf0
#define DS2505_CMD_READ_STATUS         0xaa
#define DS2505_CMD_READ_EXTENDED       0xa5
#define DS2505_CMD_WRITE_MEMORY        0x0f
#define DS2505_CMD_SPEED_WRITE_MEMORY  0xf3
#define DS2505_CMD_WRITE_STATUS        0x55
#define DS2505_CMD_SPEED_WRITE_STATUS  0xf5

#define DS2505_STATUS_PAGE_SIZE 8

/* Both fields are addressed by the same 11-bit register; see the note above. */
#define DS2505_ADDR_MASK (DS2505_DATA_SIZE - 1)

OBJECT_DECLARE_SIMPLE_TYPE(DS2505State, ONEWIRE_DS2505)

/*
 * What the *previous* function_byte call placed on the wire, so this call
 * knows how to account for it before deciding what comes next -- see the
 * one-window lag documented on OneWireSlaveClass::function_byte.
 *
 * Every CRC-16 below goes out low byte first, then high (CRC_LO before
 * CRC_HI) -- the extraction never states the byte order for this class's
 * two-byte CRC, but this matches both hw/misc/sgi-hub.c and
 * hw/pci-host/sgi-bridge.c, the existing hand-written responders.
 */
typedef enum {
    DS2505_UNIT_DATA,       /* a plain data or status byte */
    DS2505_UNIT_REDIRECT,   /* A5h's redirection byte */
    DS2505_UNIT_CRC_LO,     /* low byte of a pending CRC-16 */
    DS2505_UNIT_CRC_HI,     /* high byte of a pending CRC-16 */
} DS2505Unit;

struct DS2505State {
    OneWireSlave parent_obj;

    uint8_t data[DS2505_DATA_SIZE];

    /*
     * Header capture: the command byte, then TA1, then TA2, exactly as
     * received on the wire (never the masked address -- the echoed CRC-16
     * covers what was actually sent).
     */
    uint8_t cmd;
    uint8_t hdr[3];
    uint8_t hdr_pos;        /* header bytes captured so far, 0..3 */

    uint16_t addr;          /* address of the byte/segment now in progress */
    uint16_t page_end;      /* last address of the page/status unit in play */
    uint16_t crc_acc;       /* running CRC-16 of the segment now in progress */
    uint16_t crc_pending;   /* the full CRC-16, split across its two windows */
    uint8_t sent_byte;      /* the data/status/redirection byte last sent */
    uint8_t unit;           /* a DS2505Unit value; fixed width so it migrates */
    bool in_redirect;       /* A5h only: mid redirection-byte-then-CRC pair */
    bool finished;          /* nothing further is defined; answer 0xff */
};

static uint8_t ds2505_data_byte(DS2505State *d, uint16_t addr)
{
    return (addr < DS2505_DATA_SIZE) ? d->data[addr] : 0xff;
}

/*
 * DS2505 status/redirection memory (section D.2): only four small regions
 * are physically implemented -- write-protect bits for the data pages
 * (000h-007h), write-protect bits for the redirection bytes (020h-027h),
 * a bitmap of used pages (040h-047h), and the redirection bytes themselves
 * (100h-13Fh) -- everywhere else reads unprogrammed 0xff. This model has
 * no Write Status support, so none of that content can ever differ from
 * blank EPROM: every status byte, implemented region or not, reads 0xff.
 *
 * A5h's redirection byte (100h + page number in this same status field)
 * is a second, independent 0xff literal in
 * ds2505_advance_read_extended() rather than a call through here -- they
 * agree today only because both are blank EPROM. If Write Status is ever
 * added, that byte and this function must be kept in step, or 0xAA and
 * 0xA5 will read the same address differently.
 */
static uint8_t ds2505_status_byte(void)
{
    return 0xff;
}

/*
 * Read Memory (F0h): flat stream, one CRC-16 only at the true end of the
 * data field (07FFh), covering the header and every data byte read.
 */
static uint8_t ds2505_advance_read_memory(DS2505State *d)
{
    if (d->unit == DS2505_UNIT_CRC_HI) {
        d->finished = true;
        return 0xff;
    }
    if (d->unit == DS2505_UNIT_CRC_LO) {
        d->unit = DS2505_UNIT_CRC_HI;
        return d->crc_pending >> 8;
    }

    d->crc_acc = onewire_crc16(d->crc_acc, &d->sent_byte, 1);

    if (d->addr == DS2505_DATA_SIZE - 1) {
        d->crc_pending = ~d->crc_acc;
        d->unit = DS2505_UNIT_CRC_LO;
        return d->crc_pending & 0xff;
    }

    d->addr++;
    d->sent_byte = ds2505_data_byte(d, d->addr);
    d->unit = DS2505_UNIT_DATA;
    return d->sent_byte;
}

/*
 * Read Status (AAh): 8-byte pages, each closed by a CRC-16 -- the first
 * covering the header too, later ones cleared -- continuing to the same
 * 07FFh top the data field shares. That shared top is derived, not
 * stated: the extraction caps only F0h explicitly. A literal AAh
 * end-of-field statement, or a guest read that actually reaches this
 * far, would settle it.
 */
static uint8_t ds2505_advance_read_status(DS2505State *d)
{
    if (d->unit == DS2505_UNIT_CRC_HI) {
        if (d->addr == DS2505_DATA_SIZE - 1) {
            d->finished = true;
            return 0xff;
        }
        d->addr++;                                  /* now page-aligned */
        d->page_end = d->addr | (DS2505_STATUS_PAGE_SIZE - 1);
        d->crc_acc = 0;
        d->sent_byte = ds2505_status_byte();
        d->unit = DS2505_UNIT_DATA;
        return d->sent_byte;
    }
    if (d->unit == DS2505_UNIT_CRC_LO) {
        d->unit = DS2505_UNIT_CRC_HI;
        return d->crc_pending >> 8;
    }

    d->crc_acc = onewire_crc16(d->crc_acc, &d->sent_byte, 1);

    if (d->addr == d->page_end) {
        d->crc_pending = ~d->crc_acc;
        d->unit = DS2505_UNIT_CRC_LO;
        return d->crc_pending & 0xff;
    }

    d->addr++;
    d->sent_byte = ds2505_status_byte();
    d->unit = DS2505_UNIT_DATA;
    return d->sent_byte;
}

/*
 * Extended Read Memory (A5h): a redirection byte and its CRC-16, then that
 * page's 32 data bytes and their own CRC-16, page after page, to the same
 * 07FFh top as a plain read -- this is the data field, just framed
 * differently. That shared top is derived, not stated: the extraction
 * caps only F0h explicitly. A literal A5h end-of-field statement, or a
 * guest read that actually reaches this far, would settle it.
 * Redirection is always 0xff (never redirected): this model has no
 * Write Memory/Write Status, so no redirection byte can ever be
 * programmed away from its unprogrammed value, and the hardware never
 * acts on it regardless (see the header comment). The 0xff literals
 * below and ds2505_status_byte()'s answer for the same address (100h +
 * page number) are two independent sources for one byte -- see the
 * comment there.
 */
static uint8_t ds2505_advance_read_extended(DS2505State *d)
{
    if (d->unit == DS2505_UNIT_CRC_HI) {
        if (d->in_redirect) {
            /* The redirection CRC just finished: this page's data starts. */
            d->in_redirect = false;
            d->crc_acc = 0;
            d->page_end = d->addr | (DS2505_PAGE_SIZE - 1);
            d->sent_byte = ds2505_data_byte(d, d->addr);
            d->unit = DS2505_UNIT_DATA;
            return d->sent_byte;
        }
        /* A page's data CRC just finished: the next page, or the top. */
        if (d->addr == DS2505_DATA_SIZE - 1) {
            d->finished = true;
            return 0xff;
        }
        d->addr++;                                  /* now page-aligned */
        d->crc_acc = 0;
        d->in_redirect = true;
        d->sent_byte = 0xff;   /* this page's redirection byte; see above */
        d->unit = DS2505_UNIT_REDIRECT;
        return d->sent_byte;
    }
    if (d->unit == DS2505_UNIT_CRC_LO) {
        d->unit = DS2505_UNIT_CRC_HI;
        return d->crc_pending >> 8;
    }
    if (d->unit == DS2505_UNIT_REDIRECT) {
        d->crc_acc = onewire_crc16(d->crc_acc, &d->sent_byte, 1);
        d->crc_pending = ~d->crc_acc;
        d->unit = DS2505_UNIT_CRC_LO;
        return d->crc_pending & 0xff;
    }

    /* DS2505_UNIT_DATA: a page-data byte just sent. */
    d->crc_acc = onewire_crc16(d->crc_acc, &d->sent_byte, 1);

    if (d->addr == d->page_end) {
        d->crc_pending = ~d->crc_acc;
        d->unit = DS2505_UNIT_CRC_LO;
        return d->crc_pending & 0xff;
    }

    d->addr++;
    d->sent_byte = ds2505_data_byte(d, d->addr);
    d->unit = DS2505_UNIT_DATA;
    return d->sent_byte;
}

static uint8_t ds2505_advance_stream(DS2505State *d)
{
    if (d->finished) {
        return 0xff;
    }

    switch (d->cmd) {
    case DS2505_CMD_READ_MEMORY:
        return ds2505_advance_read_memory(d);
    case DS2505_CMD_READ_STATUS:
        return ds2505_advance_read_status(d);
    case DS2505_CMD_READ_EXTENDED:
        return ds2505_advance_read_extended(d);
    default:
        g_assert_not_reached();
    }
}

/*
 * TA2 has just been captured: this class echoes no header CRC, so the very
 * next window is already the first content byte. Mask the raw address into
 * the field (the header CRC below still covers the raw bytes as sent, not
 * this masked value), seed the running CRC-16 with the header -- every one
 * of this class's first CRCs covers it -- and set up the first byte each
 * command starts with.
 */
static void ds2505_start_stream(DS2505State *d)
{
    uint16_t raw = ((uint16_t)d->hdr[2] << 8) | d->hdr[1];

    d->addr = raw & DS2505_ADDR_MASK;
    d->crc_acc = onewire_crc16(0, d->hdr, 3);
    d->finished = false;

    switch (d->cmd) {
    case DS2505_CMD_READ_EXTENDED:
        d->in_redirect = true;
        d->sent_byte = 0xff;   /* the start page's redirection byte */
        d->unit = DS2505_UNIT_REDIRECT;
        break;
    case DS2505_CMD_READ_STATUS:
        d->page_end = d->addr | (DS2505_STATUS_PAGE_SIZE - 1);
        d->sent_byte = ds2505_status_byte();
        d->unit = DS2505_UNIT_DATA;
        break;
    default:                                    /* DS2505_CMD_READ_MEMORY */
        d->sent_byte = ds2505_data_byte(d, d->addr);
        d->unit = DS2505_UNIT_DATA;
        break;
    }
}

static bool ds2505_function_byte(OneWireSlave *s, uint8_t in, uint8_t *out)
{
    DS2505State *d = ONEWIRE_DS2505(s);

    if (d->cmd == 0) {
        /* The first byte of a fresh memory function: the command. */
        switch (in) {
        case DS2505_CMD_READ_MEMORY:
        case DS2505_CMD_READ_STATUS:
        case DS2505_CMD_READ_EXTENDED:
            d->cmd = in;
            d->hdr[0] = in;
            d->hdr_pos = 1;
            *out = 0xff;            /* TA1 is a master write */
            return true;
        case DS2505_CMD_WRITE_MEMORY:
        case DS2505_CMD_SPEED_WRITE_MEMORY:
        case DS2505_CMD_WRITE_STATUS:
        case DS2505_CMD_SPEED_WRITE_STATUS:
        default:
            /*
             * Add-only: writes are refused, not misread as a read. An
             * unrecognised byte (0xC3 included -- that command does not
             * exist on this class) is refused the same way. The base
             * layer logs LOG_UNIMP for both.
             */
            return false;
        }
    }

    if (d->hdr_pos < 3) {
        d->hdr[d->hdr_pos++] = in;      /* TA1, then TA2 */
        if (d->hdr_pos < 3) {
            *out = 0xff;                /* TA2 still to come, master write */
        } else {
            ds2505_start_stream(d);
            *out = d->sent_byte;
        }
        return true;
    }

    *out = ds2505_advance_stream(d);
    return true;
}

static void ds2505_function_reset(OneWireSlave *s)
{
    DS2505State *d = ONEWIRE_DS2505(s);

    d->cmd = 0;
    memset(d->hdr, 0, sizeof(d->hdr));
    d->hdr_pos = 0;
    d->addr = 0;
    d->page_end = 0;
    d->crc_acc = 0;
    d->crc_pending = 0;
    d->sent_byte = 0;
    d->unit = DS2505_UNIT_DATA;
    d->in_redirect = false;
    d->finished = false;
}

void ds2505_set_image(DeviceState *dev, const uint8_t *buf, size_t len)
{
    DS2505State *d = ONEWIRE_DS2505(dev);
    size_t n = MIN(len, DS2505_DATA_SIZE);

    memcpy(d->data, buf, n);
    /* Bytes beyond n keep instance_init's unprogrammed 0xff. */
}

static void ds2505_instance_init(Object *obj)
{
    DS2505State *d = ONEWIRE_DS2505(obj);

    memset(d->data, 0xff, sizeof(d->data));    /* unprogrammed EPROM */
}

/*
 * `cmd` is loaded unvalidated: a migration stream is not trusted input.
 * ds2505_advance_stream()'s switch on `cmd` only ever reaches its
 * g_assert_not_reached() default when `cmd` holds something other than
 * 0 (no command chosen yet) or one of the three read commands -- a state
 * this device itself never produces, but an incoming stream could claim.
 * Reject it here instead of aborting QEMU, the same way the base class's
 * own post_load rejects an out-of-range `bit` (hw/1wire/core.c).
 */
static int ds2505_post_load(void *opaque, int version_id)
{
    DS2505State *d = opaque;

    switch (d->cmd) {
    case 0:
    case DS2505_CMD_READ_MEMORY:
    case DS2505_CMD_READ_STATUS:
    case DS2505_CMD_READ_EXTENDED:
        return 0;
    default:
        return -EINVAL;
    }
}

static const VMStateDescription vmstate_ds2505 = {
    .name = "ds2505",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ds2505_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_ONEWIRE_SLAVE(parent_obj, DS2505State),
        VMSTATE_UINT8_ARRAY(data, DS2505State, DS2505_DATA_SIZE),
        VMSTATE_UINT8(cmd, DS2505State),
        VMSTATE_UINT8_ARRAY(hdr, DS2505State, 3),
        VMSTATE_UINT8(hdr_pos, DS2505State),
        VMSTATE_UINT16(addr, DS2505State),
        VMSTATE_UINT16(page_end, DS2505State),
        VMSTATE_UINT16(crc_acc, DS2505State),
        VMSTATE_UINT16(crc_pending, DS2505State),
        VMSTATE_UINT8(sent_byte, DS2505State),
        VMSTATE_UINT8(unit, DS2505State),
        VMSTATE_BOOL(in_redirect, DS2505State),
        VMSTATE_BOOL(finished, DS2505State),
        VMSTATE_END_OF_LIST()
    }
};

static void ds2505_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    OneWireSlaveClass *k = ONEWIRE_SLAVE_CLASS(oc);

    k->function_reset = ds2505_function_reset;
    k->function_byte = ds2505_function_byte;
    dc->vmsd = &vmstate_ds2505;
}

static const TypeInfo ds2505_info = {
    .name = TYPE_ONEWIRE_DS2505,
    .parent = TYPE_ONEWIRE_SLAVE,
    .instance_size = sizeof(DS2505State),
    .instance_init = ds2505_instance_init,
    .class_init = ds2505_class_init,
};

static void ds2505_register_types(void)
{
    type_register_static(&ds2505_info);
}

type_init(ds2505_register_types)
