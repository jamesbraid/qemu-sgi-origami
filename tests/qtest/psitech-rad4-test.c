/*
 * PsiTech RAD4 (psitech-rad4): register contract for the one-mode functional
 * approximation, through the real Origin 200 Bridge PCI path.
 *
 * The card is an add-in PCI card, so like tests/qtest/sn0-tigon-test.c it is
 * instantiated with -device at a free slot and reached through that slot's
 * own Bridge decode. Expected values come from the device's register
 * documentation, not from this device's own implementation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#include "psitech-rad4-mode0-fixture.h"
#include "psitech-rad4-modes-fixture.h"
#include "sgi-test.h"

/*
 * PCI type 0 configuration space offsets, restated the way
 * tests/qtest/sn0-tigon-test.c restates its own.
 */
#define PCI_VENDOR_ID_OFF   0x00
#define PCI_REVISION_ID_OFF 0x08
#define PCI_CLASS_DEVICE_OFF 0x0a
#define PCI_BAR0_OFF        0x10
#define PCI_BAR2_CFG_OFF    0x18
#define PCI_BAR3_CFG_OFF    0x1c
#define PCI_ROM_ADDRESS_OFF 0x30
#define PCI_COMMAND_OFF     0x04

/*
 * PCI_STATUS_INTERRUPT (config+0x06 bit 3, Linux pci_regs.h), read through
 * the same dword as PCI_COMMAND_OFF since STATUS occupies its high 16 bits:
 * core QEMU's pci_update_irq_status() (hw/pci/pci.c) mirrors this bit to a
 * device's own irq_state on every edge, regardless of the Interrupt Disable
 * bit. This standard, chip-independent PCI bit shows whether the device
 * asserted its IRQ line. It needs no GPIO interception into the origin200
 * machine's TYPE_SGI_BRIDGE chip (which, unlike TYPE_SGI_XBRIDGE,
 * exposes no named PCI-INTx GPIO-out group for tests/qtest/sgi-board-test.c's
 * own interception pattern to reach).
 */
#define PCI_STATUS_INTERRUPT (1u << 19)

/* I/O space, and the Bridge's widget window inside it. */
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_IO + ((uint64_t)BRIDGE_WIDGET << 24))

/*
 * Devices 0 and 1 are BaseIO's SCSI adapters and 2 is IOC3; 3 is the first
 * free slot, with the first of the six 1 MiB windows beyond the two 2 MiB
 * ones. The test in tests/qtest/sn0-tigon-test.c uses the same slot.
 */
#define RAD4_SLOT       3
#define WINDOW_RAD4     (BRIDGE_BASE + 0x700000)
#define WINDOW_SIZE     0x100000u

#define BRIDGE_CFG0     0x020000
#define CFG0_STRIDE     0x1000
#define CFG0(dev)       (BRIDGE_BASE + BRIDGE_CFG0 + (dev) * CFG0_STRIDE)

#define REG_DATA            4
#define BRIDGE_DEVICE(n)    (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)

#define PCI_ROM_ENABLE      1u

#define MACHINE_WITH_BRIDGE \
    "-M origin200 -device psitech-rad4,addr=" G_STRINGIFY(RAD4_SLOT)

/*
 * PCI addresses this test assigns the card's apertures; 1 MiB aligned so BAR0
 * and the ROM route never need the window to slide, and BAR2's base is 1 MiB
 * aligned so the offsets below compose with it directly. RAD4_KM_BAR3_PCI is
 * the RAD4-KM identity's own keyboard/mouse register file (BAR 3), unused by
 * plain RAD4.
 */
#define RAD4_BAR0_PCI   0x20000000u
#define RAD4_BAR2_PCI   0x30000000u
#define RAD4_ROM_PCI    0x40000000u
#define RAD4_KM_BAR3_PCI 0x50000000u

#define RAD4_VENDOR_ID  0x12d1
#define RAD4_DEVICE_ID  0x0104

/*
 * RAD4-KM (12d1:0105), the same shared psitech-rad4 implementation at
 * device-id=0x105. A second synthetic slot, one window further along than
 * RAD4_SLOT's, so both identities can be brought up independently in the same
 * process; like RAD4_SLOT, this is a convenient free device number for
 * register-contract testing, not a claim about which real Origin 200
 * backplane slot either board occupies.
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
 * RGB561 multi-cycle index ranges reached through B_RGB_RESERVED, from IBM
 * Microelectronics "RGB561 Architecture" Preliminary Rev. 1.1: Table 20's
 * register address table (page 27) for the base addresses, page 47's FB_WAT
 * bit layout and page 2's Table 1 color LUT addressing formula.
 */
#define WAT_BASE        0x1000
#define OL_WAT_BASE     0x1400
#define COLOR_LUT_BASE  0x4000
#define GAMMA_RED       0x3000
#define GAMMA_GREEN     0x3400
#define GAMMA_BLUE      0x3800

/*
 * Single-cycle registers that address the window attribute tables and
 * combine the overlay with the frame buffer (same document, pages 29, 32, 34
 * and 46), and the values the shipped X server programs into them: a 4:1
 * basic serializer with an 8-bit overlay port and 8 window-ID bits, all of
 * them split to the frame buffer, and overlay value 0 transparent through
 * chroma key 0 under mask 0x0f.
 */
#define RGB_CONF1               0x0001
#define RGB_CONF4               0x0004
#define RGB_OL_WAT_SEGMENT      0x0007
#define RGB_AUX_OL_WAT_SEGMENT  0x0009
#define RGB_CHROMA_KEY0         0x0010
#define RGB_CHROMA_MASK0        0x0012
#define RGB_AUX_FB_WAT          0x0e00
#define RGB_AUX_OL_WAT          0x0f00
#define AUX_WAT_GAMMA_BYPASS    0x04

#define CONF1_4TO1_OVLY8_LWID8  0x2c
#define CONF4_SPLIT_FB8         0x45
#define CONF4_COMMON            0x00
#define CONF4_COMMON_AOW        0x02
#define CONF4_SPLIT_FB4         0x24
#define CONF1_4TO1_OVLY8_LWID4  0x2a
#define AUX_OL_WAT_CK0_ENABLED  0x28
#define AUX_OL_WAT_CK0_DISABLED 0x20

/* 10-bit WAT entries, page 47-48: START_ADDR, PIX_FORM, BS, MODE, TR. */
#define WAT_ENTRY(start, pix_form, bs, mode, tr) \
    (((start) << 6) | ((pix_form) << 4) | ((bs) << 3) | ((mode) << 1) | (tr))
#define FB_WAT_TRUE_24          WAT_ENTRY(0, 3, 0, 3, 0)
#define OL_WAT_4BIT_INDEX(start, bs) WAT_ENTRY(start, 3, bs, 0, 1)

#define CTRL_MODE_CMD   0x20u
#define CTRL_STORED_BIT 0x02u
#define STATUS_FRAME    0x20u

/*
 * RGB561 indexed cursor registers (IBM Microelectronics "RGB561
 * Architecture", Preliminary Rev. 1.1, pages 27 and 37-39, 45).
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
 * Expansion-ROM route DMA registers (PLX PCI9054 Data Book Tables 11-76,
 * 11-80 and 11-86): DMAMODE0, DMADPR0 and DMACSR0 at local offsets 0x100,
 * 0x110 and 0x128.
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
 * Bridge's byte-lane correction for a device-initiated DMA transfer (crosses
 * XIO, unlike this file's own PIO-only apertures above):
 * tests/qtest/sn0-qlogic-isp-test.c exercises the mechanism itself. The
 * Direct window's own swap enable, and the register holding the bit that
 * clears its "add 512 megabytes" quirk, which resets set.
 */
#define DEV_SWAP_DIRECT         (1u << 19)
#define PCI_DMA_DIRECT          0x80000000u
#define BRIDGE_DIRECT_MAP_REG   (BRIDGE_BASE + 0x080 + REG_DATA)

/*
 * Two low, otherwise-unused guest physical pages: one for descriptor blocks a
 * DMA start names, one for the pixel payload a descriptor points at as its
 * source.
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
 * up its own single BAR. A PCI reset (cold boot or an explicit system reset)
 * clears these config-space registers back to unprogrammed, exactly as real
 * firmware would find them, so this must run again after any reset before the
 * card answers PIO again.
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
 * BAR0/BAR2 stay plain RAD4's own graphics/DMA register layout, reused as a
 * disclosed approximation (hw/display/psitech-rad4.c's file header). These
 * two helpers exist to prove that reuse round-trips as stated, not to assert
 * any independently confirmed RAD4-KM graphics register fact. BAR3 is
 * different: it only exists under this identity, and is the keyboard/mouse
 * controller's own cleared register contract (hw/input/psitech-rad4-km.c).
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

/*
 * RAD4-KM keyboard/mouse register file (BAR 3): sixteen registers, each on
 * its own 64 KiB stride, restated from include/hw/input/psitech-rad4-km.h the
 * way this file restates every other register layout it tests rather than
 * including the production header.
 */
#define KM_REG_STRIDE     0x10000u
#define KM_REG(index)     ((index) * KM_REG_STRIDE)

#define KM_REG_MRA        KM_REG(0x0)
#define KM_REG_SRA_CSRA   KM_REG(0x1)
#define KM_REG_CRA        KM_REG(0x2)
#define KM_REG_RHRA_THRA  KM_REG(0x3)
#define KM_REG_ACR        KM_REG(0x4)
#define KM_REG_ISR_IMR    KM_REG(0x5)
#define KM_REG_CTUR       KM_REG(0x6)
#define KM_REG_CTLR       KM_REG(0x7)
#define KM_REG_MRB        KM_REG(0x8)
#define KM_REG_SRB_CSRB   KM_REG(0x9)
#define KM_REG_CRB        KM_REG(0xa)
#define KM_REG_RHRB_THRB  KM_REG(0xb)
#define KM_REG_IVR        KM_REG(0xc)
#define KM_REG_OPCR       KM_REG(0xd)
#define KM_REG_OPR_SET    KM_REG(0xe)
#define KM_REG_OPR_RESET  KM_REG(0xf)

#define KM_SR_RXRDY       (1u << 0)
#define KM_SR_TXRDY       (1u << 2)

#define KM_ISR_TXRDY_A    (1u << 0)
#define KM_ISR_RXRDY_A    (1u << 1)
#define KM_ISR_TXRDY_B    (1u << 4)
#define KM_ISR_RXRDY_B    (1u << 5)

#define KM_CR_RX_ENABLE   0x01u
#define KM_CR_RX_DISABLE  0x02u
#define KM_CR_TX_ENABLE   0x04u
#define KM_CR_TX_DISABLE  0x08u

static uint32_t bar3_km_read(QTestState *qts, uint32_t off)
{
    return rad4km_readl(qts, RAD4_KM_BAR3_PCI + off);
}

static void bar3_km_write(QTestState *qts, uint32_t off, uint32_t val)
{
    rad4km_writel(qts, RAD4_KM_BAR3_PCI + off, val);
}

