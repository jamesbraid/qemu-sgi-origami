/*
 * PsiTech RAD1 (psitech-rad1): register contract for the reviewed flat-copy
 * functional approximation, through the real Origin 200 Bridge PCI path.
 *
 * The card is an add-in PCI card, so like tests/qtest/sn0-tigon-test.c and
 * tests/qtest/psitech-rad4-test.c it is instantiated with -device at a free
 * slot and reached through that slot's own Bridge decode. Expected values
 * come from docs/datasheets/psitech-rad1.md in the owning Origami
 * repository and the public TI/3Dlabs TVP4020 Programmer's Reference
 * Manual it cites, not from this device's own implementation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

/* PCI type 0 configuration space offsets, restated per file convention. */
#define PCI_VENDOR_ID_OFF   0x00
#define PCI_REVISION_ID_OFF 0x08
#define PCI_BAR0_OFF        0x10
#define PCI_BAR1_OFF        0x14
#define PCI_BAR2_OFF        0x18
#define PCI_COMMAND_OFF     0x04
#define PCI_COMMAND_MEMORY  0x0002u
#define PCI_COMMAND_MASTER  0x0004u
#define PCI_SUBSYS_ID_OFF   0x2c

/* Crosstalk space and the Bridge's widget window inside it. */
#define SPACE_XTALK     0x20000000000ULL
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

/*
 * Devices 0 and 1 are BaseIO's SCSI adapters and 2 is IOC3; 3 is the first
 * free slot, with the first of the six 1 MiB windows beyond the two 2 MiB
 * ones -- the same slot tests/qtest/sn0-tigon-test.c and
 * tests/qtest/psitech-rad4-test.c use for their own single-instance qtest.
 */
#define RAD1_SLOT       3
#define WINDOW_RAD1     (BRIDGE_BASE + 0x700000)
#define WINDOW_SIZE     0x100000u

#define BRIDGE_CFG0     0x020000
#define CFG0_STRIDE     0x1000
#define CFG0(dev)       (BRIDGE_BASE + BRIDGE_CFG0 + (dev) * CFG0_STRIDE)

#define REG_DATA            4
#define BRIDGE_DEVICE(n)    (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)
#define DEV_MEMORY_SPACE    (1u << 12)

#define MACHINE_WITH_BRIDGE \
    "-M origin200 -device psitech-rad1,addr=" G_STRINGIFY(RAD1_SLOT)

/*
 * PCI addresses this test assigns the card's apertures; 1 MiB aligned so
 * the window never needs to slide within one BAR's own accesses.
 */
#define RAD1_BAR0_PCI   0x20000000u  /* Region Zero, 128 KiB. */
#define RAD1_BAR1_PCI   0x30000000u  /* Framebuffer, 8 MiB. */
#define RAD1_BAR2_PCI   0x40000000u  /* Flash aperture, 64 KiB. */

/*
 * Primary identity is the 3Dlabs-badged Permedia 2 die's own PCI
 * Vendor/Device ID, one of the two the IRIX kernel driver's own
 * registration call accepts (docs/datasheets/psitech-rad1.md,
 * "Kernel-driver attach: the real cause is a PCI identity mismatch") and
 * one of two the IO6 PROM's RAD1 console installer also recognizes by
 * Device ID alone (hw/display/psitech-rad1.c's own comment), not
 * PsiTech's board identity.
 */
#define RAD1_VENDOR_ID  0x3d3d
#define RAD1_DEVICE_ID  0x0009

/*
 * PsiTech's board/subsystem identity, exposed as the separate PCI
 * Subsystem Vendor/Device ID rather than the primary Vendor/Device ID.
 */
#define RAD1_SUBSYSTEM_VENDOR_ID 0x12d1
#define RAD1_SUBSYSTEM_DEVICE_ID 0x0101

#define RAD1_REGZERO_SIZE   0x20000u
#define RAD1_FB_SIZE        (8 * 1024 * 1024u)
#define RAD1_FLASH_SIZE     0x10000u

/* Region Zero: Control & Status. */
#define R_CTRL_ACK      0x08
#define R_INT_STATUS    0x10
#define R_FIFO_SPACE    0x18
#define R_ATTACH_GATE   0x40

#define INT_BIT0        (1u << 0)
#define INT_BIT1        (1u << 1)
#define INT_BIT7        (1u << 7)
#define INT_KNOWN_MASK  (INT_BIT0 | INT_BIT1 | INT_BIT7)

/*
 * Region Zero: Bypass DMA (docs/datasheets/psitech-rad1.md, "Bypass DMA
 * write-completion path"; offsets and field layout cross-checked against
 * the TVP4020 Hardware Reference Manual Issue 6, pages 44-51, and against
 * the driver's own disassembled posting sequence). ByDMAControl's own
 * Mode/RestartGP/format/patch-type field values are not exercised by this
 * model -- any write triggers the transfer -- so BYDMA_CONTROL_TRIGGER
 * below is a nominal value, not a decoded one.
 */
