/*
 * SGI Origin 2000 BaseIO-G board-identity records: the two DS2505 parts
 * on the BaseIO Bridge's one-wire bus, read back through the Bridge's
 * one-wire master register and compared byte for byte with the page
 * images docs/datasheets/baseio.md gives in "Board-identity records on
 * the Bridge one-wire bus".
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

/* Number node 0 so its own crosstalk space has an address. */
static QTestState *start_io6g(void)
{
    QTestState *qts = qtest_init("-M origin2000,baseio-preset=io6g -smp 8 "
                                 "-m 256M -d unimp,guest_errors");

    hub_write(qts, 0, NI_STATUS,
              NI_REGION_SIZE | ((uint64_t)BASEIO_NASID << 8));
    g_assert_cmphex((hub_read(qts, 0, NI_STATUS) >> 8) & 0xff, ==,
                    BASEIO_NASID);
    return qts;
}

static uint64_t bridge_nic_address(void)
{
    return XTALK_BASE + BASEIO_NASID * NODE_STRIDE +
           BASEIO_WIDGET * XTALK_WIDGET_SIZE + BRIDGE_NIC + XTALK_REG_DATA;
}

/*
 * One bit slot, mainline Linux's widths: a long reset, a short
 * write-one/read, a middling write-zero.
 */
static bool nic_slot(QTestState *qts, unsigned int pulse, unsigned int sample)
{
    uint8_t bytes[sizeof(uint32_t)];
    uint32_t nic;

    stl_be_p(bytes, NIC_PULSE(pulse) | NIC_SAMPLE(sample));
    g_assert_true(qtest_memwrite_cpu(qts, 0, bridge_nic_address(),
                                     bytes, sizeof(bytes)));
    g_assert_true(qtest_memread_cpu(qts, 0, bridge_nic_address(),
                                    bytes, sizeof(bytes)));
    nic = ldl_be_p(bytes);
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
 * Both records read back exactly as the datasheet gives them, and the
 * memory past them reads unprogrammed. Which part the search finds first
 * is not asserted: that order is an open question the datasheet settles
 * by a live boot, not a register contract.
 */
static void test_io6g_records_match_the_datasheet(void)
{
    QTestState *qts = start_io6g();
    uint8_t memory[2][RECORD_SIZE + RECORD_PAGE];
    bool seen_baseio = false, seen_mio = false;
    uint64_t rom[4];
    unsigned int i, j;

    g_assert_cmpuint(nic_search_all(qts, rom, G_N_ELEMENTS(rom)), ==, 2);

    for (i = 0; i < 2; i++) {
        const uint8_t *record = memory[i];

        nic_read_memory(qts, rom[i], memory[i], sizeof(memory[i]));
        if (memcmp(record, baseio_record, RECORD_SIZE) == 0) {
            seen_baseio = true;
        } else {
            g_assert_cmpmem(record, RECORD_SIZE, mio_record, RECORD_SIZE);
            seen_mio = true;
        }
        for (j = RECORD_SIZE; j < sizeof(memory[i]); j++) {
            g_assert_cmphex(record[j], ==, 0xff);
        }
    }
    g_assert_true(seen_baseio);
    g_assert_true(seen_mio);

    qtest_quit(qts);
}

/*
 * Each stored page, check value included, leaves the residue Linux's
 * Bridge probe requires, and a single changed field byte does not: the
 * check is over the field bytes, so the residue alone catches a record
 * whose fields and check value disagree.
 */
static void test_io6g_record_pages_leave_the_residue(void)
{
    QTestState *qts = start_io6g();
    uint8_t record[RECORD_SIZE];
    uint64_t rom[4];
    unsigned int count, i, page;

    count = nic_search_all(qts, rom, G_N_ELEMENTS(rom));
    g_assert_cmpuint(count, ==, 2);

    for (i = 0; i < count; i++) {
        nic_read_memory(qts, rom[i], record, sizeof(record));
        for (page = 0; page < 2; page++) {
            uint8_t *at = &record[page * RECORD_PAGE];

            g_assert_cmphex(nic_crc16(0, at, RECORD_PAGE), ==,
                            NIC_CRC_RESIDUE);
            /* The revision letter on page 1, the barcode on page 0. */
            at[page ? 38 - RECORD_PAGE : 1] ^= 0x01;
            g_assert_cmphex(nic_crc16(0, at, RECORD_PAGE), !=,
                            NIC_CRC_RESIDUE);
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/bridge-identity/io6g-records-match-the-datasheet",
                   test_io6g_records_match_the_datasheet);
    qtest_add_func("sn0/bridge-identity/io6g-record-pages-leave-the-residue",
                   test_io6g_record_pages_leave_the_residue);
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
    return g_test_run();
}
