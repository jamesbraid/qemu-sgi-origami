/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI KONA register-contract, one-wire identity and textport qtests.
 *
 * Increment 1 is identity only: a widget-ID/probe register set and the
 * DS2502 identity parts a stock PROM reads while classifying the
 * InfiniteReality graphics pipe (hw/misc/sgi-kona.c). Increment 2 adds the
 * `+0x800xxx` register window, the `+0xc10000` textport command port and a
 * native display surface; see docs/designs/
 * infinitereality-host-command-approximation.md for what each case below
 * approximates and why. This fixture fits KONA to an origin2000 the same
 * way sn-builder-test.c's boards reach it: as a `-device` at the empty
 * XIO slot IO4, crossbow0's widget 11, outside the default topology's
 * startup-links-alive (BaseIO at 8, the two host-node Hubs at 9 and
 * 10). It takes a fixed `id=kona0`
 * so the increment-2 cases can reach its `x-test-pixel` QOM properties --
 * but unlike a plain `-device` that stays where device_add put it,
 * hw/mips/sgi/xio-board.c's own sgi_xio_board_reparent() moves every
 * sgi-xtalk-board subclass out from under /machine/peripheral and into a
 * container named after its module once build_widget() returns, keeping
 * the `id=` name: /machine/module0/kona0, not /machine/peripheral/kona0.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
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

/*
 * The kernel driver's own register offset onto the identical one-wire
 * chain REG_ONEWIRE carries -- SGI_KONA_NIC's own header comment
 * (include/hw/misc/sgi-kona.h) has the established/inferred evidence.
 * Every helper below that used to hardcode REG_ONEWIRE now takes a
 * register argument, so the same Match ROM/Read Memory/Search ROM
 * exchanges below can run against either offset without a second,
 * duplicated set of primitives.
 */
#define REG_NIC            KONA_REG(0x00009c)

/*
 * Increment 2: the register window and textport command port, offsets
 * per include/hw/misc/sgi-kona.h.
 */
#define REG_LOW_WINDOW_WRITE KONA_REG(0x00007c)
#define REG_CREDIT           KONA_REG(0x800208)
#define REG_RESET_ARG        KONA_REG(0x800220)
#define REG_INT_CAUSE        KONA_REG(0x800238)
#define REG_MAILBOX          KONA_REG(0x800240)
#define REG_RESET_STROBE     KONA_REG(0x800300)
#define REG_EEPROM_ENABLE    KONA_REG(0x800308)
#define REG_EEPROM_ADDR      KONA_REG(0x8003e8)
#define REG_EEPROM_DATA      KONA_REG(0x8003f0)
#define REG_TEXTPORT         KONA_REG(0xc10000)

#define KONA_INT_CAUSE_MAILBOX (1u << 4)
#define KONA_EEPROM_CHECKED_ADDR  0x87ff1c
#define KONA_EEPROM_CHECKED_VALUE 2

#define MACHINE_ARGS \
    "-M origin2000 -device sgi-kona,id=kona0,slot=io4"

/*
 * origin2000's default topology is the full four-node/multi-CPU module
 * sn-catalogue.json describes, with a matching multi-gigabyte default RAM
 * size -- fine for every other case here, which shares one already-running
 * instance for plain register I/O, but too large to migrate twice (once
 * per instance) inside a single qtest's time budget. Pinned down the same
 * way sn0-origin2000-mgras-test.c's own MIGRATION_ARGS does, for the same
 * machine type and the same reason.
 */
#define MIGRATION_ARGS MACHINE_ARGS " -smp 8 -m 256M"

#define KONA_QOM_PATH "/machine/module0/kona0"

/*
 * hw/misc/sgi-mgras.c's own qtest fixture hand-rolls this same helper
 * (mgras_qom_get_uint()): libqtest.h has no generic uint getter, only
 * qtest_qom_get_bool()/qtest_qom_set_bool().
 */
static uint64_t kona_qom_get_uint(QTestState *s, const char *property)
{
    QDict *resp = qtest_qmp(s,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': %s}}", KONA_QOM_PATH, property);
    uint64_t value;

    if (!qdict_haskey(resp, "return")) {
        g_autoptr(GString) str = qobject_to_json(QOBJECT(resp));
        g_error("qom-get %s failed: %s", property, str->str);
    }
    value = qdict_get_int(resp, "return");
    qobject_unref(resp);
    return value;
}

static uint32_t kona_test_pixel(QTestState *s, uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", KONA_QOM_PATH, x);
    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", KONA_QOM_PATH, y);
    return kona_qom_get_uint(s, "x-test-pixel");
}

/*
 * Push one textport wire word: the fixed leading sync word `1`, then a
 * selector, then its argc argument words -- hw/misc/sgi-kona.c's own
 * sgi_kona_textport_push()/sgi_command_port_write() framing. Callers
 * push a whole command in one call so a failed assertion never leaves a
 * later case mid-packet.
 */
static void kona_textport_push(QTestState *s, uint32_t selector,
                               const uint32_t *args, unsigned argc)
{
    unsigned i;

    qtest_writel(s, REG_TEXTPORT, 1);
    qtest_writel(s, REG_TEXTPORT, selector);
    for (i = 0; i < argc; i++) {
        qtest_writel(s, REG_TEXTPORT, args[i]);
    }
}

/*
 * Drain one already-queued mailbox reply word: read it, consume it, then
 * acknowledge the cause bit. A reply queued behind it re-arms the cause
 * bit synchronously (hw/misc/sgi-kona.c's sgi_kona_write()), so the
 * caller learns whether more remain from this call's return value
 * instead of a poll loop -- the mailbox's own queue length is always
 * known up front, so no case needs one.
 */
static uint32_t kona_mailbox_drain_one(QTestState *s, bool *more_pending)
{
    uint32_t value = qtest_readl(s, REG_MAILBOX);

    uint32_t cause;

    qtest_writel(s, REG_MAILBOX, 0);
    qtest_writel(s, REG_INT_CAUSE, KONA_INT_CAUSE_MAILBOX);
    cause = qtest_readl(s, REG_INT_CAUSE);
    *more_pending = (cause & KONA_INT_CAUSE_MAILBOX) != 0;
    return value;
}

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

/*
 * `reg` selects REG_ONEWIRE or REG_NIC: two register offsets onto the
 * identical onewire_reg/onewire_bus state (hw/misc/sgi-kona.c), so a
 * caller can run the exact same bit-banged exchange through either one.
 */
static bool kona_slot(QTestState *s, uint64_t reg, unsigned int pulse,
                      unsigned int sample)
{
    uint32_t val;

    qtest_writel(s, reg,
                 KONA_ONEWIRE_PULSE(pulse) | KONA_ONEWIRE_SAMPLE(sample));
    val = qtest_readl(s, reg);
    g_assert_cmphex(val & KONA_ONEWIRE_DONE, ==, KONA_ONEWIRE_DONE);
    return val & KONA_ONEWIRE_LEVEL;
}

#define KONA_ONEWIRE_RESET(s, reg) kona_slot((s), (reg), 520, 65)
#define KONA_READ(s, reg)          kona_slot((s), (reg), 6, 13)
#define KONA_WRITE_ONE(s, reg)     kona_slot((s), (reg), 6, 13)
#define KONA_WRITE_ZERO(s, reg)    kona_slot((s), (reg), 80, 30)

static void kona_write_byte(QTestState *s, uint64_t reg, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            KONA_WRITE_ONE(s, reg);
        } else {
            KONA_WRITE_ZERO(s, reg);
        }
    }
}

