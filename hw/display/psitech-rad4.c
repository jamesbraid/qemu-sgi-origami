/* SPDX-License-Identifier: BSD-3-Clause */
/* PsiTech RAD4 (12d1:0104) and RAD4-KM primary PCI heads on an Origin 200
 * Bridge slot. */
/*
 * This models a compatibility profile for one exact stock software path
 * (PsiTech release 1.3k kernel driver and
 * standalone X server, the driver's mode zero at 1280x1024), not recovered
 * RAD4 hardware fidelity. The
 * expansion-ROM-route DMA registers (RAD4_ROM_MODE/RAD4_ROM_DESC/
 * RAD4_ROM_COMMAND) model the PLX
 * PCI9060/9054-family scatter/gather engine the driver's buffered/type-3 path
 * drives; every other pixel path still
 * moves data through ordinary CPU-issued BAR2 stores.
 *
 * The one-mode fixed timing sequence gating scanout comes from a generated
 * fixture (psitech-rad4-mode0-fixture.h),
 * not from vendor source or a copied instruction listing.
 *
 * This file also answers the PCI identity 12d1:0105 (RAD4-KM), selected by
 * the device-id property. Two facts are
 * established for that identity: the PCI vendor/device pair itself, and the
 * guest-visible outcome that IRIX's
 * rad4pci driver, the same driver package as plain RAD4's, attaches to it
 * as a RAD4-KM board. Everything else
 * this file implements under that identity (the BAR0/BAR2/expansion-ROM
 * layout, the RGB561 path, the DMA engine) is
 * plain RAD4's own approximation, reused here rather than an established
 * RAD4-KM register fact.
 *
 * RAD4-KM's keyboard/mouse controller is modeled separately, as the child
 * device hw/input/psitech-rad4-km.c,
 * realized onto PCI BAR 3 only under the RAD4-KM identity. That file
 * implements a bounded, disclosed approximation
 * of the keyboard and mouse register contract; its own header names,
 * separately, the chip identity and
 * interrupt-routing facts it deliberately leaves unmodeled rather than
 * guessing.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/input/psitech-rad4-km.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "qom/object.h"
#include "trace.h"

#include "psitech-rad4-mode0-fixture.h"

#define TYPE_PSITECH_RAD4 "psitech-rad4"
OBJECT_DECLARE_SIMPLE_TYPE(PsitechRad4State, PSITECH_RAD4)

#define RAD4_VENDOR_ID          0x12d1
#define RAD4_DEVICE_ID          0x0104

/*
 * RAD4-KM, PCI identity 12d1:0105: a distinct PsiTech product that shares
 * RAD4's own rad4pci driver and hwgraph
 * subtree. Only the PCI identity and the guest-visible attach outcome are
 * established for it; this device answers
 * that identity with plain RAD4's own register layout below, an explicitly
 * disclosed approximation, not an
 * independently established RAD4-KM register contract. See the file header
 * for the full disclosure.
 */
#define RAD4_KM_DEVICE_ID       0x0105

#define RAD4_BAR0_SIZE          128
#define RAD4_BAR2_SIZE          (16 * MiB)
#define RAD4_ROMROUTE_SIZE      (8 * MiB)

#define RAD4_MODE0_WIDTH        1280
#define RAD4_MODE0_HEIGHT       1024
#define RAD4_MODE0_STRIDE       (RAD4_MODE0_WIDTH * 4)
#define RAD4_FB_SIZE            (8 * MiB)

/* BAR0 offsets, PLX9060-compatible. */
#define RAD4_BAR0_REG14         0x14
#define RAD4_BAR0_REG18         0x18
#define RAD4_BAR0_MAILBOX       0x40
#define RAD4_BAR0_INTCSR        0x68
#define RAD4_BAR0_SERIAL        0x6c

#define RAD4_INTCSR_PENDING     (1u << 15)
#define RAD4_INTCSR_ENABLE      (1u << 8)
#define RAD4_INTCSR_WRITABLE    0xffff7fffu

#define RAD4_SERIAL_CLOCK       (1u << 24)
#define RAD4_SERIAL_CS          (1u << 25)
#define RAD4_SERIAL_MOSI        (1u << 26)
#define RAD4_SERIAL_MISO        (1u << 27)
#define RAD4_SERIAL_WRITABLE    0x0700000fu

/*
 * Expansion-ROM route, revision-3-compatible DMA register aliases. Offsets
 * match the PLX PCI9054 Data Book (PLX
 * Technology, 1998; the PCI9060's own documented family), Tables 11-76, 11-80
 * and 11-86: DMAMODE0 at local offset
 * 0x100, DMADPR0 (the Channel 0 Descriptor Pointer Register) at 0x110,
 * DMACSR0 (Channel 0 Command/Status) at 0x128.
 * The driver writes a descriptor pointer to +0x110 and a mode value of 0x7c3
 * to +0x100 before starting the transfer
 * at +0x128. This reverses this file's earlier RAD4_ROM_DESC/RAD4_ROM_MODE
 * values, which had the two registers
 * swapped.
 */
#define RAD4_ROM_MODE           0x100
#define RAD4_ROM_DESC           0x110
#define RAD4_ROM_COMMAND        0x128

/*
 * DMAMODE0 bits this model acts on (PLX PCI9054 Data Book, Table 11-76).
 * Every other bit (bus width, wait states,
 * burst/TA# enables, and so on) only matters to real bus timing this model
 * does not simulate.
 */
#define RAD4_DMA_MODE_SCATTER_GATHER   (1u << 9)
#define RAD4_DMA_MODE_DONE_IRQ_ENABLE  (1u << 10)

/*
 * DMACSR0 bits (PLX PCI9054 Data Book, Table 11-86). The driver writes
 * ENABLE|START (0x3) to begin a transfer and
 * ABORT (0x4) once its `ddi_sv_wait` timeout expires; it polls DONE (0x10)
 * for completion.
 */
#define RAD4_DMA_CMD_ENABLE     (1u << 0)
#define RAD4_DMA_CMD_START      (1u << 1)
#define RAD4_DMA_CMD_ABORT      (1u << 2)
#define RAD4_DMA_STATUS_DONE    (1u << 4)

/*
 * DMADPR0 bits (PLX PCI9054 Data Book, Table 11-80): the low 4 bits of both
 * the Descriptor Pointer Register and
 * each in-memory descriptor's own "next descriptor" word.
 * RAD4_DMA_DESC_ADDR_MASK isolates the quad-word-aligned
 * address the rest of the word carries.
 */
/* 1: descriptor in host memory */
#define RAD4_DMA_DESC_LOC_PCI       (1u << 0)
#define RAD4_DMA_DESC_END_OF_CHAIN  (1u << 1)
#define RAD4_DMA_DESC_INTR_ON_TC    (1u << 2)
#define RAD4_DMA_DESC_LOCAL_TO_PCI  (1u << 3) /* 1: framebuffer -> host */
#define RAD4_DMA_DESC_ADDR_MASK     0xfffffff0u

/* DMASIZ0 is a 23-bit byte count (Table 11-79); bound a chain walk too. */
#define RAD4_DMA_SIZE_MASK      0x7fffffu
#define RAD4_DMA_MAX_DESCRIPTORS 256

/* BAR2 apertures. */
#define RAD4_BAR2_FB_BASE       0x000000
#define RAD4_BAR2_RGB_LO        0x900000
#define RAD4_BAR2_RGB_HI        0x910000
#define RAD4_BAR2_RGB_DATA      0x920000
#define RAD4_BAR2_RGB_RESERVED  0x930000
#define RAD4_BAR2_HORIZ0        0xa00000
#define RAD4_BAR2_HORIZ1        0xa01000
#define RAD4_BAR2_HORIZ3        0xa03000
#define RAD4_BAR2_VERT          0xb00000
#define RAD4_BAR2_MASK          0xd00000
#define RAD4_BAR2_CTRL          0xf00000

#define RAD4_HORIZ_WORDS        422
#define RAD4_VERT_WORDS         1065

#define RAD4_CTRL_MODE_CMD      0x20u
#define RAD4_CTRL_STORED_BIT    0x02u
#define RAD4_STATUS_FRAME       0x20u

#define RAD4_FRAME_HZ           60

/*
 * RGB561 indexed cursor registers. IBM Microelectronics, "RGB561
 * Architecture", Preliminary Rev. 1.1, 23 January
 * 1995, page 27's register address table and pages 37-39. Index 0x0030's
 * value 0x60 disables the cursor and
 * cross-hair at initialization; the driver's own init_RGB561 routine writes
 * that value and then nine more RGB561
 * data-port writes that are all zero, confirming every other cursor register
 * here also resets to zero on that path.
 */
#define RAD4_RGB_CURSOR_CONTROL     0x0030
#define RAD4_RGB_CURSOR_HOTSPOT_X   0x0034
#define RAD4_RGB_CURSOR_HOTSPOT_Y   0x0035
#define RAD4_RGB_CURSOR_X_LOW       0x0036
#define RAD4_RGB_CURSOR_X_HIGH      0x0037
#define RAD4_RGB_CURSOR_Y_LOW       0x0038
#define RAD4_RGB_CURSOR_Y_HIGH      0x0039
#define RAD4_CURSOR_CONTROL_CE      (1u << 0)   /* Cursor Enabling, page 37 */
#define RAD4_CURSOR_HOTSPOT_MASK    0x3f        /* 6 bits, page 38 */

/*
 * Cursor/cross-hair look-up table, indices 0x0a10-0x0a1f (IBM page 27's
 * "CURSOR LUT (3 RW/ADDR)" and "CROSS-HAIR
 * CURSOR LUT (3 RW/ADDR)", page 45's Table 22). Two things make this range
 * unlike every other indexed range this
 * file already implements:
 *
 * - Reads and writes go through the fourth RGB561 port, BAR2 +0x930000
 * (RAD4_BAR2_RGB_RESERVED below), not the
 *   ordinary data port at +0x920000. The shipped driver's init_RGB561 routine
 * sets the index through the same two
 *   struct fields it uses for every other index-then-data sequence, but the
 * three RGB data writes that follow
 *   dereference a different struct field from the one used for every ordinary
 * single-cycle register nearby,
 *   including the cursor control block set a few writes earlier. The struct
 * holds four consecutive pointer-sized
 *   fields, matching the chip's four apertures in order (index-low,
 * index-high, data, reserved); the cursor data
 *   writes use the fourth.
 * - Real hardware does not advance the visible index after each byte here:
 * the driver points the index at 0x0a11
 *   once, issues six raw writes to that fourth port with no index write
 * between them, then explicitly re-points the
 *   index to 0x0a15 rather than continuing to rely on it having advanced
 * there. That is only consistent with the
 *   index auto-incrementing once per three bytes (one RGB triple per
 * addressed entry), not once per byte.
 *
 * The six bytes observed are (0, 0, 0, 0xff, 0xff, 0xff): primary color 1
 * (0x0a11) is black, primary color 2
 * (0x0a12) is white, matching an ordinary white-on-black pointer; primary
 * color 3 (0x0a13) is left unwritten
 * (zero). Component order within each triple (red, green, blue) matches this
 * same driver's separate palette upload
 * helper, which extracts red/green/blue from software word bits
 * 16-23/8-15/0-7 in that order; it is not
 * independently confirmed for this specific register block.
 */