static void rad4km_configure_bars(QTestState *qts)
{
    qtest_writel(qts, CFG0(RAD4_KM_SLOT) + PCI_BAR0_OFF, RAD4_BAR0_PCI);
    qtest_writel(qts, CFG0(RAD4_KM_SLOT) + PCI_BAR2_CFG_OFF, RAD4_BAR2_PCI);
    qtest_writel(qts, CFG0(RAD4_KM_SLOT) + PCI_BAR3_CFG_OFF,
                RAD4_KM_BAR3_PCI);
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

    /*
     * BAR3 exists only under this identity and sizes to the km device's own 1
     * MiB register file (sixteen registers x 64 KiB stride,
     * include/hw/input/psitech-rad4-km.h). WINDOW_SIZE is the same 1 MiB.
     */
    qtest_writel(qts, CFG0(RAD4_KM_SLOT) + PCI_BAR3_CFG_OFF, 0xffffffffu);
    uint32_t bar3_size_mask =
        qtest_readl(qts, CFG0(RAD4_KM_SLOT) + PCI_BAR3_CFG_OFF);
    g_assert_cmphex(bar3_size_mask & 0xfffffff0u, ==,
                    (uint32_t)-WINDOW_SIZE & 0xfffffff0u);
    qtest_writel(qts, CFG0(RAD4_KM_SLOT) + PCI_BAR3_CFG_OFF, RAD4_KM_BAR3_PCI);

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

/*
 * CR's Rx/Tx enable bits (index 0x2/0xa, write) control SR's RXRDY/TXRDY bits
 * (index 0x1/0x9, read) per channel. TXRDY is ready as soon
 * as Tx is enabled. RXRDY tracks the channel's own ps2 core queue,
 * which starts empty after reset.
 */
static void test_km_channel_enable_reflects_in_status(void)
{
    QTestState *qts = rad4km_init();

    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRA_CSRA), ==, 0);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRB_CSRB), ==, 0);

    bar3_km_write(qts, KM_REG_CRA, KM_CR_RX_ENABLE | KM_CR_TX_ENABLE);
    bar3_km_write(qts, KM_REG_CRB, KM_CR_RX_ENABLE | KM_CR_TX_ENABLE);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRA_CSRA), ==, KM_SR_TXRDY);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRB_CSRB), ==, KM_SR_TXRDY);

    bar3_km_write(qts, KM_REG_CRA, KM_CR_RX_DISABLE | KM_CR_TX_DISABLE);
    bar3_km_write(qts, KM_REG_CRB, KM_CR_RX_DISABLE | KM_CR_TX_DISABLE);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRA_CSRA), ==, 0);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRB_CSRB), ==, 0);

    qtest_quit(qts);
}

/*
 * ISR (index 0x5, read) aggregates both channels' Rx/Tx ready state into one
 * chip-wide register, matching the cited driver's IMR-shadow test. This model
 * recomputes ISR purely from the channels' own enable/queue state on every
 * read. The same index's write side (IMR) is stored but never consulted for
 * this read (rad4_km_read_isr() ignores s->imr), matching the established
 * behavior. IMR does gate a separate thing this file tests next,
 * test_km_interrupt_reaches_pci_status(): the aggregate qemu_irq output this
 * chip now drives from the same ISR condition, masked.
 */
static void test_km_isr_aggregates_both_channels(void)
{
    QTestState *qts = rad4km_init();

    g_assert_cmphex(bar3_km_read(qts, KM_REG_ISR_IMR), ==, 0);

    bar3_km_write(qts, KM_REG_CRA, KM_CR_TX_ENABLE);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_ISR_IMR), ==, KM_ISR_TXRDY_A);

    bar3_km_write(qts, KM_REG_CRB, KM_CR_TX_ENABLE);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_ISR_IMR), ==,
                    KM_ISR_TXRDY_A | KM_ISR_TXRDY_B);

    /* A write to the same index (the IMR shadow) does not gate the read. */
    bar3_km_write(qts, KM_REG_ISR_IMR, 0x00);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_ISR_IMR), ==,
                    KM_ISR_TXRDY_A | KM_ISR_TXRDY_B);

    qtest_quit(qts);
}

/*
 * The km child's aggregate ISR/IMR condition (hw/input/psitech-rad4-km.c's
 * rad4_km_update_irq()) drives its own qemu_irq output, which
 * hw/display/psitech-rad4.c's rad4_km_irq_handler() ORs into this device's
 * single shared PCI interrupt line (rad4_update_irq()); see
 * include/hw/input/psitech-rad4-km.h for what that wiring does and does not
 * claim about the board-level gate at driver offset 0x78. This test observes
 * that line through config space's own PCI_STATUS_INTERRUPT bit (defined
 * above), independent of the ISR read path test_km_isr_
 * aggregates_both_channels() already covers.
 */
static void test_km_interrupt_reaches_pci_status(void)
{
    QTestState *qts = rad4km_init();

    g_assert_cmphex(qtest_readl(qts, CFG0(RAD4_KM_SLOT) + PCI_COMMAND_OFF) &
                    PCI_STATUS_INTERRUPT, ==, 0);

    /* ISR's TXRDY_A condition alone, masked by a zero IMR, is not enough. */
    bar3_km_write(qts, KM_REG_CRA, KM_CR_TX_ENABLE);
    g_assert_cmphex(qtest_readl(qts, CFG0(RAD4_KM_SLOT) + PCI_COMMAND_OFF) &
                    PCI_STATUS_INTERRUPT, ==, 0);

    /* Unmasking the already-set ISR bit raises the shared PCI line. */
    bar3_km_write(qts, KM_REG_ISR_IMR, KM_ISR_TXRDY_A);
    g_assert_cmphex(qtest_readl(qts, CFG0(RAD4_KM_SLOT) + PCI_COMMAND_OFF) &
                    PCI_STATUS_INTERRUPT, ==, PCI_STATUS_INTERRUPT);

    /* Clearing the ISR condition it was masking lowers the line again. */
    bar3_km_write(qts, KM_REG_CRA, KM_CR_TX_DISABLE);
    g_assert_cmphex(qtest_readl(qts, CFG0(RAD4_KM_SLOT) + PCI_COMMAND_OFF) &
                    PCI_STATUS_INTERRUPT, ==, 0);

    qtest_quit(qts);
}

/*
 * MRA/MRB, ACR, OPCR, CTUR/CTLR, IVR and the OPR set/reset pair have no
 * established read behavior or driver-visible effect beyond being written;
 * this is a smoke test that writing each of them neither crashes nor perturbs
 * the channels' own status.
 */
static void test_km_register_file_write_smoke(void)
{
    QTestState *qts = rad4km_init();

    bar3_km_write(qts, KM_REG_MRA, 0x13);
    bar3_km_write(qts, KM_REG_MRA, 0x07);
    bar3_km_write(qts, KM_REG_MRB, 0x13);
    bar3_km_write(qts, KM_REG_MRB, 0x07);
    bar3_km_write(qts, KM_REG_ACR, 0x00);
    bar3_km_write(qts, KM_REG_OPCR, 0x00);
    bar3_km_write(qts, KM_REG_CTUR, 0xff);
    bar3_km_write(qts, KM_REG_CTLR, 0xff);
    bar3_km_write(qts, KM_REG_IVR, 0x40);
    bar3_km_write(qts, KM_REG_OPR_SET, 0x01);
    bar3_km_write(qts, KM_REG_OPR_RESET, 0x01);

    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRA_CSRA), ==, 0);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRB_CSRB), ==, 0);

    qtest_quit(qts);
}

/*
 * Channel A is fixed to the keyboard (include/hw/input/psitech-rad4-km.h).
 * Writing the PS/2 reset command to its data register must return the same
 * two-byte ACK/POR sequence hw/input/ps2.c's ps2_write_keyboard() queues for
 * KBD_CMD_RESET (0xfa, 0xaa), and RXRDY must track the queue draining.
 */
static void test_km_kbd_ps2_reset_round_trip(void)
{
    QTestState *qts = rad4km_init();

    bar3_km_write(qts, KM_REG_CRA, KM_CR_RX_ENABLE | KM_CR_TX_ENABLE);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRA_CSRA), ==, KM_SR_TXRDY);

    bar3_km_write(qts, KM_REG_RHRA_THRA, 0xff);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRA_CSRA), ==,
                    KM_SR_TXRDY | KM_SR_RXRDY);

    g_assert_cmphex(bar3_km_read(qts, KM_REG_RHRA_THRA), ==, 0xfa);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_RHRA_THRA), ==, 0xaa);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRA_CSRA), ==, KM_SR_TXRDY);

    qtest_quit(qts);
}

/*
 * Channel B is fixed to the mouse. AUX_RESET (also 0xff) queues three bytes
 * (hw/input/ps2.c: AUX_ACK, 0xaa, then mouse_type, which resets to 0), one
 * more than the keyboard's reply.
 */
static void test_km_mouse_ps2_reset_round_trip(void)
{
    QTestState *qts = rad4km_init();

    bar3_km_write(qts, KM_REG_CRB, KM_CR_RX_ENABLE | KM_CR_TX_ENABLE);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRB_CSRB), ==, KM_SR_TXRDY);

    bar3_km_write(qts, KM_REG_RHRB_THRB, 0xff);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRB_CSRB), ==,
                    KM_SR_TXRDY | KM_SR_RXRDY);

    g_assert_cmphex(bar3_km_read(qts, KM_REG_RHRB_THRB), ==, 0xfa);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_RHRB_THRB), ==, 0xaa);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_RHRB_THRB), ==, 0x00);
    g_assert_cmphex(bar3_km_read(qts, KM_REG_SRB_CSRB), ==, KM_SR_TXRDY);

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
 * Enables bus mastering and this slot's Direct-window byte-lane correction,
 * then writes DMACSR0 (ROM_COMMAND) to start (or abort) a transfer. Real
 * firmware programs a working device's DMA for the corrected case. The
 * disabled case leaves the crossed wiring this chip's DMA path otherwise
 * faithfully emulates, which tests/qtest/sn0-qlogic-isp-test.c exercises
 * directly. rad4_window_to()'s own writes to the slot's device register
 * never set this bit, so it is set again immediately before every start or
 * abort this file issues rather than once in rad4_init().
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
     * cleanly.
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

/*
 * Plain RAD4 never realizes the km child (hw/display/psitech-rad4.c's
 * rad4_realize() only registers BAR 3 under the RAD4-KM identity), so its BAR
 * 3 config-space register was never given a decode: an unregistered BAR's
 * bytes stay hardwired at 0, not a size mask, and a write to them is dropped.
 */
static void test_plain_rad4_has_no_km_bar3(void)
{
    QTestState *qts = rad4_init();

    qtest_writel(qts, CFG0(RAD4_SLOT) + PCI_BAR3_CFG_OFF, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, CFG0(RAD4_SLOT) + PCI_BAR3_CFG_OFF),
                    ==, 0);

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
 * driver's own bit-bang convention: outgoing data is stable before the rising
 * edge, incoming data is read after the falling edge.
 */
static bool serial_clock_bit(QTestState *qts, uint32_t *ctrl, bool mosi)
{
    uint32_t base = (*ctrl & ~(SERIAL_CLOCK | SERIAL_MOSI)) |
                    (mosi ? SERIAL_MOSI : 0);

    bar0_write(qts, R_SERIAL, base);                /* clock low, set MOSI */
    bar0_write(qts, R_SERIAL, base | SERIAL_CLOCK);  /* rising: sample MOSI */
    bar0_write(qts, R_SERIAL, base);                 /* falling edge: MISO */
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

    /* Start bit, then EWDS (00 00xxxx): a command that returns no data. */
    static const int bad_bits[9] = { 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 9; i++) {
        serial_clock_bit(qts, &ctrl, bad_bits[i]);
    }

    /* Only READ drives data-out low; every other command leaves it high. */
    bool bit = serial_clock_bit(qts, &ctrl, 0);
    g_assert_true(bit);

    qtest_quit(qts);
}

/*
 * The serial EEPROM is a 93C46-type 64x16 part: after a start bit, two
 * opcode bits and six address bits. These helpers drive it the way the
 * driver's own EEPROM routines do: chip select high, the command, then data
 * clocked MSB first.
 */
