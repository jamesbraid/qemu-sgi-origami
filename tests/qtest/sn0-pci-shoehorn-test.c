/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test the SGI PCI-XTALK adapter with a Cad Duo IOC3 input card. */
/*
 * Register-contract tests for the SGI PCI-XTALK ADAPTER (the "PCI shoehorn",
 * hw/mips/sgi/board-pci-shoehorn.c) fitted with an SGI Cad Duo input card
 * (an sgi-ioc3, docs/datasheets/sgi-cad-duo-pci-card.md).
 *
 * The board attaches at the empty XIO slot IO4 exactly as
 * sgi-pci-expansion does (tests/qtest/sn-builder-test.c): crossbow0's
 * widget 11, outside its startup-links-alive (0x0700). A processor
 * reaches the internal Bridge and the fitted Cad Duo through crossbow0's
 * real small window for widget 11.
 *
 * Two things are checked. First, the Cad Duo enumerates: its PCI identity
 * answers through the widget window, the same config accesses a driver's own
 * probe issues. Second, its PS/2 keyboard interrupt reaches the guest: the
 * chip's SuperIO interrupt output must be wired to a Bridge pin for a
 * keystroke to be delivered, and the shoehorn wires it (to the card's INTB,
 * which the Bridge routes to pin slot ^ 4). A Cad Duo behind a plain
 * sgi-pci-expansion, which does no such wiring, leaves that interrupt dead
 * -- the RED baseline the shoehorn's wiring turns GREEN.
 *
 * Third, the card carries its own board record: a DS2505-class part on the
 * IOC3's one-wire bus, beside the station-address part, holding the
 * PCI_ENET 030-1155-002 rev D record a real card reports
 * (docs/datasheets/sgi-cad-duo-pci-card.md, "Board-identity record on the
 * IOC3 one-wire bus"). The same card behind a plain sgi-pci-expansion
 * carries only the station-address part.
 *
 * The same checks then run on a shoehorn fitted in Origin 2000's IO2
 * position, crossbow0 widget 12, where the PCI module goes, with its Cad
 * Duo at PCI device 1. That widget must read present and alive once the
 * machine is up, which is what the stock PROM needs to discover it rather
 * than report the link bad.
 */

#include "qemu/osdep.h"
#include <sys/wait.h>
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-test.h"

/*
 * IO4, the empty XIO slot a `-device` board takes on Origin 2000:
 * crossbow0's widget 11. Its own small window reaches the board's Bridge
 * and the Cad Duo.
 */
#define SHOEHORN_WIDGET     11

/*
 * The catalogue's own placement: Origin 2000 IO2, crossbow0 widget 12 (the
 * pci-expansion connector), with the Cad Duo at PCI device 1, where real
 * Origin 2000 hardware graphs show a card behind this adapter
 * (.../slot/io5/xtalk_pci/pci/1/pckb in nekonomicon thread 5749).
 */
#define PLACED_WIDGET       12
#define PLACED_CAD_DUO      1

/*
 * One fixture: which widget the board answers at and which PCI device its Cad
 * Duo occupies.
 */
typedef struct {
    const char *args;
    unsigned int widget;
    unsigned int device;
} ShoehornCase;

#define WIDGET_BASE(w)      (SPACE_IO + ((uint64_t)(w) << 24))

/* The Bridge's own chip registers sit at a slot's upper half (+REG_DATA). */
#define REG_DATA            4
#define CFG0(c)             (WIDGET_BASE((c)->widget) + 0x020000 + \
                             (c)->device * 0x1000)
#define BRIDGE_DEVICE(c)    (WIDGET_BASE((c)->widget) + 0x200 + \
                             8 * (c)->device + REG_DATA)

/* The Cad Duo's PCI slot in the adapter and its own 1 MB MMIO window. */
#define CAD_DUO_DEVICE      2
/* Bridge device windows: 2 MB for devices 0 and 1, then 1 MB each. */
#define DEVIO_OFFSET(d)     ((d) < 2 ? 0x200000 + (d) * 0x200000 : \
                             0x600000 + ((d) - 2) * 0x100000)
#define CAD_DUO_WINDOW(c)   (WIDGET_BASE((c)->widget) + \
                             DEVIO_OFFSET((c)->device))
#define CAD_DUO_PCI_MMIO    0x1c000000u

