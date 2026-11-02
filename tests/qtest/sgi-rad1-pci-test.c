/*
 * Register-contract tests for the SGI PCI Digital Audio Board (RAD board 13)
 * on a Bridge test head.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * The "none" machine holds one Bridge and a block of transfer memory; no
 * machine places the card here. tests/qtest/sn1-rad1-pci-test.c covers the
 * machines that do.
 *
 * Offsets, values and channel types are restated from the board-13 datasheet
 * rather than shared with the device, so the test pins the contract
 * independently of the implementation. Where the datasheet grades a fact
 * unknown the model makes a stated choice; those checks name the choice.
 *
 * The cases cover the PCI identity (revision 0xd0, class 0, no interrupt
 * pin), BAR0 sizing, the board-13 start-up stores, the absence of the analog
 * interface, the UST counter and its control word, the one-shot and
 * continuous status DMA, the position-field encoding, the bus-master gate,
 * reset and migration.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqos/libqos-malloc.h"
#include "libqos/pci.h"
#include "libqos/pci-sgi-bridge.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"

#define RAD_VENDOR          0x10a9
#define RAD_DEVICE          0x0005
#define BOARD13_REVISION    0xd0
#define BOARD12_REVISION    0xc0
#define BAR0_SIZE           0x2000

/* BAR0 registers. */
#define REG_POS_FIRST       0x04
#define REG_POS_LAST        0x2c
#define REG_UST             0x30
#define REG_UST_CONTROL     0x40
#define REG_ADAT_IN_EN      0x44
#define REG_AES_IN_EN       0x48
#define REG_4C              0x4c
#define REG_ADAT_OUT_EN     0x50
#define REG_AES_OUT_EN      0x54
#define REG_58              0x58
#define REG_STATUS_CTRL     0x5c
#define REG_70              0x70
#define REG_74              0x74
#define REG_78              0x78
#define REG_7C              0x7c
#define REG_80              0x80
#define REG_84              0x84
#define REG_88              0x88
#define REG_8C              0x8c
#define REG_90              0x90
#define REG_A0              0xa0
#define REG_B8              0xb8
#define REG_BC              0xbc
#define REG_STATUS_LIMIT    0x504
#define REG_STATUS_LO       0x500
#define REG_STATUS_HI       0x528
#define REG_SUBCODE_LAST    0x1218

/* A channel's descriptor set A, enable register and bit, tag and positions. */
typedef struct {
    const char *name;
    unsigned tag;
    unsigned a_lo, a_hi, a_ctrl;
    unsigned b_lo, b_hi, b_ctrl;
    unsigned enable_reg, enable_bit;
    unsigned pos_a, pos_b;
    unsigned slot_ptr;
    unsigned frame_bytes;
} Chan;

static const Chan AES_IN = {
    "AES3 input", 2, 0x4c8, 0x50c, 0x4cc, 0x418, 0x41c, 0x420,
    REG_AES_IN_EN, 1, 0x0c, 0x10, 0x08, 8,
};
static const Chan AES_OUT = {
    "AES3 output", 3, 0x4f0, 0x520, 0x4f4, 0x424, 0x428, 0x42c,
    REG_AES_OUT_EN, 1, 0x24, 0x28, 0x30, 8,
};
static const Chan ADAT_OUT = {
    "ADAT output", 5, 0x4e8, 0x51c, 0x4ec, 0x43c, 0x440, 0x444,
    REG_ADAT_OUT_EN, 1, 0x1c, 0x20, 0x28, 32,
};
static const Chan AES_SUB = {
    "AES3 input subcode", 6, 0x4e0, 0x518, 0x4e4, 0x448, 0x44c, 0x450,
    REG_AES_IN_EN, 4, 0x10, 0x10, 0x20, 256,
};
static const Chan ADAT_SUB = {
    "ADAT input subcode", 7, 0x4d8, 0x514, 0x4dc, 0x454, 0x458, 0x45c,
    REG_ADAT_IN_EN, 4, 0x08, 0x08, 0x18, 128,
};

/* The analog channels the board does not have: types 0 and 1. */
static const Chan ANALOG_IN = {
    "analog input", 0, 0x4d0, 0x510, 0x4d4, 0x400, 0x404, 0x408,
    REG_4C, 1, 0x14, 0x18, 0x10, 8,
};
static const Chan ANALOG_OUT = {
    "analog output", 1, 0x4f8, 0x524, 0x4fc, 0x40c, 0x410, 0x414,
    REG_58, 1, 0x2c, 0x18, 0x38, 8,
};