#define R_BYDMA_ADDRESS     0x00a0
#define R_BYDMA_STRIDE      0x00b8
#define R_BYDMA_MEM_ADDR    0x00c0
#define R_BYDMA_SIZE        0x00c8
#define R_BYDMA_BYTE_MASK   0x00d0
#define R_BYDMA_CONTROL     0x00d8

#define BYDMA_CONTROL_TRIGGER   1u

/* Left mask in bits 0-3, right mask in bits 16-19; page 50, offset D0h. */
#define BYDMA_BYTE_MASK(left, right) \
    (((uint32_t)(right) << 16) | ((left) & 0xfu))
#define BYDMA_BYTE_MASK_OPEN    BYDMA_BYTE_MASK(0xf, 0xf)

/* Region Zero: RAMDAC (docs/datasheets/psitech-rad1.md). */
#define R_RAMDAC_0      0x4000
#define R_RAMDAC_1      0x4010
#define R_RAMDAC_6      0x4050
/* An offset inside the RAMDAC block the driver is not established to use. */
#define R_RAMDAC_UNSUPPORTED 0x4008

/*
 * Region Zero: FIFO-tagged GP registers (docs/datasheets/psitech-rad1.md;
 * offsets cross-checked against the TVP4020 Programmer's Reference
 * Manual's own per-register table entries).
 */
#define R_RENDER            0x8038
#define R_WAIT_FOR_COMPLETE 0x8088
#define R_RECT_ORIGIN       0x80d0
#define R_RECT_SIZE         0x80d8
#define R_S_START           0x8388
#define R_T_START           0x83a0
#define R_TEX_BASE_ADDR     0x8580
#define R_FB_READ_MODE      0x8a80
#define R_FB_HW_WRITE_MASK  0x8ac0

#define TEXBASE_SYSTEM_MEMORY   (1u << 30)
#define FBREADMODE_READBACK_BIT (1u << 10)

#define RECT(x, y)          (((uint32_t)(y) << 16) | ((x) & 0xffffu))
#define SIZE(w, h)          (((uint32_t)(h) << 16) | ((w) & 0xffffu))

/* ByDMASize: Width in bits 0-11, Height in bits 16-27 (RAD1_BYDMA_DIM_MASK). */
#define BYDMA_SIZE(w, h)    (((uint32_t)(h) << 16) | ((w) & 0x0fffu))

/*
 * Flash/EEPROM: generic JEDEC/AMD unlock-and-program sequence, addressed
 * here using the driver's own disassembled CPU-side literal offsets
 * (docs/datasheets/psitech-rad1.md), not the standard JEDEC addresses
 * hw/display/psitech-rad1.c's state machine matches against -- this
 * exercises the real Bridge byte-lane swap (addr ^ 3) that reconciles the
 * two, the same way a real driver's own literal store address would.
 */
#define FLASH_UNLOCK1_ADDR  0x5556
#define FLASH_UNLOCK1_DATA  0xaa
#define FLASH_UNLOCK2_ADDR  0x2aa9
#define FLASH_UNLOCK2_DATA  0x55
#define FLASH_PROGRAM_ADDR  0x5556
#define FLASH_PROGRAM_DATA  0xa0

/*
 * Bridge's byte-lane correction for a device-initiated DMA transfer
 * (crosses crosstalk, unlike this file's own PIO-only apertures above):
 * tests/qtest/sn0-qlogic-isp-test.c exercises the mechanism itself, and
 * tests/qtest/psitech-rad4-test.c's dma_start() applies the same setup
 * this file needs for a host-memory Render source.
 */
#define DEV_SWAP_DIRECT         (1u << 19)
#define PCI_DMA_DIRECT          0x80000000u
#define BRIDGE_DIRECT_MAP_REG   (BRIDGE_BASE + 0x080 + REG_DATA)

/* Two low, otherwise-unused guest physical pages for host-memory tests. */
#define HOST_SRC_PHYS   0x00100000u
#define HOST_DST_PHYS   0x00200000u

static void rad1_window_to(QTestState *qts, uint32_t pci_addr)
{
    uint32_t base = pci_addr & ~(WINDOW_SIZE - 1);

    qtest_writel(qts, BRIDGE_DEVICE(RAD1_SLOT), DEV_MEMORY_SPACE |
                (base >> 20));
}

static uint32_t rad1_readl(QTestState *qts, uint32_t pci_addr)
{
    rad1_window_to(qts, pci_addr);
    return qtest_readl(qts, WINDOW_RAD1 + (pci_addr & (WINDOW_SIZE - 1)));
}

