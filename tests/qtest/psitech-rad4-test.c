/*
 * PsiTech RAD4 (psitech-rad4): register contract for the reviewed one-mode
 * functional approximation, through the real Origin 200 Bridge PCI path.
 *
 * The card is an add-in PCI card, so like tests/qtest/sn0-tigon-test.c it is
 * instantiated with -device at a free slot and reached through that slot's
 * own Bridge decode. Expected values come from
 * docs/designs/rad4-one-mode-functional-approximation.md in the owning
 * Origami repository, not from this device's own implementation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#include "psitech-rad4-mode0-fixture.h"

/*
 * PCI type 0 configuration space offsets, restated the way
 * tests/qtest/sn0-tigon-test.c restates its own.
 */
#define PCI_VENDOR_ID_OFF   0x00
#define PCI_REVISION_ID_OFF 0x08
#define PCI_CLASS_DEVICE_OFF 0x0a
#define PCI_BAR0_OFF        0x10
#define PCI_BAR2_CFG_OFF    0x18
#define PCI_ROM_ADDRESS_OFF 0x30
#define PCI_COMMAND_OFF     0x04
#define PCI_COMMAND_MEMORY  0x0002u

/* Crosstalk space and the Bridge's widget window inside it. */
#define SPACE_XTALK     0x20000000000ULL
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

/*
 * Devices 0 and 1 are BaseIO's SCSI adapters and 2 is IOC3; 3 is the first
 * free slot, with the first of the six 1 MiB windows beyond the two 2 MiB
 * ones -- the same slot tests/qtest/sn0-tigon-test.c uses.
 */
#define RAD4_SLOT       3
#define WINDOW_RAD4     (BRIDGE_BASE + 0x700000)
#define WINDOW_SIZE     0x100000u

#define BRIDGE_CFG0     0x020000
#define CFG0_STRIDE     0x1000
#define CFG0(dev)       (BRIDGE_BASE + BRIDGE_CFG0 + (dev) * CFG0_STRIDE)

#define REG_DATA            4
#define BRIDGE_DEVICE(n)    (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)
#define DEV_MEMORY_SPACE    (1u << 12)

#define PCI_ROM_ENABLE      1u

#define MACHINE_WITH_BRIDGE \
    "-M origin200 -device psitech-rad4,addr=" G_STRINGIFY(RAD4_SLOT)

/*
 * PCI addresses this test assigns the card's apertures; 1 MiB aligned so
 * BAR0 and the ROM route never need the window to slide, and BAR2's base is
 * 1 MiB aligned so the offsets below compose with it directly.
 */
#define RAD4_BAR0_PCI   0x20000000u
#define RAD4_BAR2_PCI   0x30000000u
#define RAD4_ROM_PCI    0x40000000u

#define RAD4_VENDOR_ID  0x12d1
#define RAD4_DEVICE_ID  0x0104

/* BAR0 offsets. */
#define R_REG14         0x14
#define R_REG18         0x18
#define R_MAILBOX       0x40
#define R_INTCSR        0x68
#define R_SERIAL        0x6c

#define INTCSR_PENDING  (1u << 15)
#define INTCSR_ENABLE   (1u << 8)

#define SERIAL_CLOCK    (1u << 24)
#define SERIAL_CS       (1u << 25)
#define SERIAL_MOSI     (1u << 26)
#define SERIAL_MISO     (1u << 27)

/* BAR2 offsets. */
#define B_RGB_LO        0x900000
#define B_RGB_HI        0x910000
#define B_RGB_DATA      0x920000
#define B_RGB_RESERVED  0x930000
#define B_HORIZ0        0xa00000
#define B_VERT          0xb00000
#define B_MASK          0xd00000
#define B_CTRL          0xf00000

#define CTRL_MODE_CMD   0x20u
#define CTRL_STORED_BIT 0x02u
#define STATUS_FRAME    0x20u

/*
 * RGB561 indexed cursor registers (docs/datasheets/psitech-rad4.md,
 * "Cursor"; IBM Microelectronics "RGB561 Architecture", Preliminary
 * Rev. 1.1, pages 27 and 37-39, 45).
 */