#define RAD4_RGB_CURSOR_LUT_BASE    0x0a10
#define RAD4_RGB_CURSOR_LUT_COUNT   16
#define RAD4_RGB_CURSOR_LUT_LAST    (RAD4_RGB_CURSOR_LUT_BASE + \
                                     RAD4_RGB_CURSOR_LUT_COUNT - 1)

/*
 * Cursor pixel map, index 0x2000-0x23ff: a plain 1Kx8 SRAM (IBM page 27), two
 * bits per pixel packed four to a byte,
 * least-significant pair first, sixteen bytes per 64-pixel row (IBM page 3,
 * "Cursor Pixel Map", and Tables 3-4).
 * Each two-bit value selects a transparent pixel (0) or one of the three
 * primary-color LUT entries above (1-3);
 * this model does not implement cursor or cross-hair blinking, so the blink
 * LUT is stored (through the ordinary
 * indexed path) but never consulted.
 */
#define RAD4_RGB_CURSOR_PIXMAP_BASE 0x2000
#define RAD4_CURSOR_WIDTH           64
#define RAD4_CURSOR_HEIGHT          64
#define RAD4_CURSOR_PIXMAP_STRIDE   (RAD4_CURSOR_WIDTH / 4)
#define RAD4_CURSOR_PIXMAP_SIZE     (RAD4_CURSOR_PIXMAP_STRIDE * \
                                     RAD4_CURSOR_HEIGHT)

/*
 * RGB561 index ranges this profile stores and treats as supported. The
 * cursor/cross-hair look-up table
 * (0x0a10-0x0a1f) is handled entirely separately, before this table is
 * consulted, because it needs the
 * three-writes-per-address behavior described above rather than plain
 * per-byte storage.
 */

/*
 * RGB561 multi-cycle registers reached through RAD4_BAR2_RGB_RESERVED (BAR2
 * +0x930000). IBM Microelectronics,
 * "RGB561 Architecture", Preliminary Rev. 1.1, 23 January 1995, page 1's
 * "Programming" section and Table 20's
 * register address table (page 27) establish that every "N RW/ADDR" index
 * range in that table, both window
 * attribute tables, their auxiliary tables, the gamma tables, the color
 * look-up table and the cursor/cross-hair
 * look-up table, goes through this one physical port, driven by the same
 * RAD4_BAR2_RGB_LO/RAD4_BAR2_RGB_HI index
 * registers already used to select an entry for the ordinary single-cycle
 * RAD4_BAR2_RGB_DATA port. This model
 * implements the two ranges the qualified guest workload actually exercises:
 * the frame-buffer window attribute
 * table (FB_WAT) and the runtime color look-up table it points into. Any
 * other multi-cycle index, the overlay
 * WAT, the gamma tables, the cursor/cross-hair look-up table, is
 * unimplemented and trace-visible rather than
 * silently accepted.
 */
#define RAD4_WAT_BASE           0x1000
#define RAD4_WAT_COUNT          256
#define RAD4_WAT_LAST           (RAD4_WAT_BASE + RAD4_WAT_COUNT - 1)
#define RAD4_WAT_CYCLES         2

#define RAD4_COLOR_LUT_BASE     0x4000
#define RAD4_COLOR_LUT_COUNT    1024
#define RAD4_COLOR_LUT_LAST     (RAD4_COLOR_LUT_BASE + RAD4_COLOR_LUT_COUNT - 1)
#define RAD4_COLOR_LUT_CYCLES   3

/*
 * FB_WAT entry bit layout, same document page 47's "X'1000-10FF FB_WAT"
 * table: a 10-bit value with START_ADDR in
 * bits 9-6, PIX_FORM in bits 5-4, BS in bit 3, MODE in bits 2-1 and TR in bit
 * 0. Table 1 on page 2 adds START_ADDR
 * to the pixel's index data to form the color LUT address; this model only
 * evidences PIX_FORM 8bpp (00) in Index
 * mode, so RAD4_WAT_PIXFORM_8BPP gates that formula and anything else is
 * traced rather than guessed at.
 */
#define RAD4_WAT_START_ADDR_SHIFT 6
#define RAD4_WAT_START_ADDR_MASK  0xf
#define RAD4_WAT_PIXFORM_SHIFT    4
#define RAD4_WAT_PIXFORM_MASK     0x3
#define RAD4_WAT_PIXFORM_8BPP     0x0
#define RAD4_WAT_MODE_SHIFT       1
#define RAD4_WAT_MODE_MASK        0x3
#define RAD4_WAT_MODE_INDEX       0x0
#define RAD4_WAT_MODE_TRUE        0x3

/*
 * Two cycle transfer of a 10-bit WAT entry, same document page 4's Table 5:
 * the first cycle carries bits 9-2 in bit
 * positions 7-0 of the byte, the second carries bits 1-0 in bit positions
 * 7-6.
 */
#define RAD4_WAT_CYCLE0_SHIFT   2
#define RAD4_WAT_CYCLE1_SHIFT   6
#define RAD4_WAT_CYCLE1_MASK    0x3

/*
 * The WID byte's position within RAD4's 32-bit software pixel word. The
 * RGB561 datasheet defines the WID *input
 * pins* and how many bits CONF/4 allocates to them, but not which VRAM byte a
 * given board wires to those pins. That is board-specific. This model's
 * best-supported choice is the top byte:
 * every login-screen pixel already has
 * this byte at zero, matching the "0x00RRGGBB" True-color layout established
 * from the driver's channel order and
 * the shipped X server's compiled visual masks, consistent with those pixels
 * all belonging to window ID 0. With
 * this shift, the whole qualified guest workload (IRIX 6.5.30, RAD4 at PCI
 * slot 5) renders a correctly colored xdm
 * login screen end to end, light gray/white window chrome, a pink login
 * field and a real yellow/ red/black
 * EZsetup icon, not the near-black symptom this change fixes. This confirms
 * the shift for window ID 0, the only
 * value the qualified workload's login screen exercises; it has not been
 * independently confirmed against a pixel
 * with a nonzero window ID.
 */
#define RAD4_PIXEL_WID_SHIFT    24

/* RGB561 index ranges this profile stores and treats as supported. */
typedef struct Rad4RgbRange {
    uint16_t first;
    uint16_t last;
} Rad4RgbRange;

static const Rad4RgbRange rad4_rgb_supported_ranges[] = {
    { 0x0001, 0x0009 },
    { 0x0010, 0x0013 },
    { 0x0020, 0x0022 },
    { 0x0030, 0x0039 },
    { 0x0040, 0x004e },
    { 0x0050, 0x0056 },
    { 0x005f, 0x005f },
    { 0x0082, 0x0082 },
    { 0x0e00, 0x0e0f },
    { 0x0f00, 0x0f0f },
    { 0x1000, 0x10ff },
    { 0x1400, 0x14ff },
    { 0x2000, 0x23ff },
    { 0x3000, 0x30ff },
    { 0x3400, 0x34ff },
    { 0x3800, 0x38ff },
    { 0x4000, 0x43ff },
};

/*
 * Indices that gate mode recognition: writing data here after recognition
 * invalidates the active mode instead of
 * taking effect as an ordinary runtime register update.
 */
static bool rad4_rgb_index_is_mode_mutating(uint16_t index)
{
    return index == 0x0020 || index == 0x0021 || index == 0x0022;
}

static bool rad4_rgb_index_supported(uint16_t index)
{
    for (size_t i = 0; i < ARRAY_SIZE(rad4_rgb_supported_ranges); i++) {
        if (index >= rad4_rgb_supported_ranges[i].first &&
            index <= rad4_rgb_supported_ranges[i].last) {
            return true;
        }
    }
    return false;
}

typedef enum Rad4SerialState {
    RAD4_SERIAL_IDLE,
    RAD4_SERIAL_COMMAND,
    RAD4_SERIAL_ACK,
    RAD4_SERIAL_PAYLOAD,
    RAD4_SERIAL_FAILED,
} Rad4SerialState;

/* Start bit (1) followed by command byte 0xac, MSB first, 9 bits total. */
#define RAD4_SERIAL_COMMAND_BITS 9
#define RAD4_SERIAL_PAYLOAD_BITS (40 * 8)
static const uint16_t rad4_serial_expected_command = 0x1ac;

typedef enum Rad4RecState {
    RAD4_REC_UNARMED,
    RAD4_REC_PREFIX_SEEN,
    RAD4_REC_COLLECTING,
    RAD4_REC_RECOGNIZED,
    RAD4_REC_FAILED,
} Rad4RecState;

struct PsitechRad4State {
    PCIDevice pci;

    /*
     * Selects the PCI identity this instance answers as: RAD4_DEVICE_ID
     * (0x0104, plain RAD4) or RAD4_KM_DEVICE_ID
     * (0x0105, RAD4-KM), a settable property mirroring how
     * hw/misc/sgi-ioc3.c's subsystem-id property selects a
     * board identity on one shared implementation. Every register below
     * behaves identically regardless of this
     * value; see the file header for what that reuse does and does not claim
     * for RAD4-KM.
     */
    uint16_t device_id;

    MemoryRegion bar0;
    MemoryRegion bar2;
    MemoryRegion romroute;

    /*
     * The keyboard/mouse controller child, realized onto BAR 3 only under the
     * RAD4-KM identity (device_id ==
     * RAD4_KM_DEVICE_ID); unparented otherwise. See
     * hw/input/psitech-rad4-km.c.
     */
    PsitechRad4KMState km;

    /*
     * The km child's own aggregate IRQ output (rad4_km_irq_handler()), cached
     * so rad4_update_irq() can OR it into
     * the shared PCI line and so a post-load recompute doesn't depend on
     * child/parent VMState load order. Always
     * false under the plain RAD4 identity, since the child is unparented and
     * never realized there.
     */
    bool km_irq_level;

