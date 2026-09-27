/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Onyx2 machine's two topologies, as the Onyx2 datasheet in the Origami
 * repository describes them (docs/datasheets/onyx2.md): the rack is the
 * Origin 2000 eight-processor module with a BaseIO-G in IO1 (Crossbow 0
 * widget 8) and the pipe's KONA widget behind an Xtown in IO3 (widget F);
 * the deskside has one Crossbow whose table puts the BaseIO-G at widget F
 * and the pipe directly at widget 8. Neither fits an SI graphics board.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-test.h"

#define RACK_ARGS     "-M onyx2 -display none"
#define DESKSIDE_ARGS \
    "-M onyx2,topology=onyx2-deskside -smp 4 -m 128M -display none"

#define WIDGET_SIZE   UINT64_C(0x1000000)
#define WIDGET_ID     4

/* revision:part:manufacturer:1, the identification register's layout. */
#define WIDGET_PART(id) (((id) >> 12) & 0xffff)
#define WIDGET_MFG(id)  (((id) >> 1) & 0x7ff)

#define KONA_PART     0xc102
#define KONA_MFG      0x2aa
#define BRIDGE_PART   0xc002
#define BRIDGE_MFG    0x036

static uint32_t widget_id(QTestState *qts, unsigned int widget)
{
    uint8_t bytes[4];

    /* Through CPU 0's Hub, which every topology here attaches to Crossbow 0. */
    g_assert_true(qtest_memread_cpu(qts, 0, SPACE_XTALK +
                                    widget * WIDGET_SIZE + WIDGET_ID,
                                    bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

static void assert_widget(QTestState *qts, unsigned int widget,
                          unsigned int part, unsigned int mfg)
{
    uint32_t id = widget_id(qts, widget);

    g_assert_cmphex(WIDGET_PART(id), ==, part);
    g_assert_cmphex(WIDGET_MFG(id), ==, mfg);
}

static uint64_t qom_uint(QTestState *qts, const char *path, const char *prop)
{
    g_autoptr(QDict) r = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments':"
                                   " {'path': %s, 'property': %s}}",
                                   path, prop);

    g_assert_false(qdict_haskey(r, "error"));
    return qdict_get_int(r, "return");
}

static bool qom_has_child(QTestState *qts, const char *parent,
                          const char *child)
{
    g_autoptr(QDict) r = qtest_qmp(qts, "{'execute': 'qom-list',"
                                   " 'arguments': {'path': %s}}", parent);
    const QListEntry *entry;

    for (entry = qlist_first(qdict_get_qlist(r, "return")); entry;
         entry = qlist_next(entry)) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(item, "name"), child)) {
            return true;
        }
    }
    return false;
}

/*
 * The pipe's one-wire chain, read the way the PROM reads it: through KONA's
 * +0x98 bit-level register (hw/misc/sgi-kona.c), whose pulse and sample
 * fields and done/level bits are the Bridge NIC register's. Each part is
 * a DS2502 holding the fixed-offset board record docs/datasheets/bridge.md
 * in the Origami tree lays out.
 */
#define KONA_ONEWIRE           0x98
#define ONEWIRE_LEVEL          (1u << 0)
#define ONEWIRE_DONE           (1u << 1)
#define ONEWIRE_PULSE(n)       ((uint32_t)(n) << 10)
#define ONEWIRE_SAMPLE(n)      ((uint32_t)(n) << 2)
#define ONEWIRE_FAMILY_DS2502  0x09
#define ONEWIRE_MATCH_ROM      0x55
#define DS2502_READ_MEMORY     0xf0

/*
 * sgi-kona's placeholder serials: the geometry board is 1 and the Ktown
 * 2 (hw/misc/sgi-kona.c; no capture gives real ones).
 */
#define GEOMETRY_SERIAL        1
#define KTOWN_SERIAL           2

/* The record's part number: nine characters at 21, three more at 35. */
#define RECORD_PART_BASE       21
#define RECORD_PART_DASH       35

static bool onewire_slot(QTestState *qts, uint64_t reg, unsigned int pulse,
                         unsigned int sample)
{
    uint32_t value;

    qtest_writel(qts, reg, ONEWIRE_PULSE(pulse) | ONEWIRE_SAMPLE(sample));
    value = qtest_readl(qts, reg);
    g_assert_cmphex(value & ONEWIRE_DONE, ==, ONEWIRE_DONE);
    return value & ONEWIRE_LEVEL;
}

static void onewire_write_byte(QTestState *qts, uint64_t reg, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            onewire_slot(qts, reg, 6, 13);
        } else {
            onewire_slot(qts, reg, 80, 30);
        }
    }
}