static uint8_t kona_read_byte(QTestState *s, uint64_t reg)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (KONA_READ(s, reg)) {
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
static void kona_match_rom(QTestState *s, uint64_t reg,
                           const uint8_t identity[8])
{
    unsigned int i;

    g_assert_false(KONA_ONEWIRE_RESET(s, reg)); /* pulled low: a part answers */
    kona_write_byte(s, reg, 0x55);               /* Match ROM */
    for (i = 0; i < 8; i++) {
        kona_write_byte(s, reg, identity[i]);
    }
}

/* The same byte as DS2502_CMD_READ_MEMORY below, before any part is matched. */
#define KONA_ONEWIRE_CMD_SEARCH_ROM 0xf0

/*
 * One pass of the standard one-wire Search ROM discrepancy walk (Maxim
 * AN187), taking the zero branch first. Chain each pass's result and
 * `*last_discrepancy` into the next, starting from 0 and 0.
 * sn0-bridge-identity-test.c's own nic_search_pass() runs the identical
 * algorithm over Bridge's NIC register; reused here rather than
 * reimplemented because REG_ONEWIRE and REG_NIC share the same one-wire
 * ROM-command state machine (hw/1wire/core.c) Bridge's NIC register does.
 * This is the walk the orchestrating datasheet note's discriminating test
 * asks for: whether +0x9c's own Search ROM finds the same parts +0x98
 * does on a rack, and only the geometry board on a deskside pipe.
 */
static uint64_t kona_search_pass(QTestState *s, uint64_t reg, uint64_t rom,
                                 unsigned int *last_discrepancy)
{
    unsigned int bit_number, last_zero = 0;
    uint64_t found = 0;

    /* Pulled low: someone answers. */
    g_assert_false(KONA_ONEWIRE_RESET(s, reg));
    kona_write_byte(s, reg, KONA_ONEWIRE_CMD_SEARCH_ROM);
    for (bit_number = 1; bit_number <= 64; bit_number++) {
        bool bit = KONA_READ(s, reg);
        bool complement = KONA_READ(s, reg);
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
            KONA_WRITE_ONE(s, reg);
        } else {
            KONA_WRITE_ZERO(s, reg);
        }
    }
    *last_discrepancy = last_zero;
    return found;
}

/* Discover every part on the chain through `reg`; returns how many. */
static unsigned int kona_search_all(QTestState *s, uint64_t reg,
                                    uint64_t *rom, unsigned int max)
{
    unsigned int last_discrepancy = 0;
    unsigned int count = 0;

    do {
        g_assert_cmpuint(count, <, max);
        rom[count] = kona_search_pass(s, reg, count ? rom[count - 1] : 0,
                                      &last_discrepancy);
        count++;
    } while (last_discrepancy != 0);
    return count;
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
static void kona_read_memory(QTestState *s, uint64_t reg,
                             uint8_t out[DS2502_DATA_SIZE])
{
    uint8_t hdr[3] = { DS2502_CMD_READ_MEMORY, 0x00, 0x00 };
    unsigned int i;

    for (i = 0; i < sizeof(hdr); i++) {
        kona_write_byte(s, reg, hdr[i]);
    }
    g_assert_cmphex(kona_read_byte(s, reg), ==, kona_crc8(0, hdr, sizeof(hdr)));

    for (i = 0; i < DS2502_DATA_SIZE; i++) {
        out[i] = kona_read_byte(s, reg);
    }
    g_assert_cmphex(kona_read_byte(s, reg), ==,
                    kona_crc8(0, out, DS2502_DATA_SIZE));
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

/*
 * kona_reset()'s call to qtest_system_reset() now clears every increment-2
 * field too (sgi_kona_reset_hold(), hw/misc/sgi-kona.c) --
 * test_reset_clears_increment2_state() below is the claim that checks it.
 * Mailbox and framing state were always self-contained regardless of
 * reset -- sgi_kona_mailbox_enqueue() fully replaces the queue and length
 * on every push, and every packet handler finishes by rearming
 * textport_expect_sync -- but the pixel-rendering cases below still each
 * use their own previously untouched coordinates: harmless now that reset
 * also clears the framebuffer, and it keeps each case's assertions
 * independent of case-run order either way.
 */

static void test_credit_register_reads_fixed_zero(void)
{
    kona_reset();
    /* The command port's own credit hook is left NULL. */
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT), ==, 0);
}

static void test_low_window_write_is_plain_storage(void)
{
    kona_reset();
    qtest_writel(qts, REG_LOW_WINDOW_WRITE, 0x23);
    g_assert_cmphex(qtest_readl(qts, REG_LOW_WINDOW_WRITE), ==, 0x23);
}

