/* SPDX-License-Identifier: BSD-3-Clause */
/* PsiTech RAD4 (12d1:0104) and RAD4-KM primary PCI heads on an Origin 200
 * Bridge slot. */
/*
 * This models a compatibility profile for one exact stock software path
 * (PsiTech release 1.3k kernel driver and
 * standalone X server, in any of the driver's 13 display modes), not
 * recovered RAD4 hardware fidelity. The
 * expansion-ROM-route DMA registers (RAD4_ROM_MODE/RAD4_ROM_DESC/
 * RAD4_ROM_COMMAND) model the PLX
 * PCI9060/9054-family scatter/gather engine the driver's buffered/type-3 path
 * drives; every other pixel path still
 * moves data through ordinary CPU-issued BAR2 stores.
 *
 * The per-mode timing sequences gating scanout come from a generated
 * fixture (psitech-rad4-modes-fixture.h),
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
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/input/psitech-rad4-km.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "qom/object.h"
#include "trace.h"

#include "psitech-rad4-modes-fixture.h"

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

#define RAD4_FB_SIZE            (8 * MiB)

/* BAR0 offsets, PLX9060-compatible. */
#define RAD4_BAR0_REG14         0x14
#define RAD4_BAR0_REG18         0x18
#define RAD4_BAR0_MAILBOX       0x40
#define RAD4_BAR0_INTCSR        0x68
#define RAD4_BAR0_SERIAL        0x6c

/*
 * INTCSR bits, PLX PCI 9060 Data Sheet (Version 1.2, December 1995) Table
 * 45. PENDING is the local interrupt input's status and DMA0_ACTIVE the DMA
 * channel 0 interrupt's; both follow their sources and ignore writes. The
 * PCI line follows the local input only with both the PCI interrupt enable
 * and the PCI local interrupt enable set.
 *
 * RAD4_INTCSR_WRITABLE is every bit the table marks writable: bits 2:0, 12:8,
 * 19:16 and 27. The rest are read-only and ignore writes. Bits 8 and 16
 * reset set. The writable bits for sources this model does not have (LSERR#,
 * SERR#, doorbells, aborts, retry abort, DMA channel 1) are stored and read
 * back without effect.
 */
#define RAD4_INTCSR_SERR                (1u << 2)
#define RAD4_INTCSR_ENABLE              (1u << 8)
#define RAD4_INTCSR_LOCAL_IN_ENABLE     (1u << 11)
#define RAD4_INTCSR_PENDING             (1u << 15)
#define RAD4_INTCSR_LOCAL_OUT_ENABLE    (1u << 16)
#define RAD4_INTCSR_LOCAL_DMA0_ENABLE   (1u << 18)
#define RAD4_INTCSR_DMA0_ACTIVE         (1u << 21)
#define RAD4_INTCSR_STATUS      (RAD4_INTCSR_PENDING | RAD4_INTCSR_DMA0_ACTIVE)
#define RAD4_INTCSR_WRITABLE    0x080f1f07u
#define RAD4_INTCSR_RESET \
    (RAD4_INTCSR_ENABLE | RAD4_INTCSR_LOCAL_OUT_ENABLE)

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
#define RAD4_DMA_CMD_CLEAR_INT  (1u << 3)
#define RAD4_DMA_STATUS_DONE    (1u << 4)

/*
 * DMADPR0 bits (PCI 9060 Data Sheet Table 51): the low 4 bits of both
 * the Descriptor Pointer Register and each descriptor's own "next
 * descriptor" word. RAD4_DMA_DESC_ADDR_MASK isolates the quad-word-aligned
 * local address the rest of the word carries. Bit 0 is reserved on the
 * 9060; the PCI 9054 later used it to place descriptors in host memory.
 */
#define RAD4_DMA_DESC_RESERVED      (1u << 0)
#define RAD4_DMA_DESC_END_OF_CHAIN  (1u << 1)
#define RAD4_DMA_DESC_INTR_ON_TC    (1u << 2)
#define RAD4_DMA_DESC_LOCAL_TO_PCI  (1u << 3) /* 1: framebuffer -> host */
#define RAD4_DMA_DESC_ADDR_MASK     0xfffffff0u

/* DMASIZ0 is a 23-bit byte count (Table 11-79). */
#define RAD4_DMA_SIZE_MASK      0x7fffffu
/*
 * The chip walks a chain until a descriptor ends it; the bound only stops
 * the model spinning on a chain that loops. Local memory holds this many
 * distinct descriptors, so any longer walk has revisited one. The shipped
 * driver's chains run one descriptor per scanline, 1024 for a full
 * 1280x1024 upload.
 */
#define RAD4_DMA_MAX_DESCRIPTORS (RAD4_FB_SIZE / 16)

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

/*
 * Timing memory depths. The physical depths are unknown; these hold the
 * largest bank of any driver mode (781 horizontal words, 1656 vertical),
 * and a horizontal bank cannot exceed the 4 KiB stride between banks.
 */
#define RAD4_HORIZ_WORDS        1024
#define RAD4_VERT_WORDS         2048

#define RAD4_CTRL_MODE_CMD      0x20u

/* One bit per entry of rad4_sequences[]. */
#define RAD4_CANDIDATE_WORDS    DIV_ROUND_UP(RAD4_SEQUENCE_COUNT, 64)
#define RAD4_CTRL_STORED_BIT    0x02u
#define RAD4_STATUS_FRAME       0x20u

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
 * RAD4_BAR2_RGB_DATA port. This model implements the ranges the qualified
 * guest workload exercises for display: the frame-buffer and overlay window
 * attribute tables (FB_WAT, OL_WAT), the runtime color look-up table they
 * point into, the cursor/cross-hair look-up table and the red, green and
 * blue gamma tables.
 */
#define RAD4_WAT_BASE           0x1000
#define RAD4_WAT_COUNT          256
#define RAD4_WAT_LAST           (RAD4_WAT_BASE + RAD4_WAT_COUNT - 1)
#define RAD4_OL_WAT_BASE        0x1400
#define RAD4_OL_WAT_LAST        (RAD4_OL_WAT_BASE + RAD4_WAT_COUNT - 1)
#define RAD4_WAT_CYCLES         2

/*
 * Gamma tables, pages 3 and 27: one 256x10 table per DAC component at
 * X'3000, X'3400 and X'3800, loaded in the same two cycles as a WAT entry
 * (Table 2).
 */
#define RAD4_GAMMA_BASE         0x3000
#define RAD4_GAMMA_STRIDE       0x400
#define RAD4_GAMMA_COUNT        256

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
 * traced rather than guessed at. Page 48's OL_WAT entry uses the same field
 * positions with overlay meanings: PIX_FORM 00 is an 8-bit overlay and 11 a
 * double-buffered 4-bit overlay, and MODE 00 is Index.
 */
#define RAD4_WAT_START_ADDR_SHIFT 6
#define RAD4_WAT_START_ADDR_MASK  0xf
#define RAD4_WAT_PIXFORM_SHIFT    4
#define RAD4_WAT_PIXFORM_MASK     0x3
#define RAD4_WAT_PIXFORM_8BPP     0x0
#define RAD4_WAT_PIXFORM_12BPP    0x1
#define RAD4_WAT_PIXFORM_16BPP    0x2
#define RAD4_WAT_PIXFORM_24BPP    0x3
#define RAD4_WAT_BS               (1u << 3)
#define RAD4_WAT_MODE_SHIFT       1
#define RAD4_WAT_MODE_MASK        0x3
#define RAD4_WAT_MODE_INDEX       0x0
#define RAD4_WAT_MODE_GREY        0x1
#define RAD4_WAT_MODE_TRUE        0x3
#define RAD4_WAT_TR               (1u << 0)
#define RAD4_OL_WAT_PIXFORM_8BIT  0x0
#define RAD4_OL_WAT_PIXFORM_4BIT  0x3

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
 * Single-cycle registers that select how the window attribute tables are
 * addressed and how overlay data is combined with frame buffer data: CONF/1
 * (page 29), CONF/4 (page 32), the WAT segment registers and the overlay
 * chroma key and mask registers (page 34), and the auxiliary window
 * attribute tables (page 46).
 */
#define RAD4_RGB_CONF1              0x0001
#define RAD4_RGB_CONF4              0x0004
#define RAD4_RGB_FB_WAT_SEGMENT     0x0006
#define RAD4_RGB_OL_WAT_SEGMENT     0x0007
#define RAD4_RGB_AUX_FB_WAT_SEGMENT 0x0008
#define RAD4_RGB_AUX_OL_WAT_SEGMENT 0x0009
#define RAD4_RGB_CHROMA_KEY0        0x0010
#define RAD4_RGB_CHROMA_MASK0       0x0012
#define RAD4_RGB_AUX_FB_WAT         0x0e00
#define RAD4_RGB_AUX_OL_WAT         0x0f00
#define RAD4_RGB_AUX_WAT_COUNT      16

#define RAD4_CONF1_MUX_SHIFT        5
#define RAD4_CONF1_MUX_4TO1_BASIC   0x1
#define RAD4_CONF1_OVLY_SHIFT       3
#define RAD4_CONF1_OVLY_MASK        0x3
#define RAD4_CONF1_OVLY_NONE        0x0
#define RAD4_CONF1_OVLY_8BIT        0x1
#define RAD4_CONF1_LWID_MASK        0x7
#define RAD4_CONF4_FB_WID_SHIFT     3
#define RAD4_CONF4_FB_WID_MASK      0xf
#define RAD4_CONF4_SWE              (1u << 2)
#define RAD4_CONF4_AOW              (1u << 1)
#define RAD4_CONF4_AFW              (1u << 0)

/* AUX_FB_WAT and AUX_OL_WAT bit 2, page 46: bypass the gamma tables. */
#define RAD4_AUX_WAT_GAMMA_BYPASS   (1u << 2)

/* AUX_OL_WAT entry, page 46: CK in bit 5, OL enable in bit 3, OT in bit 0. */
#define RAD4_AUX_OL_WAT_CK          (1u << 5)
#define RAD4_AUX_OL_WAT_OL          (1u << 3)
#define RAD4_AUX_OL_WAT_OT          (1u << 0)

/*
 * The top byte of RAD4's 32-bit software pixel word. The RGB561 4:1 basic
 * serializer that CONF/1 selects (page 9, Table 9) takes 32 pixel bits, of
 * which bits 24-31 are the overlay port, and a separate 8-bit window-ID
 * input. A 4-bit overlay places buffer A in bits 24-27 and buffer B in bits
 * 28-31.
 *
 * The board's wiring of the window-ID input is not documented. This model
 * feeds it the same top byte. The shipped X server's own programming
 * supports that: it loads FB_WAT in blocks of 16 identical entries, so the
 * entry it selects depends only on bits 28-31 and not on the overlay value
 * in bits 24-27, and it sets CONF/4's AFW so the auxiliary frame-buffer
 * table follows the same upper four bits.
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

/*
 * The serial configuration EEPROM behind BAR0 offset 0x6c, a 93C46-type
 * 64-word by 16-bit Microwire part. The driver's own commands identify it:
 * after a start bit it sends READ (10), WRITE (01), EWEN (00 11xxxx) and
 * EWDS (00 00xxxx) with six address bits, the public 93C46 x16 command set.
 * The part number on the board is not established.
 *
 * Each command is a start bit (the first one sampled with chip select
 * high), two opcode bits and six address bits. READ answers a dummy zero
 * and then streams words, most significant bit first, continuing into the
 * next address for as long as chip select stays high. WRITE takes 16 data
 * bits and programs the word when chip select drops, only after EWEN.
 * Programming completes at once: chip select raised again reads data-out
 * one (ready), so the driver's busy poll ends on its first sample.
 */