#define EE_READ(word)   (0x80 | (word))
#define EE_WRITE(word)  (0x40 | (word))
#define EE_EWEN         0x30
#define EE_EWDS         0x00

/* The driver's configuration record: 40 bytes from byte 0x58, word 0x2c. */
#define EE_RECORD_WORD  0x2c
#define EE_RECORD_WORDS 20

static void ee_select(QTestState *qts, uint32_t *ctrl, bool on)
{
    *ctrl = on ? SERIAL_CS : 0;
    bar0_write(qts, R_SERIAL, *ctrl);
}

/* Clock out a start bit and an 8-bit command; returns the last sample. */
static bool ee_command(QTestState *qts, uint32_t *ctrl, uint8_t command)
{
    bool miso;

    serial_clock_bit(qts, ctrl, 1);
    for (int i = 7; i >= 0; i--) {
        miso = serial_clock_bit(qts, ctrl, (command >> i) & 1);
    }
    return miso;
}

static void ee_simple_command(QTestState *qts, uint8_t command)
{
    uint32_t ctrl;

    ee_select(qts, &ctrl, true);
    ee_command(qts, &ctrl, command);
    ee_select(qts, &ctrl, false);
}

/*
 * A sequential READ of @count words, as the driver reads its record in one
 * chip-select period: the dummy zero is visible right after the last
 * address bit, then each data bit after its clock's falling edge.
 */
static void ee_read(QTestState *qts, uint8_t word, uint16_t *out,
                    unsigned count)
{
    uint32_t ctrl;

    ee_select(qts, &ctrl, true);
    g_assert_false(ee_command(qts, &ctrl, EE_READ(word)));
    for (unsigned i = 0; i < count; i++) {
        out[i] = 0;
        for (int bit = 0; bit < 16; bit++) {
            out[i] = (out[i] << 1) | serial_clock_bit(qts, &ctrl, 0);
        }
    }
    ee_select(qts, &ctrl, false);
}

/*
 * WRITE one word, then raise chip select again and sample data-out without
 * clocking, the driver's busy poll: one means programming has finished.
 */
static void ee_write(QTestState *qts, uint8_t word, uint16_t value)
{
    uint32_t ctrl;

    ee_select(qts, &ctrl, true);
    ee_command(qts, &ctrl, EE_WRITE(word));
    for (int bit = 15; bit >= 0; bit--) {
        serial_clock_bit(qts, &ctrl, (value >> bit) & 1);
    }
    ee_select(qts, &ctrl, false);
    ee_select(qts, &ctrl, true);
    g_assert_cmphex(bar0_read(qts, R_SERIAL) & SERIAL_MISO, ==, SERIAL_MISO);
    ee_select(qts, &ctrl, false);
}

/*
 * The record the driver writes back after changing to @mode: its 32-bit
 * request word in the first four record bytes, each byte pair shifted out
 * odd byte first, so word 0x2d carries the index in its high byte. The
 * other 36 bytes are what it read at attach, zero here.
 */
static void ee_expected_mode_record(uint16_t *record, uint8_t mode)
{
    memset(record, 0, EE_RECORD_WORDS * sizeof(*record));
    record[1] = (uint16_t)mode << 8;
}

static void ee_write_record(QTestState *qts, const uint16_t *record)
{
    ee_simple_command(qts, EE_EWEN);
    for (unsigned i = 0; i < EE_RECORD_WORDS; i++) {
        ee_write(qts, EE_RECORD_WORD + i, record[i]);
    }
    ee_simple_command(qts, EE_EWDS);
}

static void test_serial_eeprom_write(void)
{
    QTestState *qts = rad4_init();
    uint16_t expected[EE_RECORD_WORDS], record[EE_RECORD_WORDS];
    uint16_t first[2];

    /* The default image is all zero: mode 0, every option bit clear. */
    ee_read(qts, EE_RECORD_WORD, record, EE_RECORD_WORDS);
    ee_expected_mode_record(expected, 0);
    g_assert_cmpmem(record, sizeof(record), expected, sizeof(expected));

    /* The part powers up write-disabled: a WRITE alone changes nothing. */
    ee_write(qts, EE_RECORD_WORD + 1, 0x0700);
    ee_read(qts, EE_RECORD_WORD, record, EE_RECORD_WORDS);
    g_assert_cmpmem(record, sizeof(record), expected, sizeof(expected));

    ee_expected_mode_record(expected, 7);
    ee_write_record(qts, expected);
    ee_read(qts, EE_RECORD_WORD, record, EE_RECORD_WORDS);
    g_assert_cmpmem(record, sizeof(record), expected, sizeof(expected));

    /* EWDS closed writing again. */
    ee_write(qts, EE_RECORD_WORD + 1, 0x0b00);
    ee_read(qts, EE_RECORD_WORD + 1, first, 1);
    g_assert_cmphex(first[0], ==, 0x0700);

    /* A sequential read runs past the last word into word 0. */
    ee_read(qts, 0x3f, first, 2);
    g_assert_cmphex(first[0], ==, 0);
    g_assert_cmphex(first[1], ==, 0);

    /* The contents are nonvolatile: a system reset keeps them. */
    qtest_system_reset(qts);
    rad4_configure_bars(qts);
    ee_read(qts, EE_RECORD_WORD, record, EE_RECORD_WORDS);
    g_assert_cmpmem(record, sizeof(record), expected, sizeof(expected));

    qtest_quit(qts);
}

static QTestState *rad4_init_eeprom_drive(const char *path)
{
    QTestState *qts = qtest_initf("-M origin200"
                                  " -drive if=none,id=rad4ee,format=raw,"
                                  "file=%s"
                                  " -device psitech-rad4,addr="
                                  G_STRINGIFY(RAD4_SLOT) ",eeprom=rad4ee",
                                  path);

    rad4_configure_bars(qts);
    return qts;
}

/*
 * With a backing drive, the image is read at realize and every programmed
 * word is written through, so a second QEMU on the same file reads it back.
 * The image stores each word most significant byte first.
 */
static void test_serial_eeprom_backing_drive(void)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    uint16_t expected[EE_RECORD_WORDS], record[EE_RECORD_WORDS];
    uint8_t image[128] = { 0 };
    uint16_t word;
    QTestState *qts;
    gsize length;
    int fd;

    fd = g_file_open_tmp("psitech-rad4-eeprom-XXXXXX.img", &path, &error);
    g_assert_no_error(error);
    close(fd);
    image[0x00] = 0x12;
    image[0x01] = 0x34;
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), &error));

    qts = rad4_init_eeprom_drive(path);
    ee_read(qts, 0, &word, 1);
    g_assert_cmphex(word, ==, 0x1234);
    ee_expected_mode_record(expected, 11);
    ee_write_record(qts, expected);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &contents, &length, &error));
    g_assert_cmpuint(length, ==, sizeof(image));
    g_assert_cmphex((uint8_t)contents[0x5a], ==, 11);
    g_assert_cmphex((uint8_t)contents[0x5b], ==, 0);

    qts = rad4_init_eeprom_drive(path);
    ee_read(qts, EE_RECORD_WORD, record, EE_RECORD_WORDS);
    g_assert_cmpmem(record, sizeof(record), expected, sizeof(expected));
    qtest_quit(qts);

    unlink(path);
}

