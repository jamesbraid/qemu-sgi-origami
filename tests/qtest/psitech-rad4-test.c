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
#include "qemu/bswap.h"
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
#define PCI_COMMAND_MASTER  0x0004u

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

/*
 * RAD4-KM (12d1:0105), the same shared psitech-rad4 implementation at
 * device-id=0x105 (docs/datasheets/psitech-rad4-km.md). A second synthetic
 * slot, one window further along than RAD4_SLOT's, so both identities can
 * be brought up independently in the same process; like RAD4_SLOT, this
 * is a convenient free device number for register-contract testing, not a
 * claim about which real Origin 200 backplane slot either board occupies.
 */
#define RAD4_KM_SLOT     4
#define WINDOW_RAD4_KM   (BRIDGE_BASE + 0x800000)
#define RAD4_KM_DEVICE_ID 0x0105

#define MACHINE_WITH_BRIDGE_KM \
    "-M origin200 -device psitech-rad4,addr=" G_STRINGIFY(RAD4_KM_SLOT) \
    ",device-id=" G_STRINGIFY(RAD4_KM_DEVICE_ID)

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

/*
 * RGB561 multi-cycle index ranges reached through B_RGB_RESERVED, from
 * IBM Microelectronics "RGB561 Architecture" Preliminary Rev. 1.1
 * (docs/library/cache/graphics/ibm-rgb561-architecture.pdf in the owning
 * Origami repository): Table 20's register address table (page 27) for
 * the base addresses, page 47's FB_WAT bit layout and page 2's Table 1
 * color LUT addressing formula.
 */
#define WAT_BASE        0x1000
#define COLOR_LUT_BASE  0x4000

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

/*
 * Expansion-ROM route DMA registers (docs/datasheets/psitech-rad4.md,
 * "DMA"; PLX PCI9054 Data Book Tables 11-76, 11-80 and 11-86): DMAMODE0,
 * DMADPR0 and DMACSR0 at local offsets 0x100, 0x110 and 0x128.
 */
#define ROM_MODE        0x100
#define ROM_DESC        0x110
#define ROM_COMMAND     0x128

#define DMA_MODE_SCATTER_GATHER (1u << 9)
#define DMA_MODE_DONE_IRQ       (1u << 10)

#define DMA_CMD_START           0x3u    /* ENABLE | START */
#define DMA_CMD_ABORT           0x4u
#define DMA_STATUS_DONE         0x10u

/* Descriptor pointer / next-descriptor-pointer low flag bits. */
#define DMA_DESC_LOC_PCI        (1u << 0)
#define DMA_DESC_END_OF_CHAIN   (1u << 1)
#define DMA_DESC_INTR_ON_TC     (1u << 2)
#define DMA_DESC_LOCAL_TO_PCI   (1u << 3)

/*
 * Bridge's byte-lane correction for a device-initiated DMA transfer
 * (crosses crosstalk, unlike this file's own PIO-only apertures above):
 * tests/qtest/sn0-qlogic-isp-test.c exercises the mechanism itself. The
 * Direct window's own swap enable, and the register holding the bit that
 * clears its "add 512 megabytes" quirk, which resets set.
 */
#define DEV_SWAP_DIRECT         (1u << 19)
#define PCI_DMA_DIRECT          0x80000000u
#define BRIDGE_DIRECT_MAP_REG   (BRIDGE_BASE + 0x080 + REG_DATA)

/*
 * Two low, otherwise-unused guest physical pages: one for descriptor
 * blocks a DMA start names, one for the pixel payload a descriptor
 * points at as its source.
 */
#define DMA_DESC_PHYS           0x00100000u
#define DMA_SRC_PHYS            0x00200000u

/* Matches RAD4_FB_SIZE in hw/display/psitech-rad4.c. */
#define FB_SIZE                 (8 * 1024 * 1024)

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

