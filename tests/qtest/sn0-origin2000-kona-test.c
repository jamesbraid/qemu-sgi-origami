/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA register-contract, one-wire identity and textport qtests.
 * These tests exercise the model's identity registers, textport commands,
 * and rendered pixels. They establish QEMU behavior only. This fixture
 * fits KONA to an origin2000 as a `-device` at the empty
 * XIO slot IO4, crossbow0's widget 11, outside the default topology's
 * startup-links-alive (BaseIO at 8, the two host-node Hubs at 9 and
 * 10). It takes a fixed `id=kona0`
 * so the increment-2 cases can reach its `x-test-pixel` QOM properties --
 * but unlike a plain `-device` that stays where device_add put it,
 * hw/mips/sgi/xio-board.c's own sgi_xio_board_reparent() moves every
 * sgi-xio-board subclass out from under /machine/peripheral and into a
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
    (SPACE_IO + (uint64_t)(widget) * SMALL_WINDOW_SIZE)

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
 * This model aliases the two offsets to one one-wire chain. The helpers take
 * the register address so the same identity transactions exercise either
 * offset without duplicating the protocol code.
 */
#define REG_NIC            KONA_REG(0x00009c)

/*
 * Increment 2: the register window and textport command port, offsets
 * per include/hw/misc/sgi-kona.h.
 */
#define REG_LOW_WINDOW_WRITE KONA_REG(0x00007c)
#define REG_CREDIT           KONA_REG(0x800208)
#define REG_CREDIT_CONFIG    KONA_REG(0x800210)
#define REG_DMA_FIFO_PUSH    KONA_REG(0x80007c)
#define REG_RESET_ARG        KONA_REG(0x800220)
#define REG_INT_ENABLE       KONA_REG(0x800230)
#define REG_INT_CAUSE        KONA_REG(0x800238)
#define REG_MAILBOX          KONA_REG(0x800240)
#define REG_RESET_STROBE     KONA_REG(0x800300)
#define REG_EEPROM_ENABLE    KONA_REG(0x800308)
#define REG_EEPROM_ADDR      KONA_REG(0x8003e8)
#define REG_EEPROM_DATA      KONA_REG(0x8003f0)
#define REG_TEXTPORT         KONA_REG(0xc10000)

/*
 * The XG map RAM window (established). KONA_MAPRAM_ENTRIES
 * matches SGI_KONA_MAPRAM_ENTRIES in include/hw/misc/sgi-kona.h: the
 * model's own +0x64 size code stays at 0, which the driver's own
 * N<<15 with N=0 meaning 8 resolves to 262,144 entries.
 */
#define REG_MAPRAM_BASE      KONA_REG(0x100000)
#define KONA_MAPRAM_ENTRIES  0x40000
#define MAPRAM_ENTRY(i)      (REG_MAPRAM_BASE + 4 * (uint64_t)(i))

#define KONA_INT_CAUSE_MAILBOX (1u << 4)
#define KONA_EEPROM_CHECKED_ADDR  0x87ff1c
#define KONA_EEPROM_CHECKED_VALUE 2

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4"

/*
 * origin2000's default topology is the full four-node/multi-CPU module
 * sn-catalogue.json describes, with a matching multi-gigabyte default RAM
 * size -- fine for every other case here, which shares one already-running
 * instance for plain register I/O, but too large to migrate twice (once
 * per instance) inside a single qtest's time budget. Pinned down the same
 * way sn0-origin2000-mgras-test.c's own MIGRATION_ARGS does, for the same
 * machine type and the same reason.
 */
#define MIGRATION_ARGS MACHINE_ARGS

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
 * `*last_discrepancy` into the next, starting from 0 and 0. The same helper
 * exercises either modeled register offset without duplicating the protocol
 * code. The test checks the identity set returned by each configured model.
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
 * Field offsets for the modeled fixed-offset board-identity record.
 * sgi_kona_onewire_attach_part() shares sgi_bridge_onewire_build_record()
 * rather than building its own record.
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
    /* Nothing is held after reset, so both readers proceed. */
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT), ==, 0);
}

/*
 * The credit register counts the port words the model holds, in bits
 * 10:1 (mask 0x7fe), and returns them when the packet is consumed: a
 * colour-map load with its selector and two of four words in reads 6,
 * zero once complete; a kernel 0x1f1 with its token and one of three
 * words reads 4; a DMA packet with its selector and three of six words
 * on the push register reads 8. Each drops to zero, the value both the
 * PROM's and the driver's waits proceed on, once its last word arrives.
 * Bit 0 is never set. A model that read a fixed zero fails the first
 * mid-packet read.
 */
