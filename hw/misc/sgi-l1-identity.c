/*
 * Synthetic L1 identity areas, shared by the behavioural L1 and the
 * firmware-backed L1 board so that both present the same identity.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/mips/sgi/sgi-synthetic-serial.h"
#include "hw/misc/sgi-l1-identity.h"

/*
 * Synthetic identity records: what the L1 hands out when no raw record file
 * names a component. Each area uses the Information-Area encoding OpenBSD's
 * l1.c reads: a type/length byte (top two bits the type, low six the length),
 * then the bytes. SGI 007-4498-002 printed pages 181-182 and 007-4365-001
 * printed page 75 give the IO8 areas' 16- and 80-byte field boundaries.
 * The contents are not factory data: dates, keys and the IO8 part are
 * placeholders, the serials are marked synthetic ones, and only the node
 * board's name, part and revision letter come from real inventories (the
 * caller supplies them). Every area
 * sums to zero modulo 256. A Fuel's single system board answers as both
 * the node board and the I/O logic board, the second area carrying the
 * station address in the IO8 position.
 */
#define L1_IA_BINARY 0
#define L1_IA_ASCII 3

static void l1_ia_field(GByteArray *area, unsigned type, const char *data,
                        size_t length)
{
    uint8_t tag = type << 6 | length;

    g_assert(length <= 0x3f);
    g_byte_array_append(area, &tag, 1);
    g_byte_array_append(area, (const uint8_t *)data, length);
}

static void l1_ia_text(GByteArray *area, const char *text)
{
    l1_ia_field(area, L1_IA_ASCII, text, strlen(text));
}

static void l1_ia_u32(GByteArray *area, uint32_t value)
{
    uint8_t data[4];

    stl_be_p(data, value);
    l1_ia_field(area, L1_IA_BINARY, (const char *)data, sizeof(data));
}

/* Pads to a multiple of eight bytes and appends the zero-sum checksum. */
static GByteArray *l1_ia_finish(GByteArray *area)
{
    uint8_t zero = 0;
    uint8_t sum = 0;

    while ((area->len + 1) % 8) {
        g_byte_array_append(area, &zero, 1);
    }
    for (unsigned i = 0; i < area->len; i++) {
        sum += area->data[i];
    }
    sum = -sum;
    g_byte_array_append(area, &sum, 1);
    return area;
}

/* The chassis area: both identity strings the placeholder "NA". */
GByteArray *sgi_l1_identity_chassis_area(void)
{
    static const uint8_t header[] = { 0x00, 0x02, 0x00 };
    GByteArray *area = g_byte_array_new();
    uint8_t eof = SGI_L1_IA_EOF;

    g_byte_array_append(area, header, sizeof(header));
    l1_ia_text(area, "NA");
    l1_ia_text(area, "NA");
    g_byte_array_append(area, &eof, 1);
    return l1_ia_finish(area);
}

/*
 * A board area: placeholder manufacturer "SYNTHETIC", a marked synthetic
 * serial and a zero date, the given name, part and revision, and zeroed
 * placeholder fields. An optional IP53 CPU context is separate from the
 * identity prefix. An I/O board also carries its station address. The serial
 * is @serial when the machine gave one, else component @letter's, instance
 * @index: "EMUN00" for node 0's board. On the model, with "000000" here
 * the stock IP35 PROM reported after every reset that the NVRAM did not
 * belong to this system and reinitialized it, and with the marked serials
 * it does not. Which property of "000000" the PROM rejects is unknown.
 */
GByteArray *sgi_l1_identity_board_area(const char *name, const char *part,
                                       const char *revision,
                                       const uint8_t *mac, char letter,
                                       unsigned int index,
                                       const char *serial_text,
                                       const SGIL1CPUContext *cpu_context)
{
    static const char zeros[4];
    GByteArray *area = g_byte_array_new();
    uint8_t header[6] = { 0 };
    uint8_t fru_file = 0, eof = SGI_L1_IA_EOF, type_three = 0x03;
    char serial[SGI_SYNTHETIC_TEXT_MIN_WIDTH + 1];

    sgi_synthetic_text_serial(serial, SGI_SYNTHETIC_TEXT_MIN_WIDTH, letter,
                              index);
    g_byte_array_append(area, header, sizeof(header));
    if (cpu_context) {
        area->data[0] = 0x02;
    }
    l1_ia_text(area, "SYNTHETIC");
    l1_ia_text(area, name);
    l1_ia_text(area, serial_text ? serial_text : serial);
    l1_ia_text(area, part);
    g_byte_array_append(area, &fru_file, 1);
    l1_ia_text(area, revision);
    l1_ia_field(area, L1_IA_BINARY, zeros, 1);
    l1_ia_text(area, "00");
    if (cpu_context) {
        l1_ia_field(area, L1_IA_BINARY, (const char *)&type_three, 1);
        static const char empty;

        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
        l1_ia_u32(area, cpu_context->mode_word);
        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
        l1_ia_u32(area, cpu_context->hub_mhz);
        l1_ia_u32(area, cpu_context->cpu_mhz);
        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
    }
    for (unsigned i = 0; i < 3; i++) {
        l1_ia_field(area, L1_IA_BINARY, zeros, sizeof(zeros));
    }
    if (mac) {
        char digits[13];

        snprintf(digits, sizeof(digits), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        l1_ia_text(area, digits);
    }
    g_byte_array_append(area, &eof, 1);
    /* Byte 1 counts the area's eight-byte units, checksum included. */
    area->data[1] = (area->len + 1 + 7) / 8;
    return l1_ia_finish(area);
}