static void rad1_writel(QTestState *qts, uint32_t pci_addr, uint32_t val)
{
    rad1_window_to(qts, pci_addr);
    qtest_writel(qts, WINDOW_RAD1 + (pci_addr & (WINDOW_SIZE - 1)), val);
}

static uint32_t regzero_read(QTestState *qts, uint32_t off)
{
    return rad1_readl(qts, RAD1_BAR0_PCI + off);
}

static void regzero_write(QTestState *qts, uint32_t off, uint32_t val)
{
    rad1_writel(qts, RAD1_BAR0_PCI + off, val);
}

static uint32_t vram_read(QTestState *qts, uint32_t off)
{
    return rad1_readl(qts, RAD1_BAR1_PCI + off);
}

static void vram_write(QTestState *qts, uint32_t off, uint32_t val)
{
    rad1_writel(qts, RAD1_BAR1_PCI + off, val);
}

/*
 * Byte-wide, unlike every other BAR on this device: the JEDEC/AMD unlock
 * addresses themselves (0x5556, 0x2aa9) are not 4-byte aligned
 * (hw/display/psitech-rad1.c's rad1_flash_ops comment), so this reaches
 * the flash aperture with single-byte accesses rather than
 * regzero_read/write's or vram_read/write's 32-bit ones.
 */
static uint32_t flash_read(QTestState *qts, uint32_t off)
{
    uint32_t pci_addr = RAD1_BAR2_PCI + off;

    rad1_window_to(qts, pci_addr);
    return qtest_readb(qts, WINDOW_RAD1 + (pci_addr & (WINDOW_SIZE - 1)));
}

static void flash_write(QTestState *qts, uint32_t off, uint32_t val)
{
    uint32_t pci_addr = RAD1_BAR2_PCI + off;

    rad1_window_to(qts, pci_addr);
    qtest_writeb(qts, WINDOW_RAD1 + (pci_addr & (WINDOW_SIZE - 1)), val);
}

/*
 * Programs the card's BARs into PCI space and opens the Bridge slot's
 * memory decode, the way psitech-rad4-test.c's rad4_configure_bars() sets
 * up its own BARs. A PCI reset clears these config-space registers back to
 * unprogrammed, so this must run again after any reset before the card
 * answers PIO again.
 */
static void rad1_configure_bars(QTestState *qts)
{
    qtest_writel(qts, CFG0(RAD1_SLOT) + PCI_BAR0_OFF, RAD1_BAR0_PCI);
    qtest_writel(qts, CFG0(RAD1_SLOT) + PCI_BAR1_OFF, RAD1_BAR1_PCI);
    qtest_writel(qts, CFG0(RAD1_SLOT) + PCI_BAR2_OFF, RAD1_BAR2_PCI);
    qtest_writel(qts, CFG0(RAD1_SLOT) + PCI_COMMAND_OFF, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(RAD1_SLOT),
                DEV_MEMORY_SPACE | (RAD1_BAR0_PCI >> 20));
}

static QTestState *rad1_init(void)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    rad1_configure_bars(qts);
    return qts;
}

/*
 * Enables bus mastering and this slot's Direct-window byte-lane
 * correction, then writes `val` to Region Zero offset `off` to trigger a
 * host-memory-capable transfer -- matching psitech-rad4-test.c's
 * dma_start() exactly, including why it does the triggering write itself
 * rather than through the generic regzero_write()/rad1_writel() helpers:
 * those re-window the Bridge slot's own device register (rad1_window_to()),
 * which shares its storage with this correction bit, so anything windowing
 * after this call would silently drop it again. This also windows to
 * Region Zero's own base (not the host address's), since the triggering
 * write itself is a Region Zero PIO access; the correction bit is this
 * Bridge slot's own property, independent of which window base happens to
 * be selected alongside it, matching how RAD4's own DMA-triggering
 * register lives in the same window its DMA needs the correction for.
 * Shared by every register whose write can start a host-memory DMA
 * (Render, and ByDMAControl below) since the correction is Bridge-slot
 * state, not tied to one particular triggering register.
 */
static void rad1_trigger_dma(QTestState *qts, uint32_t off, uint32_t val)
{
    uint32_t base = RAD1_BAR0_PCI & ~(WINDOW_SIZE - 1);

    qtest_writel(qts, CFG0(RAD1_SLOT) + PCI_COMMAND_OFF,
                PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);
    qtest_writel(qts, BRIDGE_DEVICE(RAD1_SLOT),
                DEV_MEMORY_SPACE | DEV_SWAP_DIRECT | (base >> 20));
    qtest_writel(qts, WINDOW_RAD1 + ((RAD1_BAR0_PCI + off) &
                                     (WINDOW_SIZE - 1)), val);
}