/* The Cad Duo (sgi-ioc3) PCI identity: SGI 0x10a9, IOC3 0x0003. */
#define PCI_SUBSYSTEM       0x2c
#define CAD_DUO_ID_PAIR     0x000310a9u
/* subsystem id 0xffff (uncaptured) */
#define CAD_DUO_SUBSYSTEM   0xffff10a9u

/* IOC3 registers in the chip's own MMIO window (no +REG_DATA skew). */
#define CD_INT_STATUS(c)     (CAD_DUO_WINDOW(c) + 0x1c)
#define CD_INT_ENABLE_SET(c) (CAD_DUO_WINDOW(c) + 0x20)
#define CD_KBD_TRANSMIT(c)   (CAD_DUO_WINDOW(c) + 0x0a8)
/* SuperIO keyboard/mouse status bit */
#define INT_KEYBOARD        (1u << 22)
/* Serial port A's 16550 and its pass-through SuperIO source. */
#define CD_SERIAL_A_IER(c)    (CAD_DUO_WINDOW(c) + 0x020178 + 1)
#define IER_THRI              0x02
#define SIO_SRC_A_PASSTHROUGH (1u << 6)

/*
 * The IOC3's MicroLAN engine and the pin-control bit routing it to its pin.
 */
#define CD_MICROLAN(c)       (CAD_DUO_WINDOW(c) + 0x30)
#define CD_PIN_CONTROL_SET(c) (CAD_DUO_WINDOW(c) + 0x34)
#define PIN_MICROLAN_ENABLE  (1u << 21)
#define MICROLAN_LEVEL       (1u << 0)
#define MICROLAN_DONE        (1u << 1)

#define OW_SEARCH_ROM        0xf0
#define OW_MATCH_ROM         0x55
/* the same byte, once a part is matched */
#define OW_READ_MEMORY       0xf0
#define OW_FAMILY_DS2501_UNW 0x91    /* the station-address part */
#define OW_FAMILY_DS2505     0x0b    /* the board record */

#define RECORD_PAGE          32
#define RECORD_SIZE          64
#define RECORD_CRC_RESIDUE   0xb001

/*
 * The record the card's part stores: placeholder barcode OCADDUO001, part
 * 030-1155-002, revision D, name PCI_ENET, each page closed by its inverted
 * CRC-16.
 */
static const uint8_t cad_duo_record[RECORD_SIZE] = {
    0x01, 0x4f, 0x43, 0x41, 0x44, 0x44, 0x55, 0x4f,
    0x30, 0x30, 0x31, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x33, 0x30,
    0x2d, 0x31, 0x31, 0x35, 0x35, 0x2d, 0x6e, 0x0f,
    0x20, 0x20, 0x20, 0x30, 0x30, 0x32, 0x44, 0x20,
    0x20, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x50, 0x43, 0x49, 0x5f, 0x45, 0x4e, 0x45, 0x54,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0xeb, 0x2b,
};

/* The adapter Bridge's own interrupt-forwarding registers. */
#define BRIDGE_INT_ENABLE(c)     (WIDGET_BASE((c)->widget) + 0x108 + REG_DATA)
#define BRIDGE_INT_DEST_UPPER(c) (WIDGET_BASE((c)->widget) + 0x030 + REG_DATA)
#define BRIDGE_INT_DEST_LOWER(c) (WIDGET_BASE((c)->widget) + 0x038 + REG_DATA)
#define BRIDGE_INT_STATUS(c)     (WIDGET_BASE((c)->widget) + 0x100 + REG_DATA)
#define BRIDGE_INT_HOST(c, pin)  (WIDGET_BASE((c)->widget) + 0x130 + \
                                  8 * (pin) + REG_DATA)

/*
 * A plug-in card's second interrupt leaves on its INTB, which the Bridge
 * routes to pin (slot ^ 4); INTA goes to pin slot. Public Linux SN Bridge
 * driver (Linux 2.5.0 arch/ia64/sn/io/pcibr.c, the INTB/INTD case); an add-in
 * IOC3 raises SuperIO on INTB (OpenBSD sys/arch/sgi/pci/ioc.c).
 */
#define SUPERIO_PIN(c)           ((c)->device ^ 4)

/*
 * Node 0's Hub sits at widget 9 on crossbow0; a packet-I/O host address with
 * nasid 0 aimed at the Hub's interrupt-modify offset latches an interrupt
 * number in node 0's own PI_INT_PEND0 (sn-builder-test.c's own recipe).
 */
