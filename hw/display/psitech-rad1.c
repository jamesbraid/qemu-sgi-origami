/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * psitech-rad1: bounded functional approximation of a PsiTech RAD1 PCI
 * graphics head, one primary head on an Origin 200 Bridge PCI slot.
 *
 * Primary PCI Vendor/Device ID is 104c:3d07 -- the raw TI/3Dlabs TVP4020
 * (Permedia 2) die's own identity, not PsiTech's 12d1:0101 board identity.
 * The IRIX kernel driver's boot-time registration function (`rad1pciinit`,
 * disassembled and live-traced; see docs/datasheets/psitech-rad1.md,
 * "Kernel-driver attach: the real cause is a PCI identity mismatch") calls
 * `pciio_driver_register` against 104c:3d07 and, separately, 3d3d:0009
 * (the same chip under 3Dlabs' own vendor ID) -- never against 12d1:0101.
 * IRIX's PCI bus-probe path matches a live card's primary Vendor/Device ID
 * against exactly that registered table, so a card presenting 12d1:0101 as
 * its primary identity is never claimed by any driver. 104c:3d07 is chosen
 * over the 3d3d:0009 alternative as an APPROXIMATION -- the product sheet's
 * first-listed stock reference and `rad1pciinit`'s first registration
 * call, but no evidence yet establishes which physical chip revision this
 * project's modeled RAD1 should present; see the datasheet section above
 * for the open item. 12d1:0101 remains real and is exposed separately as
 * the PCI Subsystem Vendor/Device ID, matching what the datasheet's "PCI
 * and address spaces" section and the standalone PROM textport driver's own
 * subsystem-ID check (a different, standard PCI config-space field from
 * the primary Vendor/Device ID) use for board identity independent of bus
 * probing.
 *
 * Unlike psitech-rad4.c, RAD1 is built directly on the TI/3Dlabs TVP4020
 * (Permedia2/Permedia2V) with 8 MB of onboard SGRAM, an integrated RAMDAC,
 * and no PLX bridge or external RGB561-style RAMDAC. It has no PLX-style
 * scatter/gather DMA engine: every rectangle-move path the shipped driver
 * uses (`rect_pio_move`, `rect_dma_move`, `rad1_dma_patch_move`,
 * `bigx_update`) drives the same Permedia2 rasterizer/texture-read GP
 * register set through the FIFO-tagged block at Region Zero
 * 0x8000-0xFFFF; what the driver calls "DMA" is Permedia2's own
 * texture-mapped rectangle copy, using an ordinary kernel DMA mapping only
 * to get a device-visible host physical address for TextureBaseAddress,
 * not a separate descriptor-chain format.
 *
 * This models a bounded compatibility profile for the flat, non-
 * interpolated rectangle copy the shipped `rad1pci` kernel driver and
 * PROM/standalone textport library actually perform, not full Permedia2
 * hardware fidelity: no 3D rendering, blending, logical ops, or texture
 * filtering is implemented. Every approximated behavior below is named,
 * evidenced and requires independent review before it lands, per the
 * owning Origami repository's datasheet-before-device rule:
 * docs/datasheets/psitech-rad1.md. That document's own clearance checklist
 * was not fully closed when this file was written -- several items below
 * are explicitly marked APPROXIMATED rather than established, and this
 * file must not be treated as hardware-qualified until that document's
 * review items are closed. Read that document before changing register
 * policy here; a policy change needs renewed independent review there
 * before it lands here.
 *
 * Register offsets, bit layouts and semantics not directly evidenced by
 * the disassembled driver are cross-checked against the public TI/3Dlabs
 * TVP4020 (Permedia 2) Programmer's Reference Manual (bitsavers.org,
 * cached at docs/library/cache/graphics/rad-psitech/ in the owning
 * repository); manual page numbers cited in comments below are the PDF's
 * own page count, matching docs/datasheets/psitech-rad1.md's own citation
 * convention -- not the document's printed page numbers, which run about
 * nine lower throughout the register reference chapter.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/qemu-pixman.h"
#include "qom/object.h"

#define TYPE_PSITECH_RAD1 "psitech-rad1"
OBJECT_DECLARE_SIMPLE_TYPE(PsitechRad1State, PSITECH_RAD1)

/*
 * Primary identity: the raw TI TVP4020 (Permedia 2) die, per
 * `rad1pciinit`'s first pciio_driver_register() call. APPROXIMATED choice
 * over the alternative 3Dlabs-badged 3d3d:0009 identity the same call
 * registers second -- see the file comment above and
 * docs/datasheets/psitech-rad1.md.
 */
#define RAD1_VENDOR_ID          0x104c
#define RAD1_DEVICE_ID          0x3d07

/*
 * PsiTech's own board/subsystem identity (public 1999 product sheet),
 * exposed as the PCI Subsystem Vendor/Device ID rather than the primary
 * Vendor/Device ID -- what the standalone PROM textport driver's own
 * subsystem-ID check matches against.
 */
#define RAD1_SUBSYSTEM_VENDOR_ID 0x12d1
#define RAD1_SUBSYSTEM_DEVICE_ID 0x0101

/*
 * BAR sizing. Region Zero's 128 KiB size and its role (control/RAMDAC/
 * GP-register aperture) are established (docs/datasheets/psitech-rad1.md,
 * "PCI and address spaces": PCI selector 8, softc +0x38). Which PCI base-
 * address register the driver's own selector numbering actually names for
 * the framebuffer (selector 9, most likely) and for the flash/EEPROM
 * pointer (softc +0x48, not reached through any of the four traced
 * selectors at all) were not established. BAR1 for the framebuffer and
 * BAR2 for flash are this file's own minimum-reasonable choice of PCI
 * resource numbering to make a guest attach possible, APPROXIMATED and
 * disclosed here and in the datasheet's clearance checklist -- not a
 * confirmed hardware fact.
 */
#define RAD1_REGZERO_SIZE       (128 * KiB)
#define RAD1_FB_SIZE            (8 * MiB)
#define RAD1_FLASH_SIZE         (64 * KiB)     /* APPROXIMATED, see below. */