/* Status slots: 0x80 bytes; +0x7c bit 31 is "complete". */
#define SLOT_BYTES          0x80
#define SLOT_MIRROR         0x40
#define SLOT_FLAGS          0x78
#define SLOT_DONE           0x7c
#define DONE_BIT            0x80000000u
#define AES_NO_INPUT        0x00080000u
#define STATUS_ONESHOT      0x40001u
#define STATUS_STOP         0x1fffcu
/* Interval 250 us of 33 MHz clocks: IRIX's 250, as the driver encodes it. */
#define STATUS_CONT_250     0x480efu
#define STATUS_INTERVAL_NS  250030
#define SLOT_SENTINEL       0xa5

/* A 44.1 kHz frame is 22675.7 ns; positions below use whole-frame spans. */
#define MS(n)               ((int64_t)(n) * 1000000)

typedef struct {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar bar;
    uint64_t barsize;
} Rad;

static QGuestAllocator alloc;

static void rad_attach(Rad *r, QTestState *qts)
{
    r->qts = qts;
    r->bus = qpci_init_sgi_bridge(qts, &alloc);
    r->dev = qpci_device_find(r->bus, QPCI_DEVFN(2, 0));
    g_assert_nonnull(r->dev);
}

static void rad_enable(Rad *r)
{
    r->bar = qpci_iomap(r->dev, 0, &r->barsize);
    qpci_device_enable(r->dev);
}

static Rad rad_boot_with(const char *type, const char *extra)
{
    Rad r = { 0 };

    qpci_sgi_bridge_alloc_init(&alloc, 0);
    rad_attach(&r, qtest_initf("-M none " QPCI_SGI_BRIDGE_HEAD
                               "-device %s,id=rad,addr=2 %s", type, extra));
    rad_enable(&r);
    return r;
}

static Rad rad_boot(void)
{
    return rad_boot_with("sgi-rad1-pci", "");
}

static void rad_quit(Rad *r)
{
    g_free(r->dev);
    qpci_free_sgi_bridge(r->bus);
    qtest_quit(r->qts);
    alloc_destroy(&alloc);
}

static uint32_t rd(Rad *r, unsigned off)
{
    return qpci_io_readl(r->dev, r->bar, off);
}

static void wr(Rad *r, unsigned off, uint32_t val)
{
    qpci_io_writel(r->dev, r->bar, off, val);
}

static uint32_t word(const uint8_t *slot, unsigned off)
{
    return ldl_le_p(slot + off);
}

static void slot_read(Rad *r, uint64_t at, uint8_t *slot)
{
    qtest_memread(r->qts, at, slot, SLOT_BYTES);
}

/* A field of the 14-bit position encoding. */
static uint32_t field(uint32_t reg, unsigned shift)
{
    return (reg >> shift) & 0x3fff;
}

/*
 * The one-shot status transfer, as the driver issues it: limit -4, the slot
 * address (low then high half), then 0x40001.
 */
static void status_oneshot(Rad *r, uint64_t slot_addr)
{
    wr(r, REG_STATUS_LIMIT, 0xfffffffc);
    wr(r, REG_STATUS_LO, (uint32_t)slot_addr);
    wr(r, REG_STATUS_HI, slot_addr >> 32);
    wr(r, REG_STATUS_CTRL, STATUS_ONESHOT);
}

/*
 * Start a channel as the driver does: sets A and B identical, then the
 * enable bit. The control word is (tag << 3) plus the negated length in
 * 128-byte units, shifted left 7.
 */
static void chan_start(Rad *r, const Chan *c, uint64_t ring, unsigned bytes)
{
    uint32_t ctrl = (c->tag << 3) + ((0u - bytes / 128) << 7);

    wr(r, c->a_lo, (uint32_t)ring);
    wr(r, c->a_hi, ring >> 32);
    wr(r, c->a_ctrl, ctrl);
    wr(r, c->b_lo, (uint32_t)ring);
    wr(r, c->b_hi, ring >> 32);
    wr(r, c->b_ctrl, ctrl);
    wr(r, c->enable_reg, rd(r, c->enable_reg) | c->enable_bit);
}

static void chan_stop(Rad *r, const Chan *c)
{
    wr(r, c->enable_reg, rd(r, c->enable_reg) & ~c->enable_bit);
}