static void write_host_words(QTestState *qts, uint32_t phys,
                             const uint32_t *words, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        uint8_t buf[4];

        stl_be_p(buf, words[i]);
        qtest_memwrite(qts, phys + i * 4, buf, sizeof(buf));
    }
}

static uint32_t read_host_word(QTestState *qts, uint32_t phys)
{
    uint8_t buf[4];

    qtest_memread(qts, phys, buf, sizeof(buf));
    return ldl_be_p(buf);
}

static void test_pci_identity(void)
{
    QTestState *qts = rad1_init();
    uint32_t id = qtest_readl(qts, CFG0(RAD1_SLOT) + PCI_VENDOR_ID_OFF);

    g_assert_cmphex(id & 0xffff, ==, RAD1_VENDOR_ID);
    g_assert_cmphex(id >> 16, ==, RAD1_DEVICE_ID);

    uint32_t subsys = qtest_readl(qts, CFG0(RAD1_SLOT) + PCI_SUBSYS_ID_OFF);
    g_assert_cmphex(subsys & 0xffff, ==, RAD1_SUBSYSTEM_VENDOR_ID);
    g_assert_cmphex(subsys >> 16, ==, RAD1_SUBSYSTEM_DEVICE_ID);

    uint32_t rev_class = qtest_readl(qts,
                                     CFG0(RAD1_SLOT) + PCI_REVISION_ID_OFF);
    g_assert_cmphex((rev_class >> 16) & 0xff, ==, 0x80); /* subclass */
    g_assert_cmphex((rev_class >> 24) & 0xff, ==, 0x03); /* base class */

    qtest_writel(qts, CFG0(RAD1_SLOT) + PCI_BAR0_OFF, 0xffffffffu);
    uint32_t bar0_mask = qtest_readl(qts, CFG0(RAD1_SLOT) + PCI_BAR0_OFF);
    g_assert_cmphex(bar0_mask & 0xfffffff0u, ==,
                    (uint32_t)-RAD1_REGZERO_SIZE & 0xfffffff0u);
    qtest_writel(qts, CFG0(RAD1_SLOT) + PCI_BAR0_OFF, RAD1_BAR0_PCI);

    qtest_writel(qts, CFG0(RAD1_SLOT) + PCI_BAR1_OFF, 0xffffffffu);
    uint32_t bar1_mask = qtest_readl(qts, CFG0(RAD1_SLOT) + PCI_BAR1_OFF);
    g_assert_cmphex(bar1_mask & 0xfffffff0u, ==,
                    (uint32_t)-RAD1_FB_SIZE & 0xfffffff0u);
    qtest_writel(qts, CFG0(RAD1_SLOT) + PCI_BAR1_OFF, RAD1_BAR1_PCI);

    qtest_writel(qts, CFG0(RAD1_SLOT) + PCI_BAR2_OFF, 0xffffffffu);
    uint32_t bar2_mask = qtest_readl(qts, CFG0(RAD1_SLOT) + PCI_BAR2_OFF);
    g_assert_cmphex(bar2_mask & 0xfffffff0u, ==,
                    (uint32_t)-RAD1_FLASH_SIZE & 0xfffffff0u);
    qtest_writel(qts, CFG0(RAD1_SLOT) + PCI_BAR2_OFF, RAD1_BAR2_PCI);

    qtest_quit(qts);
}

static void test_control_status_registers(void)
{
    QTestState *qts = rad1_init();

    g_assert_cmphex(regzero_read(qts, R_CTRL_ACK), ==, 0);
    regzero_write(qts, R_CTRL_ACK, 0xdeadbeefu);
    g_assert_cmphex(regzero_read(qts, R_CTRL_ACK), ==, 0xdeadbeefu);

    g_assert_cmphex(regzero_read(qts, R_INT_STATUS), ==, 0);

    /* FIFO space always reports readiness; this model has no real FIFO. */
    g_assert_cmpuint(regzero_read(qts, R_FIFO_SPACE), >, 0x64);

    /*
     * Live-gdb-traced gate `rad1pciattach` checks before it will finish
     * attach (docs/datasheets/psitech-rad1.md, "Kernel-driver attach"):
     * any nonzero word unblocks it, so this only pins "nonzero", not a
     * specific bit pattern -- see hw/display/psitech-rad1.c's own
     * disclosure at RAD1_REG_ATTACH_GATE.
     */
    g_assert_cmpuint(regzero_read(qts, R_ATTACH_GATE), !=, 0);

    qtest_quit(qts);
}