static void test_reset_and_eeprom_control_registers_are_plain_storage(void)
{
    kona_reset();

    qtest_writel(qts, REG_RESET_ARG, 3);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 3);
    qtest_writel(qts, REG_RESET_ARG, 2);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 2);

    qtest_writel(qts, REG_RESET_STROBE, 1);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_STROBE), ==, 1);
    qtest_writel(qts, REG_RESET_STROBE, 0);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_STROBE), ==, 0);

    qtest_writel(qts, REG_EEPROM_ENABLE, 1);
    g_assert_cmphex(qtest_readl(qts, REG_EEPROM_ENABLE), ==, 1);
}

static void test_eeprom_data_answers_checked_address_and_zero_elsewhere(void)
{
    kona_reset();

    qtest_writel(qts, REG_EEPROM_ADDR, KONA_EEPROM_CHECKED_ADDR);
    g_assert_cmphex(qtest_readl(qts, REG_EEPROM_ADDR), ==,
                    KONA_EEPROM_CHECKED_ADDR);
    g_assert_cmphex(qtest_readl(qts, REG_EEPROM_DATA), ==,
                    KONA_EEPROM_CHECKED_VALUE);

    qtest_writel(qts, REG_EEPROM_ADDR, 0);
    g_assert_cmphex(qtest_readl(qts, REG_EEPROM_DATA), ==, 0);
}

/*
 * Sync probe: "1,9,0" on the wire. The mailbox's own single reply word
 * (0x1234abcd) drains in one step with no more pending behind it -- the
 * full read/consume/acknowledge sequence, needing no poll loop since the
 * queue length here is always exactly one.
 */
static void test_textport_sync_probe_mailbox_protocol(void)
{
    uint32_t reply;
    bool more_pending;

    kona_reset();

    kona_textport_push(qts, 0x9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) & KONA_INT_CAUSE_MAILBOX,
                    ==, KONA_INT_CAUSE_MAILBOX);

    reply = kona_mailbox_drain_one(qts, &more_pending);
    g_assert_cmphex(reply, ==, 0x1234abcd);
    g_assert_false(more_pending);
}

/*
 * Graphics info: "1,0xc,0" on the wire, replying five words -- width and
 * height (established) at indices 3 and 4, the other three approximated
 * zero. The cause bit re-arms synchronously behind each of the first four
 * drains and clears only after the fifth.
 */
static void test_textport_graphics_info_mailbox_protocol(void)
{
    static const uint32_t expected[5] = { 0, 0, 0, 1280, 1024 };
    unsigned i;
    bool more_pending = true;

    kona_reset();

    kona_textport_push(qts, 0xc, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) & KONA_INT_CAUSE_MAILBOX,
                    ==, KONA_INT_CAUSE_MAILBOX);

    for (i = 0; i < 5; i++) {
        g_assert_cmphex(kona_mailbox_drain_one(qts, &more_pending), ==,
                        expected[i]);
        g_assert_true(more_pending == (i < 4));
    }
}

/*
 * Set color (0x2) then point (0x6): the only two textport selectors with
 * a confirmed rendering role beyond the bitmap blit (docs/datasheets/
 * infinitereality-display.md). Each point uses a coordinate no earlier
 * case in this file touches.
 */
static void test_textport_set_color_and_point_render_pixels(void)
{
    kona_reset();

    kona_textport_push(qts, 0x2, (uint32_t[]){ 0x00ff0000 }, 1);
    kona_textport_push(qts, 0x6, (uint32_t[]){ 100, 50 }, 2);
    g_assert_cmphex(kona_test_pixel(qts, 100, 50), ==, 0x00ff0000);

    kona_textport_push(qts, 0x2, (uint32_t[]){ 0x0000ff00 }, 1);
    kona_textport_push(qts, 0x6, (uint32_t[]){ 200, 150 }, 2);
    g_assert_cmphex(kona_test_pixel(qts, 200, 150), ==, 0x0000ff00);
    /* The first point's own color is untouched by the second. */
    g_assert_cmphex(kona_test_pixel(qts, 100, 50), ==, 0x00ff0000);
}

/*
 * Screen size (0x8): accepted and traced only. The framebuffer's own
 * dimensions, read back through a graphics-info reply, stay fixed at
 * 1280x1024 regardless of what this command asked for.
 */
static void test_textport_screen_size_is_traced_only(void)
{
    static const uint32_t expected[5] = { 0, 0, 0, 1280, 1024 };
    unsigned i;
    bool more_pending = true;

    kona_reset();

    kona_textport_push(qts, 0x8, (uint32_t[]){ 640, 480 }, 2);

    kona_textport_push(qts, 0xc, (uint32_t[]){ 0 }, 1);
    for (i = 0; i < 5; i++) {
        g_assert_cmphex(kona_mailbox_drain_one(qts, &more_pending), ==,
                        expected[i]);
    }
}

/*
 * Bitmap/glyph blit (0xa): six header words (x, y, width, height, then
 * two unconfirmed fields), then a packed 1-bpp payload, one bit per
 * pixel, MSB-first, row-major -- hw/misc/sgi-kona.c's own
 * sgi_kona_bitmap_blit(). A 4x2 bitmap needs one payload word; only its
 * top byte (0xa5 = 1010 0101) is meaningful; second row is (0,1,0,1).
 */
static void test_textport_bitmap_blit_renders_payload(void)
{
    kona_reset();

    kona_textport_push(qts, 0x2, (uint32_t[]){ 0x00abcdef }, 1);
    kona_textport_push(qts, 0xa, (uint32_t[]){ 300, 400, 4, 2, 0, 0 }, 6);
    qtest_writel(qts, REG_TEXTPORT, 0xa5000000);

    g_assert_cmphex(kona_test_pixel(qts, 300, 400), ==, 0x00abcdef);
    g_assert_cmphex(kona_test_pixel(qts, 301, 400), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 302, 400), ==, 0x00abcdef);
    g_assert_cmphex(kona_test_pixel(qts, 303, 400), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 300, 401), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 301, 401), ==, 0x00abcdef);
    g_assert_cmphex(kona_test_pixel(qts, 302, 401), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 303, 401), ==, 0x00abcdef);
}