static void test_config_identity(void)
{
    Rad r = rad_boot();
    QPCIDevice *d = r.dev;

    g_assert_cmphex(qpci_config_readl(d, PCI_VENDOR_ID), ==, 0x000510a9);
    g_assert_cmphex(qpci_config_readw(d, PCI_VENDOR_ID), ==, RAD_VENDOR);
    g_assert_cmphex(qpci_config_readw(d, PCI_DEVICE_ID), ==, RAD_DEVICE);
    /* Class and revision dword: class 0, revision 0xd0 (board 13.0). */
    g_assert_cmphex(qpci_config_readl(d, PCI_CLASS_REVISION), ==,
                    BOARD13_REVISION);
    g_assert_cmphex(qpci_config_readb(d, PCI_REVISION_ID) >> 4, ==, 13);
    g_assert_cmphex(qpci_config_readb(d, PCI_REVISION_ID) & 0xf, ==, 0);
    g_assert_cmphex(qpci_config_readb(d, PCI_REVISION_ID), !=,
                    BOARD12_REVISION);
    g_assert_cmphex(qpci_config_readb(d, PCI_HEADER_TYPE), ==, 0);
    /* Subsystem 0000:0000, as the one measured RAD (approximation). */
    g_assert_cmphex(qpci_config_readw(d, PCI_SUBSYSTEM_VENDOR_ID), ==, 0);
    g_assert_cmphex(qpci_config_readw(d, PCI_SUBSYSTEM_ID), ==, 0);
    /* The driver registers no interrupt: no pin, so nothing routes. */
    g_assert_cmphex(qpci_config_readb(d, PCI_INTERRUPT_PIN), ==, 0);
    /*
     * Board-13 Status is not measured: this is the model's borrowed Octane
     * value, so the test pins the model choice, not a board read.
     */
    g_assert_cmphex(qpci_config_readw(d, PCI_STATUS), ==, 0x0480);
    rad_quit(&r);
}

static void test_bar0(void)
{
    Rad r = rad_boot();
    QPCIDevice *d = r.dev;
    uint32_t bar = qpci_config_readl(d, PCI_BASE_ADDRESS_0);

    /* One 32-bit non-prefetchable memory BAR, 0x2000 bytes (approximation). */
    g_assert_cmphex(r.barsize, ==, BAR0_SIZE);
    g_assert_cmphex(bar & 0xf, ==, 0);
    qpci_config_writel(d, PCI_BASE_ADDRESS_0, 0xffffffff);
    g_assert_cmphex(qpci_config_readl(d, PCI_BASE_ADDRESS_0), ==, 0xffffe000);
    qpci_config_writel(d, PCI_BASE_ADDRESS_0, bar);
    /* No other BAR and no expansion ROM. */
    for (unsigned i = 1; i < 6; i++) {
        qpci_config_writel(d, PCI_BASE_ADDRESS_0 + 4 * i, 0xffffffff);
        g_assert_cmphex(qpci_config_readl(d, PCI_BASE_ADDRESS_0 + 4 * i), ==,
                        0);
    }
    qpci_config_writel(d, PCI_ROM_ADDRESS, 0xfffff800);
    g_assert_cmphex(qpci_config_readl(d, PCI_ROM_ADDRESS), ==, 0);

    /* The driver maps 0x2000 bytes: the last documented word and past it. */
    wr(&r, REG_SUBCODE_LAST, 0x13579bdf);
    g_assert_cmphex(rd(&r, REG_SUBCODE_LAST), ==, 0x13579bdf);
    g_assert_cmphex(rd(&r, 0x1ff8), ==, 0);
    wr(&r, 0x1ff8, 0x1234);
    g_assert_cmphex(rd(&r, 0x1ff8), ==, 0);
    rad_quit(&r);
}

/*
 * The board-13 start-up list in the driver's order. The card stores the
 * words and acts on none of them: no channel starts, no position moves and
 * no status transfer begins. The board-12 values for the three registers
 * where the boards differ are stored the same way, because no source says a
 * register reads back differently, or decodes those bits, on either board.
 */
static const struct {
    unsigned off;
    uint32_t board13;
    uint32_t board12;
} startup[] = {
    { REG_80, 0xffffffff, 0xffffffff },
    { REG_80, 0xffe3cffe, 0xffe3cffe },
    { REG_80, 0xffe3c0fe, 0xffe3c0fe },
    { REG_80, 0xffe24070, 0xffe24070 },
    { REG_80, 0xffe20000, 0xffe20000 },
    { REG_74, 0x08000010, 0x08000010 },
    { REG_78, 0x00fac688, 0x00fac688 },
    { REG_4C, 0x05000000, 0x03000000 },
    { REG_58, 0x20000000, 0x20000000 },
    { REG_AES_IN_EN, 0x10, 0x10 },
    { REG_AES_OUT_EN, 0, 0x40000000 },
    { REG_ADAT_IN_EN, 0xa0000008, 0xa0000008 },
    { REG_ADAT_OUT_EN, 0x20000000, 0x60000000 },
    { REG_70, 0x1500, 0x1500 },
    { REG_B8, 0x9fffffff, 0x9fffffff },
    { REG_BC, 0x9fffffff, 0x9fffffff },
    { REG_A0, 0x02000001, 0x02000001 },
    { REG_7C, 0xd6d6d6d6, 0xd6d6d6d6 },
    { REG_84, 0, 0 },
    { REG_88, 0, 0 },
    { REG_8C, 0, 2 },
    { REG_90, 0, 2 },
};