/*
 * RAD4-KM (12d1:0105) helpers, mirroring rad4_window_to()/rad4_readl()/
 * rad4_writel()/rad4_configure_bars() above at RAD4_KM_SLOT's own window.
 * docs/datasheets/psitech-rad4-km.md clears only the PCI identity and the
 * guest-visible driver-attach outcome for this identity; the register
 * layout underneath is plain RAD4's own, reused as a disclosed
 * approximation (hw/display/psitech-rad4.c's file header). These helpers
 * exist to prove that reuse round-trips as stated, not to assert any
 * independently confirmed RAD4-KM register fact.
 */
static void rad4km_window_to(QTestState *qts, uint32_t pci_addr)
{
    uint32_t base = pci_addr & ~(WINDOW_SIZE - 1);

    qtest_writel(qts, BRIDGE_DEVICE(RAD4_KM_SLOT), DEV_MEMORY_SPACE |
                (base >> 20));
}

static uint32_t rad4km_readl(QTestState *qts, uint32_t pci_addr)
{
    rad4km_window_to(qts, pci_addr);
    return qtest_readl(qts, WINDOW_RAD4_KM + (pci_addr & (WINDOW_SIZE - 1)));
}

static void rad4km_writel(QTestState *qts, uint32_t pci_addr, uint32_t val)
{
    rad4km_window_to(qts, pci_addr);
    qtest_writel(qts, WINDOW_RAD4_KM + (pci_addr & (WINDOW_SIZE - 1)), val);
}

static uint32_t bar0_km_read(QTestState *qts, uint32_t off)
{
    return rad4km_readl(qts, RAD4_BAR0_PCI + off);
}

static void bar0_km_write(QTestState *qts, uint32_t off, uint32_t val)
{
    rad4km_writel(qts, RAD4_BAR0_PCI + off, val);
}

static uint32_t bar2_km_read(QTestState *qts, uint32_t off)
{
    return rad4km_readl(qts, RAD4_BAR2_PCI + off);
}

static void bar2_km_write(QTestState *qts, uint32_t off, uint32_t val)
{
    rad4km_writel(qts, RAD4_BAR2_PCI + off, val);
}

static void rad4km_configure_bars(QTestState *qts)
{
    qtest_writel(qts, CFG0(RAD4_KM_SLOT) + PCI_BAR0_OFF, RAD4_BAR0_PCI);
    qtest_writel(qts, CFG0(RAD4_KM_SLOT) + PCI_BAR2_CFG_OFF, RAD4_BAR2_PCI);
    qtest_writel(qts, CFG0(RAD4_KM_SLOT) + PCI_ROM_ADDRESS_OFF,
                RAD4_ROM_PCI | PCI_ROM_ENABLE);
    qtest_writel(qts, CFG0(RAD4_KM_SLOT) + PCI_COMMAND_OFF,
                PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(RAD4_KM_SLOT),
                DEV_MEMORY_SPACE | (RAD4_BAR0_PCI >> 20));
}

static QTestState *rad4km_init(void)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE_KM);

    rad4km_configure_bars(qts);
    return qts;
}

static void test_km_pci_identity(void)
{
    QTestState *qts = rad4km_init();
    uint32_t id = qtest_readl(qts, CFG0(RAD4_KM_SLOT) + PCI_VENDOR_ID_OFF);

    g_assert_cmphex(id & 0xffff, ==, RAD4_VENDOR_ID);
    g_assert_cmphex(id >> 16, ==, RAD4_KM_DEVICE_ID);

    /* BAR0/BAR2 sizing is unchanged from plain RAD4 (128 B / 16 MiB). */
    qtest_writel(qts, CFG0(RAD4_KM_SLOT) + PCI_BAR0_OFF, 0xffffffffu);
    uint32_t bar0_size_mask = qtest_readl(qts,
                                          CFG0(RAD4_KM_SLOT) + PCI_BAR0_OFF);
    g_assert_cmphex(bar0_size_mask & 0xfffffff0u, ==,
                    (uint32_t)-128 & 0xfffffff0u);
    qtest_writel(qts, CFG0(RAD4_KM_SLOT) + PCI_BAR0_OFF, RAD4_BAR0_PCI);

    qtest_quit(qts);
}