/*
 * An offset this device does not implement stays observable (reads zero,
 * does not crash or silently alias another register) rather than being
 * guessed at -- docs/datasheets/psitech-rad1.md's own requirement that
 * "Unsupported hardware accesses must produce a deterministic error or
 * remain trace-visible".
 */
static void test_unsupported_offset_is_safe(void)
{
    QTestState *qts = rad1_init();

    g_assert_cmphex(regzero_read(qts, 0x1000), ==, 0);   /* Memory Control */
    g_assert_cmphex(regzero_read(qts, 0x3000), ==, 0);   /* Video Control */
    g_assert_cmphex(regzero_read(qts, R_RAMDAC_UNSUPPORTED), ==, 0);

    qtest_quit(qts);
}

static void test_ramdac_round_trip(void)
{
    QTestState *qts = rad1_init();

    regzero_write(qts, R_RAMDAC_0, 0x11223344u);
    regzero_write(qts, R_RAMDAC_1, 0x55667788u);
    regzero_write(qts, R_RAMDAC_6, 0x99aabbccu);

    g_assert_cmphex(regzero_read(qts, R_RAMDAC_0), ==, 0x11223344u);
    g_assert_cmphex(regzero_read(qts, R_RAMDAC_1), ==, 0x55667788u);
    g_assert_cmphex(regzero_read(qts, R_RAMDAC_6), ==, 0x99aabbccu);

    qtest_quit(qts);
}

/*
 * A same-surface (local buffer) rectangle copy: TextureBaseAddress names a
 * framebuffer offset directly (bit 30 clear, manual page 260), and
 * RectangleOrigin/RectangleSize name the destination -- this exercises the
 * plain forward path every rectangle-move helper in the driver ultimately
 * drives (docs/datasheets/psitech-rad1.md, "Rectangle transfer...").
 */
static void test_render_local_copy(void)
{
    QTestState *qts = rad1_init();
    static const uint32_t src[2] = { 0x00112233u, 0x00445566u };

    vram_write(qts, 0x1000, src[0]);
    vram_write(qts, 0x1004, src[1]);

    regzero_write(qts, R_FB_HW_WRITE_MASK, 0xffffffffu);
    regzero_write(qts, R_TEX_BASE_ADDR, 0x1000);
    regzero_write(qts, R_S_START, 0);
    regzero_write(qts, R_T_START, 0);
    regzero_write(qts, R_RECT_ORIGIN, RECT(4, 8));
    regzero_write(qts, R_RECT_SIZE, SIZE(2, 1));
    regzero_write(qts, R_RENDER, 1);

    uint32_t dst_off = (8u * 1280u + 4u) * 4u;
    g_assert_cmphex(vram_read(qts, dst_off), ==, src[0]);
    g_assert_cmphex(vram_read(qts, dst_off + 4), ==, src[1]);

    qtest_quit(qts);
}

/*
 * A host-memory (system memory) rectangle copy: TextureBaseAddress's bit
 * 30 set means this session's copy of the manual's own text -- "the
 * texture resides in system memory ... and should be fetched across the
 * PCI bus" -- so this exercises the same host-DMA-mapped source
 * rect_dma_move()/bigx_update() use (docs/datasheets/psitech-rad1.md,
 * "What 'DMA' means for this driver").
 */
static void test_render_host_copy(void)
{
    QTestState *qts = rad1_init();
    static const uint32_t payload[2] = { 0x00abcdefu, 0x00fedcbau };

    write_host_words(qts, HOST_SRC_PHYS, payload, ARRAY_SIZE(payload));

    regzero_write(qts, R_FB_HW_WRITE_MASK, 0xffffffffu);
    regzero_write(qts, R_TEX_BASE_ADDR,
                 (PCI_DMA_DIRECT | HOST_SRC_PHYS) | TEXBASE_SYSTEM_MEMORY);
    regzero_write(qts, R_S_START, 0);
    regzero_write(qts, R_T_START, 0);
    regzero_write(qts, R_RECT_ORIGIN, RECT(0, 0));
    regzero_write(qts, R_RECT_SIZE, SIZE(2, 1));
    rad1_trigger_dma(qts, R_RENDER, 1);

    g_assert_cmphex(vram_read(qts, 0), ==, payload[0]);
    g_assert_cmphex(vram_read(qts, 4), ==, payload[1]);

    qtest_quit(qts);
}

/*
 * FBHardwareWriteMask (manual page 209): "If a bit is set to one then the
 * corresponding bit in the framebuffer is enabled for writing, otherwise
 * it is disabled" -- a full 32-bit AND mask, not RAD4's software byte-
 * level plane mask.
 */