static void startup_run(bool board13)
{
    Rad r = rad_boot();
    uint64_t ring = guest_alloc(&alloc, SLOT_BYTES);
    uint8_t seed[SLOT_BYTES], slot[SLOT_BYTES];

    memset(seed, SLOT_SENTINEL, sizeof(seed));
    qtest_memwrite(r.qts, ring, seed, sizeof(seed));

    for (unsigned i = 0; i < G_N_ELEMENTS(startup); i++) {
        uint32_t v = board13 ? startup[i].board13 : startup[i].board12;

        wr(&r, startup[i].off, v);
        g_assert_cmphex(rd(&r, startup[i].off), ==, v);
    }
    qtest_clock_step(r.qts, MS(5));
    for (unsigned off = REG_POS_FIRST; off <= REG_POS_LAST; off += 4) {
        g_assert_cmphex(rd(&r, off), ==, 0);
    }
    /* Nothing began a status transfer either. */
    slot_read(&r, ring, slot);
    g_assert_cmpmem(slot, sizeof(slot), seed, sizeof(seed));
    rad_quit(&r);
}

static void test_startup_board13(void)
{
    startup_run(true);
}

static void test_startup_board12_values(void)
{
    startup_run(false);
}

/*
 * Board 13 has no analog circuit: the analog input and output do not exist.
 * The same enable sequence that runs them on a board with analog audio (the
 * MediaIO daughtercard shares the engine) leaves them idle here: the
 * position registers stay zero, the pointer words stay unwritten and the
 * rings are untouched. The enable is observable as a LOG_UNIMP log, not
 * silently accepted. The digital channels still run (see test_positions).
 */
static void analog_run(const char *type, bool *moved)
{
    Rad r = rad_boot_with(type, "");
    uint64_t in_ring = guest_alloc(&alloc, 4096);
    uint64_t out_ring = guest_alloc(&alloc, 4096);
    uint64_t slot_addr = guest_alloc(&alloc, SLOT_BYTES);
    uint8_t seed[4096], got[4096], slot[SLOT_BYTES];

    memset(seed, 0x5a, sizeof(seed));
    qtest_memwrite(r.qts, in_ring, seed, sizeof(seed));
    qtest_memwrite(r.qts, out_ring, seed, sizeof(seed));
    memset(slot, SLOT_SENTINEL, sizeof(slot));
    qtest_memwrite(r.qts, slot_addr, slot, sizeof(slot));

    chan_start(&r, &ANALOG_IN, in_ring, 4096);
    chan_start(&r, &ANALOG_OUT, out_ring, 4096);
    qtest_clock_step(r.qts, MS(10));
    status_oneshot(&r, slot_addr);

    *moved = field(rd(&r, ANALOG_IN.pos_a), 18) ||
             field(rd(&r, ANALOG_OUT.pos_a), 18) ||
             field(rd(&r, ANALOG_IN.pos_b), 18);
    slot_read(&r, slot_addr, slot);
    *moved |= word(slot, ANALOG_IN.slot_ptr) != 0xa5a5a5a5 ||
              word(slot, ANALOG_OUT.slot_ptr) != 0xa5a5a5a5;
    qtest_memread(r.qts, in_ring, got, sizeof(got));
    *moved |= memcmp(got, seed, sizeof(got)) != 0;
    rad_quit(&r);
}

static void test_digital_only(void)
{
    bool moved;

    analog_run("sgi-rad1-audio", &moved);
    /* Control: the sequence is valid, it runs the analog pair elsewhere. */
    g_assert_true(moved);
    analog_run("sgi-rad1-pci", &moved);
    g_assert_false(moved);
}

static void test_no_mux_word(void)
{
    Rad r = rad_boot();
    QDict *rsp = qtest_qmp(r.qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/rad',"
        "'property':'mux-word'}}");

    /* The MediaIO mux serial word is not a property of this card. */
    g_assert_true(qdict_haskey(rsp, "error"));
    qobject_unref(rsp);
    rad_quit(&r);
}

static void test_ust(void)
{
    Rad r = rad_boot();
    uint32_t a, b;

    /* The liveness check: two reads 10 us apart differ by at least 3. */
    a = rd(&r, REG_UST);
    qtest_clock_step(r.qts, 10000);
    b = rd(&r, REG_UST);
    g_assert_cmpuint(b - a, >=, 3);

    /* Read-only: a write does not move or set the counter. */
    wr(&r, REG_UST, 0xdeadbeef);
    g_assert_cmpuint(rd(&r, REG_UST) - b, <=, 1);

    /*
     * The control word is stored and acts on nothing (approximation): the
     * counter stays monotonic across the 6, 2 reset sequence and the 4, 0
     * one.
     */
    qtest_clock_step(r.qts, MS(1));
    a = rd(&r, REG_UST);
    wr(&r, REG_UST_CONTROL, 6);
    g_assert_cmphex(rd(&r, REG_UST_CONTROL), ==, 6);
    g_assert_cmpuint(rd(&r, REG_UST), >=, a);
    wr(&r, REG_UST_CONTROL, 2);
    qtest_clock_step(r.qts, 20000);
    g_assert_cmpuint(rd(&r, REG_UST), >, a);
    a = rd(&r, REG_UST);
    wr(&r, REG_UST_CONTROL, 4);
    wr(&r, REG_UST_CONTROL, 0);
    g_assert_cmphex(rd(&r, REG_UST_CONTROL), ==, 0);
    g_assert_cmpuint(rd(&r, REG_UST), >=, a);
    rad_quit(&r);
}

