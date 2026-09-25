/*
 * SGI Bridge board-identity records: the DS2505 parts on the Origin 2000
 * BaseIO's and BaseIO-G's Bridge one-wire bus and on the Origin 200
 * IP29's, read back through the Bridge's one-wire master register and
 * compared byte for byte with the page images docs/datasheets/baseio.md
 * gives in "Board-identity records on the Bridge one-wire bus" and
 * docs/datasheets/bridge.md gives in "The Origin 200's IP29 record".
 * An Origin 200 converted for a GIGAchannel cabinet and run in loopback
 * adds the Crosstown board's record beside the IP29's at widget 8.
 *
 * The PCI shoehorn's and PCI carrier's Bridges carry records too,
 * compared with the page images docs/datasheets/sgi-pci-shoehorn.md and
 * docs/datasheets/sgi-pci-shoebox.md give in "Board-identity record",
 * along with the part and name the firmware formats from each and the
 * Bridge chip revision each board is fitted with.
 *
 * A Bridge given no board name still stores the older text layout, the
 * part number alone; the bare PCI expansion board is read back for that
 * layout's fields.
 *
 * The machine bring-up and one-wire helpers are duplicated from
 * sn0-baseio-test.c rather than shared, matching this project's other
 * qtest files that each carry their own.
 *
 * The refusal cases check that realize turns away a record the Bridge
 * datasheet's rules forbid ("The record layout, and every constraint on
 * building it"): a ':' or ';' in a text field, a field too long for its
 * bytes, a part that is not digits and dashes, and a barcode or revision
 * with no board name.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-launch.h"
#include "sgi-test.h"

#define HUB_BASE UINT64_C(0x20001000000)
#define NI_STATUS 0x600000
#define NI_REGION_SIZE (UINT64_C(1) << 17)

#define XTALK_BASE UINT64_C(0x20000000000)
#define XTALK_REG_DATA 4
#define XTALK_WIDGET_SIZE UINT64_C(0x1000000)
#define BASEIO_WIDGET 8
#define BASEIO_NASID 0x11

/*
 * crossbow0's first spare fabric widget, where a `-device` XIO board
 * lands on an Origin 2000 (sn0-pci-shoehorn-test.c).
 */
#define SPARE_WIDGET 11

/* The widget identification register: the chip revision in bits 31:28. */
#define WIDGET_ID 0x000
#define WIDGET_ID_REVISION_SHIFT 28

/* The Bridge's one-wire bus master and the bits a driver uses. */
#define BRIDGE_NIC 0x0b0
#define NIC_LEVEL (1u << 0)
#define NIC_DONE  (1u << 1)
#define NIC_PULSE(n)  ((uint32_t)(n) << 10)
#define NIC_SAMPLE(n) ((uint32_t)(n) << 2)

#define NIC_CMD_MATCH_ROM   0x55
#define NIC_CMD_SEARCH_ROM  0xf0
#define NIC_CMD_READ_MEMORY 0xf0    /* the same byte, once a part is matched */

#define RECORD_PAGE 32
#define RECORD_SIZE 64
#define NIC_CRC_RESIDUE 0xb001

/*
 * docs/datasheets/baseio.md, "What each record holds": page 0 then
 * page 1 of each record, check values included, restated here as bytes
 * rather than rebuilt from fields so the comparison does not share the
 * model's construction.
 */
static const uint8_t baseio_record[RECORD_SIZE] = {
    0x01, 0x4f, 0x42, 0x41, 0x53, 0x45, 0x49, 0x4f,
    0x30, 0x30, 0x31, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x33, 0x30,
    0x2d, 0x30, 0x37, 0x33, 0x34, 0x2d, 0xc7, 0x85,
    0x20, 0x20, 0x20, 0x30, 0x30, 0x32, 0x4e, 0x20,
    0x20, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x42, 0x41, 0x53, 0x45, 0x49, 0x4f, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x33, 0xbb,
};