static void test_km_bar0_register_round_trip(void)
{
    QTestState *qts = rad4km_init();

    g_assert_cmphex(bar0_km_read(qts, R_MAILBOX), ==, 0);
    bar0_km_write(qts, R_MAILBOX, 0x12345678u);
    g_assert_cmphex(bar0_km_read(qts, R_MAILBOX), ==, 0x12345678u);

    qtest_quit(qts);
}

static void test_km_bar2_framebuffer_round_trip(void)
{
    QTestState *qts = rad4km_init();

    g_assert_cmphex(bar2_km_read(qts, 0x000000), ==, 0);
    bar2_km_write(qts, 0x000000, 0x00ff00ffu);
    g_assert_cmphex(bar2_km_read(qts, 0x000000), ==, 0x00ff00ffu);

    qtest_quit(qts);
}

/* Writes one 16-byte scatter/gather descriptor into guest memory. */
static void dma_write_descriptor(QTestState *qts, uint32_t phys,
                                 uint32_t pci_addr, uint32_t local_addr,
                                 uint32_t size, uint32_t next)
{
    uint8_t buf[16];

    stl_be_p(buf, pci_addr);
    stl_be_p(buf + 4, local_addr);
    stl_be_p(buf + 8, size);
    stl_be_p(buf + 12, next);
    qtest_memwrite(qts, phys, buf, sizeof(buf));
}

/* Writes a run of 32-bit pixel words into guest memory as a DMA source. */
static void dma_write_payload(QTestState *qts, uint32_t phys,
                              const uint32_t *words, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        uint8_t buf[4];

        stl_be_p(buf, words[i]);
        qtest_memwrite(qts, phys + i * 4, buf, sizeof(buf));
    }
}

/*
 * Enables bus mastering and this slot's Direct-window byte-lane
 * correction, then writes DMACSR0 (ROM_COMMAND) to start (or abort) a
 * transfer. Real firmware programs a working device's DMA for the
 * corrected case -- the disabled case leaves the crossed wiring this
 * chip's DMA path otherwise faithfully emulates, which
 * tests/qtest/sn0-qlogic-isp-test.c exercises directly -- and
 * rad4_window_to()'s own writes to the slot's device register never set
 * this bit, so it is set again immediately before every start or abort
 * this file issues rather than once in rad4_init().
 */
static void dma_start(QTestState *qts, uint32_t val)
{
    uint32_t base = RAD4_ROM_PCI & ~(WINDOW_SIZE - 1);

    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_COMMAND_OFF,
                PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    /*
     * The add-512-megabytes field resets set (tests/qtest/sn0-qlogic-isp-
     * test.c); left alone it moves every Direct-window transfer this file
     * issues half a gigabyte away from the physical page it named.
     */
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);
    qtest_writel(qts, BRIDGE_DEVICE(RAD4_SLOT),
                DEV_MEMORY_SPACE | DEV_SWAP_DIRECT | (base >> 20));
    qtest_writel(qts, WINDOW_RAD4 + ((RAD4_ROM_PCI + ROM_COMMAND) &
                                    (WINDOW_SIZE - 1)), val);
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

static void rad4_set_rgb_index(QTestState *qts, uint16_t index)
{
    bar2_write(qts, B_RGB_LO, index & 0xff);
    bar2_write(qts, B_RGB_HI, (index >> 8) & 0xff);
}

/*
 * The address index register auto increments "after each read or write
 * cycle" (IBM RGB561 Architecture p.1), a convention this model applies
 * to the ordinary single-cycle B_RGB_DATA port on write already. This
 * checks the read side, previously missing: a real fidelity gap, though
 * harmless for the qualified guest workload, since nothing there reads
 * this port back.
 */
static void test_rgb_data_read_autoincrement(void)
{
    QTestState *qts = rad4_init();

    rad4_set_rgb_index(qts, 0x0001);
    bar2_write(qts, B_RGB_DATA, 0x11);
    bar2_write(qts, B_RGB_DATA, 0x22);

    rad4_set_rgb_index(qts, 0x0001);
    g_assert_cmphex(bar2_read(qts, B_RGB_DATA), ==, 0x11);
    g_assert_cmphex(bar2_read(qts, B_RGB_DATA), ==, 0x22);

    qtest_quit(qts);
}