    QemuConsole *con;
    QEMUTimer *frame_timer;

    /* BAR0 state. */
    uint32_t reg14;
    uint32_t reg18;
    uint32_t mailbox;
    uint32_t intcsr;
    uint32_t serial;

    int serial_state; /* Rad4SerialState, as plain int for VMState */
    bool serial_clk_prev;
    bool serial_cs_prev;
    uint32_t serial_bit_count;
    uint16_t serial_command_shift;
    bool serial_miso;

    /* Expansion-ROM route state. */
    uint32_t rom_desc;
    uint32_t rom_mode;
    uint32_t rom_command;

    /* BAR2 framebuffer and mask. */
    uint8_t *framebuffer;
    uint32_t mask;

    /*
     * Scanlines of the framebuffer written since the last rad4_gfx_update(),
     * inclusive and empty when dirty_y_start
     * > dirty_y_end. framebuffer is a plain buffer behind a masked-write MMIO
     * trap, not a RAM MemoryRegion, so it
     * cannot use the region dirty-bitmap helpers in hw/display/framebuffer.c;
     * rad4_bar2_write() and
     * rad4_dma_run()'s upload branch mark rows by hand as they write, and
     * rad4_gfx_update() converts and pushes
     * only this range instead of the whole frame every refresh. Not migrated:
     * rad4_post_load() marks the whole
     * frame dirty instead, since display_buffer below is not itself part of
     * VMState.
     */
    int dirty_y_start;
    int dirty_y_end;

    /*
     * Displayed color, converted from the raw framebuffer through each
     * pixel's window ID and FB_WAT entry
     * (rad4_convert_scanout()). A plain, host-owned buffer, not
     * qemu_create_displaysurface()'s default shareable
     * pixman allocation, since nothing here needs zero-copy sharing with an
     * external consumer.
     */
    uint32_t *display_buffer;

    /*
     * Rows of display_buffer the hardware cursor sprite is composited over,
     * inclusive and empty when
     * cursor_shown_y_start > cursor_shown_y_end. Recomputed from the RGB561
     * cursor registers at each refresh, so
     * not migrated: rad4_post_load() repaints the whole frame and that
     * refresh sets it again.
     */
    int cursor_shown_y_start;
    int cursor_shown_y_end;

    /* RGB561 indexed register file. */
    uint8_t rgb_index_low;
    uint8_t rgb_index_high;
    uint8_t *rgb_store;

    /*
     * RGB561 multi-cycle port state (BAR2 +0x930000): the frame-buffer window
     * attribute table, the color look-up
     * table it addresses, and the cursor/cross-hair look-up table. One phase
     * counter serves the whole port,
     * matching the datasheet's description of a single
     * address-index-register-driven state machine (IBM RGB561
     * Architecture p.1, "Address Index Register"), not a separate counter per
     * table. The cursor LUT's three-bytes-per-entry cycle and the WAT's
     * two-bytes-per-entry cycle
     * share this same counter, distinguished by
     * which index range is currently selected.
     */
    uint16_t fb_wat[RAD4_WAT_COUNT];
    uint8_t color_lut[RAD4_COLOR_LUT_COUNT][3];
    uint8_t cursor_lut[RAD4_RGB_CURSOR_LUT_COUNT][3];
    uint8_t rgb_reserved_phase;

    /*
     * Timing memories, stored for readback only; recognition is gated by
     * exact sequence matching against the
     * generated fixture, not by interpreting these values.
     */
    uint32_t vert_timing[RAD4_VERT_WORDS];
    uint32_t horiz_timing[3][RAD4_HORIZ_WORDS];

    /* BAR2 +0xf00000 control/status. */
    uint32_t bar2_ctrl_bit;
    bool frame_pending;

    /* Mode recognizer. */
    int rec_state; /* Rad4RecState, as plain int for VMState */
    uint32_t rec_index;
};

static uint16_t rad4_rgb_index(PsitechRad4State *s)
{
    return ((uint16_t)s->rgb_index_high << 8) | s->rgb_index_low;
}

static bool rad4_index_in_wat(uint16_t index)
{
    return index >= RAD4_WAT_BASE && index <= RAD4_WAT_LAST;
}

static bool rad4_index_in_color_lut(uint16_t index)
{
    return index >= RAD4_COLOR_LUT_BASE && index <= RAD4_COLOR_LUT_LAST;
}

/*
 * Advance the RGB561 multi-cycle port's shared phase counter, completing the
 * auto-increment of the address index
 * register once an entry's last cycle has been transferred. This generalizes
 * the auto-increment RAD4_BAR2_RGB_DATA
 * already applies per cycle to instead apply once per whole multi-cycle
 * entry, per IBM RGB561 Architecture p.1,
 * "Programming": "The address index register will auto increment to the next
 * address after each read or write cycle
 * to a configuration register or after the end of a multi-cycle LUT or WAT
 * access."
 */
static void rad4_rgb_reserved_advance(PsitechRad4State *s, uint16_t index,
                                      unsigned cycles)
{
    if (++s->rgb_reserved_phase < cycles) {
        return;
    }
    s->rgb_reserved_phase = 0;
    index++;
    s->rgb_index_low = index & 0xff;
    s->rgb_index_high = (index >> 8) & 0xff;
}

/*
 * Interpret one raw 32-bit framebuffer word for display, per its window ID's
 * FB_WAT entry (IBM RGB561 Architecture
 * p.47). True color mode bypasses the color LUT and presents the pixel data
 * directly (same document p.20, Figure
 * 5): this model's raw word is already 0x00RRGGBB in that mode, so pass it
 * through unmodified. Index mode looks the
 * low byte up in the color LUT through the start-address formula in Table 1
 * (p.2): LUT[START_ADDR * 64 + index].
 * Grey Scale and Direct RGB modes, and Index mode with a PIX_FORM other than
 * 8bpp, are not exercised by the
 * qualified guest workload and are traced rather than guessed at.
 */
static uint32_t rad4_pixel_to_rgb(PsitechRad4State *s, uint32_t word)
{
    uint8_t wid = (word >> RAD4_PIXEL_WID_SHIFT) & 0xff;
    uint16_t wat = s->fb_wat[wid];
    unsigned mode = (wat >> RAD4_WAT_MODE_SHIFT) & RAD4_WAT_MODE_MASK;

    if (mode == RAD4_WAT_MODE_INDEX) {
        unsigned pix_form = (wat >> RAD4_WAT_PIXFORM_SHIFT) &
                            RAD4_WAT_PIXFORM_MASK;
        unsigned start_addr = (wat >> RAD4_WAT_START_ADDR_SHIFT) &
                              RAD4_WAT_START_ADDR_MASK;
        uint8_t index = word & 0xff;
        uint16_t lut_addr = (start_addr * 64 + index) &
                            (RAD4_COLOR_LUT_COUNT - 1);
        const uint8_t *rgb = s->color_lut[lut_addr];

        if (pix_form != RAD4_WAT_PIXFORM_8BPP) {
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad4: FB_WAT PIX_FORM %u for WID 0x%02x "
                         "in Index mode is unimplemented; treating pixel "
                         "data as an 8-bit index regardless\n",
                         pix_form, wid);
        }
        return ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2];
    }

    if (mode != RAD4_WAT_MODE_TRUE) {
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: FB_WAT color mode %u for WID 0x%02x is "
                     "unimplemented; treating as direct pass-through\n",
                     mode, wid);
    }
    return word & 0x00ffffffu;
}

static void rad4_update_irq(PsitechRad4State *s)
{
    bool level = (s->intcsr & RAD4_INTCSR_PENDING) &&
                 (s->intcsr & RAD4_INTCSR_ENABLE);

    pci_set_irq(&s->pci, level || s->km_irq_level);
}

/*
 * The km child is a single-function PCI device's own sub-block, not a second
 * INTx# source: its aggregate ISR/IMR
 * condition shares this device's one PCI interrupt line alongside INTCSR's
 * own frame interrupt (see
 * include/hw/input/psitech-rad4-km.h for what that aggregate does and does
 * not claim about the board-level gate at
 * driver offset 0x78).
 */
static void rad4_km_irq_handler(void *opaque, int n, int level)
{
    PsitechRad4State *s = opaque;

    s->km_irq_level = level;
    rad4_update_irq(s);
}

static void rad4_recognizer_reset(PsitechRad4State *s)
{
    s->rec_state = RAD4_REC_UNARMED;
    s->rec_index = 0;
}

/* Mark [y_start, y_end] (inclusive, clipped to the visible image) dirty. */
static void rad4_mark_dirty_rows(PsitechRad4State *s, int y_start, int y_end)
{
    if (y_start < 0) {
        y_start = 0;
    }
    if (y_end >= RAD4_MODE0_HEIGHT) {
        y_end = RAD4_MODE0_HEIGHT - 1;
    }
    if (y_start > y_end) {
        return;
    }
    if (s->dirty_y_start > s->dirty_y_end) {
        s->dirty_y_start = y_start;
        s->dirty_y_end = y_end;
        return;
    }
    s->dirty_y_start = MIN(s->dirty_y_start, y_start);
    s->dirty_y_end = MAX(s->dirty_y_end, y_end);
}

/*
 * Mark every scanline dirty: the first paint after mode recognition,
 * post-migration repaint and reset all need a
 * full frame, not an incremental one.
 */
static void rad4_mark_all_dirty(PsitechRad4State *s)
{
    rad4_mark_dirty_rows(s, 0, RAD4_MODE0_HEIGHT - 1);
}

/*
 * Mark the scanlines touched by a raw-framebuffer byte range dirty. offset
 * and len are relative to s->framebuffer;
 * a range outside the visible mode-zero image (the RAD4_FB_SIZE allocation is
 * larger than RAD4_MODE0_STRIDE *
 * RAD4_MODE0_HEIGHT) contributes nothing, matching how rad4_convert_scanout()
 * never reads past the visible image
 * either.
 */
static void rad4_mark_dirty_range(PsitechRad4State *s, uint32_t offset,
                                  uint32_t len)
{
    uint32_t visible = (uint32_t)RAD4_MODE0_STRIDE * RAD4_MODE0_HEIGHT;

    if (len == 0 || offset >= visible) {
        return;
    }
    rad4_mark_dirty_rows(s, offset / RAD4_MODE0_STRIDE,
                         (offset + len - 1) / RAD4_MODE0_STRIDE);
}

static void rad4_disable_scanout(PsitechRad4State *s)
{
    if (s->rec_state == RAD4_REC_RECOGNIZED) {
        timer_del(s->frame_timer);
    }
}