#define HUB_WIDGET_ON_XBOW        9
#define HUB_INT_MODIFY_OFFSET     0x01800090
#define INT_HOST_PACKET_IO_NASID0 0x200
#define CAD_DUO_INT_NUMBER        45

/* A card behind the expansion Bridge is at the same slot either way. */
#define EXPANSION_ARGS \
    "-M origin2000 -display none -device sgi-pci-expansion," \
    "slot=io4,pci-bus-name=xpci " \
    "-device sgi-ioc3,bus=xpci,addr=" G_STRINGIFY(CAD_DUO_DEVICE) \
    ",subsystem-id=0xffff"

#define SHOEHORN_ARGS \
    "-M origin2000 -display none -device sgi-pci-shoehorn," \
    "slot=io4,cad-duo-slot=" G_STRINGIFY(CAD_DUO_DEVICE)

#define PLACED_ARGS \
    "-M origin2000 -display none -device sgi-pci-shoehorn,slot=io2," \
    "cad-duo-slot=" G_STRINGIFY(PLACED_CAD_DUO)

/* The same adapter carrying the dual-serial card instead of the Cad Duo. */
#define SIO_ARGS \
    "-M origin2000 -display none -device sgi-pci-shoehorn," \
    "slot=io4,sio-slot=" G_STRINGIFY(CAD_DUO_DEVICE)

/*
 * The dual-serial card's record: placeholder barcode OSIOUFC001, the
 * reseller-cited part 030-1657-003, a declared placeholder revision A and the
 * OpenBSD-sourced name PCI_SIO_UFC.
 */
static const uint8_t sio_record[RECORD_SIZE] = {
    0x01, 0x4f, 0x53, 0x49, 0x4f, 0x55, 0x46, 0x43,
    0x30, 0x30, 0x31, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x30, 0x33, 0x30,
    0x2d, 0x31, 0x36, 0x35, 0x37, 0x2d, 0x02, 0xa0,
    0x20, 0x20, 0x20, 0x30, 0x30, 0x33, 0x41, 0x20,
    0x20, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x50, 0x43, 0x49, 0x5f, 0x53, 0x49, 0x4f, 0x5f,
    0x55, 0x46, 0x43, 0x20, 0x20, 0x20, 0x70, 0xbe,
};

static const ShoehornCase sio_case = {
    SIO_ARGS, SHOEHORN_WIDGET, CAD_DUO_DEVICE,
};

static const ShoehornCase expansion_case = {
    EXPANSION_ARGS, SHOEHORN_WIDGET, CAD_DUO_DEVICE,
};
static const ShoehornCase device_case = {
    SHOEHORN_ARGS, SHOEHORN_WIDGET, CAD_DUO_DEVICE,
};
static const ShoehornCase placed_case = {
    PLACED_ARGS, PLACED_WIDGET, PLACED_CAD_DUO,
};

/*
 * Program the Cad Duo's BAR0, enable memory, and aim the Bridge window at it.
 */
static void cad_duo_map(QTestState *qts, const ShoehornCase *c)
{
    qtest_writel(qts, CFG0(c) + PCI_BAR0, CAD_DUO_PCI_MMIO);
    qtest_writel(qts, CFG0(c) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(c),
                 DEV_MEMORY_SPACE | (CAD_DUO_PCI_MMIO >> 20));
}

/*
 * Arm the adapter Bridge so the card's SuperIO pin sends an interrupt packet
 * to node 0's Hub latching CAD_DUO_INT_NUMBER, and enable the Cad Duo's
 * keyboard interrupt so a PS/2 reply raises its SuperIO output.
 */
static void arm_interrupt(QTestState *qts, const ShoehornCase *c)
{
    qtest_writel(qts, CD_INT_ENABLE_SET(c), INT_KEYBOARD);
    qtest_writel(qts, BRIDGE_INT_DEST_UPPER(c), HUB_WIDGET_ON_XBOW << 16);
    qtest_writel(qts, BRIDGE_INT_DEST_LOWER(c), HUB_INT_MODIFY_OFFSET);
    qtest_writel(qts, BRIDGE_INT_HOST(c, SUPERIO_PIN(c)),
                 (INT_HOST_PACKET_IO_NASID0 << 8) | CAD_DUO_INT_NUMBER);
    qtest_writel(qts, BRIDGE_INT_ENABLE(c), 1u << SUPERIO_PIN(c));
}