static void test_credit_follows_consumption(void)
{
    kona_reset();

    qtest_writel(qts, REG_TEXTPORT, 1);
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT), ==, 0);
    qtest_writel(qts, REG_TEXTPORT, 0x3);
    qtest_writel(qts, REG_TEXTPORT, 7);
    qtest_writel(qts, REG_TEXTPORT, 0x40);
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT), ==, 3 << 1);
    qtest_writel(qts, REG_TEXTPORT, 0x80);
    qtest_writel(qts, REG_TEXTPORT, 0xc0);
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT), ==, 0);

    qtest_writel(qts, REG_TEXTPORT, 0x1f1);
    qtest_writel(qts, REG_TEXTPORT, 0x10000000);
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT), ==, 2 << 1);
    qtest_writel(qts, REG_TEXTPORT, 0x9000);
    qtest_writel(qts, REG_TEXTPORT, 0x4000);
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT), ==, 0);

    qtest_writel(qts, REG_DMA_FIFO_PUSH, 0x1eb);
    qtest_writel(qts, REG_DMA_FIFO_PUSH, 0x9);
    qtest_writel(qts, REG_DMA_FIFO_PUSH, 0x9004);
    qtest_writel(qts, REG_DMA_FIFO_PUSH, 4);
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT), ==, 4 << 1);
    qtest_writel(qts, REG_DMA_FIFO_PUSH, 4);
    qtest_writel(qts, REG_DMA_FIFO_PUSH, 0);
    qtest_writel(qts, REG_DMA_FIFO_PUSH, 0);
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT), ==, 0);
}

/*
 * The driver's one write of 0x700 to +0x800210 is kept and read back and
 * changes nothing the credit register says: zero at rest, and the same
 * held count mid-packet as before the write. Before, the offset fell to
 * the unknown-register path and read zero.
 */
static void test_credit_config_stored(void)
{
    kona_reset();
    qtest_writel(qts, REG_CREDIT_CONFIG, 0x700);
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT_CONFIG), ==, 0x700);
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT), ==, 0);
    qtest_writel(qts, REG_TEXTPORT, 1);
    qtest_writel(qts, REG_TEXTPORT, 0x9);
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT), ==, 1 << 1);
    qtest_writel(qts, REG_TEXTPORT, 0);
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT) & ~0x7feu, ==, 0);
    kona_reset();
    g_assert_cmphex(qtest_readl(qts, REG_CREDIT_CONFIG), ==, 0);
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

/*
 * A fresh instance logging the -d categories in `debug` to its own file,
 * for a case that has to show which "sgi-kona: unimplemented" lines an
 * access raised. `extra_props` is appended to the sgi-kona device's own
 * properties. The caller quits the instance and reads the log with
 * kona_unimp_log_read(), which also unlinks the file.
 */
static QTestState *kona_init_with_unimp_log(const char *extra_props,
                                            const char *debug, char **log)
{
    int fd;

    *log = g_strdup_printf("%s/sn0-kona-unimp-XXXXXX.log", g_get_tmp_dir());
    fd = g_mkstemp(*log);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    return qtest_initf(MACHINE_ARGS "%s -d %s -D %s",
                       extra_props, debug, *log);
}

static char *kona_unimp_log_read(const char *log)
{
    char *found = NULL;

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_cmpint(g_unlink(log), ==, 0);
    return found;
}

static void kona_assert_offset_not_unimplemented(const char *found,
                                                 uint32_t offset)
{
    g_autofree char *read = g_strdup_printf(
        "sgi-kona: unimplemented read of offset 0x%" PRIx32 "\n", offset);
    g_autofree char *write = g_strdup_printf(
        "sgi-kona: unimplemented write of offset 0x%" PRIx32 " ", offset);

    g_assert_null(strstr(found, read));
    g_assert_null(strstr(found, write));
}

/*
 * The kernel driver's own KONAinitialize, reached through the kernel
 * operations table ahead of kona_mapram_init(): strobes REG_RESET_STROBE
 * (write 1, read, write 0, read), then writes 0x100 to REG_INT_ENABLE and
 * 0x8000 to REG_RESET_ARG. Each write must be accepted without an
 * unimplemented-offset log.
 *
 * The readbacks alone cannot show that: REG_INT_ENABLE used to fall to
 * the unimplemented-offset default, which also reads zero, and the other
 * two registers already echoed their writes. So this runs its own
 * instance with -d unimp and checks the log names none of the three
 * offsets. One deliberately unimplemented read at the end proves the log
 * would have caught one.
 */
static void test_konainitialize_register_sequence_is_accepted(void)
{
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    QTestState *s = kona_init_with_unimp_log("", "unimp", &log);

    qtest_writel(s, REG_RESET_STROBE, 1);
    g_assert_cmphex(qtest_readl(s, REG_RESET_STROBE), ==, 1);
    qtest_writel(s, REG_RESET_STROBE, 0);
    g_assert_cmphex(qtest_readl(s, REG_RESET_STROBE), ==, 0);

    qtest_writel(s, REG_INT_ENABLE, 0x100);
    g_assert_cmphex(qtest_readl(s, REG_INT_ENABLE), ==, 0);

    qtest_writel(s, REG_RESET_ARG, 0x8000);
    g_assert_cmphex(qtest_readl(s, REG_RESET_ARG), ==, 0x8000);

    qtest_readl(s, KONA_REG(0x300000));
    qtest_quit(s);

    found = kona_unimp_log_read(log);
    g_assert_nonnull(strstr(found,
                            "sgi-kona: unimplemented read of offset "
                            "0x300000\n"));
    kona_assert_offset_not_unimplemented(found, 0x800300);
    kona_assert_offset_not_unimplemented(found, 0x800230);
    kona_assert_offset_not_unimplemented(found, 0x800220);
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
 * Load colormap (0x3), select colour (0x2), then point (0x6): the chain a
 * real captured boot trace exercises before every glyph stroke. Command 2
 * selects a colormap index, not a packed colour -- established from that
 * trace, whose command-2 argument values run 0-255, the identical domain
 * command 3's own index argument uses. Command 3 stores index, then
 * R/G/B each pre-scaled <<2 by the sender. Each point uses a coordinate
 * no earlier case in this file touches.
 */
static void test_textport_colormap_and_point_render_pixels(void)
{
    kona_reset();

    kona_textport_push(qts, 0x3, (uint32_t[]){ 1, 0xff << 2, 0, 0 }, 4);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 1 }, 1);
    kona_textport_push(qts, 0x6, (uint32_t[]){ 100, 50 }, 2);
    g_assert_cmphex(kona_test_pixel(qts, 100, 50), ==, 0x00ff0000);

    kona_textport_push(qts, 0x3, (uint32_t[]){ 2, 0, 0xff << 2, 0 }, 4);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 2 }, 1);
    kona_textport_push(qts, 0x6, (uint32_t[]){ 200, 150 }, 2);
    g_assert_cmphex(kona_test_pixel(qts, 200, 150), ==, 0x0000ff00);
    /* The first point's own color is untouched by the second. */
    g_assert_cmphex(kona_test_pixel(qts, 100, 50), ==, 0x00ff0000);
}

