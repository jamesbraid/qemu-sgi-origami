/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The system id, serial numbers and station addresses of an emulated SGI
 * machine.
 *
 * A machine has one observable, the 32-bit system id that IRIX reports
 * through sysinfo -s and FLEXlm's lmhostid. Every other value is derived from
 * it or is a spelling of it. The inputs, from highest precedence down, are:
 *
 * - serial, the system serial as a real unit has it written, or hostid, the
 *   id itself, or on a family whose id comes from its station address, mac;
 * - emu-id, 0 to 4095, which mints an id in the emulator layout;
 * - emu-id 0, the default.
 *
 * The emulator layout is a pure function of the kind (1 for QEMU, 2 for iris),
 * the emu-id and a small sub-index (a node, module or port), with no hashing:
 *
 * - The L1 system serial of the IP35 machines is "LEMU", the kind digit, the
 *   emu-id as four digits and "0".
 * - A module serial of the Origin 2000 and Onyx2 is "KEMU", the kind digit,
 *   the emu-id as four digits and the module number 0 to 9.
 * - The default station address is 08:00:69:E<kind>:<emu-id >> 4>:
 *   <emu-id & 15, sub-index>. The firmware insists on 08:00:69. The sub-index
 *   takes the low nibble of the last byte, so an address derived from the
 *   default by adding a node or port number up to 15 never reaches the id.
 * - A six-character board serial is "EMUN" and the node number as one hex
 *   byte. It follows no identity input: the PROM ties the ownership of its
 *   NVRAM to the node board's serial and starts the NVRAM afresh when it
 *   changes.
 * - The O2's real-time clock carries a 48-bit serial: 45 4d 55 ("EMU"), the
 *   kind, and the emu-id as two bytes.
 *
 * IRIX builds the id from a text serial by taking the low nibble of each of
 * its first ten characters and keeping the low 32 bits, or from the last four
 * bytes of the station address. A serial text is spelled from an id as eight
 * characters, one per hex digit: a digit 0 to 9 stays itself, a later digit a
 * to f becomes a letter J to O, and the first character is always a letter,
 * P for 0, A to I for 1 to 9 and J to O for a to f.
 *
 * These are synthetic values, not any machine's factory identity. iris mirrors
 * this file with kind 2 and the same unit-test vectors.
 */

#ifndef HW_MIPS_SGI_SGI_BOARD_IDENTITY_H
#define HW_MIPS_SGI_SGI_BOARD_IDENTITY_H

#include <glib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The kind of every machine this emulator builds; iris uses 2. */
#define SGI_BOARD_KIND 1

/* The emu-id is 0 (the default) or 1 to this. */
#define SGI_BOARD_EMU_ID_MAX 4095

/* Nodes or ports a default station address can number, 0 to 15. */
#define SGI_BOARD_SUB_INDEX_COUNT 16

/* Modules a module serial can number, 0 to 9. */
#define SGI_BOARD_MODULE_COUNT 10

/* The longest serial text the model takes. */
#define SGI_BOARD_SERIAL_MAX_LENGTH 10

/*
 * Whether @text can be a serial: 1 to 10 printable ASCII characters, none of
 * ':' or ';', which the module record and the L1's replies use as separators.
 */
static inline bool sgi_board_serial_valid(const char *text)
{
    size_t length = strlen(text);

    if (!length || length > SGI_BOARD_SERIAL_MAX_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (text[i] < 0x20 || text[i] > 0x7e || text[i] == ':' ||
            text[i] == ';') {
            return false;
        }
    }
    return true;
}

/* Room for "EMUNxx" and the terminator. */
#define SGI_BOARD_TEXT_SERIAL_SIZE 7

/* Room for "LEMUknnnn0" and the terminator. */
#define SGI_BOARD_SYSTEM_SERIAL_SIZE 11

/* Room for "KEMUknnnnm" and the terminator. */
#define SGI_BOARD_MODULE_SERIAL_SIZE 11

/* Room for the message a refusal gives. */
#define SGI_BOARD_ERROR_SIZE 256

