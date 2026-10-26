/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * This test checks the rack and deskside layouts configured by the Onyx2
 * machine model, including the presence of their modeled BaseIO-G and
 * KONA widgets.
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
#define KONA_WIDGET_CONTROL 0x24
#define KONA_COMMAND_PORT   0xc10000
#define KONA_RETURN_PAGE    UINT64_C(0x10000)
#define BRIDGE_PART   0xc002
#define BRIDGE_MFG    0x036

static uint32_t widget_id(QTestState *qts, unsigned int widget)
{
    uint8_t bytes[4];

    /* Through CPU 0's Hub, which every topology here attaches to Crossbow 0. */
    g_assert_true(qtest_memread_cpu(qts, 0, SPACE_IO +
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
 * fields and done/level bits are the Bridge NIC register's. The model
 * exposes a DS2502 board record through this interface.
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
 * sgi-kona's marked synthetic serials: "QEMU", the graphics-board letter
 * G, then instance 0 for the geometry board and 1 for the Ktown (no
 * capture gives real ones).
 */
#define GEOMETRY_SERIAL        UINT64_C(0x51454d554700)
#define KTOWN_SERIAL           UINT64_C(0x51454d554701)

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
    uint64_t reg = SPACE_IO + widget * WIDGET_SIZE + KONA_ONEWIRE;
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

static void test_kona_reset(const char *args, unsigned int widget,
                            const char *geometry_part, bool has_ktown)
{
    QTestState *qts = qtest_init(args);
    uint64_t base = SPACE_IO + widget * WIDGET_SIZE;
    uint64_t command = base + KONA_COMMAND_PORT;
    uint32_t id = widget_id(qts, widget);
    uint8_t geometry_before[64], geometry_after[64];
    uint8_t ktown_before[64], ktown_after[64];

    g_assert_cmphex(WIDGET_PART(id), ==, KONA_PART);
    g_assert_cmphex(WIDGET_MFG(id), ==, KONA_MFG);
    g_assert_true(kona_read_part(qts, widget, GEOMETRY_SERIAL,
                                 geometry_before));
    assert_record_part(geometry_before, geometry_part);
    if (has_ktown) {
        g_assert_true(kona_read_part(qts, widget, KTOWN_SERIAL,
                                     ktown_before));
        assert_record_part(ktown_before, "030-1067-001");
    } else {
        g_assert_false(kona_read_part(qts, widget, KTOWN_SERIAL,
                                      ktown_before));
    }

    qtest_writel(qts, base + KONA_WIDGET_CONTROL, 0x5a5a5a5a);
    g_assert_cmphex(qtest_readl(qts, base + KONA_WIDGET_CONTROL), ==,
                    0x5a5a5a5a);
    qtest_writel(qts, command, 0x1f2);
    qtest_writel(qts, command,
                 0x10000000u | (uint32_t)(KONA_RETURN_PAGE >> 12));
    qtest_writel(qts, command, 0);
    qtest_writel(qts, command, 0x1000);
    qtest_writel(qts, KONA_RETURN_PAGE, 0);
    qtest_writel(qts, command, 0x1e9);
    qtest_writel(qts, command, 0);
    g_assert_cmphex(qtest_readl(qts, KONA_RETURN_PAGE), ==, 1);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, base + KONA_WIDGET_CONTROL), ==, 0);
    /* The reply must have worked before reset, then leave this page alone. */
    qtest_writel(qts, KONA_RETURN_PAGE, 0x5a5a5a5a);
    qtest_writel(qts, command, 0x1e9);
    qtest_writel(qts, command, 0);
    g_assert_cmphex(qtest_readl(qts, KONA_RETURN_PAGE), ==, 0x5a5a5a5a);

    g_assert_cmphex(widget_id(qts, widget), ==, id);
    g_assert_true(kona_read_part(qts, widget, GEOMETRY_SERIAL,
                                 geometry_after));
    g_assert_cmpmem(geometry_after, sizeof(geometry_after),
                    geometry_before, sizeof(geometry_before));
    if (has_ktown) {
        g_assert_true(kona_read_part(qts, widget, KTOWN_SERIAL,
                                     ktown_after));
        g_assert_cmpmem(ktown_after, sizeof(ktown_after),
                        ktown_before, sizeof(ktown_before));
    } else {
        g_assert_false(kona_read_part(qts, widget, KTOWN_SERIAL,
                                      ktown_after));
    }
    qtest_quit(qts);
}

static void test_rack_kona_reset(void)
{
    test_kona_reset(RACK_ARGS, 15, "030-1398-001", true);
}

static void test_deskside_kona_reset(void)
{
    test_kona_reset(DESKSIDE_ARGS, 8, "030-1129-002", false);
}

static void test_rack_places_kona_at_io3(void)
{
    QTestState *qts = qtest_init(RACK_ARGS);

    assert_widget(qts, 15, KONA_PART, KONA_MFG);
    assert_widget(qts, 8, BRIDGE_PART, BRIDGE_MFG);
    /* The pipe is known by the slot IRIX names it under, io3. */
    g_assert_true(qom_has_child(qts, "/machine/module0", "io3"));
    /*
     * The pipe attaches to the realized Crossbow, live, so the links
     * alive say it is there, not the configured startup mask.
     */
    g_assert_cmphex(qom_uint(qts, "/machine/crossbow0", "links-alive") &
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
    /*
     * The pipe attaches to the realized Crossbow, live, so the links
     * alive say it is there, not the configured startup mask.
     */
    g_assert_cmphex(qom_uint(qts, "/machine/crossbow0", "links-alive") &
                    0xff00, ==, 0x8700);
    g_assert_false(qom_has_child(qts, "/machine", "crossbow1"));
    qtest_quit(qts);
}

/*
 * Each chassis's default pipe presents its catalogue configuration. The
 * rack's is the InfiniteReality3 of a real Onyx2 rack: the class word the
 * kernel driver's attach routine reads after selector 0x8800 is 1, and its
 * 0x1e9 answer names board set 5 and one RM. The deskside's is the
 * InfiniteReality2 of a real Onyx2 deskside: class word 0, board set 1 and
 * two RMs.
 */
static void assert_kona_configuration(const char *args, unsigned int widget,
                                      uint32_t product, uint32_t board_set,
                                      uint32_t rms)
{
    QTestState *qts = qtest_init(args);
    uint64_t base = SPACE_IO + widget * WIDGET_SIZE;
    uint64_t command = base + KONA_COMMAND_PORT;

    qtest_writel(qts, base + 0x8003c0, 0x8800);
    g_assert_cmphex(qtest_readl(qts, base + 0x8003d0), ==, product);
    qtest_writel(qts, KONA_RETURN_PAGE, 0);
    qtest_writel(qts, command, 0x1f2);
    qtest_writel(qts, command,
                 0x10000000u | (uint32_t)(KONA_RETURN_PAGE >> 12));
    qtest_writel(qts, command, 0);
    qtest_writel(qts, command, 0x1000);
    qtest_writel(qts, command, 0x1e9);
    qtest_writel(qts, command, 0);
    g_assert_cmphex(qtest_readl(qts, KONA_RETURN_PAGE), ==, 1);
    g_assert_cmphex(qtest_readl(qts, KONA_RETURN_PAGE + 8), ==, board_set);
    g_assert_cmphex(qtest_readl(qts, KONA_RETURN_PAGE + 8 + 0x58), ==, rms);
    qtest_quit(qts);
}

static void test_rack_kona_presents_ir3(void)
{
    assert_kona_configuration(RACK_ARGS, 15, 1, 5, 1);
}

static void test_deskside_kona_presents_ir2(void)
{
    assert_kona_configuration(DESKSIDE_ARGS, 8, 0, 1, 2);
}

/*
 * The two Hubs of the deskside share Crossbow 0, n2 at widget 9 and n1 at
 * widget A, and the firmware names each Hub's peer by reading the peer's
 * network status through the Crossbow: it aims big window 0 of its own
 * node's I/O space at the peer's widget, in I/O space, and reads the
 * doubleword at 0x01600000, whose bits 16:8 are the node number. The
 * firmware gives n2, the global master, number 0 and n1 number 1, so the
 * Hub in the second node position answers to number 0. A processor's
 * window must be the Hub that holds the number in the address, after
 * renumbering as before; reaching the Hub that started with that number
 * reads the processor's own Hub back and records both Crossbow host ports
 * as one node.
 */
#define LOCAL_HUB             (SPACE_IO + 0x01000000)
#define HUB_BWIN_ENTRY0       0x400160
#define BWIN_IO_SPACE         (UINT64_C(1) << 12)
#define BWIN_WIDGET(w)        ((uint64_t)(w) << 8)
#define HUB_NI_STATUS         0x600000
#define NI_REGION_SIZE        (UINT64_C(1) << 17)
#define NI_NODE(status)       (((status) >> 8) & 0x1ff)
#define BWIN0                 UINT64_C(0x20000000)
#define PEER_STATUS           UINT64_C(0x01600000)

static uint64_t cpu_readq(QTestState *qts, unsigned int cpu, uint64_t addr)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void cpu_writeq(QTestState *qts, unsigned int cpu, uint64_t addr,
                       uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
}

/* Read the peer's node number through this processor's own big window. */
static unsigned int peer_number(QTestState *qts, unsigned int cpu,
                                unsigned int own, unsigned int peer_widget)
{
    cpu_writeq(qts, cpu, LOCAL_HUB + HUB_BWIN_ENTRY0,
               BWIN_IO_SPACE | BWIN_WIDGET(peer_widget));
    return NI_NODE(cpu_readq(qts, cpu, SPACE_IO + own * NODE_STRIDE +
                             BWIN0 + PEER_STATUS));
}

static void test_deskside_peer_hubs_name_each_other(void)
{
    QTestState *qts = qtest_init(DESKSIDE_ARGS);
    /* Processors 0 and 1 sit on n1's node board, 2 and 3 on n2's. */
    const unsigned int n1_cpu = 0, n2_cpu = 2;

    /* Number the Hubs as the firmware does: n2 keeps 0, n1 takes 1. */
    cpu_writeq(qts, n1_cpu, LOCAL_HUB + HUB_NI_STATUS,
               NI_REGION_SIZE | (UINT64_C(1) << 8));
    g_assert_cmpuint(NI_NODE(cpu_readq(qts, n1_cpu,
                                       LOCAL_HUB + HUB_NI_STATUS)), ==, 1);
    g_assert_cmpuint(NI_NODE(cpu_readq(qts, n2_cpu,
                                       LOCAL_HUB + HUB_NI_STATUS)), ==, 0);

    /* Each master-side and slave-side probe, in both orders of setup. */
    g_assert_cmpuint(peer_number(qts, n2_cpu, 0, 0xa), ==, 1);
    g_assert_cmpuint(peer_number(qts, n1_cpu, 1, 0x9), ==, 0);
    g_assert_cmpuint(peer_number(qts, n2_cpu, 0, 0xa), ==, 1);
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
    qtest_add_func("sn0/onyx2/rack-kona-reset-clears-state-keeps-identity",
                   test_rack_kona_reset);
    qtest_add_func("sn0/onyx2/deskside-kona-reset-clears-state-keeps-identity",
                   test_deskside_kona_reset);
    qtest_add_func("sn0/onyx2/rack-kona-presents-ir3",
                   test_rack_kona_presents_ir3);
    qtest_add_func("sn0/onyx2/deskside-kona-presents-ir2",
                   test_deskside_kona_presents_ir2);
    qtest_add_func("sn0/onyx2/deskside-peer-hubs-name-each-other",
                   test_deskside_peer_hubs_name_each_other);
    return g_test_run();
}