#define RGB_CURSOR_CONTROL      0x0030
#define RGB_CURSOR_HOTSPOT_X    0x0034
#define RGB_CURSOR_HOTSPOT_Y    0x0035
#define RGB_CURSOR_X_LOW        0x0036
#define RGB_CURSOR_X_HIGH       0x0037
#define RGB_CURSOR_Y_LOW        0x0038
#define RGB_CURSOR_Y_HIGH       0x0039
#define CURSOR_CONTROL_CE       (1u << 0)

#define RGB_CURSOR_LUT_BASE     0x0a10
#define RGB_CURSOR_PIXMAP_BASE  0x2000

static void rad4_window_to(QTestState *qts, uint32_t pci_addr)
{
    uint32_t base = pci_addr & ~(WINDOW_SIZE - 1);

    qtest_writel(qts, BRIDGE_DEVICE(RAD4_SLOT), DEV_MEMORY_SPACE |
                (base >> 20));
}

static uint32_t rad4_readl(QTestState *qts, uint32_t pci_addr)
{
    rad4_window_to(qts, pci_addr);
    return qtest_readl(qts, WINDOW_RAD4 + (pci_addr & (WINDOW_SIZE - 1)));
}

static void rad4_writel(QTestState *qts, uint32_t pci_addr, uint32_t val)
{
    rad4_window_to(qts, pci_addr);
    qtest_writel(qts, WINDOW_RAD4 + (pci_addr & (WINDOW_SIZE - 1)), val);
}

static uint32_t bar0_read(QTestState *qts, uint32_t off)
{
    return rad4_readl(qts, RAD4_BAR0_PCI + off);
}

static void bar0_write(QTestState *qts, uint32_t off, uint32_t val)
{
    rad4_writel(qts, RAD4_BAR0_PCI + off, val);
}

static uint32_t bar2_read(QTestState *qts, uint32_t off)
{
    return rad4_readl(qts, RAD4_BAR2_PCI + off);
}

static void bar2_write(QTestState *qts, uint32_t off, uint32_t val)
{
    rad4_writel(qts, RAD4_BAR2_PCI + off, val);
}

/* Points the RGB561 index registers at a given 16-bit index. */
static void rgb_set_index(QTestState *qts, uint16_t index)
{
    bar2_write(qts, B_RGB_LO, index & 0xff);
    bar2_write(qts, B_RGB_HI, (index >> 8) & 0xff);
}

static uint16_t rgb_current_index(QTestState *qts)
{
    uint8_t lo = bar2_read(qts, B_RGB_LO);
    uint8_t hi = bar2_read(qts, B_RGB_HI);

    return ((uint16_t)hi << 8) | lo;
}

/* Ordinary single-cycle RGB561 registers: one byte per addressed index. */
static void rgb_write_byte(QTestState *qts, uint16_t index, uint8_t value)
{
    rgb_set_index(qts, index);
    bar2_write(qts, B_RGB_DATA, value);
}

static uint8_t rgb_read_byte(QTestState *qts, uint16_t index)
{
    rgb_set_index(qts, index);
    return (uint8_t)bar2_read(qts, B_RGB_DATA);
}

/*
 * Programs the card's BARs and expansion-ROM route into PCI space and opens
 * the Bridge slot's memory decode, the way tests/qtest/sn0-tigon-test.c sets
 * up its own single BAR. A PCI reset (cold boot or an explicit system
 * reset) clears these config-space registers back to unprogrammed, exactly
 * as real firmware would find them, so this must run again after any
 * reset before the card answers PIO again.
 */
static void rad4_configure_bars(QTestState *qts)
{
    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_BAR0_OFF, RAD4_BAR0_PCI);
    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_BAR2_CFG_OFF, RAD4_BAR2_PCI);
    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_ROM_ADDRESS_OFF,
                RAD4_ROM_PCI | PCI_ROM_ENABLE);
    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_COMMAND_OFF, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(RAD4_SLOT),
                DEV_MEMORY_SPACE | (RAD4_BAR0_PCI >> 20));
}

static QTestState *rad4_init(void)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    rad4_configure_bars(qts);
    return qts;
}