typedef enum Rad4SerialState {
    RAD4_SERIAL_IDLE,       /* chip select low */
    RAD4_SERIAL_START,      /* waiting for the start bit */
    RAD4_SERIAL_COMMAND,    /* opcode and address bits */
    RAD4_SERIAL_READ_DUMMY, /* READ decoded; dummy zero on the next fall */
    RAD4_SERIAL_READ,       /* streaming data bits */
    RAD4_SERIAL_WRITE_DATA, /* WRITE decoded; collecting 16 data bits */
    RAD4_SERIAL_WRITE_READY, /* data complete; programs on chip select low */
    RAD4_SERIAL_DONE,       /* command finished; data-out stays high */
} Rad4SerialState;

#define RAD4_EEPROM_WORDS       64
#define RAD4_EEPROM_BYTES       (RAD4_EEPROM_WORDS * 2)
#define RAD4_EEPROM_ADDR_BITS   6
#define RAD4_EEPROM_ADDR_MASK   (RAD4_EEPROM_WORDS - 1)
#define RAD4_EEPROM_CMD_BITS    (2 + RAD4_EEPROM_ADDR_BITS)

#define RAD4_EEPROM_OP_EXTENDED 0x0 /* address bits 5-4 select the command */
#define RAD4_EEPROM_OP_WRITE    0x1
#define RAD4_EEPROM_OP_READ     0x2
#define RAD4_EEPROM_EXT_EWDS    0x0
#define RAD4_EEPROM_EXT_EWEN    0x3

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
    /* DMA channel 0's interrupt, active until DMACSR0 clears it. */
    bool dma0_irq;

    int serial_state; /* Rad4SerialState, as plain int for VMState */
    bool serial_clk_prev;
    bool serial_cs_prev;
    uint32_t serial_bit_count;
    uint16_t serial_command_shift;
    bool serial_miso;

    /*
     * Serial EEPROM contents and command state. The contents survive device
     * and system reset, like the nonvolatile part they stand for, and are
     * written through to the optional backing drive.
     */
    uint16_t eeprom[RAD4_EEPROM_WORDS];
    uint8_t eeprom_address;
    uint16_t eeprom_data;
    bool eeprom_write_enabled;
    BlockBackend *eeprom_blk;

    /* Expansion-ROM route state. */
    uint32_t rom_desc;
    uint32_t rom_mode;
    uint32_t rom_command;

    /*
     * DMA channel 0 in flight: running from its start until the chain ends,
     * fails or is aborted, and paused while DMACSR0's enable bit is clear.
     * The descriptor being moved, how far it has got, its next-pointer word
     * and how many descriptors the chain has loaded.
     */
    QEMUTimer *dma_timer;
    bool dma_running;
    bool dma_have_desc;
    uint32_t dma_pci;
    uint32_t dma_local;
    uint32_t dma_size;
    uint32_t dma_moved;
    uint32_t dma_next;
    uint32_t dma_descs;
    /* Virtual time up to which the transfer's bus time has been spent. */
    int64_t dma_paid_ns;

    /* BAR2 framebuffer and mask. */
    uint8_t *framebuffer;
    uint32_t mask;
    bool overlay_index_compat;

    /*
     * Scanlines of the framebuffer written since the last rad4_gfx_update(),
     * inclusive and empty when dirty_y_start
     * > dirty_y_end. framebuffer is a plain buffer behind a masked-write MMIO
     * trap, not a RAM MemoryRegion, so it
     * cannot use the region dirty-bitmap helpers in hw/display/framebuffer.c;
     * rad4_bar2_write() and
     * rad4_dma_merge() mark rows by hand as they write, and
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
     * RGB561 multi-cycle port state (BAR2 +0x930000): the frame-buffer and
     * overlay window attribute tables, the color look-up
     * table they address, and the cursor/cross-hair look-up table. One phase
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
    uint16_t ol_wat[RAD4_WAT_COUNT];
    uint16_t gamma[3][RAD4_GAMMA_COUNT];
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

    /*
     * Mode recognizer: the position reached in the write sequence and the
     * sequences (bits of rad4_sequences[]) that match every write so far.
     * mode is the last recognized mode, mode 0 until one is.
     */
    int rec_state; /* Rad4RecState, as plain int for VMState */
    uint32_t rec_index;
    uint64_t rec_candidates[RAD4_CANDIDATE_WORDS];
    uint32_t mode;
};

static const Rad4ModeFixture *rad4_mode(PsitechRad4State *s)
{
    return &rad4_modes[s->mode];
}

/* display_buffer holds the largest mode's image. */
static size_t rad4_max_pixels(void)
{
    size_t pixels = 0;

    for (int m = 0; m < RAD4_MODE_COUNT; m++) {
        pixels = MAX(pixels, (size_t)rad4_modes[m].width *
                             rad4_modes[m].height);
    }
    return pixels;
}

static int rad4_width(PsitechRad4State *s)
{
    return rad4_mode(s)->width;
}

static int rad4_height(PsitechRad4State *s)
{
    return rad4_mode(s)->height;
}

/* Every mode's rows are 4 * width bytes apart in the framebuffer. */
static uint32_t rad4_stride(PsitechRad4State *s)
{
    return rad4_width(s) * 4;
}

/* One frame at the mode's refresh label, the table's nominal rate. */
static int64_t rad4_frame_period(PsitechRad4State *s)
{
    return NANOSECONDS_PER_SECOND / rad4_mode(s)->refresh_hz;
}

static uint16_t rad4_rgb_index(PsitechRad4State *s)
{
    return ((uint16_t)s->rgb_index_high << 8) | s->rgb_index_low;
}

/*
 * The FB_WAT or OL_WAT entry selected by a multi-cycle port index, or NULL
 * for any other index.
 */
static uint16_t *rad4_wat_entry(PsitechRad4State *s, uint16_t index)
{
    if (index >= RAD4_WAT_BASE && index <= RAD4_WAT_LAST) {
        return &s->fb_wat[index - RAD4_WAT_BASE];
    }
    if (index >= RAD4_OL_WAT_BASE && index <= RAD4_OL_WAT_LAST) {
        return &s->ol_wat[index - RAD4_OL_WAT_BASE];
    }
    return NULL;
}

/* The gamma table entry selected by a multi-cycle port index, or NULL. */
static uint16_t *rad4_gamma_entry(PsitechRad4State *s, uint16_t index)
{
    unsigned table = (index - RAD4_GAMMA_BASE) / RAD4_GAMMA_STRIDE;
    unsigned entry = (index - RAD4_GAMMA_BASE) % RAD4_GAMMA_STRIDE;

    if (index < RAD4_GAMMA_BASE || table >= 3 || entry >= RAD4_GAMMA_COUNT) {
        return NULL;
    }
    return &s->gamma[table][entry];
}

/*
 * The 10-bit, two-cycle entry (a WAT or gamma table entry, Tables 2 and 5)
 * selected by a multi-cycle port index, or NULL for any other index.
 */
static uint16_t *rad4_ten_bit_entry(PsitechRad4State *s, uint16_t index)
{
    uint16_t *entry = rad4_wat_entry(s, index);

    return entry ? entry : rad4_gamma_entry(s, index);
}

/*
 * Single-cycle registers whose value changes what the scanout shows for
 * pixels already in the frame buffer.
 */
static bool rad4_rgb_index_is_display_attribute(uint16_t index)
{
    return (index >= RAD4_RGB_CONF1 && index <= RAD4_RGB_AUX_OL_WAT_SEGMENT) ||
           (index >= RAD4_RGB_CHROMA_KEY0 &&
            index <= RAD4_RGB_CHROMA_MASK0 + 1) ||
           (index >= RAD4_RGB_AUX_FB_WAT &&
            index < RAD4_RGB_AUX_FB_WAT + RAD4_RGB_AUX_WAT_COUNT) ||
           (index >= RAD4_RGB_AUX_OL_WAT &&
            index < RAD4_RGB_AUX_OL_WAT + RAD4_RGB_AUX_WAT_COUNT);
}

/*
 * The overlay plane mask the shipped X server sets for its 4-plane overlay
 * visual: bits 24-27 of the pixel word (see RAD4_PIXEL_WID_SHIFT).
 */
#define RAD4_OVERLAY_PLANE_MASK 0x0f000000u

/*
 * One 32-bit framebuffer store under the plane mask:
 * new = (old & ~mask) | (value & mask).
 *
 * Guest compatibility approximation, not established board behaviour: with
 * overlay-index-compat on, a CPU store under the overlay plane mask whose
 * value is a bare 4-bit index (nothing outside bits 0-3) writes that index
 * into the overlay bits. The shipped IRIX 6.5.30 X server draws Motif's
 * insensitive (greyed) menu labels with a tiled span fill that copies raw
 * depth-4 tile pixels unshifted, while every other overlay path it and the
 * driver use shifts the index to bits 24-27. Under a plain plane mask those
 * labels come out transparent and the window behind shows through them. No
 * other observed CPU overlay store is a bare low-nibble index, so the
 * remaining overlay drawing is unchanged. DMA never gets the rule: the
 * buffered X server's sweep uploads 8-bit root-visual pixels such as
 * 0x0000000d while the overlay mask is in force, and shifting those turned
 * the whole desktop into opaque overlay. The property turns this off for
 * strict plane-mask behaviour.
 */