static const uint8_t mio_record[RECORD_SIZE] = {
    0x01, 0x4f, 0x4d, 0x49, 0x4f, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x31, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x33, 0x30,
    0x2d, 0x30, 0x38, 0x38, 0x30, 0x2d, 0xfe, 0x4d,
    0x20, 0x20, 0x20, 0x30, 0x30, 0x33, 0x47, 0x20,
    0x20, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x4d, 0x49, 0x4f, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0xc9, 0x3b,
};

/* docs/datasheets/baseio.md, "The plain BaseIO's record". */
static const uint8_t plain_baseio_record[RECORD_SIZE] = {
    0x01, 0x4f, 0x42, 0x41, 0x53, 0x45, 0x49, 0x4f,
    0x30, 0x30, 0x32, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x33, 0x30,
    0x2d, 0x31, 0x31, 0x32, 0x34, 0x2d, 0xef, 0x3e,
    0x20, 0x20, 0x20, 0x30, 0x30, 0x32, 0x4a, 0x20,
    0x20, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x42, 0x41, 0x53, 0x45, 0x49, 0x4f, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x41, 0x6e,
};

/* docs/datasheets/bridge.md, "The Origin 200's IP29 record". */
static const uint8_t ip29_record[RECORD_SIZE] = {
    0x01, 0x4f, 0x49, 0x50, 0x32, 0x39, 0x30, 0x30,
    0x30, 0x30, 0x31, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x33, 0x30,
    0x2d, 0x31, 0x32, 0x34, 0x34, 0x2d, 0x34, 0x1e,
    0x20, 0x20, 0x20, 0x30, 0x30, 0x31, 0x48, 0x20,
    0x20, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x49, 0x50, 0x32, 0x39, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0xe3, 0x24,
};

/*
 * The GIGAchannel Crosstown board's record (docs/datasheets/
 * sgi-gigachannel.md, "Product identity"), laid out by
 * docs/datasheets/bridge.md's record rules: XBOX_EXT, part 030-1264-001
 * rev C. The barcode is a placeholder.
 */
static const uint8_t crosstown_record[RECORD_SIZE] = {
    0x01, 0x4f, 0x58, 0x42, 0x4f, 0x58, 0x45, 0x58,
    0x54, 0x30, 0x31, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x33, 0x30,
    0x2d, 0x31, 0x32, 0x36, 0x34, 0x2d, 0x8e, 0x8b,
    0x20, 0x20, 0x20, 0x30, 0x30, 0x31, 0x43, 0x20,
    0x20, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x58, 0x42, 0x4f, 0x58, 0x5f, 0x45, 0x58, 0x54,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x09, 0x6f,
};


/*
 * One machine's Bridge line and the records on it. An Origin 2000's
 * crosstalk space has an address only once its node is numbered, and
 * is reached through a processor; an Origin 200's Bridge answers at
 * node 0's space directly.
 */
typedef struct IdentityCase {
    const char *name;
    const char *machine;
    bool numbered;
    unsigned int parts;
    const uint8_t *records[2];
    unsigned int widget;    /* 0: the base I/O's own widget 8 */
} IdentityCase;

static const IdentityCase identity_cases[] = {
    {
        "io6g", "-M origin2000,baseio-preset=io6g -smp 8 -m 256M",
        true, 2, { baseio_record, mio_record },
    },
    {
        "io6", "-M origin2000 -smp 8 -m 256M",
        true, 1, { plain_baseio_record },
    },
    {
        "ip29", "-M origin200",
        false, 1, { ip29_record },
    },
    {
        "gigachannel-loopback",
        "-M origin200,topology=origin200-gigachannel-loopback",
        false, 2, { ip29_record, crosstown_record },
    },
};

/*
 * A board whose Bridge has a part number and no board name, fitted at
 * the spare widget, and the part number its text layout carries.
 */
typedef struct TextLayoutCase {
    const char *name;
    const char *machine;
    const char *part;
} TextLayoutCase;

#define SPARE_BOARD(type) \
    "-M origin2000 -display none -device " type ",module=99,slot=11," \
    "fabric=/machine/crossbow0-spare-fabric[0],fabric-widget-shift=48"