static inline bool sgi_board_emu_id_valid(unsigned int emu_id)
{
    return emu_id <= SGI_BOARD_EMU_ID_MAX;
}

/* "LEMU", the kind digit, four emu-id digits and "0". */
static inline void sgi_board_system_serial(
    unsigned int kind, unsigned int emu_id,
    char text[SGI_BOARD_SYSTEM_SERIAL_SIZE])
{
    g_assert(kind >= 1 && kind <= 9 && sgi_board_emu_id_valid(emu_id));
    snprintf(text, SGI_BOARD_SYSTEM_SERIAL_SIZE, "LEMU%u%04u0", kind, emu_id);
}

/*
 * "KEMU", the kind digit, four emu-id digits and the module number. False
 * when @module is 10 or more, which the one digit cannot name.
 */
static inline bool sgi_board_module_serial(
    unsigned int kind, unsigned int emu_id, unsigned int module,
    char text[SGI_BOARD_MODULE_SERIAL_SIZE])
{
    g_assert(kind >= 1 && kind <= 9 && sgi_board_emu_id_valid(emu_id));
    if (module >= SGI_BOARD_MODULE_COUNT) {
        return false;
    }
    snprintf(text, SGI_BOARD_MODULE_SERIAL_SIZE, "KEMU%u%04u%u", kind, emu_id,
             module);
    return true;
}

/*
 * The default station address of node or port @sub: 08:00:69:E<kind>, the
 * emu-id's high 8 bits, then its low 4 bits over @sub. False when @sub is 16
 * or more.
 */
static inline bool sgi_board_default_mac(unsigned int kind,
                                         unsigned int emu_id,
                                         unsigned int sub, uint8_t mac[6])
{
    g_assert(kind >= 1 && kind <= 15 && sgi_board_emu_id_valid(emu_id));
    if (sub >= SGI_BOARD_SUB_INDEX_COUNT) {
        return false;
    }
    mac[0] = 0x08;
    mac[1] = 0x00;
    mac[2] = 0x69;
    mac[3] = 0xe0 | kind;
    mac[4] = emu_id >> 4;
    mac[5] = (emu_id & 0xf) << 4 | sub;
    return true;
}

/* "EMUN" and @node as two uppercase hex digits, modulo 256. */
static inline void sgi_board_text_serial(
    unsigned int node, char text[SGI_BOARD_TEXT_SERIAL_SIZE])
{
    snprintf(text, SGI_BOARD_TEXT_SERIAL_SIZE, "EMUN%02X", node & 0xff);
}

/*
 * The O2 clock's 48-bit serial, most significant byte first: 45 4d 55, the
 * kind and the emu-id as two bytes.
 */
static inline uint64_t sgi_board_clock_serial(unsigned int kind,
                                              unsigned int emu_id)
{
    return 0x454d55ULL << 24 | (uint64_t)(kind & 0xff) << 16 |
           (emu_id & 0xffff);
}

/*
 * The id IRIX computes from a text serial: the low nibble of each of the
 * first ten characters shifted in, the low 32 bits kept. A shorter text folds
 * in fewer nibbles; an empty one gives 0.
 */
static inline uint32_t sgi_board_hostid_from_text(const char *text)
{
    uint64_t id = 0;

    for (size_t i = 0; i < 10 && text[i]; i++) {
        id = id << 4 | (text[i] & 0xf);
    }
    return (uint32_t)id;
}

/*
 * Spell @id as an eight-character serial text that folds back to @id. A
 * digit 0 to 9 stays itself; a digit a to f becomes J to O; the first
 * character is always a letter, P for 0 and A to I for 1 to 9.
 */
static inline void sgi_board_text_from_hostid(uint32_t id, char text[9])
{
    for (unsigned int i = 0; i < 8; i++) {
        unsigned int nibble = id >> (28 - 4 * i) & 0xf;

        if (nibble >= 10) {
            text[i] = 'J' + nibble - 10;
        } else if (i == 0) {
            text[i] = nibble ? 'A' + nibble - 1 : 'P';
        } else {
            text[i] = '0' + nibble;
        }
    }
    text[8] = 0;
}