/*
 * FB_WAT entries are a 10-bit value transferred in two cycles through
 * B_RGB_RESERVED (IBM RGB561 Architecture p.4, Table 5): the first cycle
 * carries bits 9-2 in bit positions 7-0, the second carries bits 1-0 in
 * bit positions 7-6. Writing and reading one entry back must round-trip,
 * and the address index register must auto increment only after the
 * second (last) cycle, to the next window ID's entry.
 */
static void test_fb_wat_round_trip(void)
{
    QTestState *qts = rad4_init();

    /* 0x36 << 2 == 0x0d8, matching the reviewed True-24bpp init value. */
    rad4_set_rgb_index(qts, WAT_BASE + 5);
    bar2_write(qts, B_RGB_RESERVED, 0xd8);
    bar2_write(qts, B_RGB_RESERVED, 0x00);

    rad4_set_rgb_index(qts, WAT_BASE + 5);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0xd8);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0x00);

    /* Auto increment landed on WID 6's entry, still zero. */
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0x00);

    qtest_quit(qts);
}

/*
 * Re-pointing the address index register mid-entry restarts the next
 * multi-cycle transfer at its first cycle (IBM RGB561 Architecture p.1:
 * "If the address index register is accessed before completion of a
 * multi-cycle access, the counter is reset and the operation is
 * terminated.").
 */
static void test_reserved_port_index_write_resets_phase(void)
{
    QTestState *qts = rad4_init();

    rad4_set_rgb_index(qts, WAT_BASE + 9);
    bar2_write(qts, B_RGB_RESERVED, 0xff); /* First cycle only. */

    /* Re-pointing anywhere resets the phase, even back to the same WID. */
    rad4_set_rgb_index(qts, WAT_BASE + 9);
    bar2_write(qts, B_RGB_RESERVED, 0xaa); /* Treated as first cycle again. */
    bar2_write(qts, B_RGB_RESERVED, 0xc0); /* Second cycle: D1 D0 = 11. */

    rad4_set_rgb_index(qts, WAT_BASE + 9);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0xaa);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0xc0);

    qtest_quit(qts);
}

/*
 * The color look-up table is a 3 cycle (red, green, blue) transfer
 * through the same port (same document, p.2, "Color Look-Up Table
 * (LUT)"). Entry 0 corresponds to color LUT address 0.
 */
static void test_color_lut_round_trip(void)
{
    QTestState *qts = rad4_init();

    rad4_set_rgb_index(qts, COLOR_LUT_BASE);
    bar2_write(qts, B_RGB_RESERVED, 0x11);
    bar2_write(qts, B_RGB_RESERVED, 0x22);
    bar2_write(qts, B_RGB_RESERVED, 0x33);

    rad4_set_rgb_index(qts, COLOR_LUT_BASE);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0x11);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0x22);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0x33);

    /* Auto increment landed on entry 1, still zero. */
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0x00);

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

/*
 * Writing the mode and descriptor-pointer registers alone -- without also
 * writing DMACSR0's start bits -- only latches the value back for
 * readback, matching every other configuration-only register this file
 * already round-trips (R_REG14, R_REG18, R_MAILBOX).
 */
static void test_dma_registers_before_start(void)
{
    QTestState *qts = rad4_init();

    rad4_writel(qts, RAD4_ROM_PCI + ROM_MODE, 0x7c3u);
    g_assert_cmphex(rad4_readl(qts, RAD4_ROM_PCI + ROM_MODE), ==, 0x7c3u);
    rad4_writel(qts, RAD4_ROM_PCI + ROM_DESC, 0xaabbccc0u);
    g_assert_cmphex(rad4_readl(qts, RAD4_ROM_PCI + ROM_DESC), ==,
                    0xaabbccc0u);

    /* No pixel, completion or interrupt effect from either write alone. */
    g_assert_cmphex(bar2_read(qts, 0x000000), ==, 0);
    g_assert_cmphex(rad4_readl(qts, RAD4_ROM_PCI + ROM_COMMAND) &
                    DMA_STATUS_DONE, ==, 0);
    g_assert_cmphex(bar0_read(qts, R_INTCSR) & INTCSR_PENDING, ==, 0);

    qtest_quit(qts);
}