/* Send a keyboard byte and report whether node 0's Hub latched it. */
static bool keystroke_reaches_hub(QTestState *qts, const ShoehornCase *c)
{
    qtest_writel(qts, CD_KBD_TRANSMIT(c), 0xff);
    g_assert_cmphex(qtest_readl(qts, CD_INT_STATUS(c)) & INT_KEYBOARD, ==,
                    INT_KEYBOARD);
    return qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0) &
           (1ULL << CAD_DUO_INT_NUMBER);
}

/*
 * The Cad Duo's PCI identity answers through the widget window: the board
 * attaches, its widget is an inbound destination, and a bus scan reads the
 * chip's own vendor/device and subsystem dwords.
 */
static void test_cad_duo_enumerates(const void *data)
{
    const ShoehornCase *c = data;
    QTestState *qts = qtest_init(c->args);

    g_assert_cmphex(qtest_readl(qts, CFG0(c)), ==, CAD_DUO_ID_PAIR);
    g_assert_cmphex(qtest_readl(qts, CFG0(c) + PCI_SUBSYSTEM), ==,
                    CAD_DUO_SUBSYSTEM);
    qtest_quit(qts);
}

/*
 * The RED baseline: a Cad Duo plugged into a plain sgi-pci-expansion, whose
 * board does not wire the chip's SuperIO interrupt to any Bridge pin. The
 * keyboard reply raises the chip's own SuperIO output, but with nothing
 * carrying it to a Bridge pin the packet never leaves and the Hub never
 * latches. (The card's ethernet INTA is idle because there is no network
 * traffic, so its
 * pin stays low.)
 */
static void test_cad_duo_ps2_interrupt_dead_bare(const void *data)
{
    const ShoehornCase *c = data;
    QTestState *qts = qtest_init(c->args);

    cad_duo_map(qts, c);
    arm_interrupt(qts, c);
    g_assert_false(keystroke_reaches_hub(qts, c));
    qtest_quit(qts);
}

/*
 * The GREEN result: the same Cad Duo fitted in the shoehorn, which wires the
 * chip's SuperIO output to the card's INTB, Bridge pin (slot ^ 4). A keyboard
 * reply now raises that pin, the Bridge forwards an interrupt packet up
 * through crossbow0 to node 0's Hub, and PI_INT_PEND0 latches the number.
 */
static void test_cad_duo_ps2_interrupt_reaches_hub(const void *data)
{
    const ShoehornCase *c = data;
    QTestState *qts = qtest_init(c->args);

    cad_duo_map(qts, c);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0) &
                    (1ULL << CAD_DUO_INT_NUMBER), ==, 0);
    arm_interrupt(qts, c);
    g_assert_true(keystroke_reaches_hub(qts, c));
    /*
     * The keystroke must not also hold the card's INTA pin, which a guest
     * services with the Ethernet handler alone: IRIX 6.5.30 looped in that
     * handler forever when SuperIO shared the pin.
     */
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS(c)) &
                    (1u << c->device), ==, 0);
    qtest_quit(qts);
}

/*
 * One bit slot on the Cad Duo's MicroLAN, with the widths OpenBSD's and
 * Linux's masters use.
 */
static bool ow_slot(QTestState *qts, const ShoehornCase *c,
                    unsigned int pulse, unsigned int sample)
{
    uint32_t v;

    qtest_writel(qts, CD_MICROLAN(c), (pulse << 10) | (sample << 2));
    v = qtest_readl(qts, CD_MICROLAN(c));
    g_assert_cmphex(v & MICROLAN_DONE, ==, MICROLAN_DONE);
    return v & MICROLAN_LEVEL;
}

#define OW_RESET(q, c)      ow_slot((q), (c), 520, 65)
#define OW_READ(q, c)       ow_slot((q), (c), 6, 13)
#define OW_WRITE_ONE(q, c)  ow_slot((q), (c), 6, 13)
#define OW_WRITE_ZERO(q, c) ow_slot((q), (c), 80, 30)

static void ow_write_byte(QTestState *qts, const ShoehornCase *c, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            OW_WRITE_ONE(qts, c);
        } else {
            OW_WRITE_ZERO(qts, c);
        }
    }
}