/*
 * Filled rectangle (0x5): inclusive on every side -- see
 * sgi_kona_fill_rect()'s own comment for the sender-code and
 * captured-trace evidence. A rectangle from (10,20) to (12,22) covers
 * exactly a 3x3 pixel block; one step outside each edge stays untouched.
 */
static void test_textport_fill_rect_renders_inclusive_rectangle(void)
{
    kona_reset();

    kona_textport_push(qts, 0x3,
                       (uint32_t[]){ 1, 0x1f << 2, 0x2f << 2, 0x3f << 2 }, 4);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 1 }, 1);
    kona_textport_push(qts, 0x5, (uint32_t[]){ 10, 20, 12, 22 }, 4);

    g_assert_cmphex(kona_test_pixel(qts, 10, 20), ==, 0x001f2f3f);
    g_assert_cmphex(kona_test_pixel(qts, 12, 20), ==, 0x001f2f3f);
    g_assert_cmphex(kona_test_pixel(qts, 10, 22), ==, 0x001f2f3f);
    g_assert_cmphex(kona_test_pixel(qts, 12, 22), ==, 0x001f2f3f);
    g_assert_cmphex(kona_test_pixel(qts, 11, 21), ==, 0x001f2f3f);

    g_assert_cmphex(kona_test_pixel(qts, 9, 20), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 13, 20), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 10, 19), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 10, 23), ==, 0);
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
 * Bitmap/glyph blit (0xa): six header words -- width, height, x origin,
 * y origin, x advance, y advance -- then a packed 1-bpp payload, one bit
 * per pixel, MSB-first, row-major -- hw/misc/sgi-kona.c's own
 * sgi_kona_bitmap_blit(). All six header fields are established (see
 * that function's own comment, from the IO6 PROM's kona_tp_bitmap).
 * Placement is
 * relative to the textport's own raster position (command 4), not
 * absolute: pixel (c, r) of the payload lands at
 * (raster_x - xorigin + c, raster_y - yorigin + r), and a completed
 * blit advances the raster position by (xadvance, yadvance) -- the
 * "position once, then a run of blits that each advance it" shape
 * gui_draw_string's own entry documents for a text label. A 4x2 bitmap
 * needs one payload word; only its top byte (0xa5 = 1010 0101) is
 * meaningful; second row is (0,1,0,1).
 */
static void test_textport_bitmap_blit_renders_payload(void)
{
    kona_reset();

    kona_textport_push(qts, 0x3,
                       (uint32_t[]){ 1, 0xab << 2, 0xcd << 2, 0xef << 2 }, 4);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 1 }, 1);
    kona_textport_push(qts, 0x4, (uint32_t[]){ 300, 400 }, 2);
    kona_textport_push(qts, 0xa, (uint32_t[]){ 4, 2, 0, 0, 5, 0 }, 6);
    qtest_writel(qts, REG_TEXTPORT, 0xa5000000);

    g_assert_cmphex(kona_test_pixel(qts, 300, 400), ==, 0x00abcdef);
    g_assert_cmphex(kona_test_pixel(qts, 301, 400), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 302, 400), ==, 0x00abcdef);
    g_assert_cmphex(kona_test_pixel(qts, 303, 400), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 300, 401), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 301, 401), ==, 0x00abcdef);
    g_assert_cmphex(kona_test_pixel(qts, 302, 401), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 303, 401), ==, 0x00abcdef);

    /*
     * That blit's own xadvance (5) moved the raster position to
     * (305, 400) with no further selector-4 push: a second blit lands
     * relative to the advanced position, matching how a text label's
     * glyphs chain from one raster-position push.
     */
    kona_textport_push(qts, 0xa, (uint32_t[]){ 4, 2, 0, 0, 0, 0 }, 6);
    qtest_writel(qts, REG_TEXTPORT, 0xa5000000);

    g_assert_cmphex(kona_test_pixel(qts, 305, 400), ==, 0x00abcdef);
    g_assert_cmphex(kona_test_pixel(qts, 307, 400), ==, 0x00abcdef);
    g_assert_cmphex(kona_test_pixel(qts, 306, 401), ==, 0x00abcdef);
    g_assert_cmphex(kona_test_pixel(qts, 308, 401), ==, 0x00abcdef);
}