/*
 * A single, end-of-chain descriptor uploads its named span of guest
 * memory into the framebuffer at its named local address, and DMACSR0's
 * Done bit follows.
 */
static void test_dma_single_descriptor_upload(void)
{
    QTestState *qts = rad4_init();
    static const uint32_t payload[4] = {
        0x00ff0080u, 0x00112233u, 0x00445566u, 0x00778899u,
    };

    dma_write_payload(qts, DMA_SRC_PHYS, payload, ARRAY_SIZE(payload));
    dma_write_descriptor(qts, DMA_DESC_PHYS, PCI_DMA_DIRECT | DMA_SRC_PHYS,
                         0x000010u, sizeof(payload), 0);

    rad4_writel(qts, RAD4_ROM_PCI + ROM_MODE, DMA_MODE_SCATTER_GATHER);
    rad4_writel(qts, RAD4_ROM_PCI + ROM_DESC,
               (PCI_DMA_DIRECT | DMA_DESC_PHYS) | DMA_DESC_LOC_PCI |
               DMA_DESC_END_OF_CHAIN);
    dma_start(qts, DMA_CMD_START);

    g_assert_cmphex(rad4_readl(qts, RAD4_ROM_PCI + ROM_COMMAND) &
                    DMA_STATUS_DONE, ==, DMA_STATUS_DONE);
    for (unsigned i = 0; i < ARRAY_SIZE(payload); i++) {
        g_assert_cmphex(bar2_read(qts, 0x000010 + i * 4), ==, payload[i]);
    }

    qtest_quit(qts);
}

/*
 * The plane mask (RAD4_BAR2_MASK) applies to a DMA upload exactly as it
 * does to a CPU pixel write (test_framebuffer_mask): the mask register is
 * documented as programmed before a transfer and restored afterward,
 * naming both driver render paths that share this device's DMA engine,
 * not a CPU-only feature of the memory controller it sits in front of.
 */
static void test_dma_upload_honors_mask(void)
{
    QTestState *qts = rad4_init();
    static const uint32_t payload[1] = { 0x00112233u };

    bar2_write(qts, 0x000020, 0x00aabbccu);
    bar2_write(qts, B_MASK, 0x0000ffffu);

    dma_write_payload(qts, DMA_SRC_PHYS, payload, ARRAY_SIZE(payload));
    dma_write_descriptor(qts, DMA_DESC_PHYS, PCI_DMA_DIRECT | DMA_SRC_PHYS,
                         0x000020u, sizeof(payload), 0);
    rad4_writel(qts, RAD4_ROM_PCI + ROM_DESC,
               (PCI_DMA_DIRECT | DMA_DESC_PHYS) | DMA_DESC_LOC_PCI |
               DMA_DESC_END_OF_CHAIN);
    dma_start(qts, DMA_CMD_START);

    /* Only the masked low half of the incoming word took effect. */
    g_assert_cmphex(bar2_read(qts, 0x000020), ==, 0x00aa2233u);

    qtest_quit(qts);
}

/*
 * DMAMODE0's Done Interrupt Enable bit (0x400) routes transfer completion
 * into this file's existing shared interrupt-pending convention
 * (test_interrupt_acknowledge), the same one the frame timer uses.
 */