static uint32_t rad4_masked_store(PsitechRad4State *s, uint32_t old,
                                  uint32_t value)
{
    if (s->overlay_index_compat &&
        s->mask == RAD4_OVERLAY_PLANE_MASK &&
        (value & ~0xfu) == 0) {
        value <<= RAD4_PIXEL_WID_SHIFT;
    }
    return (old & ~s->mask) | (value & s->mask);
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
 * How the window-ID input addresses the window attribute tables, decoded once
 * per scanout pass from CONF/1, CONF/4 and the segment registers (IBM RGB561
 * Architecture pages 15-17, 29 and 32-34). CONF/1's LWID gives the number of
 * logical window-ID bits, taken from the least significant end of the
 * window-ID input. In common mode (CONF/4 SWE clear) those bits address both
 * FB_WAT and OL_WAT. In split mode the low FB_WID of them address FB_WAT and
 * the rest address OL_WAT. Address bits a table does not receive come from
 * its segment register, so a table that receives none uses the segment
 * register's entry for every pixel.
 */
typedef struct Rad4WatLayout {
    uint8_t lwid_mask;
    uint8_t fb_bits;
    uint8_t ol_bits;
    uint8_t ol_shift;
    uint8_t fb_segment;
    uint8_t ol_segment;
    uint8_t aux_fb_segment;
    bool aux_fb_msb;
    uint8_t aux_ol_segment;
    bool aux_ol_msb;
    unsigned overlay;
    bool overlay_reported;
    bool fb_reported;
} Rad4WatLayout;

/* The low @bits bits of a window ID; @bits may be 0 to 8. */
static uint8_t rad4_wid_mask(unsigned bits)
{
    return (1u << bits) - 1;
}

static unsigned rad4_lwid_bits(uint8_t conf1)
{
    unsigned lwid = conf1 & RAD4_CONF1_LWID_MASK;

    /* Page 29: 000, 001, 010 and 011 select 0, 2, 4 and 6; 1xx selects 8. */
    return lwid >= 4 ? 8 : lwid * 2;
}

static void rad4_wat_layout(PsitechRad4State *s, Rad4WatLayout *l)
{
    uint8_t conf1 = s->rgb_store[RAD4_RGB_CONF1];
    uint8_t conf4 = s->rgb_store[RAD4_RGB_CONF4];
    unsigned lwid = rad4_lwid_bits(conf1);

    l->lwid_mask = rad4_wid_mask(lwid);
    if (conf4 & RAD4_CONF4_SWE) {
        unsigned fb_wid = (conf4 >> RAD4_CONF4_FB_WID_SHIFT) &
                          RAD4_CONF4_FB_WID_MASK;

        /*
         * Page 32: 1xxx selects all 8 window-ID bits for the frame buffer.
         * The document does not say what happens when FB_WID asks for more
         * bits than CONF/1's LWID provides; this model gives the frame
         * buffer all of them.
         */
        l->fb_bits = MIN(fb_wid >= 8 ? 8 : fb_wid, lwid);
        l->ol_bits = lwid - l->fb_bits;
        l->ol_shift = l->fb_bits;
    } else {
        l->fb_bits = lwid;
        l->ol_bits = lwid;
        l->ol_shift = 0;
    }
    l->fb_segment = s->rgb_store[RAD4_RGB_FB_WAT_SEGMENT];
    l->ol_segment = s->rgb_store[RAD4_RGB_OL_WAT_SEGMENT];
    l->aux_fb_segment = s->rgb_store[RAD4_RGB_AUX_FB_WAT_SEGMENT] & 0xf;
    l->aux_fb_msb = conf4 & RAD4_CONF4_AFW;
    l->aux_ol_segment = s->rgb_store[RAD4_RGB_AUX_OL_WAT_SEGMENT] & 0xf;
    l->aux_ol_msb = conf4 & RAD4_CONF4_AOW;
    l->overlay = (conf1 >> RAD4_CONF1_OVLY_SHIFT) & RAD4_CONF1_OVLY_MASK;
    /*
     * Only the 4:1 basic serializer's pixel layout (page 9, Table 9) is
     * modeled; the overlay port of any other mode is reported as an
     * unimplemented overlay rather than read from the wrong bits.
     */
    if (l->overlay != RAD4_CONF1_OVLY_NONE &&
        (conf1 >> RAD4_CONF1_MUX_SHIFT) != RAD4_CONF1_MUX_4TO1_BASIC) {
        l->overlay = RAD4_CONF1_OVLY_MASK + 1;
    }
    l->overlay_reported = false;
    l->fb_reported = false;
}

/*
 * A WAT address: the window-ID bits, padded above by the segment register.
 * Pages 4, 15 and 32 and Figures 2-3 take the missing bits from the segment
 * register. The note on page 29 says they are padded with zeros instead;
 * this model follows the majority, which matches zeros whenever the
 * segment register is zero, as the shipped X server leaves it.
 */
static uint8_t rad4_wat_address(unsigned bits, uint8_t wid, uint8_t segment)
{
    uint8_t mask = rad4_wid_mask(bits);

    return (segment & ~mask) | (wid & mask);
}

/*
 * A 16-entry auxiliary WAT address, Figures 2 and 3 on pages 16-17: with four
 * or fewer window-ID bits they are padded above by the auxiliary segment
 * register; with more, the alignment bit picks the four least or most
 * significant of them.
 */
static uint8_t rad4_aux_wat_address(unsigned bits, uint8_t wid,
                                    uint8_t segment, bool msb)
{
    if (bits <= 4) {
        return rad4_wat_address(bits, wid, segment) & 0xf;
    }
    return msb ? (wid >> (bits - 4)) & 0xf : wid & 0xf;
}

static uint32_t rad4_lut_rgb(PsitechRad4State *s, uint16_t wat, uint8_t index)
{
    unsigned start_addr = (wat >> RAD4_WAT_START_ADDR_SHIFT) &
                          RAD4_WAT_START_ADDR_MASK;
    uint16_t lut_addr = (start_addr * 64 + index) & (RAD4_COLOR_LUT_COUNT - 1);
    const uint8_t *rgb = s->color_lut[lut_addr];

    return ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2];
}

/*
 * Widen an @bits-wide color component to 8 bits by repeating it from the most
 * significant end, as page 21's Table 18 does for every Direct RGB width:
 * 3 bits R2R1R0 become R2R1R0R2R1R0R2R1, 6 bits G5..G0 become G5..G0G5G4.
 */
static uint8_t rad4_widen(uint32_t value, unsigned bits)
{
    uint32_t out = 0;
    int shift = 8 - bits;

    value &= (1u << bits) - 1;
    for (; shift > -(int)bits; shift -= bits) {
        out |= shift >= 0 ? value << shift : value >> -shift;
    }
    return out & 0xff;
}

static uint32_t rad4_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/*
 * Interpret one raw 32-bit framebuffer word's frame-buffer layer, per its
 * FB_WAT entry (IBM RGB561 Architecture pages 9, 19-21 and 47).
 *
 * PIX_FORM selects the pixel width and BS which of the word's two buffers it
 * comes from: under the 4:1 basic serializer (Table 9) an 8-bit pixel is
 * bits 0-7 for buffer A and 8-15 for B, a 12-bit pixel 0-11 or 12-23 and a
 * 16-bit pixel 0-15 or 16-31; a 24-bit pixel is bits 0-23. Index mode looks
 * an 8-bit pixel up in the color LUT through Table 1's start-address
 * formula, LUT[START_ADDR * 64 + index]. Grey Scale mode presents an 8-bit
 * pixel to all three DAC inputs. True color mode bypasses the LUT: a 24-bit
 * pixel is already 0x00RRGGBB, and narrower pixels split into components at
 * Table 16's positions (R3G3B2, R4G4B4 and R5G6B5) and widen through Table
 * 18's bypass conversion.
 *
 * Index and Grey Scale pixels other than 8 bits, Direct RGB mode and buffer
 * B of a 24-bit pixel are not exercised by the qualified guest workload and
 * are reported once per scanout pass rather than guessed at. Components are
 * always in RGB order: the guest sets CONF/3's RGB bit, and this model does
 * not implement the BGR order it could select.
 */
static uint32_t rad4_fb_to_rgb(PsitechRad4State *s, Rad4WatLayout *l,
                               uint32_t word, uint8_t fb_addr)
{
    uint16_t wat = s->fb_wat[fb_addr];
    unsigned mode = (wat >> RAD4_WAT_MODE_SHIFT) & RAD4_WAT_MODE_MASK;
    unsigned pix_form = (wat >> RAD4_WAT_PIXFORM_SHIFT) &
                        RAD4_WAT_PIXFORM_MASK;
    bool buffer_b = wat & RAD4_WAT_BS;
    uint32_t pixel;

    switch (pix_form) {
    case RAD4_WAT_PIXFORM_8BPP:
        pixel = (word >> (buffer_b ? 8 : 0)) & 0xff;
        break;
    case RAD4_WAT_PIXFORM_12BPP:
        pixel = (word >> (buffer_b ? 12 : 0)) & 0xfff;
        break;
    case RAD4_WAT_PIXFORM_16BPP:
        pixel = (word >> (buffer_b ? 16 : 0)) & 0xffff;
        break;
    default:
        pixel = word & 0x00ffffffu;
        break;
    }

    if ((mode != RAD4_WAT_MODE_TRUE && mode != RAD4_WAT_MODE_INDEX &&
         mode != RAD4_WAT_MODE_GREY) ||
        (mode != RAD4_WAT_MODE_TRUE && pix_form != RAD4_WAT_PIXFORM_8BPP) ||
        (pix_form == RAD4_WAT_PIXFORM_24BPP && buffer_b)) {
        if (!l->fb_reported) {
            l->fb_reported = true;
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad4: FB_WAT entry 0x%02x = 0x%03x is "
                         "unimplemented; showing its low bits as 24-bit or "
                         "8-bit data\n", fb_addr, wat);
        }
    }

    if (mode == RAD4_WAT_MODE_TRUE) {
        switch (pix_form) {
        case RAD4_WAT_PIXFORM_8BPP:
            return rad4_rgb(rad4_widen(pixel >> 5, 3),
                            rad4_widen(pixel >> 2, 3),
                            rad4_widen(pixel, 2));
        case RAD4_WAT_PIXFORM_12BPP:
            return rad4_rgb(rad4_widen(pixel >> 8, 4),
                            rad4_widen(pixel >> 4, 4),
                            rad4_widen(pixel, 4));
        case RAD4_WAT_PIXFORM_16BPP:
            return rad4_rgb(rad4_widen(pixel >> 11, 5),
                            rad4_widen(pixel >> 5, 6),
                            rad4_widen(pixel, 5));
        default:
            return pixel;
        }
    }

    if (mode != RAD4_WAT_MODE_INDEX && mode != RAD4_WAT_MODE_GREY) {
        return word & 0x00ffffffu;
    }
    pixel &= 0xff;
    if (mode == RAD4_WAT_MODE_GREY) {
        return rad4_rgb(pixel, pixel, pixel);
    }
    return rad4_lut_rgb(s, wat, pixel);
}

/*
 * The overlay layer of one pixel, or false where the frame-buffer layer
 * shows through. IBM RGB561 Architecture pages 19-21, 46 and 48: overlay data
 * is displayed only where its AUX_OL_WAT entry enables the overlay and, when
 * its OL_WAT entry enables transparency, the data does not match the
 * AUX_OL_WAT CK/OT selection: X'00, X'FF, or chroma key 0 or 1 compared under
 * its mask. Index mode looks the data up in the color LUT at the OL_WAT
 * START_ADDR.
 *
 * The 8-bit overlay port is the pixel word's top byte. PIX_FORM 11, the
 * double-buffered 4-bit overlay, takes buffer A from its low four bits and
 * buffer B from its high four (Table 9 and Table 17). Transparency compares
 * the selected buffer's 4-bit value, shifted down, and the X'FF comparison
 * against 4-bit data uses the all-ones 4-bit value; the document says
 * neither. Overlay formats with an underlay, serializer modes other than 4:1
 * basic, the 16-bit and variable overlay ports and modes other than Index are
 * not implemented. They are reported once per scanout pass and leave the
 * frame-buffer layer showing.
 */