static void rad4_frame_timer_cb(void *opaque)
{
    PsitechRad4State *s = opaque;

    if (s->rec_state != RAD4_REC_RECOGNIZED) {
        return;
    }

    s->frame_pending = true;
    s->intcsr |= RAD4_INTCSR_PENDING;
    rad4_update_irq(s);

    timer_mod(s->frame_timer,
             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
             NANOSECONDS_PER_SECOND / RAD4_FRAME_HZ);
}

/*
 * Feed one recognition-relevant (address, value) event through the exact
 * mode-zero grammar. Addresses are
 * BAR2-relative. Returns true if the event was consumed as part of that
 * grammar (whether or not it advanced
 * recognition); the caller still applies the event's own register effect
 * separately, since recognition state and
 * register storage are distinct.
 */
static void rad4_recognizer_feed(PsitechRad4State *s, uint32_t offset,
                                 uint32_t value)
{
    if (offset == RAD4_BAR2_CTRL) {
        if (value == 0) {
            /*
             * Arms/re-arms the sequence from any state, including an
             * already-recognized mode: a fresh mode-set
             * invalidates the active mode until the new sequence completes.
             */
            rad4_disable_scanout(s);
            s->rec_state = RAD4_REC_PREFIX_SEEN;
            s->rec_index = 0;
            return;
        }
        if (s->rec_state == RAD4_REC_COLLECTING &&
            s->rec_index < RAD4_MODE0_EVENT_COUNT &&
            rad4_mode0_events[s->rec_index].bar2_offset == offset &&
            rad4_mode0_events[s->rec_index].value == value) {
            s->rec_index++;
            if (s->rec_index == RAD4_MODE0_EVENT_COUNT) {
                s->rec_state = RAD4_REC_RECOGNIZED;
                rad4_mark_all_dirty(s);
                timer_mod(s->frame_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         NANOSECONDS_PER_SECOND / RAD4_FRAME_HZ);
            }
            return;
        }
        if (s->rec_state == RAD4_REC_COLLECTING) {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad4: mode recognition mismatch at "
                         "+0x%06x=0x%08x, failing until reset\n",
                         offset, value);
            rad4_disable_scanout(s);
            s->rec_state = RAD4_REC_FAILED;
            return;
        }
        /*
         * An unrecognized control write in another state has no bearing on
         * recognition; the register-level effect
         * is handled by the caller.
         */
        return;
    }

    /* Every other recognition-relevant address. */
    switch (s->rec_state) {
    case RAD4_REC_PREFIX_SEEN:
        if (s->rec_index == 0 &&
            rad4_mode0_events[1].bar2_offset == offset &&
            rad4_mode0_events[1].value == value) {
            s->rec_state = RAD4_REC_COLLECTING;
            s->rec_index = 2;
            if (s->rec_index == RAD4_MODE0_EVENT_COUNT) {
                s->rec_state = RAD4_REC_RECOGNIZED;
                rad4_mark_all_dirty(s);
            }
        } else {
            /* Not the expected next event: quietly returns to unarmed, no
             * failure logged. */
            s->rec_state = RAD4_REC_UNARMED;
        }
        break;
    case RAD4_REC_COLLECTING:
        if (s->rec_index < RAD4_MODE0_EVENT_COUNT &&
            rad4_mode0_events[s->rec_index].bar2_offset == offset &&
            rad4_mode0_events[s->rec_index].value == value) {
            s->rec_index++;
            if (s->rec_index == RAD4_MODE0_EVENT_COUNT) {
                s->rec_state = RAD4_REC_RECOGNIZED;
                rad4_mark_all_dirty(s);
                timer_mod(s->frame_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         NANOSECONDS_PER_SECOND / RAD4_FRAME_HZ);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad4: mode recognition mismatch at "
                         "+0x%06x=0x%08x, failing until reset\n",
                         offset, value);
            rad4_disable_scanout(s);
            s->rec_state = RAD4_REC_FAILED;
        }
        break;
    case RAD4_REC_UNARMED:
    case RAD4_REC_RECOGNIZED: {
        bool timing_bank = offset == RAD4_BAR2_VERT ||
                            offset == RAD4_BAR2_HORIZ0 ||
                            offset == RAD4_BAR2_HORIZ1 ||
                            offset == RAD4_BAR2_HORIZ3;
        bool mutating_rgb = offset == RAD4_BAR2_RGB_DATA &&
                             rad4_rgb_index_is_mode_mutating(
                                 rad4_rgb_index(s));

        if (timing_bank || mutating_rgb) {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad4: timing/PLL/sync write while %s, "
                         "failing until reset\n",
                         s->rec_state == RAD4_REC_UNARMED ?
                         "unarmed" : "recognized");
            rad4_disable_scanout(s);
            s->rec_state = RAD4_REC_FAILED;
        }
        break;
    }
    case RAD4_REC_FAILED:
        break;
    }
}

/*
 * The cursor location as the guest wrote it. IBM page 38's location registers
 * are a 16-bit two's complement pair
 * with a documented valid range of -256 to +2047; bits 14-11 of the high byte
 * are specified to read back the sign
 * value regardless of what was written there. This model stores and reports
 * the full 16 bits as an ordinary signed
 * value without independently forcing that readback behavior. The qualified
 * guest workload here never reads this
 * register back, so the difference is not guest-visible in that workload, but
 * a guest that did read it back could
 * observe a difference from real hardware. That is this approximation's one
 * disclosed limitation.
 */
static void rad4_cursor_location(PsitechRad4State *s, int *x, int *y)
{
    *x = (int16_t)(((uint16_t)s->rgb_store[RAD4_RGB_CURSOR_X_HIGH] << 8) |
                   s->rgb_store[RAD4_RGB_CURSOR_X_LOW]);
    *y = (int16_t)(((uint16_t)s->rgb_store[RAD4_RGB_CURSOR_Y_HIGH] << 8) |
                   s->rgb_store[RAD4_RGB_CURSOR_Y_LOW]);
}

/*
 * Where the 64x64 pixel map's top-left corner lands on screen: the location
 * registers position the hot spot (page
 * 38), so the map starts hot_x cells left and hot_y cells above it. Returns
 * whether the cursor is enabled at all
 * (page 37's CE bit).
 */
static bool rad4_cursor_origin(PsitechRad4State *s, int *x0, int *y0)
{
    int x, y;

    rad4_cursor_location(s, &x, &y);
    *x0 = x - (s->rgb_store[RAD4_RGB_CURSOR_HOTSPOT_X] &
               RAD4_CURSOR_HOTSPOT_MASK);
    *y0 = y - (s->rgb_store[RAD4_RGB_CURSOR_HOTSPOT_Y] &
               RAD4_CURSOR_HOTSPOT_MASK);
    return (s->rgb_store[RAD4_RGB_CURSOR_CONTROL] &
            RAD4_CURSOR_CONTROL_CE) != 0;
}

/*
 * Something about the cursor changed: position, hot spot, enable, pixel map
 * or look-up table. The rows the sprite
 * was last painted over need their framebuffer content back and the rows it
 * now covers need the sprite; both ride
 * the same dirty tracking as guest framebuffer writes, so the next refresh
 * repaints exactly that span.
 */
static void rad4_cursor_invalidate(PsitechRad4State *s)
{
    int x0, y0;

    rad4_mark_dirty_rows(s, s->cursor_shown_y_start, s->cursor_shown_y_end);
    if (rad4_cursor_origin(s, &x0, &y0)) {
        rad4_mark_dirty_rows(s, y0, y0 + RAD4_CURSOR_HEIGHT - 1);
    }
}

/*
 * Overlay the cursor sprite onto the freshly converted rows [y_start, y_end]
 * of display_buffer. The RGB561 overlays
 * its cursor onto the pixel stream it sends to the monitor, so the cursor is
 * part of what the display shows and
 * belongs in the scanout, not in a UI-layer cursor handed to the console:
 * QEMU's VNC server has no protocol message
 * to place one (ui/vnc.c's vnc_mouse_set() is a stub), so a VNC client draws
 * that kind at its own pointer rather
 * than where the guest put it, and a screendump never shows it at all.
 *
 * The IBM RGB561 Architecture spec (Preliminary Rev 1.1, page 3, "Cursor
 * Pixel Map" / Table 4) packs the first
 * (leftmost) of each four-pixel group into bits[7:6] and the last (rightmost)
 * into bits[1:0], with the most-significant pair first, not least-significant
 * pair first. Value 0 is transparent; the
 * other three select cursor look-up table
 * entries whose three bytes are (red, green, blue), matching the
 * framebuffer's own palette upload helper in
 * rad4_pixel_to_rgb() above. Cursor and cross-hair blinking (page 37's
 * CB/XB/BT bits, and the blink-rate/
 * duty-cycle registers at 0x0032-0x0033) are not modeled: the sprite always
 * shows the primary-color entries, never
 * the blink entries or a transparent blink phase. Cross-hair display is not
 * modeled at all, matching the driver's
 * own cursor-only observed usage.
 */
static void rad4_cursor_compose(PsitechRad4State *s, int y_start, int y_end)
{
    int x0, y0, row, col;

    s->cursor_shown_y_start = RAD4_MODE0_HEIGHT;
    s->cursor_shown_y_end = -1;
    if (!rad4_cursor_origin(s, &x0, &y0)) {
        return;
    }
    for (row = 0; row < RAD4_CURSOR_HEIGHT; row++) {
        int y = y0 + row;
        uint32_t *dst;

        if (y < y_start || y > y_end || y < 0 || y >= RAD4_MODE0_HEIGHT) {
            continue;
        }
        dst = s->display_buffer + (size_t)y * RAD4_MODE0_WIDTH;
        for (col = 0; col < RAD4_CURSOR_WIDTH; col++) {
            int x = x0 + col;
            uint8_t byte = s->rgb_store[RAD4_RGB_CURSOR_PIXMAP_BASE +
                                        row * RAD4_CURSOR_PIXMAP_STRIDE +
                                        col / 4];
            unsigned val = (byte >> (6 - (col % 4) * 2)) & 0x3;

            if (val == 0 || x < 0 || x >= RAD4_MODE0_WIDTH) {
                continue;
            }
            dst[x] = ((uint32_t)s->cursor_lut[val][0] << 16) |
                     ((uint32_t)s->cursor_lut[val][1] << 8) |
                     s->cursor_lut[val][2];
        }
    }
    /*
     * Record the sprite's full displayed span from the cursor registers, not
     * from the rows this pass happened to
     * repaint. A refresh whose dirty interval misses the cursor, an
     * unrelated framebuffer write elsewhere,
     * repaints none of these rows but leaves the sprite standing in
     * display_buffer, so the span it occupies must be
     * re-recorded every refresh regardless. Narrowing it to the intersected
     * rows would drop the record on such a
     * refresh, and the next cursor change with no new rows of its own (a
     * disable, or a hot-spot or look-up-table
     * edit) would then fail to dirty the old rows in rad4_cursor_invalidate()
     * and strand a ghost.
     */
    s->cursor_shown_y_start = MAX(y0, 0);
    s->cursor_shown_y_end = MIN(y0 + RAD4_CURSOR_HEIGHT - 1,
                                RAD4_MODE0_HEIGHT - 1);
}