static void test_dma_done_interrupt_when_enabled(void)
{
    QTestState *qts = rad4_init();
    static const uint32_t payload[1] = { 0x00abcdefu };

    bar0_write(qts, R_INTCSR, INTCSR_ENABLE);

    dma_write_payload(qts, DMA_SRC_PHYS, payload, ARRAY_SIZE(payload));
    dma_write_descriptor(qts, DMA_DESC_PHYS, PCI_DMA_DIRECT | DMA_SRC_PHYS,
                         0x000030u, sizeof(payload), 0);
    rad4_writel(qts, RAD4_ROM_PCI + ROM_MODE,
               DMA_MODE_SCATTER_GATHER | DMA_MODE_DONE_IRQ);
    rad4_writel(qts, RAD4_ROM_PCI + ROM_DESC,
               (PCI_DMA_DIRECT | DMA_DESC_PHYS) | DMA_DESC_LOC_PCI |
               DMA_DESC_END_OF_CHAIN);
    dma_start(qts, DMA_CMD_START);

    g_assert_cmphex(bar0_read(qts, R_INTCSR) & INTCSR_PENDING, ==,
                    INTCSR_PENDING);

    qtest_quit(qts);
}

/*
 * Two chained descriptors: the first names the second through its own
 * next-descriptor-pointer word (PLX PCI9054 Data Book Table 11-80), and
 * both land in the framebuffer at their own, independent local addresses
 * -- the mechanism the driver's own disassembly shows building one
 * descriptor per page-sized run of a transfer, not one flat descriptor
 * for the whole span (docs/research/graphics/rad4-chipset-20260915.md).
 */
static void test_dma_descriptor_chain(void)
{
    QTestState *qts = rad4_init();
    static const uint32_t first_word[1] = { 0x0000aaaau };
    static const uint32_t second_word[1] = { 0x0000bbbbu };
    uint32_t second_desc_phys = DMA_DESC_PHYS + 16;

    dma_write_payload(qts, DMA_SRC_PHYS, first_word, 1);
    dma_write_payload(qts, DMA_SRC_PHYS + 0x1000, second_word, 1);

    dma_write_descriptor(qts, second_desc_phys,
                         PCI_DMA_DIRECT | (DMA_SRC_PHYS + 0x1000),
                         0x000044u, sizeof(second_word), 0);
    dma_write_descriptor(qts, DMA_DESC_PHYS, PCI_DMA_DIRECT | DMA_SRC_PHYS,
                         0x000040u, sizeof(first_word),
                         (PCI_DMA_DIRECT | second_desc_phys) |
                         DMA_DESC_LOC_PCI | DMA_DESC_END_OF_CHAIN);

    rad4_writel(qts, RAD4_ROM_PCI + ROM_DESC,
               (PCI_DMA_DIRECT | DMA_DESC_PHYS) | DMA_DESC_LOC_PCI);
    dma_start(qts, DMA_CMD_START);

    g_assert_cmphex(rad4_readl(qts, RAD4_ROM_PCI + ROM_COMMAND) &
                    DMA_STATUS_DONE, ==, DMA_STATUS_DONE);
    g_assert_cmphex(bar2_read(qts, 0x000040), ==, first_word[0]);
    g_assert_cmphex(bar2_read(qts, 0x000044), ==, second_word[0]);

    qtest_quit(qts);
}

/*
 * A Local-space descriptor (DMA_DESC_LOC_PCI clear) is read straight out of
 * the framebuffer at its own masked address instead of guest/PCI memory,
 * honoring the same convention a live IRIX 6.5.30 boot's buffered driver
 * path was captured actually using: a descriptor stored at the very top of
 * the 8 MiB local extent (docs/datasheets/psitech-rad4.md, "DMA", "Live
 * capture"). The descriptor's own storage location is Local; the payload
 * source it names is still an ordinary PCI address, exactly as the live
 * capture's own descriptor decoded.
 */