/*
 * Selectors 1, 3, 4, 5 and 7 have an established argument shape but no
 * confirmed rendering, blanking or cursor role (docs/datasheets/
 * infinitereality-display.md's "Textport-era blanking and cursor
 * commands"): that same evidence leaves kona_unblank() itself unread, so
 * which command number, if any, it pushes to +0xc10000 is unknown, and
 * none of these five may be assumed to carry it. This model frames and
 * traces all five only.
 */
static void test_textport_reserved_selectors_have_no_render_effect(void)
{
    static const uint32_t args1[1] = { 0 };
    static const uint32_t args2[2] = { 0, 0 };
    static const uint32_t args4[4] = { 0, 0, 0, 0 };

    kona_reset();

    g_assert_cmphex(kona_test_pixel(qts, 900, 900), ==, 0);

    kona_textport_push(qts, 0x1, args1, 1);
    kona_textport_push(qts, 0x3, args4, 4);
    kona_textport_push(qts, 0x4, args2, 2);
    kona_textport_push(qts, 0x5, args4, 4);
    kona_textport_push(qts, 0x7, args2, 2);

    g_assert_cmphex(kona_test_pixel(qts, 900, 900), ==, 0);

    /*
     * The port is left ready for a fresh sync-prefixed command, not stuck
     * framing one of these -- proven by a probe right after completing
     * correctly.
     */
    kona_textport_push(qts, 0x9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) & KONA_INT_CAUSE_MAILBOX,
                    ==, KONA_INT_CAUSE_MAILBOX);
}

/*
 * A selector outside sgi_kona_textport_selectors[] reaches the command
 * port's own unsupported() path (sgi_kona_textport_unsupported()), which
 * resynchronizes the textport instead of assuming an argument count for
 * an unknown code -- proven the same way as the reserved-selector case
 * above, by a probe completing correctly right after.
 */
static void test_textport_unknown_selector_resynchronizes(void)
{
    uint32_t reply;
    bool more_pending;

    kona_reset();

    qtest_writel(qts, REG_TEXTPORT, 1);
    qtest_writel(qts, REG_TEXTPORT, 0xff);

    kona_textport_push(qts, 0x9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) & KONA_INT_CAUSE_MAILBOX,
                    ==, KONA_INT_CAUSE_MAILBOX);
    reply = kona_mailbox_drain_one(qts, &more_pending);
    g_assert_cmphex(reply, ==, 0x1234abcd);
    g_assert_false(more_pending);
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
static void kona_assert_onewire_identity(QTestState *s, uint64_t reg,
                                         const char *barcode,
                                         const char *part,
                                         const char *revision,
                                         const char *name)
{
    uint8_t wire[DS2502_DATA_SIZE];
    uint8_t expected[KONA_RECORD_SIZE];
    unsigned int i;

    kona_read_memory(s, reg, wire);

    kona_build_expected_record(expected, barcode, part, revision, name);
    g_assert_cmpmem(wire, KONA_RECORD_SIZE, expected, KONA_RECORD_SIZE);
    kona_check_record_residue(wire);
    for (i = KONA_RECORD_SIZE; i < DS2502_DATA_SIZE; i++) {
        g_assert_cmphex(wire[i], ==, 0xff);
    }
}

/*
 * "other-identity-parts" defaults to the single Ktown part
 * (030-1067-001, sgi_kona_instance_init(), hw/misc/sgi-kona.c), so both
 * cases below, run against the default qts instance, also cover that
 * default: the geometry board's own record always answers regardless of
 * the property, and Ktown answers only because the default list names it.
 */
static void test_onewire_geometry_identity(void)
{
    uint8_t identity[8];

    kona_reset();
    kona_build_identity(identity, KONA_ONEWIRE_FAMILY, KONA_GEOMETRY_SERIAL);
    kona_match_rom(qts, REG_ONEWIRE, identity);
    kona_assert_onewire_identity(qts, REG_ONEWIRE, "00000001", "030-1398-001",
                                 "01", "GEOM-BD");
}

static void test_onewire_ktown_identity(void)
{
    uint8_t identity[8];

    kona_reset();
    kona_build_identity(identity, KONA_ONEWIRE_FAMILY, KONA_KTOWN_SERIAL);
    kona_match_rom(qts, REG_ONEWIRE, identity);
    kona_assert_onewire_identity(qts, REG_ONEWIRE, "00000002", "030-1067-001",
                                 "01",
                                 "KTOWN-BD");
}

/*
 * An empty "other-identity-parts" list models a deskside pipe with no
 * Ktown board: the geometry record still answers (finish_realize attaches
 * it unconditionally), but Match ROM against Ktown's own identity finds no
 * part to select. A fresh instance is needed because the default qts
 * already carries Ktown and the property is construction-time only.
 *
 * The command-line keyval grammar (util/keyval.c) has no syntax for an
 * explicitly empty list -- omitting every "other-identity-parts.N=" key
 * leaves the instance_init default in place, it does not override it to
 * empty. QMP device_add cannot substitute either: every sgi-kona
 * instance realizes onto the plain system bus (sgi_xio_board_reparent(),
 * hw/mips/sgi/xio-board.c), which has no hotplug handler, and
 * qdev_device_add_from_qdict()'s hotplug check (system/qdev-monitor.c)
 * is only skipped before PHASE_MACHINE_READY -- true while cold-boot
 * "-device" arguments are still being processed, false for any device_add
 * issued once qtest has connected. A bare '{...}' "-device" argument is
 * parsed by qobject_from_json() (system/vl.c) and reaches that same
 * qmp_device_add() during qemu_create_cli_devices(), while still
 * cold-booting, through its real QObject visitor -- which, unlike keyval,
 * can express a genuine empty array. tests/qtest/sn1-l1-test.c's own
 * JSON "-device" fixtures set sgi-l1's chassis-eeprom/board-eeprom list
 * properties the same way. Same slot as MACHINE_ARGS's own
 * kona0, so KONA_REG's fixed widget still lands on this board.
 */