/*
 * Region Zero range table (docs/datasheets/psitech-rad1.md, "PCI and
 * address spaces", matching the manual's own Region Zero address map,
 * Table 3.1 page 15). Only Control & Status, RAMDAC and GP Registers are
 * established as driver-reached; Memory Control, Video Control, Video
 * Streams, SVGA Control and Reserved are modeled as unimplemented and
 * trace-visible rather than guessed at.
 */
#define RAD1_RANGE_CTRL_STATUS_LAST    0x0fff
#define RAD1_RANGE_MEMCTRL_LAST        0x1fff
#define RAD1_RANGE_GPFIFO_LAST         0x2fff
#define RAD1_RANGE_VIDEOCTRL_LAST      0x3fff
#define RAD1_RANGE_RAMDAC_FIRST        0x4000
#define RAD1_RANGE_RAMDAC_LAST         0x4fff
#define RAD1_RANGE_VIDEOSTREAMS_LAST   0x5fff
#define RAD1_RANGE_SVGACTRL_LAST       0x6fff
#define RAD1_RANGE_RESERVED_LAST       0x7fff
#define RAD1_RANGE_GPREGS_FIRST        0x8000
#define RAD1_RANGE_GPREGS_LAST         0xffff

/*
 * Control & Status block. offset 0x10 is the interrupt status word
 * rad1_inthandler() reads and write-clears; offset 0x08 is a second
 * register the same handler unconditionally clears afterward, role not
 * established beyond that -- modeled as a plain read/write register with
 * no independent side effect. Offset 0x18 is this session's inferred
 * "InFIFOSpace" location (the driver polls some counter here against
 * thresholds before GP-register writes; the exact offset was not found as
 * a named manual entry, only inferred from the driver's own polling
 * pattern) -- APPROXIMATED: since this model executes every GP-register
 * batch synchronously with no real FIFO depth limit, this register always
 * reports far more space than any threshold the driver checks, so the
 * driver's polling loop never stalls.
 */
#define RAD1_REG_CTRL_ACK       0x08
#define RAD1_REG_INT_STATUS     0x10
#define RAD1_REG_FIFO_SPACE     0x18
#define RAD1_FIFO_SPACE_READY   0x00010000u

/*
 * Offset 0x40: live-gdb tracing against the shipped kernel (breakpoints at
 * the addresses docs/datasheets/psitech-rad1.md's "Kernel-driver attach"
 * section records) found `rad1pciattach` reads this word through the
 * mapped Region Zero aperture itself -- not, as an earlier session's
 * static reading guessed, a validity check on the softc pointer for PCI
 * selector 9 (the framebuffer BAR). `rad1pciattach` converts the raw word
 * to a boolean (`sltu`) and both (a) passes it as `rad1_pinit`'s second
 * argument and (b) branches on it directly to decide whether attach
 * continues toward `pciio_intr_alloc`/`hwgraph_char_device_add` at all.
 * Tracing 36 live invocations of this check across one boot, the raw word
 * was 0 every time: this model had no case for offset 0x40, so it fell
 * through to the Control & Status block's default handler and returned 0.
 * That is the confirmed, direct cause of the missing `/hw` vertex.
 *
 * Which Permedia2 Control & Status register (if any named one) actually
 * lives at this offset was not established this session -- the cached
 * TI/3Dlabs manual could not be read from this environment, and
 * rad1_pinit's own use of the flag (skipping one extra block of
 * GP-register zeroing when false) does not by itself name the bit.
 * APPROXIMATED and disclosed: this model returns a fixed nonzero word
 * here, which is sufficient for every use this session traced (a plain
 * zero/nonzero gate, not a specific bit pattern) -- not a confirmed
 * hardware value, and not yet independently reviewed.
 */
#define RAD1_REG_ATTACH_GATE    0x40
#define RAD1_ATTACH_GATE_VALUE  0x00000001u

/*
 * Interrupt status bits rad1_inthandler() decodes (docs/datasheets/
 * psitech-rad1.md, "Interrupts and errors"): bit 0, bit 1 and bit 7 each
 * signal a distinct softc wait-queue slot up to 200 times. This session
 * did not establish which bit corresponds to which of
 * rad1_dma_patch_move()'s four completion paths, or a mask/enable
 * register. APPROXIMATED: every completed Render sets all three known
 * bits together rather than discriminating between them, and interrupt
 * delivery is unconditional (no enable gate) since none is evidenced.
 * Any other status bit is unimplemented and left visible in the status
 * word rather than silently swallowed.
 */
#define RAD1_INT_BIT0           (1u << 0)
#define RAD1_INT_BIT1           (1u << 1)
#define RAD1_INT_BIT7           (1u << 7)
#define RAD1_INT_KNOWN_MASK     (RAD1_INT_BIT0 | RAD1_INT_BIT1 | RAD1_INT_BIT7)

/*
 * RAMDAC block, Region Zero 0x4000-0x4FFF (docs/datasheets/psitech-rad1.md
 * table row for that range): rad1_init_RAMDAC()/rad1_load_lut() are
 * established to write exactly these seven offsets. This model stores
 * each as a plain register; it does not apply palette or gamma content to
 * displayed pixels (see "Scanout" below), since the qualified guest
 * workload is scoped to a flat, non-interpolated rectangle copy and no
 * RAD1-specific pseudocolor/gamma pixel path is evidenced.
 */
#define RAD1_RAMDAC_R0          0x4000
#define RAD1_RAMDAC_R1          0x4010
#define RAD1_RAMDAC_R2          0x4020
#define RAD1_RAMDAC_R3          0x4028
#define RAD1_RAMDAC_R4          0x4030
#define RAD1_RAMDAC_R5          0x4038
#define RAD1_RAMDAC_R6          0x4050

/*
 * FIFO-tagged GP (rasterizer/texture) registers, Region Zero 0x8000-0xFFFF
 * (docs/datasheets/psitech-rad1.md, "Rectangle transfer and the FIFO-
 * tagged GP registers"). Offsets independently cross-checked against the
 * TVP4020 Programmer's Reference Manual's own per-register table entries
 * (page numbers in comments below are the PDF's own page count, matching
 * this file's own top-of-file citation convention and
 * docs/datasheets/psitech-rad1.md's, e.g. RectangleOrigin/RectangleSize
 * page 241 -- not the manual's printed page numbers, which run about nine
 * lower).
 */