static uint8_t ow_read_byte(QTestState *qts, const ShoehornCase *c)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (OW_READ(qts, c)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

/* The ROM identity's CRC-8: 0x8c reflected, seeded zero. */
static uint8_t ow_crc8(uint64_t rom)
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

/* CRC-16, reflected polynomial 0xa001, as Linux checks SGI records. */
static uint16_t ow_crc16(const uint8_t *data, unsigned int len)
{
    unsigned int i, bit;
    uint16_t crc = 0;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}

/*
 * One pass of the standard Search ROM walk (Maxim AN187), zero branch first;
 * chain each result and `*last_discrepancy` into the next pass.
 */
static uint64_t ow_search_pass(QTestState *qts, const ShoehornCase *c,
                               uint64_t rom, unsigned int *last_discrepancy)
{
    unsigned int bit_number, last_zero = 0;
    uint64_t found = 0;

    g_assert_false(OW_RESET(qts, c));       /* pulled low: someone answers */
    ow_write_byte(qts, c, OW_SEARCH_ROM);
    for (bit_number = 1; bit_number <= 64; bit_number++) {
        bool bit = OW_READ(qts, c);
        bool complement = OW_READ(qts, c);
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
            OW_WRITE_ONE(qts, c);
        } else {
            OW_WRITE_ZERO(qts, c);
        }
    }
    *last_discrepancy = last_zero;
    g_assert_cmphex(ow_crc8(found), ==, 0);
    return found;
}

/* Discover every part on the Cad Duo's bus; returns how many. */
static unsigned int ow_search_all(QTestState *qts, const ShoehornCase *c,
                                  uint64_t *rom, unsigned int max)
{
    unsigned int last_discrepancy = 0;
    unsigned int count = 0;

    qtest_writel(qts, CD_PIN_CONTROL_SET(c), PIN_MICROLAN_ENABLE);
    do {
        g_assert_cmpuint(count, <, max);
        rom[count] = ow_search_pass(qts, c, count ? rom[count - 1] : 0,
                                    &last_discrepancy);
        count++;
    } while (last_discrepancy != 0);
    return count;
}

/*
 * Select `rom` by Match ROM, as OpenBSD's owserial and the firmware do on a
 * multi-part bus, and read `len` bytes from address 0 with Read Memory.
 */
static void ow_read_memory(QTestState *qts, const ShoehornCase *c,
                           uint64_t rom, uint8_t *out, unsigned int len)
{
    unsigned int i;

    g_assert_false(OW_RESET(qts, c));
    ow_write_byte(qts, c, OW_MATCH_ROM);
    for (i = 0; i < 8; i++) {
        ow_write_byte(qts, c, (uint8_t)(rom >> (i * 8)));
    }
    ow_write_byte(qts, c, OW_READ_MEMORY);
    ow_write_byte(qts, c, 0x00);
    ow_write_byte(qts, c, 0x00);
    for (i = 0; i < len; i++) {
        out[i] = ow_read_byte(qts, c);
    }
}

/*
 * The fitted card's IOC3 bus carries two parts: the station-address part and
 * a DS2505-class part holding the card's own record, which reads back as the
 * part's page image, each page leaving the CRC-16 residue, with unprogrammed
 * memory past it.
 */
static void test_cad_duo_carries_its_board_record(const void *data)
{
    const ShoehornCase *c = data;
    QTestState *qts = qtest_init(c->args);
    uint8_t memory[RECORD_SIZE + RECORD_PAGE];
    uint64_t rom[4], record_rom = 0;
    bool station = false;
    unsigned int i, n;

    cad_duo_map(qts, c);
    n = ow_search_all(qts, c, rom, G_N_ELEMENTS(rom));
    g_assert_cmpuint(n, ==, 2);
    for (i = 0; i < n; i++) {
        switch (rom[i] & 0xff) {
        case OW_FAMILY_DS2501_UNW:
            station = true;
            break;
        case OW_FAMILY_DS2505:
            record_rom = rom[i];
            break;
        default:
            g_assert_not_reached();
        }
    }
    g_assert_true(station);
    g_assert_cmphex(record_rom, !=, 0);

    ow_read_memory(qts, c, record_rom, memory, sizeof(memory));
    g_assert_cmpmem(memory, RECORD_SIZE, cad_duo_record, RECORD_SIZE);
    g_assert_cmphex(ow_crc16(memory, RECORD_PAGE), ==, RECORD_CRC_RESIDUE);
    g_assert_cmphex(ow_crc16(memory + RECORD_PAGE, RECORD_PAGE), ==,
                    RECORD_CRC_RESIDUE);
    for (i = RECORD_SIZE; i < sizeof(memory); i++) {
        g_assert_cmphex(memory[i], ==, 0xff);
    }
    qtest_quit(qts);
}