static void test_pci_identity(void)
{
    QTestState *qts = rad4_init();
    uint32_t id = qtest_readl(qts, CFG0(RAD4_SLOT) + PCI_VENDOR_ID_OFF);

    g_assert_cmphex(id & 0xffff, ==, RAD4_VENDOR_ID);
    g_assert_cmphex(id >> 16, ==, RAD4_DEVICE_ID);

    /*
     * Read revision/prog-if/subclass/base-class as one 32-bit word: the
     * Bridge's config-space window only answers aligned 32-bit accesses
     * cleanly (docs/comment-style.md, "Access width").
     */
    uint32_t rev_class = qtest_readl(qts,
                                     CFG0(RAD4_SLOT) + PCI_REVISION_ID_OFF);
    g_assert_cmphex(rev_class & 0xff, ==, 0x03);         /* revision */
    g_assert_cmphex((rev_class >> 8) & 0xff, ==, 0x00);  /* prog-if */
    g_assert_cmphex((rev_class >> 16) & 0xff, ==, 0x80); /* subclass */
    g_assert_cmphex((rev_class >> 24) & 0xff, ==, 0x03); /* base class */

    /* BAR0 sizing: 128 bytes. */
    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_BAR0_OFF, 0xffffffffu);
    uint32_t bar0_size_mask = qtest_readl(qts, CFG0(RAD4_SLOT) + PCI_BAR0_OFF);
    g_assert_cmphex(bar0_size_mask & 0xfffffff0u, ==,
                    (uint32_t)-128 & 0xfffffff0u);
    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_BAR0_OFF, RAD4_BAR0_PCI);

    /* BAR2 sizing: 16 MiB. */
    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_BAR2_CFG_OFF, 0xffffffffu);
    uint32_t bar2_size_mask = qtest_readl(qts,
                                          CFG0(RAD4_SLOT) + PCI_BAR2_CFG_OFF);
    g_assert_cmphex(bar2_size_mask & 0xfffffff0u, ==,
                    (uint32_t)-(16 * 1024 * 1024) & 0xfffffff0u);
    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_BAR2_CFG_OFF, RAD4_BAR2_PCI);

    qtest_quit(qts);
}

static void test_bar0_registers(void)
{
    QTestState *qts = rad4_init();

    g_assert_cmphex(bar0_read(qts, R_REG14), ==, 0);
    bar0_write(qts, R_REG14, 0xe1000000u);
    g_assert_cmphex(bar0_read(qts, R_REG14), ==, 0xe1000000u);

    g_assert_cmphex(bar0_read(qts, R_MAILBOX), ==, 0);
    bar0_write(qts, R_MAILBOX, 0x12345678u);
    g_assert_cmphex(bar0_read(qts, R_MAILBOX), ==, 0x12345678u);

    /* Bit 15 (pending) is device-owned: a guest write cannot set it. */
    bar0_write(qts, R_INTCSR, 0xffffffffu);
    g_assert_cmphex(bar0_read(qts, R_INTCSR) & INTCSR_PENDING, ==, 0);
    g_assert_cmphex(bar0_read(qts, R_INTCSR) & INTCSR_ENABLE, ==,
                    INTCSR_ENABLE);

    qtest_quit(qts);
}

/*
 * Clocks one bit MSB-first on SERIAL_CLOCK's rising edge, matching the
 * driver's own bit-bang convention: outgoing data is stable before the
 * rising edge, incoming data is read after the falling edge.
 */
static bool serial_clock_bit(QTestState *qts, uint32_t *ctrl, bool mosi)
{
    uint32_t base = (*ctrl & ~(SERIAL_CLOCK | SERIAL_MOSI)) |
                    (mosi ? SERIAL_MOSI : 0);

    bar0_write(qts, R_SERIAL, base);                /* clock low, set MOSI */
    bar0_write(qts, R_SERIAL, base | SERIAL_CLOCK);  /* rising: sample MOSI */
    bar0_write(qts, R_SERIAL, base);                 /* falling: update MISO */
    uint32_t val = bar0_read(qts, R_SERIAL);
    *ctrl = base;
    return (val & SERIAL_MISO) != 0;
}

static void test_serial_eeprom_valid_command(void)
{
    QTestState *qts = rad4_init();
    uint32_t ctrl = SERIAL_CS;

    bar0_write(qts, R_SERIAL, ctrl);

    /* Start bit 1, then command byte 0xac, MSB first. */
    static const int command_bits[9] = { 1, 1, 0, 1, 0, 1, 1, 0, 0 };
    for (int i = 0; i < 9; i++) {
        serial_clock_bit(qts, &ctrl, command_bits[i]);
    }

    /* Acknowledgement bit reads zero. */
    bool ack = serial_clock_bit(qts, &ctrl, 0);
    g_assert_false(ack);

    /* All 320 payload bits (40 zero configuration bytes) read zero. */
    for (int i = 0; i < 320; i++) {
        bool bit = serial_clock_bit(qts, &ctrl, 0);
        g_assert_false(bit);
    }

    /* Deassert chip select cleanly at the end of the transaction. */
    bar0_write(qts, R_SERIAL, ctrl & ~SERIAL_CS);

    qtest_quit(qts);
}