static void test_framebuffer_mask(void)
{
    QTestState *qts = rad4_init();

    /* Full mask by default: an ordinary pixel write is stored whole. */
    bar2_write(qts, 0x000000, 0x00ff0080u);
    g_assert_cmphex(bar2_read(qts, 0x000000), ==, 0x00ff0080u);

    /*
     * A partial mask is a plane mask: only the masked bits of the write take
     * effect, the rest of the existing pixel is preserved.
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
 * The address index register auto increments "after each read or write cycle"
 * (IBM RGB561 Architecture p.1), a convention this model applies to the
 * ordinary single-cycle B_RGB_DATA port on write already. This checks the
 * read side, previously missing: a real fidelity gap, though harmless for the
 * qualified guest workload, since nothing there reads this port back.
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
 * carries bits 9-2 in bit positions 7-0, the second carries bits 1-0 in bit
 * positions 7-6. Writing and reading one entry back must round-trip, and the
 * address index register must auto increment only after the second (last)
 * cycle, to the next window ID's entry.
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
 * multi-cycle transfer at its first cycle (IBM RGB561 Architecture p.1: "If
 * the address index register is accessed before completion of a multi-cycle
 * access, the counter is reset and the operation is terminated.").
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
 * The color look-up table is a 3 cycle (red, green, blue) transfer through
 * the same port (same document, p.2, "Color Look-Up Table (LUT)"). Entry 0
 * corresponds to color LUT address 0.
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

/*
 * Mode-zero geometry restated for the dirty-tracking test below, the way this
 * file restates every other register layout rather than including
 * hw/display/psitech-rad4.c's own RAD4_MODE0_* constants.
 */
#define TEST_FB_WIDTH   1280
#define TEST_FB_HEIGHT  1024
#define TEST_FB_STRIDE  (TEST_FB_WIDTH * 4)

/*
 * rad4_gfx_update() (the GraphicHwOps refresh callback) only runs when
 * something calls graphic_hw_update(); QMP screendump is the qtest-reachable
 * way to force that without a real display backend attached.
 */
static void rad4_force_refresh(QTestState *qts)
{
    g_autofree char *path = NULL;
    g_autoptr(GError) error = NULL;
    int fd = g_file_open_tmp("psitech-rad4-screendump-XXXXXX.ppm", &path,
                             &error);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    qtest_qmp_assert_success(qts,
        "{'execute': 'screendump', 'arguments': {'filename': %s}}", path);
    unlink(path);
}

/*
 * rad4_gfx_update() used to reconvert and push the whole 1280x1024 frame on
 * every refresh regardless of whether the guest had written anything since
 * the last one, which saturated the VNC path into flicker, blackout and laggy
 * input on an otherwise-static screen. It now tracks which scanlines were
 * actually written and pushes only those, doing nothing at all when idle; the
 * rad4_gfx_update_push/rad4_gfx_update_idle trace events (only built for this
 * test's own use, not otherwise consumed) are the qtest-reachable window into
 * that, since the refresh callback has no other externally visible effect
 * besides the pixels it composes into the surface. A static framebuffer
 * reconverts to identical pixels whether or not this fix is present, so a
 * screendump alone cannot tell the two apart.
 */
static void test_gfx_update_dirty_tracking(void)
{
    g_autofree char *trace_path = NULL;
    g_autofree char *trace_contents = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("psitech-rad4-gfx-update-XXXXXX.trace", &trace_path,
                         &error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf(MACHINE_WITH_BRIDGE
                      " -trace enable=rad4_gfx_update_push"
                      " -trace enable=rad4_gfx_update_idle"
                      " -D %s", trace_path);
    rad4_configure_bars(qts);

    /*
     * Recognizing the mode marks the whole frame dirty for its first paint.
     */
    rad4_play_mode0(qts);
    rad4_force_refresh(qts);

    /* Nothing has changed since: the next refresh finds nothing dirty. */
    rad4_force_refresh(qts);

    /* A write into row 5 alone should only dirty row 5. */
    bar2_write(qts, 5 * TEST_FB_STRIDE, 0x00ff0080u);
    rad4_force_refresh(qts);

    qtest_quit(qts);

    g_assert_true(g_file_get_contents(trace_path, &trace_contents, NULL,
                                      &error));
    unlink(trace_path);

    {
        g_auto(GStrv) lines = g_strsplit(trace_contents, "\n", -1);
        int full_push_at = -1, idle_at = -1, row_push_at = -1;

        for (int i = 0; lines[i]; i++) {
            if (strstr(lines[i], "rad4_gfx_update_push")) {
                if (full_push_at < 0) {
                    full_push_at = i;
                    /* TEST_FB_HEIGHT - 1: the whole frame, first paint. */
                    g_assert_nonnull(strstr(lines[i], "rows 0-1023"));
                } else {
                    row_push_at = i;
                }
            } else if (strstr(lines[i], "rad4_gfx_update_idle") &&
                      idle_at < 0) {
                idle_at = i;
            }
        }

        g_assert_cmpint(full_push_at, >=, 0);
        g_assert_cmpint(idle_at, >, full_push_at);
        g_assert_cmpint(row_push_at, >, idle_at);
        g_assert_nonnull(strstr(lines[row_push_at], "rows 5-5"));
    }
}

/*
 * A pixel's displayed color depends on more than its own raw framebuffer
 * word: rad4_pixel_to_rgb() also consults the written window's FB_WAT entry
 * and, in Index mode, the color LUT. Neither table lives in s->framebuffer,
 * so a write to either one never touched the dirty tracking above. A guest
 * that reprograms colors or a window's interpretation without also touching
 * the framebuffer got no visible update at all until some unrelated pixel
 * write came along. This drives one FB_WAT entry through its full two-cycle
 * write with no framebuffer write anywhere nearby, and expects the completed
 * write alone to push a frame rather than leave the next refresh idle.
 */
static void test_gfx_update_palette_change_without_fb_write(void)
{
    g_autofree char *trace_path = NULL;
    g_autofree char *trace_contents = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("psitech-rad4-palette-dirty-XXXXXX.trace",
                         &trace_path, &error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf(MACHINE_WITH_BRIDGE
                      " -trace enable=rad4_gfx_update_push"
                      " -trace enable=rad4_gfx_update_idle"
                      " -D %s", trace_path);
    rad4_configure_bars(qts);

    rad4_play_mode0(qts);
    rad4_force_refresh(qts); /* First paint: consumes the full-frame range. */

    bar2_write(qts, 100 * TEST_FB_STRIDE, 0x00ff0080u);
    rad4_force_refresh(qts); /* Pushes row 100 alone, clearing the range. */

    /* FB_WAT entry 5's full two-cycle write; no framebuffer write at all. */
    rad4_set_rgb_index(qts, WAT_BASE + 5);
    bar2_write(qts, B_RGB_RESERVED, 0xd8);
    bar2_write(qts, B_RGB_RESERVED, 0x00);
    rad4_force_refresh(qts);

    qtest_quit(qts);

    g_assert_true(g_file_get_contents(trace_path, &trace_contents, NULL,
                                      &error));
    unlink(trace_path);

    {
        g_auto(GStrv) lines = g_strsplit(trace_contents, "\n", -1);
        int push_count = 0, idle_count = 0, last_push_at = -1;

        for (int i = 0; lines[i]; i++) {
            if (strstr(lines[i], "rad4_gfx_update_push")) {
                push_count++;
                last_push_at = i;
            } else if (strstr(lines[i], "rad4_gfx_update_idle")) {
                idle_count++;
            }
        }

        /*
         * Three refreshes, three pushes: the full first paint, row 100 alone,
         * then the FB_WAT write alone. Without dirty marking on that
         * write path, the old code made the third refresh idle.
         */
        g_assert_cmpint(push_count, ==, 3);
        g_assert_cmpint(idle_count, ==, 0);
        g_assert_nonnull(strstr(lines[last_push_at], "rows 0-1023"));
    }
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
     * Plain RAD4 never realizes the km child (rad4_realize()), so this
     * identity's shared PCI line is driven solely by INTCSR here --
     * confirming rad4_update_irq()'s km_irq_level OR-in leaves this
     * pre-existing path unchanged.
     */
    g_assert_cmphex(qtest_readl(qts, CFG0(RAD4_SLOT) + PCI_COMMAND_OFF) &
                    PCI_STATUS_INTERRUPT, ==, PCI_STATUS_INTERRUPT);

    /*
     * Acknowledge: a control write with bit 1 clear while a frame is pending
     * clears both status bits.
     */
    bar2_write(qts, B_CTRL, 0);
    g_assert_cmphex(bar0_read(qts, R_INTCSR) & INTCSR_PENDING, ==, 0);
    g_assert_cmphex(bar2_read(qts, B_CTRL) & STATUS_FRAME, ==, 0);
    g_assert_cmphex(qtest_readl(qts, CFG0(RAD4_SLOT) + PCI_COMMAND_OFF) &
                    PCI_STATUS_INTERRUPT, ==, 0);

    qtest_quit(qts);
}

/*
 * Writing the mode and descriptor-pointer registers without
 * writing DMACSR0's start bits only latches the value back for readback,
 * matching every other configuration-only register this file already
 * round-trips (R_REG14, R_REG18, R_MAILBOX).
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
 * A single, end-of-chain descriptor uploads its named span of guest memory
 * into the framebuffer at its named local address, and DMACSR0's Done bit
 * follows.
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
 * The plane mask (RAD4_BAR2_MASK) applies to a DMA upload exactly as it does
 * to a CPU pixel write (test_framebuffer_mask): the mask register is
 * documented as programmed before a transfer and restored afterward, naming
 * both driver render paths that share this device's DMA engine, not a
 * CPU-only feature of the memory controller it sits in front of.
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
 * next-descriptor-pointer word (PLX PCI9054 Data Book Table 11-80), and both
 * land at independent local framebuffer addresses. This is the
 * mechanism the driver uses, building one descriptor per page-sized run of a
 * transfer, not one flat descriptor for the whole span.
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
 * honoring the same convention the IRIX 6.5.30 buffered driver path uses: a
 * descriptor stored at the very top of the 8 MiB local extent. The
 * descriptor's own storage location is Local; the payload source it names is
 * still an ordinary PCI address, exactly as that driver's own descriptor is
 * laid out.
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
 * next-descriptor-pointer word naming the second exactly as the driver's
 * descriptor array does (each entry 16 bytes below the one DMADPR0 or the
 * previous entry's next-pointer named).
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
 * framebuffer) leaves DMACSR0's Done bit clear, exactly as the unmodeled-DMA
 * build already left it before this transfer existed at all. The guest's
 * own timeout/abort recovery, exercised then by every buffered-mode draw, is
 * exercised here by hand. Writing ABORT (0x4) still sets Done, matching PLX
 * PCI9054 Data Book Table 11-86 ("Sets Channel 0 Done bit when abort is
 * complete"), so that recovery path keeps working for a transfer this model
 * genuinely cannot complete.
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
     * The cursor pixel map is a plain 1Kx8 SRAM (IBM page 27 lists it without
     * the "N RW/ADDR" annotation the look-up tables carry), so writing it
     * goes through the ordinary auto-incrementing data port and advances the
     * index by exactly one per byte.
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
     * The cursor/cross-hair look-up table is a three-writes-per-address ("3
     * RW/ADDR") port reached through BAR2 +0x930000, not the ordinary data
     * port: point the index at Cursor Primary Color 1 (IBM page 45, Table 22)
     * and issue three writes there without touching the index registers in
     * between, matching the shipped driver's own init_RGB561 sequence.
     */
    rgb_set_index(qts, RGB_CURSOR_LUT_BASE + 1);
    bar2_write(qts, B_RGB_RESERVED, 0x11);
    bar2_write(qts, B_RGB_RESERVED, 0x22);
    bar2_write(qts, B_RGB_RESERVED, 0x33);

    /*
     * The visible index must have advanced by exactly one, to the next LUT
     * entry, not by three as an ordinary auto-incrementing range
     * would. This is what discriminates this port's three-writes-per-address
     * protocol from plain per-byte storage.
     */
    g_assert_cmphex(rgb_current_index(qts), ==, RGB_CURSOR_LUT_BASE + 2);

    /*
     * Reading the three components back in order, at the original address,
     * confirms they landed in three separate planes of one entry rather than
     * at three separate addresses.
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

/* One window attribute table entry, in the two cycles of page 4's Table 5. */
static void rgb_write_wat(QTestState *qts, uint16_t base, uint8_t entry,
                          uint16_t value)
{
    rgb_set_index(qts, base + entry);
    bar2_write(qts, B_RGB_RESERVED, value >> 2);
    bar2_write(qts, B_RGB_RESERVED, (value & 0x3) << 6);
}

static void rgb_write_lut(QTestState *qts, uint16_t entry, uint32_t rgb)
{
    rgb_set_index(qts, COLOR_LUT_BASE + entry);
    bar2_write(qts, B_RGB_RESERVED, (rgb >> 16) & 0xff);
    bar2_write(qts, B_RGB_RESERVED, (rgb >> 8) & 0xff);
    bar2_write(qts, B_RGB_RESERVED, rgb & 0xff);
}

/* One displayed pixel, as 0xRRGGBB, from a fresh QMP screendump. */
static uint32_t rad4_screendump_pixel(QTestState *qts, unsigned x, unsigned y)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    char *pixels;
    gsize length;
    int fd;

    fd = g_file_open_tmp("psitech-rad4-pixel-XXXXXX.ppm", &path, &error);
    g_assert_no_error(error);
    close(fd);
    qtest_qmp_assert_success(qts,
        "{'execute': 'screendump', 'arguments': {'filename': %s}}", path);
    g_assert_true(g_file_get_contents(path, &contents, &length, &error));
    g_assert_no_error(error);
    unlink(path);
    g_assert_true(g_str_has_prefix(contents, "P6\n1280 1024\n255\n"));
    pixels = g_strstr_len(contents, length, "\n255\n") + 5;
    g_assert_cmpuint(x, <, TEST_FB_WIDTH);
    g_assert_cmpuint(y, <, TEST_FB_HEIGHT);
    g_assert_cmpuint((gsize)(pixels - contents) +
                     TEST_FB_WIDTH * TEST_FB_HEIGHT * 3, <=, length);
    pixels += (y * TEST_FB_WIDTH + x) * 3;
    return ((uint8_t)pixels[0] << 16) | ((uint8_t)pixels[1] << 8) |
           (uint8_t)pixels[2];
}

/*
 * The RGB561 overlays its hardware cursor onto the pixel stream it sends to
 * the monitor, so the sprite is part of what the display shows and the model
 * composites it into the scanout the same way. That is also the only way a
 * VNC client or a screendump can show where the guest's cursor actually is:
 * QEMU's VNC server has no protocol message to place a UI-layer cursor, so a
 * client draws that kind at its own pointer instead. Program a one-pixel
 * sprite and expect the screendump to carry it at the hot spot; moving it
 * must both paint the new spot and give the old one its framebuffer content
 * back, which is the dirty tracking of both spans; the hot spot offsets the
 * pixel map; disabling removes it.
 */
static void test_cursor_composited_into_scanout(void)
{
    QTestState *qts = rad4_init();
    uint32_t under_first, under_second, under_shifted;

    rad4_play_mode0(qts);

    /* Cursor Primary Color 1 = white, through the three-cycle port. */
    rgb_set_index(qts, RGB_CURSOR_LUT_BASE + 1);
    bar2_write(qts, B_RGB_RESERVED, 0xff);
    bar2_write(qts, B_RGB_RESERVED, 0xff);
    bar2_write(qts, B_RGB_RESERVED, 0xff);
    /*
     * Pixel map row 0, first byte: the leftmost pixel of the four is the
     * most-significant pair (IBM page 3, Table 4), so 0x40 makes pixel (0, 0)
     * color 1 and leaves the rest of the sprite transparent.
     */
    rgb_write_byte(qts, RGB_CURSOR_PIXMAP_BASE, 0x40);
    rgb_write_byte(qts, RGB_CURSOR_HOTSPOT_X, 0);
    rgb_write_byte(qts, RGB_CURSOR_HOTSPOT_Y, 0);
    rgb_write_byte(qts, RGB_CURSOR_X_LOW, 100);
    rgb_write_byte(qts, RGB_CURSOR_X_HIGH, 0);
    rgb_write_byte(qts, RGB_CURSOR_Y_LOW, 200);
    rgb_write_byte(qts, RGB_CURSOR_Y_HIGH, 0);

    /* Still disabled: the framebuffer shows through. */
    under_first = rad4_screendump_pixel(qts, 100, 200);
    g_assert_cmphex(under_first, !=, 0xffffff);

    rgb_write_byte(qts, RGB_CURSOR_CONTROL, CURSOR_CONTROL_CE);
    g_assert_cmphex(rad4_screendump_pixel(qts, 100, 200), ==, 0xffffff);
    /* A transparent sprite pixel leaves the framebuffer alone. */
    g_assert_cmphex(rad4_screendump_pixel(qts, 101, 200), ==, under_first);
    under_second = rad4_screendump_pixel(qts, 300, 400);
    under_shifted = rad4_screendump_pixel(qts, 299, 400);

    /* Move to (300, 400): the old spot reverts, the new one shows it. */
    rgb_write_byte(qts, RGB_CURSOR_X_LOW, 300 & 0xff);
    rgb_write_byte(qts, RGB_CURSOR_X_HIGH, 300 >> 8);
    rgb_write_byte(qts, RGB_CURSOR_Y_LOW, 400 & 0xff);
    rgb_write_byte(qts, RGB_CURSOR_Y_HIGH, 400 >> 8);
    g_assert_cmphex(rad4_screendump_pixel(qts, 100, 200), ==, under_first);
    g_assert_cmphex(rad4_screendump_pixel(qts, 300, 400), ==, 0xffffff);

    /* The hot spot is the pixel-map cell placed at the location. */
    rgb_write_byte(qts, RGB_CURSOR_HOTSPOT_X, 1);
    g_assert_cmphex(rad4_screendump_pixel(qts, 300, 400), ==, under_second);
    g_assert_cmphex(rad4_screendump_pixel(qts, 299, 400), ==, 0xffffff);

    rgb_write_byte(qts, RGB_CURSOR_CONTROL, 0);
    g_assert_cmphex(rad4_screendump_pixel(qts, 299, 400), ==, under_shifted);

    qtest_quit(qts);
}

/*
 * A refresh whose dirty rows lie entirely outside the sprite must not lose
 * the record of where the sprite is currently painted. The compose pass runs
 * on every refresh, including ones driven by a framebuffer write far from the
 * cursor; if such a pass forgot the sprite's displayed span, a later cursor
 * change with no new rows of its own to repaint (a disable, or a hot
 * spot or look-up-table change that leaves the location alone) would fail
 * to dirty the old rows and leave a ghost sprite behind in the scanout.
 *
 * Disabling is the sharpest case: it dirties the recorded displayed span and
 * nothing else, so it clears the sprite only if that span survived the
 * unrelated refresh. (A plain position move would mask the bug: moving takes
 * four separate byte writes, and the intermediate locations re-dirty the old
 * rows by coincidence.) Compose the sprite, write a framebuffer row the
 * 64-row sprite does not cover and refresh, then disable the cursor and
 * confirm the old spot gets its framebuffer content back.
 */
static void test_cursor_no_ghost_after_far_fb_update(void)
{
    QTestState *qts = rad4_init();
    uint32_t under_old;

    rad4_play_mode0(qts);

    /* Cursor Primary Color 1 = white; one sprite pixel at map (0, 0). */
    rgb_set_index(qts, RGB_CURSOR_LUT_BASE + 1);
    bar2_write(qts, B_RGB_RESERVED, 0xff);
    bar2_write(qts, B_RGB_RESERVED, 0xff);
    bar2_write(qts, B_RGB_RESERVED, 0xff);
    rgb_write_byte(qts, RGB_CURSOR_PIXMAP_BASE, 0x40);
    rgb_write_byte(qts, RGB_CURSOR_HOTSPOT_X, 0);
    rgb_write_byte(qts, RGB_CURSOR_HOTSPOT_Y, 0);
    rgb_write_byte(qts, RGB_CURSOR_X_LOW, 100);
    rgb_write_byte(qts, RGB_CURSOR_X_HIGH, 0);
    rgb_write_byte(qts, RGB_CURSOR_Y_LOW, 200);
    rgb_write_byte(qts, RGB_CURSOR_Y_HIGH, 0);

    under_old = rad4_screendump_pixel(qts, 100, 200);
    g_assert_cmphex(under_old, !=, 0xffffff);

    /* Enable it and compose it once, so its displayed span is on record. */
    rgb_write_byte(qts, RGB_CURSOR_CONTROL, CURSOR_CONTROL_CE);
    g_assert_cmphex(rad4_screendump_pixel(qts, 100, 200), ==, 0xffffff);

    /*
     * An unrelated framebuffer write on row 900, far below the sprite's
     * rows 200..263, followed by a refresh. Its dirty interval never
     * touches the cursor, which is exactly the case that must not forget it.
     */
    bar2_write(qts, 900 * TEST_FB_STRIDE, 0x00112233u);
    rad4_force_refresh(qts);

    /* Disabling restores the sprite's rows only if the span was preserved. */
    rgb_write_byte(qts, RGB_CURSOR_CONTROL, 0);
    g_assert_cmphex(rad4_screendump_pixel(qts, 100, 200), ==, under_old);

    qtest_quit(qts);
}

/*
 * OL_WAT has the same 10-bit, two-cycle transfer as FB_WAT at its own index
 * range (IBM RGB561 Architecture pages 4, 27 and 48), and the address index
 * register auto increments after the second cycle.
 */
static void test_ol_wat_round_trip(void)
{
    QTestState *qts = rad4_init();

    rgb_write_wat(qts, OL_WAT_BASE, 7, 0x2b5);

    rgb_set_index(qts, OL_WAT_BASE + 7);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0x2b5 >> 2);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, (0x2b5 & 3) << 6);
    g_assert_cmphex(rgb_current_index(qts), ==, OL_WAT_BASE + 8);

    /* The FB_WAT entry with the same number is a separate table. */
    rgb_set_index(qts, WAT_BASE + 7);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0);

    qtest_quit(qts);
}