static bool rad4_overlay_to_rgb(PsitechRad4State *s, Rad4WatLayout *l,
                                uint32_t word, uint8_t ol_addr,
                                uint8_t aux_addr, uint32_t *rgb)
{
    uint8_t aux = s->rgb_store[RAD4_RGB_AUX_OL_WAT + aux_addr];
    uint16_t wat = s->ol_wat[ol_addr];
    unsigned pix_form = (wat >> RAD4_WAT_PIXFORM_SHIFT) &
                        RAD4_WAT_PIXFORM_MASK;
    unsigned mode = (wat >> RAD4_WAT_MODE_SHIFT) & RAD4_WAT_MODE_MASK;
    uint8_t byte = word >> RAD4_PIXEL_WID_SHIFT;
    uint8_t data, ones;

    if (l->overlay == RAD4_CONF1_OVLY_NONE || !(aux & RAD4_AUX_OL_WAT_OL)) {
        return false;
    }
    if (l->overlay != RAD4_CONF1_OVLY_8BIT ||
        (pix_form != RAD4_OL_WAT_PIXFORM_8BIT &&
         pix_form != RAD4_OL_WAT_PIXFORM_4BIT) ||
        (pix_form == RAD4_OL_WAT_PIXFORM_8BIT && (wat & RAD4_WAT_BS)) ||
        mode != RAD4_WAT_MODE_INDEX) {
        if (!l->overlay_reported) {
            l->overlay_reported = true;
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad4: overlay with CONF/1 0x%02x and "
                         "OL_WAT entry 0x%02x = 0x%03x is unimplemented; "
                         "showing the frame buffer\n",
                         s->rgb_store[RAD4_RGB_CONF1], ol_addr, wat);
        }
        return false;
    }

    if (pix_form == RAD4_OL_WAT_PIXFORM_4BIT) {
        data = (wat & RAD4_WAT_BS) ? byte >> 4 : byte & 0xf;
        ones = 0xf;
    } else {
        data = byte;
        ones = 0xff;
    }

    if (wat & RAD4_WAT_TR) {
        unsigned key = ((aux & RAD4_AUX_OL_WAT_CK) ? 2 : 0) |
                       ((aux & RAD4_AUX_OL_WAT_OT) ? 1 : 0);
        bool transparent;

        switch (key) {
        case 0:
            transparent = data == 0;
            break;
        case 1:
            transparent = data == ones;
            break;
        default: {
            uint8_t chroma = s->rgb_store[RAD4_RGB_CHROMA_KEY0 + key - 2];
            uint8_t mask = s->rgb_store[RAD4_RGB_CHROMA_MASK0 + key - 2];

            transparent = ((data ^ chroma) & mask) == 0;
            break;
        }
        }
        if (transparent) {
            return false;
        }
    }

    *rgb = rad4_lut_rgb(s, wat, data);
    return true;
}

/*
 * Pass a pixel's 8-bit components through the gamma tables, page 3: each
 * component addresses its own 256x10 table, whose 10-bit entry drives the
 * DAC. The display surface carries 8 bits per component, so this model
 * shows each entry's upper 8 bits. That matches the bypass path exactly,
 * which widens an 8-bit value to 10 bits by appending its 2 most
 * significant bits.
 */
static uint32_t rad4_gamma_rgb(PsitechRad4State *s, uint32_t rgb)
{
    return ((uint32_t)(s->gamma[0][(rgb >> 16) & 0xff] >> 2) << 16) |
           ((uint32_t)(s->gamma[1][(rgb >> 8) & 0xff] >> 2) << 8) |
           (s->gamma[2][rgb & 0xff] >> 2);
}

/*
 * One displayed pixel: the overlay layer where it shows, the frame-buffer
 * layer otherwise, then the gamma tables unless the layer's auxiliary WAT
 * entry bypasses them (page 46's GMA and GB bits). The hardware cursor is
 * composited afterwards and never passes through gamma, as page 3 requires.
 */
static uint32_t rad4_pixel_to_rgb(PsitechRad4State *s, Rad4WatLayout *l,
                                  uint32_t word)
{
    uint8_t wid = (word >> RAD4_PIXEL_WID_SHIFT) & l->lwid_mask;
    uint8_t ol_wid = (wid >> l->ol_shift) & rad4_wid_mask(l->ol_bits);
    uint8_t fb_addr = rad4_wat_address(l->fb_bits, wid, l->fb_segment);
    uint8_t ol_addr = rad4_wat_address(l->ol_bits, ol_wid, l->ol_segment);
    uint8_t aux_addr = rad4_aux_wat_address(l->ol_bits, ol_wid,
                                            l->aux_ol_segment, l->aux_ol_msb);
    uint8_t aux;
    uint32_t rgb;

    if (rad4_overlay_to_rgb(s, l, word, ol_addr, aux_addr, &rgb)) {
        aux = s->rgb_store[RAD4_RGB_AUX_OL_WAT + aux_addr];
    } else {
        aux_addr = rad4_aux_wat_address(l->fb_bits, wid, l->aux_fb_segment,
                                        l->aux_fb_msb);
        aux = s->rgb_store[RAD4_RGB_AUX_FB_WAT + aux_addr];
        rgb = rad4_fb_to_rgb(s, l, word, fb_addr);
    }
    if (aux & RAD4_AUX_WAT_GAMMA_BYPASS) {
        return rgb;
    }
    return rad4_gamma_rgb(s, rgb);
}

/*
 * The PLX part's DMA interrupt goes only to its local interrupt output
 * (PCI 9060 Data Sheet section 3.10.2, Table 45 bits 16 and 18); the data
 * sheet's own route to the host is that output wired to the local interrupt
 * input (section 3.6.1). This board is taken to be wired that way, which is
 * inferred from the shipped driver rather than a schematic: it enables the
 * DMA channel 0 interrupt only towards the local output (INTCSR bits 16 and
 * 18, DMA mode 0x7c3), and its interrupt handler looks for DMA completion,
 * INTCSR bit 21, only when the local input's status, bit 15, is set. The
 * board's frame event drives the same input.
 *
 * Bit 21 follows the channel's interrupt whether or not bit 18 enables it
 * towards the local output; Table 45 calls it the interrupt's status
 * without naming an enable, so that much is inferred.
 */
static void rad4_update_irq(PsitechRad4State *s)
{
    bool local_out = s->dma0_irq &&
                     (s->intcsr & RAD4_INTCSR_LOCAL_OUT_ENABLE) &&
                     (s->intcsr & RAD4_INTCSR_LOCAL_DMA0_ENABLE);
    bool level;

    s->intcsr &= ~RAD4_INTCSR_STATUS;
    if (s->frame_pending || local_out) {
        s->intcsr |= RAD4_INTCSR_PENDING;
    }
    if (s->dma0_irq) {
        s->intcsr |= RAD4_INTCSR_DMA0_ACTIVE;
    }
    level = (s->intcsr & RAD4_INTCSR_PENDING) &&
            (s->intcsr & RAD4_INTCSR_ENABLE) &&
            (s->intcsr & RAD4_INTCSR_LOCAL_IN_ENABLE);
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
    memset(s->rec_candidates, 0, sizeof(s->rec_candidates));
}

/* Mark [y_start, y_end] (inclusive, clipped to the visible image) dirty. */
static void rad4_mark_dirty_rows(PsitechRad4State *s, int y_start, int y_end)
{
    if (y_start < 0) {
        y_start = 0;
    }
    if (y_end >= rad4_height(s)) {
        y_end = rad4_height(s) - 1;
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
    /*
     * Replace rather than merge: after a mode change the old range can
     * reach below the new mode's last row.
     */
    s->dirty_y_start = 0;
    s->dirty_y_end = rad4_height(s) - 1;
}

/*
 * Mark the scanlines touched by a raw-framebuffer byte range dirty. offset
 * and len are relative to s->framebuffer;
 * a range outside the current mode's visible image (the RAD4_FB_SIZE
 * allocation is larger than any mode's stride times height) contributes
 * nothing, matching how rad4_convert_scanout()
 * never reads past the visible image
 * either.
 */
static void rad4_mark_dirty_range(PsitechRad4State *s, uint32_t offset,
                                  uint32_t len)
{
    uint32_t stride = rad4_stride(s);
    uint32_t visible = stride * rad4_height(s);

    if (len == 0 || offset >= visible) {
        return;
    }
    rad4_mark_dirty_rows(s, offset / stride, (offset + len - 1) / stride);
}

static void rad4_disable_scanout(PsitechRad4State *s)
{
    if (s->rec_state == RAD4_REC_RECOGNIZED) {
        timer_del(s->frame_timer);
    }
}

/*
 * A frame raises the board's frame event only while board-control bit 1
 * asks for one. That bit is read from the shipped driver, not a
 * schematic: it sets the bit, with a 400 ms fallback timer, whenever it
 * defers work to the next frame; its interrupt handler clears it when the
 * frame arrives; and before every DMA transfer it clears the bit if set and
 * restores it afterwards, because its handler does not service frames while
 * a transfer is in flight. A frame event the bit did not request would
 * leave that handler unable to clear the interrupt.
 *
 * The readable frame status (RAD4_STATUS_FRAME) follows the same request,
 * which is part of the same inference: nothing shows whether the board
 * latches the status of an unrequested frame and gates only the interrupt.
 * Nothing in the shipped software would see the difference. The only
 * driver code testing status bit 0x20 is its interrupt handler, which runs
 * on a requested frame. A scan of the standalone X server's and rad4d's
 * disassembly finds no instruction loading the control register's BAR2
 * offset (0xf00000) as a constant, so neither appears to poll it. Were
 * the status latched regardless and only the interrupt gated, setting bit
 * 1 to wait for a frame would raise a stale interrupt at once.
 */
static void rad4_frame_timer_cb(void *opaque)
{
    PsitechRad4State *s = opaque;

    if (s->rec_state != RAD4_REC_RECOGNIZED) {
        return;
    }

    if (s->bar2_ctrl_bit & RAD4_CTRL_STORED_BIT) {
        s->frame_pending = true;
        rad4_update_irq(s);
    }

    timer_mod(s->frame_timer,
             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
             rad4_frame_period(s));
}

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
    if (ds && surface_data(ds) == (uint8_t *)s->display_buffer &&
        surface_width(ds) == rad4_width(s) &&
        surface_height(ds) == rad4_height(s)) {
        return;
    }
    /*
     * A new mode reuses the same buffer, sized for the largest mode, at the
     * new geometry; display_buffer rows are packed at the mode's width.
     */
    ds = qemu_create_displaysurface_from(rad4_width(s), rad4_height(s),
                                         PIXMAN_x8r8g8b8,
                                         rad4_width(s) * 4,
                                         (uint8_t *)s->display_buffer);
    qemu_console_set_surface(s->con, ds);
}

/*
 * Event @pos of write sequence @q: board control, the PLL index and the
 * mode's two PLL bytes, the sequence's four timing banks expanded from
 * their runs, then the sync register, with the sequence's sync value, and
 * board control again. The two control writes are never compared through
 * here: rad4_recognizer_control() matches them on bit 0x20 alone.
 */
