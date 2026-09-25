/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI KONA register-contract and one-wire identity qtests.
 *
 * Increment 1 is identity only: a widget-ID/probe register set and the
 * DS2502 identity parts a stock PROM reads while classifying the
 * InfiniteReality graphics pipe (hw/misc/sgi-kona.c). KONA has no onboard
 * preset of its own yet, so this fixture attaches it the same way
 * sn-builder-test.c's boards reach an origin2000 machine's spare crosstalk
 * fabric: as a `-device` proxied through crossbow0's spare-fabric link at
 * widget 11, the lowest widget the default topology's
 * startup-links-alive (BaseIO at 8, the two host-node Hubs at 9 and 10, SI
 * Viz at 15) leaves free.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-test.h"

#define SMALL_WINDOW_SIZE UINT64_C(0x1000000)
#define SMALL_WINDOW(widget) (SPACE_XTALK + (uint64_t)(widget) * SMALL_WINDOW_SIZE)

/* sn-builder-test.c's own SPARE_WIDGET: KONA's board->cpu_face is a plain
 * MemoryRegion with no Bridge-style packet-address skew, so unlike that
 * file's SPARE_IN_REG() this adds no +REG_DATA. */
#define KONA_WIDGET 11
#define KONA_BASE   SMALL_WINDOW(KONA_WIDGET)
#define KONA_REG(offset) (KONA_BASE + (offset))

#define REG_IDENTIFICATION KONA_REG(0x000004)
#define REG_WIDGET_CONTROL KONA_REG(0x000024)
#define REG_PROBE          KONA_REG(0x000064)
#define REG_ONEWIRE        KONA_REG(0x000098)

#define MACHINE_ARGS \
    "-M origin2000 -device sgi-kona,module=99,slot=8," \
    "fabric=/machine/crossbow0-spare-fabric[0]"

#define ID_PART(v)         (((v) >> 12) & 0xffff)
#define ID_MANUFACTURER(v) (((v) >> 1) & 0x7ff)
#define ID_ALWAYS_ONE(v)   ((v) & 1)

#define KONA_PART         0xc102
#define KONA_MANUFACTURER 0x02aa

/*
 * Started once in main() and reset before each case, exactly as
 * sn0-origin2000-mgras-test.c's own si_reset() does: booting a machine
 * per case is the slowest way to get the same coverage.
 */
static QTestState *qts;

static void kona_reset(void)
{
    qtest_system_reset(qts);
    /* The one register increment 1 gives no write path at all: an
     * end-to-end decode check, not a claim that reset clears anything
     * -- WIDGET_CONTROL and the one-wire register have no reset method
     * of their own yet and are not asserted to any value here. */
    g_assert_cmphex(qtest_readl(qts, REG_PROBE), ==, 0);
}

/*
 * The one-wire register's bit slot, matching Bridge's own NIC widths
 * (sn1-xbridge-onewire-test.c's nic_slot()) -- KONA's pulse/sample shifts
 * are the same the Bridge register uses.
 */
#define KONA_ONEWIRE_LEVEL (1u << 0)
#define KONA_ONEWIRE_DONE  (1u << 1)
#define KONA_ONEWIRE_PULSE(n)  ((uint32_t)(n) << 10)
#define KONA_ONEWIRE_SAMPLE(n) ((uint32_t)(n) << 2)

static bool kona_slot(unsigned int pulse, unsigned int sample)
{
    uint32_t reg;

    qtest_writel(qts, REG_ONEWIRE,
                 KONA_ONEWIRE_PULSE(pulse) | KONA_ONEWIRE_SAMPLE(sample));
    reg = qtest_readl(qts, REG_ONEWIRE);
    g_assert_cmphex(reg & KONA_ONEWIRE_DONE, ==, KONA_ONEWIRE_DONE);
    return reg & KONA_ONEWIRE_LEVEL;
}

#define KONA_ONEWIRE_RESET() kona_slot(520, 65)
#define KONA_READ()          kona_slot(6, 13)
#define KONA_WRITE_ONE()     kona_slot(6, 13)
#define KONA_WRITE_ZERO()    kona_slot(80, 30)

static void kona_write_byte(uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            KONA_WRITE_ONE();
        } else {
            KONA_WRITE_ZERO();
        }
    }
}

static uint8_t kona_read_byte(void)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (KONA_READ()) {
            byte |= 1u << i;
        }
    }
    return byte;
}

/* CRC-8, poly 0x8c: matches onewire_crc8() (hw/1wire/crc.c). Reimplemented
 * locally because a qtest binary cannot link the device model's own
 * internal symbols, exactly as sn1-xbridge-onewire-test.c's nic_crc8()
 * does for the same reason. */
static uint8_t kona_crc8(uint8_t crc, const uint8_t *buf, size_t len)
{
    size_t i;
    unsigned int bit;

    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }
    return crc;
}

/* CRC-16, poly 0xa001: matches onewire_crc16() (hw/1wire/crc.c) and
 * sn1-xbridge-onewire-test.c's own nic_crc(). */
