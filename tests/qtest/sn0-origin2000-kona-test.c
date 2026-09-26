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
#define SMALL_WINDOW(widget) \
    (SPACE_XTALK + (uint64_t)(widget) * SMALL_WINDOW_SIZE)

/*
 * sn-builder-test.c's own SPARE_WIDGET: KONA's board->cpu_face is a plain
 * MemoryRegion with no Bridge-style packet-address skew, so unlike that
 * file's SPARE_IN_REG() this adds no +REG_DATA.
 */
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
    /*
     * An end-to-end decode check for PROBE, not a claim about the rest of
     * the register file: WIDGET_CONTROL and the one-wire register do have
     * their own reset method now (sgi_kona_reset_hold(), hw/misc/
     * sgi-kona.c) and go back to zero on every reset this calls, but that
     * is test_reset_clears_plain_storage()'s claim to check, not this
     * helper's.
     */
    g_assert_cmphex(qtest_readl(qts, REG_PROBE), ==, 0);
}

/*
 * The one-wire register's bit slot, matching Bridge's own NIC widths
 * (sn1-xbridge-test.c's nic_slot()) -- KONA's pulse/sample shifts
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

/*
 * CRC-8, poly 0x8c: matches onewire_crc8() (hw/1wire/crc.c). Reimplemented
 * locally because a qtest binary cannot link the device model's own
 * internal symbols, exactly as sn1-xbridge-test.c's nic_crc8()
 * does for the same reason.
 */
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

/*
 * CRC-16, poly 0xa001: matches onewire_crc16() (hw/1wire/crc.c) and
 * sn1-xbridge-test.c's own nic_crc16().
 */
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
/*
 * The Dallas/Maxim inverted-CRC-16 check residue: re-running the same
 * generator over data with its own complemented CRC appended always lands
 * here, the same identity sn1-xbridge-test.c's NIC_CRC_RESIDUE
 * checks for the Bridge NIC's unrelated page framing.
 */
#define KONA_CRC16_RESIDUE 0xb001

#define KONA_ONEWIRE_FAMILY 0x09
#define KONA_GEOMETRY_SERIAL UINT64_C(0x000000000001)
#define KONA_KTOWN_SERIAL    UINT64_C(0x000000000002)

/*
 * The 64-bit ROM code a Match ROM selects: family, then serial LSB first,
 * then a CRC-8 over those seven bytes -- onewire_slave_set_identity()'s
 * own layout (hw/1wire/core.c), built here byte by byte so nothing depends
 * on host endianness.
 */
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
 * sn1-xbridge-test.c's "0xa5 read memory" exchange belongs to a
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

#define KONA_RECORD_PAGE 32
#define KONA_RECORD_SIZE 64

/*
 * Field offsets matching hw/pci-host/sgi-bridge.c's own fixed-offset
 * board-identity record (docs/datasheets/bridge.md, "The record layout,
 * and every constraint on building it") -- sgi_kona_onewire_attach_part()
 * (hw/misc/sgi-kona.c) shares that file's
 * sgi_bridge_onewire_build_record() rather than building its own record.
 * A discriminating test does not call the code it is checking, so this
 * constructs the expected bytes independently, the same way
 * sn0-bridge-identity-test.c's own records are hardcoded rather than
 * generated: from the field layout, not from sgi-kona.c or sgi-bridge.c.
 */
#define KONA_RECORD_FORMAT        0x01
#define KONA_RECORD_BARCODE       1
#define KONA_RECORD_PART_BASE     21
#define KONA_RECORD_PART_BASE_LEN 9
#define KONA_RECORD_PART_DASH     35
#define KONA_RECORD_PART_DASH_LEN 3
#define KONA_RECORD_REVISION      38
#define KONA_RECORD_FLAGS         42
#define KONA_RECORD_FLAGS_LEN     6
#define KONA_RECORD_NAME          48

/*
 * Seal both pages the way sgi_bridge_onewire_seal_pages() does: each
 * page's last two bytes hold its own first thirty bytes' CRC-16,
 * inverted, so a reader running the same generator over a page and its
 * stored check lands on the fixed residue KONA_CRC16_RESIDUE names.
 */
static void kona_seal_record_pages(uint8_t rom[KONA_RECORD_SIZE])
{
    unsigned int page;

    for (page = 0; page < KONA_RECORD_SIZE / KONA_RECORD_PAGE; page++) {
        uint8_t *at = &rom[page * KONA_RECORD_PAGE];
        uint16_t crc = ~kona_crc16(0, at, KONA_RECORD_PAGE - 2);

        at[KONA_RECORD_PAGE - 2] = crc & 0xff;
        at[KONA_RECORD_PAGE - 1] = crc >> 8;
    }
}

/*
 * Build one record's bytes the fixed-offset way: unset bytes are spaces,
 * as sgi_bridge_onewire_build_record() leaves them, and the flags region
 * is always 0xff -- this model stores no group/capability/variety fields
 * of its own. `part` must be the full twelve-character part number; this
 * splits it around page 0's check value the same way production code
 * does, at nine characters then three.
 */