static void test_serial_eeprom_bad_command(void)
{
    QTestState *qts = rad4_init();
    uint32_t ctrl = SERIAL_CS;

    bar0_write(qts, R_SERIAL, ctrl);

    /* A command that is not "start bit, then 0xac". */
    static const int bad_bits[9] = { 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 9; i++) {
        serial_clock_bit(qts, &ctrl, bad_bits[i]);
    }

    /* Any other command reads incoming one until cold reset. */
    bool bit = serial_clock_bit(qts, &ctrl, 0);
    g_assert_true(bit);

    qtest_quit(qts);
}

static void test_framebuffer_mask(void)
{
    QTestState *qts = rad4_init();

    /* Full mask by default: an ordinary pixel write is stored whole. */
    bar2_write(qts, 0x000000, 0x00ff0080u);
    g_assert_cmphex(bar2_read(qts, 0x000000), ==, 0x00ff0080u);

    /*
     * A partial mask is a plane mask: only the masked bits of the
     * write take effect, the rest of the existing pixel is preserved.
     */
    bar2_write(qts, B_MASK, 0x0000ffffu);
    bar2_write(qts, 0x000004, 0x00112233u);
    g_assert_cmphex(bar2_read(qts, 0x000004), ==, 0x00002233u);

    /* Restoring the full mask lets a write replace the whole pixel. */
    bar2_write(qts, B_MASK, 0xffffffffu);
    bar2_write(qts, 0x000004, 0x00112233u);
    g_assert_cmphex(bar2_read(qts, 0x000004), ==, 0x00112233u);

    qtest_quit(qts);
}

/* Replays the exact reviewed mode-zero write sequence, in order. */
static void rad4_play_mode0(QTestState *qts)
{
    for (int i = 0; i < RAD4_MODE0_EVENT_COUNT; i++) {
        bar2_write(qts, rad4_mode0_events[i].bar2_offset,
                  rad4_mode0_events[i].value);
    }
}

static void test_mode_recognition(void)
{
    QTestState *qts = rad4_init();

    rad4_play_mode0(qts);

    /*
     * Recognized: the frame timer starts, so a virtual second later at
     * least one frame event has fired.
     */
    qtest_clock_step(qts, 1000000000);
    uint32_t status = bar2_read(qts, B_CTRL);
    g_assert_cmphex(status & STATUS_FRAME, ==, STATUS_FRAME);
    g_assert_cmphex(bar0_read(qts, R_INTCSR) & INTCSR_PENDING, ==,
                    INTCSR_PENDING);

    qtest_quit(qts);
}

static void test_mode_recognition_rejects_mutation(void)
{
    QTestState *qts = rad4_init();

    for (int i = 0; i < RAD4_MODE0_EVENT_COUNT; i++) {
        uint32_t value = rad4_mode0_events[i].value;

        /* Corrupt the very last vertical-bank word. */
        if (i == 5 + 1064) {
            value ^= 1;
        }
        bar2_write(qts, rad4_mode0_events[i].bar2_offset, value);
    }

    qtest_clock_step(qts, 1000000000);
    uint32_t status = bar2_read(qts, B_CTRL);
    g_assert_cmphex(status & STATUS_FRAME, ==, 0);

    qtest_quit(qts);
}

static void test_interrupt_acknowledge(void)
{
    QTestState *qts = rad4_init();

    rad4_play_mode0(qts);
    bar0_write(qts, R_INTCSR, INTCSR_ENABLE);

    qtest_clock_step(qts, 1000000000 / 60 + 1000);
    g_assert_cmphex(bar0_read(qts, R_INTCSR) & INTCSR_PENDING, ==,
                    INTCSR_PENDING);

    /*
     * Acknowledge: a control write with bit 1 clear while a frame is
     * pending clears both status bits.
     */
    bar2_write(qts, B_CTRL, 0);
    g_assert_cmphex(bar0_read(qts, R_INTCSR) & INTCSR_PENDING, ==, 0);
    g_assert_cmphex(bar2_read(qts, B_CTRL) & STATUS_FRAME, ==, 0);

    qtest_quit(qts);
}