/* The X server's RAMDAC setup, with one 4-bit index overlay entry. */
static void rad4_setup_overlay(QTestState *qts, uint16_t ol_wat)
{
    rad4_play_mode0(qts);
    rgb_write_byte(qts, RGB_CONF1, CONF1_4TO1_OVLY8_LWID8);
    rgb_write_byte(qts, RGB_CONF4, CONF4_SPLIT_FB8);
    rgb_write_byte(qts, RGB_CHROMA_KEY0, 0x00);
    rgb_write_byte(qts, RGB_CHROMA_MASK0, 0x0f);
    rgb_write_byte(qts, RGB_AUX_OL_WAT, AUX_OL_WAT_CK0_ENABLED);
    for (unsigned wid = 0; wid < 0x100; wid++) {
        rgb_write_wat(qts, WAT_BASE, wid, FB_WAT_TRUE_24);
    }
    rgb_write_wat(qts, OL_WAT_BASE, 0, ol_wat);
    /* Overlay value 5 with START_ADDR 2 is color LUT entry 2 * 64 + 5. */
    rgb_write_lut(qts, 2 * 64 + 5, 0x112233);
}

/*
 * A Motif menu on the IRIX desktop is drawn through a 4-bit overlay visual
 * into bits 24-27 of each pixel and leaves the true-color bits underneath
 * alone, so the menu shows only if the scanout combines the overlay with the
 * frame buffer the way the RAMDAC does (IBM RGB561 Architecture pages 9,
 * 15-21, 32, 46 and 48). A nonzero overlay value shows its color LUT entry,
 * overlay value 0 matches chroma key 0 and lets the frame buffer through,
 * disabling the overlay in AUX_OL_WAT shows the frame buffer everywhere, and
 * buffer B takes bits 28-31 instead.
 */
static void test_overlay_composited_into_scanout(void)
{
    QTestState *qts = rad4_init();

    rad4_setup_overlay(qts, OL_WAT_4BIT_INDEX(2, 0));
    bar2_write(qts, 10 * TEST_FB_STRIDE + 10 * 4, 0x05abcdefu);
    bar2_write(qts, 10 * TEST_FB_STRIDE + 11 * 4, 0x00abcdefu);
    bar2_write(qts, 10 * TEST_FB_STRIDE + 12 * 4, 0x50abcdefu);

    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 10), ==, 0x112233);
    g_assert_cmphex(rad4_screendump_pixel(qts, 11, 10), ==, 0xabcdef);
    /* Bits 28-31 select the window, not buffer A's overlay data. */
    g_assert_cmphex(rad4_screendump_pixel(qts, 12, 10), ==, 0xabcdef);

    /*
     * Disabling the overlay changes no frame buffer word, so the scanout
     * must notice the AUX_OL_WAT write itself.
     */
    rgb_write_byte(qts, RGB_AUX_OL_WAT, AUX_OL_WAT_CK0_DISABLED);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 10), ==, 0xabcdef);
    rgb_write_byte(qts, RGB_AUX_OL_WAT, AUX_OL_WAT_CK0_ENABLED);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 10), ==, 0x112233);

    /* Buffer B: bits 28-31 carry the overlay data. */
    rgb_write_wat(qts, OL_WAT_BASE, 0, OL_WAT_4BIT_INDEX(2, 1));
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 10), ==, 0xabcdef);
    g_assert_cmphex(rad4_screendump_pixel(qts, 12, 10), ==, 0x112233);

    qtest_quit(qts);
}

/*
 * Overlay transparency compares the overlay data with chroma key 0 under its
 * mask, and only when the OL_WAT entry enables transparency (pages 21, 46
 * and 48). With transparency off, even value 0 is an opaque overlay color.
 */
static void test_overlay_chroma_key_and_opaque(void)
{
    QTestState *qts = rad4_init();

    rad4_setup_overlay(qts, OL_WAT_4BIT_INDEX(2, 0));
    rgb_write_lut(qts, 2 * 64 + 4, 0x445566);
    rgb_write_lut(qts, 2 * 64 + 0, 0x778899);
    bar2_write(qts, 20 * TEST_FB_STRIDE + 10 * 4, 0x05abcdefu);
    bar2_write(qts, 20 * TEST_FB_STRIDE + 11 * 4, 0x04abcdefu);
    bar2_write(qts, 20 * TEST_FB_STRIDE + 12 * 4, 0x00abcdefu);

    /*
     * Key 0 under mask 0x0f: only 0 is transparent. Taking this screendump
     * first means the key change below repaints only if the scanout
     * notices the key and mask writes themselves.
     */
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 20), ==, 0x112233);
    g_assert_cmphex(rad4_screendump_pixel(qts, 12, 20), ==, 0xabcdef);

    /* Key 4 under mask 0x0e makes both 4 and 5 transparent, and 0 opaque. */
    rgb_write_byte(qts, RGB_CHROMA_KEY0, 0x04);
    rgb_write_byte(qts, RGB_CHROMA_MASK0, 0x0e);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 20), ==, 0xabcdef);
    g_assert_cmphex(rad4_screendump_pixel(qts, 11, 20), ==, 0xabcdef);
    g_assert_cmphex(rad4_screendump_pixel(qts, 12, 20), ==, 0x778899);

    /* TR clear: no comparison at all. */
    rgb_write_wat(qts, OL_WAT_BASE, 0, WAT_ENTRY(2, 3, 0, 0, 0));
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 20), ==, 0x112233);
    g_assert_cmphex(rad4_screendump_pixel(qts, 11, 20), ==, 0x445566);

    qtest_quit(qts);
}