static uint16_t kona_crc16(uint16_t crc, const uint8_t *buf, size_t len)
{
    size_t i;
    unsigned int bit;

    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}
/* The Dallas/Maxim inverted-CRC-16 check residue: re-running the same
 * generator over data with its own complemented CRC appended always lands
 * here, the same identity sn1-xbridge-onewire-test.c's NIC_CRC_RESIDUE
 * checks for the Bridge NIC's unrelated page framing. */
#define KONA_CRC16_RESIDUE 0xb001

#define KONA_ONEWIRE_FAMILY 0x09
#define KONA_GEOMETRY_SERIAL UINT64_C(0x000000000001)
#define KONA_KTOWN_SERIAL    UINT64_C(0x000000000002)

/* The 64-bit ROM code a Match ROM selects: family, then serial LSB first,
 * then a CRC-8 over those seven bytes -- onewire_slave_set_identity()'s
 * own layout (hw/1wire/core.c), built here byte by byte so nothing depends
 * on host endianness. */
static void kona_build_identity(uint8_t identity[8], uint8_t family,
                                uint64_t serial)
{
    unsigned int i;

    identity[0] = family;
    for (i = 0; i < 6; i++) {
        identity[1 + i] = (serial >> (8 * i)) & 0xff;
    }
    identity[7] = kona_crc8(0, identity, 7);
}

/*
 * Select one part by its exact identity. hw/1wire/core.c's Match ROM drops
 * a non-matching part to idle at its first differing bit rather than
 * contending the bus, so this needs no Search ROM discrepancy walk to
 * address either part individually.
 */
static void kona_match_rom(const uint8_t identity[8])
{
    unsigned int i;

    g_assert_false(KONA_ONEWIRE_RESET());  /* pulled low: a part answers */
    kona_write_byte(0x55);                 /* Match ROM */
    for (i = 0; i < 8; i++) {
        kona_write_byte(identity[i]);
    }
}

#define DS2502_CMD_READ_MEMORY 0xf0
#define DS2502_DATA_SIZE       128

/*
 * Read Memory (F0h) from address 0: a 3-byte header (command, TA1, TA2)
 * answered by an echoed CRC-8, then the flat 128-byte image, then one
 * closing CRC-8 over all of it -- hw/1wire/ds2502.c's own protocol.
 * sn1-xbridge-onewire-test.c's "0xa5 read memory" exchange belongs to a
 * different, unidentified one-wire part its own fixture instantiates and
 * does not apply to a DS2502.
 */
static void kona_read_memory(uint8_t out[DS2502_DATA_SIZE])
{
    uint8_t hdr[3] = { DS2502_CMD_READ_MEMORY, 0x00, 0x00 };
    unsigned int i;

    for (i = 0; i < sizeof(hdr); i++) {
        kona_write_byte(hdr[i]);
    }
    g_assert_cmphex(kona_read_byte(), ==, kona_crc8(0, hdr, sizeof(hdr)));

    for (i = 0; i < DS2502_DATA_SIZE; i++) {
        out[i] = kona_read_byte();
    }
    g_assert_cmphex(kona_read_byte(), ==, kona_crc8(0, out, DS2502_DATA_SIZE));
}

/* sn1-xbridge-onewire-test.c's page_field(), unchanged: find "name" within
 * the first `len` bytes and copy everything up to the next ';' delimiter,
 * exactly, never collecting the delimiter itself. */
static bool page_field(const uint8_t *page, unsigned int len,
                       const char *name, char *out)
{
    unsigned int pos = 0, n = strlen(name);

    while (pos < len) {
        unsigned int i;

        if (pos + n <= len && memcmp(&page[pos], name, n) == 0) {
            unsigned int j = 0;

            for (i = pos + n; i < len && page[i] != ';'; i++) {
                out[j++] = page[i];
            }
            out[j] = 0;
            return true;
        }
        for (i = pos; i < len && page[i] != ';'; i++) {
            /* skip to the next delimiter */
        }
        if (i >= len) {
            return false;
        }
        pos = i + 1;
    }
    return false;
}

#define KONA_ONEWIRE_PAGE_SIZE 32
#define KONA_ONEWIRE_CONTENT_PER_PAGE 30
#define KONA_ONEWIRE_PAGES 4
#define KONA_ONEWIRE_CONTENT_SIZE \
    (KONA_ONEWIRE_CONTENT_PER_PAGE * KONA_ONEWIRE_PAGES)

/*
 * Check each page's CRC-16 framing residue, then strip its trailing two
 * CRC bytes and concatenate what is left into one contiguous logical
 * buffer -- hw/misc/sgi-kona.c's sgi_kona_onewire_build_rom() is what wove
 * the two together. A named field (e.g. "Variety:GEOM;" in the geometry
 * record) can straddle a page's 30-byte content boundary, so field
 * verification has to run over this reconstructed buffer, not per page.
 */