static void rad4_sequence_event(const Rad4ModeSequence *q, uint32_t pos,
                                uint32_t *offset, uint32_t *value)
{
    static const uint32_t prefix[][2] = {
        { RAD4_BAR2_CTRL, 0 },
        { RAD4_BAR2_RGB_LO, 0x21 },
        { RAD4_BAR2_RGB_HI, 0 },
    };
    static const uint32_t suffix_offset[] = {
        RAD4_BAR2_RGB_LO, RAD4_BAR2_RGB_HI, RAD4_BAR2_RGB_DATA, RAD4_BAR2_CTRL,
    };
    static const uint32_t bank_base[RAD4_MODE_BANKS] = {
        RAD4_BAR2_VERT, RAD4_BAR2_HORIZ0, RAD4_BAR2_HORIZ1, RAD4_BAR2_HORIZ3,
    };
    const Rad4ModeFixture *m = &rad4_modes[q->mode];
    uint32_t suffix_value[] = { 0x20, 0, q->sync, RAD4_CTRL_MODE_CMD };

    if (pos < ARRAY_SIZE(prefix)) {
        *offset = prefix[pos][0];
        *value = prefix[pos][1];
        return;
    }
    pos -= ARRAY_SIZE(prefix);
    if (pos < ARRAY_SIZE(m->pll)) {
        *offset = RAD4_BAR2_RGB_DATA;
        *value = m->pll[pos];
        return;
    }
    pos -= ARRAY_SIZE(m->pll);
    for (int b = 0; b < RAD4_MODE_BANKS; b++) {
        const Rad4TimingBank *bank = q->banks[b];

        if (pos >= bank->words) {
            pos -= bank->words;
            continue;
        }
        *offset = bank_base[b] + 4 * pos;
        for (int r = 0; r < bank->run_count; r++) {
            if (pos < bank->runs[r].count) {
                *value = bank->runs[r].value;
                return;
            }
            pos -= bank->runs[r].count;
        }
        g_assert_not_reached();
    }
    assert(pos < ARRAY_SIZE(suffix_offset));
    *offset = suffix_offset[pos];
    *value = suffix_value[pos];
}

static bool rad4_candidate(PsitechRad4State *s, uint32_t q)
{
    return s->rec_candidates[q / 64] & BIT_ULL(q % 64);
}

static void rad4_recognition_failed(PsitechRad4State *s, uint32_t offset,
                                    uint32_t value)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "psitech-rad4: mode recognition mismatch at "
                  "+0x%06x=0x%08x, failing until the next mode sequence\n",
                  offset, value);
    rad4_disable_scanout(s);
    s->rec_state = RAD4_REC_FAILED;
}

/*
 * The whole sequence of mode @mode has been seen: present its geometry and
 * start its frame schedule.
 */
static void rad4_mode_recognized(PsitechRad4State *s, uint32_t mode)
{
    s->mode = mode;
    s->rec_state = RAD4_REC_RECOGNIZED;
    s->cursor_shown_y_start = rad4_height(s);
    s->cursor_shown_y_end = -1;
    rad4_ensure_surface(s);
    rad4_mark_all_dirty(s);
    timer_mod(s->frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + rad4_frame_period(s));
}

/*
 * One event while collecting: keep the sequences that have exactly this
 * write at the current position. Each mode has eight distinct sequences,
 * one for each group of option words (EEPROM configuration bits 8, 10 and
 * 12-13) that the driver's timing loaders treat alike. Modes 4 and 11, and
 * 5 and 6, share PLL bytes and modes 2 and 3 share every timing bank, so a
 * single field never decides; only the whole sequence does. No candidate
 * left is a mismatch. The last event, board control, is matched by
 * rad4_recognizer_control().
 */
static void rad4_recognizer_collect(PsitechRad4State *s, uint32_t offset,
                                    uint32_t value)
{
    uint64_t keep[RAD4_CANDIDATE_WORDS] = { 0 };
    bool any = false;

    for (uint32_t q = 0; q < RAD4_SEQUENCE_COUNT; q++) {
        uint32_t expected_offset, expected_value;

        if (!rad4_candidate(s, q) ||
            s->rec_index + 1 >= rad4_sequences[q].event_count) {
            continue;
        }
        rad4_sequence_event(&rad4_sequences[q], s->rec_index,
                            &expected_offset, &expected_value);
        if (expected_offset == offset && expected_value == value) {
            keep[q / 64] |= BIT_ULL(q % 64);
            any = true;
        }
    }
    if (!any) {
        rad4_recognition_failed(s, offset, value);
        return;
    }
    memcpy(s->rec_candidates, keep, sizeof(keep));
    s->rec_index++;
}

/*
 * A write to board control at BAR2 +0xf00000. The driver keeps bit 0x20
 * set in its saved control word from the end of its first mode sequence
 * on. With one board, only the mode routine's opening write clears it, so
 * a write with bit 0x20 clear starts a mode sequence whatever its other
 * bits: bit
 * 1 when a palette or cursor update is waiting for the next frame, bit
 * 0x80 for the keyboard serial port. It arms recognition from any state,
 * including a recognized mode, whose scanout stops until the new
 * sequence completes, and including after a failed sequence. With two or
 * more boards the driver's open path also writes the word with bit 0x20
 * clear and then restores it; this model does not support that
 * configuration, and such an open stops the display.
 *
 * The same write may also have acknowledged a pending frame
 * (@acknowledged); a mode change requested while a frame is pending is
 * still a mode change. The sequence ends with the saved word with bit 0x20
 * set. While collecting, a write with bit 0x20 set completes a mode at
 * that sequence's last position. Anywhere else, the driver's interrupt
 * handler acknowledging a frame part-way through is not part of the
 * sequence and is ignored; any other control write fails recognition.
 */
static void rad4_recognizer_control(PsitechRad4State *s, uint32_t value,
                                    bool acknowledged)
{
    if (!(value & RAD4_CTRL_MODE_CMD)) {
        rad4_disable_scanout(s);
        s->rec_state = RAD4_REC_PREFIX_SEEN;
        s->rec_index = 0;
        return;
    }
    if (s->rec_state != RAD4_REC_COLLECTING) {
        return;
    }
    for (uint32_t q = 0; q < RAD4_SEQUENCE_COUNT; q++) {
        if (rad4_candidate(s, q) &&
            s->rec_index == rad4_sequences[q].event_count - 1u) {
            s->rec_index++;
            rad4_mode_recognized(s, rad4_sequences[q].mode);
            return;
        }
    }
    if (!acknowledged) {
        rad4_recognition_failed(s, RAD4_BAR2_CTRL, value);
    }
}

/*
 * Feed one recognition-relevant (address, value) event other than a board
 * control write (rad4_recognizer_control()) through the exact grammar of
 * the driver's mode sequences. Addresses are BAR2-relative.
 * The caller still applies the event's own register effect separately,
 * since recognition state and register storage are distinct.
 */