static const TextLayoutCase text_layout_cases[] = {
    { "pci-expansion", SPARE_BOARD("sgi-pci-expansion"), "NOT-OBSERVED" },
};

/*
 * One board whose Bridge carries a single identity part, fitted by an
 * Origin 2000 topology on node 0 without numbering it: the machine, the
 * crosstalk widget its Bridge answers at, the record the board's
 * datasheet gives, the part number and name the firmware's formatter
 * must print from it, and the Bridge chip revision the board is fitted
 * with.
 */
typedef struct BoardCase {
    const char *name;
    const char *machine;
    unsigned int widget;
    const uint8_t *record;
    const char *part;
    const char *board_name;
    uint32_t bridge_revision;
} BoardCase;

/*
 * The PCI shoehorn (docs/datasheets/sgi-pci-shoehorn.md, "Board-identity
 * record"): assembly 030-1275-002, the REV B its photographed label reads,
 * the XTALK_PCI name real Origin 2000 inventories print, a placeholder
 * barcode, and the Bridge D the photographed board carries. The
 * origin2000-pci-shoehorn topology fits it in IO2, crossbow0 widget 12.
 */
static const uint8_t shoehorn_record[RECORD_SIZE] = {
    0x01, 0x4f, 0x58, 0x54, 0x50, 0x43, 0x49, 0x30,
    0x30, 0x30, 0x31, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x33, 0x30,
    0x2d, 0x31, 0x32, 0x37, 0x35, 0x2d, 0xef, 0xbf,
    0x20, 0x20, 0x20, 0x30, 0x30, 0x32, 0x42, 0x20,
    0x20, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x58, 0x54, 0x41, 0x4c, 0x4b, 0x5f, 0x50, 0x43,
    0x49, 0x20, 0x20, 0x20, 0x20, 0x20, 0xa8, 0x7e,
};

/*
 * The PCI Carrier Assembly the origin2000-pci-carrier topology fits in
 * IO2, crossbow0 widget 12 (docs/datasheets/sgi-pci-shoebox.md,
 * "Board-identity record"): the Origin 2000 PCI cardcage's part
 * 030-1062-002, revision C and name PCI-XIO, as a real Origin 2000's
 * inventory prints them, with a placeholder barcode, on the Bridge
 * revision 3 both real PCI_XIO boards report.
 */
static const uint8_t carrier_record[RECORD_SIZE] = {
    0x01, 0x4f, 0x50, 0x43, 0x49, 0x58, 0x49, 0x4f,
    0x30, 0x30, 0x31, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x33, 0x30,
    0x2d, 0x31, 0x30, 0x36, 0x32, 0x2d, 0x6e, 0x29,
    0x20, 0x20, 0x20, 0x30, 0x30, 0x32, 0x43, 0x20,
    0x20, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x50, 0x43, 0x49, 0x2d, 0x58, 0x49, 0x4f, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0xde, 0x5f,
};

static const BoardCase board_cases[] = {
    {
        "pci-shoehorn",
        "-M origin2000,topology=origin2000-pci-shoehorn -display none",
        12, shoehorn_record, "030-1275-002", "XTALK_PCI", 4,
    },
    {
        "pci-carrier",
        "-M origin2000,topology=origin2000-pci-carrier -display none",
        12, carrier_record, "030-1062-002", "PCI-XIO", 3,
    },
};

/* Set by start_machine(): where the one-wire master sits, and how. */
static uint64_t nic_address;
static bool nic_via_cpu;

static uint64_t hub_read(QTestState *qts, unsigned int node, uint64_t offset)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, 2 * node, HUB_BASE + offset,
                                    bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void hub_write(QTestState *qts, unsigned int node, uint64_t offset,
                      uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 2 * node, HUB_BASE + offset,
                                     bytes, sizeof(bytes)));
}