/*
 * Convert rows [y_start, y_end] (inclusive) of the raw guest framebuffer into
 * displayed color, one pixel at a time
 * through rad4_pixel_to_rgb(). This can no longer alias guest memory directly
 * (qemu_create_displaysurface_from()'s
 * previous role): a pixel's displayed color depends on its window ID's FB_WAT
 * entry and, in Index mode, the color
 * LUT, both of which the guest can reprogram after the pixel data underneath
 * is already written.
 */
static void rad4_convert_scanout(PsitechRad4State *s, int y_start, int y_end)
{
    int x, y;

    for (y = y_start; y <= y_end; y++) {
        uint32_t row = (uint32_t)y * RAD4_MODE0_STRIDE;
        uint32_t *dst = s->display_buffer + (size_t)y * RAD4_MODE0_WIDTH;

        for (x = 0; x < RAD4_MODE0_WIDTH; x++) {
            uint32_t word = ldl_be_p(s->framebuffer + row + x * 4);

            *dst++ = rad4_pixel_to_rgb(s, word);
        }
    }
}

/*
 * Convert and push only the scanlines the guest actually touched since the
 * last refresh, instead of the whole
 * 1280x1024 frame every ~30 Hz tick regardless of change. The old
 * unconditional full-frame path made VNC re-encode
 * and send a full frame continuously even for a static screen, which
 * saturates the VNC path into flicker, blackout
 * and laggy input; an idle screen with no writes since the last tick now does
 * no conversion work and sends nothing.
 */
static bool rad4_gfx_update(void *opaque)
{
    PsitechRad4State *s = opaque;

    if (s->rec_state != RAD4_REC_RECOGNIZED) {
        return true;
    }
    if (s->dirty_y_start > s->dirty_y_end) {
        trace_rad4_gfx_update_idle();
        return true;
    }
    trace_rad4_gfx_update_push(s->dirty_y_start, s->dirty_y_end);
    rad4_convert_scanout(s, s->dirty_y_start, s->dirty_y_end);
    rad4_cursor_compose(s, s->dirty_y_start, s->dirty_y_end);
    qemu_console_update(s->con, 0, s->dirty_y_start, RAD4_MODE0_WIDTH,
                        s->dirty_y_end - s->dirty_y_start + 1);
    s->dirty_y_start = RAD4_MODE0_HEIGHT;
    s->dirty_y_end = -1;
    return true;
}

static const GraphicHwOps rad4_gfx_ops = {
    .gfx_update = rad4_gfx_update,
};

static void rad4_ensure_surface(PsitechRad4State *s)
{
    DisplaySurface *ds = qemu_console_surface(s->con);

    /*
     * qemu_graphic_console_create() starts the console on a small placeholder
     * surface of its own; checking for any
     * non-NULL surface here (rather than specifically this device's own one)
     * would leave that placeholder in place
     * forever, since it is never NULL once set. Recognize our own surface by
     * its backing buffer, exactly as this
     * device recognized its old direct-alias surface before this conversion
     * pass existed.
     */
    if (ds && surface_data(ds) == (uint8_t *)s->display_buffer) {
        return;
    }
    ds = qemu_create_displaysurface_from(RAD4_MODE0_WIDTH, RAD4_MODE0_HEIGHT,
                                         PIXMAN_x8r8g8b8,
                                         RAD4_MODE0_WIDTH * 4,
                                         (uint8_t *)s->display_buffer);
    qemu_console_set_surface(s->con, ds);
}

static bool rad4_index_is_cursor_lut(uint16_t index)
{
    return index >= RAD4_RGB_CURSOR_LUT_BASE &&
           index <= RAD4_RGB_CURSOR_LUT_LAST;
}

static uint64_t rad4_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    PsitechRad4State *s = opaque;

    switch (addr) {
    case RAD4_BAR0_REG14:
        return s->reg14;
    case RAD4_BAR0_REG18:
        return s->reg18;
    case RAD4_BAR0_MAILBOX:
        return s->mailbox;
    case RAD4_BAR0_INTCSR:
        return s->intcsr;
    case RAD4_BAR0_SERIAL:
        return s->serial;
    default:
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: unsupported BAR0 read at 0x%02x\n",
                     (unsigned)addr);
        return 0;
    }
}

static void rad4_serial_reset(PsitechRad4State *s)
{
    s->serial_state = RAD4_SERIAL_IDLE;
    s->serial_bit_count = 0;
    s->serial_command_shift = 0;
    s->serial_miso = true;
}

/*
 * Bit-banged synthetic EEPROM: a start bit followed by command 0xac reads
 * back 40 zero bytes. Any other command, or
 * chip select deasserted before the transaction completes, latches an
 * all-ones readback until cold reset.
 */
static void rad4_serial_edge(PsitechRad4State *s, bool cs, bool clk,
                             bool mosi)
{
    bool clk_rising = clk && !s->serial_clk_prev;
    bool clk_falling = !clk && s->serial_clk_prev;

    if (!s->serial_cs_prev && cs) {
        rad4_serial_reset(s);
        s->serial_state = RAD4_SERIAL_COMMAND;
    }
    if (s->serial_cs_prev && !cs) {
        if (s->serial_state != RAD4_SERIAL_IDLE &&
            s->serial_bit_count <
                (s->serial_state == RAD4_SERIAL_PAYLOAD ?
                 RAD4_SERIAL_PAYLOAD_BITS : 0)) {
            /* Premature deassertion mid-transaction. */
            s->serial_state = RAD4_SERIAL_FAILED;
            s->serial_miso = true;
        } else if (s->serial_state == RAD4_SERIAL_COMMAND ||
                  s->serial_state == RAD4_SERIAL_ACK) {
            s->serial_state = RAD4_SERIAL_FAILED;
            s->serial_miso = true;
        } else {
            s->serial_state = RAD4_SERIAL_IDLE;
        }
    }

    if (cs && clk_rising) {
        switch (s->serial_state) {
        case RAD4_SERIAL_COMMAND:
            s->serial_command_shift =
                (uint16_t)((s->serial_command_shift << 1) | (mosi ? 1 : 0));
            s->serial_bit_count++;
            if (s->serial_bit_count == RAD4_SERIAL_COMMAND_BITS) {
                if (s->serial_command_shift == rad4_serial_expected_command) {
                    s->serial_state = RAD4_SERIAL_ACK;
                } else {
                    s->serial_state = RAD4_SERIAL_FAILED;
                    s->serial_miso = true;
                }
                s->serial_bit_count = 0;
            }
            break;
        default:
            break;
        }
    }

    if (cs && clk_falling) {
        switch (s->serial_state) {
        case RAD4_SERIAL_ACK:
            s->serial_miso = false;
            s->serial_state = RAD4_SERIAL_PAYLOAD;
            s->serial_bit_count = 0;
            break;
        case RAD4_SERIAL_PAYLOAD:
            s->serial_miso = false;
            s->serial_bit_count++;
            break;
        default:
            break;
        }
    }

    s->serial_clk_prev = clk;
    s->serial_cs_prev = cs;
}

static void rad4_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    PsitechRad4State *s = opaque;

    switch (addr) {
    case RAD4_BAR0_REG14:
        s->reg14 = val;
        break;
    case RAD4_BAR0_REG18:
        s->reg18 = val;
        break;
    case RAD4_BAR0_MAILBOX:
        s->mailbox = val;
        break;
    case RAD4_BAR0_INTCSR:
        s->intcsr = (s->intcsr & RAD4_INTCSR_PENDING) |
                    (val & RAD4_INTCSR_WRITABLE & ~RAD4_INTCSR_PENDING);
        rad4_update_irq(s);
        break;
    case RAD4_BAR0_SERIAL: {
        uint32_t written = (val & RAD4_SERIAL_WRITABLE);
        bool cs = (written & RAD4_SERIAL_CS) != 0;
        bool clk = (written & RAD4_SERIAL_CLOCK) != 0;
        bool mosi = (written & RAD4_SERIAL_MOSI) != 0;

        rad4_serial_edge(s, cs, clk, mosi);
        s->serial = written | (s->serial_miso ? RAD4_SERIAL_MISO : 0);
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: unsupported BAR0 write at 0x%02x\n",
                     (unsigned)addr);
        break;
    }
}