/*
 * Which OL_WAT entry a pixel uses depends on how CONF/4 shares the window-ID
 * bits (pages 15-17 and 32). Split with all 8 bits to the frame buffer, no
 * bits are left for the overlay and the OL_WAT segment register picks the
 * entry for every pixel. In common mode the whole window ID addresses
 * OL_WAT too, so the overlay value in bits 24-27 is part of the address.
 */
static void test_overlay_wat_addressing(void)
{
    QTestState *qts = rad4_init();

    rad4_setup_overlay(qts, OL_WAT_4BIT_INDEX(2, 0));
    rgb_write_wat(qts, OL_WAT_BASE, 3, OL_WAT_4BIT_INDEX(4, 0));
    rgb_write_wat(qts, OL_WAT_BASE, 5, OL_WAT_4BIT_INDEX(6, 0));
    rgb_write_lut(qts, 4 * 64 + 5, 0x445566);
    rgb_write_lut(qts, 6 * 64 + 5, 0x778899);
    bar2_write(qts, 30 * TEST_FB_STRIDE + 10 * 4, 0x05abcdefu);

    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 30), ==, 0x112233);
    rgb_write_byte(qts, RGB_OL_WAT_SEGMENT, 3);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 30), ==, 0x445566);

    /*
     * Common mode also addresses AUX_OL_WAT from the window ID's low four
     * bits, so entry 5 must enable the overlay as well.
     */
    rgb_write_byte(qts, RGB_CONF4, CONF4_COMMON);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 30), ==, 0xabcdef);
    rgb_write_byte(qts, RGB_AUX_OL_WAT + 5, AUX_OL_WAT_CK0_ENABLED);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 30), ==, 0x778899);

    qtest_quit(qts);
}

/*
 * The other WAT addressing shapes of Figures 2 and 3 (pages 16-17). Split
 * with 4 bits to the frame buffer, the overlay takes the upper four
 * window-ID bits for both OL_WAT and AUX_OL_WAT. In common mode with AOW
 * set, AUX_OL_WAT takes the window ID's upper four bits. With 4 logical
 * window-ID bits, only the low four reach the tables and the segment
 * register supplies the rest.
 */
static void test_overlay_wat_addressing_shapes(void)
{
    QTestState *qts = rad4_init();

    rad4_setup_overlay(qts, WAT_ENTRY(2, 3, 0, 0, 1));
    rgb_write_wat(qts, OL_WAT_BASE, 0x03, OL_WAT_4BIT_INDEX(4, 0));
    rgb_write_wat(qts, OL_WAT_BASE, 0x35, OL_WAT_4BIT_INDEX(6, 0));
    rgb_write_wat(qts, OL_WAT_BASE, 0x75, OL_WAT_4BIT_INDEX(8, 0));
    rgb_write_lut(qts, 4 * 64 + 5, 0x445566);
    rgb_write_lut(qts, 6 * 64 + 5, 0x778899);
    rgb_write_lut(qts, 8 * 64 + 5, 0xaabbcc);
    bar2_write(qts, 40 * TEST_FB_STRIDE + 10 * 4, 0x35abcdefu);

    /* AUX_OL_WAT 0 enables the overlay; 3 and 5 stay disabled. */
    rgb_write_byte(qts, RGB_CONF4, CONF4_SPLIT_FB4);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 40), ==, 0xabcdef);
    rgb_write_byte(qts, RGB_AUX_OL_WAT + 3, AUX_OL_WAT_CK0_ENABLED);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 40), ==, 0x445566);

    rgb_write_byte(qts, RGB_CONF4, CONF4_COMMON_AOW);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 40), ==, 0x778899);
    rgb_write_byte(qts, RGB_AUX_OL_WAT + 3, AUX_OL_WAT_CK0_DISABLED);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 40), ==, 0xabcdef);

    rgb_write_byte(qts, RGB_CONF1, CONF1_4TO1_OVLY8_LWID4);
    rgb_write_byte(qts, RGB_CONF4, CONF4_COMMON);
    rgb_write_byte(qts, RGB_OL_WAT_SEGMENT, 0x70);
    rgb_write_byte(qts, RGB_AUX_OL_WAT_SEGMENT, 0x0);
    rgb_write_byte(qts, RGB_AUX_OL_WAT + 5, AUX_OL_WAT_CK0_ENABLED);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 40), ==, 0xaabbcc);

    qtest_quit(qts);
}

/*
 * The frame-buffer pixel formats the shipped X server loads into FB_WAT
 * besides 24-bit true color and 8-bit index (IBM RGB561 Architecture pages
 * 9 and 19-21, 47): 8-, 12- and 16-bit true color widen R3G3B2, R4G4B4 and
 * R5G6B5 through Table 18's bypass conversion, Grey Scale shows an 8-bit
 * pixel on all three components, and buffer select takes a pixel from the
 * word's upper buffer. The window IDs have a zero low nibble, so no overlay
 * is involved.
 */
static void test_fb_pixel_formats(void)
{
    QTestState *qts = rad4_init();
    uint32_t row = 50 * TEST_FB_STRIDE;

    rad4_play_mode0(qts);
    rgb_write_byte(qts, RGB_CONF1, CONF1_4TO1_OVLY8_LWID8);
    rgb_write_byte(qts, RGB_CONF4, CONF4_SPLIT_FB8);
    rgb_write_wat(qts, WAT_BASE, 0x10, WAT_ENTRY(0, 0, 0, 3, 0));
    rgb_write_wat(qts, WAT_BASE, 0x20, WAT_ENTRY(0, 1, 0, 3, 0));
    rgb_write_wat(qts, WAT_BASE, 0x30, WAT_ENTRY(0, 2, 0, 3, 0));
    rgb_write_wat(qts, WAT_BASE, 0x40, WAT_ENTRY(0, 2, 1, 3, 0));
    rgb_write_wat(qts, WAT_BASE, 0x50, WAT_ENTRY(0, 0, 0, 1, 0));
    rgb_write_wat(qts, WAT_BASE, 0x60, WAT_ENTRY(1, 0, 1, 0, 0));
    rgb_write_lut(qts, 64 + 5, 0x123456);

    bar2_write(qts, row + 0 * 4, 0x100000e9u);  /* R 111, G 010, B 01 */
    bar2_write(qts, row + 1 * 4, 0x20000abcu);
    bar2_write(qts, row + 2 * 4, 0x30001234u);  /* R 00010, G 010001 */
    bar2_write(qts, row + 3 * 4, 0x40ab0000u);  /* buffer B is 0x40ab */
    bar2_write(qts, row + 4 * 4, 0x50000080u);
    bar2_write(qts, row + 5 * 4, 0x60000500u);  /* buffer B index 5 */

    g_assert_cmphex(rad4_screendump_pixel(qts, 0, 50), ==, 0xff4955);
    g_assert_cmphex(rad4_screendump_pixel(qts, 1, 50), ==, 0xaabbcc);
    g_assert_cmphex(rad4_screendump_pixel(qts, 2, 50), ==, 0x1045a5);
    g_assert_cmphex(rad4_screendump_pixel(qts, 3, 50), ==, 0x42145a);
    g_assert_cmphex(rad4_screendump_pixel(qts, 4, 50), ==, 0x808080);
    g_assert_cmphex(rad4_screendump_pixel(qts, 5, 50), ==, 0x123456);

    qtest_quit(qts);
}

/*
 * Gamma tables (IBM RGB561 Architecture pages 3, 27 and 46): each displayed
 * component addresses its own 256x10 table, loaded in two cycles like a WAT
 * entry, unless the pixel's auxiliary WAT entry bypasses gamma. With CONF/4's
 * AFW set, AUX_FB_WAT follows the window ID's upper four bits. The display
 * shows each 10-bit entry's upper 8 bits.
 */
static void test_gamma_tables(void)
{
    QTestState *qts = rad4_init();

    rad4_play_mode0(qts);
    rgb_write_byte(qts, RGB_CONF1, CONF1_4TO1_OVLY8_LWID8);
    rgb_write_byte(qts, RGB_CONF4, CONF4_SPLIT_FB8);
    rgb_write_wat(qts, WAT_BASE, 0x00, FB_WAT_TRUE_24);
    rgb_write_wat(qts, WAT_BASE, 0x40, FB_WAT_TRUE_24);
    bar2_write(qts, 60 * TEST_FB_STRIDE + 10 * 4, 0x00102030u);
    bar2_write(qts, 60 * TEST_FB_STRIDE + 11 * 4, 0x40102030u);

    /* The reset tables are the linear ramp: colors unchanged. */
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 60), ==, 0x102030);

    rgb_write_wat(qts, GAMMA_RED, 0x10, 0x3ff);
    rgb_write_wat(qts, GAMMA_GREEN, 0x20, 0x101);
    rgb_write_wat(qts, GAMMA_BLUE, 0x30, 0x000);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 60), ==, 0xff4000);
    g_assert_cmphex(rad4_screendump_pixel(qts, 11, 60), ==, 0xff4000);

    rgb_set_index(qts, GAMMA_GREEN + 0x20);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, 0x101 >> 2);
    g_assert_cmphex(bar2_read(qts, B_RGB_RESERVED), ==, (0x101 & 3) << 6);
    g_assert_cmphex(rgb_current_index(qts), ==, GAMMA_GREEN + 0x21);

    /* Window 0x00 bypasses gamma; window 0x40 uses AUX_FB_WAT entry 4. */
    rgb_write_byte(qts, RGB_AUX_FB_WAT, AUX_WAT_GAMMA_BYPASS);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 60), ==, 0x102030);
    g_assert_cmphex(rad4_screendump_pixel(qts, 11, 60), ==, 0xff4000);

    qtest_quit(qts);
}

/*
 * The overlay layer takes its gamma bypass from its own AUX_OL_WAT entry's
 * GB bit, not from the frame-buffer layer's AUX_FB_WAT entry (page 46).
 */
static void test_gamma_overlay_bypass(void)
{
    QTestState *qts = rad4_init();

    rad4_setup_overlay(qts, OL_WAT_4BIT_INDEX(2, 0));
    bar2_write(qts, 70 * TEST_FB_STRIDE + 10 * 4, 0x05abcdefu);
    rgb_write_wat(qts, GAMMA_RED, 0x11, 0x3ff);
    rgb_write_wat(qts, GAMMA_RED, 0xab, 0x000);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 70), ==, 0xff2233);

    /* Bypassing the frame-buffer layer leaves the overlay pixel alone. */
    rgb_write_byte(qts, RGB_AUX_FB_WAT, AUX_WAT_GAMMA_BYPASS);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 70), ==, 0xff2233);

    rgb_write_byte(qts, RGB_AUX_OL_WAT,
                   AUX_OL_WAT_CK0_ENABLED | AUX_WAT_GAMMA_BYPASS);
    g_assert_cmphex(rad4_screendump_pixel(qts, 10, 70), ==, 0x112233);

    qtest_quit(qts);
}

/*
 * The driver's 13 display modes. Each sequence is the shared grammar --
 * board control with bit 0x20 clear, the PLL index, the mode's two PLL
 * bytes, its vertical and three horizontal timing banks, the sync register
 * and board control with bit 0x20 set -- around timing banks expanded from
 * the generated per-mode fixture.
 */