/*
 * A bare sgi-ioc3 given no record, behind a plain expansion board, keeps the
 * station-address part alone.
 */
static void test_bare_card_has_no_board_record(const void *data)
{
    const ShoehornCase *c = data;
    QTestState *qts = qtest_init(c->args);
    uint64_t rom[4];

    cad_duo_map(qts, c);
    g_assert_cmpuint(ow_search_all(qts, c, rom, G_N_ELEMENTS(rom)), ==, 1);
    g_assert_cmphex(rom[0] & 0xff, ==, OW_FAMILY_DS2501_UNW);
    qtest_quit(qts);
}

/*
 * The dual-serial card enumerates as an IOC3 with the uncaptured subsystem
 * placeholder, and has serial ports and nothing else.
 */
static void test_sio_enumerates(const void *data)
{
    const ShoehornCase *c = data;
    QTestState *qts = qtest_init(c->args);
    static const char *const absent[] = {
        "eth", "kbc", "parallel", "rtc", "ds1685", "onewire-part",
    };
    size_t i;

    g_assert_cmphex(qtest_readl(qts, CFG0(c)), ==, CAD_DUO_ID_PAIR);
    g_assert_cmphex(qtest_readl(qts, CFG0(c) + PCI_SUBSYSTEM), ==,
                    CAD_DUO_SUBSYSTEM);
    for (i = 0; i < ARRAY_SIZE(absent); i++) {
        g_autofree char *path = g_strdup_printf(
            "/machine/module0/io4/sio-ufc/%s", absent[i]);
        g_autoptr(QDict) response = qtest_qmp(qts,
            "{'execute': 'qom-list', 'arguments': {'path': %s}}", path);

        g_assert_true(qdict_haskey(response, "error"));
    }
    {
        g_autoptr(QDict) response = qtest_qmp(qts,
            "{'execute': 'qom-list', 'arguments': {'path': "
            "'/machine/module0/io4/sio-ufc/serial[1]'}}");

        g_assert_false(qdict_haskey(response, "error"));
    }
    qtest_quit(qts);
}

/*
 * The card's one-wire bus carries its record part alone: no Ethernet, so no
 * station-address part.
 */
static void test_sio_carries_its_board_record(const void *data)
{
    const ShoehornCase *c = data;
    QTestState *qts = qtest_init(c->args);
    uint8_t memory[RECORD_SIZE + RECORD_PAGE];
    uint64_t rom[4];
    unsigned int i;

    cad_duo_map(qts, c);
    g_assert_cmpuint(ow_search_all(qts, c, rom, G_N_ELEMENTS(rom)), ==, 1);
    g_assert_cmphex(rom[0] & 0xff, ==, OW_FAMILY_DS2505);
    ow_read_memory(qts, c, rom[0], memory, sizeof(memory));
    g_assert_cmpmem(memory, RECORD_SIZE, sio_record, RECORD_SIZE);
    g_assert_cmphex(ow_crc16(memory, RECORD_PAGE), ==, RECORD_CRC_RESIDUE);
    g_assert_cmphex(ow_crc16(memory + RECORD_PAGE, RECORD_PAGE), ==,
                    RECORD_CRC_RESIDUE);
    for (i = RECORD_SIZE; i < sizeof(memory); i++) {
        g_assert_cmphex(memory[i], ==, 0xff);
    }
    qtest_quit(qts);
}

/*
 * A serial interrupt on the card raises its SuperIO output, which the adapter
 * wires to the card's INTB, Bridge pin slot ^ 4, and reaches node 0's Hub;
 * the card's own INTA pin stays low.
 */
static void test_sio_serial_interrupt_reaches_hub(const void *data)
{
    const ShoehornCase *c = data;
    QTestState *qts = qtest_init(c->args);

    cad_duo_map(qts, c);
    qtest_writel(qts, BRIDGE_INT_DEST_UPPER(c), HUB_WIDGET_ON_XBOW << 16);
    qtest_writel(qts, BRIDGE_INT_DEST_LOWER(c), HUB_INT_MODIFY_OFFSET);
    qtest_writel(qts, BRIDGE_INT_HOST(c, SUPERIO_PIN(c)),
                 (INT_HOST_PACKET_IO_NASID0 << 8) | CAD_DUO_INT_NUMBER);
    qtest_writel(qts, BRIDGE_INT_ENABLE(c), 1u << SUPERIO_PIN(c));
    qtest_writel(qts, CD_INT_ENABLE_SET(c), SIO_SRC_A_PASSTHROUGH);
    qtest_writeb(qts, CD_SERIAL_A_IER(c), IER_THRI);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS(c)) &
                    ((1u << SUPERIO_PIN(c)) | (1u << c->device)), ==,
                    1u << SUPERIO_PIN(c));
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0) &
                    (1ULL << CAD_DUO_INT_NUMBER), ==,
                    1ULL << CAD_DUO_INT_NUMBER);
    qtest_quit(qts);
}