static void test_render_honors_write_mask(void)
{
    QTestState *qts = rad1_init();

    vram_write(qts, 0, 0x00aabbccu);        /* pre-existing destination. */
    vram_write(qts, 0x3000, 0x00112233u);   /* local copy source. */

    regzero_write(qts, R_FB_HW_WRITE_MASK, 0x0000ffffu);
    regzero_write(qts, R_TEX_BASE_ADDR, 0x3000);
    regzero_write(qts, R_S_START, 0);
    regzero_write(qts, R_T_START, 0);
    regzero_write(qts, R_RECT_ORIGIN, RECT(0, 0));
    regzero_write(qts, R_RECT_SIZE, SIZE(1, 1));
    regzero_write(qts, R_RENDER, 1);

    /* Only the masked low half of the incoming word took effect. */
    g_assert_cmphex(vram_read(qts, 0), ==, 0x00aa2233u);

    qtest_quit(qts);
}

/*
 * The read-back direction (FBReadMode's Read Destination Enable, manual
 * page 211) copies the framebuffer rectangle out to a host address named
 * by TextureBaseAddress -- the read direction of rad1_dma_patch_move()
 * (docs/datasheets/psitech-rad1.md). This model's specific trigger
 * (Read Destination Enable selecting the read-back path at all) is an
 * approximation disclosed in hw/display/psitech-rad1.c and the datasheet.
 */
static void test_render_readback(void)
{
    QTestState *qts = rad1_init();

    vram_write(qts, 0, 0x00c0ffeeu);

    regzero_write(qts, R_FB_READ_MODE, FBREADMODE_READBACK_BIT);
    regzero_write(qts, R_TEX_BASE_ADDR, PCI_DMA_DIRECT | HOST_DST_PHYS);
    regzero_write(qts, R_RECT_ORIGIN, RECT(0, 0));
    regzero_write(qts, R_RECT_SIZE, SIZE(1, 1));
    rad1_trigger_dma(qts, R_RENDER, 1);

    g_assert_cmphex(read_host_word(qts, HOST_DST_PHYS), ==, 0x00c0ffeeu);

    qtest_quit(qts);
}

/*
 * Round-trip register storage for the six Bypass DMA registers
 * (docs/datasheets/psitech-rad1.md, "Bypass DMA write-completion path"),
 * writing a zero-sized ByDMASize so ByDMAControl's own write -- which
 * always starts a transfer, see hw/display/psitech-rad1.c's
 * rad1_regzero_write() -- returns immediately without touching the
 * framebuffer or the interrupt status this test does not want to disturb.
 */
static void test_bydma_registers_round_trip(void)
{
    QTestState *qts = rad1_init();

    regzero_write(qts, R_BYDMA_ADDRESS, 0x00123400u);
    regzero_write(qts, R_BYDMA_STRIDE, 0x00000ff0u);
    regzero_write(qts, R_BYDMA_MEM_ADDR, 0x00abcdefu);
    regzero_write(qts, R_BYDMA_BYTE_MASK, BYDMA_BYTE_MASK_OPEN);
    /* ByDMASize left at its post-reset zero: width and height both 0. */
    regzero_write(qts, R_BYDMA_CONTROL, BYDMA_CONTROL_TRIGGER);

    g_assert_cmphex(regzero_read(qts, R_BYDMA_ADDRESS), ==, 0x00123400u);
    g_assert_cmphex(regzero_read(qts, R_BYDMA_STRIDE), ==, 0x00000ff0u);
    g_assert_cmphex(regzero_read(qts, R_BYDMA_MEM_ADDR), ==, 0x00abcdefu);
    g_assert_cmphex(regzero_read(qts, R_BYDMA_SIZE), ==, 0);
    g_assert_cmphex(regzero_read(qts, R_BYDMA_BYTE_MASK), ==,
                    BYDMA_BYTE_MASK_OPEN);
    g_assert_cmphex(regzero_read(qts, R_BYDMA_CONTROL), ==,
                    BYDMA_CONTROL_TRIGGER);

    /* Zero-sized transfer ran and returned early: no completion raised. */
    g_assert_cmphex(regzero_read(qts, R_INT_STATUS), ==, 0);

    qtest_quit(qts);
}

/*
 * The write-DMA completion path itself: a host-memory source posted
 * through ByDMAAddress/ByDMAStride/ByDMAMemAddr/ByDMASize/ByDMAByteMask,
 * started by the ByDMAControl write -- the transfer `rad1pci`'s posted
 * write times out waiting for completion (docs/datasheets/psitech-rad1.md,
 * "Bypass DMA write-completion path"). Confirms both the framebuffer
 * content and that completion raises only IntFlags bit 7, distinct from
 * Render's own RAD1_INT_KNOWN_MASK (see test_interrupt_status_write_clear
 * below).
 */