static void test_status_oneshot(void)
{
    Rad r = rad_boot();
    uint64_t addr = guest_alloc(&alloc, SLOT_BYTES);
    uint8_t seed[SLOT_BYTES], slot[SLOT_BYTES];
    uint32_t ust;

    memset(seed, SLOT_SENTINEL, sizeof(seed));
    qtest_memwrite(r.qts, addr, seed, sizeof(seed));
    qtest_clock_step(r.qts, MS(1));

    status_oneshot(&r, addr);
    slot_read(&r, addr, slot);
    /* Complete, with no AES3 input and no ADAT: the unconnected default. */
    g_assert_cmphex(word(slot, SLOT_DONE), ==, DONE_BIT | AES_NO_INPUT);
    g_assert_cmphex(word(slot, SLOT_FLAGS), ==, 0);
    /* The rest of the slot was written: the register copy at +0x44..+0x74. */
    for (unsigned off = REG_POS_FIRST; off < REG_UST; off += 4) {
        g_assert_cmphex(word(slot, SLOT_MIRROR + off), ==, 0);
    }
    ust = word(slot, SLOT_MIRROR + REG_UST);
    g_assert_cmpuint(ust, ==, rd(&r, REG_UST));
    g_assert_cmpuint(ust, >=, 1000000 / 800);
    /* Channels that never ran leave their pointer words alone. */
    g_assert_cmphex(word(slot, AES_OUT.slot_ptr), ==, 0xa5a5a5a5);
    rad_quit(&r);
}

/*
 * The descriptor and status halves are written as two 32-bit registers and
 * name a 64-bit PCI address: widget in bits 63:60. A model that dropped the
 * high half would write the 32-bit address instead. The slot's low 32 bits
 * are also a valid transfer-memory address, so a truncating model lands
 * there; the right one lands at the same offset in the widget-1 target the
 * test maps at packet address 1 << 48.
 */
static void test_status_64bit(void)
{
    Rad r = rad_boot_with("sgi-rad1-pci",
        "-device sgi-xio-test-target,id=far,size=16M");
    uint64_t far = UINT64_C(1) << 48;
    uint64_t low = guest_alloc(&alloc, SLOT_BYTES);
    uint64_t addr = (UINT64_C(1) << 60) | low;
    uint8_t seed[SLOT_BYTES], slot[SLOT_BYTES];

    /* The target covers the same 16 MiB the transfer memory does. */
    qtest_sysbus_mmio_map(r.qts, "/machine/peripheral/far", 0,
                          far + 0x02000000);
    memset(seed, SLOT_SENTINEL, sizeof(seed));
    qtest_memwrite(r.qts, far + low, seed, sizeof(seed));
    qtest_memwrite(r.qts, low, seed, sizeof(seed));

    status_oneshot(&r, addr);
    slot_read(&r, far + low, slot);
    /*
     * The completion doubleword arrived whole. Which lane each byte takes is
     * the Bridge's placement for a 64-bit address, an open question for this
     * board, so only the multiset of bytes is checked: the done word's 0x80
     * and 0x08 and six zeros.
     */
    g_assert_cmphex(slot[SLOT_FLAGS + 0] | slot[SLOT_FLAGS + 1] |
                    slot[SLOT_FLAGS + 2] | slot[SLOT_FLAGS + 3] |
                    slot[SLOT_DONE + 0] | slot[SLOT_DONE + 1] |
                    slot[SLOT_DONE + 2] | slot[SLOT_DONE + 3], ==, 0x88);
    g_assert_cmpuint(memchr(slot + SLOT_FLAGS, 0x80, 8) != NULL, ==, 1);
    g_assert_cmpuint(memchr(slot + SLOT_FLAGS, 0xa5, 8) == NULL, ==, 1);
    /* Not truncated to the 32-bit address. */
    qtest_memread(r.qts, low, slot, sizeof(slot));
    g_assert_cmpmem(slot, sizeof(slot), seed, sizeof(seed));
    rad_quit(&r);
}