static void rad4_recognizer_feed(PsitechRad4State *s, uint32_t offset,
                                 uint32_t value)
{
    uint32_t arm_offset, arm_value;

    switch (s->rec_state) {
    case RAD4_REC_PREFIX_SEEN:
        /* The first write after arming is common to every sequence. */
        rad4_sequence_event(&rad4_sequences[0], 1, &arm_offset, &arm_value);
        if (s->rec_index == 0 && arm_offset == offset && arm_value == value) {
            s->rec_state = RAD4_REC_COLLECTING;
            s->rec_index = 2;
            for (uint32_t q = 0; q < RAD4_SEQUENCE_COUNT; q++) {
                s->rec_candidates[q / 64] |= BIT_ULL(q % 64);
            }
        } else {
            /* Not the expected next event: quietly returns to unarmed, no
             * failure logged. */
            s->rec_state = RAD4_REC_UNARMED;
        }
        break;
    case RAD4_REC_COLLECTING:
        rad4_recognizer_collect(s, offset, value);
        break;
    case RAD4_REC_UNARMED:
    case RAD4_REC_RECOGNIZED: {
        bool timing_bank = (offset >= RAD4_BAR2_VERT &&
                            offset < RAD4_BAR2_VERT + 4 * RAD4_VERT_WORDS) ||
                           (offset >= RAD4_BAR2_HORIZ0 &&
                            offset < RAD4_BAR2_HORIZ3 + 4 * RAD4_HORIZ_WORDS);
        bool mutating_rgb = offset == RAD4_BAR2_RGB_DATA &&
                             rad4_rgb_index_is_mode_mutating(
                                 rad4_rgb_index(s));

        if (timing_bank || mutating_rgb) {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad4: timing/PLL/sync write while %s, "
                         "failing until the next mode sequence\n",
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

    s->cursor_shown_y_start = rad4_height(s);
    s->cursor_shown_y_end = -1;
    if (!rad4_cursor_origin(s, &x0, &y0)) {
        return;
    }
    for (row = 0; row < RAD4_CURSOR_HEIGHT; row++) {
        int y = y0 + row;
        uint32_t *dst;

        if (y < y_start || y > y_end || y < 0 || y >= rad4_height(s)) {
            continue;
        }
        dst = s->display_buffer + (size_t)y * rad4_width(s);
        for (col = 0; col < RAD4_CURSOR_WIDTH; col++) {
            int x = x0 + col;
            uint8_t byte = s->rgb_store[RAD4_RGB_CURSOR_PIXMAP_BASE +
                                        row * RAD4_CURSOR_PIXMAP_STRIDE +
                                        col / 4];
            unsigned val = (byte >> (6 - (col % 4) * 2)) & 0x3;

            if (val == 0 || x < 0 || x >= rad4_width(s)) {
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
                                rad4_height(s) - 1);
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
    Rad4WatLayout layout;
    int width = rad4_width(s);
    int x, y;

    rad4_wat_layout(s, &layout);
    for (y = y_start; y <= y_end; y++) {
        uint32_t row = (uint32_t)y * rad4_stride(s);
        uint32_t *dst = s->display_buffer + (size_t)y * width;

        for (x = 0; x < width; x++) {
            uint32_t word = ldl_be_p(s->framebuffer + row + x * 4);

            *dst++ = rad4_pixel_to_rgb(s, &layout, word);
        }
    }
}

/*
 * Convert and push only the scanlines the guest actually touched since the
 * last refresh, instead of the whole
 * frame every ~30 Hz tick regardless of change. The old
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
    qemu_console_update(s->con, 0, s->dirty_y_start, rad4_width(s),
                        s->dirty_y_end - s->dirty_y_start + 1);
    s->dirty_y_start = rad4_height(s);
    s->dirty_y_end = -1;
    return true;
}

static const GraphicHwOps rad4_gfx_ops = {
    .gfx_update = rad4_gfx_update,
};

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

/* Write one EEPROM word through to the backing drive, if there is one. */
static void rad4_eeprom_flush_word(PsitechRad4State *s, unsigned address)
{
    uint8_t buf[2];

    if (!s->eeprom_blk) {
        return;
    }
    stw_be_p(buf, s->eeprom[address]);
    if (blk_pwrite(s->eeprom_blk, address * 2, sizeof(buf), buf, 0) < 0) {
        error_report("psitech-rad4: failed to write EEPROM word 0x%02x to "
                     "its backing drive", address);
    }
}

static void rad4_eeprom_decode(PsitechRad4State *s)
{
    unsigned opcode = s->serial_command_shift >> RAD4_EEPROM_ADDR_BITS;

    unsigned extended;

    s->eeprom_address = s->serial_command_shift & RAD4_EEPROM_ADDR_MASK;
    extended = s->eeprom_address >> (RAD4_EEPROM_ADDR_BITS - 2);
    s->serial_bit_count = 0;
    s->serial_state = RAD4_SERIAL_DONE;

    if (opcode == RAD4_EEPROM_OP_READ) {
        s->serial_state = RAD4_SERIAL_READ_DUMMY;
    } else if (opcode == RAD4_EEPROM_OP_WRITE) {
        s->eeprom_data = 0;
        s->serial_state = RAD4_SERIAL_WRITE_DATA;
    } else if (opcode == RAD4_EEPROM_OP_EXTENDED &&
               extended == RAD4_EEPROM_EXT_EWDS) {
        s->eeprom_write_enabled = false;
    } else if (opcode == RAD4_EEPROM_OP_EXTENDED &&
               extended == RAD4_EEPROM_EXT_EWEN) {
        s->eeprom_write_enabled = true;
    } else {
        /* ERASE, ERAL and WRAL: the driver never sends them. */
        qemu_log_mask(LOG_UNIMP,
                      "psitech-rad4: EEPROM command 0x%02x is unimplemented\n",
                      s->serial_command_shift);
    }
}

/*
 * One write to the serial register: chip select, clock and data-in as the
 * guest drives them. Data-in is sampled on rising clock edges and data-out
 * changes on falling ones, where the driver reads it.
 */
static void rad4_serial_edge(PsitechRad4State *s, bool cs, bool clk,
                             bool mosi)
{
    bool clk_rising = clk && !s->serial_clk_prev;
    bool clk_falling = !clk && s->serial_clk_prev;

    if (!s->serial_cs_prev && cs) {
        rad4_serial_reset(s);
        s->serial_state = RAD4_SERIAL_START;
    }
    if (s->serial_cs_prev && !cs) {
        if (s->serial_state == RAD4_SERIAL_WRITE_READY) {
            if (s->eeprom_write_enabled) {
                s->eeprom[s->eeprom_address] = s->eeprom_data;
                rad4_eeprom_flush_word(s, s->eeprom_address);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "psitech-rad4: EEPROM write to word 0x%02x "
                              "without write enable, ignored\n",
                              s->eeprom_address);
            }
        }
        rad4_serial_reset(s);
    }

    if (cs && clk_rising) {
        switch (s->serial_state) {
        case RAD4_SERIAL_START:
            if (mosi) {
                s->serial_command_shift = 0;
                s->serial_bit_count = 0;
                s->serial_state = RAD4_SERIAL_COMMAND;
            }
            break;
        case RAD4_SERIAL_COMMAND:
            s->serial_command_shift = (s->serial_command_shift << 1) | mosi;
            if (++s->serial_bit_count == RAD4_EEPROM_CMD_BITS) {
                rad4_eeprom_decode(s);
            }
            break;
        case RAD4_SERIAL_WRITE_DATA:
            s->eeprom_data = (s->eeprom_data << 1) | mosi;
            if (++s->serial_bit_count == 16) {
                s->serial_state = RAD4_SERIAL_WRITE_READY;
            }
            break;
        default:
            break;
        }
    }

    if (cs && clk_falling) {
        switch (s->serial_state) {
        case RAD4_SERIAL_READ_DUMMY:
            s->serial_miso = false;
            s->serial_bit_count = 0;
            s->serial_state = RAD4_SERIAL_READ;
            break;
        case RAD4_SERIAL_READ:
            s->serial_miso = (s->eeprom[s->eeprom_address] >>
                              (15 - s->serial_bit_count)) & 1;
            if (++s->serial_bit_count == 16) {
                s->serial_bit_count = 0;
                s->eeprom_address = (s->eeprom_address + 1) &
                                    RAD4_EEPROM_ADDR_MASK;
            }
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
        if (val & RAD4_INTCSR_SERR) {
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad4: INTCSR SERR# generation not "
                         "modelled\n");
        }
        s->intcsr = (s->intcsr & RAD4_INTCSR_STATUS) |
                    (val & RAD4_INTCSR_WRITABLE);
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
 * DMA channel 0 runs alongside the processor, as the PLX part does: the
 * DMACSR0 store that starts it only latches the transfer, and a virtual-clock
 * timer moves it (the pattern of hw/dma/pl330.c). Each expiry moves what
 * the bus could have moved since the last one, at least RAD4_DMA_SLICE
 * bytes and at most RAD4_DMA_CATCHUP, and runs again at once while still
 * behind, so time the host spends elsewhere between callbacks is made up
 * rather than added to the transfer. That is all the catch-up does: it
 * cannot copy faster than the host can, and a loaded host copies far below
 * the bus rate, so a transfer still takes longer in virtual time than on
 * the bus.
 *
 * Stretched transfers matter. The shipped driver gives a transfer
 * size / 20 + 400000 microseconds, size in bytes (about 0.41 s for a
 * 200 KB chain), before it aborts it, and its abort path then deadlocks a
 * uniprocessor guest: it polls for Done while holding the spinlock the
 * completion interrupt needs (driver object listing offsets 0x9eb0-0x9f2c
 * for the timeout, 0x9f4c-0xa0dc for the abort). Real hardware, finishing
 * a 200 KB chain in about 1.5 ms, never gets there.
 *
 * RAD4_DMA_CATCHUP bounds one callback's work, as hw/dma/i8257.c bounds
 * each run of its bottom half, at the cost of how long that callback holds
 * the BQL. 256 KiB covers one of the driver's usual 40-row chains, about
 * 200 KB, in one callback. In one paired run of IRIX 6.5.30's buffered X
 * server on a host at load about 120 on 32 CPUs, against a 64 KiB cap,
 * the 99th-percentile hold rose from 25 ms to 54 ms, but the slowest
 * transfer fell from 324 ms to 221 ms and the closest any transfer came
 * to the driver's timeout widened from 86 ms to 184 ms. A stall is
 * preferable to a deadlocked guest.
 *
 * The rate is an approximation: the PCI 9060 Data Sheet (Version 1.2,
 * December 1995, page 1) gives "PCI Bus Master transfers up to 132
 * MBytes/sec", one 32-bit dword per clock at its 33 MHz maximum PCI clock
 * (page 71's AC characteristics). Arbitration, wait states, the Bridge's own
 * latency and the board's local bus are not modelled, so this is the
 * chip's upper bound, not a measured transfer time. A descriptor fetch is
 * charged as 16 bytes of bus time, the 16 bytes it reads (inferred: the
 * data sheet gives no fetch timing).
 */
#define RAD4_DMA_RATE           132000000   /* bytes per second */
#define RAD4_DMA_DESC_BYTES     16
#define RAD4_DMA_SLICE          4096
#define RAD4_DMA_CATCHUP        (256 * KiB)
#define RAD4_DMA_SLOT_NS \
    ((int64_t)RAD4_DMA_SLICE * NANOSECONDS_PER_SECOND / RAD4_DMA_RATE)

/*
 * DMACSR0 bits a read returns (PCI 9060 Data Sheet Table 57): each channel's
 * enable and user-defined bits as written, and its Done bit. Start, abort
 * and clear-interrupt are write-only. Channel 1 is not modelled; its bits
 * are kept for readback only.
 */
#define RAD4_DMA_CSR_STORED     0xe1e1u
#define RAD4_DMA_CH1_CONTROL    0x0e00u
#define RAD4_DMA_CH1_DONE       (1u << 12)

/*
 * Byte order of DMA data, decided here and nowhere else. The PLX part has
 * no byte-order control and wires PCI byte lanes straight to local ones
 * (PCI 9060 Data Sheet page 18: BE0#-BE3# select LD[7:0]-LD[31:24]), so
 * local byte address A is PCI byte lane A % 4 of its dword: the dword's
 * least significant byte at the lowest address, little endian. This model
 * keeps each local dword in s->framebuffer as the value a CPU store through
 * BAR2 writes, most significant byte first, so local byte A lives at
 * framebuffer[A ^ 3].
 */
static uint32_t rad4_dma_local_byte(uint32_t local_addr)
{
    return local_addr ^ 3;
}

/*
 * Merge @len bytes from PCI into the framebuffer at local byte address
 * @local_addr, each under its own byte lane of the plane mask
 * (RAD4_BAR2_MASK). The channel moves any byte count between any two byte
 * addresses and writes only the bytes inside the span (PCI 9060 Data Sheet
 * page 6, section 2.2.2.5 on page 9 and section 3.6.3.3 on page 26), so
 * the bytes either side of an unaligned span keep their values.
 *
 * That the plane mask applies to DMA at all is inferred, not documented
 * for the board: the mask register is programmed "before CPU
 * transfer/fill and restored afterward" and both driver DMA render paths
 * (bigx_update and rad4_rect_dma_move/fill/copy) share the threshold logic
 * that chooses between CPU and DMA, so it is taken to be the memory
 * controller's masked write rather than a CPU-only feature. Applying it per
 * byte lane for a partial dword is inferred the same way. The mask in force
 * when the data arrives applies. The overlay-index rule of
 * rad4_masked_store() is a CPU-store compatibility rule and never applies
 * here.
 */
static void rad4_dma_merge(PsitechRad4State *s, uint32_t local_addr,
                           const uint8_t *buf, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        uint32_t addr = local_addr + i;
        uint8_t mask = s->mask >> (8 * (addr % 4));
        uint8_t *dst = s->framebuffer + rad4_dma_local_byte(addr);

        *dst = (*dst & ~mask) | (buf[i] & mask);
    }
    rad4_mark_dirty_range(s, local_addr, len);
}

/*
 * Move @len bytes, at most RAD4_DMA_SLICE, between host memory and the
 * framebuffer, from any byte address to any other. One PCI access covers
 * the whole span; the Bridge still translates each dword of it. If it
 * fails, the span is redone one PCI dword at a time so the transfer stops
 * with exactly the bytes before the failing dword moved.
 */
static bool rad4_dma_move(PsitechRad4State *s, bool local_to_pci,
                          uint32_t pci_addr, uint32_t local_addr,
                          uint32_t len)
{
    uint8_t buf[RAD4_DMA_SLICE];
    uint32_t off, piece;

    assert(len <= sizeof(buf));
    if (local_to_pci) {
        for (off = 0; off < len; off++) {
            buf[off] = s->framebuffer[rad4_dma_local_byte(local_addr + off)];
        }
        if (pci_dma_write(&s->pci, pci_addr, buf, len) == MEMTX_OK) {
            return true;
        }
        for (off = 0; off < len; off += piece) {
            piece = MIN(len - off, 4 - (pci_addr + off) % 4);
            if (pci_dma_write(&s->pci, pci_addr + off, buf + off,
                              piece) != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                             "psitech-rad4: DMA readback write failed at "
                             "0x%x\n", pci_addr + off);
                return false;
            }
        }
        return true;
    }

    if (pci_dma_read(&s->pci, pci_addr, buf, len) == MEMTX_OK) {
        rad4_dma_merge(s, local_addr, buf, len);
        return true;
    }
    for (off = 0; off < len; off += piece) {
        piece = MIN(len - off, 4 - (pci_addr + off) % 4);
        if (pci_dma_read(&s->pci, pci_addr + off, buf + off,
                         piece) != MEMTX_OK) {
            rad4_dma_merge(s, local_addr, buf, off);
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad4: DMA payload read failed at 0x%x\n",
                         pci_addr + off);
            return false;
        }
    }
    rad4_dma_merge(s, local_addr, buf, len);
    return true;
}

/*
 * Load the descriptor s->dma_next names. A descriptor is four 32-bit words
 * in local memory, PCI address, local address, transfer size and
 * next-descriptor pointer (PCI 9060 Data Sheet section 3.6.2 and figure
 * 6), read as the values the CPU stored through BAR2: the channel loads
 * them into its own registers (sections 3.6.3.1 and 3.6.3.2), so the
 * next-pointer word becomes DMADPR0. Its flags describe the descriptor
 * that holds them (section 3.10.2.3, Table 51): bit 1 ends the chain after
 * it, bit 2 asks for an interrupt once its transfer completes, and bit 3
 * picks its direction. The shipped driver relies on this, putting each
 * descriptor's direction in its own next-pointer word and end of chain
 * with interrupt (0x6) in the last one's.
 *
 * Descriptors always come from local memory. Pointer bit 0 is reserved on
 * the 9060 (Table 51); the PCI 9054 later used it to fetch descriptors from
 * host memory, which the shipped driver never asks for. A pointer with the
 * bit set is logged and the bit ignored, which is inferred: the data sheet
 * does not say what the part does with a reserved bit set.
 *
 * BAR2's framebuffer memory is this model's only locally addressable
 * storage. A live capture of an IRIX 6.5.30 buffered-mode transfer
 * confirmed the shipped 1.101 driver builds its descriptor array there, at
 * the top of the 8 MiB local extent.
 *
 * A descriptor this model cannot honour (a pointer outside local memory or
 * a span outside the framebuffer) stops the channel with Done clear; the
 * guest's own timeout and abort recovery then runs.
 */
static bool rad4_dma_load_desc(PsitechRad4State *s)
{
    uint32_t ptr = s->dma_next;
    hwaddr desc_addr = ptr & RAD4_DMA_DESC_ADDR_MASK;
    uint32_t word[4];
    int i;

    if (s->dma_descs >= RAD4_DMA_MAX_DESCRIPTORS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                     "psitech-rad4: DMA descriptor chain exceeded %" PRId64
                     " entries without an end-of-chain descriptor\n",
                     (int64_t)RAD4_DMA_MAX_DESCRIPTORS);
        return false;
    }
    s->dma_descs++;

    if (ptr & RAD4_DMA_DESC_RESERVED) {
        qemu_log_mask(LOG_GUEST_ERROR,
                     "psitech-rad4: DMA descriptor pointer 0x%x sets "
                     "reserved bit 0; reading the descriptor from local "
                     "memory\n", ptr);
    }
    if (desc_addr >= RAD4_FB_SIZE ||
        RAD4_DMA_DESC_BYTES > RAD4_FB_SIZE - desc_addr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                     "psitech-rad4: DMA descriptor at Local address "
                     "0x%"HWADDR_PRIx" falls outside the "
                     "framebuffer\n", desc_addr);
        return false;
    }
    for (i = 0; i < 4; i++) {
        word[i] = ldl_be_p(s->framebuffer + desc_addr + 4 * i);
    }

    s->dma_pci = word[0];
    s->dma_local = word[1];
    s->dma_size = word[2] & RAD4_DMA_SIZE_MASK;
    s->dma_next = word[3];
    s->dma_moved = 0;
    s->rom_desc = word[3];

    if (s->dma_local >= RAD4_FB_SIZE ||
        s->dma_size > RAD4_FB_SIZE - s->dma_local) {
        qemu_log_mask(LOG_GUEST_ERROR,
                     "psitech-rad4: DMA descriptor at 0x%"HWADDR_PRIx
                     " targets local address 0x%x size 0x%x outside "
                     "the framebuffer\n", desc_addr, s->dma_local,
                     s->dma_size);
        return false;
    }
    s->dma_have_desc = true;
    return true;
}

/* Run the channel's timer exactly while it is running and enabled. */
static void rad4_dma_schedule(PsitechRad4State *s)
{
    if (!s->dma_running || !(s->rom_command & RAD4_DMA_CMD_ENABLE)) {
        timer_del(s->dma_timer);
    } else if (!timer_pending(s->dma_timer)) {
        /* Starting or resuming: bus time counts from now. */
        s->dma_paid_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_mod(s->dma_timer, s->dma_paid_ns + RAD4_DMA_SLOT_NS);
    }
}

static void rad4_dma_stop(PsitechRad4State *s)
{
    s->dma_running = false;
    s->dma_have_desc = false;
    timer_del(s->dma_timer);
}

/*
 * The current descriptor has moved its last byte. Its own interrupt request
 * needs no mode bit: the mode register enables only the done interrupt (PCI
 * 9060 Data Sheet section 3.10.2.3). The done interrupt follows the mode
 * register as it stands when the chain ends: the driver's timeout path
 * clears that bit to stop an interrupt from a transfer it gave up on.
 */
static void rad4_dma_desc_done(PsitechRad4State *s)
{
    s->dma_have_desc = false;
    if (s->dma_next & RAD4_DMA_DESC_INTR_ON_TC) {
        s->dma0_irq = true;
    }
    if (s->dma_next & RAD4_DMA_DESC_END_OF_CHAIN) {
        rad4_dma_stop(s);
        trace_rad4_dma_done(s->dma_descs);
        s->rom_command |= RAD4_DMA_STATUS_DONE;
        if (s->rom_mode & RAD4_DMA_MODE_DONE_IRQ_ENABLE) {
            s->dma0_irq = true;
        }
    }
    rad4_update_irq(s);
}

/*
 * Bus time has passed since the last expiry: move what fits in it.
 *
 * QEMU's MMIO reentrancy guard does not cover timer context, so a transfer
 * aimed at this card's own BARs could reach rad4_dma_command_write() from
 * inside rad4_dma_move(). A stop or start there clears dma_have_desc; the
 * loop then neither counts the move against the descriptor it dropped nor
 * completes it, and either ends or loads a fresh descriptor.
 */
static void rad4_dma_timer_cb(void *opaque)
{
    PsitechRad4State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t owed = muldiv64(MAX(now - s->dma_paid_ns, 0), RAD4_DMA_RATE,
                            NANOSECONDS_PER_SECOND);
    uint32_t budget = MIN(MAX(owed, RAD4_DMA_SLICE), RAD4_DMA_CATCHUP);
    uint32_t start = budget;

    while (s->dma_running && budget) {
        uint32_t len;

        if (!s->dma_have_desc) {
            if (!rad4_dma_load_desc(s)) {
                rad4_dma_stop(s);
                return;
            }
            budget -= MIN(budget, RAD4_DMA_DESC_BYTES);
            continue;
        }
        len = MIN(MIN(budget, s->dma_size - s->dma_moved), RAD4_DMA_SLICE);
        if (len &&
            !rad4_dma_move(s, s->dma_next & RAD4_DMA_DESC_LOCAL_TO_PCI,
                           s->dma_pci + s->dma_moved,
                           s->dma_local + s->dma_moved, len)) {
            rad4_dma_stop(s);
            return;
        }
        if (!s->dma_have_desc) {
            continue;
        }
        s->dma_moved += len;
        budget -= len;
        if (s->dma_moved == s->dma_size) {
            rad4_dma_desc_done(s);
        }
    }
    if (!s->dma_running) {
        return;
    }
    s->dma_paid_ns += muldiv64(start - budget, NANOSECONDS_PER_SECOND,
                               RAD4_DMA_RATE);
    /* Still behind after a capped run: go again at once. */
    timer_mod(s->dma_timer, MAX(s->dma_paid_ns + RAD4_DMA_SLOT_NS, now));
}

/*
 * DMACSR0 (RAD4_ROM_COMMAND) write side, per PCI 9060 Data Sheet Table 57.
 * Start (bit 1) begins a transfer if enable (bit 0) is set, clearing Done
 * (bit 4) until the chain ends. Clearing enable while a transfer runs
 * suspends it ("Pause"); setting it again resumes it, which the table
 * implies but does not state. Abort (bit 2) needs enable clear in the same
 * write and sets Done once the channel has stopped; an abort written with
 * enable set has no stated effect, so it is logged and ignored. Bit 3
 * clears the channel's interrupt.
 *
 * The shipped driver writes ENABLE|START (0x3), sleeps until the
 * interrupt or its own timeout, and on timeout writes ABORT (0x4) and polls
 * Done; its interrupt handler acknowledges with Done and clear-interrupt
 * set and enable clear.
 */
static void rad4_dma_command_write(PsitechRad4State *s, uint32_t val)
{
    bool enable = val & RAD4_DMA_CMD_ENABLE;

    if (val & RAD4_DMA_CH1_CONTROL) {
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: DMA channel 1 command 0x%x not "
                     "modelled\n", val);
    }
    s->rom_command = (s->rom_command &
                      (RAD4_DMA_STATUS_DONE | RAD4_DMA_CH1_DONE)) |
                     (val & RAD4_DMA_CSR_STORED);

    if (val & RAD4_DMA_CMD_CLEAR_INT) {
        s->dma0_irq = false;
        rad4_update_irq(s);
    }

    if ((val & RAD4_DMA_CMD_ABORT) && enable) {
        qemu_log_mask(LOG_GUEST_ERROR,
                     "psitech-rad4: DMA abort written with the channel "
                     "enabled; ignored\n");
    } else if (val & RAD4_DMA_CMD_ABORT) {
        rad4_dma_stop(s);
        s->rom_command |= RAD4_DMA_STATUS_DONE;
    }

    if (enable && (val & RAD4_DMA_CMD_START)) {
        if (s->dma_running) {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad4: DMA start while a transfer is in "
                         "progress; ignored\n");
        } else {
            s->rom_command &= ~RAD4_DMA_STATUS_DONE;
            s->dma_running = true;
            s->dma_have_desc = false;
            s->dma_next = s->rom_desc;
            s->dma_descs = 0;
            trace_rad4_dma_start(s->rom_desc);
        }
    }
    rad4_dma_schedule(s);
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
         * look-up table are modeled, along with the overlay window attribute
         * table and the gamma tables.
         */
        uint16_t index = rad4_rgb_index(s);
        uint8_t val = 0;

        if (rad4_index_is_cursor_lut(index)) {
            val = s->cursor_lut[index - RAD4_RGB_CURSOR_LUT_BASE]
                               [s->rgb_reserved_phase];
            rad4_rgb_reserved_advance(s, index, 3);
        } else if (rad4_ten_bit_entry(s, index)) {
            uint16_t wat = *rad4_ten_bit_entry(s, index);

            /*
             * Tables 2 and 5, p.3-4: cycle 0 is bits 9-2, cycle 1 is bits
             * 1-0.
             */
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

        stl_be_p(s->framebuffer + offset, rad4_masked_store(s, old, v));
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
        } else if (rad4_rgb_index_is_display_attribute(index)) {
            /*
             * Same reasoning as an FB_WAT write: these registers change how
             * existing frame buffer data is displayed.
             */
            rad4_mark_all_dirty(s);
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
        } else if (rad4_ten_bit_entry(s, index)) {
            uint16_t *wat = rad4_ten_bit_entry(s, index);

            /*
             * Tables 2 and 5, p.3-4: cycle 0 is bits 9-2, cycle 1 is bits
             * 1-0.
             */
            if (s->rgb_reserved_phase == 0) {
                *wat = (uint16_t)(v & 0xff) << RAD4_WAT_CYCLE0_SHIFT;
            } else {
                *wat |= (v >> RAD4_WAT_CYCLE1_SHIFT) & RAD4_WAT_CYCLE1_MASK;
            }
            rad4_rgb_reserved_advance(s, index, RAD4_WAT_CYCLES);
            /*
             * A window's FB_WAT or OL_WAT entry selects how
             * rad4_pixel_to_rgb() interprets every pixel tagged with that
             * window ID, and a gamma entry changes every pixel with that
             * component value. That changes what the existing scanout data
             * means without touching s->framebuffer, so the raw-write dirty
             * tracking above never sees it. Repaint the whole frame once
             * this entry is fully written.
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
    case RAD4_BAR2_CTRL: {
        bool acknowledged = s->frame_pending &&
                            (v & RAD4_CTRL_STORED_BIT) == 0;

        if (acknowledged) {
            s->frame_pending = false;
            rad4_update_irq(s);
        }
        s->bar2_ctrl_bit = v & RAD4_CTRL_STORED_BIT;
        rad4_recognizer_control(s, v, acknowledged);
        break;
    }
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
    /*
     * No mode is recognized yet; show mode 0's geometry until one is, and
     * blank it: display_buffer still holds the previous mode's rows packed
     * at that mode's width, and nothing converts the cleared framebuffer
     * into it until a mode is recognized.
     */
    s->mode = 0;
    memset(s->display_buffer, 0, rad4_max_pixels() * sizeof(uint32_t));

    s->reg14 = 0;
    s->reg18 = 0;
    s->mailbox = 0;
    s->intcsr = RAD4_INTCSR_RESET;
    s->dma0_irq = false;
    s->serial = 0;
    rad4_serial_reset(s);
    s->serial_clk_prev = false;
    s->serial_cs_prev = false;
    /* The part powers up write-disabled; its contents are not reset. */
    s->eeprom_address = 0;
    s->eeprom_data = 0;
    s->eeprom_write_enabled = false;

    s->rom_desc = 0;
    s->rom_mode = 0;
    /* PCI 9060 Data Sheet Table 57: both channels' Done bits reset set. */
    s->rom_command = RAD4_DMA_STATUS_DONE | RAD4_DMA_CH1_DONE;
    rad4_dma_stop(s);

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
     * deterministic post-reset state. The gamma tables are the exception:
     * zero there would blank every pixel.
     */
    memset(s->fb_wat, 0, sizeof(s->fb_wat));
    memset(s->ol_wat, 0, sizeof(s->ol_wat));
    /*
     * The gamma tables start as the linear ramp the bypass path produces,
     * so a guest that never loads them sees its colors unchanged rather
     * than a black screen.
     */
    for (int i = 0; i < RAD4_GAMMA_COUNT; i++) {
        uint16_t linear = (i << 2) | (i >> 6);

        s->gamma[0][i] = s->gamma[1][i] = s->gamma[2][i] = linear;
    }
    memset(s->color_lut, 0, sizeof(s->color_lut));
    memset(s->cursor_lut, 0, sizeof(s->cursor_lut));
    s->rgb_reserved_phase = 0;

    memset(s->vert_timing, 0, sizeof(s->vert_timing));
    memset(s->horiz_timing, 0, sizeof(s->horiz_timing));

    s->bar2_ctrl_bit = 0;
    s->frame_pending = false;

    rad4_recognizer_reset(s);
    rad4_ensure_surface(s);
    qemu_console_update(s->con, 0, 0, rad4_width(s), rad4_height(s));

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
    s->cursor_shown_y_start = rad4_height(s);
    s->cursor_shown_y_end = -1;
}

/*
 * Load the EEPROM from its backing drive: a raw 128-byte 93C46 image, each
 * 16-bit word stored most significant byte first, the order the part shifts
 * it out. Without a drive the EEPROM starts all zero, which the driver reads
 * as mode zero with every option bit clear.
 */
static bool rad4_eeprom_realize(PsitechRad4State *s, Error **errp)
{
    uint8_t image[RAD4_EEPROM_BYTES];
    int64_t len;

    if (!s->eeprom_blk) {
        return true;
    }
    /* blk_getlength() rounds up to a whole sector, so only check a floor. */
    len = blk_getlength(s->eeprom_blk);
    if (len < RAD4_EEPROM_BYTES) {
        error_setg(errp, "psitech-rad4: EEPROM image must hold %d bytes",
                   RAD4_EEPROM_BYTES);
        return false;
    }
    if (blk_set_perm(s->eeprom_blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_ALL, errp) < 0) {
        return false;
    }
    if (blk_pread(s->eeprom_blk, 0, sizeof(image), image, 0) < 0) {
        error_setg(errp, "psitech-rad4: failed to read the EEPROM image");
        return false;
    }
    for (int i = 0; i < RAD4_EEPROM_WORDS; i++) {
        s->eeprom[i] = lduw_be_p(image + i * 2);
    }
    return true;
}

static void rad4_realize(PCIDevice *dev, Error **errp)
{
    PsitechRad4State *s = PSITECH_RAD4(dev);
    Object *obj = OBJECT(dev);

    if (!rad4_eeprom_realize(s, errp)) {
        return;
    }
    s->framebuffer = g_malloc0(RAD4_FB_SIZE);
    s->rgb_store = g_malloc0(0x10000);
    s->display_buffer = g_new0(uint32_t, rad4_max_pixels());

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
    s->dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rad4_dma_timer_cb, s);

    rad4_ensure_surface(s);
}