static void test_onewire_other_identity_parts_empty(void)
{
    QTestState *s;
    uint8_t identity[8];

    s = qtest_init(
        "-M origin2000 -device '{\"driver\":\"sgi-kona\",\"id\":\"kona1\","
        "\"slot\":\"io4\","
        "\"other-identity-parts\":[]}'");
    g_assert_cmphex(qtest_readl(s, REG_PROBE), ==, 0);

    kona_build_identity(identity, KONA_ONEWIRE_FAMILY, KONA_GEOMETRY_SERIAL);
    kona_match_rom(s, REG_ONEWIRE, identity);
    kona_assert_onewire_identity(s, REG_ONEWIRE, "00000001", "030-1398-001",
                                 "01", "GEOM-BD");

    /*
     * Match ROM against Ktown's own identity still sees the reset pulse
     * pulled low (geometry answers every reset regardless of which
     * identity gets selected next), but no part on this chain matches it,
     * so the bus idles for the rest of the transaction. KONA_READ() alone
     * cannot tell that apart from a real, matched Ktown waiting for its
     * next command: both read a single undriven "1" either way, because
     * neither is a complete ROM-command slot -- onewire_slave_slot_advance()
     * (hw/1wire/core.c) never pulls low mid ROM-command, so KONA_READ()'s
     * and KONA_WRITE_ONE()'s shared pulse/sample timing looks the same
     * whether or not a genuine part is selected underneath it.
     *
     * Sending Ktown's own Read Memory header instead does discriminate: a
     * real, selected DS2502 answers with the CRC-8 over that header
     * (kona_read_memory() checks exactly that in
     * test_onewire_ktown_identity() above, where Ktown genuinely answers),
     * but with no part selected, every bit of the reply -- the echoed CRC
     * byte included -- reads as the undriven "1", so the byte comes back
     * 0xff instead.
     */
    kona_build_identity(identity, KONA_ONEWIRE_FAMILY, KONA_KTOWN_SERIAL);
    kona_match_rom(s, REG_ONEWIRE, identity);
    kona_write_byte(s, REG_ONEWIRE, DS2502_CMD_READ_MEMORY);
    kona_write_byte(s, REG_ONEWIRE, 0x00);
    kona_write_byte(s, REG_ONEWIRE, 0x00);
    g_assert_cmphex(kona_read_byte(s, REG_ONEWIRE), ==, 0xff);

    qtest_quit(s);
}

/*
 * A rejection reports through error_report and exits; it must not reach
 * the guest as a signal (SIGABRT from a failed assertion, SIGSEGV from a
 * wild pointer). WIFSIGNALED tells the two apart where a bare nonzero
 * exit status cannot. Reimplemented locally from
 * tests/qtest/sn-builder-test.c's own assert_qemu_rejects(): a qtest
 * binary cannot link another qtest file's static helper.
 */
static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                               NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_false(WIFSIGNALED(wait_status));
    g_assert_nonnull(strstr(error_output, message));
}

/*
 * An unrecognized part number is a realize-time configuration error, not a
 * silently-ignored entry: sgi_kona_finish_realize()'s own lookup against
 * sgi_kona_identity_parts[] (hw/misc/sgi-kona.c) rejects it before the
 * machine starts. The command-line keyval grammar's dotted-index syntax
 * ("other-identity-parts.0=", util/keyval.c) has a confirmed precedent
 * for -machine properties (hw/mips/sgi/sn-machine.c's own
 * chassis-eeprom.0=, exercised by tests/qtest/sn-builder-test.c and
 * tests/qtest/sn1-address-map-test.c), but no working precedent for a
 * -device property anywhere in this tree: -device arguments parse
 * through qemu_opts_parse_noisily() (system/vl.c), an older option-list
 * reader that takes "other-identity-parts.0" as one literal property
 * name rather than an index into a list, and rejects it as not found.
 * Confirmed by trying it (qemu-system-mips64: -device
 * sgi-kona,other-identity-parts.0=999-9999-999: Property
 * 'sgi-kona.other-identity-parts.0' not found). A one-element list here
 * therefore uses the same bare JSON "-device" form the empty-list case
 * above does, which does understand a real JSON array regardless of
 * qemu_opts_parse_noisily()'s own limits.
 */
static void test_onewire_other_identity_parts_unknown_rejected(void)
{
    /*
     * "-qtest stdio -accel qtest" makes qtest_enabled() true without a
     * live qtest peer, the same way tests/qtest/sn0-ioc3-test.c's own
     * assert_qemu_rejects() callers do for this machine family: without
     * it, sgi_machine_topology_apply()'s "supports stock-PROM boot only"
     * check (hw/mips/sgi/sn-machine.c) rejects the missing -bios/pflash
     * before realize ever reaches this device's own check. The slot
     * matches MACHINE_ARGS's own kona0, so realize gets far enough to
     * look at other-identity-parts at all.
     */
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000",
        "-device",
        "{\"driver\":\"sgi-kona\",\"slot\":\"io4\","
        "\"other-identity-parts\":[\"999-9999-999\"]}",
        "-display", "none", "-qtest", "stdio", "-accel", "qtest", NULL
    };

    assert_qemu_rejects(args, "unknown other-identity-parts entry");
}

/*
 * A repeated part number would otherwise abort the whole process:
 * sgi_kona_onewire_attach_part() names each part's child object after its
 * own fixed child_name ("onewire-ktown" for Ktown), and
 * object_property_add_child() (qom/object.c) takes a second add under the
 * same name straight through &error_abort. sgi_kona_finish_realize() must
 * catch the repeat itself before ever attaching the same part twice, and
 * report it the same clean, non-signal way
 * test_onewire_other_identity_parts_unknown_rejected() above checks for an
 * unrecognized one.
 */
static void test_onewire_other_identity_parts_duplicate_rejected(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000",
        "-device",
        "{\"driver\":\"sgi-kona\",\"slot\":\"io4\","
        "\"other-identity-parts\":[\"030-1067-001\",\"030-1067-001\"]}",
        "-display", "none", "-qtest", "stdio", "-accel", "qtest", NULL
    };

    assert_qemu_rejects(args, "duplicate other-identity-parts entry");
}