static void test_bus_master_gate(void)
{
    Rad r = rad_boot();
    uint64_t addr = guest_alloc(&alloc, SLOT_BYTES);
    uint8_t seed[SLOT_BYTES], slot[SLOT_BYTES];
    uint16_t cmd = qpci_config_readw(r.dev, PCI_COMMAND);

    memset(seed, SLOT_SENTINEL, sizeof(seed));
    qtest_memwrite(r.qts, addr, seed, sizeof(seed));

    /* Memory space on, bus master off: the card cannot reach host memory. */
    qpci_config_writew(r.dev, PCI_COMMAND, cmd & ~PCI_COMMAND_MASTER);
    status_oneshot(&r, addr);
    slot_read(&r, addr, slot);
    g_assert_cmpmem(slot, sizeof(slot), seed, sizeof(seed));

    qpci_config_writew(r.dev, PCI_COMMAND, cmd | PCI_COMMAND_MASTER);
    status_oneshot(&r, addr);
    slot_read(&r, addr, slot);
    g_assert_cmphex(word(slot, SLOT_DONE), ==, DONE_BIT | AES_NO_INPUT);
    rad_quit(&r);
}

/*
 * Continuous status with the driver's interval of 250: the first slot after
 * the start completes when the interval has elapsed, then one per interval.
 * Positions advance only while a channel runs. The driver clears each done
 * bit as it consumes the slot; this test does the same.
 */
static void test_status_continuous(void)
{
    enum { SLOTS = 4 };
    Rad r = rad_boot();
    uint64_t ring = guest_alloc(&alloc, SLOTS * SLOT_BYTES);
    uint64_t data = guest_alloc(&alloc, 4096);
    uint8_t zero[SLOTS * SLOT_BYTES] = { 0 }, slot[SLOT_BYTES];
    uint32_t last;

    qtest_memwrite(r.qts, ring, zero, sizeof(zero));
    wr(&r, REG_STATUS_LIMIT, 0x78 - SLOTS * SLOT_BYTES);
    wr(&r, REG_STATUS_LO, (uint32_t)ring);
    wr(&r, REG_STATUS_HI, ring >> 32);
    wr(&r, 0x4b4, (uint32_t)ring);
    wr(&r, 0x4b8, ring >> 32);
    wr(&r, 0x4bc, 0x78 - SLOTS * SLOT_BYTES);
    wr(&r, REG_STATUS_CTRL, STATUS_CONT_250);

    qtest_clock_step(r.qts, STATUS_INTERVAL_NS - 1000);
    slot_read(&r, ring + SLOT_BYTES, slot);
    g_assert_cmphex(word(slot, SLOT_DONE) & DONE_BIT, ==, 0);
    qtest_clock_step(r.qts, 2000);
    slot_read(&r, ring + SLOT_BYTES, slot);
    g_assert_cmphex(word(slot, SLOT_DONE) & DONE_BIT, ==, DONE_BIT);
    /* One slot per interval, in ring order, none early. */
    slot_read(&r, ring + 2 * SLOT_BYTES, slot);
    g_assert_cmphex(word(slot, SLOT_DONE) & DONE_BIT, ==, 0);
    qtest_clock_step(r.qts, STATUS_INTERVAL_NS);
    slot_read(&r, ring + 2 * SLOT_BYTES, slot);
    g_assert_cmphex(word(slot, SLOT_DONE) & DONE_BIT, ==, DONE_BIT);

    /* Idle: the AES3 output position copy stays zero while nothing runs. */
    g_assert_cmphex(word(slot, SLOT_MIRROR + AES_OUT.pos_a), ==, 0);
    chan_start(&r, &AES_OUT, data, 4096);
    qtest_clock_step(r.qts, STATUS_INTERVAL_NS * SLOTS);
    slot_read(&r, ring + 2 * SLOT_BYTES, slot);
    last = field(word(slot, SLOT_MIRROR + AES_OUT.pos_a), 18);
    g_assert_cmpuint(last, >, 0);

    /* Stopping the engine stops the completions. */
    wr(&r, REG_STATUS_CTRL, STATUS_STOP);
    qtest_memwrite(r.qts, ring, zero, sizeof(zero));
    qtest_clock_step(r.qts, STATUS_INTERVAL_NS * 3);
    qtest_memread(r.qts, ring, zero, sizeof(zero));
    for (unsigned i = 0; i < SLOTS; i++) {
        g_assert_cmphex(word(zero + i * SLOT_BYTES, SLOT_DONE), ==, 0);
    }
    rad_quit(&r);
}

/*
 * Position encoding. Each position is a 14-bit frame count at shift 18
 * (bits 31:18), wrapping; a subcode channel's own count is the A register's
 * bits 17:4, and the parent's field in bits 31:18 of the same word is not
 * touched. The BAR0 bank and the status-slot copy agree at the same instant.
 */