static void rad4_exit(PCIDevice *dev)
{
    PsitechRad4State *s = PSITECH_RAD4(dev);

    timer_free(s->frame_timer);
    timer_free(s->dma_timer);
    qemu_graphic_console_close(s->con);
    g_free(s->framebuffer);
    g_free(s->rgb_store);
    g_free(s->display_buffer);
}

/*
 * Whether migrated state that indexes arrays or feeds shifts is in range:
 * every state the device itself can reach passes. A completed recognition
 * leaves rec_index equal to its sequence's length, so the longest sequence's
 * length is itself a valid value.
 */
static bool rad4_migrated_state_valid(PsitechRad4State *s)
{
    uint32_t longest = 0;

    for (int q = 0; q < RAD4_SEQUENCE_COUNT; q++) {
        longest = MAX(longest, rad4_sequences[q].event_count);
    }
    if (s->mode >= RAD4_MODE_COUNT ||
        s->rec_state < RAD4_REC_UNARMED || s->rec_state > RAD4_REC_FAILED ||
        s->rec_index > longest ||
        s->eeprom_address >= RAD4_EEPROM_WORDS ||
        s->rgb_reserved_phase >= RAD4_COLOR_LUT_CYCLES) {
        return false;
    }
    if (s->dma_have_desc &&
        (s->dma_local >= RAD4_FB_SIZE ||
         s->dma_size > RAD4_FB_SIZE - s->dma_local ||
         s->dma_moved > s->dma_size)) {
        return false;
    }
    switch (s->serial_state) {
    case RAD4_SERIAL_IDLE:
    case RAD4_SERIAL_START:
    case RAD4_SERIAL_READ_DUMMY:
    case RAD4_SERIAL_WRITE_READY:
    case RAD4_SERIAL_DONE:
        return true;
    case RAD4_SERIAL_COMMAND:
        return s->serial_bit_count < RAD4_EEPROM_CMD_BITS;
    case RAD4_SERIAL_READ:
    case RAD4_SERIAL_WRITE_DATA:
        return s->serial_bit_count < 16;
    default:
        return false;
    }
}