/*
 * A command argument value equal to SGI_KONA_BITMAP_SELECTOR (0xa) must
 * not be misread as a fresh bitmap-blit start when it falls mid-packet:
 * sgi_kona_textport_push() only intercepts 0xa between packets, when the
 * generic framer is not already framing one. Found by tracing a real
 * boot, where command 3's own r/g/b arguments routinely carry the value
 * 0xa. Proven the same way as the reserved-selector case below: a probe
 * completing correctly right after shows the port stayed synchronized
 * instead of getting stuck waiting for a bitmap header that never comes.
 */
static void test_textport_bitmap_value_mid_packet_not_misframed(void)
{
    uint32_t reply;
    bool more_pending;

    kona_reset();

    kona_textport_push(qts, 0x3, (uint32_t[]){ 1, 0xa, 0xa, 0xa }, 4);

    kona_textport_push(qts, 0x9, (uint32_t[]){ 0 }, 1);
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) & KONA_INT_CAUSE_MAILBOX,
                    ==, KONA_INT_CAUSE_MAILBOX);
    reply = kona_mailbox_drain_one(qts, &more_pending);
    g_assert_cmphex(reply, ==, 0x1234abcd);
    g_assert_false(more_pending);
}

/*
 * Selector 4 stores the textport's raster position for a following
 * bitmap blit and draws nothing itself; the drawing side is covered by
 * test_textport_bitmap_blit_renders_payload()'s advance case above.
 * Selectors 1 and 7 have a known argument shape but no known rendering,
 * blanking or cursor role, and which command number, if any, the
 * driver's unblank routine sends is unknown, so neither may be assumed
 * to carry it. This test proves all three draw no pixel of their own,
 * including selector 4 at a raster position distinct from the reset
 * default. Selectors 2, 3, 5 and 6 (colour select, colour map load,
 * filled rectangle, point) have their own rendering tests.
 */
static void test_textport_reserved_selectors_have_no_render_effect(void)
{
    static const uint32_t args1[1] = { 0 };
    static const uint32_t args2[2] = { 0, 0 };
    static const uint32_t raster_args[2] = { 500, 500 };

    kona_reset();

    g_assert_cmphex(kona_test_pixel(qts, 900, 900), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 500, 500), ==, 0);

    kona_textport_push(qts, 0x1, args1, 1);
    kona_textport_push(qts, 0x4, raster_args, 2);
    kona_textport_push(qts, 0x7, args2, 2);

    g_assert_cmphex(kona_test_pixel(qts, 900, 900), ==, 0);
    g_assert_cmphex(kona_test_pixel(qts, 500, 500), ==, 0);

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
    g_assert_cmphex(qtest_readl(qts, KONA_REG(0x300000)), ==, 0);

    /*
     * Compare against whatever WIDGET_CONTROL reads back after
     * kona_reset(), not a hardcoded value: this checks that an unmapped
     * write does not disturb it, independent of what value reset itself
     * leaves there (test_reset_clears_plain_storage() checks that
     * separately).
     */
    control_before = qtest_readl(qts, REG_WIDGET_CONTROL);
    qtest_writel(qts, KONA_REG(0x300000), 0x12345678);
    g_assert_cmphex(qtest_readl(qts, KONA_REG(0x300000)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, control_before);
}

/*
 * The XG map RAM: kona_mapram_init()'s own per-entry probe -- write
 * 0x10000000|i, read back, expect an exact match -- run over a
 * representative sample plus the table's own first and last entry
 * (test_mapram_full_window_sweep() below replays this over every
 * entry). Each sampled entry is written and checked before its
 * neighbour is touched, and entry 1 is re-checked at the end, so a
 * stride bug that only shows up once two entries alias the same word
 * cannot hide behind whichever entry was written most recently.
 */
static void test_mapram_stride_sample(void)
{
    static const uint32_t sample[] = { 0, 1, 0x7fff, 0x8000, 0x3ffff };
    unsigned int i;

    kona_reset();
    for (i = 0; i < G_N_ELEMENTS(sample); i++) {
        uint32_t index = sample[i];

        qtest_writel(qts, MAPRAM_ENTRY(index), 0x10000000 | index);
        g_assert_cmphex(qtest_readl(qts, MAPRAM_ENTRY(index)), ==,
                        0x10000000 | index);
    }

    g_assert_cmphex(qtest_readl(qts, MAPRAM_ENTRY(1)), ==, 0x10000001);
}

/*
 * Every one of the 32 bits an entry holds round-trips, not only the
 * low bits kona_mapram_init()'s own 0x10000000|i pattern exercises:
 * kona_mapram_map()'s own ordinary-mapping encoding (established) sets
 * bit 28 in neither operand, so a second write with it set, plus
 * every other bit up to bit 27, proves the model stores the whole word
 * rather than truncating to whatever subset the init loop alone needs.
 */