#define RAD1_REG_RENDER             0x8038  /* manual: write-only command */
#define RAD1_REG_WAIT_FOR_COMPLETE 0x8088  /* manual page 275: write-only */
#define RAD1_REG_RECT_ORIGIN        0x80d0  /* manual page 241 */
#define RAD1_REG_RECT_SIZE          0x80d8  /* manual page 241 */
#define RAD1_REG_TEX_ADDR_MODE      0x8380
#define RAD1_REG_S_START            0x8388
#define RAD1_REG_DSDX               0x8390
#define RAD1_REG_DSDYDOM            0x8398
#define RAD1_REG_T_START            0x83a0
#define RAD1_REG_DTDX               0x83a8
#define RAD1_REG_TEX_BASE_ADDR      0x8580  /* manual page 260 */
#define RAD1_REG_TEX_DATA_FORMAT    0x8590
#define RAD1_REG_TEX_READ_MODE      0x8670
#define RAD1_REG_TEXEL_LUT_MODE     0x8678
#define RAD1_REG_TEX_COLOR_MODE     0x8680
#define RAD1_REG_ALPHA_BLEND_MODE   0x8810
#define RAD1_REG_LOGICAL_OP_MODE    0x8828
#define RAD1_REG_FB_READ_MODE       0x8a80  /* manual page 211 */
#define RAD1_REG_FB_WRITE_MODE      0x8ab8
#define RAD1_REG_FB_HW_WRITE_MASK   0x8ac0  /* manual page 209 */
#define RAD1_REG_FILTER_MODE        0x8c00  /* manual page 219 */
#define RAD1_REG_SYNC               0x8c40

/*
 * RectangleOrigin/RectangleSize bit packing, confirmed directly against
 * the manual's own register diagram (page 241): X/Width in bits 0-15, Y/
 * Height in bits 16-31.
 */
#define RAD1_RECT_LOW_MASK      0xffffu
#define RAD1_RECT_HIGH_SHIFT    16

/*
 * TextureBaseAddress bit 30 (manual page 260): "If bit 30 is set the
 * texture resides in system memory rather than local buffer and should be
 * fetched across the PCI bus." This resolves, from the public manual
 * itself, the local-vs-host addressing ambiguity
 * docs/datasheets/psitech-rad1.md left open for `TextureBaseAddress` --
 * the datasheet's own text notes the driver writes either a host DMA
 * address or a same-surface framebuffer address into this register
 * without independently tying that choice to a bit. This model honors
 * bit 30 as the manual documents it. The low bits are nominally a texel
 * count "from the base of the memory" per the manual; the shipped
 * driver's own DMA path writes a raw kernel-computed physical address
 * directly into this register (docs/datasheets/psitech-rad1.md, "What
 * 'DMA' means for this driver"), so this model treats bits 0-23 as a
 * plain byte offset/address rather than re-deriving a texel-to-byte
 * scale -- APPROXIMATED, disclosed here and in the datasheet.
 *
 * Bit 31 is deliberately left in place by RAD1_TEXBASE_ADDR_MASK (it only
 * clears bit 30): a system-memory address the driver computes through
 * IRIX's pciio_dmatrans_addr() carries the Bridge's own PCI_DMA_DIRECT
 * address-space-selector bit as part of the same 32-bit value, and
 * pci_dma_read()/pci_dma_write() need that bit intact to resolve the
 * address at all -- matching psitech-rad4.c's own DMA descriptor handling,
 * which never strips it either.
 */
#define RAD1_TEXBASE_SYSTEM_MEMORY  (1u << 30)
#define RAD1_TEXBASE_ADDR_MASK      0xbfffffffu

/*
 * FBReadMode bits 9/10 (manual page 211): Read Source Enable and Read
 * Destination Enable. docs/datasheets/psitech-rad1.md's own reading of
 * this register is narrower -- "used for the read direction (framebuffer
 * -> host) of rad1_dma_patch_move" -- than the manual's general-purpose
 * framebuffer-read-unit description, and does not tie that behavior to a
 * specific bit. APPROXIMATED: this model treats Read Destination Enable
 * (bit 10) as the signal that a Render targets a host-directed
 * framebuffer read-back (the "Host Out" path FilterMode/Sync also
 * describe) rather than an ordinary forward copy into the framebuffer;
 * this conflates the manual's general read-enable bit with the specific
 * host-readback use this driver makes of it, and needs independent review
 * once a live capture of an actual read-back transfer exists.
 */
#define RAD1_FBREADMODE_READBACK_BIT   (1u << 10)

/*
 * Flat-copy pixel size: 32-bit words, matching the RAD4 sibling product's
 * established True-color framebuffer layout. RAD1's own pixel format is
 * not independently confirmed -- APPROXIMATED.
 */
#define RAD1_BYTES_PER_PIXEL    4

/*
 * The three fixed mode records (docs/datasheets/psitech-rad1.md,
 * "Startup and flash programming": NumPER2Modes == 3, matching the
 * product sheet's three listed resolutions exactly). How the driver picks
 * among them at startup is an open question in that document.
 * APPROXIMATED: this model always runs mode 0 (the product sheet's first-
 * listed resolution) and does not implement mode switching; this is the
 * minimum-reasonable choice to get a boot attempt working, not a modeled
 * selection mechanism.
 */
typedef struct Rad1ModeRecord {
    uint32_t width;
    uint32_t height;
} Rad1ModeRecord;

#define RAD1_NUM_MODES 3
static const Rad1ModeRecord rad1_mode_records[RAD1_NUM_MODES] = {
    { 1280, 1024 },
    { 1024, 768 },
    { 1024, 1024 },
};
#define RAD1_ACTIVE_MODE 0