typedef struct Rad4Event {
    uint32_t offset;
    uint32_t value;
} Rad4Event;

#define B_HORIZ1        0xa01000
#define B_HORIZ3        0xa03000
#define EV_PLL0         3
#define EV_PLL1         4
#define EV_BANKS        5

static const uint32_t mode_bank_base[RAD4_MODE_BANKS] = {
    B_VERT, B_HORIZ0, B_HORIZ1, B_HORIZ3,
};

static void events_append(GArray *events, uint32_t offset, uint32_t value)
{
    Rad4Event event = { offset, value };

    g_array_append_val(events, event);
}

/* Bank @bank of @m, expanded, appended to @events. */
static void events_append_bank(GArray *events, const Rad4ModeFixture *m,
                               int bank)
{
    const Rad4TimingBank *b = &m->banks[bank];
    uint32_t address = mode_bank_base[bank];

    for (int r = 0; r < b->run_count; r++) {
        for (int i = 0; i < b->runs[r].count; i++) {
            events_append(events, address, b->runs[r].value);
            address += 4;
        }
    }
    g_assert_cmpuint((address - mode_bank_base[bank]) / 4, ==, b->words);
}

/*
 * One mode-programming sequence. The PLL bytes come from @pll and the
 * vertical bank and horizontal banks from @vertical and @horizontal, so a
 * test can splice two modes; pass the same mode three times for a real one.
 */
static GArray *mode_events_spliced(const Rad4ModeFixture *pll,
                                   const Rad4ModeFixture *vertical,
                                   const Rad4ModeFixture *horizontal)
{
    GArray *events = g_array_new(false, false, sizeof(Rad4Event));

    events_append(events, B_CTRL, 0);
    events_append(events, B_RGB_LO, 0x21);
    events_append(events, B_RGB_HI, 0);
    events_append(events, B_RGB_DATA, pll->pll[0]);
    events_append(events, B_RGB_DATA, pll->pll[1]);
    events_append_bank(events, vertical, 0);
    for (int bank = 1; bank < RAD4_MODE_BANKS; bank++) {
        events_append_bank(events, horizontal, bank);
    }
    events_append(events, B_RGB_LO, 0x20);
    events_append(events, B_RGB_HI, 0);
    events_append(events, B_RGB_DATA, 1);
    events_append(events, B_CTRL, CTRL_MODE_CMD);
    return events;
}

static GArray *mode_events(int mode)
{
    GArray *events = mode_events_spliced(&rad4_modes[mode], &rad4_modes[mode],
                                         &rad4_modes[mode]);

    g_assert_cmpuint(events->len, ==, rad4_modes[mode].event_count);
    return events;
}

static Rad4Event *event_at(GArray *events, unsigned i)
{
    return &g_array_index(events, Rad4Event, i);
}

/*
 * Write @events in order. The Bridge window only moves when an event leaves
 * the current 1 MiB region, which keeps a 3,000-event replay quick.
 */
static void play_events(QTestState *qts, GArray *events)
{
    uint32_t window = UINT32_MAX;

    for (unsigned i = 0; i < events->len; i++) {
        uint32_t pci = RAD4_BAR2_PCI + event_at(events, i)->offset;

        if ((pci & ~(WINDOW_SIZE - 1)) != window) {
            window = pci & ~(WINDOW_SIZE - 1);
            rad4_window_to(qts, pci);
        }
        qtest_writel(qts, WINDOW_RAD4 + (pci & (WINDOW_SIZE - 1)),
                     event_at(events, i)->value);
    }
}

static void play_mode(QTestState *qts, int mode)
{
    g_autoptr(GArray) events = mode_events(mode);

    play_events(qts, events);
}

/* A QMP screendump's geometry and pixels, as 0xRRGGBB words. */
typedef struct Screen {
    unsigned width;
    unsigned height;
    uint32_t *pixels;
} Screen;

static Screen *screen_capture(QTestState *qts)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    Screen *screen = g_new0(Screen, 1);
    unsigned header = 0;
    const uint8_t *rgb;
    gsize length;
    int fd;

    fd = g_file_open_tmp("psitech-rad4-mode-XXXXXX.ppm", &path, &error);
    g_assert_no_error(error);
    close(fd);
    qtest_qmp_assert_success(qts,
        "{'execute': 'screendump', 'arguments': {'filename': %s}}", path);
    g_assert_true(g_file_get_contents(path, &contents, &length, &error));
    unlink(path);
    g_assert_cmpint(sscanf(contents, "P6\n%u %u\n255\n%n", &screen->width,
                           &screen->height, &header), ==, 2);
    g_assert_cmpuint(header, >, 0);
    g_assert_cmpuint(header + screen->width * screen->height * 3, <=, length);
    rgb = (const uint8_t *)contents + header;
    screen->pixels = g_new(uint32_t, screen->width * screen->height);
    for (unsigned i = 0; i < screen->width * screen->height; i++) {
        screen->pixels[i] = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
        rgb += 3;
    }
    return screen;
}

static void screen_free(Screen *screen)
{
    g_free(screen->pixels);
    g_free(screen);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Screen, screen_free)

static uint32_t screen_pixel(Screen *screen, unsigned x, unsigned y)
{
    g_assert_cmpuint(x, <, screen->width);
    g_assert_cmpuint(y, <, screen->height);
    return screen->pixels[y * screen->width + x];
}

static bool frame_pending(QTestState *qts)
{
    return bar2_read(qts, B_CTRL) & STATUS_FRAME;
}

/* The driver's interrupt handler: control with bit 1 clear, 0x20 kept. */
static void frame_acknowledge(QTestState *qts)
{
    bar2_write(qts, B_CTRL, CTRL_MODE_CMD);
    g_assert_false(frame_pending(qts));
}

/*
 * Right after recognition, frames arrive exactly every 1/@hz s of virtual
 * time: none a microsecond before each one, one a microsecond after. The
 * labels are at least 0.25 ms apart in period, so this tells every pair of
 * modes with different labels apart.
 */
static void check_frame_cadence(QTestState *qts, unsigned hz)
{
    int64_t period = 1000000000 / hz;

    g_assert_false(frame_pending(qts));
    qtest_clock_step(qts, period - 1000);
    g_assert_false(frame_pending(qts));
    qtest_clock_step(qts, 2000);
    g_assert_true(frame_pending(qts));
    frame_acknowledge(qts);
    qtest_clock_step(qts, period - 2000);
    g_assert_false(frame_pending(qts));
    qtest_clock_step(qts, 2000);
    g_assert_true(frame_pending(qts));
    frame_acknowledge(qts);
}

/*
 * The scanout has the mode's geometry and rows 4 * width bytes apart: a
 * pixel stored at the last column of the last row shows there, and one at
 * the start of row 1 shows at (0, 1). FB_WAT entry 0 is set to 24-bit true
 * color so the stored words display as themselves.
 */
static void check_mode_scanout(QTestState *qts, int mode)
{
    const Rad4ModeFixture *m = &rad4_modes[mode];
    uint32_t stride = m->width * 4;
    g_autoptr(Screen) screen = NULL;

    rgb_write_wat(qts, WAT_BASE, 0, FB_WAT_TRUE_24);
    bar2_write(qts, stride * (m->height - 1) + 4 * (m->width - 1),
               0x00a1b2c3);
    bar2_write(qts, stride, 0x00445566);
    screen = screen_capture(qts);
    g_assert_cmpuint(screen->width, ==, m->width);
    g_assert_cmpuint(screen->height, ==, m->height);
    g_assert_cmphex(screen_pixel(screen, m->width - 1, m->height - 1), ==,
                    0xa1b2c3);
    g_assert_cmphex(screen_pixel(screen, 0, 1), ==, 0x445566);
    g_assert_cmphex(screen_pixel(screen, m->width - 2, m->height - 1), ==, 0);
    g_assert_cmphex(screen_pixel(screen, m->width - 1, 0), ==, 0);
    bar2_write(qts, stride * (m->height - 1) + 4 * (m->width - 1), 0);
    bar2_write(qts, stride, 0);
}

static void check_mode_active(QTestState *qts, int mode)
{
    check_frame_cadence(qts, rad4_modes[mode].refresh_hz);
    check_mode_scanout(qts, mode);
}

/* No frame arrives for a whole second: no mode is active. */
static void check_no_mode(QTestState *qts)
{
    qtest_clock_step(qts, 1000000000);
    g_assert_false(frame_pending(qts));
}

/*
 * Each of the 13 sequences, replayed alone, is recognized as its own mode:
 * the scanout takes the mode's size and stride and the frame interrupt
 * its refresh label.
 */
static void test_mode_recognized(const void *data)
{
    int mode = GPOINTER_TO_INT(data);
    QTestState *qts = rad4_init();

    if (mode == 0) {
        /* The generated fixture's mode 0 is the reviewed mode-zero one. */
        g_autoptr(GArray) events = mode_events(0);

        g_assert_cmpuint(events->len, ==, RAD4_MODE0_EVENT_COUNT);
        for (unsigned i = 0; i < events->len; i++) {
            g_assert_cmphex(event_at(events, i)->offset, ==,
                            rad4_mode0_events[i].bar2_offset);
            g_assert_cmphex(event_at(events, i)->value, ==,
                            rad4_mode0_events[i].value);
        }
    }
    play_mode(qts, mode);
    check_mode_active(qts, mode);
    qtest_quit(qts);
}

typedef enum MutationKind {
    MUTATE_XOR,
    MUTATE_DROP,
    MUTATE_DUPLICATE,
} MutationKind;

typedef struct Mutation {
    const char *what;
    MutationKind kind;
    /* The bank holding the event, or -1 for one outside the banks. */
    int bank;
    /* Index within that bank or the sequence; negative counts from the end. */
    int index;
} Mutation;

static const Mutation mode_mutations[] = {
    { "first PLL byte", MUTATE_XOR, -1, EV_PLL0 },
    { "second PLL byte", MUTATE_XOR, -1, EV_PLL1 },
    { "first vertical word", MUTATE_XOR, 0, 0 },
    { "last vertical word", MUTATE_XOR, 0, -1 },
    { "horizontal bank 0 word", MUTATE_XOR, 1, 7 },
    { "horizontal bank 1 word", MUTATE_XOR, 2, -2 },
    { "horizontal bank 3 word", MUTATE_XOR, 3, 20 },
    { "sync value", MUTATE_XOR, -1, -2 },
    { "vertical bank one word short", MUTATE_DROP, 0, -1 },
    { "horizontal bank 3 one word long", MUTATE_DUPLICATE, 3, -1 },
};

static unsigned mutation_position(int mode, const Mutation *mutation)
{
    const Rad4ModeFixture *m = &rad4_modes[mode];
    unsigned start = EV_BANKS;

    if (mutation->bank < 0) {
        return mutation->index < 0 ? m->event_count + mutation->index
                                   : mutation->index;
    }
    for (int b = 0; b < mutation->bank; b++) {
        start += m->banks[b].words;
    }
    return start + (mutation->index < 0 ?
                    m->banks[mutation->bank].words + mutation->index :
                    mutation->index);
}

/*
 * Every distinguishing field of a sequence is checked: changing one PLL
 * byte, one word of each bank or the sync value, or making a bank one word
 * shorter or longer, leaves no mode recognized. The unmodified sequence
 * then still recognizes the mode, so the failures are not a broken replay.
 */