/* The id IRIX takes from a station address: its last four bytes. */
static inline uint32_t sgi_board_hostid_from_mac(const uint8_t mac[6])
{
    return (uint32_t)mac[2] << 24 | mac[3] << 16 | mac[4] << 8 | mac[5];
}

/* The station address whose last four bytes are @id. */
static inline void sgi_board_mac_from_hostid(uint32_t id, uint8_t mac[6])
{
    mac[0] = 0x08;
    mac[1] = 0x00;
    mac[2] = id >> 24;
    mac[3] = id >> 16;
    mac[4] = id >> 8;
    mac[5] = id;
}

/* Parses exactly eight hex digits. False for anything else. */
static inline bool sgi_board_hostid_parse(const char *text, uint32_t *id)
{
    uint32_t value = 0;

    if (strlen(text) != 8) {
        return false;
    }
    for (size_t i = 0; i < 8; i++) {
        if (!g_ascii_isxdigit(text[i])) {
            return false;
        }
        value = value << 4 | g_ascii_xdigit_value(text[i]);
    }
    *id = value;
    return true;
}

/*
 * The host id from the Fuel's 12-hex-digit L1 text, hex digits 4 to 11,
 * which is the last four bytes of the address. False when @text is not 12
 * hex digits.
 */
static inline bool sgi_board_hostid_from_hex_text(const char *text,
                                                  uint32_t *id)
{
    uint32_t value = 0;

    if (strlen(text) != 12) {
        return false;
    }
    for (size_t i = 0; i < 12; i++) {
        if (!g_ascii_isxdigit(text[i])) {
            return false;
        }
        if (i >= 4) {
            value = value << 4 | g_ascii_xdigit_value(text[i]);
        }
    }
    *id = value;
    return true;
}

typedef enum SGIBoardFamily {
    /* The id comes from the L1's text serial: the IP35 machines. */
    SGI_BOARD_FAMILY_TEXT,
    /*
     * The id comes from a module serial, which the guest only reads when it
     * starts with K: the Origin 2000 and Onyx2.
     */
    SGI_BOARD_FAMILY_MODULE,
    /* The id comes from the station address: Octane, Origin 200, Fuel. */
    SGI_BOARD_FAMILY_ADDRESS,
} SGIBoardFamily;

/* What the inputs of one machine come to. */
typedef struct SGIBoardId {
    SGIBoardFamily family;
    unsigned int kind;
    unsigned int emu_id;
    uint32_t id;            /* the id of node, module or port 0 */
    /* The id came from serial, hostid or mac, so it has no emulator marker. */
    bool user;
    /* The system serial text of a text family: the base for module 0. */
    char text[SGI_BOARD_SYSTEM_SERIAL_SIZE];
    /* The station address derives from an id the user gave, not emu-id. */
    bool address_from_id;
} SGIBoardId;

static inline void sgi_board_error(char *error, const char *format, ...)
    G_GNUC_PRINTF(2, 3);

static inline void sgi_board_error(char *error, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vsnprintf(error, SGI_BOARD_ERROR_SIZE, format, ap);
    va_end(ap);
}

/*
 * Parses the hostid of an address family: eight hex digits starting 69, or
 * six, the low 24 bits with the 69 implied. False with a message in @error
 * otherwise.
 */