static const MemoryRegionOps rad4_bar0_ops = {
    .read = rad4_bar0_read,
    .write = rad4_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * Run one PLX9054-family scatter/gather DMA transfer to completion, starting
 * from the descriptor chain DMADPR0
 * (RAD4_ROM_DESC) names. Real PCI9060/9054 hardware overlaps this with the
 * rest of the system; this model runs it
 * synchronously inside the register write that starts it, matching how every
 * other multi-step register sequence in
 * this file (the RGB561 serial EEPROM shift, the mode recognizer) already
 * resolves within the write that drives it.
 *
 * Descriptor format: 16 bytes per entry (PLX PCI9054 Data Book, Table 11-80
 * and Figure 3-16): PCI address, Local address, transfer size, and
 * next-descriptor pointer, each a big-endian 32-bit
 * word matching every other guest-visible
 * multi-byte value this file already handles
 * (rad4_bar2_read()/rad4_bar2_write()'s ldl_be_p()/stl_be_p() on the
 * framebuffer). The low 4 bits of a pointer, the initial DMADPR0 value for
 * the first descriptor, or a
 * descriptor's own next-pointer word for every one after it, are flags for
 * the descriptor that pointer names:
 * bit0 selects which address space the descriptor itself lives in, bit1 marks
 * the last descriptor in the chain,
 * bit2 asks for an interrupt once that descriptor's own transfer completes,
 * and bit3 picks direction.
 *
 * A descriptor claiming Local Address space is read straight out of
 * s->framebuffer at the masked pointer: BAR2's
 * framebuffer memory is this model's only locally addressable storage, the
 * same array
 * rad4_bar2_read()/rad4_bar2_write() already serve BAR2 accesses from, so a
 * Local-space descriptor pointer is
 * simply a BAR2 offset. A live capture confirmed this against a real IRIX
 * 6.5.30 buffered-mode transfer: the
 * shipped 1.101 driver's own first descriptor names Local Address space, and
 * its 16 bytes decode into a
 * self-consistent chain only under that interpretation: local address 0,
 * size 0x1400 (one 1280-pixel, 4-byte
 * scanline), a PCI-Direct source address, and a next-pointer chaining
 * backward through a 40-entry descriptor array
 * built at the top of the 8 MiB local extent, while the same 16 bytes read
 * as an ordinary guest-physical address
 * were all zero.
 *
 * Returns true once an end-of-chain descriptor has been transferred, false on
 * any failure (a chain that never ends
 * within RAD4_DMA_MAX_DESCRIPTORS, a Local-space descriptor pointer outside
 * the framebuffer, a failed guest-memory
 * read for a PCI-space descriptor, or a descriptor whose Local address/size
 * run outside the framebuffer). The
 * caller leaves RAD4_DMA_STATUS_DONE clear on failure, so the guest's
 * existing timeout/abort recovery (already
 * exercised before this transfer was modeled at all, since the guest's
 * wait-and-retry logic runs regardless of
 * whether the model completes it) runs unchanged.
 */
static bool rad4_dma_run(PsitechRad4State *s)
{
    uint32_t ptr = s->rom_desc;
    int guard;

    for (guard = 0; guard < RAD4_DMA_MAX_DESCRIPTORS; guard++) {
        hwaddr desc_addr = ptr & RAD4_DMA_DESC_ADDR_MASK;
        bool from_pci = (ptr & RAD4_DMA_DESC_LOC_PCI) != 0;
        bool local_to_pci = (ptr & RAD4_DMA_DESC_LOCAL_TO_PCI) != 0;
        bool end_of_chain = (ptr & RAD4_DMA_DESC_END_OF_CHAIN) != 0;
        bool intr_on_tc = (ptr & RAD4_DMA_DESC_INTR_ON_TC) != 0;
        uint8_t raw[16];
        uint32_t pci_addr, local_addr, size, next;

        if (from_pci) {
            if (pci_dma_read(&s->pci, desc_addr, raw,
                             sizeof(raw)) != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                             "psitech-rad4: DMA descriptor read failed at "
                             "0x%"HWADDR_PRIx"\n", desc_addr);
                return false;
            }
        } else {
            if (desc_addr >= RAD4_FB_SIZE ||
                sizeof(raw) > RAD4_FB_SIZE - desc_addr) {
                qemu_log_mask(LOG_GUEST_ERROR,
                             "psitech-rad4: DMA descriptor at Local address "
                             "0x%"HWADDR_PRIx" falls outside the "
                             "framebuffer\n", desc_addr);
                return false;
            }
            memcpy(raw, s->framebuffer + desc_addr, sizeof(raw));
        }

        pci_addr = ldl_be_p(raw);
        local_addr = ldl_be_p(raw + 4);
        size = ldl_be_p(raw + 8) & RAD4_DMA_SIZE_MASK;
        next = ldl_be_p(raw + 12);

        if (local_addr >= RAD4_FB_SIZE || size > RAD4_FB_SIZE - local_addr) {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad4: DMA descriptor at 0x%"HWADDR_PRIx
                         " targets local address 0x%x size 0x%x outside "
                         "the framebuffer\n", desc_addr, local_addr, size);
            return false;
        }

        if (local_to_pci) {
            if (pci_dma_write(&s->pci, pci_addr, s->framebuffer + local_addr,
                              size) != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                             "psitech-rad4: DMA readback write failed at "
                             "0x%x\n", pci_addr);
                return false;
            }
        } else {
            /*
             * Upload into the framebuffer, honoring the same plane mask
             * (RAD4_BAR2_MASK) a CPU pixel write does:
             * the mask register is documented as programmed "before CPU
             * transfer/fill and restored afterward" and
             * both driver DMA render paths (bigx_update and
             * rad4_rect_dma_move/fill/copy) share the same threshold
             * logic deciding between them, so it is the memory controller's
             * masked write, not a CPU-only feature.
             */
            uint32_t off;

            if (local_addr % 4 != 0 || size % 4 != 0) {
                qemu_log_mask(LOG_GUEST_ERROR,
                             "psitech-rad4: DMA descriptor at 0x%"HWADDR_PRIx
                             " has an unaligned local address 0x%x or size "
                             "0x%x\n", desc_addr, local_addr, size);
                return false;
            }

            for (off = 0; off < size; off += 4) {
                uint8_t word[4];
                uint32_t old, incoming;

                if (pci_dma_read(&s->pci, pci_addr + off, word,
                                 sizeof(word)) != MEMTX_OK) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                 "psitech-rad4: DMA payload read failed at "
                                 "0x%x\n", pci_addr + off);
                    return false;
                }
                incoming = ldl_be_p(word);
                old = ldl_be_p(s->framebuffer + local_addr + off);
                stl_be_p(s->framebuffer + local_addr + off,
                        (old & ~s->mask) | (incoming & s->mask));
                rad4_mark_dirty_range(s, local_addr + off, 4);
            }
        }

        if (intr_on_tc && (s->rom_mode & RAD4_DMA_MODE_DONE_IRQ_ENABLE)) {
            s->intcsr |= RAD4_INTCSR_PENDING;
            rad4_update_irq(s);
        }

        if (end_of_chain) {
            return true;
        }
        ptr = next;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                 "psitech-rad4: DMA descriptor chain exceeded %d entries "
                 "without an end-of-chain descriptor\n",
                 RAD4_DMA_MAX_DESCRIPTORS);
    return false;
}

/*
 * DMACSR0 (RAD4_ROM_COMMAND) write side. The driver writes ENABLE|START (0x3)
 * to begin a transfer, then either
 * observes DONE (0x10) or, after its own `ddi_sv_wait` timeout, writes ABORT
 * (0x4).
 */
static void rad4_dma_command_write(PsitechRad4State *s, uint32_t val)
{
    bool starting = (val & RAD4_DMA_CMD_ENABLE) &&
                    (val & RAD4_DMA_CMD_START);

    s->rom_command = val;

    if (starting) {
        if (rad4_dma_run(s)) {
            s->rom_command |= RAD4_DMA_STATUS_DONE;
            if (s->rom_mode & RAD4_DMA_MODE_DONE_IRQ_ENABLE) {
                s->intcsr |= RAD4_INTCSR_PENDING;
                rad4_update_irq(s);
            }
        }
    } else if (val & RAD4_DMA_CMD_ABORT) {
        /*
         * PLX PCI9054 Data Book, Table 11-86: "Channel 0 Abort ... Sets
         * Channel 0 Done bit when abort is complete."
         * This model's transfers already ran to completion (or to a logged
         * failure) synchronously inside the write
         * above that started them, so by the time an abort arrives the only
         * thing left for it to do is unblock a
         * guest still waiting because that outcome was failure. That is the
         * recovery path already exercised
         * before DMA execution was modeled at all.
         */
        s->rom_command |= RAD4_DMA_STATUS_DONE;
    }
}

static uint64_t rad4_romroute_read(void *opaque, hwaddr addr, unsigned size)
{
    PsitechRad4State *s = opaque;

    switch (addr) {
    case RAD4_ROM_DESC:
        return s->rom_desc;
    case RAD4_ROM_MODE:
        return s->rom_mode;
    case RAD4_ROM_COMMAND:
        return s->rom_command;
    default:
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: unsupported ROM-route read at 0x%03x\n",
                     (unsigned)addr);
        return 0;
    }
}

static void rad4_romroute_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    PsitechRad4State *s = opaque;

    switch (addr) {
    case RAD4_ROM_DESC:
        s->rom_desc = val;
        break;
    case RAD4_ROM_MODE:
        s->rom_mode = val;
        break;
    case RAD4_ROM_COMMAND:
        rad4_dma_command_write(s, val);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: unsupported ROM-route write at 0x%03x\n",
                     (unsigned)addr);
        break;
    }
}

static const MemoryRegionOps rad4_romroute_ops = {
    .read = rad4_romroute_read,
    .write = rad4_romroute_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rad4_apply_mask_write(PsitechRad4State *s, uint32_t val)
{
    s->mask = val;
}

static uint64_t rad4_bar2_read(void *opaque, hwaddr addr, unsigned size)
{
    PsitechRad4State *s = opaque;
    uint32_t offset = addr;

    if (offset < RAD4_FB_SIZE) {
        return ldl_be_p(s->framebuffer + offset);
    }
    if (offset >= RAD4_BAR2_VERT &&
        offset < RAD4_BAR2_VERT + 4 * RAD4_VERT_WORDS) {
        return s->vert_timing[(offset - RAD4_BAR2_VERT) / 4];
    }
    if (offset >= RAD4_BAR2_HORIZ0 &&
        offset < RAD4_BAR2_HORIZ0 + 4 * RAD4_HORIZ_WORDS) {
        return s->horiz_timing[0][(offset - RAD4_BAR2_HORIZ0) / 4];
    }
    if (offset >= RAD4_BAR2_HORIZ1 &&
        offset < RAD4_BAR2_HORIZ1 + 4 * RAD4_HORIZ_WORDS) {
        return s->horiz_timing[1][(offset - RAD4_BAR2_HORIZ1) / 4];
    }
    if (offset >= RAD4_BAR2_HORIZ3 &&
        offset < RAD4_BAR2_HORIZ3 + 4 * RAD4_HORIZ_WORDS) {
        return s->horiz_timing[2][(offset - RAD4_BAR2_HORIZ3) / 4];
    }

    switch (offset) {
    case RAD4_BAR2_RGB_LO:
        return s->rgb_index_low;
    case RAD4_BAR2_RGB_HI:
        return s->rgb_index_high;
    case RAD4_BAR2_RGB_DATA: {
        uint16_t index = rad4_rgb_index(s);
        uint8_t val = s->rgb_store[index];

        /*
         * Real fidelity fix, not a functional necessity for any currently
         * observed guest behavior: IBM RGB561
         * Architecture p.1 documents the address index register
         * auto-incrementing "after each read or write cycle,"
         * but this model previously only advanced it after a write. Nothing
         * in the qualified guest workload reads
         * this port back, so the omission was harmless in practice; it is
         * still wrong to leave uncorrected while
         * this file's other index-then-data ports are being made to match the
         * same documented convention.
         */
        index++;
        s->rgb_index_low = index & 0xff;
        s->rgb_index_high = (index >> 8) & 0xff;
        return val;
    }
    case RAD4_BAR2_RGB_RESERVED: {
        /*
         * The fourth RGB561 indexed port, used for every multi-cycle ("N
         * RW/ADDR") register: window-attribute
         * tables, gamma tables, the runtime palette and the cursor/cross-hair
         * look-up table. One phase counter
         * serves the whole port (rgb_reserved_phase), matching the
         * datasheet's description of a single
         * address-index-register-driven state machine (IBM RGB561
         * Architecture p.1, "Address Index Register"). Only
         * the frame-buffer window attribute table, the runtime color look-up
         * table and the cursor/cross-hair
         * look-up table are modeled; the overlay WAT and gamma tables
         * reaching this port through other indices are
         * not handled by this model.
         */
        uint16_t index = rad4_rgb_index(s);
        uint8_t val = 0;

        if (rad4_index_is_cursor_lut(index)) {
            val = s->cursor_lut[index - RAD4_RGB_CURSOR_LUT_BASE]
                               [s->rgb_reserved_phase];
            rad4_rgb_reserved_advance(s, index, 3);
        } else if (rad4_index_in_wat(index)) {
            uint16_t wat = s->fb_wat[index - RAD4_WAT_BASE];

            /* Table 5, p.4: cycle 0 is bits 9-2, cycle 1 is bits 1-0. */
            val = s->rgb_reserved_phase == 0 ?
                  (wat >> RAD4_WAT_CYCLE0_SHIFT) & 0xff :
                  (uint8_t)((wat & RAD4_WAT_CYCLE1_MASK) <<
                            RAD4_WAT_CYCLE1_SHIFT);
            rad4_rgb_reserved_advance(s, index, RAD4_WAT_CYCLES);
        } else if (rad4_index_in_color_lut(index)) {
            val = s->color_lut[index - RAD4_COLOR_LUT_BASE]
                              [s->rgb_reserved_phase];
            rad4_rgb_reserved_advance(s, index, RAD4_COLOR_LUT_CYCLES);
        } else {
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad4: RGB561 multi-cycle read from "
                         "unsupported index 0x%04x\n", index);
        }
        return val;
    }
    case RAD4_BAR2_MASK:
        return s->mask;
    case RAD4_BAR2_CTRL: {
        uint32_t val = s->bar2_ctrl_bit;

        if (s->frame_pending) {
            val |= RAD4_STATUS_FRAME;
        }
        return val;
    }
    default:
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: unsupported BAR2 read at +0x%06x\n",
                     offset);
        return 0;
    }
}