/*
 * "board-part" configures the geometry board's own record, the same way
 * "other-identity-parts" configures which parts join it: a real deskside
 * pipe's GE14-4 board (030-1129-002) differs from a rack's GE16-4 default
 * (030-1398-001, test_onewire_geometry_identity() above). A fresh instance
 * is needed, like the empty-list case above: the property is
 * construction-time only and the default qts already carries the rack's
 * part.
 */
static void test_onewire_board_part_configures_geometry_record(void)
{
    QTestState *s;
    uint8_t identity[8];

    s = qtest_init(
        "-M origin2000 -device sgi-kona,id=kona2,slot=io4,"
        "board-part=030-1129-002");
    g_assert_cmphex(qtest_readl(s, REG_PROBE), ==, 0);

    kona_build_identity(identity, KONA_ONEWIRE_FAMILY, KONA_GEOMETRY_SERIAL);
    kona_match_rom(s, REG_ONEWIRE, identity);
    kona_assert_onewire_identity(s, REG_ONEWIRE, "00000001", "030-1129-002",
                                 "01", "GEOM-BD");

    qtest_quit(s);
}

/*
 * board-part shares its check, sgi_bridge_board_part_error()
 * (include/hw/pci-host/sgi-bridge.h), with hw/pci-host/sgi-bridge.c: a
 * malformed value must still be refused at realize, and the message KONA
 * reports for its own rejection must name sgi-kona, not the shared check's
 * former hard-coded "the I/O bridge" -- this device is not a bridge, and a
 * misnamed message would send a reader chasing the wrong contract.
 */
static void test_board_part_invalid_is_rejected(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000",
        "-device",
        "sgi-kona,id=kona2,slot=io4,"
        "board-part=030-1129",
        "-display", "none", "-qtest", "stdio", "-accel", "qtest", NULL
    };

    assert_qemu_rejects(args,
                        "sgi-kona board part must be 12 characters without "
                        "a semicolon");
}

/*
 * REG_NIC (+0x9c) is the kernel driver's own offset onto the identical
 * one-wire chain REG_ONEWIRE (+0x98) carries, established by
 * kona_attach()'s call into nic_xg_vertex_info() and the disassembly
 * trace into nic.o's generic one-wire NIC driver -- not a second,
 * independent chain. The most direct proof is register-level: a slot
 * issued through one offset must read back through the other with no
 * further pulse, since both drive and observe the identical onewire_reg
 * field (SGI_KONA_NIC's own header comment, include/hw/misc/sgi-kona.h,
 * has the established/inferred evidence and the discriminating test that
 * would tell a shared register apart from two chains sharing one
 * protocol).
 */
static void test_nic_register_mirrors_onewire_register_state(void)
{
    uint32_t via_onewire, via_nic;

    kona_reset();

    /* A slot driven through +0x98 reads back identically through +0x9c. */
    KONA_WRITE_ONE(qts, REG_ONEWIRE);
    via_onewire = qtest_readl(qts, REG_ONEWIRE);
    via_nic = qtest_readl(qts, REG_NIC);
    g_assert_cmphex(via_nic, ==, via_onewire);

    kona_reset();

    /* And the reverse: a slot driven through +0x9c reads back via +0x98. */
    KONA_WRITE_ZERO(qts, REG_NIC);
    via_nic = qtest_readl(qts, REG_NIC);
    via_onewire = qtest_readl(qts, REG_ONEWIRE);
    g_assert_cmphex(via_onewire, ==, via_nic);
}

/*
 * The same records answer through either register: replaying
 * test_onewire_geometry_identity() and test_onewire_ktown_identity()'s
 * own Match ROM/Read Memory exchanges against REG_NIC on the same shared
 * rack instance gets the identical geometry-board and Ktown records.
 */
static void test_nic_identity_matches_onewire_identity(void)
{
    uint8_t identity[8];

    kona_reset();
    kona_build_identity(identity, KONA_ONEWIRE_FAMILY, KONA_GEOMETRY_SERIAL);
    kona_match_rom(qts, REG_NIC, identity);
    kona_assert_onewire_identity(qts, REG_NIC, "00000001", "030-1398-001",
                                 "01", "GEOM-BD");

    kona_reset();
    kona_build_identity(identity, KONA_ONEWIRE_FAMILY, KONA_KTOWN_SERIAL);
    kona_match_rom(qts, REG_NIC, identity);
    kona_assert_onewire_identity(qts, REG_NIC, "00000002", "030-1067-001",
                                 "01", "KTOWN-BD");
}

/*
 * Validate a Search ROM hit's own trailing CRC-8 and return the family
 * byte and serial it encodes, the same layout kona_build_identity()
 * builds byte by byte.
 */
static void kona_check_search_hit(uint64_t rom, uint8_t *family,
                                  uint64_t *serial)
{
    uint8_t identity[8];
    unsigned int i;

    for (i = 0; i < 8; i++) {
        identity[i] = (rom >> (8 * i)) & 0xff;
    }
    g_assert_cmphex(kona_crc8(0, identity, 7), ==, identity[7]);

    *family = identity[0];
    *serial = 0;
    for (i = 0; i < 6; i++) {
        *serial |= (uint64_t)identity[1 + i] << (8 * i);
    }
}

/*
 * The ROM search through +0x9c walks both parts on the default rack
 * fixture (the geometry board plus Ktown, "other-identity-parts"'s own
 * default) and only the geometry board on a deskside pipe with an empty
 * "other-identity-parts" list -- the same configuration
 * test_onewire_other_identity_parts_empty() above uses. Which part the
 * search visits first is not asserted, matching
 * sn0-bridge-identity-test.c's own test_records_match_the_datasheet(): a
 * two-part bus's discovery order is not part of this register's
 * contract.
 */