/* The adapter has one PCI position, so it takes one card. */
static void test_one_card_only(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000", "-accel", "qtest",
        "-display", "none", "-device",
        "sgi-pci-shoehorn,slot=io4,"
        "cad-duo-slot=1,sio-slot=2", NULL
    };
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_false(WIFSIGNALED(wait_status));
    g_assert_nonnull(strstr(error_output,
                            "set cad-duo-slot or sio-slot, not both"));
}

/*
 * A board fitted at launch is attached once every Crossbow has realized:
 * its port then reads a widget present and the link alive
 * (docs/datasheets/xbow.md, link auxiliary status bit 5 and link status
 * bit 31, Crossbow 0's own registers at widget 0).
 */
#define XBOW_LINK_STATUS(w) (SPACE_IO + 0x110 + ((w) - 8) * 0x40 + 4)
#define XBOW_LINK_AUX(w)    (SPACE_IO + 0x138 + ((w) - 8) * 0x40 + 4)
#define XBOW_LINK_ALIVE     (1u << 31)
#define XBOW_AUX_PRESENT    (1u << 5)

static void test_fitted_widget_is_alive(const void *data)
{
    QTestState *qts = qtest_init(PLACED_ARGS);

    g_assert_cmphex(qtest_readl(qts, XBOW_LINK_AUX(PLACED_WIDGET)) &
                    XBOW_AUX_PRESENT, ==, XBOW_AUX_PRESENT);
    g_assert_cmphex(qtest_readl(qts, XBOW_LINK_STATUS(PLACED_WIDGET)) &
                    XBOW_LINK_ALIVE, ==, XBOW_LINK_ALIVE);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/pci-shoehorn/cad-duo-enumerates", &device_case,
                        test_cad_duo_enumerates);
    qtest_add_data_func("sn0/pci-shoehorn/cad-duo-ps2-interrupt-dead-bare",
                        &expansion_case,
                        test_cad_duo_ps2_interrupt_dead_bare);
    qtest_add_data_func("sn0/pci-shoehorn/cad-duo-ps2-interrupt-reaches-hub",
                        &device_case,
                        test_cad_duo_ps2_interrupt_reaches_hub);
    qtest_add_data_func("sn0/pci-shoehorn/cad-duo-board-record",
                        &device_case, test_cad_duo_carries_its_board_record);
    qtest_add_data_func("sn0/pci-shoehorn/bare-card-has-no-board-record",
                        &expansion_case, test_bare_card_has_no_board_record);
    qtest_add_data_func("sn0/pci-shoehorn/sio-enumerates", &sio_case,
                        test_sio_enumerates);
    qtest_add_data_func("sn0/pci-shoehorn/sio-board-record", &sio_case,
                        test_sio_carries_its_board_record);
    qtest_add_data_func("sn0/pci-shoehorn/sio-serial-interrupt-reaches-hub",
                        &sio_case, test_sio_serial_interrupt_reaches_hub);
    qtest_add_data_func("sn0/pci-shoehorn/one-card-only", NULL,
                        test_one_card_only);
    qtest_add_data_func("sn0/pci-shoehorn/placed/widget-alive",
                        NULL, test_fitted_widget_is_alive);
    qtest_add_data_func("sn0/pci-shoehorn/placed/cad-duo-enumerates",
                        &placed_case, test_cad_duo_enumerates);
    qtest_add_data_func("sn0/pci-shoehorn/placed/cad-duo-interrupt-reaches-hub",
                        &placed_case, test_cad_duo_ps2_interrupt_reaches_hub);
    qtest_add_data_func("sn0/pci-shoehorn/placed/cad-duo-board-record",
                        &placed_case, test_cad_duo_carries_its_board_record);
    return g_test_run();
}
