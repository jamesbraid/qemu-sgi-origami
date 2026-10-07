/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The identity every SGI machine gives a part when no real value is known:
 * one-wire ROM serials, board-record barcodes and serial texts, module and
 * board-area serials. No real machine's identity is copied, so each of these
 * is a synthetic value, and every one carries a marker that says so. An
 * inventory dump from the emulator should never pass for a physical
 * machine's.
 *
 * Values are a pure function of a component letter and an instance number:
 * the same machine configuration gives the same serials on every boot, and
 * no time or host data enters them. The machine (or its catalogue entry)
 * picks each component's instance number so that no two parts of one
 * machine share a serial.
 *
 * The component letters:
 *
 *   N  node or system board (IP27/IP29 node record, IP35 node board area,
 *      IP30 and IP34 system boards)
 *   U  processor module (IP30 PM10)
 *   I  I/O board (IO8, BaseIO)
 *   F  frontplane (IP30)
 *   L  midplane (an Origin 2000 module's midplane record part)
 *   Y  module-identity part (an Origin 2000 module's MODULEID part)
 *   S  power supply (IP30)
 *   B  Bridge one-wire part
 *   X  XBridge one-wire part
 *   C  IOC3 one-wire parts
 *   R  Router
 *   M  module or brick (a module number is reserved for its management chip)
 *   G  graphics board
 *   P  PCI option card
 *   O  XIO option board
 *   D  daughtercard
 *
 * scripts/sgi_model/synthetic_serial.py is this header's twin for the
 * catalogue generator; the two must agree value for value.
 */

#ifndef HW_MIPS_SGI_SGI_SYNTHETIC_SERIAL_H
#define HW_MIPS_SGI_SGI_SYNTHETIC_SERIAL_H

/*
 * A 48-bit one-wire ROM serial: "QEMU" in the top 32 bits, then the
 * component letter and the instance number, so the serial's six bytes spell
 * 'Q' 'E' 'M' 'U' letter index from the most significant down. The family
 * code and ROM CRC around it are the part's own and are unaffected.
 */
#define SGI_SYNTHETIC_ONEWIRE_PREFIX  UINT64_C(0x51454d55)

/*
 * A serial for a UniqueWare part whose top twelve bits are a customer code a
 * driver matches on (OpenBSD's owmac takes family 0x91 only with 0x5e7
 * there): the code, then "EMU" in the next 24 bits, then a 12-bit instance
 * number. The component is implied by the part.
 */
#define SGI_SYNTHETIC_UNIQUEWARE_MARKER UINT64_C(0x454d55)

/*
 * The two serial forms as constant expressions, for property defaults and
 * static tables. @letter is an upper-case component letter from the table
 * above; @index fits eight bits (twelve for UniqueWare).
 */
#define SGI_SYNTHETIC_ONEWIRE_SERIAL(letter, index) \
    (SGI_SYNTHETIC_ONEWIRE_PREFIX << 16 | (uint64_t)(letter) << 8 | (index))
#define SGI_SYNTHETIC_UNIQUEWARE_SERIAL(customer, index) \
    ((uint64_t)(customer) << 36 | SGI_SYNTHETIC_UNIQUEWARE_MARKER << 12 | \
     (index))

/* The text marker: "EMU", a component letter, then decimal digits. */
#define SGI_SYNTHETIC_TEXT_MARKER     "EMU"

/* The shortest text serial: the marker, a letter and two digits. */
#define SGI_SYNTHETIC_TEXT_MIN_WIDTH  6

/*
 * An Origin 2000 or Onyx2 module serial is the exception to the text form:
 * the kernel takes a module serial only if it begins with K, so the marker
 * is "KORIGAMI", followed by two decimal digits, ten characters in all.
 */
#define SGI_SYNTHETIC_MODULE_SERIAL_MARKER "KORIGAMI"

static inline uint64_t sgi_synthetic_onewire_serial(char letter,
                                                    unsigned int index)
{
    g_assert(letter >= 'A' && letter <= 'Z' && index <= 0xff);
    return SGI_SYNTHETIC_ONEWIRE_SERIAL(letter, index);
}

static inline uint64_t sgi_synthetic_uniqueware_serial(unsigned int customer,
                                                       unsigned int index)
{
    g_assert(customer <= 0xfff && index <= 0xfff);
    return SGI_SYNTHETIC_UNIQUEWARE_SERIAL(customer, index);
}

/* Whether a 48-bit one-wire serial carries either marker above. */
static inline bool sgi_synthetic_onewire_serial_marked(uint64_t serial)
{
    return (serial >> 16) == SGI_SYNTHETIC_ONEWIRE_PREFIX ||
           ((serial >> 12) & 0xffffff) == SGI_SYNTHETIC_UNIQUEWARE_MARKER;
}

/*
 * Write the text serial for @letter and @index into @buf, exactly @width
 * characters and a NUL: the marker, the letter, then @index in decimal,
 * zero-padded to fill the field. @width counts the characters only.
 */
static inline void sgi_synthetic_text_serial(char *buf, size_t width,
                                             char letter, unsigned int index)
{
    int digits = width - (sizeof(SGI_SYNTHETIC_TEXT_MARKER) - 1) - 1;
    uint64_t limit = 1;
    int written;

    g_assert(letter >= 'A' && letter <= 'Z');
    g_assert(width >= SGI_SYNTHETIC_TEXT_MIN_WIDTH && width < 32);
    for (int i = 0; i < digits && limit <= UINT32_MAX; i++) {
        limit *= 10;
    }
    g_assert(index < limit);
    written = snprintf(buf, width + 1, SGI_SYNTHETIC_TEXT_MARKER "%c%0*u",
                       letter, digits, index);
    g_assert(written == (int)width);
}

/*
 * The text serial a marked one-wire serial stands for, @width characters as
 * above: a part's printed serial and its ROM serial then name the same
 * letter and instance; @width must leave room for the index's digits.
 * False, leaving @buf alone, for an unmarked serial such as a raw override.
 */
static inline bool sgi_synthetic_text_from_onewire(char *buf, size_t width,
                                                   uint64_t serial)
{
    char letter = (serial >> 8) & 0xff;

    if ((serial >> 16) != SGI_SYNTHETIC_ONEWIRE_PREFIX ||
        letter < 'A' || letter > 'Z') {
        return false;
    }
    sgi_synthetic_text_serial(buf, width, letter, serial & 0xff);
    return true;
}

/* Room for a barcode sgi_synthetic_barcode() writes, and its NUL. */
#define SGI_SYNTHETIC_BARCODE_SIZE    9

/*
 * The barcode for a board-identity record kept on a part whose ROM serial
 * is @part_serial: the board's own letter, then the part's instance, so the
 * record and the part that holds it name the same unit, and a board fitted
 * twice gets two barcodes. An unmarked part serial, such as a raw override,
 * gives instance 0. Six characters for instances below 100, more above.
 */
static inline void sgi_synthetic_barcode(char *buf, char letter,
                                         uint64_t part_serial)
{
    unsigned int index = 0;

    if ((part_serial >> 16) == SGI_SYNTHETIC_ONEWIRE_PREFIX) {
        index = part_serial & 0xff;
    } else if (sgi_synthetic_onewire_serial_marked(part_serial)) {
        index = part_serial & 0xfff;
    }
    sgi_synthetic_text_serial(buf, index < 100 ? 6 : index < 1000 ? 7 : 8,
                              letter, index);
}

#endif /* HW_MIPS_SGI_SGI_SYNTHETIC_SERIAL_H */