static void test_dma_local_descriptor_upload(void)
{
    QTestState *qts = rad4_init();
    static const uint32_t payload[4] = {
        0x0000aaaau, 0x0000bbbbu, 0x0000ccccu, 0x0000ddddu,
    };
    uint32_t desc_local = FB_SIZE - 16;

    dma_write_payload(qts, DMA_SRC_PHYS, payload, ARRAY_SIZE(payload));
    bar2_write(qts, desc_local, PCI_DMA_DIRECT | DMA_SRC_PHYS);
    bar2_write(qts, desc_local + 4, 0x000050u);
    bar2_write(qts, desc_local + 8, sizeof(payload));
    bar2_write(qts, desc_local + 12, 0);

    rad4_writel(qts, RAD4_ROM_PCI + ROM_DESC,
               desc_local | DMA_DESC_END_OF_CHAIN);
    dma_start(qts, DMA_CMD_START);

    g_assert_cmphex(rad4_readl(qts, RAD4_ROM_PCI + ROM_COMMAND) &
                    DMA_STATUS_DONE, ==, DMA_STATUS_DONE);
    for (unsigned i = 0; i < ARRAY_SIZE(payload); i++) {
        g_assert_cmphex(bar2_read(qts, 0x000050 + i * 4), ==, payload[i]);
    }

    qtest_quit(qts);
}

/*
 * Two chained descriptors both stored in Local space, the first's own
 * next-descriptor-pointer word naming the second exactly as the live
 * capture's descriptor array did (each entry 16 bytes below the one
 * DMADPR0 or the previous entry's next-pointer named).
 */
static void test_dma_local_descriptor_chain(void)
{
    QTestState *qts = rad4_init();
    static const uint32_t first_word[1] = { 0x0000ccccu };
    static const uint32_t second_word[1] = { 0x0000ddddu };
    uint32_t first_desc_local = FB_SIZE - 32;
    uint32_t second_desc_local = FB_SIZE - 16;

    dma_write_payload(qts, DMA_SRC_PHYS, first_word, 1);
    dma_write_payload(qts, DMA_SRC_PHYS + 0x1000, second_word, 1);

    bar2_write(qts, second_desc_local,
              PCI_DMA_DIRECT | (DMA_SRC_PHYS + 0x1000));
    bar2_write(qts, second_desc_local + 4, 0x000064u);
    bar2_write(qts, second_desc_local + 8, sizeof(second_word));
    bar2_write(qts, second_desc_local + 12, 0);

    bar2_write(qts, first_desc_local, PCI_DMA_DIRECT | DMA_SRC_PHYS);
    bar2_write(qts, first_desc_local + 4, 0x000060u);
    bar2_write(qts, first_desc_local + 8, sizeof(first_word));
    bar2_write(qts, first_desc_local + 12,
              second_desc_local | DMA_DESC_END_OF_CHAIN);

    rad4_writel(qts, RAD4_ROM_PCI + ROM_DESC, first_desc_local);
    dma_start(qts, DMA_CMD_START);

    g_assert_cmphex(rad4_readl(qts, RAD4_ROM_PCI + ROM_COMMAND) &
                    DMA_STATUS_DONE, ==, DMA_STATUS_DONE);
    g_assert_cmphex(bar2_read(qts, 0x000060), ==, first_word[0]);
    g_assert_cmphex(bar2_read(qts, 0x000064), ==, second_word[0]);

    qtest_quit(qts);
}

/*
 * A Local-space descriptor pointer at or past the framebuffer's own end
 * leaves no room for the 16 bytes it names and leaves DMACSR0's Done bit
 * clear, the same failure convention test_dma_abort_recovers_a_failed_
 * transfer already exercises for an out-of-bounds payload target.
 */
static void test_dma_local_descriptor_out_of_bounds(void)
{
    QTestState *qts = rad4_init();

    rad4_writel(qts, RAD4_ROM_PCI + ROM_DESC,
               FB_SIZE | DMA_DESC_END_OF_CHAIN);
    dma_start(qts, DMA_CMD_START);

    g_assert_cmphex(rad4_readl(qts, RAD4_ROM_PCI + ROM_COMMAND) &
                    DMA_STATUS_DONE, ==, 0);

    qtest_quit(qts);
}

/*
 * A descriptor this model cannot honor (its local span runs past the
 * framebuffer) leaves DMACSR0's Done bit clear, exactly as the
 * unmodeled-DMA build already left it before this transfer existed at
 * all -- the guest's own timeout/abort recovery, exercised then by every
 * buffered-mode draw, is exercised here by hand. Writing ABORT (0x4)
 * still sets Done, matching PLX PCI9054 Data Book Table 11-86 ("Sets
 * Channel 0 Done bit when abort is complete"), so that recovery path
 * keeps working for a transfer this model genuinely cannot complete.
 */