static void test_mode_rejects_mutation(const void *data)
{
    int mode = GPOINTER_TO_INT(data);
    QTestState *qts = rad4_init();

    play_mode(qts, mode);
    check_mode_active(qts, mode);
    for (unsigned i = 0; i < ARRAY_SIZE(mode_mutations); i++) {
        const Mutation *mutation = &mode_mutations[i];
        g_autoptr(GArray) events = mode_events(mode);
        unsigned pos = mutation_position(mode, mutation);
        Rad4Event copy = *event_at(events, pos);

        if (g_test_verbose()) {
            g_test_message("mode %d: %s", mode, mutation->what);
        }
        switch (mutation->kind) {
        case MUTATE_XOR:
            event_at(events, pos)->value ^= 0x01;
            break;
        case MUTATE_DROP:
            g_array_remove_index(events, pos);
            break;
        case MUTATE_DUPLICATE:
            copy.offset += 4;
            g_array_insert_val(events, pos + 1, copy);
            break;
        }
        play_events(qts, events);
        check_no_mode(qts);
    }
    play_mode(qts, mode);
    check_mode_active(qts, mode);
    qtest_quit(qts);
}

/*
 * Modes 4 and 11 program the same PLL bytes, as do modes 5 and 6, so only
 * their timing banks tell them apart: each is recognized with its own
 * refresh label and geometry, and one mode's vertical bank followed by the
 * other's horizontal banks is no mode at all.
 */
static void test_mode_shared_pll(void)
{
    static const int pairs[][2] = { { 4, 11 }, { 5, 6 } };
    QTestState *qts = rad4_init();

    for (unsigned p = 0; p < ARRAY_SIZE(pairs); p++) {
        const Rad4ModeFixture *a = &rad4_modes[pairs[p][0]];
        const Rad4ModeFixture *b = &rad4_modes[pairs[p][1]];
        g_autoptr(GArray) ab = mode_events_spliced(a, a, b);
        g_autoptr(GArray) ba = mode_events_spliced(b, b, a);

        g_assert_cmpmem(a->pll, 2, b->pll, 2);
        play_mode(qts, pairs[p][0]);
        check_mode_active(qts, pairs[p][0]);
        play_mode(qts, pairs[p][1]);
        check_mode_active(qts, pairs[p][1]);
        play_events(qts, ab);
        check_no_mode(qts);
        play_events(qts, ba);
        check_no_mode(qts);
    }
    qtest_quit(qts);
}

/*
 * Modes 2 and 3 write identical timing banks and differ only in their PLL
 * bytes: the same banks are 1600x1200 at 60 Hz after mode 2's PLL bytes
 * and at 72 Hz after mode 3's, and mixing one byte from each is neither.
 */
static void test_mode_shared_banks(void)
{
    const Rad4ModeFixture *two = &rad4_modes[2];
    const Rad4ModeFixture *three = &rad4_modes[3];
    QTestState *qts = rad4_init();
    g_autoptr(GArray) mixed = mode_events(2);

    for (int b = 0; b < RAD4_MODE_BANKS; b++) {
        g_assert_cmpuint(two->banks[b].words, ==, three->banks[b].words);
    }
    play_mode(qts, 2);
    check_mode_active(qts, 2);
    play_mode(qts, 3);
    check_mode_active(qts, 3);
    event_at(mixed, EV_PLL0)->value = three->pll[0];
    play_events(qts, mixed);
    check_no_mode(qts);
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
     * The card's cold BAR0 mailbox and framebuffer must read the reset state
     * once its BARs are reprogrammed.
     */
    g_assert_cmphex(bar0_read(qts, R_MAILBOX), ==, 0);
    g_assert_cmphex(bar2_read(qts, 0x000000), ==, 0);
    g_assert_cmphex(bar2_read(qts, B_MASK), ==, 0xffffffffu);

    qtest_clock_step(qts, 1000000000);
    g_assert_cmphex(bar2_read(qts, B_CTRL) & STATUS_FRAME, ==, 0);

    qtest_quit(qts);
}

/*
 * Reset zeroes the framebuffer but, before the fix in rad4_reset_hold(), left
 * the dirty range exactly as reset found it. A display that was already
 * recognized and idle at reset time (the ordinary case, since recognition
 * survives no reset path) would then see rad4_gfx_update() take the
 * empty-range idle fast path forever, keeping the pre-reset frame on screen
 * instead of showing the cleared card the guest actually has.
 *
 * Note this test drives the only qtest-reachable route back to a recognized,
 * refreshable state: replaying the mode-0 grammar. That replay's own
 * completion unconditionally calls rad4_mark_all_dirty() (so does vmstate
 * load on migration), independently of what reset left behind, so this test
 * cannot by itself isolate reset's own dirty-marking line from its absence --
 * it pins the guest-visible behavior ("the display always repaints in full
 * after a reset"), not that specific line. The FB_WAT/color-LUT test above
 * has no such masking and is the one that actually falls over without its
 * fix.
 */
static void test_gfx_update_reset_forces_full_repaint(void)
{
    g_autofree char *trace_path = NULL;
    g_autofree char *trace_contents = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("psitech-rad4-reset-repaint-XXXXXX.trace",
                         &trace_path, &error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf(MACHINE_WITH_BRIDGE
                      " -trace enable=rad4_gfx_update_push"
                      " -trace enable=rad4_gfx_update_idle"
                      " -D %s", trace_path);
    rad4_configure_bars(qts);

    rad4_play_mode0(qts);
    rad4_force_refresh(qts); /* First paint: consumes the full-frame range. */

    qtest_system_reset(qts);
    rad4_configure_bars(qts);
    rad4_play_mode0(qts);
    rad4_force_refresh(qts);

    qtest_quit(qts);

    g_assert_true(g_file_get_contents(trace_path, &trace_contents, NULL,
                                      &error));
    unlink(trace_path);

    {
        g_auto(GStrv) lines = g_strsplit(trace_contents, "\n", -1);
        int push_count = 0, idle_count = 0, last_push_at = -1;

        for (int i = 0; lines[i]; i++) {
            if (strstr(lines[i], "rad4_gfx_update_push")) {
                push_count++;
                last_push_at = i;
            } else if (strstr(lines[i], "rad4_gfx_update_idle")) {
                idle_count++;
            }
        }

        g_assert_cmpint(push_count, ==, 2);
        g_assert_cmpint(idle_count, ==, 0);
        g_assert_nonnull(strstr(lines[last_push_at], "rows 0-1023"));
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/psitech-rad4/pci-identity", test_pci_identity);
    qtest_add_func("/psitech-rad4/no-km-bar3", test_plain_rad4_has_no_km_bar3);
    qtest_add_func("/psitech-rad4/bar0-registers", test_bar0_registers);
    qtest_add_func("/psitech-rad4/serial-eeprom-valid",
                   test_serial_eeprom_valid_command);
    qtest_add_func("/psitech-rad4/serial-eeprom-bad",
                   test_serial_eeprom_bad_command);
    qtest_add_func("/psitech-rad4/serial-eeprom-write",
                   test_serial_eeprom_write);
    qtest_add_func("/psitech-rad4/serial-eeprom-backing-drive",
                   test_serial_eeprom_backing_drive);
    qtest_add_func("/psitech-rad4/framebuffer-mask", test_framebuffer_mask);
    qtest_add_func("/psitech-rad4/rgb-data-read-autoincrement",
                   test_rgb_data_read_autoincrement);
    qtest_add_func("/psitech-rad4/fb-wat-round-trip", test_fb_wat_round_trip);
    qtest_add_func("/psitech-rad4/ol-wat-round-trip", test_ol_wat_round_trip);
    qtest_add_func("/psitech-rad4/reserved-port-index-write-resets-phase",
                   test_reserved_port_index_write_resets_phase);
    qtest_add_func("/psitech-rad4/color-lut-round-trip",
                   test_color_lut_round_trip);
    qtest_add_func("/psitech-rad4/mode-recognition-mutation",
                   test_mode_recognition_rejects_mutation);
    qtest_add_func("/psitech-rad4/gfx-update-dirty-tracking",
                   test_gfx_update_dirty_tracking);
    qtest_add_func("/psitech-rad4/gfx-update-palette-change-without-fb-write",
                   test_gfx_update_palette_change_without_fb_write);
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
    qtest_add_func("/psitech-rad4/cursor-composited-into-scanout",
                   test_cursor_composited_into_scanout);
    qtest_add_func("/psitech-rad4/cursor-no-ghost-after-far-fb-update",
                   test_cursor_no_ghost_after_far_fb_update);
    qtest_add_func("/psitech-rad4/overlay-composited-into-scanout",
                   test_overlay_composited_into_scanout);
    qtest_add_func("/psitech-rad4/overlay-chroma-key-and-opaque",
                   test_overlay_chroma_key_and_opaque);
    qtest_add_func("/psitech-rad4/overlay-wat-addressing",
                   test_overlay_wat_addressing);
    qtest_add_func("/psitech-rad4/overlay-wat-addressing-shapes",
                   test_overlay_wat_addressing_shapes);
    qtest_add_func("/psitech-rad4/fb-pixel-formats", test_fb_pixel_formats);
    qtest_add_func("/psitech-rad4/gamma-tables", test_gamma_tables);
    qtest_add_func("/psitech-rad4/gamma-overlay-bypass",
                   test_gamma_overlay_bypass);
    qtest_add_func("/psitech-rad4/reset", test_reset);
    for (int mode = 0; mode < RAD4_MODE_COUNT; mode++) {
        g_autofree char *path = g_strdup_printf(
            "/psitech-rad4/mode-recognized/%d", mode);

        qtest_add_data_func(path, GINT_TO_POINTER(mode), test_mode_recognized);
    }
    /* The smallest, a portrait, the shortest frame and the largest mode. */
    qtest_add_data_func("/psitech-rad4/mode-rejects-mutation/7",
                        GINT_TO_POINTER(7), test_mode_rejects_mutation);
    qtest_add_data_func("/psitech-rad4/mode-rejects-mutation/5",
                        GINT_TO_POINTER(5), test_mode_rejects_mutation);
    qtest_add_data_func("/psitech-rad4/mode-rejects-mutation/11",
                        GINT_TO_POINTER(11), test_mode_rejects_mutation);
    qtest_add_func("/psitech-rad4/mode-shared-pll", test_mode_shared_pll);
    qtest_add_func("/psitech-rad4/mode-shared-banks", test_mode_shared_banks);
    qtest_add_func("/psitech-rad4/gfx-update-reset-forces-full-repaint",
                   test_gfx_update_reset_forces_full_repaint);

    qtest_add_func("/psitech-rad4-km/pci-identity", test_km_pci_identity);
    qtest_add_func("/psitech-rad4-km/bar0-register-round-trip",
                   test_km_bar0_register_round_trip);
    qtest_add_func("/psitech-rad4-km/bar2-framebuffer-round-trip",
                   test_km_bar2_framebuffer_round_trip);
    qtest_add_func("/psitech-rad4-km/channel-enable-reflects-in-status",
                   test_km_channel_enable_reflects_in_status);
    qtest_add_func("/psitech-rad4-km/isr-aggregates-both-channels",
                   test_km_isr_aggregates_both_channels);
    qtest_add_func("/psitech-rad4-km/interrupt-reaches-pci-status",
                   test_km_interrupt_reaches_pci_status);
    qtest_add_func("/psitech-rad4-km/register-file-write-smoke",
                   test_km_register_file_write_smoke);
    qtest_add_func("/psitech-rad4-km/kbd-ps2-reset-round-trip",
                   test_km_kbd_ps2_reset_round_trip);
    qtest_add_func("/psitech-rad4-km/mouse-ps2-reset-round-trip",
                   test_km_mouse_ps2_reset_round_trip);

    return g_test_run();
}