static void test_bydma_write_transfer(void)
{
    QTestState *qts = rad1_init();
    static const uint32_t payload[2] = { 0x0011aabbu, 0x0022ccddu };

    write_host_words(qts, HOST_SRC_PHYS, payload, ARRAY_SIZE(payload));

    regzero_write(qts, R_BYDMA_ADDRESS, HOST_SRC_PHYS);
    regzero_write(qts, R_BYDMA_STRIDE, ARRAY_SIZE(payload) * 4);
    regzero_write(qts, R_BYDMA_MEM_ADDR, 0);
    regzero_write(qts, R_BYDMA_SIZE, BYDMA_SIZE(ARRAY_SIZE(payload), 1));
    regzero_write(qts, R_BYDMA_BYTE_MASK, BYDMA_BYTE_MASK_OPEN);
    rad1_trigger_dma(qts, R_BYDMA_CONTROL, BYDMA_CONTROL_TRIGGER);

    g_assert_cmphex(vram_read(qts, 0), ==, payload[0]);
    g_assert_cmphex(vram_read(qts, 4), ==, payload[1]);

    uint32_t status = regzero_read(qts, R_INT_STATUS);
    g_assert_cmphex(status, ==, INT_BIT7);

    regzero_write(qts, R_INT_STATUS, status);
    g_assert_cmphex(regzero_read(qts, R_INT_STATUS), ==, 0);

    qtest_quit(qts);
}

/*
 * ByDMAByteMask (page 50, "3.2.23 Bypass DMA Byte Mask", offset D0h)
 * applies only to the first and last column of a transfer row; a middle
 * column of a wider row is always fully written regardless of the mask.
 * The specific lane-to-byte mapping is APPROXIMATED beyond the driver's
 * only-traced fully-open value (hw/display/psitech-rad1.c's
 * rad1_bydma_edge_mask() comment) -- this test pins this model's own
 * chosen mapping, not an independently confirmed hardware fact.
 */
static void test_bydma_byte_mask_edges(void)
{
    QTestState *qts = rad1_init();
    static const uint32_t payload[3] = {
        0xaabbccddu, 0x88888888u, 0x99887766u,
    };

    vram_write(qts, 0, 0x11223344u);
    vram_write(qts, 4, 0x99999999u);
    vram_write(qts, 8, 0x55667788u);

    write_host_words(qts, HOST_SRC_PHYS, payload, ARRAY_SIZE(payload));

    regzero_write(qts, R_BYDMA_ADDRESS, HOST_SRC_PHYS);
    regzero_write(qts, R_BYDMA_STRIDE, ARRAY_SIZE(payload) * 4);
    regzero_write(qts, R_BYDMA_MEM_ADDR, 0);
    regzero_write(qts, R_BYDMA_SIZE, BYDMA_SIZE(ARRAY_SIZE(payload), 1));
    /* Left column: lanes 0-1 only. Right column: lanes 2-3 only. */
    regzero_write(qts, R_BYDMA_BYTE_MASK, BYDMA_BYTE_MASK(0x3, 0xc));
    rad1_trigger_dma(qts, R_BYDMA_CONTROL, BYDMA_CONTROL_TRIGGER);

    g_assert_cmphex(vram_read(qts, 0), ==, 0x1122ccddu);
    g_assert_cmphex(vram_read(qts, 4), ==, 0x88888888u);
    g_assert_cmphex(vram_read(qts, 8), ==, 0x99887788u);

    qtest_quit(qts);
}

/*
 * Every completed Render sets all three known interrupt-status bits
 * together (docs/datasheets/psitech-rad1.md's bit-to-source mapping is
 * unresolved; hw/display/psitech-rad1.c's RAD1_INT_KNOWN_MASK comment
 * discloses this as an approximation), and a guest write-clears exactly
 * the bits it read (rad1_inthandler()'s own documented behavior).
 */
static void test_interrupt_status_write_clear(void)
{
    QTestState *qts = rad1_init();

    vram_write(qts, 0x4000, 0x00000000u);
    regzero_write(qts, R_TEX_BASE_ADDR, 0x4000);
    regzero_write(qts, R_FB_HW_WRITE_MASK, 0xffffffffu);
    regzero_write(qts, R_RECT_ORIGIN, RECT(0, 0));
    regzero_write(qts, R_RECT_SIZE, SIZE(1, 1));
    regzero_write(qts, R_RENDER, 1);

    uint32_t status = regzero_read(qts, R_INT_STATUS);
    g_assert_cmphex(status, ==, INT_KNOWN_MASK);

    regzero_write(qts, R_INT_STATUS, status);
    g_assert_cmphex(regzero_read(qts, R_INT_STATUS), ==, 0);

    qtest_quit(qts);
}