static int rad4_post_load(void *opaque, int version_id)
{
    PsitechRad4State *s = opaque;

    if (!rad4_migrated_state_valid(s)) {
        return -EINVAL;
    }

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
    s->cursor_shown_y_start = rad4_height(s);
    s->cursor_shown_y_end = -1;
    rad4_ensure_surface(s);
    rad4_update_irq(s);
    /* A transfer in flight resumes with the next slot of bus time. */
    timer_del(s->dma_timer);
    rad4_dma_schedule(s);
    if (s->rec_state == RAD4_REC_RECOGNIZED && s->frame_timer &&
        !timer_pending(s->frame_timer)) {
        timer_mod(s->frame_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 rad4_frame_period(s));
    }
    return 0;
}

static const VMStateDescription vmstate_psitech_rad4 = {
    .name = TYPE_PSITECH_RAD4,
    .version_id = 10,
    .minimum_version_id = 10,
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
        VMSTATE_UINT16_ARRAY(eeprom, PsitechRad4State, RAD4_EEPROM_WORDS),
        VMSTATE_UINT8(eeprom_address, PsitechRad4State),
        VMSTATE_UINT16(eeprom_data, PsitechRad4State),
        VMSTATE_BOOL(eeprom_write_enabled, PsitechRad4State),
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
        VMSTATE_UINT16_ARRAY(ol_wat, PsitechRad4State, RAD4_WAT_COUNT),
        VMSTATE_UINT16_2DARRAY(gamma, PsitechRad4State, 3, RAD4_GAMMA_COUNT),
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
        VMSTATE_UINT64_ARRAY(rec_candidates, PsitechRad4State,
                             RAD4_CANDIDATE_WORDS),
        VMSTATE_UINT32(mode, PsitechRad4State),
        VMSTATE_BOOL(km_irq_level, PsitechRad4State),
        VMSTATE_BOOL(dma0_irq, PsitechRad4State),
        VMSTATE_BOOL(dma_running, PsitechRad4State),
        VMSTATE_BOOL(dma_have_desc, PsitechRad4State),
        VMSTATE_UINT32(dma_pci, PsitechRad4State),
        VMSTATE_UINT32(dma_local, PsitechRad4State),
        VMSTATE_UINT32(dma_size, PsitechRad4State),
        VMSTATE_UINT32(dma_moved, PsitechRad4State),
        VMSTATE_UINT32(dma_next, PsitechRad4State),
        VMSTATE_UINT32(dma_descs, PsitechRad4State),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * device-id defaults to plain RAD4; "-device psitech-rad4,device-id=0x105"
 * (RAD4_KM_DEVICE_ID) selects the RAD4-KM
 * identity instead, over this same shared implementation, the way
 * hw/misc/sgi-ioc3.c's own "subsystem-id" property
 * selects a board identity.
 *
 * eeprom optionally names a raw drive holding the serial EEPROM image
 * (rad4_eeprom_realize()), so a mode the guest stores there survives a
 * restart of QEMU, not only a guest reboot.
 */
static const Property psitech_rad4_properties[] = {
    DEFINE_PROP_UINT16("device-id", PsitechRad4State, device_id,
                       RAD4_DEVICE_ID),
    DEFINE_PROP_DRIVE("eeprom", PsitechRad4State, eeprom_blk),
    DEFINE_PROP_BOOL("overlay-index-compat", PsitechRad4State,
                     overlay_index_compat, true),
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

    dc->desc = "PsiTech RAD4 (driver-mode functional approximation)";
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