static void rad4_bar2_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    PsitechRad4State *s = opaque;
    uint32_t offset = addr;
    uint32_t v = val;

    if (offset < RAD4_FB_SIZE) {
        uint32_t old = ldl_be_p(s->framebuffer + offset);

        stl_be_p(s->framebuffer + offset, (old & ~s->mask) | (v & s->mask));
        rad4_mark_dirty_range(s, offset, size);
        return;
    }
    if (offset >= RAD4_BAR2_VERT &&
        offset < RAD4_BAR2_VERT + 4 * RAD4_VERT_WORDS) {
        s->vert_timing[(offset - RAD4_BAR2_VERT) / 4] = v;
        rad4_recognizer_feed(s, offset, v);
        return;
    }
    if (offset >= RAD4_BAR2_HORIZ0 &&
        offset < RAD4_BAR2_HORIZ0 + 4 * RAD4_HORIZ_WORDS) {
        s->horiz_timing[0][(offset - RAD4_BAR2_HORIZ0) / 4] = v;
        rad4_recognizer_feed(s, offset, v);
        return;
    }
    if (offset >= RAD4_BAR2_HORIZ1 &&
        offset < RAD4_BAR2_HORIZ1 + 4 * RAD4_HORIZ_WORDS) {
        s->horiz_timing[1][(offset - RAD4_BAR2_HORIZ1) / 4] = v;
        rad4_recognizer_feed(s, offset, v);
        return;
    }
    if (offset >= RAD4_BAR2_HORIZ3 &&
        offset < RAD4_BAR2_HORIZ3 + 4 * RAD4_HORIZ_WORDS) {
        s->horiz_timing[2][(offset - RAD4_BAR2_HORIZ3) / 4] = v;
        rad4_recognizer_feed(s, offset, v);
        return;
    }

    switch (offset) {
    case RAD4_BAR2_RGB_LO:
        s->rgb_index_low = v;
        /*
         * Pointing the index register anywhere restarts any in-progress
         * multi-cycle transfer at its first cycle:
         * IBM RGB561 Architecture p.1, "If the address index register is
         * accessed before completion of a
         * multi-cycle access, the counter is reset and the operation is
         * terminated."
         */
        s->rgb_reserved_phase = 0;
        rad4_recognizer_feed(s, offset, v);
        break;
    case RAD4_BAR2_RGB_HI:
        s->rgb_index_high = v;
        s->rgb_reserved_phase = 0;
        rad4_recognizer_feed(s, offset, v);
        break;
    case RAD4_BAR2_RGB_DATA: {
        uint16_t index = rad4_rgb_index(s);

        if (!rad4_rgb_index_supported(index)) {
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad4: RGB561 write to reserved index "
                         "0x%04x\n", index);
        }
        s->rgb_store[index] = v;
        rad4_recognizer_feed(s, offset, v);

        if (index >= RAD4_RGB_CURSOR_CONTROL &&
            index <= RAD4_RGB_CURSOR_Y_HIGH) {
            int x, y;

            rad4_cursor_location(s, &x, &y);
            trace_rad4_cursor_position(x, y,
                                       (s->rgb_store[RAD4_RGB_CURSOR_CONTROL]
                                        & RAD4_CURSOR_CONTROL_CE) != 0);
            rad4_cursor_invalidate(s);
        } else if (index >= RAD4_RGB_CURSOR_PIXMAP_BASE &&
                  index < RAD4_RGB_CURSOR_PIXMAP_BASE +
                          RAD4_CURSOR_PIXMAP_SIZE) {
            rad4_cursor_invalidate(s);
        }

        /* Documented auto-increment on data-port access. */
        index++;
        s->rgb_index_low = index & 0xff;
        s->rgb_index_high = (index >> 8) & 0xff;
        break;
    }
    case RAD4_BAR2_RGB_RESERVED: {
        /* See the matching comment in rad4_bar2_read(). */
        uint16_t index = rad4_rgb_index(s);

        if (rad4_index_is_cursor_lut(index)) {
            s->cursor_lut[index - RAD4_RGB_CURSOR_LUT_BASE]
                         [s->rgb_reserved_phase] = v;
            rad4_recognizer_feed(s, offset, v);
            rad4_rgb_reserved_advance(s, index, 3);
            /*
             * Each cursor LUT entry is three bus cycles (R, G, B). Repainting
             * the sprite after the first or second
             * cycle would show a torn colour: two channels from the new write
             * and one
             * still holding the previous entry's byte, for a whole refresh
             * interval rather than the
             * fraction of a frame real hardware would.
             * Wait for rad4_rgb_reserved_advance() to wrap the phase counter
             * back to 0, marking the entry complete.
             */
            if (s->rgb_reserved_phase == 0) {
                rad4_cursor_invalidate(s);
            }
        } else if (rad4_index_in_wat(index)) {
            uint16_t wid = index - RAD4_WAT_BASE;

            /* Table 5, p.4: cycle 0 is bits 9-2, cycle 1 is bits 1-0. */
            if (s->rgb_reserved_phase == 0) {
                s->fb_wat[wid] = (uint16_t)(v & 0xff) <<
                                 RAD4_WAT_CYCLE0_SHIFT;
            } else {
                s->fb_wat[wid] |= (v >> RAD4_WAT_CYCLE1_SHIFT) &
                                  RAD4_WAT_CYCLE1_MASK;
            }
            rad4_rgb_reserved_advance(s, index, RAD4_WAT_CYCLES);
            /*
             * A window's FB_WAT entry selects how rad4_pixel_to_rgb()
             * interprets every pixel tagged with that
             * window ID. That changes what the existing scanout data means
             * without touching s->framebuffer, so the
             * raw-write dirty tracking above never sees it. Repaint the whole
             * frame once this entry is fully
             * written.
             */
            if (s->rgb_reserved_phase == 0) {
                rad4_mark_all_dirty(s);
            }
        } else if (rad4_index_in_color_lut(index)) {
            s->color_lut[index - RAD4_COLOR_LUT_BASE]
                        [s->rgb_reserved_phase] = v;
            rad4_rgb_reserved_advance(s, index, RAD4_COLOR_LUT_CYCLES);
            /*
             * Indexed-mode pixels resolve through this LUT in
             * rad4_pixel_to_rgb(); same reasoning as the FB_WAT
             * case above.
             */
            if (s->rgb_reserved_phase == 0) {
                rad4_mark_all_dirty(s);
            }
        } else {
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad4: RGB561 multi-cycle write to "
                         "unsupported index 0x%04x\n", index);
        }
        break;
    }
    case RAD4_BAR2_MASK:
        rad4_apply_mask_write(s, v);
        break;
    case RAD4_BAR2_CTRL:
        if (s->frame_pending && (v & RAD4_CTRL_STORED_BIT) == 0) {
            s->frame_pending = false;
            s->intcsr &= ~RAD4_INTCSR_PENDING;
            s->bar2_ctrl_bit = v & RAD4_CTRL_STORED_BIT;
            rad4_update_irq(s);
            break;
        }
        s->bar2_ctrl_bit = v & RAD4_CTRL_STORED_BIT;
        rad4_recognizer_feed(s, offset, v);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: unsupported BAR2 write at +0x%06x\n",
                     offset);
        break;
    }
}