/*
 * Flash/EEPROM programming (docs/datasheets/psitech-rad1.md, "Startup and
 * flash programming"): a generic JEDEC/AMD two-cycle parallel NOR flash
 * unlock-and-program sequence, industry-standard and independent of any
 * RAD1- or IRIX-specific source. Which PCI resource carries the softc
 * +0x48 flash pointer, and the flash part's actual size, were not traced
 * -- RAD1_FLASH_SIZE and its BAR number are this file's own minimum-
 * reasonable choice, APPROXIMATED. Byte-wide access (unlike Region Zero's
 * confirmed 32-bit-only accesses) is inferred from the classic parallel
 * NOR unlock algorithm itself, which only makes sense as byte offsets
 * into an 8-bit-wide parallel flash part; this is a generic inference
 * from the algorithm, not a disassembled access-width fact.
 *
 * Unlock addresses: the datasheet's disassembly reports the driver's own
 * CPU-side literal offsets as 0x5556 and 0x2AA9, one off from the
 * universal JEDEC/AMD standard unlock addresses 0x5555 and 0x2AAA (used
 * industry-wide, e.g. the Am29F0x0 family). A qtest against this device
 * confirmed why directly: the Bridge applies a real byte-lane swap
 * (addr ^ 3) to sub-word PCI accesses from this big-endian host to a
 * little-endian target -- the same class of hardware behavior this
 * project's Bridge model already documents for DMA (docs/comment-style.md
 * and the Bridge qtest coverage). The driver's own literal byte offset is
 * therefore the pre-swap, CPU-side address; what the flash chip itself
 * sees, and what this model's state machine below matches against, is the
 * standard post-swap JEDEC address. This model uses the standard
 * addresses directly rather than re-deriving the swap itself, since the
 * swap is the Bridge's behavior, not this device's.
 */
#define RAD1_FLASH_UNLOCK1_ADDR     0x5555
#define RAD1_FLASH_UNLOCK1_DATA     0xaa
#define RAD1_FLASH_UNLOCK2_ADDR     0x2aaa
#define RAD1_FLASH_UNLOCK2_DATA     0x55
#define RAD1_FLASH_PROGRAM_ADDR     0x5555
#define RAD1_FLASH_PROGRAM_DATA     0xa0

typedef enum Rad1FlashState {
    RAD1_FLASH_IDLE,
    RAD1_FLASH_UNLOCK1,
    RAD1_FLASH_UNLOCK2,
    RAD1_FLASH_ARMED,
} Rad1FlashState;

struct PsitechRad1State {
    PCIDevice pci;

    MemoryRegion regzero;
    MemoryRegion vram;
    MemoryRegion flash;

    QemuConsole *con;

    /*
     * Framebuffer storage. Unlike RAD4's own BAR2, this is not a genuine
     * RAM-backed MemoryRegion: the Bridge's PIO window applies the same
     * byte-lane behavior to every device on it, and this codebase's own
     * working convention (hw/display/psitech-rad4.c's BAR0 and BAR2 both)
     * is to declare DEVICE_LITTLE_ENDIAN MemoryRegionOps and store
     * multi-byte values explicitly big-endian (ldl_be_p()/stl_be_p())
     * inside a plain byte array -- not to rely on a RAM region's own
     * default handling, which does not carry that same correction. A
     * qtest regression (an earlier revision of this file used
     * memory_region_init_ram() here) demonstrated the resulting byte swap
     * directly. `display_buffer` is a separate, host-native-order copy
     * rad1_gfx_update() maintains for scanout, exactly matching
     * rad4_convert_scanout()'s role in the sibling device.
     */
    uint8_t *framebuffer;
    uint32_t *display_buffer;
    uint8_t *flash_data;

    /* Control & Status block. */
    uint32_t int_status;
    uint32_t ctrl_ack;

    /* RAMDAC block: seven established offsets, stored only. */
    uint32_t ramdac[7];

    /* GP register block. */
    uint32_t rect_origin;
    uint32_t rect_size;
    uint32_t tex_addr_mode;
    uint32_t s_start;
    uint32_t t_start;
    uint32_t dsdx;
    uint32_t dsdydom;
    uint32_t dtdx;
    uint32_t tex_base_addr;
    uint32_t tex_data_format;
    uint32_t tex_read_mode;
    uint32_t texel_lut_mode;
    uint32_t tex_color_mode;
    uint32_t alpha_blend_mode;
    uint32_t logical_op_mode;
    uint32_t fb_read_mode;
    uint32_t fb_write_mode;
    uint32_t fb_hw_write_mask;
    uint32_t filter_mode;
    uint32_t sync_reg;

    /* Flash/EEPROM programming state machine. */
    int flash_state; /* Rad1FlashState, as plain int for VMState */
};

static int rad1_ramdac_index(hwaddr addr)
{
    switch (addr) {
    case RAD1_RAMDAC_R0: return 0;
    case RAD1_RAMDAC_R1: return 1;
    case RAD1_RAMDAC_R2: return 2;
    case RAD1_RAMDAC_R3: return 3;
    case RAD1_RAMDAC_R4: return 4;
    case RAD1_RAMDAC_R5: return 5;
    case RAD1_RAMDAC_R6: return 6;
    default: return -1;
    }
}

static void rad1_update_irq(PsitechRad1State *s)
{
    /*
     * No mask/enable register is evidenced (docs/datasheets/
     * psitech-rad1.md, "Interrupts and errors"), so this model asserts
     * INTA# whenever any status bit is pending and deasserts once the
     * guest write-clears them all.
     */
    pci_set_irq(&s->pci, s->int_status != 0);
}

/*
 * Perform one flat, non-interpolated rectangle copy per the Render
 * command, per docs/datasheets/psitech-rad1.md's explicit scope ("What
 * 'DMA' means for this driver"). This model runs the whole operation
 * synchronously inside the register write that triggers it -- there is no
 * real FIFO depth or rasterizer latency to model for the one operation
 * kind this driver is established to use.
 *
 * Two directions are modeled, selected by RAD1_FBREADMODE_READBACK_BIT
 * (APPROXIMATED, see its definition above):
 *   - forward (default): copy from the texture-read source named by
 *     TextureBaseAddress into the framebuffer rectangle named by
 *     RectangleOrigin/RectangleSize, honoring FBHardwareWriteMask.
 *   - read-back: copy the framebuffer rectangle named by
 *     RectangleOrigin/RectangleSize out to the host/PCI address named by
 *     TextureBaseAddress. FBHardwareWriteMask does not apply to a
 *     host-memory destination.
 *
 * Source and destination are both treated as one contiguous run of
 * RAD1_BYTES_PER_PIXEL-sized words per scanline, width RectangleSize.width
 * -- i.e. this model assumes the source rectangle's own stride equals its
 * width, matching a flat copy with no scaling (dSdx/dSdyDom/dTdx are
 * stored but not applied, since Permedia2's fixed-point texture-stepping
 * format is out of scope for a copy the driver itself never uses for
 * actual texture mapping). SStart/TStart offset into the source as a
 * texel-granularity (x, y) origin, matching their documented role as the
 * source rectangle's origin.
 */