static void test_positions(void)
{
    Rad r = rad_boot();
    uint64_t aes = guest_alloc(&alloc, 4096);
    uint64_t adat = guest_alloc(&alloc, 4096);
    uint64_t aes_in = guest_alloc(&alloc, 4096);
    uint64_t slot_addr = guest_alloc(&alloc, SLOT_BYTES);
    uint8_t slot[SLOT_BYTES];

    chan_start(&r, &AES_OUT, aes, 4096);
    chan_start(&r, &ADAT_OUT, adat, 4096);
    chan_start(&r, &AES_IN, aes_in, 4096);

    /* 10 ms at 44.1 kHz is 441 frames. */
    qtest_clock_step(r.qts, MS(10));
    g_assert_cmpuint(field(rd(&r, AES_OUT.pos_a), 18), ==, 441);
    g_assert_cmpuint(field(rd(&r, AES_OUT.pos_b), 18), ==, 441);
    g_assert_cmpuint(field(rd(&r, ADAT_OUT.pos_a), 18), ==, 441);
    g_assert_cmpuint(field(rd(&r, ADAT_OUT.pos_b), 18), ==, 441);
    /* The AES3 input follows the 48 kHz default receiver clock: 480. */
    g_assert_cmpuint(field(rd(&r, AES_IN.pos_a), 18), ==, 480);
    g_assert_cmpuint(field(rd(&r, AES_IN.pos_b), 18), ==, 480);

    /* The pointer word is the ring base plus the frames moved. */
    status_oneshot(&r, slot_addr);
    slot_read(&r, slot_addr, slot);
    g_assert_cmphex(word(slot, AES_OUT.slot_ptr), ==,
                    (uint32_t)aes + (441 * 8) % 4096);
    g_assert_cmphex(word(slot, ADAT_OUT.slot_ptr), ==,
                    (uint32_t)adat + (441 * 32) % 4096);
    /* Both paths of the position read give the same words. */
    for (unsigned off = REG_POS_FIRST; off <= REG_POS_LAST; off += 4) {
        g_assert_cmphex(word(slot, SLOT_MIRROR + off), ==, rd(&r, off));
    }
    /* The analog positions do not move. */
    g_assert_cmphex(rd(&r, ANALOG_IN.pos_a), ==, 0);
    g_assert_cmphex(rd(&r, ANALOG_OUT.pos_a), ==, 0);

    /* 400 ms in all is 17640 frames: the 14-bit field has wrapped. */
    qtest_clock_step(r.qts, MS(390));
    g_assert_cmpuint(17640 & 0x3fff, ==, 1256);
    g_assert_cmpuint(field(rd(&r, AES_OUT.pos_a), 18), ==, 1256);
    g_assert_cmpuint(field(rd(&r, AES_OUT.pos_b), 18), ==, 1256);
    g_assert_cmpuint(field(rd(&r, AES_IN.pos_a), 18), ==, 19200 & 0x3fff);

    /* A stopped channel holds its position. */
    chan_stop(&r, &AES_OUT);
    chan_stop(&r, &ADAT_OUT);
    qtest_clock_step(r.qts, MS(10));
    g_assert_cmpuint(field(rd(&r, AES_OUT.pos_a), 18), ==, 1256);
    rad_quit(&r);
}

static void test_subcode_positions(void)
{
    Rad r = rad_boot();
    uint64_t aes = guest_alloc(&alloc, 1024);
    uint64_t adat = guest_alloc(&alloc, 1024);

    chan_start(&r, &AES_SUB, aes, 1024);
    chan_start(&r, &ADAT_SUB, adat, 1024);
    qtest_clock_step(r.qts, MS(10));

    /*
     * One record per parent frame at 48 kHz (a model policy, flagged in the
     * device): 10 ms is 480. The count is in bits 17:4 (shift 4), and
     * bits 31:18, the parent input's B field, stay zero.
     */
    g_assert_cmpuint(field(rd(&r, AES_SUB.pos_a), 4), ==, 480);
    g_assert_cmpuint(rd(&r, AES_SUB.pos_a) >> 18, ==, 0);
    g_assert_cmpuint(field(rd(&r, ADAT_SUB.pos_a), 4), ==, 480);
    g_assert_cmpuint(rd(&r, ADAT_SUB.pos_a) >> 18, ==, 0);
    rad_quit(&r);
}

/*
 * Reset returns the stored words to zero, stops the channels and the status
 * engine, and restarts the UST from zero.
 */