static const MemoryRegionOps rad4_bar2_ops = {
    .read = rad4_bar2_read,
    .write = rad4_bar2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rad4_reset_hold(Object *obj, ResetType type)
{
    PsitechRad4State *s = PSITECH_RAD4(obj);

    timer_del(s->frame_timer);

    s->reg14 = 0;
    s->reg18 = 0;
    s->mailbox = 0;
    s->intcsr = 0;
    s->serial = 0;
    rad4_serial_reset(s);
    s->serial_clk_prev = false;
    s->serial_cs_prev = false;

    s->rom_desc = 0;
    s->rom_mode = 0;
    s->rom_command = 0;

    memset(s->framebuffer, 0, RAD4_FB_SIZE);
    s->mask = 0xffffffffu;
    /*
     * The framebuffer just went to zero; an already-recognized display needs
     * the next refresh to repaint the whole
     * frame from that, not take the idle fast path and keep showing the
     * pre-reset display_buffer indefinitely.
     */
    rad4_mark_all_dirty(s);

    s->rgb_index_low = 0;
    s->rgb_index_high = 0;
    memset(s->rgb_store, 0, 0x10000);

    /*
     * IBM RGB561 Architecture p.1 excludes the Color, Cursor and Gamma LUTs
     * and the Window Attribute Tables from
     * the reset-to-zero rule, since real hardware does not clear this SRAM on
     * reset and expects software to reload
     * it. This model zeroes them anyway, matching rgb_store's own
     * unconditional reset just above, for a
     * deterministic post-reset state.
     */
    memset(s->fb_wat, 0, sizeof(s->fb_wat));
    memset(s->color_lut, 0, sizeof(s->color_lut));
    memset(s->cursor_lut, 0, sizeof(s->cursor_lut));
    s->rgb_reserved_phase = 0;

    memset(s->vert_timing, 0, sizeof(s->vert_timing));
    memset(s->horiz_timing, 0, sizeof(s->horiz_timing));

    s->bar2_ctrl_bit = 0;
    s->frame_pending = false;

    rad4_recognizer_reset(s);

    /*
     * The km child sits on the default sysbus, not a dedicated internal qbus,
     * so a reset of this device does not
     * reach it on its own and needs explicit propagation here.
     */
    if (s->device_id == RAD4_KM_DEVICE_ID) {
        device_cold_reset(DEVICE(&s->km));
    }

    rad4_update_irq(s);
    /* The full repaint above recomputes where the (now disabled) cursor
     * is. */
    s->cursor_shown_y_start = RAD4_MODE0_HEIGHT;
    s->cursor_shown_y_end = -1;
}

static void rad4_realize(PCIDevice *dev, Error **errp)
{
    PsitechRad4State *s = PSITECH_RAD4(dev);
    Object *obj = OBJECT(dev);

    s->framebuffer = g_malloc0(RAD4_FB_SIZE);
    s->rgb_store = g_malloc0(0x10000);
    s->display_buffer = g_malloc0(RAD4_MODE0_WIDTH * RAD4_MODE0_HEIGHT * 4);

    s->con = qemu_graphic_console_create(DEVICE(dev), 0, &rad4_gfx_ops, s);

    memory_region_init_io(&s->bar0, obj, &rad4_bar0_ops, s,
                          "psitech-rad4-bar0", RAD4_BAR0_SIZE);
    memory_region_init_io(&s->bar2, obj, &rad4_bar2_ops, s,
                          "psitech-rad4-bar2", RAD4_BAR2_SIZE);
    memory_region_init_io(&s->romroute, obj, &rad4_romroute_ops, s,
                          "psitech-rad4-romroute", RAD4_ROMROUTE_SIZE);

    pci_set_byte(&s->pci.config[PCI_REVISION_ID], 0x03);
    pci_config_set_class(s->pci.config, PCI_CLASS_DISPLAY_OTHER);
    pci_set_word(&s->pci.config[PCI_SUBSYSTEM_VENDOR_ID], 0);
    pci_set_word(&s->pci.config[PCI_SUBSYSTEM_ID], 0);
    s->pci.config[PCI_INTERRUPT_PIN] = 0x01;

    /*
     * The identity a guest's hinv/PCI probe actually reads, over the
     * class-wide RAD4_DEVICE_ID default the PCI core
     * already wrote. Mirrors hw/misc/sgi-ioc3.c's own board-identity override
     * of PCI_SUBSYSTEM_ID in its realize().
     */
    pci_set_word(&s->pci.config[PCI_DEVICE_ID], s->device_id);

    pci_register_bar(&s->pci, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
    pci_register_bar(&s->pci, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar2);
    pci_register_bar(&s->pci, PCI_ROM_SLOT, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->romroute);

    /*
     * The keyboard/mouse controller only exists under the RAD4-KM identity
     * (hw/input/psitech-rad4-km.c's own header
     * names what is and is not modeled for it); plain RAD4 never had this
     * hardware.
     */
    if (s->device_id == RAD4_KM_DEVICE_ID) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->km), errp)) {
            return;
        }
        pci_register_bar(&s->pci, 3, PCI_BASE_ADDRESS_SPACE_MEMORY,
                         sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->km), 0));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->km), 0,
                           qdev_get_gpio_in_named(DEVICE(dev), "km-irq", 0));
    } else {
        object_unparent(OBJECT(&s->km));
    }

    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rad4_frame_timer_cb, s);

    rad4_ensure_surface(s);
}

static void rad4_exit(PCIDevice *dev)
{
    PsitechRad4State *s = PSITECH_RAD4(dev);

    timer_free(s->frame_timer);
    qemu_graphic_console_close(s->con);
    g_free(s->framebuffer);
    g_free(s->rgb_store);
    g_free(s->display_buffer);
}

static int rad4_post_load(void *opaque, int version_id)
{
    PsitechRad4State *s = opaque;

    /*
     * display_buffer is not itself part of VMState (only the raw framebuffer
     * it is converted from is);
     * dirty_y_start/dirty_y_end are not migrated either, and on the
     * destination are whatever a fresh realize() left
     * them. Without an explicit full repaint here, a migrated-in guest whose
     * refresh finds nothing newly dirty
     * would show a stale or blank display_buffer until the guest next writes.
     */
    rad4_mark_all_dirty(s);
    s->cursor_shown_y_start = RAD4_MODE0_HEIGHT;
    s->cursor_shown_y_end = -1;
    rad4_ensure_surface(s);
    rad4_update_irq(s);
    if (s->rec_state == RAD4_REC_RECOGNIZED && s->frame_timer &&
        !timer_pending(s->frame_timer)) {
        timer_mod(s->frame_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 NANOSECONDS_PER_SECOND / RAD4_FRAME_HZ);
    }
    return 0;
}

static const VMStateDescription vmstate_psitech_rad4 = {
    .name = TYPE_PSITECH_RAD4,
    .version_id = 3,
    .minimum_version_id = 3,
    .post_load = rad4_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(pci, PsitechRad4State),
        VMSTATE_UINT32(reg14, PsitechRad4State),
        VMSTATE_UINT32(reg18, PsitechRad4State),
        VMSTATE_UINT32(mailbox, PsitechRad4State),
        VMSTATE_UINT32(intcsr, PsitechRad4State),
        VMSTATE_UINT32(serial, PsitechRad4State),
        VMSTATE_INT32(serial_state, PsitechRad4State),
        VMSTATE_BOOL(serial_clk_prev, PsitechRad4State),
        VMSTATE_BOOL(serial_cs_prev, PsitechRad4State),
        VMSTATE_UINT32(serial_bit_count, PsitechRad4State),
        VMSTATE_UINT16(serial_command_shift, PsitechRad4State),
        VMSTATE_BOOL(serial_miso, PsitechRad4State),
        VMSTATE_UINT32(rom_desc, PsitechRad4State),
        VMSTATE_UINT32(rom_mode, PsitechRad4State),
        VMSTATE_UINT32(rom_command, PsitechRad4State),
        VMSTATE_BUFFER_POINTER_UNSAFE(framebuffer, PsitechRad4State, 1,
                                      RAD4_FB_SIZE),
        VMSTATE_UINT32(mask, PsitechRad4State),
        VMSTATE_UINT8(rgb_index_low, PsitechRad4State),
        VMSTATE_UINT8(rgb_index_high, PsitechRad4State),
        VMSTATE_BUFFER_POINTER_UNSAFE(rgb_store, PsitechRad4State, 1,
                                      0x10000),
        VMSTATE_UINT16_ARRAY(fb_wat, PsitechRad4State, RAD4_WAT_COUNT),
        VMSTATE_UINT8_2DARRAY(color_lut, PsitechRad4State,
                              RAD4_COLOR_LUT_COUNT, 3),
        VMSTATE_UINT8_2DARRAY(cursor_lut, PsitechRad4State,
                              RAD4_RGB_CURSOR_LUT_COUNT, 3),
        VMSTATE_UINT8(rgb_reserved_phase, PsitechRad4State),
        VMSTATE_UINT32_ARRAY(vert_timing, PsitechRad4State, RAD4_VERT_WORDS),
        VMSTATE_UINT32_2DARRAY(horiz_timing, PsitechRad4State, 3,
                               RAD4_HORIZ_WORDS),
        VMSTATE_UINT32(bar2_ctrl_bit, PsitechRad4State),
        VMSTATE_BOOL(frame_pending, PsitechRad4State),
        VMSTATE_INT32(rec_state, PsitechRad4State),
        VMSTATE_UINT32(rec_index, PsitechRad4State),
        VMSTATE_BOOL(km_irq_level, PsitechRad4State),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * device-id defaults to plain RAD4; "-device psitech-rad4,device-id=0x105"
 * (RAD4_KM_DEVICE_ID) selects the RAD4-KM
 * identity instead, over this same shared implementation, the way
 * hw/misc/sgi-ioc3.c's own "subsystem-id" property
 * selects a board identity.
 */
static const Property psitech_rad4_properties[] = {
    DEFINE_PROP_UINT16("device-id", PsitechRad4State, device_id,
                       RAD4_DEVICE_ID),
};

static void rad4_instance_init(Object *obj)
{
    PsitechRad4State *s = PSITECH_RAD4(obj);

    /*
     * Created unconditionally; rad4_realize() only realizes it (and only
     * registers BAR 3) under the RAD4-KM
     * identity, and unparents it otherwise. See hw/input/psitech-rad4-km.c.
     */
    object_initialize_child(obj, "km", &s->km, TYPE_PSITECH_RAD4_KM);
    qdev_init_gpio_in_named(DEVICE(obj), rad4_km_irq_handler, "km-irq", 1);
}

static void rad4_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    k->vendor_id = RAD4_VENDOR_ID;
    k->device_id = RAD4_DEVICE_ID;
    k->class_id = PCI_CLASS_DISPLAY_OTHER;
    k->realize = rad4_realize;
    k->exit = rad4_exit;

    rc->phases.hold = rad4_reset_hold;

    dc->desc = "PsiTech RAD4 (one-mode functional approximation)";
    dc->vmsd = &vmstate_psitech_rad4;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, psitech_rad4_properties);
}

static const TypeInfo psitech_rad4_type_info = {
    .name          = TYPE_PSITECH_RAD4,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PsitechRad4State),
    .instance_init = rad4_instance_init,
    .class_init    = rad4_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void psitech_rad4_register_types(void)
{
    type_register_static(&psitech_rad4_type_info);
}

type_init(psitech_rad4_register_types)