static void kona_strip_pages(const uint8_t wire[DS2502_DATA_SIZE],
                             uint8_t content[KONA_ONEWIRE_CONTENT_SIZE])
{
    unsigned int page;

    for (page = 0; page < KONA_ONEWIRE_PAGES; page++) {
        const uint8_t *at = &wire[page * KONA_ONEWIRE_PAGE_SIZE];

        g_assert_cmphex(kona_crc16(0, at, KONA_ONEWIRE_PAGE_SIZE), ==,
                        KONA_CRC16_RESIDUE);
        memcpy(&content[page * KONA_ONEWIRE_CONTENT_PER_PAGE], at,
              KONA_ONEWIRE_CONTENT_PER_PAGE);
    }
}

static void kona_assert_field(const uint8_t *content, const char *name,
                              const char *expected)
{
    char value[64];

    g_assert_true(page_field(content, KONA_ONEWIRE_CONTENT_SIZE, name,
                             value));
    g_assert_cmpstr(value, ==, expected);
}

static void test_identity_and_aperture(void)
{
    uint32_t id;

    kona_reset();
    id = qtest_readl(qts, REG_IDENTIFICATION);
    g_assert_cmphex(ID_PART(id), ==, KONA_PART);
    g_assert_cmphex(ID_MANUFACTURER(id), ==, KONA_MANUFACTURER);
    g_assert_cmphex(ID_ALWAYS_ONE(id), ==, 1);
}

static void test_widget_control_is_plain_storage(void)
{
    kona_reset();
    qtest_writel(qts, REG_WIDGET_CONTROL, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0xdeadbeef);
}

static void test_probe_reads_documented_reset_state(void)
{
    kona_reset();
    g_assert_cmphex(qtest_readl(qts, REG_PROBE), ==, 0);
}

static void test_unknown_offset_reads_zero_and_accepts_writes(void)
{
    uint32_t control_before;

    kona_reset();
    g_assert_cmphex(qtest_readl(qts, KONA_REG(0x100000)), ==, 0);

    /* Compare against whatever WIDGET_CONTROL already held, not a fixed
     * value: it has no reset method yet, so an earlier case's write can
     * still be sitting there (see kona_reset()). What this checks is that
     * an unmapped write does not disturb it, not what it currently is. */
    control_before = qtest_readl(qts, REG_WIDGET_CONTROL);
    qtest_writel(qts, KONA_REG(0x100000), 0x12345678);
    g_assert_cmphex(qtest_readl(qts, KONA_REG(0x100000)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, control_before);
}

static void test_onewire_geometry_identity(void)
{
    uint8_t identity[8];
    uint8_t wire[DS2502_DATA_SIZE];
    uint8_t content[KONA_ONEWIRE_CONTENT_SIZE];

    kona_reset();
    kona_build_identity(identity, KONA_ONEWIRE_FAMILY, KONA_GEOMETRY_SERIAL);
    kona_match_rom(identity);
    kona_read_memory(wire);
    kona_strip_pages(wire, content);

    kona_assert_field(content, "Name:", "GEOM-BD");
    kona_assert_field(content, "Serial:", "00000001");
    kona_assert_field(content, "Revision:", "01");
    kona_assert_field(content, "Group:", "GFX");
    kona_assert_field(content, "Capability:", "0000");
    kona_assert_field(content, "Variety:", "GEOM");
    kona_assert_field(content, "Laser:", "NONE");
    kona_assert_field(content, "Part:", "030-1398-001");
}

static void test_onewire_ktown_identity(void)
{
    uint8_t identity[8];
    uint8_t wire[DS2502_DATA_SIZE];
    uint8_t content[KONA_ONEWIRE_CONTENT_SIZE];

    kona_reset();
    kona_build_identity(identity, KONA_ONEWIRE_FAMILY, KONA_KTOWN_SERIAL);
    kona_match_rom(identity);
    kona_read_memory(wire);
    kona_strip_pages(wire, content);

    kona_assert_field(content, "Name:", "KTOWN-BD");
    kona_assert_field(content, "Serial:", "00000002");
    kona_assert_field(content, "Revision:", "01");
    kona_assert_field(content, "Group:", "GFX");
    kona_assert_field(content, "Capability:", "0000");
    kona_assert_field(content, "Variety:", "KTWN");
    kona_assert_field(content, "Laser:", "NONE");
    kona_assert_field(content, "Part:", "030-1067-001");
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

    qtest_add_func("sn0/origin2000/kona/identity-and-aperture",
                   test_identity_and_aperture);
    qtest_add_func("sn0/origin2000/kona/widget-control-is-plain-storage",
                   test_widget_control_is_plain_storage);
    qtest_add_func("sn0/origin2000/kona/probe-reads-documented-reset-state",
                   test_probe_reads_documented_reset_state);
    qtest_add_func("sn0/origin2000/kona/unknown-offset-reads-zero-and-accepts-writes",
                   test_unknown_offset_reads_zero_and_accepts_writes);
    qtest_add_func("sn0/origin2000/kona/onewire-geometry-identity",
                   test_onewire_geometry_identity);
    qtest_add_func("sn0/origin2000/kona/onewire-ktown-identity",
                   test_onewire_ktown_identity);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