static void rad1_do_render(PsitechRad1State *s)
{
    uint32_t width = s->rect_size & RAD1_RECT_LOW_MASK;
    uint32_t height = (s->rect_size >> RAD1_RECT_HIGH_SHIFT) &
                      RAD1_RECT_LOW_MASK;
    uint32_t dst_x = s->rect_origin & RAD1_RECT_LOW_MASK;
    uint32_t dst_y = (s->rect_origin >> RAD1_RECT_HIGH_SHIFT) &
                     RAD1_RECT_LOW_MASK;
    const Rad1ModeRecord *mode = &rad1_mode_records[RAD1_ACTIVE_MODE];
    uint32_t fb_stride = mode->width * RAD1_BYTES_PER_PIXEL;
    bool readback = (s->fb_read_mode & RAD1_FBREADMODE_READBACK_BIT) != 0;
    uint32_t row_bytes;
    uint8_t *linebuf;
    uint32_t row;

    if (width == 0 || height == 0) {
        return;
    }
    if (dst_x > mode->width || dst_y > mode->height ||
        width > mode->width - dst_x || height > mode->height - dst_y) {
        qemu_log_mask(LOG_GUEST_ERROR,
                     "psitech-rad1: Render rectangle (%u,%u)+(%ux%u) falls "
                     "outside the active %ux%u mode\n",
                     dst_x, dst_y, width, height, mode->width, mode->height);
        return;
    }

    row_bytes = width * RAD1_BYTES_PER_PIXEL;
    linebuf = g_malloc(row_bytes);

    if (readback) {
        hwaddr host_addr = s->tex_base_addr & RAD1_TEXBASE_ADDR_MASK;

        for (row = 0; row < height; row++) {
            uint8_t *src = s->framebuffer + (dst_y + row) * fb_stride +
                          dst_x * RAD1_BYTES_PER_PIXEL;

            if (pci_dma_write(&s->pci, host_addr + (hwaddr)row * row_bytes,
                              src, row_bytes) != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                             "psitech-rad1: read-back write failed at "
                             "0x%"HWADDR_PRIx"\n", host_addr);
                break;
            }
        }
    } else {
        bool system_memory = (s->tex_base_addr &
                              RAD1_TEXBASE_SYSTEM_MEMORY) != 0;
        hwaddr src_base = s->tex_base_addr & RAD1_TEXBASE_ADDR_MASK;
        uint32_t src_x = s->s_start & RAD1_RECT_LOW_MASK;
        uint32_t src_y = s->t_start & RAD1_RECT_LOW_MASK;
        hwaddr src_origin = src_base + (hwaddr)src_y * row_bytes +
                            (hwaddr)src_x * RAD1_BYTES_PER_PIXEL;

        for (row = 0; row < height; row++) {
            uint8_t *dst = s->framebuffer + (dst_y + row) * fb_stride +
                           dst_x * RAD1_BYTES_PER_PIXEL;
            hwaddr src_addr = src_origin + (hwaddr)row * row_bytes;

            if (system_memory) {
                if (pci_dma_read(&s->pci, src_addr, linebuf,
                                 row_bytes) != MEMTX_OK) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                 "psitech-rad1: texture-read failed at "
                                 "0x%"HWADDR_PRIx"\n", src_addr);
                    break;
                }
            } else {
                if (src_addr + row_bytes > RAD1_FB_SIZE) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                 "psitech-rad1: local texture read at "
                                 "0x%"HWADDR_PRIx" falls outside the "
                                 "framebuffer\n", src_addr);
                    break;
                }
                memcpy(linebuf, s->framebuffer + src_addr, row_bytes);
            }

            if (s->fb_hw_write_mask == 0xffffffffu) {
                memcpy(dst, linebuf, row_bytes);
            } else {
                uint32_t col;

                for (col = 0; col < width; col++) {
                    uint32_t old = ldl_be_p(dst + col * 4);
                    uint32_t incoming = ldl_be_p(linebuf + col * 4);
                    uint32_t mask = s->fb_hw_write_mask;

                    stl_be_p(dst + col * 4,
                            (old & ~mask) | (incoming & mask));
                }
            }
        }
    }

    g_free(linebuf);

    /* APPROXIMATED completion signaling; see RAD1_INT_KNOWN_MASK above. */
    s->int_status |= RAD1_INT_KNOWN_MASK;
    rad1_update_irq(s);
}

static uint64_t rad1_regzero_read(void *opaque, hwaddr addr, unsigned size)
{
    PsitechRad1State *s = opaque;
    int ramdac_idx;

    if (addr <= RAD1_RANGE_CTRL_STATUS_LAST) {
        switch (addr) {
        case RAD1_REG_CTRL_ACK:
            return s->ctrl_ack;
        case RAD1_REG_INT_STATUS:
            return s->int_status;
        case RAD1_REG_FIFO_SPACE:
            return RAD1_FIFO_SPACE_READY;
        case RAD1_REG_ATTACH_GATE:
            return RAD1_ATTACH_GATE_VALUE;
        default:
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad1: unsupported Control & Status read "
                         "at 0x%04x\n", (unsigned)addr);
            return 0;
        }
    }

    ramdac_idx = rad1_ramdac_index(addr);
    if (ramdac_idx >= 0) {
        return s->ramdac[ramdac_idx];
    }
    if (addr >= RAD1_RANGE_RAMDAC_FIRST && addr <= RAD1_RANGE_RAMDAC_LAST) {
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad1: unsupported RAMDAC read at 0x%04x\n",
                     (unsigned)addr);
        return 0;
    }

    if (addr >= RAD1_RANGE_GPREGS_FIRST && addr <= RAD1_RANGE_GPREGS_LAST) {
        switch (addr) {
        case RAD1_REG_WAIT_FOR_COMPLETE:
            /*
             * Manual page 275: write-only command register, no readable
             * status bits. This model completes every Render synchronously,
             * so there is nothing to wait for by the time a read could
             * observe it; return 0 rather than asserting a specific
             * "busy" bit semantics the manual does not document.
             */
            return 0;
        case RAD1_REG_RECT_ORIGIN: return s->rect_origin;
        case RAD1_REG_RECT_SIZE: return s->rect_size;
        case RAD1_REG_TEX_ADDR_MODE: return s->tex_addr_mode;
        case RAD1_REG_S_START: return s->s_start;
        case RAD1_REG_T_START: return s->t_start;
        case RAD1_REG_DSDX: return s->dsdx;
        case RAD1_REG_DSDYDOM: return s->dsdydom;
        case RAD1_REG_DTDX: return s->dtdx;
        case RAD1_REG_TEX_BASE_ADDR: return s->tex_base_addr;
        case RAD1_REG_TEX_DATA_FORMAT: return s->tex_data_format;
        case RAD1_REG_TEX_READ_MODE: return s->tex_read_mode;
        case RAD1_REG_TEXEL_LUT_MODE: return s->texel_lut_mode;
        case RAD1_REG_TEX_COLOR_MODE: return s->tex_color_mode;
        case RAD1_REG_ALPHA_BLEND_MODE: return s->alpha_blend_mode;
        case RAD1_REG_LOGICAL_OP_MODE: return s->logical_op_mode;
        case RAD1_REG_FB_READ_MODE: return s->fb_read_mode;
        case RAD1_REG_FB_WRITE_MODE: return s->fb_write_mode;
        case RAD1_REG_FB_HW_WRITE_MASK: return s->fb_hw_write_mask;
        case RAD1_REG_FILTER_MODE: return s->filter_mode;
        case RAD1_REG_SYNC: return s->sync_reg;
        default:
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad1: unsupported GP register read at "
                         "0x%04x\n", (unsigned)addr);
            return 0;
        }
    }

    qemu_log_mask(LOG_UNIMP,
                 "psitech-rad1: unsupported Region Zero read at 0x%04x\n",
                 (unsigned)addr);
    return 0;
}