static void test_nic_search_rom_walks_rack_and_deskside(void)
{
    uint64_t rom[4];
    unsigned int count, i;
    bool seen_geometry, seen_ktown;
    QTestState *s;

    kona_reset();
    count = kona_search_all(qts, REG_NIC, rom, ARRAY_SIZE(rom));
    g_assert_cmpuint(count, ==, 2);

    seen_geometry = seen_ktown = false;
    for (i = 0; i < count; i++) {
        uint8_t family;
        uint64_t serial;

        kona_check_search_hit(rom[i], &family, &serial);
        g_assert_cmphex(family, ==, KONA_ONEWIRE_FAMILY);
        if (serial == KONA_GEOMETRY_SERIAL) {
            seen_geometry = true;
        } else if (serial == KONA_KTOWN_SERIAL) {
            seen_ktown = true;
        } else {
            g_assert_not_reached();
        }
    }
    g_assert_true(seen_geometry);
    g_assert_true(seen_ktown);

    s = qtest_init(
        "-M origin2000 -device '{\"driver\":\"sgi-kona\",\"id\":\"kona3\","
        "\"slot\":\"io4\","
        "\"other-identity-parts\":[]}'");
    g_assert_cmphex(qtest_readl(s, REG_PROBE), ==, 0);

    count = kona_search_all(s, REG_NIC, rom, ARRAY_SIZE(rom));
    g_assert_cmpuint(count, ==, 1);
    {
        uint8_t family;
        uint64_t serial;

        kona_check_search_hit(rom[0], &family, &serial);
        g_assert_cmphex(family, ==, KONA_ONEWIRE_FAMILY);
        g_assert_cmphex(serial, ==, KONA_GEOMETRY_SERIAL);
    }

    qtest_quit(s);
}

/*
 * Must-fix 2: a mutated WIDGET_CONTROL and a one-wire transaction left
 * mid-slot both go back to zero on system reset (sgi_kona_reset_hold(),
 * hw/misc/sgi-kona.c), matching hw/pci-host/sgi-bridge.c's own
 * sgi_bridge_reset_hold(). KONA_ONEWIRE_RESET()'s pulse/sample bits are
 * nonzero on their own, so onewire_reg reads back nonzero here whether or
 * not a part answers the reset pulse. REG_NIC is checked too: it is the
 * identical onewire_reg, not a second field reset would need to know
 * about separately.
 */
static void test_reset_clears_plain_storage(void)
{
    kona_reset();
    qtest_writel(qts, REG_WIDGET_CONTROL, 0xdeadbeef);
    KONA_ONEWIRE_RESET(qts, REG_ONEWIRE);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), !=, 0);
    g_assert_cmphex(qtest_readl(qts, REG_ONEWIRE), !=, 0);
    g_assert_cmphex(qtest_readl(qts, REG_NIC), ==,
                    qtest_readl(qts, REG_ONEWIRE));

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_ONEWIRE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_NIC), ==, 0);
}

/*
 * Independent review (Origami PR #1799) found increment 2's own fields
 * left out of the widget's existing reset method: sgi_kona_reset_hold()
 * clears widget_control/onewire_reg but stopped there. This is the claim
 * that the fix (hw/misc/sgi-kona.c) covers every field increment 2 added:
 * the register window, the mailbox queue, the textport command port's
 * framing state and the framebuffer.
 *
 * Two phases, dirtied and reset separately: the textport's generic-framer
 * mid-packet state and its bitmap mid-payload state are mutually
 * exclusive (the bitmap interceptor only engages once selector 0xa is
 * seen, and while engaged no word reaches the generic framer), so no
 * single dirty pass can leave both at once.
 *
 * Phase 1 dirties the generic framer with selector 0x3 (argc 4),
 * deliberately not the verification probe's own argc (selector 0x9,
 * argc 1): a framer left stuck mid packet would otherwise misconsume the
 * probe's own words at the same argc and coincidentally complete with
 * the same net effect, passing even though reset never ran. With
 * mismatched argc, a stuck framer instead misaligns word consumption and
 * the probe genuinely fails to complete.
 */
static void test_reset_clears_increment2_state(void)
{
    uint32_t reply;
    bool more_pending;

    kona_reset();

    qtest_writel(qts, REG_LOW_WINDOW_WRITE, 0x23);
    qtest_writel(qts, REG_RESET_ARG, 3);
    qtest_writel(qts, REG_RESET_STROBE, 1);
    qtest_writel(qts, REG_EEPROM_ENABLE, 1);
    qtest_writel(qts, REG_EEPROM_ADDR, KONA_EEPROM_CHECKED_ADDR);

    kona_textport_push(qts, 0x2, (uint32_t[]){ 0x00c0ffee }, 1);
    kona_textport_push(qts, 0x6, (uint32_t[]){ 700, 700 }, 2);
    g_assert_cmphex(kona_test_pixel(qts, 700, 700), ==, 0x00c0ffee);

    /* An undrained mailbox reply, left queued with its cause bit set. */
    kona_textport_push(qts, 0x9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) & KONA_INT_CAUSE_MAILBOX,
                    ==, KONA_INT_CAUSE_MAILBOX);

    /*
     * The generic framer mid-packet: a sync word, selector 0x3, and two
     * of its four established arguments.
     */
    qtest_writel(qts, REG_TEXTPORT, 1);
    qtest_writel(qts, REG_TEXTPORT, 0x3);
    qtest_writel(qts, REG_TEXTPORT, 0x11);
    qtest_writel(qts, REG_TEXTPORT, 0x22);

    qtest_system_reset(qts);

    g_assert_cmphex(qtest_readl(qts, REG_LOW_WINDOW_WRITE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_STROBE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_EEPROM_ENABLE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_EEPROM_ADDR), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) & KONA_INT_CAUSE_MAILBOX,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_MAILBOX), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 700, 700), ==, 0);

    /*
     * A fresh, complete sync probe must still complete correctly. See
     * this function's own comment for why selector 0x3's mismatched argc
     * makes this a genuine discriminator rather than a coincidental pass.
     */
    kona_textport_push(qts, 0x9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) & KONA_INT_CAUSE_MAILBOX,
                    ==, KONA_INT_CAUSE_MAILBOX);
    reply = kona_mailbox_drain_one(qts, &more_pending);
    g_assert_cmphex(reply, ==, 0x1234abcd);
    g_assert_false(more_pending);

    /*
     * Phase 2: the bitmap interceptor mid-payload instead. An 8x8 bitmap
     * needs two packed payload words; send only the first, leaving the
     * blit mid-transaction.
     */
    kona_reset();

    qtest_writel(qts, REG_TEXTPORT, 1);
    qtest_writel(qts, REG_TEXTPORT, 0xa);
    qtest_writel(qts, REG_TEXTPORT, 500);
    qtest_writel(qts, REG_TEXTPORT, 500);
    qtest_writel(qts, REG_TEXTPORT, 8);
    qtest_writel(qts, REG_TEXTPORT, 8);
    qtest_writel(qts, REG_TEXTPORT, 0);
    qtest_writel(qts, REG_TEXTPORT, 0);
    qtest_writel(qts, REG_TEXTPORT, 0xffffffff);
    g_assert_cmphex(kona_test_pixel(qts, 500, 500), ==, 0);

    qtest_system_reset(qts);

    g_assert_cmphex(kona_test_pixel(qts, 500, 500), ==, 0);

    /*
     * A fresh sync probe must still complete: a bitmap left stuck
     * mid-payload would instead swallow this probe's own leading sync
     * word as its second payload word and never resynchronize.
     */
    kona_textport_push(qts, 0x9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) & KONA_INT_CAUSE_MAILBOX,
                    ==, KONA_INT_CAUSE_MAILBOX);
    reply = kona_mailbox_drain_one(qts, &more_pending);
    g_assert_cmphex(reply, ==, 0x1234abcd);
    g_assert_false(more_pending);
}