static void test_mapram_stores_full_32_bits(void)
{
    kona_reset();

    qtest_writel(qts, MAPRAM_ENTRY(0x3ffff), 0x10000000 | 0x3ffff);
    g_assert_cmphex(qtest_readl(qts, MAPRAM_ENTRY(0x3ffff)), ==,
                    0x10000000 | 0x3ffff);

    qtest_writel(qts, MAPRAM_ENTRY(0), 0x20000000 | 0x0fffffff);
    g_assert_cmphex(qtest_readl(qts, MAPRAM_ENTRY(0)), ==,
                    0x20000000 | 0x0fffffff);
}

/*
 * The window's own bounds: the last offset below it and the first
 * offset past it both fall to the unimplemented-offset default case
 * (test_unknown_offset_reads_zero_and_accepts_writes()'s own
 * discriminator -- a write does not persist), while the table's own
 * last entry, right at the upper edge, takes the write like any other
 * entry.
 */
static void test_mapram_bounds_fall_through_to_unimplemented(void)
{
    kona_reset();

    g_assert_cmphex(qtest_readl(qts, KONA_REG(0xffffc)), ==, 0);
    qtest_writel(qts, KONA_REG(0xffffc), 0x12345678);
    g_assert_cmphex(qtest_readl(qts, KONA_REG(0xffffc)), ==, 0);

    g_assert_cmphex(qtest_readl(qts, KONA_REG(0x200000)), ==, 0);
    qtest_writel(qts, KONA_REG(0x200000), 0x12345678);
    g_assert_cmphex(qtest_readl(qts, KONA_REG(0x200000)), ==, 0);

    qtest_writel(qts, MAPRAM_ENTRY(KONA_MAPRAM_ENTRIES - 1), 0x12345678);
    g_assert_cmphex(qtest_readl(qts, MAPRAM_ENTRY(KONA_MAPRAM_ENTRIES - 1)),
                    ==, 0x12345678);
}

static void test_mapram_reset_clears_table(void)
{
    kona_reset();

    qtest_writel(qts, MAPRAM_ENTRY(0), 0x10000000);
    qtest_writel(qts, MAPRAM_ENTRY(0x3ffff), 0x1003ffff);
    g_assert_cmphex(qtest_readl(qts, MAPRAM_ENTRY(0)), !=, 0);
    g_assert_cmphex(qtest_readl(qts, MAPRAM_ENTRY(0x3ffff)), !=, 0);

    qtest_system_reset(qts);

    g_assert_cmphex(qtest_readl(qts, MAPRAM_ENTRY(0)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MAPRAM_ENTRY(0x3ffff)), ==, 0);
}

/*
 * A nonzero size code in SGI_KONA_PROBE's bits [5:3] shrinks the active
 * table: N = 1 selects 1 << 15 entries, where the default N = 0 selects
 * the whole window (the driver's own N << 15 with 0 meaning 8). The last
 * entry below the count stores like any other; the first entry at
 * the count, though still inside the physical window, falls to the
 * unimplemented-offset path: it reads zero, keeps no write, and both
 * accesses leave a beyond-count trace and a LOG_UNIMP line.
 */
static void test_mapram_size_code_limits_entry_count(void)
{
    const uint32_t count = 1 << 15;
    g_autofree char *log = NULL;
    g_autofree char *found = NULL;
    g_autofree char *beyond = NULL;
    QTestState *s = kona_init_with_unimp_log(
        ",probe-value=0x8", "unimp,trace:sgi_kona_mapram_beyond_count", &log);

    g_assert_cmphex(qtest_readl(s, REG_PROBE), ==, 0x8);

    qtest_writel(s, MAPRAM_ENTRY(count - 1), 0x10000000 | (count - 1));
    g_assert_cmphex(qtest_readl(s, MAPRAM_ENTRY(count - 1)), ==,
                    0x10000000 | (count - 1));

    qtest_writel(s, MAPRAM_ENTRY(count), 0x10000000 | count);
    g_assert_cmphex(qtest_readl(s, MAPRAM_ENTRY(count)), ==, 0);
    qtest_quit(s);

    found = kona_unimp_log_read(log);
    beyond = g_strdup_printf("index %u count %u", count, count);
    g_assert_nonnull(strstr(found, beyond));
    g_assert_nonnull(strstr(found,
                            "sgi-kona: unimplemented write of offset "
                            "0x120000 "));
    g_assert_nonnull(strstr(found,
                            "sgi-kona: unimplemented read of offset "
                            "0x120000\n"));
    kona_assert_offset_not_unimplemented(found, 0x11fffc);
}

/*
 * kona_mapram_map()'s own ordinary-mapping encoding: 0x20000000 |
 * ((paddr >> 14) << 2), with a page-size code in bits [30:29]
 * (established formula, inferred field names). The model does
 * not interpret this value; it is plain storage exactly like the init
 * loop's own pattern, so this only proves the driver's real
 * construction round-trips, rather than an arbitrary bit pattern.
 */
static void test_mapram_ordinary_mapping_write_read_back(void)
{
    uint64_t paddr = 0x40028000;
    uint32_t value = 0x20000000 | ((uint32_t)(paddr >> 14) << 2);

    kona_reset();
    qtest_writel(qts, MAPRAM_ENTRY(5), value);
    g_assert_cmphex(qtest_readl(qts, MAPRAM_ENTRY(5)), ==, value);
}