static void test_dma_registers_inert(void)
{
    QTestState *qts = rad4_init();

    bar2_write(qts, 0x100000, 0x12345678u); /* +0x100 through the ROM route */
    rad4_writel(qts, RAD4_ROM_PCI + 0x100, 0xaabbccddu);
    g_assert_cmphex(rad4_readl(qts, RAD4_ROM_PCI + 0x100), ==, 0xaabbccddu);
    /*
     * No pixel, interrupt or completion effect: the framebuffer at offset
     * zero is untouched by this store.
     */
    g_assert_cmphex(bar2_read(qts, 0x000000), ==, 0);
    g_assert_cmphex(bar0_read(qts, R_INTCSR) & INTCSR_PENDING, ==, 0);

    qtest_quit(qts);
}

static void test_cursor_position_and_enable(void)
{
    QTestState *qts = rad4_init();

    /* Cursor control: CE (bit 0) is the enable bit (IBM page 37). */
    rgb_write_byte(qts, RGB_CURSOR_CONTROL, CURSOR_CONTROL_CE);
    g_assert_cmphex(rgb_read_byte(qts, RGB_CURSOR_CONTROL), ==,
                    CURSOR_CONTROL_CE);

    /* Position: an ordinary 16-bit two's complement pair (IBM page 38). */
    rgb_write_byte(qts, RGB_CURSOR_X_LOW, 100);
    rgb_write_byte(qts, RGB_CURSOR_X_HIGH, 0);
    rgb_write_byte(qts, RGB_CURSOR_Y_LOW, 200);
    rgb_write_byte(qts, RGB_CURSOR_Y_HIGH, 0);

    g_assert_cmphex(rgb_read_byte(qts, RGB_CURSOR_X_LOW), ==, 100);
    g_assert_cmphex(rgb_read_byte(qts, RGB_CURSOR_X_HIGH), ==, 0);
    g_assert_cmphex(rgb_read_byte(qts, RGB_CURSOR_Y_LOW), ==, 200);
    g_assert_cmphex(rgb_read_byte(qts, RGB_CURSOR_Y_HIGH), ==, 0);

    /* Hot spot: six bits (IBM page 38). */
    rgb_write_byte(qts, RGB_CURSOR_HOTSPOT_X, 0x3f);
    rgb_write_byte(qts, RGB_CURSOR_HOTSPOT_Y, 0x01);
    g_assert_cmphex(rgb_read_byte(qts, RGB_CURSOR_HOTSPOT_X), ==, 0x3f);
    g_assert_cmphex(rgb_read_byte(qts, RGB_CURSOR_HOTSPOT_Y), ==, 0x01);

    /* Disabling again clears the enable bit but not the position. */
    rgb_write_byte(qts, RGB_CURSOR_CONTROL, 0);
    g_assert_cmphex(rgb_read_byte(qts, RGB_CURSOR_CONTROL), ==, 0);
    g_assert_cmphex(rgb_read_byte(qts, RGB_CURSOR_X_LOW), ==, 100);

    qtest_quit(qts);
}

static void test_cursor_pixmap_round_trip(void)
{
    QTestState *qts = rad4_init();

    /*
     * The cursor pixel map is a plain 1Kx8 SRAM (IBM page 27 lists it
     * without the "N RW/ADDR" annotation the look-up tables carry), so
     * writing it goes through the ordinary auto-incrementing data port
     * and advances the index by exactly one per byte.
     */
    rgb_set_index(qts, RGB_CURSOR_PIXMAP_BASE);
    bar2_write(qts, B_RGB_DATA, 0x1b);
    bar2_write(qts, B_RGB_DATA, 0xe4);
    g_assert_cmphex(rgb_current_index(qts), ==, RGB_CURSOR_PIXMAP_BASE + 2);

    g_assert_cmphex(rgb_read_byte(qts, RGB_CURSOR_PIXMAP_BASE), ==, 0x1b);
    g_assert_cmphex(rgb_read_byte(qts, RGB_CURSOR_PIXMAP_BASE + 1), ==, 0xe4);

    /* A byte at the far end of the 1 KiB pixel map round-trips too. */
    rgb_write_byte(qts, RGB_CURSOR_PIXMAP_BASE + 0x3ff, 0x55);
    g_assert_cmphex(rgb_read_byte(qts, RGB_CURSOR_PIXMAP_BASE + 0x3ff), ==,
                    0x55);

    qtest_quit(qts);
}