/*
 * Independent review (Origami PR #1799) found the framebuffer pixel array
 * missing from vmstate_sgi_kona despite the header's own claim of parity
 * with hw/misc/sgi-mgras.c's migrated front/back planes. Draw a pixel
 * through the textport, migrate to a second instance, and read it back on
 * the destination: proves the array itself crossed over, not only the
 * registers and framing state the cases above already exercise. Follows
 * sn0-origin2000-mgras-test.c's own test_migration() -- the closest
 * sibling, another origin2000 xtalk-board graphics device with a migrated
 * pixel array and the same x-test-pixel introspection pattern -- rather
 * than tests/qtest/sgi-command-port-test.c's generic "-M none" fixture,
 * whose destination waits on "-incoming defer" plus a separate
 * migrate-incoming QMP command: mgras's own precedent instead starts the
 * destination directly with "-incoming <uri>" on its command line and
 * neither instance needs -S, so this follows suit rather than inventing a
 * third convention.
 */
static void test_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    kona_textport_push(from, 0x2, (uint32_t[]){ 0x00c0ffee }, 1);
    kona_textport_push(from, 0x6, (uint32_t[]){ 640, 480 }, 2);
    g_assert_cmphex(kona_test_pixel(from, 640, 480), ==, 0x00c0ffee);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(kona_test_pixel(to, 640, 480), ==, 0x00c0ffee);
    /* An untouched coordinate migrates as zero, not stale host memory. */
    g_assert_cmphex(kona_test_pixel(to, 641, 480), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
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
    qtest_add_func("sn0/origin2000/kona/onewire-other-identity-parts-empty",
                   test_onewire_other_identity_parts_empty);
    qtest_add_func("sn0/origin2000/kona/"
                   "onewire-other-identity-parts-unknown-rejected",
                   test_onewire_other_identity_parts_unknown_rejected);
    qtest_add_func("sn0/origin2000/kona/"
                   "onewire-other-identity-parts-duplicate-rejected",
                   test_onewire_other_identity_parts_duplicate_rejected);
    qtest_add_func("sn0/origin2000/kona/"
                   "onewire-board-part-configures-geometry-record",
                   test_onewire_board_part_configures_geometry_record);
    qtest_add_func("sn0/origin2000/kona/board-part-invalid-is-rejected",
                   test_board_part_invalid_is_rejected);
    qtest_add_func("sn0/origin2000/kona/"
                   "nic-register-mirrors-onewire-register-state",
                   test_nic_register_mirrors_onewire_register_state);
    qtest_add_func("sn0/origin2000/kona/"
                   "nic-identity-matches-onewire-identity",
                   test_nic_identity_matches_onewire_identity);
    qtest_add_func("sn0/origin2000/kona/"
                   "nic-search-rom-walks-rack-and-deskside",
                   test_nic_search_rom_walks_rack_and_deskside);
    qtest_add_func("sn0/origin2000/kona/reset-clears-plain-storage",
                   test_reset_clears_plain_storage);
    qtest_add_func("sn0/origin2000/kona/reset-clears-increment-2-state",
                   test_reset_clears_increment2_state);
    qtest_add_func("sn0/origin2000/kona/credit-register-reads-fixed-zero",
                   test_credit_register_reads_fixed_zero);
    qtest_add_func("sn0/origin2000/kona/low-window-write-is-plain-storage",
                   test_low_window_write_is_plain_storage);
    qtest_add_func("sn0/origin2000/kona/"
                   "reset-and-eeprom-control-registers-are-plain-storage",
                   test_reset_and_eeprom_control_registers_are_plain_storage);
    qtest_add_func("sn0/origin2000/kona/"
                   "eeprom-data-answers-checked-address-and-zero-elsewhere",
                   test_eeprom_data_answers_checked_address_and_zero_elsewhere);
    qtest_add_func("sn0/origin2000/kona/textport-sync-probe-mailbox-protocol",
                   test_textport_sync_probe_mailbox_protocol);
    qtest_add_func("sn0/origin2000/kona/"
                   "textport-graphics-info-mailbox-protocol",
                   test_textport_graphics_info_mailbox_protocol);
    qtest_add_func("sn0/origin2000/kona/"
                   "textport-set-color-and-point-render-pixels",
                   test_textport_set_color_and_point_render_pixels);
    qtest_add_func("sn0/origin2000/kona/textport-screen-size-is-traced-only",
                   test_textport_screen_size_is_traced_only);
    qtest_add_func("sn0/origin2000/kona/textport-bitmap-blit-renders-payload",
                   test_textport_bitmap_blit_renders_payload);
    qtest_add_func("sn0/origin2000/kona/"
                   "textport-reserved-selectors-have-no-render-effect",
                   test_textport_reserved_selectors_have_no_render_effect);
    qtest_add_func("sn0/origin2000/kona/"
                   "textport-unknown-selector-resynchronizes",
                   test_textport_unknown_selector_resynchronizes);
    qtest_add_func("sn0/origin2000/kona/migration", test_migration);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