static uint8_t onewire_read_byte(QTestState *qts, uint64_t reg)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (onewire_slot(qts, reg, 6, 13)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

static uint8_t onewire_crc8(uint8_t crc, const uint8_t *buf, size_t len)
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
 * Select the part with this serial and read its memory from address 0.
 * Returns false when nothing answers the read, as when no part carries
 * the serial: every deselected part leaves the bus high, so the CRC the
 * part would echo for the command reads as 0xff instead.
 */
static bool kona_read_part(QTestState *qts, unsigned int widget,
                           uint64_t serial, uint8_t record[64])
{
    uint64_t reg = SPACE_XTALK + widget * WIDGET_SIZE + KONA_ONEWIRE;
    uint8_t rom[8] = { ONEWIRE_FAMILY_DS2502 };
    uint8_t cmd[3] = { DS2502_READ_MEMORY, 0, 0 };
    uint8_t echo;
    unsigned int i;

    for (i = 0; i < 6; i++) {
        rom[1 + i] = (serial >> (8 * i)) & 0xff;
    }
    rom[7] = onewire_crc8(0, rom, 7);

    /* The reset's presence pulse: some part on the chain answers. */
    g_assert_false(onewire_slot(qts, reg, 520, 65));
    onewire_write_byte(qts, reg, ONEWIRE_MATCH_ROM);
    for (i = 0; i < sizeof(rom); i++) {
        onewire_write_byte(qts, reg, rom[i]);
    }
    for (i = 0; i < sizeof(cmd); i++) {
        onewire_write_byte(qts, reg, cmd[i]);
    }
    echo = onewire_read_byte(qts, reg);
    if (echo == 0xff) {
        return false;
    }
    g_assert_cmphex(echo, ==, onewire_crc8(0, cmd, sizeof(cmd)));
    for (i = 0; i < 64; i++) {
        record[i] = onewire_read_byte(qts, reg);
    }
    return true;
}

static void assert_record_part(const uint8_t record[64], const char *part)
{
    g_autofree char *got = g_strdup_printf("%.9s%.3s",
                                           &record[RECORD_PART_BASE],
                                           &record[RECORD_PART_DASH]);

    g_assert_cmpstr(got, ==, part);
}

/*
 * A rack pipe's geometry board is a GE16-4, 030-1398-001, and its chain
 * also holds the Ktown's record, 030-1067-001, from the graphics module
 * it is cabled through.
 */
static void test_rack_chain_holds_ge16_and_ktown(void)
{
    QTestState *qts = qtest_init(RACK_ARGS);
    uint8_t record[64];

    g_assert_true(kona_read_part(qts, 15, GEOMETRY_SERIAL, record));
    assert_record_part(record, "030-1398-001");
    g_assert_true(kona_read_part(qts, 15, KTOWN_SERIAL, record));
    assert_record_part(record, "030-1067-001");
    qtest_quit(qts);
}

/*
 * A deskside pipe's geometry board is a GE14-4, 030-1129-002, and it has
 * no Ktown: its chain holds the geometry record alone.
 */
static void test_deskside_chain_holds_ge14_alone(void)
{
    QTestState *qts = qtest_init(DESKSIDE_ARGS);
    uint8_t record[64];

    g_assert_true(kona_read_part(qts, 8, GEOMETRY_SERIAL, record));
    assert_record_part(record, "030-1129-002");
    g_assert_false(kona_read_part(qts, 8, KTOWN_SERIAL, record));
    qtest_quit(qts);
}

static void test_rack_places_kona_at_io3(void)
{
    QTestState *qts = qtest_init(RACK_ARGS);

    assert_widget(qts, 15, KONA_PART, KONA_MFG);
    assert_widget(qts, 8, BRIDGE_PART, BRIDGE_MFG);
    /* The pipe is known by the slot IRIX names it under, io3. */
    g_assert_true(qom_has_child(qts, "/machine/module0", "io3"));
    g_assert_cmphex(qom_uint(qts, "/machine/crossbow0",
                             "startup-links-alive") &
                    0xff00, ==, 0x8700);
    qtest_quit(qts);
}

static void test_deskside_places_kona_at_io4(void)
{
    QTestState *qts = qtest_init(DESKSIDE_ARGS);

    assert_widget(qts, 8, KONA_PART, KONA_MFG);
    assert_widget(qts, 15, BRIDGE_PART, BRIDGE_MFG);
    g_assert_true(qom_has_child(qts, "/machine/module0", "io4"));
    /* Widgets 9 and A are the two Hubs; nothing else is fitted. */
    g_assert_cmphex(qom_uint(qts, "/machine/crossbow0",
                             "startup-links-alive") &
                    0xff00, ==, 0x8700);
    g_assert_false(qom_has_child(qts, "/machine", "crossbow1"));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/onyx2/rack-places-kona-at-io3",
                   test_rack_places_kona_at_io3);
    qtest_add_func("sn0/onyx2/deskside-places-kona-at-io4",
                   test_deskside_places_kona_at_io4);
    qtest_add_func("sn0/onyx2/rack-chain-holds-ge16-and-ktown",
                   test_rack_chain_holds_ge16_and_ktown);
    qtest_add_func("sn0/onyx2/deskside-chain-holds-ge14-alone",
                   test_deskside_chain_holds_ge14_alone);
    return g_test_run();
}
