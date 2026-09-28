/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SGI DS2505-class board records: fixed-offset identity on a one-wire part. */
/*
 * Firmware and IRIX format "Part:...;Name:...;"; hinv prints
 * "<name> Board: barcode ... part ... rev ...". OpenBSD owserial reads the
 * same layout through IOC3's one-wire bus.
 */

#ifndef HW_MISC_SGI_BOARD_RECORD_H
#define HW_MISC_SGI_BOARD_RECORD_H

#include "hw/1wire/onewire.h"

/* Two 32-byte pages, each closed by its own inverted CRC-16. */
#define SGI_BOARD_RECORD_PAGE_SIZE 32
#define SGI_BOARD_RECORD_SIZE      (2 * SGI_BOARD_RECORD_PAGE_SIZE)

/* The part family board records use: DS2505. */
#define SGI_BOARD_RECORD_FAMILY    0x0b

/*
 * The text fields of one record. `part` is twelve characters, a nine
 * character base number such as "030-1155-" and a three-digit dash number;
 * the barcode, revision and name are space-padded to ten, four and fourteen
 * characters.
 */
typedef struct SGIBoardRecord {
    const char *barcode;
    const char *part;
    const char *revision;
    const char *name;
} SGIBoardRecord;

/*
 * Refuse a record the layout rules forbid: a missing or over-long field, a
 * ':' or ';' or unprintable byte, or a part that is not twelve digits and
 * dashes. `owner` names the device in the message, e.g. "IOC3". Exported for
 * a model that stores a record some other way than through
 * sgi_board_record_attach(), such as the Bridge's own record part.
 */
bool sgi_board_record_check(const SGIBoardRecord *record, const char *owner,
                            Error **errp);

/* Store `record`, already checked, as its two sealed pages. */
void sgi_board_record_build(uint8_t rom[SGI_BOARD_RECORD_SIZE],
                            const SGIBoardRecord *record);

/*
 * Check `record`, then realize a DS2505 part holding it on `bus` as child
 * `child_name` of `owner`, with ROM serial `serial` (48 bits).
 */
bool sgi_board_record_attach(Object *owner, const char *child_name,
                             OneWireBus *bus, const SGIBoardRecord *record,
                             uint64_t serial, const char *what,
                             Error **errp);

#endif