static void kona_build_expected_record(uint8_t rom[KONA_RECORD_SIZE],
                                       const char *barcode, const char *part,
                                       const char *revision, const char *name)
{
    memset(rom, ' ', KONA_RECORD_SIZE);
    rom[0] = KONA_RECORD_FORMAT;
    memcpy(&rom[KONA_RECORD_BARCODE], barcode, strlen(barcode));
    memcpy(&rom[KONA_RECORD_PART_BASE], part, KONA_RECORD_PART_BASE_LEN);
    memcpy(&rom[KONA_RECORD_PART_DASH], part + KONA_RECORD_PART_BASE_LEN,
          KONA_RECORD_PART_DASH_LEN);
    memcpy(&rom[KONA_RECORD_REVISION], revision, strlen(revision));
    memset(&rom[KONA_RECORD_FLAGS], 0xff, KONA_RECORD_FLAGS_LEN);
    memcpy(&rom[KONA_RECORD_NAME], name, strlen(name));
    kona_seal_record_pages(rom);
}

/*
 * Each stored page leaves the residue a real one-wire reader's CRC-16
 * requires, matching sn0-bridge-identity-test.c's own
 * check_record_pages_leave_the_residue(): the check covers the field
 * bytes, so this is independent of comparing against the expected buffer
 * kona_build_expected_record() returns, and catches a record whose fields
 * and stored check disagree even if both happened to match by mistake.
 */
static void kona_check_record_residue(const uint8_t *record)
{
    unsigned int page;

    for (page = 0; page < KONA_RECORD_SIZE / KONA_RECORD_PAGE; page++) {
        g_assert_cmphex(kona_crc16(0, &record[page * KONA_RECORD_PAGE],
                                   KONA_RECORD_PAGE),
                        ==, KONA_CRC16_RESIDUE);
    }
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

    /*
     * Compare against whatever WIDGET_CONTROL reads back after
     * kona_reset(), not a hardcoded value: this checks that an unmapped
     * write does not disturb it, independent of what value reset itself
     * leaves there (test_reset_clears_plain_storage() checks that
     * separately).
     */
    control_before = qtest_readl(qts, REG_WIDGET_CONTROL);
    qtest_writel(qts, KONA_REG(0x100000), 0x12345678);
    g_assert_cmphex(qtest_readl(qts, KONA_REG(0x100000)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, control_before);
}

/*
 * The DS2502 the guest reads back holds exactly the fixed-offset record
 * production code builds, its own two pages leaving the CRC-16 residue,
 * and the remaining 64 bytes of its 128-byte address space unprogrammed
 * (ds2502_set_image() leaves whatever a 64-byte image does not cover at
 * ds2502_instance_init()'s own 0xff default).
 */
static void kona_assert_onewire_identity(const char *barcode,
                                         const char *part,
                                         const char *revision,
                                         const char *name)
{
    uint8_t wire[DS2502_DATA_SIZE];
    uint8_t expected[KONA_RECORD_SIZE];
    unsigned int i;

    kona_read_memory(wire);

    kona_build_expected_record(expected, barcode, part, revision, name);
    g_assert_cmpmem(wire, KONA_RECORD_SIZE, expected, KONA_RECORD_SIZE);
    kona_check_record_residue(wire);
    for (i = KONA_RECORD_SIZE; i < DS2502_DATA_SIZE; i++) {
        g_assert_cmphex(wire[i], ==, 0xff);
    }
}

static void test_onewire_geometry_identity(void)
{
    uint8_t identity[8];

    kona_reset();
    kona_build_identity(identity, KONA_ONEWIRE_FAMILY, KONA_GEOMETRY_SERIAL);
    kona_match_rom(identity);
    kona_assert_onewire_identity("00000001", "030-1398-001", "01",
                                 "GEOM-BD");
}

static void test_onewire_ktown_identity(void)
{
    uint8_t identity[8];

    kona_reset();
    kona_build_identity(identity, KONA_ONEWIRE_FAMILY, KONA_KTOWN_SERIAL);
    kona_match_rom(identity);
    kona_assert_onewire_identity("00000002", "030-1067-001", "01",
                                 "KTOWN-BD");
}

/*
 * Must-fix 2: a mutated WIDGET_CONTROL and a one-wire transaction left
 * mid-slot both go back to zero on system reset (sgi_kona_reset_hold(),
 * hw/misc/sgi-kona.c), matching hw/pci-host/sgi-bridge.c's own
 * sgi_bridge_reset_hold(). KONA_ONEWIRE_RESET()'s pulse/sample bits are
 * nonzero on their own, so onewire_reg reads back nonzero here whether or
 * not a part answers the reset pulse.
 */
static void test_reset_clears_plain_storage(void)
{
    kona_reset();
    qtest_writel(qts, REG_WIDGET_CONTROL, 0xdeadbeef);
    KONA_ONEWIRE_RESET();
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), !=, 0);
    g_assert_cmphex(qtest_readl(qts, REG_ONEWIRE), !=, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_ONEWIRE), ==, 0);
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
    qtest_add_func("sn0/origin2000/kona/"
                   "unknown-offset-reads-zero-and-accepts-writes",
                   test_unknown_offset_reads_zero_and_accepts_writes);
    qtest_add_func("sn0/origin2000/kona/onewire-geometry-identity",
                   test_onewire_geometry_identity);
    qtest_add_func("sn0/origin2000/kona/onewire-ktown-identity",
                   test_onewire_ktown_identity);
    qtest_add_func("sn0/origin2000/kona/reset-clears-plain-storage",
                   test_reset_clears_plain_storage);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