static void rad1_regzero_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    PsitechRad1State *s = opaque;
    int ramdac_idx;

    if (addr <= RAD1_RANGE_CTRL_STATUS_LAST) {
        switch (addr) {
        case RAD1_REG_CTRL_ACK:
            s->ctrl_ack = val;
            break;
        case RAD1_REG_INT_STATUS:
            /* Write-to-clear: the guest writes back the value it read. */
            s->int_status &= ~(uint32_t)val;
            rad1_update_irq(s);
            break;
        case RAD1_REG_FIFO_SPACE:
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad1: unexpected write to read-only FIFO "
                         "space register at 0x%04x\n", (unsigned)addr);
            break;
        default:
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad1: unsupported Control & Status write "
                         "at 0x%04x\n", (unsigned)addr);
            break;
        }
        return;
    }

    ramdac_idx = rad1_ramdac_index(addr);
    if (ramdac_idx >= 0) {
        s->ramdac[ramdac_idx] = val;
        return;
    }
    if (addr >= RAD1_RANGE_RAMDAC_FIRST && addr <= RAD1_RANGE_RAMDAC_LAST) {
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad1: unsupported RAMDAC write at 0x%04x\n",
                     (unsigned)addr);
        return;
    }

    if (addr >= RAD1_RANGE_GPREGS_FIRST && addr <= RAD1_RANGE_GPREGS_LAST) {
        switch (addr) {
        case RAD1_REG_RENDER:
            rad1_do_render(s);
            break;
        case RAD1_REG_WAIT_FOR_COMPLETE:
            /* Manual page 275: data field reserved/unused. */
            break;
        case RAD1_REG_RECT_ORIGIN:
            s->rect_origin = val;
            break;
        case RAD1_REG_RECT_SIZE:
            s->rect_size = val;
            break;
        case RAD1_REG_TEX_ADDR_MODE:
            s->tex_addr_mode = val;
            break;
        case RAD1_REG_S_START:
            s->s_start = val;
            break;
        case RAD1_REG_T_START:
            s->t_start = val;
            break;
        case RAD1_REG_DSDX:
            s->dsdx = val;
            break;
        case RAD1_REG_DSDYDOM:
            s->dsdydom = val;
            break;
        case RAD1_REG_DTDX:
            s->dtdx = val;
            break;
        case RAD1_REG_TEX_BASE_ADDR:
            s->tex_base_addr = val;
            break;
        case RAD1_REG_TEX_DATA_FORMAT:
            s->tex_data_format = val;
            break;
        case RAD1_REG_TEX_READ_MODE:
            s->tex_read_mode = val;
            break;
        case RAD1_REG_TEXEL_LUT_MODE:
            s->texel_lut_mode = val;
            break;
        case RAD1_REG_TEX_COLOR_MODE:
            s->tex_color_mode = val;
            break;
        case RAD1_REG_ALPHA_BLEND_MODE:
            s->alpha_blend_mode = val;
            break;
        case RAD1_REG_LOGICAL_OP_MODE:
            s->logical_op_mode = val;
            break;
        case RAD1_REG_FB_READ_MODE:
            s->fb_read_mode = val;
            break;
        case RAD1_REG_FB_WRITE_MODE:
            s->fb_write_mode = val;
            break;
        case RAD1_REG_FB_HW_WRITE_MASK:
            s->fb_hw_write_mask = val;
            break;
        case RAD1_REG_FILTER_MODE:
            s->filter_mode = val;
            break;
        case RAD1_REG_SYNC:
            s->sync_reg = val;
            break;
        default:
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad1: unsupported GP register write at "
                         "0x%04x (value 0x%"PRIx64")\n",
                         (unsigned)addr, val);
            break;
        }
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                 "psitech-rad1: unsupported Region Zero write at 0x%04x\n",
                 (unsigned)addr);
}