static QTestState *start_machine(const char *machine, bool numbered,
                                 unsigned int widget)
{
    QTestState *qts = qtest_initf("%s -d unimp,guest_errors", machine);
    uint64_t nasid = 0;

    if (numbered) {
        /* Number node 0 so its own crosstalk space has an address. */
        nasid = BASEIO_NASID;
        hub_write(qts, 0, NI_STATUS, NI_REGION_SIZE | (nasid << 8));
        g_assert_cmphex((hub_read(qts, 0, NI_STATUS) >> 8) & 0xff, ==,
                        nasid);
    }
    nic_address = XTALK_BASE + nasid * NODE_STRIDE +
                  widget * XTALK_WIDGET_SIZE + BRIDGE_NIC + XTALK_REG_DATA;
    nic_via_cpu = numbered;
    return qts;
}

/*
 * One bit slot, mainline Linux's widths: a long reset, a short
 * write-one/read, a middling write-zero.
 */
static bool nic_slot(QTestState *qts, unsigned int pulse, unsigned int sample)
{
    uint8_t bytes[sizeof(uint32_t)];
    uint32_t nic;

    if (nic_via_cpu) {
        stl_be_p(bytes, NIC_PULSE(pulse) | NIC_SAMPLE(sample));
        g_assert_true(qtest_memwrite_cpu(qts, 0, nic_address,
                                         bytes, sizeof(bytes)));
        g_assert_true(qtest_memread_cpu(qts, 0, nic_address,
                                        bytes, sizeof(bytes)));
        nic = ldl_be_p(bytes);
    } else {
        qtest_writel(qts, nic_address, NIC_PULSE(pulse) | NIC_SAMPLE(sample));
        nic = qtest_readl(qts, nic_address);
    }
    g_assert_cmphex(nic & NIC_DONE, ==, NIC_DONE);
    return nic & NIC_LEVEL;
}

#define NIC_RESET(q)      nic_slot((q), 520, 65)
#define NIC_READ(q)       nic_slot((q), 6, 13)
#define NIC_WRITE_ONE(q)  nic_slot((q), 6, 13)
#define NIC_WRITE_ZERO(q) nic_slot((q), 80, 30)

static void nic_write_byte(QTestState *qts, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
}

