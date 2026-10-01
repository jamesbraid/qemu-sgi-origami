/* SPDX-License-Identifier: BSD-3-Clause */
/* SGI board-identity records on DS2505-class one-wire parts. */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/1wire/crc.h"
#include "hw/1wire/ds2505.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/sgi-board-record.h"

/*
 * Field offsets. The part is split around page 0's check value: spaces then
 * the base number at 11-29, spaces then the dash number at 32-37.
 */
#define RECORD_FORMAT        0x01
#define RECORD_BARCODE       1
#define RECORD_BARCODE_LEN   10
#define RECORD_PART_LEN      12
#define RECORD_PART_BASE     21
#define RECORD_PART_BASE_LEN 9
#define RECORD_PART_DASH     35
#define RECORD_PART_DASH_LEN 3
#define RECORD_REVISION      38
#define RECORD_REVISION_LEN  4
#define RECORD_FLAGS         42  /* group, capability, variety */
#define RECORD_FLAGS_LEN     6
#define RECORD_NAME          48
#define RECORD_NAME_LEN      14

#define RECORD_SERIAL_MASK   MAKE_64BIT_MASK(0, 48)

/*
 * The formatter IRIX shows these fields through substitutes '?' for ':' and
 * ';', and a NUL shows as '.' or '_', so a field is printable ASCII other
 * than those two.
 */
static const char *sgi_board_record_field_error(const char *field,
                                                size_t width)
{
    const char *c;

    if (!field || !*field || strlen(field) > width) {
        return "is required and must fit its field";
    }
    for (c = field; *c; c++) {
        if (*c < 0x20 || *c > 0x7e || *c == ':' || *c == ';') {
            return "must be printable, without ':' or ';'";
        }
    }
    return NULL;
}

bool sgi_board_record_check(const SGIBoardRecord *record, const char *owner,
                            Error **errp)
{
    const struct {
        const char *what;
        const char *value;
        size_t width;
    } fields[] = {
        { "barcode", record->barcode, RECORD_BARCODE_LEN },
        { "revision", record->revision, RECORD_REVISION_LEN },
        { "name", record->name, RECORD_NAME_LEN },
    };
    unsigned int i;

    /*
     * OpenBSD's reader rejects a record whose part, bytes 21-29 and 35-37,
     * holds anything but digits and dashes.
     */
    if (!record->part || strlen(record->part) != RECORD_PART_LEN ||
        strspn(record->part, "0123456789-") != RECORD_PART_LEN) {
        error_setg(errp, "the %s board part must be %d digits and dashes",
                   owner, RECORD_PART_LEN);
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(fields); i++) {
        const char *error = sgi_board_record_field_error(fields[i].value,
                                                         fields[i].width);

        if (error) {
            error_setg(errp, "the %s board %s %s (at most %zu characters)",
                       owner, fields[i].what, error, fields[i].width);
            return false;
        }
    }
    return true;
}

void sgi_board_record_build(uint8_t rom[SGI_BOARD_RECORD_SIZE],
                            const SGIBoardRecord *record)
{
    unsigned int page;

    memset(rom, ' ', SGI_BOARD_RECORD_SIZE);
    rom[0] = RECORD_FORMAT;
    memcpy(&rom[RECORD_BARCODE], record->barcode, strlen(record->barcode));
    memcpy(&rom[RECORD_PART_BASE], record->part, RECORD_PART_BASE_LEN);
    memcpy(&rom[RECORD_PART_DASH], record->part + RECORD_PART_BASE_LEN,
           RECORD_PART_DASH_LEN);
    memcpy(&rom[RECORD_REVISION], record->revision, strlen(record->revision));
    memset(&rom[RECORD_FLAGS], 0xff, RECORD_FLAGS_LEN);
    memcpy(&rom[RECORD_NAME], record->name, strlen(record->name));

    /*
     * Each page's check is image content, not part behaviour: stored
     * inverted, so a page and its check shifted through the generator land on
     * the fixed residue 0xb001.
     */
    for (page = 0; page < 2; page++) {
        uint8_t *at = &rom[page * SGI_BOARD_RECORD_PAGE_SIZE];
        uint16_t crc = ~onewire_crc16(0, at, SGI_BOARD_RECORD_PAGE_SIZE - 2);

        at[SGI_BOARD_RECORD_PAGE_SIZE - 2] = crc & 0xff;
        at[SGI_BOARD_RECORD_PAGE_SIZE - 1] = crc >> 8;
    }
}

bool sgi_board_record_attach(Object *owner, const char *child_name,
                             OneWireBus *bus, const SGIBoardRecord *record,
                             uint64_t serial, const char *what,
                             Error **errp)
{
    uint8_t rom[SGI_BOARD_RECORD_SIZE];
    DeviceState *part;

    if (!sgi_board_record_check(record, what, errp)) {
        return false;
    }
    if (serial & ~RECORD_SERIAL_MASK) {
        error_setg(errp, "the %s board record serial 0x%" PRIx64
                   " does not fit in 48 bits", what, serial);
        return false;
    }

    sgi_board_record_build(rom, record);
    part = qdev_new(TYPE_ONEWIRE_DS2505);
    object_property_add_child(owner, child_name, OBJECT(part));
    qdev_prop_set_uint8(part, "family", SGI_BOARD_RECORD_FAMILY);
    qdev_prop_set_uint64(part, "serial", serial);
    ds2505_set_image(part, rom, sizeof(rom));
    return qdev_realize_and_unref(part, BUS(bus), errp);
}