static void test_cursor_lut_tri_write(void)
{
    QTestState *qts = rad4_init();

    /*
     * The cursor/cross-hair look-up table is a three-writes-per-address
     * ("3 RW/ADDR") port reached through BAR2 +0x930000, not the ordinary
     * data port: point the index at Cursor Primary Color 1 (IBM page 45,
     * Table 22) and issue three writes there without touching the index
     * registers in between, matching the shipped driver's own
     * init_RGB561 sequence.
     */
    rgb_set_index(qts, RGB_CURSOR_LUT_BASE + 1);
    bar2_write(qts, B_RGB_RESERVED, 0x11);
    bar2_write(qts, B_RGB_RESERVED, 0x22);
    bar2_write(qts, B_RGB_RESERVED, 0x33);

    /*
     * The visible index must have advanced by exactly one, to the next
     * LUT entry -- not by three, the way an ordinary auto-incrementing
     * range would. This is what discriminates this port's
     * three-writes-per-address protocol from plain per-byte storage.
     */
    g_assert_cmphex(rgb_current_index(qts), ==, RGB_CURSOR_LUT_BASE + 2);

    /*
     * Reading the three components back in order, at the original
     * address, confirms they landed in three separate planes of one
     * entry rather than at three separate addresses.
     */
    rgb_set_index(qts, RGB_CURSOR_LUT_BASE + 1);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0x11);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0x22);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0x33);
    g_assert_cmphex(rgb_current_index(qts), ==, RGB_CURSOR_LUT_BASE + 2);

    /* The next entry (Cursor Primary Color 2) was left untouched. */
    rgb_set_index(qts, RGB_CURSOR_LUT_BASE + 2);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0);

    /* This port never reaches the plain framebuffer or interrupt path. */
    g_assert_cmphex(bar2_read(qts, 0x000000), ==, 0);
    g_assert_cmphex(bar0_read(qts, R_INTCSR) & INTCSR_PENDING, ==, 0);

    qtest_quit(qts);
}

static void test_reset(void)
{
    QTestState *qts = rad4_init();

    rad4_play_mode0(qts);
    bar0_write(qts, R_MAILBOX, 0x55aa55aau);
    bar2_write(qts, 0x000000, 0x00ff00ffu);

    qtest_system_reset(qts);
    rad4_configure_bars(qts);

    /*
     * The card's cold BAR0 mailbox and framebuffer must read the reset
     * state once its BARs are reprogrammed.
     */
    g_assert_cmphex(bar0_read(qts, R_MAILBOX), ==, 0);
    g_assert_cmphex(bar2_read(qts, 0x000000), ==, 0);
    g_assert_cmphex(bar2_read(qts, B_MASK), ==, 0xffffffffu);

    qtest_clock_step(qts, 1000000000);
    g_assert_cmphex(bar2_read(qts, B_CTRL) & STATUS_FRAME, ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/psitech-rad4/pci-identity", test_pci_identity);
    qtest_add_func("/psitech-rad4/bar0-registers", test_bar0_registers);
    qtest_add_func("/psitech-rad4/serial-eeprom-valid",
                   test_serial_eeprom_valid_command);
    qtest_add_func("/psitech-rad4/serial-eeprom-bad",
                   test_serial_eeprom_bad_command);
    qtest_add_func("/psitech-rad4/framebuffer-mask", test_framebuffer_mask);
    qtest_add_func("/psitech-rad4/mode-recognition", test_mode_recognition);
    qtest_add_func("/psitech-rad4/mode-recognition-mutation",
                   test_mode_recognition_rejects_mutation);
    qtest_add_func("/psitech-rad4/interrupt-acknowledge",
                   test_interrupt_acknowledge);
    qtest_add_func("/psitech-rad4/dma-registers-inert",
                   test_dma_registers_inert);
    qtest_add_func("/psitech-rad4/cursor-position-and-enable",
                   test_cursor_position_and_enable);
    qtest_add_func("/psitech-rad4/cursor-pixmap-round-trip",
                   test_cursor_pixmap_round_trip);
    qtest_add_func("/psitech-rad4/cursor-lut-tri-write",
                   test_cursor_lut_tri_write);
    qtest_add_func("/psitech-rad4/reset", test_reset);

    return g_test_run();
}