/*
 * kona_mapram_init()'s own attach-time probe, replayed over the whole
 * table instead of a sample: write 0x10000000|i to every entry, then
 * read every entry back and expect an exact match throughout -- the
 * actual discriminator the driver's init loop applies before it ever
 * logs "IR: Bad XG MAPRAM entry found during kona_mapram_init!"
 * (established).
 *
 * A plain qtest_writel()/qtest_readl() loop over all
 * SGI_KONA_MAPRAM_ENTRIES entries measured around 8.7s (one
 * control-socket round trip per access, 2 accesses per entry) --
 * further from "a few seconds" than this suite's other cases run.
 * qtest_memwrite()/qtest_memread() instead build and check the whole
 * table in two round trips regardless of size, measured here at well
 * under a second, at the cost of handling the device's own big-endian
 * byte order by hand: confirmed empirically (with a throwaway
 * instrumented case, not by reasoning about qtest's internals alone)
 * that these calls copy the raw byte buffer with no endian conversion
 * of their own, unlike qtest_writel()/qtest_readl()'s own per-word
 * handling -- so GUINT32_TO_BE()/GUINT32_FROM_BE() do that conversion
 * on the host side here instead.
 */
static void test_mapram_full_window_sweep(void)
{
    uint32_t *wbuf = g_new(uint32_t, KONA_MAPRAM_ENTRIES);
    uint32_t *rbuf = g_new(uint32_t, KONA_MAPRAM_ENTRIES);
    uint32_t i;

    for (i = 0; i < KONA_MAPRAM_ENTRIES; i++) {
        wbuf[i] = GUINT32_TO_BE(0x10000000 | i);
    }

    kona_reset();
    qtest_memwrite(qts, REG_MAPRAM_BASE, wbuf,
                   KONA_MAPRAM_ENTRIES * sizeof(uint32_t));
    qtest_memread(qts, REG_MAPRAM_BASE, rbuf,
                  KONA_MAPRAM_ENTRIES * sizeof(uint32_t));

    for (i = 0; i < KONA_MAPRAM_ENTRIES; i++) {
        g_assert_cmphex(GUINT32_FROM_BE(rbuf[i]), ==, 0x10000000 | i);
    }

    g_free(wbuf);
    g_free(rbuf);
}

/*
 * The framebuffer pixel array's own migration test is the closest precedent
 * for a large migrated array: draw through the textport, migrate, read back
 * on the destination (test_migration() above). The map RAM table gets the
 * same treatment here -- two written entries plus one left untouched, so the
 * destination is checked against a real value in one case and against
 * migrated zero, not stale host memory, in the other.
 */
static void test_mapram_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-kona-mapram-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    qtest_writel(from, MAPRAM_ENTRY(0), 0x10000000);
    qtest_writel(from, MAPRAM_ENTRY(0x3ffff), 0x1003ffff);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readl(to, MAPRAM_ENTRY(0)), ==, 0x10000000);
    g_assert_cmphex(qtest_readl(to, MAPRAM_ENTRY(0x3ffff)), ==, 0x1003ffff);
    g_assert_cmphex(qtest_readl(to, MAPRAM_ENTRY(1)), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
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
 * kona_attach()'s call into nic_xg_vertex_info() and on into nic.o's
 * generic one-wire NIC driver -- not a second,
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
 * The default fixture returns both configured identity records. A fixture
 * with an empty "other-identity-parts" list returns only its own record.
 * This test checks the returned set without asserting discovery order.
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
 * Mutated widget control and a one-wire transaction left mid-slot both
 * return to zero on system reset. The reset pulse writes nonzero control
 * bits, so the register reads nonzero here even if no part answers. REG_NIC
 * exposes the same model state as REG_ONEWIRE.
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
 * Dirty each state added by the Kona device, reset, then probe it. The
 * textport's generic-framer state and bitmap mid-payload state need
 * separate phases: once selector 0xa engages the bitmap interceptor, its
 * words no longer reach the generic framer.
 *
 * Phase 1 dirties the generic framer with selector 0x3 (argc 4),
 * deliberately not the verification probe's own argc (selector 0x9,
 * argc 1): a framer left stuck mid-packet would otherwise misconsume the
 * probe's own words at the same argc and coincidentally complete with the
 * same net effect, passing even though reset never ran. With mismatched
 * argc, a stuck framer instead misaligns word consumption and the probe
 * genuinely fails to complete.
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

    kona_textport_push(qts, 0x3,
                       (uint32_t[]){ 1, 0xc0 << 2, 0xff << 2, 0xee << 2 }, 4);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 1 }, 1);
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
    qtest_writel(qts, REG_TEXTPORT, 8);
    qtest_writel(qts, REG_TEXTPORT, 8);
    qtest_writel(qts, REG_TEXTPORT, 500);
    qtest_writel(qts, REG_TEXTPORT, 500);
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

    /*
     * Phase 3: the raster position itself (command 4). Dirty it to a
     * value distinct from its documented reset default and prove it
     * live with a discriminating draw (a 1x1 bitmap with zero origin
     * lands exactly at the raster position) before reset, then prove it
     * cleared with the same draw after -- landing at the origin only if
     * raster_x/raster_y themselves went back to zero, not merely if the
     * bitmap interceptor's own framing state (already proven above) did.
     */
    kona_reset();

    kona_textport_push(qts, 0x3,
                       (uint32_t[]){ 1, 0xa0 << 2, 0xb0 << 2, 0xc0 << 2 }, 4);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 1 }, 1);
    kona_textport_push(qts, 0x4, (uint32_t[]){ 200, 200 }, 2);
    kona_textport_push(qts, 0xa, (uint32_t[]){ 1, 1, 0, 0, 0, 0 }, 6);
    qtest_writel(qts, REG_TEXTPORT, 0x80000000);
    g_assert_cmphex(kona_test_pixel(qts, 200, 200), ==, 0x00a0b0c0);

    qtest_system_reset(qts);
    g_assert_cmphex(kona_test_pixel(qts, 200, 200), ==, 0);

    kona_textport_push(qts, 0x3,
                       (uint32_t[]){ 1, 0xa0 << 2, 0xb0 << 2, 0xc0 << 2 }, 4);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 1 }, 1);
    kona_textport_push(qts, 0xa, (uint32_t[]){ 1, 1, 0, 0, 0, 0 }, 6);
    qtest_writel(qts, REG_TEXTPORT, 0x80000000);
    g_assert_cmphex(kona_test_pixel(qts, 0, 0), ==, 0x00a0b0c0);
    g_assert_cmphex(kona_test_pixel(qts, 200, 200), ==, 0);
}