static void test_reset(void)
{
    Rad r = rad_boot();
    uint64_t ring = guest_alloc(&alloc, SLOT_BYTES);
    uint64_t data = guest_alloc(&alloc, 4096);
    uint8_t zero[SLOT_BYTES] = { 0 }, slot[SLOT_BYTES];

    wr(&r, REG_BC, 0x9fffffff);
    wr(&r, REG_UST_CONTROL, 6);
    chan_start(&r, &AES_OUT, data, 4096);
    qtest_memwrite(r.qts, ring, zero, sizeof(zero));
    wr(&r, REG_STATUS_LIMIT, 0x78 - SLOT_BYTES);
    wr(&r, REG_STATUS_LO, (uint32_t)ring);
    wr(&r, REG_STATUS_HI, ring >> 32);
    wr(&r, REG_STATUS_CTRL, STATUS_CONT_250);
    qtest_clock_step(r.qts, MS(2));
    g_assert_cmpuint(field(rd(&r, AES_OUT.pos_a), 18), >, 0);

    qtest_system_reset(r.qts);
    rad_enable(&r);
    g_assert_cmphex(rd(&r, REG_BC), ==, 0);
    g_assert_cmphex(rd(&r, REG_UST_CONTROL), ==, 0);
    g_assert_cmphex(rd(&r, AES_OUT.a_lo), ==, 0);
    g_assert_cmphex(rd(&r, REG_STATUS_CTRL), ==, 0);
    /* The UST restarted from zero. */
    g_assert_cmpuint(rd(&r, REG_UST), <=, 1);
    qtest_clock_step(r.qts, MS(5));
    for (unsigned off = REG_POS_FIRST; off <= REG_POS_LAST; off += 4) {
        g_assert_cmphex(rd(&r, off), ==, 0);
    }
    /* No completion arrives after reset. */
    qtest_memwrite(r.qts, ring, zero, sizeof(zero));
    qtest_clock_step(r.qts, MS(2));
    slot_read(&r, ring, slot);
    g_assert_cmpmem(slot, sizeof(slot), zero, sizeof(zero));
    rad_quit(&r);
}

/*
 * A running channel and the stored words travel; the UST control word, which
 * has a subsection of its own, arrives with them.
 */
static void test_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("rad1-pci-migrate-XXXXXX", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    Rad from = rad_boot(), to = { 0 };
    uint64_t data = guest_alloc(&alloc, 4096);
    uint32_t pos;

    wr(&from, REG_BC, 0x9fffffff);
    wr(&from, REG_UST_CONTROL, 6);
    chan_start(&from, &AES_OUT, data, 4096);
    qtest_clock_step(from.qts, MS(10));
    pos = field(rd(&from, AES_OUT.pos_a), 18);
    g_assert_cmpuint(pos, ==, 441);

    to.qts = qtest_initf("-M none " QPCI_SGI_BRIDGE_HEAD
                         "-device sgi-rad1-pci,id=rad,addr=2 "
                         "-incoming %s", uri);
    qtest_qmp_assert_success(from.qts, "{ 'execute': 'stop' }");
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    to.bus = qpci_init_sgi_bridge(to.qts, &alloc);
    sgi_migrate_resume(to.qts);
    to.dev = qpci_device_find(to.bus, QPCI_DEVFN(2, 0));
    g_assert_nonnull(to.dev);
    /* Already enabled and placed: the migrated config space says where. */
    to.bar = from.bar;

    g_assert_cmphex(rd(&to, REG_BC), ==, 0x9fffffff);
    g_assert_cmphex(rd(&to, REG_UST_CONTROL), ==, 6);
    g_assert_cmphex(qpci_config_readb(to.dev, PCI_REVISION_ID), ==,
                    BOARD13_REVISION);
    g_assert_cmpuint(field(rd(&to, AES_OUT.pos_a), 18), ==, pos);
    /*
     * A qtest destination's virtual clock restarts from zero, and the
     * channel's epoch travels with it, so it counts again once the clock has
     * passed the 10 ms already played: 20 ms is 882 frames.
     */
    qtest_clock_step(to.qts, MS(20));
    g_assert_cmpuint(field(rd(&to, AES_OUT.pos_a), 18), ==, 882);

    g_free(from.dev);
    qpci_free_sgi_bridge(from.bus);
    qtest_quit(from.qts);
    g_free(to.dev);
    qpci_free_sgi_bridge(to.bus);
    qtest_quit(to.qts);
    alloc_destroy(&alloc);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sgi-rad1-pci/config-identity", test_config_identity);
    qtest_add_func("/sgi-rad1-pci/bar0", test_bar0);
    qtest_add_func("/sgi-rad1-pci/startup-board13", test_startup_board13);
    qtest_add_func("/sgi-rad1-pci/startup-board12-values",
                   test_startup_board12_values);
    qtest_add_func("/sgi-rad1-pci/digital-only", test_digital_only);
    qtest_add_func("/sgi-rad1-pci/no-mux-word", test_no_mux_word);
    qtest_add_func("/sgi-rad1-pci/ust", test_ust);
    qtest_add_func("/sgi-rad1-pci/status-oneshot", test_status_oneshot);
    qtest_add_func("/sgi-rad1-pci/status-64bit", test_status_64bit);
    qtest_add_func("/sgi-rad1-pci/bus-master-gate", test_bus_master_gate);
    qtest_add_func("/sgi-rad1-pci/status-continuous",
                   test_status_continuous);
    qtest_add_func("/sgi-rad1-pci/positions", test_positions);
    qtest_add_func("/sgi-rad1-pci/subcode-positions", test_subcode_positions);
    qtest_add_func("/sgi-rad1-pci/reset", test_reset);
    qtest_add_func("/sgi-rad1-pci/migration", test_migration);

    return g_test_run();
}