static inline bool sgi_board_address_hostid_parse(const char *text,
                                                  uint32_t *id, char *error)
{
    uint32_t value;
    size_t length = strlen(text);
    char digits[9];

    for (size_t i = 0; i < length; i++) {
        if (!g_ascii_isxdigit(text[i])) {
            length = 0;
        }
    }
    if (length == 6) {
        g_snprintf(digits, sizeof(digits), "69%s", text);
        sgi_board_hostid_parse(digits, id);
        return true;
    }
    if (length != 8 || !sgi_board_hostid_parse(text, &value)) {
        sgi_board_error(error, "hostid must be 6 hex digits (the 69 is "
                        "implied) or 8 starting 69, not '%s'", text);
        return false;
    }
    if (value >> 24 != 0x69) {
        sgi_board_error(error, "hostid=%s: this machine's host id is 69 plus "
                        "6 hex digits; use hostid=%06x or "
                        "mac=08:00:69:%02x:%02x:%02x", text, value & 0xffffff,
                        value >> 16 & 0xff, value >> 8 & 0xff, value & 0xff);
        return false;
    }
    *id = value;
    return true;
}

/*
 * Work out @out from the inputs of a machine of @family. @serial and
 * @hostid are NULL or empty when not given, @mac is NULL when not given and
 * is only a source of the id on an address family. Two inputs that name
 * different ids are refused. False with a message in @error (at least
 * SGI_BOARD_ERROR_SIZE bytes) when the inputs are not acceptable.
 */