static void test_dma_abort_recovers_a_failed_transfer(void)
{
    QTestState *qts = rad4_init();

    dma_write_descriptor(qts, DMA_DESC_PHYS, PCI_DMA_DIRECT | DMA_SRC_PHYS,
                         FB_SIZE - 4, 16, 0);
    rad4_writel(qts, RAD4_ROM_PCI + ROM_DESC,
               (PCI_DMA_DIRECT | DMA_DESC_PHYS) | DMA_DESC_LOC_PCI |
               DMA_DESC_END_OF_CHAIN);
    dma_start(qts, DMA_CMD_START);
    g_assert_cmphex(rad4_readl(qts, RAD4_ROM_PCI + ROM_COMMAND) &
                    DMA_STATUS_DONE, ==, 0);

    dma_start(qts, DMA_CMD_ABORT);
    g_assert_cmphex(rad4_readl(qts, RAD4_ROM_PCI + ROM_COMMAND) &
                    DMA_STATUS_DONE, ==, DMA_STATUS_DONE);

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
    qtest_add_func("/psitech-rad4/rgb-data-read-autoincrement",
                   test_rgb_data_read_autoincrement);
    qtest_add_func("/psitech-rad4/fb-wat-round-trip", test_fb_wat_round_trip);
    qtest_add_func("/psitech-rad4/reserved-port-index-write-resets-phase",
                   test_reserved_port_index_write_resets_phase);
    qtest_add_func("/psitech-rad4/color-lut-round-trip",
                   test_color_lut_round_trip);
    qtest_add_func("/psitech-rad4/mode-recognition", test_mode_recognition);
    qtest_add_func("/psitech-rad4/mode-recognition-mutation",
                   test_mode_recognition_rejects_mutation);
    qtest_add_func("/psitech-rad4/interrupt-acknowledge",
                   test_interrupt_acknowledge);
    qtest_add_func("/psitech-rad4/dma-registers-before-start",
                   test_dma_registers_before_start);
    qtest_add_func("/psitech-rad4/dma-single-descriptor-upload",
                   test_dma_single_descriptor_upload);
    qtest_add_func("/psitech-rad4/dma-upload-honors-mask",
                   test_dma_upload_honors_mask);
    qtest_add_func("/psitech-rad4/dma-done-interrupt-when-enabled",
                   test_dma_done_interrupt_when_enabled);
    qtest_add_func("/psitech-rad4/dma-descriptor-chain",
                   test_dma_descriptor_chain);
    qtest_add_func("/psitech-rad4/dma-local-descriptor-upload",
                   test_dma_local_descriptor_upload);
    qtest_add_func("/psitech-rad4/dma-local-descriptor-chain",
                   test_dma_local_descriptor_chain);
    qtest_add_func("/psitech-rad4/dma-local-descriptor-out-of-bounds",
                   test_dma_local_descriptor_out_of_bounds);
    qtest_add_func("/psitech-rad4/dma-abort-recovers-a-failed-transfer",
                   test_dma_abort_recovers_a_failed_transfer);
    qtest_add_func("/psitech-rad4/cursor-position-and-enable",
                   test_cursor_position_and_enable);
    qtest_add_func("/psitech-rad4/cursor-pixmap-round-trip",
                   test_cursor_pixmap_round_trip);
    qtest_add_func("/psitech-rad4/cursor-lut-tri-write",
                   test_cursor_lut_tri_write);
    qtest_add_func("/psitech-rad4/reset", test_reset);

    qtest_add_func("/psitech-rad4-km/pci-identity", test_km_pci_identity);
    qtest_add_func("/psitech-rad4-km/bar0-register-round-trip",
                   test_km_bar0_register_round_trip);
    qtest_add_func("/psitech-rad4-km/bar2-framebuffer-round-trip",
                   test_km_bar2_framebuffer_round_trip);

    return g_test_run();
}