static const MemoryRegionOps rad1_regzero_ops = {
    .read = rad1_regzero_read,
    .write = rad1_regzero_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * Flash/EEPROM aperture: a plain JEDEC/AMD-sequenced parallel NOR flash
 * model. Real NOR flash can only clear bits on a program cycle (an erase
 * sets them), so a program applies a bitwise AND -- this both matches real
 * hardware and gives the qtest register-contract coverage a discriminating
 * check: skipping the unlock sequence leaves the erased (all-ones) content
 * unchanged, reproducing the byte-mismatch condition
 * program_eeprom_sector()'s own caller detects, without this model needing
 * to know that caller's error-code convention.
 *
 * Access width: byte-wide (impl/valid min=max=1), unlike every other
 * region on this device. This is forced by the unlock addresses
 * themselves: 0x5556 and 0x2aa9 are not 4-byte aligned, so a 32-bit-only
 * region would never see a 4-byte access land exactly on them (confirmed
 * directly: an earlier revision of this file declared the flash aperture
 * 32-bit-only, and a qtest driving the documented unlock sequence at
 * those exact offsets silently failed to arm). This session did not
 * disassemble program_eeprom_sector()'s own access width, but a
 * byte-wide parallel NOR part addressed at odd byte offsets is the
 * ordinary, industry-standard shape for this JEDEC/AMD sequence --
 * APPROXIMATED in the sense that the disassembly itself was not the
 * source of this choice, not in the sense that another width is equally
 * plausible.
 */
static uint64_t rad1_flash_read(void *opaque, hwaddr addr, unsigned size)
{
    PsitechRad1State *s = opaque;

    return s->flash_data[addr & (RAD1_FLASH_SIZE - 1)];
}

static void rad1_flash_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    PsitechRad1State *s = opaque;
    uint32_t off = addr & (RAD1_FLASH_SIZE - 1);
    uint8_t byte = val & 0xff;

    switch (s->flash_state) {
    case RAD1_FLASH_IDLE:
        if (off == RAD1_FLASH_UNLOCK1_ADDR && byte == RAD1_FLASH_UNLOCK1_DATA) {
            s->flash_state = RAD1_FLASH_UNLOCK1;
        }
        break;
    case RAD1_FLASH_UNLOCK1:
        if (off == RAD1_FLASH_UNLOCK2_ADDR && byte == RAD1_FLASH_UNLOCK2_DATA) {
            s->flash_state = RAD1_FLASH_UNLOCK2;
        } else {
            s->flash_state = RAD1_FLASH_IDLE;
        }
        break;
    case RAD1_FLASH_UNLOCK2:
        if (off == RAD1_FLASH_PROGRAM_ADDR && byte == RAD1_FLASH_PROGRAM_DATA) {
            s->flash_state = RAD1_FLASH_ARMED;
        } else {
            s->flash_state = RAD1_FLASH_IDLE;
        }
        break;
    case RAD1_FLASH_ARMED:
        s->flash_data[off] &= byte;
        s->flash_state = RAD1_FLASH_IDLE;
        break;
    }
}

static const MemoryRegionOps rad1_flash_ops = {
    .read = rad1_flash_read,
    .write = rad1_flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

/*
 * Framebuffer BAR MemoryRegionOps. Region Zero's confirmed 32-bit-only
 * access width is extended here too: nothing in the datasheet's evidence
 * distinguishes the framebuffer aperture's own access width, and 32-bit
 * only is this device's established convention everywhere else.
 */
static uint64_t rad1_vram_read(void *opaque, hwaddr addr, unsigned size)
{
    PsitechRad1State *s = opaque;

    return ldl_be_p(s->framebuffer + addr);
}

static void rad1_vram_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    PsitechRad1State *s = opaque;

    stl_be_p(s->framebuffer + addr, val);
}