/*
 * The full JEDEC/AMD unlock-and-program sequence
 * (docs/datasheets/psitech-rad1.md, "Startup and flash programming")
 * programs one byte; reset-erased flash reads all-ones, and a real NOR
 * program cycle can only clear bits, matching this model's bitwise AND.
 */
static void test_flash_program_sequence(void)
{
    QTestState *qts = rad1_init();

    g_assert_cmphex(flash_read(qts, 0x1234), ==, 0xffu);

    flash_write(qts, FLASH_UNLOCK1_ADDR, FLASH_UNLOCK1_DATA);
    flash_write(qts, FLASH_UNLOCK2_ADDR, FLASH_UNLOCK2_DATA);
    flash_write(qts, FLASH_PROGRAM_ADDR, FLASH_PROGRAM_DATA);
    flash_write(qts, 0x1234, 0x5au);

    g_assert_cmphex(flash_read(qts, 0x1234), ==, 0x5au);

    qtest_quit(qts);
}

/*
 * A data write that skips the unlock sequence leaves flash content
 * unchanged -- reproducing, at the register-contract level, the byte-
 * mismatch condition program_eeprom_sector()'s caller detects
 * (docs/datasheets/psitech-rad1.md, "Required qtests before guest
 * execution", item 5), without this model needing to know that caller's
 * own error-code convention.
 */
static void test_flash_program_without_unlock_is_ignored(void)
{
    QTestState *qts = rad1_init();

    flash_write(qts, 0x1234, 0x5au);
    g_assert_cmphex(flash_read(qts, 0x1234), ==, 0xffu);

    /* An interrupted sequence (wrong second unlock byte) also aborts. */
    flash_write(qts, FLASH_UNLOCK1_ADDR, FLASH_UNLOCK1_DATA);
    flash_write(qts, FLASH_UNLOCK2_ADDR, 0x00);
    flash_write(qts, FLASH_PROGRAM_ADDR, FLASH_PROGRAM_DATA);
    flash_write(qts, 0x1234, 0x5au);
    g_assert_cmphex(flash_read(qts, 0x1234), ==, 0xffu);

    qtest_quit(qts);
}

static void test_reset(void)
{
    QTestState *qts = rad1_init();

    regzero_write(qts, R_RECT_ORIGIN, RECT(1, 2));
    regzero_write(qts, R_FB_HW_WRITE_MASK, 0x12345678u);
    vram_write(qts, 0, 0x00ffffffu);
    flash_write(qts, FLASH_UNLOCK1_ADDR, FLASH_UNLOCK1_DATA);
    flash_write(qts, FLASH_UNLOCK2_ADDR, FLASH_UNLOCK2_DATA);
    flash_write(qts, FLASH_PROGRAM_ADDR, FLASH_PROGRAM_DATA);
    flash_write(qts, 0x10, 0x00u);

    qtest_system_reset(qts);
    rad1_configure_bars(qts);

    g_assert_cmphex(regzero_read(qts, R_RECT_ORIGIN), ==, 0);
    g_assert_cmphex(regzero_read(qts, R_FB_HW_WRITE_MASK), ==, 0xffffffffu);
    g_assert_cmphex(vram_read(qts, 0), ==, 0);
    g_assert_cmphex(flash_read(qts, 0x10), ==, 0xffu);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/psitech-rad1/pci-identity", test_pci_identity);
    qtest_add_func("/psitech-rad1/control-status-registers",
                   test_control_status_registers);
    qtest_add_func("/psitech-rad1/unsupported-offset-is-safe",
                   test_unsupported_offset_is_safe);
    qtest_add_func("/psitech-rad1/ramdac-round-trip",
                   test_ramdac_round_trip);
    qtest_add_func("/psitech-rad1/render-local-copy", test_render_local_copy);
    qtest_add_func("/psitech-rad1/render-host-copy", test_render_host_copy);
    qtest_add_func("/psitech-rad1/render-honors-write-mask",
                   test_render_honors_write_mask);
    qtest_add_func("/psitech-rad1/render-readback", test_render_readback);
    qtest_add_func("/psitech-rad1/bydma-registers-round-trip",
                   test_bydma_registers_round_trip);
    qtest_add_func("/psitech-rad1/bydma-write-transfer",
                   test_bydma_write_transfer);
    qtest_add_func("/psitech-rad1/bydma-byte-mask-edges",
                   test_bydma_byte_mask_edges);
    qtest_add_func("/psitech-rad1/interrupt-status-write-clear",
                   test_interrupt_status_write_clear);
    qtest_add_func("/psitech-rad1/flash-program-sequence",
                   test_flash_program_sequence);
    qtest_add_func("/psitech-rad1/flash-program-without-unlock-is-ignored",
                   test_flash_program_without_unlock_is_ignored);
    qtest_add_func("/psitech-rad1/reset", test_reset);

    return g_test_run();
}