static uint8_t nic_read_byte(QTestState *qts)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (NIC_READ(qts)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

/* CRC-16, reflected polynomial 0xa001, as Linux's Bridge probe runs it. */
static uint16_t nic_crc16(uint16_t crc, const uint8_t *data, unsigned int len)
{
    unsigned int i, bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}

static uint8_t nic_crc8(uint64_t rom)
{
    unsigned int byte, bit;
    uint8_t crc = 0;

    for (byte = 0; byte < 8; byte++) {
        crc ^= rom >> (byte * 8);
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }
    return crc;
}

/*
 * One pass of the standard one-wire Search ROM walk (Maxim AN187), taking
 * the zero branch first. Chain each pass's result and `*last_discrepancy`
 * into the next, starting from 0 and 0.
 */
static uint64_t nic_search_pass(QTestState *qts, uint64_t rom,
                                unsigned int *last_discrepancy)
{
    unsigned int bit_number, last_zero = 0;
    uint64_t found = 0;

    g_assert_false(NIC_RESET(qts));         /* pulled low: someone answers */
    nic_write_byte(qts, NIC_CMD_SEARCH_ROM);
    for (bit_number = 1; bit_number <= 64; bit_number++) {
        bool bit = NIC_READ(qts);
        bool complement = NIC_READ(qts);
        bool direction;

        g_assert_false(bit && complement);
        if (bit != complement) {
            direction = bit;
        } else if (bit_number == *last_discrepancy) {
            direction = true;
        } else if (bit_number > *last_discrepancy) {
            direction = false;
            last_zero = bit_number;
        } else {
            direction = (rom >> (bit_number - 1)) & 1;
            if (!direction) {
                last_zero = bit_number;
            }
        }
        if (direction) {
            found |= UINT64_C(1) << (bit_number - 1);
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
    *last_discrepancy = last_zero;
    return found;
}

/*
 * Select `rom` by Match ROM and read `len` bytes from address 0 with
 * Read Memory, the route the system firmware takes. A two-part bus rules
 * out Skip ROM.
 */
static void nic_read_memory(QTestState *qts, uint64_t rom, uint8_t *out,
                            unsigned int len)
{
    unsigned int i;

    g_assert_cmphex(nic_crc8(rom), ==, 0);
    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, NIC_CMD_MATCH_ROM);
    for (i = 0; i < 8; i++) {
        nic_write_byte(qts, (uint8_t)(rom >> (i * 8)));
    }
    nic_write_byte(qts, NIC_CMD_READ_MEMORY);
    nic_write_byte(qts, 0x00);
    nic_write_byte(qts, 0x00);
    for (i = 0; i < len; i++) {
        out[i] = nic_read_byte(qts);
    }
}

/* Discover every part on the bus; returns how many. */
static unsigned int nic_search_all(QTestState *qts, uint64_t *rom,
                                   unsigned int max)
{
    unsigned int last_discrepancy = 0;
    unsigned int count = 0;

    do {
        g_assert_cmpuint(count, <, max);
        rom[count] = nic_search_pass(qts, count ? rom[count - 1] : 0,
                                     &last_discrepancy);
        count++;
    } while (last_discrepancy != 0);
    return count;
}

/*
 * Each stored page, check value included, leaves the residue Linux's
 * Bridge probe requires, and a single changed field byte does not: the
 * check is over the field bytes, so the residue alone catches a record
 * whose fields and check value disagree.
 */
static void check_record_pages_leave_the_residue(const uint8_t *memory)
{
    uint8_t record[RECORD_SIZE];
    unsigned int page;

    memcpy(record, memory, sizeof(record));
    for (page = 0; page < 2; page++) {
        uint8_t *at = &record[page * RECORD_PAGE];

        g_assert_cmphex(nic_crc16(0, at, RECORD_PAGE), ==, NIC_CRC_RESIDUE);
        /* The revision letter on page 1, the barcode on page 0. */
        at[page ? 38 - RECORD_PAGE : 1] ^= 0x01;
        g_assert_cmphex(nic_crc16(0, at, RECORD_PAGE), !=, NIC_CRC_RESIDUE);
    }
}

/*
 * Every record on the line reads back exactly as the datasheet gives it,
 * and the memory past it reads unprogrammed. Which part the search finds
 * first is not asserted: on the BaseIO-G that order is an open question
 * the datasheet settles by a live boot, not a register contract.
 */
static void test_records_match_the_datasheet(const void *data)
{
    const IdentityCase *c = data;
    QTestState *qts = start_machine(c->machine, c->numbered,
                                    c->widget ? c->widget : BASEIO_WIDGET);
    uint8_t memory[RECORD_SIZE + RECORD_PAGE];
    bool seen[2] = { false, false };
    uint64_t rom[4];
    unsigned int i, j;

    g_assert_cmpuint(nic_search_all(qts, rom, G_N_ELEMENTS(rom)), ==,
                     c->parts);

    for (i = 0; i < c->parts; i++) {
        nic_read_memory(qts, rom[i], memory, sizeof(memory));
        check_record_pages_leave_the_residue(memory);
        for (j = 0; j < c->parts; j++) {
            if (memcmp(memory, c->records[j], RECORD_SIZE) == 0) {
                break;
            }
        }
        if (j == c->parts) {
            /* No match: show the difference from the first record. */
            g_assert_cmpmem(memory, RECORD_SIZE, c->records[0], RECORD_SIZE);
        }
        g_assert_cmpuint(j, <, G_N_ELEMENTS(seen));
        g_assert_false(seen[j]);
        seen[j] = true;
        for (j = RECORD_SIZE; j < sizeof(memory); j++) {
            g_assert_cmphex(memory[j], ==, 0xff);
        }
    }

    qtest_quit(qts);
}

/*
 * A Bridge whose four record fields all come from the command line.
 * sgi-board-test-fixture builds the Bridge the way a BaseIO
 * carrier does; naming no known preset leaves each record property to
 * -global, and a Bridge that accepts its record hands the launch on to
 * sgi-baseio, which refuses the preset with its own error.
 */
typedef struct RecordCase {
    const char *part;
    const char *barcode;
    const char *revision;
    const char *name;
    const char *message;
} RecordCase;

#define GOOD_PART     "030-0734-002"
#define GOOD_BARCODE  "OBASEIO001"
#define GOOD_REVISION "N"
#define GOOD_NAME     "BASEIO"
#define REFUSED_FIELD "must be printable, without ':' or ';'"
#define REFUSED_WIDTH "is required and must fit its field"
#define REFUSED_PART  "board part in a board record must be digits and dashes"
#define REFUSED_BARE  "board-barcode and board-revision properties need " \
                      "board-name"

static void assert_record_launch(const RecordCase *c)
{
    const struct {
        const char *property;
        const char *value;
    } fields[] = {
        { "board-part", c->part },
        { "board-barcode", c->barcode },
        { "board-revision", c->revision },
        { "board-name", c->name },
    };
    g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
    unsigned int i;

    g_ptr_array_add(args, g_strdup(qtest_qemu_binary(NULL)));
    g_ptr_array_add(args, g_strdup("-M"));
    g_ptr_array_add(args, g_strdup("none"));
    g_ptr_array_add(args, g_strdup("-accel"));
    g_ptr_array_add(args, g_strdup("qtest"));
    g_ptr_array_add(args, g_strdup("-display"));
    g_ptr_array_add(args, g_strdup("none"));
    g_ptr_array_add(args, g_strdup("-nodefaults"));
    g_ptr_array_add(args, g_strdup("-qmp"));
    g_ptr_array_add(args, g_strdup("stdio"));
    g_ptr_array_add(args, g_strdup("-device"));
    g_ptr_array_add(args, g_strdup("sgi-board-test-fixture,"
                                   "board-type=sgi-baseio,preset=none"));
    for (i = 0; i < G_N_ELEMENTS(fields); i++) {
        if (fields[i].value) {
            g_ptr_array_add(args, g_strdup("-global"));
            g_ptr_array_add(args, g_strdup_printf("sgi-bridge.%s=%s",
                                                  fields[i].property,
                                                  fields[i].value));
        }
    }
    g_ptr_array_add(args, NULL);
    assert_launch_rejected((const char *const *)args->pdata, c->message);
}

/* The control: a good record gets past the Bridge to sgi-baseio. */
static void test_record_accepted_reaches_the_preset(void)
{
    const RecordCase c = {
        GOOD_PART, GOOD_BARCODE, GOOD_REVISION, GOOD_NAME,
        "sgi-baseio: 'none' names no known preset",
    };

    assert_record_launch(&c);
}

static void test_record_refuses_separators(void)
{
    const RecordCase cases[] = {
        { GOOD_PART, "OBASE:O001", GOOD_REVISION, GOOD_NAME,
          "board barcode " REFUSED_FIELD },
        { GOOD_PART, "OBASE;O001", GOOD_REVISION, GOOD_NAME,
          "board barcode " REFUSED_FIELD },
        { GOOD_PART, GOOD_BARCODE, "N:", GOOD_NAME,
          "board revision " REFUSED_FIELD },
        { GOOD_PART, GOOD_BARCODE, "N;", GOOD_NAME,
          "board revision " REFUSED_FIELD },
        { GOOD_PART, GOOD_BARCODE, GOOD_REVISION, "BASE:IO",
          "board name " REFUSED_FIELD },
        { GOOD_PART, GOOD_BARCODE, GOOD_REVISION, "BASE;IO",
          "board name " REFUSED_FIELD },
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
        assert_record_launch(&cases[i]);
    }
}

/* One character past each field's width: ten, four and fourteen. */
static void test_record_refuses_overlong_fields(void)
{
    const RecordCase cases[] = {
        { GOOD_PART, "OBASEIO0012", GOOD_REVISION, GOOD_NAME,
          "board barcode " REFUSED_WIDTH " (at most 10 characters)" },
        { GOOD_PART, GOOD_BARCODE, "NNNNN", GOOD_NAME,
          "board revision " REFUSED_WIDTH " (at most 4 characters)" },
        { GOOD_PART, GOOD_BARCODE, GOOD_REVISION, "BASEIOBASEIOBAS",
          "board name " REFUSED_WIDTH " (at most 14 characters)" },
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
        assert_record_launch(&cases[i]);
    }
}

/*
 * A board record's part must be digits and dashes; the older text layout's
 * twelve-character check alone lets a ':' or a letter through.
 */
static void test_record_refuses_a_part_not_digits_and_dashes(void)
{
    const RecordCase cases[] = {
        { "030-0734:002", GOOD_BARCODE, GOOD_REVISION, GOOD_NAME,
          REFUSED_PART },
        { "030-0734-00A", GOOD_BARCODE, GOOD_REVISION, GOOD_NAME,
          REFUSED_PART },
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
        assert_record_launch(&cases[i]);
    }
}

/*
 * A barcode or revision needs a name, which selects the record layout;
 * a name needs the barcode and revision to go with it.
 */
static void test_record_refuses_a_partial_record(void)
{
    const RecordCase cases[] = {
        { GOOD_PART, GOOD_BARCODE, NULL, NULL, REFUSED_BARE },
        { GOOD_PART, NULL, GOOD_REVISION, NULL, REFUSED_BARE },
        { GOOD_PART, NULL, GOOD_REVISION, GOOD_NAME,
          "board barcode " REFUSED_WIDTH },
        { GOOD_PART, GOOD_BARCODE, NULL, GOOD_NAME,
          "board revision " REFUSED_WIDTH },
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
        assert_record_launch(&cases[i]);
    }
}

/*
 * The text layout: the format byte, `;Part:` ending where the part number
 * begins at byte 11, a `;` after its twelve characters, each page's check
 * leaving the residue, and unprogrammed memory past the two pages.
 */
static void test_text_layout_reads_back(const void *data)
{
    const TextLayoutCase *c = data;
    QTestState *qts = start_machine(c->machine, false, SPARE_WIDGET);
    uint8_t memory[RECORD_SIZE + RECORD_PAGE];
    uint64_t rom[4];
    unsigned int i;

    g_assert_cmpuint(nic_search_all(qts, rom, G_N_ELEMENTS(rom)), ==, 1);
    nic_read_memory(qts, rom[0], memory, sizeof(memory));

    g_assert_cmphex(memory[0], ==, 0x01);
    g_assert_cmpmem(&memory[5], 6, ";Part:", 6);
    g_assert_cmpmem(&memory[11], 12, c->part, 12);
    g_assert_cmphex(memory[23], ==, ';');
    for (i = 0; i < RECORD_SIZE; i += RECORD_PAGE) {
        g_assert_cmphex(nic_crc16(0, &memory[i], RECORD_PAGE), ==,
                        NIC_CRC_RESIDUE);
    }
    for (i = RECORD_SIZE; i < sizeof(memory); i++) {
        g_assert_cmphex(memory[i], ==, 0xff);
    }

    qtest_quit(qts);
}

/*
 * One field as the IP27 PROM's formatter renders it into the
 * "Part:...;Name:..." string: ':' and ';' become '?', 0xff a space,
 * spaces are dropped, and any other control or high byte becomes '.'.
 */
static void record_field(const uint8_t *bytes, unsigned int len, GString *out)
{
    unsigned int i;

    for (i = 0; i < len; i++) {
        uint8_t c = bytes[i];

        if (c == ':' || c == ';') {
            c = '?';
        } else if (c == 0xff) {
            c = ' ';
        }
        if (c == ' ') {
            continue;
        }
        g_string_append_c(out, (c < 0x20 || c >= 0x80) ? '.' : c);
    }
}

/*
 * The board's one part holds its datasheet record exactly, unprogrammed
 * memory after it, and a record every reader accepts: format 1, text
 * fields with no ':', ';' or NUL, 0xff flag bytes, both pages leaving the
 * CRC residue, and a part number and name that format without a stray
 * '?' or '.'. The Bridge reports the board's chip revision.
 */
static void test_board_record(const void *data)
{
    const BoardCase *c = data;
    QTestState *qts = start_machine(c->machine, false, c->widget);
    uint8_t memory[RECORD_SIZE + RECORD_PAGE];
    g_autoptr(GString) part = g_string_new(NULL);
    g_autoptr(GString) name = g_string_new(NULL);
    uint64_t rom[4];
    uint32_t id;
    unsigned int i;

    g_assert_cmpuint(nic_search_all(qts, rom, G_N_ELEMENTS(rom)), ==, 1);
    nic_read_memory(qts, rom[0], memory, sizeof(memory));

    g_assert_cmpmem(memory, RECORD_SIZE, c->record, RECORD_SIZE);
    for (i = RECORD_SIZE; i < sizeof(memory); i++) {
        g_assert_cmphex(memory[i], ==, 0xff);
    }

    g_assert_cmphex(memory[0], ==, 0x01);
    for (i = 1; i < RECORD_SIZE - 2; i++) {
        if (i == 30 || i == 31) {
            continue;                       /* page 0's check value */
        }
        if (i >= 42 && i < 48) {
            g_assert_cmphex(memory[i], ==, 0xff);
            continue;
        }
        g_assert_cmphex(memory[i], !=, ':');
        g_assert_cmphex(memory[i], !=, ';');
        g_assert_cmphex(memory[i], !=, 0x00);
    }
    check_record_pages_leave_the_residue(memory);

    record_field(&memory[11], 19, part);
    record_field(&memory[32], 6, part);
    g_assert_cmpstr(part->str, ==, c->part);
    record_field(&memory[48], 14, name);
    g_assert_cmpstr(name->str, ==, c->board_name);

    id = qtest_readl(qts, XTALK_BASE + c->widget * XTALK_WIDGET_SIZE +
                          WIDGET_ID + XTALK_REG_DATA);
    g_assert_cmpuint(id >> WIDGET_ID_REVISION_SHIFT, ==, c->bridge_revision);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    unsigned int i;

    g_test_init(&argc, &argv, NULL);
    for (i = 0; i < G_N_ELEMENTS(identity_cases); i++) {
        const IdentityCase *c = &identity_cases[i];
        g_autofree char *path = g_strdup_printf(
            "sn0/bridge-identity/%s-records-match-the-datasheet", c->name);

        qtest_add_data_func(path, c, test_records_match_the_datasheet);
    }
    for (i = 0; i < G_N_ELEMENTS(board_cases); i++) {
        const BoardCase *c = &board_cases[i];
        g_autofree char *path = g_strdup_printf(
            "sn0/bridge-identity/%s-record", c->name);

        qtest_add_data_func(path, c, test_board_record);
    }
    qtest_add_func("sn0/bridge-identity/record-accepted-reaches-the-preset",
                   test_record_accepted_reaches_the_preset);
    qtest_add_func("sn0/bridge-identity/record-refuses-separators",
                   test_record_refuses_separators);
    qtest_add_func("sn0/bridge-identity/record-refuses-overlong-fields",
                   test_record_refuses_overlong_fields);
    qtest_add_func("sn0/bridge-identity/record-refuses-a-bad-part",
                   test_record_refuses_a_part_not_digits_and_dashes);
    qtest_add_func("sn0/bridge-identity/record-refuses-a-partial-record",
                   test_record_refuses_a_partial_record);
    for (i = 0; i < G_N_ELEMENTS(text_layout_cases); i++) {
        const TextLayoutCase *c = &text_layout_cases[i];
        g_autofree char *path = g_strdup_printf(
            "sn0/bridge-identity/%s-text-layout-reads-back", c->name);

        qtest_add_data_func(path, c, test_text_layout_reads_back);
    }
    return g_test_run();
}