static inline bool sgi_board_id_resolve(
    SGIBoardFamily family, unsigned int kind, unsigned int emu_id,
    const char *serial, const char *hostid, const uint8_t *mac,
    SGIBoardId *out, char *error)
{
    bool have_serial = serial && serial[0];
    bool have_hostid = hostid && hostid[0];
    uint32_t serial_id = 0, hostid_id = 0, mac_id = 0;
    bool address = family == SGI_BOARD_FAMILY_ADDRESS;
    bool module = family == SGI_BOARD_FAMILY_MODULE;
    char padded[SGI_BOARD_MODULE_SERIAL_SIZE];
    const char *given = serial;

    memset(out, 0, sizeof(*out));
    out->family = family;
    out->kind = kind;
    out->emu_id = emu_id;
    if (!sgi_board_emu_id_valid(emu_id)) {
        sgi_board_error(error, "emu-id must be 0 to %d, not %u",
                        SGI_BOARD_EMU_ID_MAX, emu_id);
        return false;
    }
    if (have_serial) {
        if (!sgi_board_serial_valid(serial)) {
            sgi_board_error(error, "serial '%s' must be 1 to %d printable "
                            "characters, none of ':' or ';'", serial,
                            SGI_BOARD_SERIAL_MAX_LENGTH);
            return false;
        }
        if (module && serial[0] != 'K') {
            sgi_board_error(error, "serial '%s' must start with K on this "
                            "machine: the guest ignores a module serial that "
                            "does not", serial);
            return false;
        }
        if (module) {
            /*
             * The record holds ten characters. A shorter serial is padded on
             * the left with K, so it still starts with K; the padding is a
             * model choice. The id is the fold of the padded text.
             */
            size_t length = strlen(serial);

            memset(padded, 'K', SGI_BOARD_SERIAL_MAX_LENGTH - length);
            memcpy(padded + SGI_BOARD_SERIAL_MAX_LENGTH - length, serial,
                   length + 1);
            serial = padded;
        }
        serial_id = sgi_board_hostid_from_text(serial);
    }
    if (have_hostid && !address && !sgi_board_hostid_parse(hostid,
                                                          &hostid_id)) {
        sgi_board_error(error, "hostid must be 8 hex digits, not '%s'",
                        hostid);
        return false;
    }
    if (address) {
        char mac_text[18];

        if (have_hostid && !sgi_board_address_hostid_parse(hostid,
                                                           &hostid_id,
                                                           error)) {
            return false;
        }
        if (have_serial && serial_id >> 24 != 0x69) {
            sgi_board_error(error, "serial folds to %08x; this machine's "
                            "host id comes from its station address, which "
                            "always starts 69; use hostid=%06x or "
                            "mac=08:00:69:%02x:%02x:%02x", serial_id,
                            serial_id & 0xffffff, serial_id >> 16 & 0xff,
                            serial_id >> 8 & 0xff, serial_id & 0xff);
            return false;
        }
        if (mac) {
            mac_id = sgi_board_hostid_from_mac(mac);
            snprintf(mac_text, sizeof(mac_text),
                     "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
                     mac[3], mac[4], mac[5]);
            if (mac[0] != 0x08 || mac[1] != 0x00 || mac[2] != 0x69) {
                sgi_board_error(error, "mac=%s: this machine's station "
                                "address must start 08:00:69; use "
                                "mac=08:00:69:%02x:%02x:%02x or hostid=%02x"
                                "%02x%02x", mac_text, mac[3], mac[4], mac[5],
                                mac[3], mac[4], mac[5]);
                return false;
            }
        }
        if (have_serial && have_hostid && serial_id != hostid_id) {
            sgi_board_error(error, "serial %s folds to %08x but hostid=%s "
                            "gives %08x; this machine's host id comes from "
                            "its station address, so they must name the "
                            "same id", serial, serial_id, hostid, hostid_id);
            return false;
        }
        if (mac && have_hostid && mac_id != hostid_id) {
            sgi_board_error(error, "mac=%s gives host id %08x but hostid=%s "
                            "gives %08x; this machine's host id comes from "
                            "its station address, so it would take %08x "
                            "from mac; make them agree", mac_text, mac_id,
                            hostid, hostid_id, mac_id);
            return false;
        }
        if (mac && have_serial && mac_id != serial_id) {
            sgi_board_error(error, "mac=%s gives host id %08x but serial %s "
                            "folds to %08x; this machine's host id comes "
                            "from its station address, so it would take "
                            "%08x from mac; make them agree", mac_text,
                            mac_id, serial, serial_id, mac_id);
            return false;
        }
        if (mac) {
            out->user = true;
            out->id = mac_id;
        } else if (have_hostid || have_serial) {
            out->user = true;
            out->address_from_id = true;
            out->id = have_hostid ? hostid_id : serial_id;
        } else {
            uint8_t base[6];

            sgi_board_default_mac(kind, emu_id, 0, base);
            out->id = sgi_board_hostid_from_mac(base);
        }
        return true;
    }
    if (have_serial && have_hostid && serial_id != hostid_id) {
        sgi_board_error(error, "serial %s folds to %08x but hostid=%s; "
                        "this machine's host id comes from its serial text, "
                        "so they must name the same id", given, serial_id,
                        hostid);
        return false;
    }
    if (have_serial || have_hostid) {
        out->user = true;
        out->id = have_serial ? serial_id : hostid_id;
        if (have_serial) {
            g_strlcpy(out->text, serial, sizeof(out->text));
        } else if (module) {
            /* "KK" and the spelling: any id is reachable and K comes first. */
            out->text[0] = out->text[1] = 'K';
            sgi_board_text_from_hostid(hostid_id, out->text + 2);
        } else {
            sgi_board_text_from_hostid(hostid_id, out->text);
        }
    } else {
        sgi_board_system_serial(kind, emu_id, out->text);
        out->id = sgi_board_hostid_from_text(out->text);
    }
    return true;
}

/*
 * The serial text of module @module, always ten characters and starting with
 * K. An id the user gave puts the serial as given, padded on the left with K,
 * on module 0, and "KK" and the spelling of the id plus @module on module N;
 * otherwise it is the emulator layout. False when @module is 10 or more.
 */
static inline bool sgi_board_id_module_text(
    const SGIBoardId *id, unsigned int module,
    char text[SGI_BOARD_MODULE_SERIAL_SIZE])
{
    if (module >= SGI_BOARD_MODULE_COUNT) {
        return false;
    }
    if (id->family == SGI_BOARD_FAMILY_ADDRESS || !id->user) {
        return sgi_board_module_serial(id->kind, id->emu_id, module, text);
    }
    if (!module) {
        g_strlcpy(text, id->text, SGI_BOARD_MODULE_SERIAL_SIZE);
    } else {
        text[0] = text[1] = 'K';
        sgi_board_text_from_hostid(id->id + module, text + 2);
    }
    return true;
}

#endif /* HW_MIPS_SGI_SGI_BOARD_IDENTITY_H */