/*
 * Draw a pixel through the textport, migrate to a second instance, and
 * read the pixel on the destination. This checks that migration carries
 * the framebuffer array as well as the separately tested registers and
 * framing state.
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

    kona_textport_push(from, 0x3,
                       (uint32_t[]){ 1, 0xc0 << 2, 0xff << 2, 0xee << 2 }, 4);
    kona_textport_push(from, 0x2, (uint32_t[]){ 1 }, 1);
    kona_textport_push(from, 0x6, (uint32_t[]){ 640, 480 }, 2);
    g_assert_cmphex(kona_test_pixel(from, 640, 480), ==, 0x00c0ffee);

    /*
     * The raster position itself (command 4) must cross the migration
     * boundary too, not only the pixel array and colour map above: draw
     * one 1x1 bitmap on the source that leaves the raster position
     * advanced by its own header's xadvance, then migrate. A second 1x1
     * bitmap on the destination, with no further selector-4 push, lands
     * at the advanced position only if raster_x/raster_y themselves
     * migrated.
     */
    kona_textport_push(from, 0x4, (uint32_t[]){ 300, 200 }, 2);
    kona_textport_push(from, 0xa, (uint32_t[]){ 1, 1, 0, 0, 5, 0 }, 6);
    qtest_writel(from, REG_TEXTPORT, 0x80000000);
    g_assert_cmphex(kona_test_pixel(from, 300, 200), ==, 0x00c0ffee);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(kona_test_pixel(to, 640, 480), ==, 0x00c0ffee);
    /* An untouched coordinate migrates as zero, not stale host memory. */
    g_assert_cmphex(kona_test_pixel(to, 641, 480), ==, 0);
    g_assert_cmphex(kona_test_pixel(to, 300, 200), ==, 0x00c0ffee);

    kona_textport_push(to, 0xa, (uint32_t[]){ 1, 1, 0, 0, 0, 0 }, 6);
    qtest_writel(to, REG_TEXTPORT, 0x80000000);
    g_assert_cmphex(kona_test_pixel(to, 305, 200), ==, 0x00c0ffee);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * A v1 source predates raster_x/raster_y entirely: both the fields and
 * the command-4 handler that sets them arrived in this same series (see
 * "hw/misc/sgi-kona: place bitmap blits relative to the raster
 * position"), while vmstate_sgi_kona has carried version_id 1 since the
 * device's introduction. A v1 stream therefore has no bytes for either
 * field. Confirms the destination's post_load leaves them at
 * sgi_kona_reset_hold()'s own (0, 0) default -- matching what a v1
 * source's absence of the concept always implied -- rather than reading
 * stray bytes from whatever vmstate_sgi_kona placed next in a v1 stream,
 * which would misplace every bitmap blit issued after migrating an old
 * guest onto this code.
 *
 * Skipped unless a v1 reference binary is available; build one from
 * commit 12a129769c (this series' immediate parent, the last commit
 * before raster_x/raster_y existed anywhere in the device) and point
 * QTEST_QEMU_BINARY_KONA_V1 at its qemu-system-mips64. See
 * sn0-migration-test.c's QTEST_QEMU_BINARY_IOC3_V15 and
 * sn0-origin2000-mgras-test.c's QTEST_QEMU_BINARY_MGRAS_V9 for the same
 * pattern.
 */
static void test_migration_from_v1_no_raster_position(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    if (!g_getenv("QTEST_QEMU_BINARY_KONA_V1")) {
        g_test_skip("QTEST_QEMU_BINARY_KONA_V1 is not set");
        return;
    }

    tmpfs = g_dir_make_tmp("sn0-kona-v1-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init_ext("QTEST_QEMU_BINARY_KONA_V1", MIGRATION_ARGS,
                          NULL, true);
    kona_textport_push(from, 0x3,
                       (uint32_t[]){ 1, 0xc0 << 2, 0xff << 2, 0xee << 2 }, 4);
    kona_textport_push(from, 0x2, (uint32_t[]){ 1 }, 1);
    kona_textport_push(from, 0x6, (uint32_t[]){ 640, 480 }, 2);
    g_assert_cmphex(kona_test_pixel(from, 640, 480), ==, 0x00c0ffee);

    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(kona_test_pixel(to, 640, 480), ==, 0x00c0ffee);

    /*
     * No selector-4 raster-position-set was ever issued on `to`: this
     * draws with whatever raster_x/raster_y the migration itself left
     * behind. Landing at (0, 0) proves post_load left the gated fields
     * at their reset default rather than misreading a v1 stream that
     * never had them.
     */
    kona_textport_push(to, 0xa, (uint32_t[]){ 1, 1, 0, 0, 0, 0 }, 6);
    qtest_writel(to, REG_TEXTPORT, 0x80000000);
    g_assert_cmphex(kona_test_pixel(to, 0, 0), ==, 0x00c0ffee);

    /*
     * A v1 stream predates the XG map RAM table even more thoroughly
     * than raster_x/raster_y (gated at version 2): map_ram is gated at
     * version 3, so a v1 stream has no bytes for it either. Confirms
     * post_load leaves the table at sgi_kona_reset_hold()'s own
     * all-zero default rather than misparsing stray v1 bytes as table
     * contents.
     */
    g_assert_cmphex(qtest_readl(to, MAPRAM_ENTRY(0)), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
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
    qtest_add_func("sn0/origin2000/kona/credit-follows-consumption",
                   test_credit_follows_consumption);
    qtest_add_func("sn0/origin2000/kona/credit-config-stored",
                   test_credit_config_stored);
    qtest_add_func("sn0/origin2000/kona/low-window-write-is-plain-storage",
                   test_low_window_write_is_plain_storage);
    qtest_add_func("sn0/origin2000/kona/"
                   "reset-and-eeprom-control-registers-are-plain-storage",
                   test_reset_and_eeprom_control_registers_are_plain_storage);
    qtest_add_func("sn0/origin2000/kona/"
                   "konainitialize-register-sequence-is-accepted",
                   test_konainitialize_register_sequence_is_accepted);
    qtest_add_func("sn0/origin2000/kona/"
                   "eeprom-data-answers-checked-address-and-zero-elsewhere",
                   test_eeprom_data_answers_checked_address_and_zero_elsewhere);
    qtest_add_func("sn0/origin2000/kona/textport-sync-probe-mailbox-protocol",
                   test_textport_sync_probe_mailbox_protocol);
    qtest_add_func("sn0/origin2000/kona/"
                   "textport-graphics-info-mailbox-protocol",
                   test_textport_graphics_info_mailbox_protocol);
    qtest_add_func("sn0/origin2000/kona/"
                   "textport-colormap-and-point-render-pixels",
                   test_textport_colormap_and_point_render_pixels);
    qtest_add_func("sn0/origin2000/kona/"
                   "textport-fill-rect-renders-inclusive-rectangle",
                   test_textport_fill_rect_renders_inclusive_rectangle);
    qtest_add_func("sn0/origin2000/kona/textport-screen-size-is-traced-only",
                   test_textport_screen_size_is_traced_only);
    qtest_add_func("sn0/origin2000/kona/textport-bitmap-blit-renders-payload",
                   test_textport_bitmap_blit_renders_payload);
    qtest_add_func("sn0/origin2000/kona/"
                   "textport-bitmap-selector-value-mid-packet-is-not-misframed",
                   test_textport_bitmap_value_mid_packet_not_misframed);
    qtest_add_func("sn0/origin2000/kona/"
                   "textport-reserved-selectors-have-no-render-effect",
                   test_textport_reserved_selectors_have_no_render_effect);
    qtest_add_func("sn0/origin2000/kona/"
                   "textport-unknown-selector-resynchronizes",
                   test_textport_unknown_selector_resynchronizes);
    qtest_add_func("sn0/origin2000/kona/migration", test_migration);
    qtest_add_func("sn0/origin2000/kona/migration-from-v1-no-raster-position",
                   test_migration_from_v1_no_raster_position);
    qtest_add_func("sn0/origin2000/kona/mapram-stride-sample",
                   test_mapram_stride_sample);
    qtest_add_func("sn0/origin2000/kona/mapram-stores-full-32-bits",
                   test_mapram_stores_full_32_bits);
    qtest_add_func("sn0/origin2000/kona/"
                   "mapram-bounds-fall-through-to-unimplemented",
                   test_mapram_bounds_fall_through_to_unimplemented);
    qtest_add_func("sn0/origin2000/kona/"
                   "mapram-size-code-limits-entry-count",
                   test_mapram_size_code_limits_entry_count);
    qtest_add_func("sn0/origin2000/kona/mapram-reset-clears-table",
                   test_mapram_reset_clears_table);
    qtest_add_func("sn0/origin2000/kona/"
                   "mapram-ordinary-mapping-write-read-back",
                   test_mapram_ordinary_mapping_write_read_back);
    qtest_add_func("sn0/origin2000/kona/mapram-full-window-sweep",
                   test_mapram_full_window_sweep);
    qtest_add_func("sn0/origin2000/kona/mapram-migration",
                   test_mapram_migration);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