static const MemoryRegionOps rad1_vram_ops = {
    .read = rad1_vram_read,
    .write = rad1_vram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * Scanout. RAD1's framebuffer is presented directly as True-color 32-bit
 * pixels (PIXMAN_x8r8g8b8) with no palette/gamma indirection -- unlike
 * RAD4's RGB561 window-ID/WAT/LUT path, nothing in the scoped guest
 * workload (a flat rectangle copy and the PROM/standalone textport) is
 * evidenced to need indexed color. This is APPROXIMATED: RAD1's actual
 * pixel format was not independently captured.
 *
 * `framebuffer` stores big-endian words (see the struct field's own
 * comment); `display_buffer` is a host-native-order copy this function
 * maintains for qemu_create_displaysurface_from(), the same two-buffer
 * split psitech-rad4.c's rad4_convert_scanout()/display_buffer use for
 * the same reason.
 */
static void rad1_convert_scanout(PsitechRad1State *s)
{
    const Rad1ModeRecord *mode = &rad1_mode_records[RAD1_ACTIVE_MODE];
    uint32_t count = mode->width * mode->height;
    uint32_t i;

    for (i = 0; i < count; i++) {
        s->display_buffer[i] = ldl_be_p(s->framebuffer + i * 4);
    }
}

static bool rad1_gfx_update(void *opaque)
{
    PsitechRad1State *s = opaque;

    rad1_convert_scanout(s);
    qemu_console_update_full(s->con);
    return true;
}

static const GraphicHwOps rad1_gfx_ops = {
    .gfx_update = rad1_gfx_update,
};

static void rad1_ensure_surface(PsitechRad1State *s)
{
    const Rad1ModeRecord *mode = &rad1_mode_records[RAD1_ACTIVE_MODE];
    DisplaySurface *ds = qemu_console_surface(s->con);

    if (ds && surface_data(ds) == (uint8_t *)s->display_buffer) {
        return;
    }
    ds = qemu_create_displaysurface_from(mode->width, mode->height,
                                         PIXMAN_x8r8g8b8,
                                         mode->width * RAD1_BYTES_PER_PIXEL,
                                         (uint8_t *)s->display_buffer);
    qemu_console_set_surface(s->con, ds);
}

static void rad1_reset_hold(Object *obj, ResetType type)
{
    PsitechRad1State *s = PSITECH_RAD1(obj);

    s->int_status = 0;
    s->ctrl_ack = 0;
    memset(s->ramdac, 0, sizeof(s->ramdac));

    s->rect_origin = 0;
    s->rect_size = 0;
    s->tex_addr_mode = 0;
    s->s_start = 0;
    s->t_start = 0;
    s->dsdx = 0;
    s->dsdydom = 0;
    s->dtdx = 0;
    s->tex_base_addr = 0;
    s->tex_data_format = 0;
    s->tex_read_mode = 0;
    s->texel_lut_mode = 0;
    s->tex_color_mode = 0;
    s->alpha_blend_mode = 0;
    s->logical_op_mode = 0;
    s->fb_read_mode = 0;
    s->fb_write_mode = 0;
    s->fb_hw_write_mask = 0xffffffffu;
    s->filter_mode = 0;
    s->sync_reg = 0;

    memset(s->framebuffer, 0, RAD1_FB_SIZE);
    /* Erased NOR flash reads as all-ones. */
    memset(s->flash_data, 0xff, RAD1_FLASH_SIZE);
    s->flash_state = RAD1_FLASH_IDLE;

    rad1_update_irq(s);

    /*
     * APPROXIMATED: startup mode-record selection is an open question
     * (docs/datasheets/psitech-rad1.md, "Startup and flash programming");
     * this model always runs RAD1_ACTIVE_MODE and says so, rather than
     * silently picking a resolution with no trace of having done so.
     */
    qemu_log_mask(LOG_UNIMP,
                 "psitech-rad1: startup mode-record selection is not "
                 "established; defaulting to mode %u (%ux%u)\n",
                 RAD1_ACTIVE_MODE, rad1_mode_records[RAD1_ACTIVE_MODE].width,
                 rad1_mode_records[RAD1_ACTIVE_MODE].height);
}

static void rad1_realize(PCIDevice *dev, Error **errp)
{
    PsitechRad1State *s = PSITECH_RAD1(dev);
    Object *obj = OBJECT(dev);

    memory_region_init_io(&s->regzero, obj, &rad1_regzero_ops, s,
                          "psitech-rad1-regzero", RAD1_REGZERO_SIZE);
    memory_region_init_io(&s->vram, obj, &rad1_vram_ops, s,
                          "psitech-rad1-vram", RAD1_FB_SIZE);
    memory_region_init_io(&s->flash, obj, &rad1_flash_ops, s,
                          "psitech-rad1-flash", RAD1_FLASH_SIZE);

    s->framebuffer = g_malloc0(RAD1_FB_SIZE);
    s->display_buffer = g_malloc0(RAD1_FB_SIZE);
    s->flash_data = g_malloc(RAD1_FLASH_SIZE);

    s->con = qemu_graphic_console_create(DEVICE(dev), 0, &rad1_gfx_ops, s);

    pci_set_byte(&s->pci.config[PCI_REVISION_ID], 0x00);
    pci_config_set_class(s->pci.config, PCI_CLASS_DISPLAY_OTHER);
    pci_set_word(&s->pci.config[PCI_SUBSYSTEM_VENDOR_ID],
                RAD1_SUBSYSTEM_VENDOR_ID);
    pci_set_word(&s->pci.config[PCI_SUBSYSTEM_ID], RAD1_SUBSYSTEM_DEVICE_ID);
    s->pci.config[PCI_INTERRUPT_PIN] = 0x01;

    pci_register_bar(&s->pci, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->regzero);
    pci_register_bar(&s->pci, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->vram);
    pci_register_bar(&s->pci, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->flash);

    rad1_ensure_surface(s);
}

static void rad1_exit(PCIDevice *dev)
{
    PsitechRad1State *s = PSITECH_RAD1(dev);

    qemu_graphic_console_close(s->con);
    g_free(s->framebuffer);
    g_free(s->display_buffer);
    g_free(s->flash_data);
}

static int rad1_post_load(void *opaque, int version_id)
{
    PsitechRad1State *s = opaque;

    rad1_convert_scanout(s);
    rad1_ensure_surface(s);
    rad1_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_psitech_rad1 = {
    .name = TYPE_PSITECH_RAD1,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rad1_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(pci, PsitechRad1State),
        VMSTATE_BUFFER_POINTER_UNSAFE(framebuffer, PsitechRad1State, 1,
                                      RAD1_FB_SIZE),
        VMSTATE_UINT32(int_status, PsitechRad1State),
        VMSTATE_UINT32(ctrl_ack, PsitechRad1State),
        VMSTATE_UINT32_ARRAY(ramdac, PsitechRad1State, 7),
        VMSTATE_UINT32(rect_origin, PsitechRad1State),
        VMSTATE_UINT32(rect_size, PsitechRad1State),
        VMSTATE_UINT32(tex_addr_mode, PsitechRad1State),
        VMSTATE_UINT32(s_start, PsitechRad1State),
        VMSTATE_UINT32(t_start, PsitechRad1State),
        VMSTATE_UINT32(dsdx, PsitechRad1State),
        VMSTATE_UINT32(dsdydom, PsitechRad1State),
        VMSTATE_UINT32(dtdx, PsitechRad1State),
        VMSTATE_UINT32(tex_base_addr, PsitechRad1State),
        VMSTATE_UINT32(tex_data_format, PsitechRad1State),
        VMSTATE_UINT32(tex_read_mode, PsitechRad1State),
        VMSTATE_UINT32(texel_lut_mode, PsitechRad1State),
        VMSTATE_UINT32(tex_color_mode, PsitechRad1State),
        VMSTATE_UINT32(alpha_blend_mode, PsitechRad1State),
        VMSTATE_UINT32(logical_op_mode, PsitechRad1State),
        VMSTATE_UINT32(fb_read_mode, PsitechRad1State),
        VMSTATE_UINT32(fb_write_mode, PsitechRad1State),
        VMSTATE_UINT32(fb_hw_write_mask, PsitechRad1State),
        VMSTATE_UINT32(filter_mode, PsitechRad1State),
        VMSTATE_UINT32(sync_reg, PsitechRad1State),
        VMSTATE_INT32(flash_state, PsitechRad1State),
        VMSTATE_BUFFER_POINTER_UNSAFE(flash_data, PsitechRad1State, 1,
                                      RAD1_FLASH_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void rad1_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    k->vendor_id = RAD1_VENDOR_ID;
    k->device_id = RAD1_DEVICE_ID;
    k->subsystem_vendor_id = RAD1_SUBSYSTEM_VENDOR_ID;
    k->subsystem_id = RAD1_SUBSYSTEM_DEVICE_ID;
    k->class_id = PCI_CLASS_DISPLAY_OTHER;
    k->realize = rad1_realize;
    k->exit = rad1_exit;

    rc->phases.hold = rad1_reset_hold;

    dc->desc = "PsiTech RAD1 (flat-copy functional approximation)";
    dc->vmsd = &vmstate_psitech_rad1;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo psitech_rad1_type_info = {
    .name          = TYPE_PSITECH_RAD1,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PsitechRad1State),
    .class_init    = rad1_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void psitech_rad1_register_types(void)
{
    type_register_static(&psitech_rad1_type_info);
}

type_init(psitech_rad1_register_types)
